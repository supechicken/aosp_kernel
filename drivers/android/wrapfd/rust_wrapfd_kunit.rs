// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2026 Google LLC.

//! KUnit tests for the Rust WrapFD driver.

use kernel::{prelude::*, Module};

module! {
    type: WrapFdKunitModule,
    name: "rust_wrapfd_kunit",
    author: "Isaac J. Manjarres",
    description: "WrapFD KUnit Test",
    license: "GPL",
    imports_ns: ["DMA_BUF"],
}

struct WrapFdKunitModule;

impl Module for WrapFdKunitModule {
    fn init(_module: &'static ThisModule) -> Result<Self> {
        Ok(Self)
    }
}

#[kernel::macros::kunit_tests(wrapfd)]
mod tests {
    use crate::THIS_MODULE;
    use core::ptr::{self, NonNull};
    use kernel::{
        alloc::KBox,
        bindings::{
            self, wrapfd_acquire_ownership, wrapfd_get_mappable, wrapfd_kunit_create_file,
            wrapfd_put_mappable, wrapfd_release_ownership,
        },
        c_str,
        dma_buf::DmaBuf,
        error::{from_err_ptr, to_result},
        ffi,
        fs::File,
        page::{Page, PAGE_SIZE},
        prelude::*,
        types::ARef,
    };

    struct TestBuffer {
        _page: Page,
        sgt: bindings::sg_table,
    }

    impl Drop for TestBuffer {
        fn drop(&mut self) {
            // SAFETY: `self.sgt` was initialized by `sg_alloc_table_from_pages` in
            // `create_test_dmabuf`.
            unsafe { bindings::sg_free_table(&raw mut self.sgt) };
        }
    }

    unsafe extern "C" fn test_map_dmabuf(
        attach: *mut bindings::dma_buf_attachment,
        _direction: bindings::dma_data_direction,
    ) -> *mut bindings::sg_table {
        // SAFETY: `attach` and `attach->dmabuf` are valid pointers provided by DMA-BUF, and
        // `priv_` points to a `TestBuffer` allocated in `create_test_dmabuf`.
        unsafe {
            let priv_ = (*(*attach).dmabuf).priv_.cast::<TestBuffer>();
            &raw mut (*priv_).sgt
        }
    }

    unsafe extern "C" fn test_unmap_dmabuf(
        _attach: *mut bindings::dma_buf_attachment,
        _sgt: *mut bindings::sg_table,
        _direction: bindings::dma_data_direction,
    ) {
    }

    unsafe extern "C" fn test_release(dmabuf: *mut bindings::dma_buf) {
        // SAFETY: `dmabuf` is valid and `priv_` owns a `KBox<TestBuffer>` transferred via
        // `KBox::into_raw` in `create_test_dmabuf`.
        unsafe {
            drop(KBox::from_raw((*dmabuf).priv_.cast::<TestBuffer>()));
        }
    }

    static TEST_DMABUF_OPS: bindings::dma_buf_ops = {
        // SAFETY: All-zeroes is a valid representation for `dma_buf_ops`.
        let mut ops: bindings::dma_buf_ops = unsafe { core::mem::zeroed() };
        ops.map_dma_buf = Some(test_map_dmabuf);
        ops.unmap_dma_buf = Some(test_unmap_dmabuf);
        ops.release = Some(test_release);
        ops
    };

    fn create_test_dmabuf() -> Result<ARef<DmaBuf>> {
        let page = Page::alloc_page(GFP_KERNEL)?;
        let mut page_ptr = page.as_ptr();

        let mut sgt = bindings::sg_table::default();
        // SAFETY: `sgt` is a valid pointer to an uninitialized `sg_table`, and `page_ptr` points
        // to a valid `struct page *` owned by `page`.
        to_result(unsafe {
            bindings::sg_alloc_table_from_pages(
                &raw mut sgt,
                &raw mut page_ptr,
                1,
                0,
                PAGE_SIZE,
                bindings::GFP_KERNEL,
            )
        })?;

        let priv_ = KBox::new(TestBuffer { _page: page, sgt }, GFP_KERNEL).inspect_err(|_| {
            // SAFETY: `sgt` was initialized by `sg_alloc_table_from_pages` above.
            unsafe { bindings::sg_free_table(&raw mut sgt) };
        })?;
        let priv_ptr = KBox::into_raw(priv_);

        let exp_info = bindings::dma_buf_export_info {
            exp_name: c_str!("wrapfd_kunit").as_char_ptr(),
            owner: THIS_MODULE.as_ptr(),
            ops: &TEST_DMABUF_OPS,
            size: PAGE_SIZE,
            flags: bindings::O_RDWR as ffi::c_int,
            resv: ptr::null_mut(),
            priv_: priv_ptr.cast(),
            ..Default::default()
        };

        // SAFETY: `exp_info` is properly initialized and points to static ops and valid `priv_`.
        let dmabuf_ptr = match from_err_ptr(unsafe { bindings::dma_buf_export(&exp_info) }) {
            Ok(ptr) => ptr,
            Err(err) => {
                // SAFETY: `dma_buf_export` failed so `test_release` was not called; reclaim
                // ownership of `priv_ptr` to free `sgt` and `_page`.
                unsafe { drop(KBox::from_raw(priv_ptr)) };
                return Err(err);
            }
        };

        let non_null = NonNull::new(dmabuf_ptr).ok_or(EINVAL)?;
        // SAFETY: `dma_buf_export` returns a valid `dma_buf` with an initial refcount of 1,
        // which we transfer to `ARef`.
        Ok(unsafe { ARef::from_raw(non_null.cast()) })
    }

    fn create_test_wrap_file(dmabuf: Option<&DmaBuf>) -> Result<ARef<File>> {
        let dmabuf_ptr = dmabuf.map_or(ptr::null_mut(), DmaBuf::as_ptr);
        // SAFETY: `dmabuf_ptr` is either null or a valid `struct dma_buf` pointer.
        let file_ptr = from_err_ptr(unsafe { wrapfd_kunit_create_file(dmabuf_ptr) })?;
        let non_null = NonNull::new(file_ptr).ok_or(EINVAL)?;
        // SAFETY: `wrapfd_kunit_create_file` returns a valid `struct file` with an initial
        // refcount of 1, which we transfer to `ARef`.
        Ok(unsafe { ARef::from_raw(non_null.cast()) })
    }

    struct KUnitDevice {
        test: NonNull<bindings::kunit>,
        dev: NonNull<bindings::device>,
    }

    impl KUnitDevice {
        fn register(test: NonNull<bindings::kunit>, name: &'static CStr) -> Result<Self> {
            // SAFETY: `test` is a valid KUnit context and `name` is a null-terminated C string.
            let dev_ptr = from_err_ptr(unsafe {
                bindings::kunit_device_register(test.as_ptr(), name.as_char_ptr())
            })?;
            let dev = NonNull::new(dev_ptr).ok_or(ENODEV)?;
            Ok(Self { test, dev })
        }

        fn as_ptr(&self) -> *mut bindings::device {
            self.dev.as_ptr()
        }
    }

    impl Drop for KUnitDevice {
        fn drop(&mut self) {
            // SAFETY: `self.dev` was registered with `kunit_device_register` for `self.test`.
            unsafe {
                bindings::kunit_device_unregister(self.test.as_ptr(), self.dev.as_ptr());
            }
        }
    }

    struct TestContext {
        dev1: KUnitDevice,
        dev2: KUnitDevice,
        file: ARef<File>,
        empty_file: ARef<File>,
    }

    impl TestContext {
        fn new() -> Result<Self> {
            // SAFETY: Called from within a running KUnit test case.
            let test = NonNull::new(unsafe { bindings::kunit_get_current_test() }).ok_or(EINVAL)?;

            let dev1 = KUnitDevice::register(test, c_str!("mock_device1"))?;
            let dev2 = KUnitDevice::register(test, c_str!("mock_device2"))?;

            let dmabuf = create_test_dmabuf()?;
            let file = create_test_wrap_file(Some(&dmabuf))?;
            let empty_file = create_test_wrap_file(None)?;

            Ok(Self {
                dev1,
                dev2,
                file,
                empty_file,
            })
        }
    }

    #[test]
    fn wrapfd_get_mappable_test() {
        let ctx = TestContext::new().expect("Failed to initialize test context");
        let dev1 = ctx.dev1.as_ptr();
        let dev2 = ctx.dev2.as_ptr();
        let file = ctx.file.as_ptr();
        let empty_file = ctx.empty_file.as_ptr();
        let mut mappable = bindings::wrapfd_mappable::default();

        // SAFETY: All calls below exercise the C FFI entrypoints with either valid pointers from
        // `TestContext` or intentionally null pointers to verify error handling.
        unsafe {
            // Test invalid inputs.
            assert_eq!(
                wrapfd_get_mappable(ptr::null_mut(), dev1, &raw mut mappable),
                EBADF.to_errno()
            );
            assert_eq!(
                wrapfd_put_mappable(ptr::null_mut(), dev1, &raw mut mappable),
                EBADF.to_errno()
            );
            assert_eq!(
                wrapfd_get_mappable(file, ptr::null_mut(), &raw mut mappable),
                ENODEV.to_errno()
            );
            assert_eq!(
                wrapfd_put_mappable(file, ptr::null_mut(), &raw mut mappable),
                ENODEV.to_errno()
            );
            assert_eq!(
                wrapfd_get_mappable(file, dev1, ptr::null_mut()),
                EINVAL.to_errno()
            );
            assert_eq!(
                wrapfd_put_mappable(file, dev1, ptr::null_mut()),
                EINVAL.to_errno()
            );

            // Get mappable object when no owners and no other users.
            assert_eq!(wrapfd_get_mappable(file, dev1, &raw mut mappable), 0);
            assert_eq!(wrapfd_put_mappable(file, dev1, &raw mut mappable), 0);

            // Get mappable object with other users.
            assert_eq!(wrapfd_get_mappable(file, dev1, &raw mut mappable), 0);
            assert_eq!(wrapfd_get_mappable(file, dev2, &raw mut mappable), 0);
            assert_eq!(wrapfd_put_mappable(file, dev1, &raw mut mappable), 0);
            assert_eq!(wrapfd_put_mappable(file, dev2, &raw mut mappable), 0);

            // Get mappable object while being the owner.
            assert_eq!(wrapfd_acquire_ownership(file, dev1), 0);
            assert_eq!(wrapfd_get_mappable(file, dev1, &raw mut mappable), 0);
            assert_eq!(wrapfd_put_mappable(file, dev1, &raw mut mappable), 0);
            assert_eq!(wrapfd_release_ownership(file, dev1), 0);

            // Get mappable object while someone else is the owner.
            assert_eq!(wrapfd_acquire_ownership(file, dev2), 0);
            assert_eq!(
                wrapfd_get_mappable(file, dev1, &raw mut mappable),
                EBUSY.to_errno()
            );
            assert_eq!(wrapfd_release_ownership(file, dev2), 0);

            // Get mappable object of an empty buffer.
            assert_eq!(
                wrapfd_get_mappable(empty_file, dev1, &raw mut mappable),
                ENOENT.to_errno()
            );
        }
    }

    #[test]
    fn wrapfd_acquire_ownership_test() {
        let ctx = TestContext::new().expect("Failed to initialize test context");
        let dev1 = ctx.dev1.as_ptr();
        let dev2 = ctx.dev2.as_ptr();
        let file = ctx.file.as_ptr();
        let empty_file = ctx.empty_file.as_ptr();
        let mut mappable = bindings::wrapfd_mappable::default();

        // SAFETY: All calls below exercise the C FFI entrypoints with either valid pointers from
        // `TestContext` or intentionally null pointers to verify error handling.
        unsafe {
            // Test invalid inputs.
            assert_eq!(
                wrapfd_acquire_ownership(ptr::null_mut(), dev1),
                EBADF.to_errno()
            );
            assert_eq!(
                wrapfd_release_ownership(ptr::null_mut(), dev1),
                EBADF.to_errno()
            );
            assert_eq!(
                wrapfd_acquire_ownership(file, ptr::null_mut()),
                ENODEV.to_errno()
            );
            assert_eq!(
                wrapfd_release_ownership(file, ptr::null_mut()),
                ENODEV.to_errno()
            );

            // Get ownership when no owners and no other users.
            assert_eq!(wrapfd_acquire_ownership(file, dev1), 0);
            assert_eq!(wrapfd_release_ownership(file, dev1), 0);

            // Get ownership while owning the buffer.
            assert_eq!(wrapfd_acquire_ownership(file, dev1), 0);
            assert_eq!(wrapfd_acquire_ownership(file, dev1), 0);
            assert_eq!(wrapfd_release_ownership(file, dev1), 0);

            // Get ownership while someone else owns the buffer.
            assert_eq!(wrapfd_acquire_ownership(file, dev2), 0);
            assert_eq!(wrapfd_acquire_ownership(file, dev1), EBUSY.to_errno());
            assert_eq!(wrapfd_release_ownership(file, dev2), 0);

            // Get ownership while mapping the buffer.
            assert_eq!(wrapfd_get_mappable(file, dev1, &raw mut mappable), 0);
            assert_eq!(wrapfd_acquire_ownership(file, dev1), EINVAL.to_errno());
            assert_eq!(wrapfd_put_mappable(file, dev1, &raw mut mappable), 0);

            // Get ownership while someone else maps the buffer.
            assert_eq!(wrapfd_get_mappable(file, dev2, &raw mut mappable), 0);
            assert_eq!(wrapfd_acquire_ownership(file, dev1), EINVAL.to_errno());
            assert_eq!(wrapfd_put_mappable(file, dev2, &raw mut mappable), 0);

            // Put ownership when nobody owns the buffer.
            assert_eq!(wrapfd_release_ownership(file, dev1), EBUSY.to_errno());

            // Put ownership when someone else owns the buffer.
            assert_eq!(wrapfd_acquire_ownership(file, dev2), 0);
            assert_eq!(wrapfd_release_ownership(file, dev1), EBUSY.to_errno());
            assert_eq!(wrapfd_release_ownership(file, dev2), 0);

            // Get ownership of an empty buffer.
            assert_eq!(
                wrapfd_acquire_ownership(empty_file, dev2),
                ENOENT.to_errno()
            );
        }
    }
}
