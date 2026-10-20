use orbis_pkg_util::{PkgExtractor, SilentProgress, open_pkg};
use std::ffi::{CStr, c_char};
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::path::Path;

#[cfg(unix)]
use memmap2::Mmap;
#[cfg(unix)]
use orbis_pkg::Pkg;
#[cfg(unix)]
use std::fs::File;
#[cfg(unix)]
use std::mem::ManuallyDrop;
#[cfg(unix)]
use std::os::fd::FromRawFd;

fn write_error(buffer: *mut c_char, capacity: usize, message: &str) {
    if buffer.is_null() || capacity == 0 {
        return;
    }
    let bytes = message.as_bytes();
    let count = bytes.len().min(capacity - 1);
    unsafe {
        std::ptr::copy_nonoverlapping(bytes.as_ptr(), buffer.cast::<u8>(), count);
        *buffer.add(count) = 0;
    }
}

fn extract(input: *const c_char, output: *const c_char) -> Result<(), String> {
    if input.is_null() || output.is_null() {
        return Err("PKG input or output path is null".to_owned());
    }
    let input = unsafe { CStr::from_ptr(input) }
        .to_str()
        .map_err(|_| "PKG input path is not UTF-8".to_owned())?;
    let output = unsafe { CStr::from_ptr(output) }
        .to_str()
        .map_err(|_| "PKG output path is not UTF-8".to_owned())?;
    if input.is_empty() || output.is_empty() {
        return Err("PKG input or output path is empty".to_owned());
    }

    let package = unsafe { open_pkg(Path::new(input)) }
        .map_err(|error| format!("cannot open PKG: {error}"))?;
    PkgExtractor::new(&package, SilentProgress, false)
        .extract(Path::new(output))
        .map_err(|error| format!("cannot extract PKG/PFS: {error}"))
}

#[cfg(unix)]
fn extract_fd(input_fd: i32, output: *const c_char) -> Result<(), String> {
    if input_fd < 0 || output.is_null() {
        return Err("PKG descriptor or output path is unavailable".to_owned());
    }
    let output = unsafe { CStr::from_ptr(output) }
        .to_str()
        .map_err(|_| "PKG output path is not UTF-8".to_owned())?;
    if output.is_empty() {
        return Err("PKG output path is empty".to_owned());
    }




    let file = ManuallyDrop::new(unsafe { File::from_raw_fd(input_fd) });
    let raw = unsafe { Mmap::map(&*file) }
        .map_err(|error| format!("cannot map PKG descriptor: {error}"))?;
    let package = Pkg::new(raw)
        .map_err(|error| format!("cannot parse PKG: {error}"))?;
    PkgExtractor::new(&package, SilentProgress, false)
        .extract(Path::new(output))
        .map_err(|error| format!("cannot extract PKG/PFS: {error}"))
}

#[cfg(not(unix))]
fn extract_fd(_input_fd: i32, _output: *const c_char) -> Result<(), String> {
    Err("descriptor-backed PKG extraction is unavailable on this platform".to_owned())
}

#[unsafe(no_mangle)]
pub extern "C" fn lsx4_extract_ps4_pkg(
    input: *const c_char,
    output: *const c_char,
    error_buffer: *mut c_char,
    error_capacity: usize,
) -> i32 {
    match catch_unwind(AssertUnwindSafe(|| extract(input, output))) {
        Ok(Ok(())) => 0,
        Ok(Err(error)) => {
            write_error(error_buffer, error_capacity, &error);
            1
        }
        Err(_) => {
            write_error(error_buffer, error_capacity, "PKG extractor panicked");
            2
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn lsx4_extract_ps4_pkg_fd(
    input_fd: i32,
    output: *const c_char,
    error_buffer: *mut c_char,
    error_capacity: usize,
) -> i32 {
    match catch_unwind(AssertUnwindSafe(|| extract_fd(input_fd, output))) {
        Ok(Ok(())) => 0,
        Ok(Err(error)) => {
            write_error(error_buffer, error_capacity, &error);
            1
        }
        Err(_) => {
            write_error(error_buffer, error_capacity, "PKG extractor panicked");
            2
        }
    }
}
