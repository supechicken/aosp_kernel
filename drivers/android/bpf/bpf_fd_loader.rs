// SPDX-License-Identifier: GPL-2.0-only
//! Kernel module exposing an IOCTL interface to userspace for loading and executing
//! BPF loader programs from an ELF file descriptor.

use core::{mem::size_of, ptr::NonNull};
use kernel::{
    alloc::kvec::KVVec,
    bindings,
    error::{from_err_ptr, to_result, Error},
    ffi,
    fs::{File, LocalFile},
    ioctl::{_IO, _IOWR},
    miscdevice::{MiscDevice, MiscDeviceOptions, MiscDeviceRegistration},
    prelude::*,
    sizes::SZ_8M,
    uaccess::{UserPtr, UserSlice},
};
use zerocopy::{FromBytes, IntoBytes};
use zerocopy_derive::{FromBytes, Immutable, IntoBytes, KnownLayout};

const ANDROID_BPF_FD_LOADER_MAGIC: u32 = b'B' as u32;

/// IOCTL argument structure matching `struct android_bpf_fd_loader_arg` in
/// `<uapi/linux/android/bpf_fd_loader.h>`.
#[repr(C, align(8))]
#[derive(Copy, Clone, Default, FromBytes, IntoBytes, Immutable, KnownLayout)]
struct AndroidBpfFdLoaderArg {
    /// File descriptor to an open ELF file containing the loader program and data.
    loader_fd: u32,
    /// Size of the context passed to the loader program.
    ctx_size: u32,
    /// Userspace pointer to the context passed to the loader program.
    ctx: u64,
}

const ANDROID_BPF_FD_LOADER_IOCTL_PING: u32 = _IO(ANDROID_BPF_FD_LOADER_MAGIC, 1);
const ANDROID_BPF_FD_LOADER_IOCTL_RECEIVE_FD: u32 =
    _IOWR::<AndroidBpfFdLoaderArg>(ANDROID_BPF_FD_LOADER_MAGIC, 2);

const DEVICE_NAME: &CStr = c"android_bpf_loader";

const BPF_LOADER_PROG_SEC: &CStr = c"__loader.prog";
const BPF_LOADER_MAP_SEC: &CStr = c"__loader.map";
const BPF_LOADER_LICENSE_SEC: &CStr = c"license";
const BPF_LOADER_MAX_SIZE: usize = SZ_8M;

module! {
    type: BpfFdLoaderModule,
    name: "bpf_fd_loader",
    authors: [
        "Thiebaud Weksteen <tweek@google.com>",
        "Siddharth Nayyar <sidnayyar@google.com>",
    ],
    description: "Android BPF FD Loader Driver",
    license: "GPL",
    imports_ns: [
        "BPF_INTERNAL",
    ],
}

#[pin_data]
struct BpfFdLoaderModule {
    #[pin]
    _miscdev: MiscDeviceRegistration<BpfFdLoaderDevice>,
}

impl kernel::InPlaceModule for BpfFdLoaderModule {
    fn init(_module: &'static ThisModule) -> impl PinInit<Self, Error> {
        let options = MiscDeviceOptions { name: DEVICE_NAME };

        pr_debug!("module loaded, device node /dev/{DEVICE_NAME} created\n");

        try_pin_init!(Self {
            _miscdev <- MiscDeviceRegistration::register(options),
        })
    }
}

struct BpfFdLoaderDevice;

#[vtable]
impl MiscDevice for BpfFdLoaderDevice {
    type Ptr = ();

    fn open(_file: &File, _misc: &MiscDeviceRegistration<Self>) -> Result<()> {
        pr_debug!("device opened\n");
        Ok(())
    }

    fn release(_device: (), _file: &File) {
        pr_debug!("device closed\n");
    }

    fn ioctl(_device: (), _file: &File, cmd: u32, arg: usize) -> Result<isize> {
        match cmd {
            ANDROID_BPF_FD_LOADER_IOCTL_PING => {
                pr_debug!("IOCTL PING received\n");
                Ok(0)
            }
            ANDROID_BPF_FD_LOADER_IOCTL_RECEIVE_FD => {
                let (mut reader, mut writer) =
                    UserSlice::new(UserPtr::from_addr(arg), size_of::<AndroidBpfFdLoaderArg>())
                        .reader_writer();
                let mut loader_arg = AndroidBpfFdLoaderArg::default();
                reader.read_slice(loader_arg.as_mut_bytes())?;

                pr_debug!(
                    "received loader_fd = {}, ctx_size = {}\n",
                    loader_arg.loader_fd,
                    loader_arg.ctx_size
                );

                loader_load_fd(&loader_arg)?;

                writer.write_slice(loader_arg.as_bytes())?;
                Ok(0)
            }
            _ => {
                pr_debug!("unknown IOCTL cmd 0x{:x}\n", cmd);
                Err(ENOTTY)
            }
        }
    }
}

/// Reads the contents of a file up to `max_size` bytes into a `VBox<[u8]>`.
fn read_kernel_file(file: &LocalFile, max_size: usize) -> Result<VBox<[u8]>> {
    let mut raw_buf = core::ptr::null_mut();
    // SAFETY: `file.as_ptr()` points to a valid open `struct file`.
    // `kernel_read_file` allocates memory via vmalloc and writes the pointer into `raw_buf`.
    let len = unsafe {
        bindings::kernel_read_file(
            file.as_ptr(),
            0,
            &mut raw_buf,
            max_size,
            core::ptr::null_mut(),
            bindings::kernel_read_file_id_READING_MODULE,
        )
    };
    if len < 0 {
        return Err(Error::from_errno(len as ffi::c_int));
    }
    if len == 0 || raw_buf.is_null() {
        return Err(ENOEXEC);
    }
    let slice = core::ptr::slice_from_raw_parts_mut(raw_buf.cast::<u8>(), len as usize);
    // SAFETY: `raw_buf` points to memory allocated by `kernel_read_file` with `vmalloc`
    // (represented by `Vmalloc`), is properly aligned for `u8`, and is valid for `len`
    // elements. Ownership of the allocation is transferred to the `VBox`.
    Ok(unsafe { VBox::from_raw(slice) })
}

#[repr(C)]
#[derive(Copy, Clone, FromBytes, IntoBytes, Immutable, KnownLayout)]
struct Elf64Ehdr {
    e_ident: [u8; 16],
    e_type: u16,
    e_machine: u16,
    e_version: u32,
    e_entry: u64,
    e_phoff: u64,
    e_shoff: u64,
    e_flags: u32,
    e_ehsize: u16,
    e_phentsize: u16,
    e_phnum: u16,
    e_shentsize: u16,
    e_shnum: u16,
    e_shstrndx: u16,
}

#[repr(C)]
#[derive(Copy, Clone, FromBytes, IntoBytes, Immutable, KnownLayout)]
struct Elf64Shdr {
    sh_name: u32,
    sh_type: u32,
    sh_flags: u64,
    sh_addr: u64,
    sh_offset: u64,
    sh_size: u64,
    sh_link: u32,
    sh_info: u32,
    sh_addralign: u64,
    sh_entsize: u64,
}

/// Parsed 64-bit ELF binary metadata used for section extraction.
struct ElfInfo<'a> {
    /// Raw bytes of the ELF binary buffer.
    buf: &'a [u8],
    /// Slice of section headers from the section header table.
    sechdrs: &'a [Elf64Shdr],
    /// Byte slice of the section header string table (`.shstrtab`).
    secstrings: &'a [u8],
}

impl<'a> ElfInfo<'a> {
    fn parse(buf: &'a [u8]) -> Result<Self> {
        let (hdr, _) = Elf64Ehdr::ref_from_prefix(buf).map_err(|_| {
            pr_debug!("Invalid ELF header len {}\n", buf.len());
            ENOEXEC
        })?;

        if &hdr.e_ident[..bindings::SELFMAG as usize]
            != &bindings::ELFMAG[..bindings::SELFMAG as usize]
        {
            pr_debug!("Invalid ELF header magic: != \\177ELF\n");
            return Err(ENOEXEC);
        }
        if hdr.e_ident[bindings::EI_CLASS as usize] != bindings::ELFCLASS64 as u8 {
            pr_debug!("Only 64-bit ELF is supported\n");
            return Err(ENOEXEC);
        }
        if hdr.e_type != bindings::ET_REL as u16 {
            pr_debug!(
                "Invalid ELF header type: {} != {}\n",
                hdr.e_type,
                bindings::ET_REL
            );
            return Err(ENOEXEC);
        }
        if hdr.e_machine != bindings::EM_BPF as u16 {
            pr_debug!(
                "Invalid ELF machine type: {} != {}\n",
                hdr.e_machine,
                bindings::EM_BPF
            );
            return Err(ENOEXEC);
        }
        if hdr.e_shentsize as usize != size_of::<Elf64Shdr>() {
            pr_debug!("Invalid ELF section header size\n");
            return Err(ENOEXEC);
        }

        let shoff = usize::try_from(hdr.e_shoff).map_err(|_| ENOEXEC)?;
        let shnum = hdr.e_shnum as usize;
        let sh_table_bytes = shnum.checked_mul(size_of::<Elf64Shdr>()).ok_or(ENOEXEC)?;

        if shoff >= buf.len() || sh_table_bytes > buf.len() - shoff {
            pr_debug!("Invalid ELF section header overflow\n");
            return Err(ENOEXEC);
        }

        let sh_slice = &buf[shoff..shoff + sh_table_bytes];
        let (sechdrs, _) =
            <[Elf64Shdr]>::ref_from_prefix_with_elems(sh_slice, shnum).map_err(|_| {
                pr_debug!("Invalid ELF section header alignment or size\n");
                ENOEXEC
            })?;

        if sechdrs.is_empty()
            || sechdrs[0].sh_type != bindings::SHT_NULL
            || sechdrs[0].sh_size != 0
            || sechdrs[0].sh_addr != 0
        {
            pr_debug!(
                "ELF Spec violation: section 0 type({})!=SH_NULL or non-zero len or addr\n",
                sechdrs.first().map_or(0, |s| s.sh_type)
            );
            return Err(ENOEXEC);
        }

        for (i, shdr) in sechdrs.iter().enumerate().skip(1) {
            match shdr.sh_type {
                bindings::SHT_NULL | bindings::SHT_NOBITS => continue,
                _ => {
                    let secend = shdr.sh_offset.checked_add(shdr.sh_size).ok_or(ENOEXEC)?;
                    if secend > buf.len() as u64 {
                        pr_debug!(
                            "Invalid ELF section in BPF loader (section {} type {})\n",
                            i,
                            shdr.sh_type
                        );
                        return Err(ENOEXEC);
                    }
                }
            }
        }

        let shstrndx = hdr.e_shstrndx as usize;
        if shstrndx == bindings::SHN_UNDEF as usize || shstrndx >= shnum {
            pr_debug!(
                "Invalid ELF section name index: {} || e_shstrndx ({}) >= e_shnum ({})\n",
                shstrndx,
                shstrndx,
                shnum
            );
            return Err(ENOEXEC);
        }

        let strhdr = &sechdrs[shstrndx];
        if strhdr.sh_type != bindings::SHT_STRTAB {
            pr_debug!("Invalid ELF section name table type: {}\n", strhdr.sh_type);
            return Err(ENOEXEC);
        }
        if strhdr.sh_size == 0 {
            pr_debug!("empty section name table\n");
            return Err(ENOEXEC);
        }

        let str_off = strhdr.sh_offset as usize;
        let str_sz = strhdr.sh_size as usize;
        let str_end = str_off.checked_add(str_sz).ok_or(ENOEXEC)?;
        let secstrings = buf.get(str_off..str_end).ok_or(ENOEXEC)?;
        if secstrings[str_sz - 1] != 0 {
            pr_debug!("ELF Spec violation: section name table isn't null terminated\n");
            return Err(ENOEXEC);
        }

        for (i, shdr) in sechdrs.iter().enumerate() {
            if shdr.sh_type == bindings::SHT_NULL {
                continue;
            }
            if (shdr.sh_name as usize) >= str_sz {
                pr_debug!(
                    "Invalid ELF section name in BPF loader (section {} type {})\n",
                    i,
                    shdr.sh_type
                );
                return Err(ENOEXEC);
            }
        }

        Ok(Self {
            buf,
            sechdrs,
            secstrings,
        })
    }

    fn find_section(&self, sect_name: &CStr) -> Result<&'a [u8]> {
        for shdr in self.sechdrs.iter().skip(1) {
            if shdr.sh_type == bindings::SHT_NULL || shdr.sh_type == bindings::SHT_NOBITS {
                continue;
            }
            let name_start = shdr.sh_name as usize;
            let name =
                CStr::from_bytes_until_nul(&self.secstrings[name_start..]).map_err(|_| ENOEXEC)?;
            if name == sect_name {
                let off = usize::try_from(shdr.sh_offset).map_err(|_| ENOEXEC)?;
                let sz = usize::try_from(shdr.sh_size).map_err(|_| ENOEXEC)?;
                let end = off.checked_add(sz).ok_or(ENOEXEC)?;
                let slice = self.buf.get(off..end).ok_or(ENOEXEC)?;
                return Ok(slice);
            }
        }
        Err(ENOEXEC)
    }
}

fn bpf_capable() -> bool {
    // SAFETY: `capable` is safe to call from process context.
    unsafe {
        bindings::capable(bindings::CAP_BPF as ffi::c_int)
            || bindings::capable(bindings::CAP_SYS_ADMIN as ffi::c_int)
    }
}

// `kern_sys_bpf` is not declared in a public header file; it is exported in the
// `BPF_INTERNAL` namespace from `kernel/bpf/syscall.c`.
extern "C" {
    fn kern_sys_bpf(
        cmd: ffi::c_int,
        attr: *mut bindings::bpf_attr,
        size: ffi::c_uint,
    ) -> ffi::c_int;
}

fn encode_obj_name(name: &CStr) -> [ffi::c_char; bindings::BPF_OBJ_NAME_LEN as usize] {
    let mut buf = [0; bindings::BPF_OBJ_NAME_LEN as usize];
    let bytes = name.to_bytes();
    let len = bytes.len().min(buf.len() - 1);
    buf[..len].copy_from_slice(&bytes[..len]);
    buf
}

/// An RAII guard for a `struct bpf_map` reference acquired via `bpf_map_get`.
///
/// Automatically decrements the map reference count using `bpf_map_put` upon drop.
struct BpfMap {
    ptr: NonNull<bindings::bpf_map>,
}

impl BpfMap {
    fn from_fd(fd: ffi::c_int) -> Result<Self> {
        // SAFETY: `bpf_map_get` safely looks up the file descriptor in `current->files`
        // and returns either a valid pointer with an incremented refcount or an ERR_PTR.
        let raw_ptr = from_err_ptr(unsafe { bindings::bpf_map_get(fd as u32) })?;
        let ptr = NonNull::new(raw_ptr).ok_or(EINVAL)?;
        Ok(Self { ptr })
    }

    fn as_ptr(&self) -> *mut bindings::bpf_map {
        self.ptr.as_ptr()
    }

    fn is_frozen(&self) -> bool {
        // SAFETY: `self.ptr` points to a valid `struct bpf_map` for the lifetime of `self`.
        // Equivalent to `READ_ONCE(map->frozen)`.
        unsafe { core::ptr::read_volatile(&raw const (*self.as_ptr()).frozen) }
    }

    fn update_elem(&self, mut key: u32, data: &[u8]) -> Result {
        // SAFETY: `self.ptr` points to a valid `struct bpf_map`, and `(*map).ops` is a valid,
        // immutable vtable pointer for the lifetime of the map.
        let update_fn = unsafe { (*(*self.as_ptr()).ops).map_update_elem }.ok_or(EOPNOTSUPP)?;
        let key_ptr: *mut ffi::c_void = (&raw mut key).cast();
        let val_ptr: *mut ffi::c_void = data.as_ptr().cast_mut().cast();
        // SAFETY: `self.as_ptr()` is a valid array map created with `key_size = 4` and
        // `value_size = data.len()`. `key_ptr` points to a valid 4-byte `u32` key, and
        // `val_ptr` points to `data.len()` readable bytes.
        let err = unsafe { update_fn(self.as_ptr(), key_ptr, val_ptr, bindings::BPF_ANY as u64) };
        to_result(err as ffi::c_int)
    }
}

impl Drop for BpfMap {
    fn drop(&mut self) {
        // SAFETY: `self.ptr` was acquired via `bpf_map_get` and holds a valid reference count.
        unsafe { bindings::bpf_map_put(self.ptr.as_ptr()) };
    }
}

/// An RAII guard for a `struct bpf_prog` reference acquired via `bpf_prog_get_type_dev`.
///
/// Automatically decrements the program reference count using `bpf_prog_put` upon drop.
struct BpfProg {
    ptr: NonNull<bindings::bpf_prog>,
}

impl BpfProg {
    fn load_syscall(
        license: &[u8],
        insns: &[u8],
        fd_array: &[ffi::c_int],
        prog_name: &CStr,
    ) -> Result<Self> {
        let mut sattr = bindings::bpf_attr::default();
        sattr.__bindgen_anon_3.prog_type = bindings::bpf_prog_type_BPF_PROG_TYPE_SYSCALL;
        sattr.__bindgen_anon_3.license = (license.as_ptr() as usize) as u64;
        sattr.__bindgen_anon_3.insns = (insns.as_ptr() as usize) as u64;
        sattr.__bindgen_anon_3.insn_cnt = (insns.len() / size_of::<bindings::bpf_insn>()) as u32;
        sattr.__bindgen_anon_3.fd_array = (fd_array.as_ptr() as usize) as u64;
        sattr.__bindgen_anon_3.prog_flags = bindings::BPF_F_SLEEPABLE;
        sattr.__bindgen_anon_3.prog_name = encode_obj_name(prog_name);

        // SAFETY: Calling `kern_sys_bpf` with `BPF_PROG_LOAD` and a valid `bpf_attr` pointer.
        let prog_fd = unsafe {
            kern_sys_bpf(
                bindings::bpf_cmd_BPF_PROG_LOAD as ffi::c_int,
                &mut sattr,
                size_of::<bindings::bpf_attr>() as u32,
            )
        };
        if prog_fd < 0 {
            return Err(Error::from_errno(prog_fd));
        }

        // Pin the `struct bpf_prog` immediately and close the transient `prog_fd` entry in
        // `current->files`.
        // SAFETY: `bpf_prog_get_type_dev` safely looks up `prog_fd` in `current->files`,
        // verifies its type is `BPF_PROG_TYPE_SYSCALL`, and returns either a valid pointer
        // with an incremented refcount or an ERR_PTR. `close_fd` closes `prog_fd` in
        // `current->files`.
        let raw_ptr = unsafe {
            let ptr = bindings::bpf_prog_get_type_dev(
                prog_fd as u32,
                bindings::bpf_prog_type_BPF_PROG_TYPE_SYSCALL,
                false,
            );
            bindings::close_fd(prog_fd as u32);
            from_err_ptr(ptr)?
        };
        let ptr = NonNull::new(raw_ptr).ok_or(EINVAL)?;
        Ok(Self { ptr })
    }

    fn as_ptr(&self) -> *mut bindings::bpf_prog {
        self.ptr.as_ptr()
    }

    fn verify_binding(&self, map: &BpfMap, ctx_sz: u32) -> Result {
        // SAFETY: `self.ptr` points to a valid verified `struct bpf_prog`, and `(*prog).aux` is a
        // valid `struct bpf_prog_aux` pointer initialized during program allocation/verification.
        let aux = unsafe { (*self.as_ptr()).aux };
        if aux.is_null() {
            return Err(EINVAL);
        }

        // SAFETY: `aux` is valid for the lifetime of `self`. If `used_map_cnt == 1`,
        // `used_maps` points to a valid array of 1 `*mut bpf_map`.
        unsafe {
            if (*aux).used_map_cnt != 1
                || (*aux).used_maps.is_null()
                || *(*aux).used_maps != map.as_ptr()
            {
                return Err(EAGAIN);
            }
            if ctx_sz < (*aux).max_ctx_offset {
                return Err(EINVAL);
            }
        }
        Ok(())
    }

    fn run(&self, kctx: &mut [u8]) -> Result {
        // SAFETY: `self.as_ptr()` is a valid sleepable `BPF_PROG_TYPE_SYSCALL` program whose
        // `max_ctx_offset` has been verified against `kctx.len()`. Sleepable BPF programs
        // must be executed within an RCU tasks trace read-side critical section.
        let retval = unsafe {
            bindings::rcu_read_lock_trace();
            let ret = bindings::bpf_prog_run_pin_on_cpu(
                self.as_ptr(),
                kctx.as_mut_ptr().cast::<ffi::c_void>(),
            );
            bindings::rcu_read_unlock_trace();
            ret as i32
        };
        if retval < 0 {
            if retval < -(bindings::MAX_ERRNO as i32) {
                return Err(EINVAL);
            }
            return Err(Error::from_errno(retval));
        }
        Ok(())
    }
}

impl Drop for BpfProg {
    fn drop(&mut self) {
        // SAFETY: `self.ptr` was acquired via `bpf_prog_get_type_dev` and holds a valid refcount.
        unsafe { bindings::bpf_prog_put(self.ptr.as_ptr()) };
    }
}

fn bpf_create_array_map(value_size: u32, map_name: &CStr) -> Result<ffi::c_int> {
    let mut sattr = bindings::bpf_attr::default();
    sattr.__bindgen_anon_1.map_type = bindings::bpf_map_type_BPF_MAP_TYPE_ARRAY;
    sattr.__bindgen_anon_1.key_size = size_of::<u32>() as u32;
    sattr.__bindgen_anon_1.value_size = value_size;
    sattr.__bindgen_anon_1.max_entries = 1;
    sattr.__bindgen_anon_1.map_name = encode_obj_name(map_name);

    // SAFETY: Calling `kern_sys_bpf` with `BPF_MAP_CREATE` and a valid `bpf_attr` pointer.
    let fd = unsafe {
        kern_sys_bpf(
            bindings::bpf_cmd_BPF_MAP_CREATE as ffi::c_int,
            &mut sattr,
            size_of::<bindings::bpf_attr>() as u32,
        )
    };
    if fd < 0 {
        return Err(Error::from_errno(fd));
    }
    Ok(fd)
}

fn bpf_freeze_map(map_fd: ffi::c_int) -> Result {
    let mut sattr = bindings::bpf_attr::default();
    sattr.__bindgen_anon_2.map_fd = map_fd as u32;

    // SAFETY: Calling `kern_sys_bpf` with `BPF_MAP_FREEZE` and a valid `bpf_attr` pointer.
    let err = unsafe {
        kern_sys_bpf(
            bindings::bpf_cmd_BPF_MAP_FREEZE as ffi::c_int,
            &mut sattr,
            size_of::<bindings::bpf_attr>() as u32,
        )
    };
    to_result(err)
}

fn prepare_loader_prog(
    insns: &[u8],
    data: &[u8],
    license: &[u8],
    ctx_sz: u32,
) -> Result<(BpfMap, BpfProg)> {
    // 1. Create array map and pin the underlying kernel `struct bpf_map` object.
    let map_fd = bpf_create_array_map(data.len() as u32, BPF_LOADER_MAP_SEC)?;
    let res = (|| -> Result<(BpfMap, BpfProg)> {
        let map = BpfMap::from_fd(map_fd)?;

        // 2. Freeze first so userspace cannot mutate map contents via `map_fd` or `map_id`,
        //    verify that `map` itself was frozen (detecting if `map_fd` was swapped),
        //    and populate the map directly from kernel space.
        bpf_freeze_map(map_fd)?;
        if !map.is_frozen() {
            return Err(EAGAIN);
        }
        map.update_elem(0, data)?;

        // 3. Load the loader program, pin the underlying `struct bpf_prog` object, and
        //    verify that it binds exclusively to our pinned `map` and accepts `ctx_sz`.
        let fd_array = [map_fd];
        let prog = BpfProg::load_syscall(license, insns, &fd_array, BPF_LOADER_PROG_SEC)?;
        prog.verify_binding(&map, ctx_sz)?;

        Ok((map, prog))
    })();

    // Close the transient `map_fd` slot in `current->files`; `BpfMap` and `BpfProg` hold
    // direct kernel references to the underlying objects.
    // SAFETY: `close_fd` safely closes the file descriptor in `current->files` if still open.
    unsafe { bindings::close_fd(map_fd as u32) };
    res
}

fn loader_load_fd(args: &AndroidBpfFdLoaderArg) -> Result {
    if !bpf_capable() {
        return Err(EPERM);
    }

    if args.ctx_size == 0 || args.ctx_size > u16::MAX as u32 {
        return Err(EINVAL);
    }

    let file = LocalFile::fget(args.loader_fd).map_err(|_| EINVAL)?;
    let vm_buf = read_kernel_file(&file, BPF_LOADER_MAX_SIZE)?;
    let elf_info = ElfInfo::parse(&vm_buf)?;

    let insns = elf_info.find_section(BPF_LOADER_PROG_SEC)?;
    if insns.is_empty() || (insns.len() % size_of::<bindings::bpf_insn>()) != 0 {
        pr_debug!("ELF Spec violation: invalid instruction section size\n");
        return Err(ENOEXEC);
    }

    let data = elf_info.find_section(BPF_LOADER_MAP_SEC)?;
    if data.is_empty() {
        pr_debug!("ELF Spec violation: invalid map section size\n");
        return Err(ENOEXEC);
    }

    let license = elf_info.find_section(BPF_LOADER_LICENSE_SEC)?;
    if license.is_empty() || license[license.len() - 1] != 0 {
        pr_debug!("ELF Spec violation: license section isn't null terminated\n");
        return Err(ENOEXEC);
    }

    let ctx_sz = args.ctx_size as usize;
    let ctx_addr = usize::try_from(args.ctx).map_err(|_| EINVAL)?;
    let (ureader, mut uwriter) =
        UserSlice::new(UserPtr::from_addr(ctx_addr), ctx_sz).reader_writer();
    let mut kctx = KVVec::<u8>::new();
    ureader.read_all(&mut kctx, GFP_KERNEL)?;

    let (_map, prog) = prepare_loader_prog(insns, data, license, kctx.len() as u32)?;

    // 4. Execute the pinned program directly without going through `prog_fd`.
    prog.run(&mut kctx)?;

    uwriter.write_slice(&kctx)?;

    Ok(())
}
