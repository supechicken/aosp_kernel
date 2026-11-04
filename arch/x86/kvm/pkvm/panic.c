// SPDX-License-Identifier: GPL-2.0
#include <linux/kernel.h>
#include <linux/atomic.h>
#include <linux/list.h>
#include <linux/rslib.h>
#include <asm/io.h>
#include <asm/apic.h>
#include <asm/kvm_pkvm.h>
#include <asm/msr.h>
#include <asm-generic/bug.h>
#include "debug.h"
#include "ram_internal.h"
#include "pkvm.h"
#include "memory.h"

static struct persistent_ram_zone pkvm_ramoops_prz;
static struct rs_control pkvm_ramoops_rs_decoder;
static struct rs_codec pkvm_ramoops_rs_codec;
static u16 pkvm_ramoops_rs_alpha_to[256];
static u16 pkvm_ramoops_rs_index_of[256];
static u16 pkvm_ramoops_rs_genpoly[PKVM_RAMOOPS_ECC_MAX_SIZE + 1];
static u16 pkvm_ramoops_rs_par[PKVM_RAMOOPS_ECC_MAX_SIZE];

int pkvm_ramoops_init_prz(phys_addr_t pa, size_t size, unsigned int ecc_size,
			  const struct rs_codec *codec)
{
	struct persistent_ram_zone *prz = &pkvm_ramoops_prz;
	struct persistent_ram_ecc_info ecc_info = {
		.ecc_size = ecc_size,
	};
	int ret;

	memset(prz, 0, sizeof(*prz));
	if (size <= sizeof(struct persistent_ram_buffer))
		return -EINVAL;

	prz->paddr = pa;
	prz->size = size;
	prz->flags = PRZ_FLAG_NO_LOCK;
	prz->buffer = __pkvm_va(pa);
	prz->buffer_size = size - sizeof(struct persistent_ram_buffer);

	if (!ecc_size)
		return 0;

	if (!codec || !codec->alpha_to || !codec->index_of || !codec->genpoly ||
	    ecc_size > PKVM_RAMOOPS_ECC_MAX_SIZE ||
	    codec->mm != 8 || codec->nn != 255 || codec->nroots != ecc_size)
		goto err;

	ret = persistent_ram_init_ecc_layout(prz, &ecc_info);
	if (ret)
		goto err;

	pkvm_ramoops_rs_codec = *codec;
	INIT_LIST_HEAD(&pkvm_ramoops_rs_codec.list);
	memcpy(pkvm_ramoops_rs_alpha_to, codec->alpha_to,
	       sizeof(pkvm_ramoops_rs_alpha_to));
	memcpy(pkvm_ramoops_rs_index_of, codec->index_of,
	       sizeof(pkvm_ramoops_rs_index_of));
	memcpy(pkvm_ramoops_rs_genpoly, codec->genpoly,
	       (ecc_size + 1) * sizeof(pkvm_ramoops_rs_genpoly[0]));

	pkvm_ramoops_rs_codec.alpha_to = pkvm_ramoops_rs_alpha_to;
	pkvm_ramoops_rs_codec.index_of = pkvm_ramoops_rs_index_of;
	pkvm_ramoops_rs_codec.genpoly = pkvm_ramoops_rs_genpoly;

	pkvm_ramoops_rs_decoder.codec = &pkvm_ramoops_rs_codec;
	prz->rs_decoder = &pkvm_ramoops_rs_decoder;
	prz->ecc_info.par = pkvm_ramoops_rs_par;

	return 0;

err:
	memset(prz, 0, sizeof(*prz));
	return -EINVAL;
}

static void pkvm_write_ramoops_console(const char *msg)
{
	struct persistent_ram_zone *prz = &pkvm_ramoops_prz;
	struct persistent_ram_buffer *buffer = prz->buffer;
	size_t capacity = prz->buffer_size;
	size_t msg_len = strlen(msg);
	u32 start, size;

	/* Size check is sufficient; address validity is verified during init */
	if (!buffer || !capacity || !msg_len)
		return;

	/*
	 * Sanity check if writing to correct area by making sure it is valid
	 * persistent_ram_buffer structure which starts with PERSISTENT_RAM_SIG
	 */
	if (READ_ONCE(buffer->sig) != PERSISTENT_RAM_SIG)
		return;

	/*
	 * 'start' indicates where the host stopped logging. In the
	 * persistent_ram_buffer ABI, this field is the "write pointer".
	 *
	 * By reading this value, the hypervisor knows where the host VM (which
	 * is now frozen) left off, allowing the panic message to be appended
	 * to the console log. Sanitize untrusted host ring-buffer pointers
	 * before calling persistent_ram_write().
	 */
	start = atomic_read(&buffer->start);
	size = atomic_read(&buffer->size);

	if (start >= capacity)
		start = 0;
	if (size > capacity)
		size = capacity;
	else if (size < capacity)
		size = start;

	atomic_set(&buffer->start, start);
	atomic_set(&buffer->size, size);

	persistent_ram_write(prz, msg, msg_len);

	/* Ensure data is visible in physical RAM before the hardware reset */
	clflush_cache_range(buffer, prz->size);
}

/*
 * To not include ACPI based reboot and its complexity, try to reset the system
 * via PCI warm reset (0xCF9), the keyboard controller (PS/2), PCI cold reset
 * register (CF9), or triple fault.
 *
 * All based on arch/x86/kernel/reboot.c native_machine_emergency_restart().
 */
static void __noreturn pkvm_emergency_reset(void)
{
	struct desc_ptr idt;

	/*
	 * PCI warm reset (0xCF9), which is usually effectively the ACPI reset,
	 * i.e. the modern recommended default.
	 */
	outb(0x06, 0xcf9);
	pkvm_udelay(50000);

	/* Keyboard controller (PS/2) reset */
	outb(0xfe, 0x64);
	pkvm_udelay(50000);

	/* PCI cold reset */
	outb(0x0e, 0xcf9);
	pkvm_udelay(50000);

	/* Triple fault */
	idt.size = 0;
	idt.address = 0;
	asm volatile("lidt %0" : : "m"(idt));
	asm volatile("int3");

	while (1)
		asm volatile("cli; hlt");
}

static void pkvm_get_timestamp(unsigned long *sec, unsigned long *usec)
{
	u64 cycles, rem_cycles;

	cycles = rdtsc_ordered();
	*sec = cycles / (tsc_khz * 1000ULL);
	rem_cycles = cycles % (tsc_khz * 1000ULL);
	*usec = (rem_cycles * 1000ULL) / tsc_khz;
}

/*
 * Approximate uptime timestamp derived directly from hardware TSC cycles.
 * Note: it is slightly ahead (~1-2s on Chromebooks) of the host kernel printk
 * timestamp, which zeroes at kernel entry, but preserves proper chronological
 * ordering for crash log analysis.
 */
static int pkvm_printk_prefix(char *buf, size_t size)
{
	char caller[8];
	unsigned long sec, usec;

	scnprintf(caller, sizeof(caller), "C%d", raw_smp_processor_id());
	pkvm_get_timestamp(&sec, &usec);
	return scnprintf(buf, size, "<0>[%5lu.%06lu][%6s] ",
			 sec, usec, caller);
}

atomic_t pkvm_panic_in_progress = ATOMIC_INIT(0);

void __noreturn panic(const char *fmt, ...)
{
	static char panic_msg[1024];
	va_list args;
	int len;

	/*
	 * Ensure only one CPU handles the panic and writes to ramoops.
	 * This also signals the VM exit handler to catch and hold all other
	 * CPUs.
	 */
	if (atomic_cmpxchg(&pkvm_panic_in_progress, 0, 1) != 0) {
		while (1)
			asm volatile("cli; hlt");
	}

	/*
	 * Broadcast INIT to all other CPUs (excluding self) via x2APIC ICR
	 * to pull them out of the guest mode. This ensures that the host will not
	 * interfere with panic handling and e.g. will not interfere with
	 * ramoops update.
	 */
	native_x2apic_icr_write(APIC_DEST_ALLBUT | APIC_INT_ASSERT | APIC_DM_INIT, 0);

	/*
	 * Wait for all IPIs to deliver and targets to enter Wait-for-SIPI.
	 * Note: using pkvm_wait_vcpu_kicked_out() is tempting but for reliability,
	 * since we are in the middle of fatal error handling, it is better to
	 * use simple hw delay. This avoids relying on various sw states (like
	 * vcpu->mode) that could be corrupted by the fatal error itself.
	 */
	pkvm_udelay(10000);

	len = pkvm_printk_prefix(panic_msg, sizeof(panic_msg));
	len += scnprintf(panic_msg + len, sizeof(panic_msg) - len,
			 "Kernel panic - not syncing: HYP panic:\n");

	len += pkvm_printk_prefix(panic_msg + len, sizeof(panic_msg) - len);

	va_start(args, fmt);
	vscnprintf(panic_msg + len, sizeof(panic_msg) - len, fmt, args);
	va_end(args);

	pkvm_err("%s", panic_msg);

	pkvm_write_ramoops_console(panic_msg);

	pkvm_emergency_reset();
}
