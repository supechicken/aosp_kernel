# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2026 The Android Open Source Project

"""Kleaf build definitions for P3S (per-process page size) mm kselftests."""

load("@bazel_skylib//lib:paths.bzl", "paths")
load("@rules_cc//cc:defs.bzl", "cc_library")
load(
    "@rules_pkg//pkg:mappings.bzl",
    "pkg_attributes",
    "pkg_filegroup",
    "pkg_files",
)
load(":bazel/abi.bzl", "cc_binary_with_abi")

visibility("private")

_P3S_ASM_TARGETS = {
    "brk_aslr_p3s_sample": {
        "features": ["-pie"],
        "linkopts": [
            "-nostdlib",
            "-static",
            "-no-pie",
            "-Wl,-z,max-page-size=16384",
        ],
    },
    "elf_4k_align_probe": {
        "features": ["-pie"],
        "linkopts": [
            "-nostdlib",
            "-static",
            "-Wl,-no-pie",
            "-Wl,--build-id=none",
            "-Wl,-z,max-page-size=4096",
        ],
    },
    "exec_aslr_p3s_helper": {
        "features": [],
        "linkopts": [
            "-nostdlib",
            "-pie",
            "-Wl,-e,_start",
            "-Wl,--dynamic-linker=/proc/self/fd/3",
            "-Wl,--build-id=none",
            "-Wl,-z,max-page-size=16384",
        ],
    },
    "exec_aslr_p3s_interp": {
        "features": ["-pie"],
        "linkopts": [
            "-nostdlib",
            "-static-pie",
            "-Wl,-e,_start",
            "-Wl,--build-id=none",
            "-Wl,-z,max-page-size=16384",
        ],
    },
}

def define_p3s_kselftests(name, kselftest_dir, kselftest_copts):
    """Defines userspace P3S mm kselftest targets for arm64 tests.zip.

    Args:
      name: Name prefix for generated kselftest targets.
      kselftest_dir: Package-relative kselftest root directory.
      kselftest_copts: Common compiler flags inherited from GKI kselftests.
    """
    p3s_copts = kselftest_copts + [
        "-U_FORTIFY_SOURCE",
        "-ffile-prefix-map=common/tools/testing/selftests/mm/p3s/=",
        "-ffile-prefix-map=common/tools/testing/selftests/mm/=",
    ]

    lib_name = name + "_lib"
    cc_library(
        name = lib_name,
        srcs = ["tools/testing/selftests/mm/thp_settings.c"],
        hdrs = native.glob(["tools/testing/selftests/mm/p3s/*.h"]) + [
            "drivers/staging/android/uapi/ashmem.h",
            "drivers/vhost/test.h",
            "include/uapi/linux/trace_mmap.h",
            "include/uapi/linux/virtio_ring.h",
            "tools/testing/selftests/mm/thp_settings.h",
        ],
        copts = p3s_copts,
        includes = [
            "include/uapi/",
            "tools/testing/selftests",
            "tools/testing/selftests/mm",
            "tools/testing/selftests/mm/p3s",
        ],
        visibility = ["//visibility:private"],
        deps = [
            ":kselftest_headers_lib",
            ":kselftest_mm_vm_util",
        ],
    )

    pkg_srcs = []

    for src in native.glob(["tools/testing/selftests/mm/p3s/*.c"]):
        stem = paths.split_extension(paths.basename(src))[0]
        target_name = name + "_" + stem
        cc_binary_with_abi(
            name = target_name,
            srcs = [src],
            out = "mm/p3s/" + stem,
            abis = ["arm64"],
            copts = p3s_copts,
            linkopts = ["-Wl,-z,max-page-size=16384"],
            path_prefix = kselftest_dir,
            target_compatible_with = ["@platforms//os:android"],
            visibility = ["//visibility:private"],
            deps = [
                ":kselftest_headers_lib",
                ":" + lib_name,
                ":kselftest_mm_vm_util",
            ],
        )
        pkg_srcs.append(":" + target_name + "_arm64")

    pagemap_name = name + "_pagemap_ioctl_bin"
    cc_binary_with_abi(
        name = pagemap_name,
        srcs = ["tools/testing/selftests/mm/pagemap_ioctl.c"],
        out = "mm/p3s/pagemap_ioctl",
        abis = ["arm64"],
        copts = p3s_copts,
        linkopts = ["-Wl,-z,max-page-size=16384"],
        path_prefix = kselftest_dir,
        target_compatible_with = ["@platforms//os:android"],
        visibility = ["//visibility:private"],
        deps = [
            ":kselftest_headers_lib",
            ":" + lib_name,
            ":kselftest_mm_vm_util",
        ],
    )
    pkg_srcs.append(":" + pagemap_name + "_arm64")

    for stem, cfg in _P3S_ASM_TARGETS.items():
        target_name = name + "_" + stem
        cc_binary_with_abi(
            name = target_name,
            srcs = ["tools/testing/selftests/mm/p3s/" + stem + ".S"],
            out = "mm/p3s/" + stem,
            abis = ["arm64"],
            features = cfg["features"],
            linkopts = cfg["linkopts"],
            path_prefix = kselftest_dir,
            target_compatible_with = ["@platforms//os:android"],
            visibility = ["//visibility:private"],
        )
        pkg_srcs.append(":" + target_name + "_arm64")

    scripts = native.glob(["tools/testing/selftests/mm/p3s/*.sh"])
    scripts_name = name + "_scripts_arm64"
    pkg_files(
        name = scripts_name,
        srcs = scripts,
        attributes = pkg_attributes(mode = "0755"),
        renames = {
            script: paths.join(kselftest_dir, "arm64/mm/p3s", paths.basename(script))
            for script in scripts
        },
        visibility = ["//visibility:private"],
    )
    pkg_srcs.append(":" + scripts_name)

    pkg_filegroup(
        name = name + "_arm64",
        srcs = pkg_srcs,
        visibility = ["//visibility:private"],
    )
