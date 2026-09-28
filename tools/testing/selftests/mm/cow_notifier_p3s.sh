#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Load the MMU notifier test fixture, run the COW test, then unload it.

here=$(dirname "$0")
runner=$here/p3s_run_module.sh
[ -x "$runner" ] || runner=$here/../p3s/p3s_run_module.sh

"$runner" "$here/p3s_modules/mmu_notifier_p3s_module.ko" \
	/dev/mmu_notifier_p3s -- "$here/cow_notifier_p3s"
