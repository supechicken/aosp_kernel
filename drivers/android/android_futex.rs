// SPDX-License-Identifier: GPL-2.0

#![allow(missing_docs)]

//! `/dev/android_futex` - ioctl interface to the PING futex operations
//!
//! Provides a sys_futex()-like ioctl that dispatches
//! `ANDROID_FUTEX_LOCK_PING`, `ANDROID_FUTEX_UNLOCK_PING` and
//! `ANDROID_FUTEX_TRYLOCK_PING` into `kernel/futex/ping.c`.

use kernel::bindings;
use kernel::error::to_result;
use kernel::ioctl::_IOW;
use kernel::miscdevice::{MiscDevice, MiscDeviceOptions, MiscDeviceRegistration};
use kernel::prelude::*;
use kernel::uaccess::{UserPtr, UserSlice};

const ANDROID_FUTEX_LOCK_PING: u32 = 14;
const ANDROID_FUTEX_UNLOCK_PING: u32 = 15;
const ANDROID_FUTEX_TRYLOCK_PING: u32 = 16;

const FUTEX_PRIVATE_FLAG: u32 = 128;
const FUTEX_CLOCK_REALTIME: u32 = 256;
const FUTEX_CMD_MASK: u32 = !(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME);

const FLAGS_SIZE_32: u32 = 0x0002;
const FLAGS_SHARED: u32 = 0x0010;
const FLAGS_CLOCKRT: u32 = 0x0020;

const NSEC_PER_SEC: i64 = 1_000_000_000;
const KTIME_SEC_MAX: i64 = i64::MAX / NSEC_PER_SEC;

/// Arguments matching `struct android_futex_args` from `<uapi/linux/android_futex.h>`.
#[repr(C)]
#[derive(Copy, Clone, Default, Debug)]
pub struct AndroidFutexArgs {
    /// User address of the 32-bit futex word.
    pub uaddr: u64,
    /// FUTEX_*_PING operation, optionally OR'ed with flags.
    pub op: u32,
    /// Unused (must be zero).
    pub val: u32,
    /// User pointer to struct __kernel_timespec (or 0).
    pub timeout: u64,
    /// Unused (must be zero).
    pub uaddr2: u64,
    /// Unused (must be zero).
    pub val3: u32,
    /// Reserved padding (must be zero).
    pub __reserved: u32,
}

// SAFETY: All fields are integer types, so any bit pattern is valid.
unsafe impl kernel::transmute::FromBytes for AndroidFutexArgs {}

kernel::static_assert!(core::mem::size_of::<AndroidFutexArgs>() == 40);
kernel::static_assert!(core::mem::align_of::<AndroidFutexArgs>() == 8);

const ANDROID_FUTEX_IOC_MAGIC: u32 = 0xFA;
const ANDROID_FUTEX_IOC_FUTEX: u32 = _IOW::<AndroidFutexArgs>(ANDROID_FUTEX_IOC_MAGIC, 0x00);

#[repr(C)]
#[derive(Copy, Clone, Default, Debug)]
struct Timespec64 {
    tv_sec: i64,
    tv_nsec: core::ffi::c_long,
}

extern "C" {
    fn futex_lock_ping(
        uaddr: *mut u32,
        flags: core::ffi::c_uint,
        time: *mut bindings::ktime_t,
        trylock: core::ffi::c_int,
    ) -> core::ffi::c_int;

    fn futex_unlock_ping(
        uaddr: *mut u32,
        flags: core::ffi::c_uint,
    ) -> core::ffi::c_int;

    fn get_timespec64(
        ts: *mut Timespec64,
        uts: *const core::ffi::c_void,
    ) -> core::ffi::c_int;

    #[link_name = "rust_helper_timens_ktime_to_host"]
    fn timens_ktime_to_host(
        clockid: bindings::clockid_t,
        tim: bindings::ktime_t,
    ) -> bindings::ktime_t;
}

fn futex_to_flags(op: u32) -> u32 {
    let mut flags = FLAGS_SIZE_32;

    if (op & FUTEX_PRIVATE_FLAG) == 0 {
        flags |= FLAGS_SHARED;
    }

    if (op & FUTEX_CLOCK_REALTIME) != 0 {
        flags |= FLAGS_CLOCKRT;
    }

    flags
}

fn timespec64_valid(ts: &Timespec64) -> bool {
    ts.tv_sec >= 0 && (ts.tv_nsec as u64) < NSEC_PER_SEC as u64
}

fn timespec64_to_ktime(ts: &Timespec64) -> bindings::ktime_t {
    if ts.tv_sec >= KTIME_SEC_MAX {
        bindings::ktime_t::MAX
    } else {
        ts.tv_sec * NSEC_PER_SEC + ts.tv_nsec as i64
    }
}

fn android_futex_do_op(args: &AndroidFutexArgs) -> Result<isize> {
    let uaddr = args.uaddr as usize as *mut u32;
    let flags = futex_to_flags(args.op);
    let cmd = args.op & FUTEX_CMD_MASK;

    if args.val != 0 || args.uaddr2 != 0 || args.val3 != 0 || args.__reserved != 0 {
        return Err(EINVAL);
    }

    // Timeout and FUTEX_CLOCK_REALTIME only apply to FUTEX_LOCK_PING.
    if (args.timeout != 0 || (flags & FLAGS_CLOCKRT) != 0) && cmd != ANDROID_FUTEX_LOCK_PING {
        return Err(EINVAL);
    }

    match cmd {
        ANDROID_FUTEX_LOCK_PING => {
            let mut t: bindings::ktime_t;
            let tp = if args.timeout != 0 {
                let mut ts = Timespec64::default();
                // SAFETY: `get_timespec64` copies `struct __kernel_timespec` from userspace
                // into `ts` handling compat mode appropriately.
                let ret = unsafe {
                    get_timespec64(
                        &mut ts,
                        args.timeout as usize as *const core::ffi::c_void,
                    )
                };
                if ret != 0 {
                    return Err(EFAULT);
                }
                if !timespec64_valid(&ts) {
                    return Err(EINVAL);
                }
                t = timespec64_to_ktime(&ts);
                if (flags & FLAGS_CLOCKRT) == 0 {
                    // SAFETY: `timens_ktime_to_host` is safe to call from task context.
                    t = unsafe {
                        timens_ktime_to_host(
                            bindings::CLOCK_MONOTONIC as bindings::clockid_t,
                            t,
                        )
                    };
                }
                &mut t as *mut bindings::ktime_t
            } else {
                core::ptr::null_mut()
            };

            // SAFETY: `futex_lock_ping` safely accesses user pointer `uaddr` and kernel pointer `tp`.
            let ret = unsafe { futex_lock_ping(uaddr, flags, tp, 0) };
            to_result(ret).map(|_| ret as isize)
        }
        ANDROID_FUTEX_UNLOCK_PING => {
            // SAFETY: `futex_unlock_ping` safely accesses user pointer `uaddr`.
            let ret = unsafe { futex_unlock_ping(uaddr, flags) };
            to_result(ret).map(|_| ret as isize)
        }
        ANDROID_FUTEX_TRYLOCK_PING => {
            // SAFETY: `futex_lock_ping` safely accesses user pointer `uaddr`.
            let ret = unsafe { futex_lock_ping(uaddr, flags, core::ptr::null_mut(), 1) };
            to_result(ret).map(|_| ret as isize)
        }
        _ => Err(EINVAL),
    }
}

struct AndroidFutexDevice;

#[vtable]
impl MiscDevice for AndroidFutexDevice {
    type File = ();

    fn open(
        _file: &kernel::fs::File,
        _misc: &MiscDeviceRegistration<Self>,
    ) -> Result<Self::File> {
        Ok(())
    }

    fn ioctl(_me: (), _file: &kernel::fs::File, cmd: u32, arg: usize) -> Result<isize> {
        match cmd {
            ANDROID_FUTEX_IOC_FUTEX => {
                let user_slice =
                    UserSlice::new(UserPtr::from_addr(arg), core::mem::size_of::<AndroidFutexArgs>());
                let args = user_slice.reader().read::<AndroidFutexArgs>()?;
                android_futex_do_op(&args)
            }
            _ => Err(ENOTTY),
        }
    }
}

#[pin_data]
struct AndroidFutexModule {
    #[pin]
    _miscdev: MiscDeviceRegistration<AndroidFutexDevice>,
}

impl kernel::InPlaceModule for AndroidFutexModule {
    fn init(_module: &'static ThisModule) -> impl PinInit<Self, Error> {
        let options = MiscDeviceOptions {
            name: kernel::c_str!("android_futex"),
        };

        try_pin_init!(Self {
            _miscdev <- MiscDeviceRegistration::register(options, Ok(AndroidFutexDevice)),
        })
    }
}

module! {
    type: AndroidFutexModule,
    name: "android_futex",
    authors: ["John Stultz <jstultz@google.com>"],
    description: "ioctl interface for PING futex operations",
    license: "GPL",
}
