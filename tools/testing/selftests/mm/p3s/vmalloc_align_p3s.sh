#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
here=$(dirname "$0")
runner=$here/p3s_run_module.sh
if [ "$(getconf PAGESIZE)" != 16384 ]; then
	echo '1..0 # SKIP vmalloc slice fixture requires native 16KB pages'
	exit 4
fi
exec sh "$runner" "$here/p3s_modules/vmalloc_align_p3s_module.ko" \
	/dev/vmalloc_align_p3s -- "$here/vmalloc_align_p3s" "$@"
