/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PKVM_X86_ECC_H
#define __PKVM_X86_ECC_H

#include <linux/types.h>
#include <linux/atomic.h>

/*
 * NOTE: Both PERSISTENT_RAM_SIG and persistent_ram_buffer definition copied
 * from fs/pstore/ram_core.c
 */
#define PERSISTENT_RAM_SIG		(0x43474244) /* DBGC */
#define PERSISTENT_RAM_ECC_BLOCK_SIZE	128
#define PERSISTENT_RAM_ECC_MAX_SIZE	16

struct persistent_ram_buffer {
	uint32_t    sig;
	atomic_t    start;
	atomic_t    size;
	uint8_t     data[];
};

struct pkvm_ramoops_ecc {
	struct persistent_ram_buffer *buffer;
	size_t capacity;
	unsigned int ecc_size;
	u8 *par_buffer;
	u8 *par_header;
	u8 genpoly[PERSISTENT_RAM_ECC_MAX_SIZE + 1];
};

int pkvm_ramoops_ecc_init(struct pkvm_ramoops_ecc *ecc,
			  struct persistent_ram_buffer *buffer,
			  size_t total_size, unsigned int ecc_size);
void pkvm_ramoops_ecc_update_blocks(const struct pkvm_ramoops_ecc *ecc,
				    size_t start, size_t count);
void pkvm_ramoops_ecc_update_header(const struct pkvm_ramoops_ecc *ecc);

#endif /* __PKVM_X86_ECC_H */
