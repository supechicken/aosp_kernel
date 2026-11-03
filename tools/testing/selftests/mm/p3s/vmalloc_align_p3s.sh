#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
here=$(dirname "$0")
runner=$here/p3s_run_module.sh
exec sh "$runner" "$here/p3s_modules/vmalloc_align_p3s_module.ko" \
	/dev/vmalloc_align_p3s -- "$here/vmalloc_align_p3s" "$@"
