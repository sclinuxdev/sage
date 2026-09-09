use std::fs::File;

/// Read-only memory map of a file descriptor for Linux.
pub struct Mmap {
    ptr: *mut std::ffi::c_void,
    len: usize,
}

impl Mmap {
    /// Maps a file into read-only memory.
    ///
    /// # Safety
    /// The caller must ensure the underlying file is not modified or truncated while mapped.
    pub unsafe fn map(file: &File) -> std::io::Result<Self> {
        use std::os::fd::AsRawFd;
        let len = file.metadata()?.len() as usize;
        if len == 0 {
            return Ok(Self {
                ptr: std::ptr::null_mut(),
                len: 0,
            });
        }
        let ptr = unsafe {
            nix::libc::mmap(
                std::ptr::null_mut(),
                len,
                nix::libc::PROT_READ,
                nix::libc::MAP_SHARED,
                file.as_raw_fd(),
                0,
            )
        };
        if ptr == nix::libc::MAP_FAILED {
            return Err(std::io::Error::last_os_error());
        }
        Ok(Self { ptr, len })
    }
}

impl std::ops::Deref for Mmap {
    type Target = [u8];
    fn deref(&self) -> &Self::Target {
        if self.len == 0 || self.ptr.is_null() {
            &[]
        } else {
            unsafe { std::slice::from_raw_parts(self.ptr as *const u8, self.len) }
        }
    }
}

impl Drop for Mmap {
    fn drop(&mut self) {
        if self.len > 0 && !self.ptr.is_null() {
            unsafe {
                nix::libc::munmap(self.ptr, self.len);
            }
        }
    }
}

unsafe impl Send for Mmap {}
unsafe impl Sync for Mmap {}
