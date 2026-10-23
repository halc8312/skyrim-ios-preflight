//! Apple-only loader. No /proc/ELF assumptions. SDK NativeHost owns the public
//! NSBundle/dladdr/dyld queries. Complete selected-file bytes and mapped header
//! are compared; mapped inode attestation is intentionally not claimed.
use crate::{
    Check,
    abi::*,
    abi_v2,
    binding::{MAX_FUNCTIONS, TrustedBinding},
    guest_abi::GuestAbi,
    mapping::{self, Image},
    paths::{FRAMEWORK_RELATIVE, PinnedFile},
};
use std::{
    ffi::{CStr, CString, c_char, c_void},
    path::{Path, PathBuf},
};
pub struct Api {
    // V2 requirements intentionally retain the exact V1 216-byte layout.
    pub requirements: RequirementsFn,
    pub bind: HostBind,
    pub publish: PublishFn,
    pub init: InitFn,
    pub call: CallFn,
    pub config: ConfigFn,
    pub functions: FunctionsFn,
}
#[derive(Clone, Copy)]
pub enum HostBind {
    V1(BindFn),
    V2(abi_v2::BindFn),
}
unsafe extern "C" {
    pub fn OHNativeHostLayoutV1(out: *mut u64, capacity: usize) -> i32;
    pub fn OHNativeHostLayoutV2(out: *mut u64, capacity: usize) -> i32;
    pub fn OHNativePlatformV1() -> u32;
    pub fn OHNativeOSVersionV1() -> u32;
    pub fn OHNativeBundlePathV1(out: *mut c_char, capacity: usize) -> i32;
    pub fn OHNativeSandboxPathV1(out: *mut c_char, capacity: usize) -> i32;
    fn OHNativeOpenGuestV1(canonical_path: *const c_char) -> *mut c_void;
    fn OHNativeLocateImageV1(
        symbol: *const c_void,
        header: *mut usize,
        slide: *mut i64,
        path: *mut c_char,
        capacity: usize,
    ) -> i32;
    pub fn OHNativePrepareHostV1(
        host: *mut PGHostCallbacksV1,
        bytes: u32,
        fixture_header: *const c_void,
    ) -> i32;
    pub fn OHNativePrepareHostV2(
        host: *mut abi_v2::PGHostCallbacksV2,
        bytes: u32,
        fixture_header: *const c_void,
    ) -> i32;
}
pub struct Library {
    handle: *mut c_void,
    pinned: PinnedFile,
    image: Image,
    header: usize,
    slide: i64,
    abi: GuestAbi,
}
// A successful owner is never unloaded. It holds no mutable guest state: all
// adapter concurrency is the validated public API's admission/atomic contract.
unsafe impl Send for Library {}
unsafe impl Sync for Library {}
impl Drop for Library {
    fn drop(&mut self) {
        if !self.handle.is_null() {
            unsafe { libc::dlclose(self.handle) };
        }
    }
}
pub fn sdk_string(call: impl FnOnce(*mut c_char, usize) -> i32) -> Check<PathBuf> {
    let mut bytes = vec![0u8; 4096];
    let size = call(bytes.as_mut_ptr().cast(), bytes.len());
    if size < 1 || size as usize >= bytes.len() || bytes[size as usize] != 0 {
        return Err("SDK bundle path query failed".into());
    }
    let text = CStr::from_bytes_with_nul(&bytes[..=size as usize])
        .map_err(|_| "SDK path is not terminated")?;
    use std::os::unix::ffi::OsStrExt;
    Ok(Path::new(std::ffi::OsStr::from_bytes(text.to_bytes())).to_path_buf())
}
fn locate(symbol: *mut c_void) -> Check<(usize, i64, PathBuf)> {
    let (mut header, mut slide) = (0usize, 0i64);
    let mut bytes = vec![0u8; 4096];
    if unsafe {
        OHNativeLocateImageV1(
            symbol,
            &mut header,
            &mut slide,
            bytes.as_mut_ptr().cast(),
            bytes.len(),
        )
    } != 0
        || header == 0
    {
        return Err("SDK dyld image lookup rejected symbol".into());
    }
    let text =
        CStr::from_bytes_until_nul(&bytes).map_err(|_| "dyld image path is not terminated")?;
    use std::os::unix::ffi::OsStrExt;
    let path = Path::new(std::ffi::OsStr::from_bytes(text.to_bytes()))
        .canonicalize()
        .map_err(|_| "dyld image path unavailable")?;
    Ok((header, slide, path))
}
impl Library {
    pub fn open(binding: &TrustedBinding) -> Check<Self> {
        let abi = binding.guest_abi()?;
        let bundle = sdk_string(|out, cap| unsafe { OHNativeBundlePathV1(out, cap) })?;
        let pinned = PinnedFile::open(
            &bundle,
            Path::new(FRAMEWORK_RELATIVE),
            (ios_signed_image_policy::MAX_IMAGE + ios_signed_image_policy::MAX_SIGNATURE) as u64,
        )?;
        ios_signed_image_policy::candidate_verify_binding(
            &pinned.bytes,
            &binding.normalization,
            &binding.unsigned_framework_sha256,
        )?;
        let platform = unsafe { OHNativePlatformV1() };
        if !matches!(platform, 2 | 7) {
            return Err("SDK host is not plain ARM64 iOS/device or Simulator".into());
        }
        let image = mapping::parse_after_content_binding(&pinned.bytes, platform)?;
        if image.min_os > unsafe { OHNativeOSVersionV1() } {
            return Err("fixture deployment target exceeds running OS".into());
        }
        use std::os::unix::ffi::OsStrExt;
        let path = CString::new(pinned.path.as_os_str().as_bytes())
            .map_err(|_| "bundle fixture path contains NUL")?;
        let handle = unsafe { OHNativeOpenGuestV1(path.as_ptr()) };
        if handle.is_null() {
            return Err("dyld refused the pinned public fixture".into());
        }
        // Construct owner before any following fallible check, so dlclose is
        // guaranteed on failure. dlsym alone does not establish image ownership.
        let mut owner = Self {
            handle,
            pinned,
            image,
            header: 0,
            slide: 0,
            abi,
        };
        let first = owner.raw(abi.requirements_symbol())?;
        let (header, slide, path) = locate(first)?;
        if path != owner.pinned.path
            || i128::from(owner.image.header_vmaddr) + i128::from(slide) != header as i128
        {
            return Err("loaded dyld image differs from selected bundle fixture".into());
        }
        owner.header = header;
        owner.slide = slide;
        let count = owner.image.header_and_commands_size;
        if !owner
            .image
            .contains_loaded(header, header, count, false, false)
        {
            return Err("loaded header is outside expected readable segment".into());
        }
        // The trusted SDK dyld header plus bounded file layout justify reading
        // this header; this is not an arbitrary caller-supplied pointer.
        let actual = unsafe { std::slice::from_raw_parts(header as *const u8, count) };
        if actual != &owner.pinned.bytes[..count] {
            return Err("loaded Mach-O header/commands differ from retained file".into());
        }
        // No text relocations are permitted in this fixture. Comparing all RX
        // file-backed bytes also rejects a stale already-loaded image whose
        // UUID/header survived an on-disk code replacement. Writable/fixup data
        // is not compared as if it were immutable. No mapped-inode claim follows.
        for segment in &owner.image.segments {
            if segment.initprot != 5 || segment.filesize == 0 {
                continue;
            }
            let start = i128::from(segment.vmaddr) + i128::from(slide);
            let length = usize::try_from(segment.filesize).map_err(|_| "RX bytecount overflow")?;
            if start < 0
                || start > usize::MAX as i128
                || !owner
                    .image
                    .contains_loaded(header, start as usize, length, false, false)
            {
                return Err("loaded RX segment extent rejected".into());
            }
            let offset = usize::try_from(segment.fileoff).map_err(|_| "RX file offset overflow")?;
            let end = offset
                .checked_add(length)
                .ok_or("RX file extent overflow")?;
            let expected = owner
                .pinned
                .bytes
                .get(offset..end)
                .ok_or("RX file range unavailable")?;
            let actual = unsafe { std::slice::from_raw_parts(start as *const u8, length) };
            if actual != expected {
                return Err("loaded RX bytes differ from complete pinned file".into());
            }
        }
        owner.pinned.recheck()?;
        Ok(owner)
    }
    fn raw(&self, name: &CStr) -> Check<*mut c_void> {
        let ptr = unsafe { libc::dlsym(self.handle, name.as_ptr()) };
        if ptr.is_null() {
            Err("fixture-owned API symbol unavailable".into())
        } else {
            Ok(ptr)
        }
    }
    fn symbol(&self, name: &CStr) -> Check<*mut c_void> {
        let ptr = self.raw(name)?;
        let (header, slide, path) = locate(ptr)?;
        if header != self.header
            || slide != self.slide
            || path != self.pinned.path
            || ptr as usize % 4 != 0
            || !self
                .image
                .contains_loaded(self.header, ptr as usize, 4, true, false)
        {
            return Err("API symbol is not RX code in the selected dyld image".into());
        }
        Ok(ptr)
    }
    pub fn api(&self) -> Check<Api> {
        // Concrete function types are bound only after ownership/RX checks.
        // The metadata-selected pair is mandatory; no other version is probed.
        let raw_bind = self.symbol(self.abi.bind_symbol())?;
        let bind = match self.abi {
            GuestAbi::V1 => {
                HostBind::V1(unsafe { std::mem::transmute::<*mut c_void, BindFn>(raw_bind) })
            }
            GuestAbi::V2 => HostBind::V2(unsafe {
                std::mem::transmute::<*mut c_void, abi_v2::BindFn>(raw_bind)
            }),
        };
        Ok(Api {
            requirements: unsafe {
                std::mem::transmute::<*mut c_void, RequirementsFn>(
                    self.symbol(self.abi.requirements_symbol())?,
                )
            },
            bind,
            publish: unsafe {
                std::mem::transmute::<*mut c_void, PublishFn>(self.symbol(c"PG_PublishMemoryV1")?)
            },
            init: unsafe {
                std::mem::transmute::<*mut c_void, InitFn>(self.symbol(c"PG_InitContextV1")?)
            },
            call: unsafe {
                std::mem::transmute::<*mut c_void, CallFn>(self.symbol(c"PG_CallGuestV1")?)
            },
            config: unsafe {
                std::mem::transmute::<*mut c_void, ConfigFn>(self.symbol(c"skyrt_ppc_config")?)
            },
            functions: unsafe {
                std::mem::transmute::<*mut c_void, FunctionsFn>(self.symbol(c"skyrt_func_addrs")?)
            },
        })
    }
    pub fn readonly_table(
        &self,
        ptr: *const u32,
        count: usize,
        expected_count: usize,
    ) -> Check<Box<[u32]>> {
        if count != expected_count
            || count == 0
            || count > MAX_FUNCTIONS
            || ptr.is_null()
            || ptr as usize % std::mem::align_of::<u32>() != 0
            || !self.image.contains_loaded(
                self.header,
                ptr as usize,
                count.checked_mul(4).ok_or("table bytecount overflow")?,
                false,
                true,
            )
        {
            return Err("function table is not a bounded read-only range of selected image".into());
        }
        // Copy, never extend a borrowed dylib slice to 'static.
        Ok(unsafe { std::slice::from_raw_parts(ptr, count) }
            .to_vec()
            .into_boxed_slice())
    }
    pub fn header(&self) -> *const c_void {
        self.header as *const c_void
    }
    pub fn native_trace(&self) -> crate::native_trace::NativeTrace {
        // Fixed storage; no symbolization or raw-PC logging. libc's unwinder
        // itself has no strict time guarantee, so callers must first arm the
        // independent fatal ceiling and publish their original failure record.
        let mut raw = [std::ptr::null_mut(); crate::native_trace::MAX_CAPTURE];
        let returned = unsafe { libc::backtrace(raw.as_mut_ptr(), raw.len() as i32) };
        let raw = raw.map(|pc| pc as usize);
        crate::native_trace::NativeTrace::filter(&raw, returned, self.header, |pc| {
            self.image.contains_loaded(self.header, pc, 1, true, false)
        })
    }
    pub fn release_bytes(&mut self) {
        self.pinned.release_bytes();
    }
    pub fn recheck(&mut self) -> Check<()> {
        self.pinned.recheck()
    }
}
