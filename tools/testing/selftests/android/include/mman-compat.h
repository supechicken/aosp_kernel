/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Fallback mman UAPI definitions for the NDK sysroot used by Kleaf.
 *
 * mlock2-tests.c uses MAP_DROPPABLE (added in v6.11), but the NDK r27
 * sysroot only ships v6.8 kernel headers. Include the bionic header first
 * and only define what is missing, so this file is a no-op once the
 * sysroot is updated.
 *
 * Value matches include/uapi/linux/mman.h.
 *
 * TODO(b/557760113): Remove once Kleaf uses an NDK with v6.11+ headers.
 */
#ifndef __ANDROID_MMAN_COMPAT_H
#define __ANDROID_MMAN_COMPAT_H

#include <sys/mman.h>

#ifndef MAP_DROPPABLE
#define MAP_DROPPABLE	0x08
#endif

#endif /* __ANDROID_MMAN_COMPAT_H */
