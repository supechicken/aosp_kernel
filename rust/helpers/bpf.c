// SPDX-License-Identifier: GPL-2.0

#include <linux/filter.h>

#if IS_ENABLED(CONFIG_ANDROID_BPF_FD_LOADER)
__rust_helper u32 rust_helper_bpf_prog_run_pin_on_cpu(const struct bpf_prog *prog,
						      const void *ctx)
{
	return bpf_prog_run_pin_on_cpu(prog, ctx);
}
#endif
