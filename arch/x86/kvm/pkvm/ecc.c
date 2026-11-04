// SPDX-License-Identifier: GPL-2.0
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/math.h>
#include <linux/string.h>
#include "ecc.h"

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

int pkvm_ramoops_ecc_init(struct pkvm_ramoops_ecc *ecc,
			  struct persistent_ram_buffer *buffer,
			  size_t total_size, unsigned int ecc_size)
{
	size_t data_total;

	if (!ecc || !buffer || total_size <= sizeof(*buffer))
		return -EINVAL;

	data_total = total_size - sizeof(*buffer);
	if (ecc_size > PERSISTENT_RAM_ECC_MAX_SIZE)
		ecc_size = 0;

	ecc->buffer = buffer;
	ecc->ecc_size = ecc_size;

	if (ecc_size) {
		size_t ecc_blocks, ecc_total;

		if (data_total <= ecc_size)
			return -EINVAL;

		ecc_blocks = DIV_ROUND_UP(data_total - ecc_size,
					  PERSISTENT_RAM_ECC_BLOCK_SIZE + ecc_size);
		ecc_total = (ecc_blocks + 1) * ecc_size;
		if (ecc_total >= data_total)
			return -EINVAL;

		ecc->capacity = data_total - ecc_total;
		ecc->par_buffer = &buffer->data[ecc->capacity];
		ecc->par_header = &ecc->par_buffer[ecc_blocks * ecc_size];
		pkvm_rs8_init_genpoly(ecc->genpoly, ecc_size);
	} else {
		ecc->capacity = data_total;
		ecc->par_buffer = NULL;
		ecc->par_header = NULL;
	}

	return 0;
}

void pkvm_ramoops_ecc_update_blocks(const struct pkvm_ramoops_ecc *ecc,
				    size_t start, size_t count)
{
	u8 *buffer_end;
	u8 *block;
	u8 *par;
	size_t size = PERSISTENT_RAM_ECC_BLOCK_SIZE;

	if (!ecc || !ecc->ecc_size || !count)
		return;

	buffer_end = ecc->buffer->data + ecc->capacity;
	block = ecc->buffer->data + (start & ~(PERSISTENT_RAM_ECC_BLOCK_SIZE - 1));
	par = ecc->par_buffer + (start / PERSISTENT_RAM_ECC_BLOCK_SIZE) * ecc->ecc_size;

	do {
		if (block + PERSISTENT_RAM_ECC_BLOCK_SIZE > buffer_end)
			size = buffer_end - block;
		pkvm_rs8_encode(ecc->genpoly, ecc->ecc_size, block, size, par);
		block += PERSISTENT_RAM_ECC_BLOCK_SIZE;
		par += ecc->ecc_size;
	} while (block < ecc->buffer->data + start + count);
}

void pkvm_ramoops_ecc_update_header(const struct pkvm_ramoops_ecc *ecc)
{
	if (!ecc || !ecc->ecc_size)
		return;

	pkvm_rs8_encode(ecc->genpoly, ecc->ecc_size, ecc->buffer,
			sizeof(*ecc->buffer), ecc->par_header);
}
