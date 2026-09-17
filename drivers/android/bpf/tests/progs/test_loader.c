// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Google LLC
 *
 * Minimal BPF program to validate BPF FD loading via light skeleton.
 */

#include <linux/compiler_attributes.h>

#define __uint(name, val) int (*name)[val]

static void *(*bpf_map_lookup_elem)(void *map, const void *key) = (void *) 1;

struct {
	__uint(type, 2); /* BPF_MAP_TYPE_ARRAY */
	__uint(max_entries, 4);
	unsigned int *key;
	unsigned long long *value;
} test_map __section(".maps") __used;

__section("socket") __used
int probe(void *ctx)
{
	unsigned int key = 0;
	unsigned long long *val = bpf_map_lookup_elem(&test_map, &key);

	return val ? (int)*val : 0;
}

char _license[] __section("license") __used = "GPL";
