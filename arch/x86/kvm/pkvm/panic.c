// SPDX-License-Identifier: GPL-2.0
#include <linux/kernel.h>
#include <linux/atomic.h>
#include <asm/io.h>
#include <asm/apic.h>
#include <asm/kvm_pkvm.h>
#include <asm/msr.h>
#include <asm-generic/bug.h>
#include "debug.h"
#include "pkvm.h"
#include "memory.h"

/*
 * NOTE: Both PERSISTENT_RAM_SIG and persistent_ram_buffer definition copied
 * from fs/pstore/ram_core.c
 */
#define PERSISTENT_RAM_SIG (0x43474244) /* DBGC */
#define PERSISTENT_RAM_ECC_BLOCK_SIZE	128
#define PERSISTENT_RAM_ECC_MAX_SIZE	16

struct persistent_ram_buffer {
	uint32_t    sig;
	atomic_t    start;
	atomic_t    size;
	uint8_t     data[];
};

static u8 pkvm_gf256_mul(u8 a, u8 b)
{
	u8 p = 0;
	int i;

	for (i = 0; i < 8; i++) {
		if (b & 1)
			p ^= a;
		if (a & 0x80)
			a = (a << 1) ^ 0x1d;
		else
			a <<= 1;
		b >>= 1;
	}
	return p;
}

static void pkvm_rs8_init_genpoly(u8 *genpoly, unsigned int ecc_size)
{
	unsigned int i, j;
	u8 root = 1;

	memset(genpoly, 0, ecc_size + 1);
	genpoly[0] = 1;
	for (i = 0; i < ecc_size; i++) {
		genpoly[i + 1] = 1;
		for (j = i; j > 0; j--)
			genpoly[j] = genpoly[j - 1] ^
				     pkvm_gf256_mul(genpoly[j], root);
		genpoly[0] = pkvm_gf256_mul(genpoly[0], root);
		root = pkvm_gf256_mul(root, 2);
	}
}

static void pkvm_rs8_encode(const u8 *genpoly, unsigned int ecc_size,
			    const void *data, size_t len, u8 *par)
{
	const u8 *src = data;
	unsigned int j;
	size_t i;

	memset(par, 0, ecc_size);
	for (i = 0; i < len; i++) {
		u8 fb = src[i] ^ par[0];

		for (j = 0; j < ecc_size - 1; j++)
			WRITE_ONCE(par[j], par[j + 1]);
		par[ecc_size - 1] = 0;

		if (fb) {
			for (j = 0; j < ecc_size; j++)
				par[j] ^= pkvm_gf256_mul(fb, genpoly[ecc_size - 1 - j]);
		}
	}
}

static void pkvm_ramoops_update_ecc(struct persistent_ram_buffer *buffer,
				    size_t capacity, u8 *par_buffer,
				    const u8 *genpoly, unsigned int ecc_size,
				    size_t start, size_t count)
{
	u8 *buffer_end = buffer->data + capacity;
	u8 *block;
	u8 *par;
	size_t size = PERSISTENT_RAM_ECC_BLOCK_SIZE;

	if (!ecc_size || !count)
		return;

	block = buffer->data + (start & ~(PERSISTENT_RAM_ECC_BLOCK_SIZE - 1));
	par = par_buffer + (start / PERSISTENT_RAM_ECC_BLOCK_SIZE) * ecc_size;

	do {
		if (block + PERSISTENT_RAM_ECC_BLOCK_SIZE > buffer_end)
			size = buffer_end - block;
		pkvm_rs8_encode(genpoly, ecc_size, block, size, par);
		block += PERSISTENT_RAM_ECC_BLOCK_SIZE;
		par += ecc_size;
	} while (block < buffer->data + start + count);
}

static void pkvm_write_ramoops_console(const char *msg)
{
	struct persistent_ram_buffer *buffer;
	u8 genpoly[PERSISTENT_RAM_ECC_MAX_SIZE + 1];
	unsigned int ecc_size = pkvm_ramoops_console_ecc_size;
	u8 *par_buffer = NULL, *par_header = NULL;
	size_t msg_len = strlen(msg);
	size_t data_total, capacity;
	u32 start, size;

	/* Size check is sufficient; address validity is verified during init */
	if (pkvm_ramoops_console_size <= sizeof(struct persistent_ram_buffer) ||
	    !msg_len)
		return;

	buffer = __pkvm_va(pkvm_ramoops_console_pa);

	/*
	 * Sanity check if writing to correct area by making sure it is valid
	 * persistent_ram_buffer structure which starts with PERSISTENT_RAM_SIG
	 */
	if (READ_ONCE(buffer->sig) != PERSISTENT_RAM_SIG)
		return;

	/*
	 * The ramoops console uses the 'persistent_ram_buffer' ABI defined in
	 * fs/pstore/ram_core.c. The buffer starts with a header:
	 * [u32 sig] [u32 start] [u32 size] [raw data...]
	 */
	data_total = pkvm_ramoops_console_size - sizeof(struct persistent_ram_buffer);
	if (ecc_size > PERSISTENT_RAM_ECC_MAX_SIZE)
		ecc_size = 0;

	if (ecc_size) {
		size_t ecc_blocks, ecc_total;

		if (data_total <= ecc_size)
			return;
		ecc_blocks = DIV_ROUND_UP(data_total - ecc_size,
					  PERSISTENT_RAM_ECC_BLOCK_SIZE + ecc_size);
		ecc_total = (ecc_blocks + 1) * ecc_size;
		if (ecc_total >= data_total)
			return;

		capacity = data_total - ecc_total;
		par_buffer = &buffer->data[capacity];
		par_header = &par_buffer[ecc_blocks * ecc_size];
		pkvm_rs8_init_genpoly(genpoly, ecc_size);
	} else {
		capacity = data_total;
	}

	if (msg_len > capacity) {
		msg += msg_len - capacity;
		msg_len = capacity;
	}

	/*
	 * 'start' indicates where the host stopped logging. In the
	 * persistent_ram_buffer ABI, this field is the "write pointer".
	 *
	 * By reading this value, the hypervisor knows where the host VM (which
	 * is now frozen) left off, allowing the panic message to be appended
	 * to the console log.
	 */
	start = atomic_read(&buffer->start);
	size = atomic_read(&buffer->size);

	if (start >= capacity)
		start = 0;
	if (size > capacity)
		size = capacity;
	else if (size < capacity)
		size = start;

	/* Circular append logic */
	if (start + msg_len <= capacity) {
		memcpy(&buffer->data[start], msg, msg_len);
		pkvm_ramoops_update_ecc(buffer, capacity, par_buffer, genpoly,
					ecc_size, start, msg_len);
		start += msg_len;
	} else {
		size_t first_part = capacity - start;
		size_t second_part = msg_len - first_part;

		memcpy(&buffer->data[start], msg, first_part);
		pkvm_ramoops_update_ecc(buffer, capacity, par_buffer, genpoly,
					ecc_size, start, first_part);
		memcpy(&buffer->data[0], msg + first_part, second_part);
		pkvm_ramoops_update_ecc(buffer, capacity, par_buffer, genpoly,
					ecc_size, 0, second_part);
		start = second_part;
	}

	if (start >= capacity)
		start = 0;

	/* Update total valid data size, capping at the buffer capacity */
	if (size < capacity) {
		size += msg_len;
		if (size > capacity)
			size = capacity;
	}

	/* Sync metadata so host pstore can find the new data after reboot */
	atomic_set(&buffer->start, start);
	atomic_set(&buffer->size, size);
	if (ecc_size)
		pkvm_rs8_encode(genpoly, ecc_size, buffer, sizeof(*buffer),
				par_header);

	/* Ensure data is visible in physical RAM before the hardware reset */
	clflush_cache_range(buffer, pkvm_ramoops_console_size);
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
