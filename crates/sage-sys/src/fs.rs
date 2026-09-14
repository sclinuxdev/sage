//! Dirfd-anchored filesystem operations, safe sysroot traversals,
//! atomic file writes, and non-following cache cleanups.

use std::os::fd::{AsRawFd, FromRawFd, OwnedFd};
use std::path::{Component, Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

use nix::errno::Errno;
use nix::fcntl::{AtFlags, OFlag, open, openat, renameat};
use nix::sys::stat::{Mode, fstatat, mkdirat};
use nix::unistd::{UnlinkatFlags, unlinkat};

use crate::SysError;
use crate::gc::CleanReport;

static TEMP_SEQ: AtomicU64 = AtomicU64::new(0);

/// RAII cleanup guard for temporary files created beneath a directory descriptor.
pub(crate) struct TempFileGuard {
    pub dirfd: i32,
    pub name: String,
    pub active: bool,
}

impl Drop for TempFileGuard {
    fn drop(&mut self) {
        if self.active {
            let _ = unlinkat(
                Some(self.dirfd),
                self.name.as_str(),
                UnlinkatFlags::NoRemoveDir,
            );
        }
    }
}

/// Opens the sysroot base directory as a file descriptor.
pub fn open_root(root: &Path) -> Result<OwnedFd, SysError> {
    let raw = open(
        root,
        OFlag::O_RDONLY | OFlag::O_DIRECTORY | OFlag::O_CLOEXEC,
        Mode::empty(),
    )?;
    // SAFETY: open returned a fresh descriptor transferred to OwnedFd.
    Ok(unsafe { OwnedFd::from_raw_fd(raw) })
}

/// Strips the sysroot prefix or root slashes from a path to obtain a safe relative path.
pub fn clean_relative_path(root: &Path, path: &Path) -> Result<PathBuf, SysError> {
    let path = path.strip_prefix(root).unwrap_or(path);
    let mut clean = PathBuf::new();
    for component in path.components() {
        match component {
            Component::RootDir | Component::CurDir => {}
            Component::Normal(val) => clean.push(val),
            Component::ParentDir | Component::Prefix(_) => {
                return Err(SysError::Invalid(format!(
                    "unsafe path escaping sysroot: {}",
                    path.display()
                )));
            }
        }
    }
    if clean.as_os_str().is_empty() {
        return Err(SysError::Invalid("empty relative path".into()));
    }
    Ok(clean)
}

/// Opens an existing subdirectory beneath `root` without following intermediate symlinks.
pub fn open_dir_beneath(root: &OwnedFd, relative: &Path) -> Result<OwnedFd, SysError> {
    let duplicate = openat(
        Some(root.as_raw_fd()),
        ".",
        OFlag::O_RDONLY | OFlag::O_DIRECTORY | OFlag::O_CLOEXEC,
        Mode::empty(),
    )?;
    // SAFETY: duplicate is a fresh descriptor owned by current.
    let mut current = unsafe { OwnedFd::from_raw_fd(duplicate) };

    for component in relative.components() {
        let Component::Normal(name) = component else {
            return Err(SysError::Invalid(format!(
                "invalid directory path component in {}",
                relative.display()
            )));
        };
        let next = openat(
            Some(current.as_raw_fd()),
            name,
            OFlag::O_RDONLY | OFlag::O_DIRECTORY | OFlag::O_CLOEXEC | OFlag::O_NOFOLLOW,
            Mode::empty(),
        )?;
        // SAFETY: next replaces current with a new validated dirfd.
        current = unsafe { OwnedFd::from_raw_fd(next) };
    }
    Ok(current)
}

/// Ensures each subdirectory exists beneath `root` without following intermediate symlinks.
pub fn ensure_dir_beneath(root: &OwnedFd, relative: &Path) -> Result<OwnedFd, SysError> {
    let duplicate = openat(
        Some(root.as_raw_fd()),
        ".",
        OFlag::O_RDONLY | OFlag::O_DIRECTORY | OFlag::O_CLOEXEC,
        Mode::empty(),
    )?;
    // SAFETY: duplicate is a fresh descriptor owned by current.
    let mut current = unsafe { OwnedFd::from_raw_fd(duplicate) };

    for component in relative.components() {
        let Component::Normal(name) = component else {
            return Err(SysError::Invalid(format!(
                "invalid directory path component in {}",
                relative.display()
            )));
        };
        match mkdirat(
            Some(current.as_raw_fd()),
            name,
            Mode::from_bits_truncate(0o755),
        ) {
            Ok(()) | Err(Errno::EEXIST) => {}
            Err(err) => return Err(err.into()),
        }
        let next = openat(
            Some(current.as_raw_fd()),
            name,
            OFlag::O_RDONLY | OFlag::O_DIRECTORY | OFlag::O_CLOEXEC | OFlag::O_NOFOLLOW,
            Mode::empty(),
        )?;
        // SAFETY: next replaces current with a new validated dirfd.
        current = unsafe { OwnedFd::from_raw_fd(next) };
    }
    Ok(current)
}

/// Unlinks a file beneath `root` without following intermediate symlinks.
/// If intermediate directories contain a symlink or the file does not exist,
/// returns without traversing or escapes.
pub fn unlink_beneath(root: &Path, target: &Path) -> Result<(), SysError> {
    let relative = match clean_relative_path(root, target) {
        Ok(rel) => rel,
        Err(_) => return Ok(()),
    };
    let root_fd = match open_root(root) {
        Ok(fd) => fd,
        Err(_) => return Ok(()),
    };
    let parent = relative.parent().unwrap_or_else(|| Path::new(""));
    let file_name = match relative.file_name().and_then(|n| n.to_str()) {
        Some(name) => name,
        None => return Ok(()),
    };
    let parent_fd = match open_dir_beneath(&root_fd, parent) {
        Ok(fd) => fd,
        Err(_) => return Ok(()),
    };
    match unlinkat(
        Some(parent_fd.as_raw_fd()),
        file_name,
        UnlinkatFlags::NoRemoveDir,
    ) {
        Ok(()) | Err(Errno::ENOENT) => Ok(()),
        Err(err) => Err(err.into()),
    }
}

/// Atomically writes content to a target path beneath `root` without following symlinks.
pub fn write_atomic_under_root(root: &Path, relative: &Path, bytes: &[u8]) -> Result<(), SysError> {
    let clean = clean_relative_path(root, relative)?;
    let root_fd = open_root(root)?;
    let parent = clean.parent().unwrap_or_else(|| Path::new(""));
    let file_name = clean
        .file_name()
        .and_then(|n| n.to_str())
        .ok_or_else(|| SysError::Invalid("target has no filename".into()))?;

    let parent_fd = ensure_dir_beneath(&root_fd, parent)?;
    let seq = TEMP_SEQ.fetch_add(1, Ordering::Relaxed);
    let temp_name = format!(".sage-tmp-{}-{seq}", std::process::id());

    let mut guard = TempFileGuard {
        dirfd: parent_fd.as_raw_fd(),
        name: temp_name.clone(),
        active: true,
    };

    let temp_raw = openat(
        Some(parent_fd.as_raw_fd()),
        temp_name.as_str(),
        OFlag::O_WRONLY | OFlag::O_CREAT | OFlag::O_EXCL | OFlag::O_CLOEXEC | OFlag::O_NOFOLLOW,
        Mode::from_bits_truncate(0o644),
    )?;
    // SAFETY: temp_raw is a new file descriptor.
    let temp_fd = unsafe { OwnedFd::from_raw_fd(temp_raw) };

    use std::io::Write as _;
    let mut file = std::fs::File::from(temp_fd);
    file.write_all(bytes)?;
    file.sync_all()?;
    drop(file);

    renameat(
        Some(parent_fd.as_raw_fd()),
        temp_name.as_str(),
        Some(parent_fd.as_raw_fd()),
        file_name,
    )?;
    guard.active = false;
    Ok(())
}

/// Recursively cleans cache directories beneath `root` without following symlinks.
pub fn clean_cache_beneath(root: &Path, all: bool) -> Result<CleanReport, SysError> {
    let mut report = CleanReport::default();
    let root_fd = match open_root(root) {
        Ok(fd) => fd,
        Err(_) => return Ok(report),
    };

    // Clean var/cache/sage
    if let Ok(cache_fd) = open_dir_beneath(&root_fd, Path::new("var/cache/sage")) {
        clean_dir_fd(&cache_fd, all, &mut report)?;
    }

    // Clean ephemeral temporary files in var/lib/sage
    if let Ok(lib_fd) = open_dir_beneath(&root_fd, Path::new("var/lib/sage")) {
        clean_temp_files_fd(&lib_fd, &mut report)?;
    }

    Ok(report)
}

fn clean_dir_fd(dir_fd: &OwnedFd, all: bool, report: &mut CleanReport) -> Result<(), SysError> {
    // Duplicate dirfd so nix::dir::Dir can take ownership
    let dup_raw = openat(
        Some(dir_fd.as_raw_fd()),
        ".",
        OFlag::O_RDONLY | OFlag::O_DIRECTORY | OFlag::O_CLOEXEC,
        Mode::empty(),
    )?;
    let mut dir = match nix::dir::Dir::from_fd(dup_raw) {
        Ok(d) => d,
        Err(_) => {
            let _ = nix::unistd::close(dup_raw);
            return Ok(());
        }
    };

    let mut subdirs_to_clean = Vec::new();
    let mut files_to_unlink = Vec::new();

    for entry in dir.iter() {
        let entry = match entry {
            Ok(e) => e,
            Err(_) => continue,
        };
        let file_name_bytes = entry.file_name().to_bytes();
        if file_name_bytes == b"." || file_name_bytes == b".." {
            continue;
        }
        let file_name_cstr = entry.file_name();
        let stat = match fstatat(
            Some(dir_fd.as_raw_fd()),
            file_name_cstr,
            AtFlags::AT_SYMLINK_NOFOLLOW,
        ) {
            Ok(s) => s,
            Err(_) => continue,
        };

        let mode = stat.st_mode as nix::libc::mode_t;
        let is_dir = (mode & nix::libc::S_IFMT) == nix::libc::S_IFDIR;
        let is_symlink = (mode & nix::libc::S_IFMT) == nix::libc::S_IFLNK;
        let is_reg = (mode & nix::libc::S_IFMT) == nix::libc::S_IFREG;

        // CRITICAL SECURITY BOUNDARY: Never follow symlinks when cleaning cache.
        if is_symlink {
            continue;
        }

        if is_dir {
            // Open child directory with O_NOFOLLOW
            if let Ok(child_raw) = openat(
                Some(dir_fd.as_raw_fd()),
                file_name_cstr,
                OFlag::O_RDONLY | OFlag::O_DIRECTORY | OFlag::O_CLOEXEC | OFlag::O_NOFOLLOW,
                Mode::empty(),
            ) {
                // SAFETY: child_raw transferred to OwnedFd.
                let child_fd = unsafe { OwnedFd::from_raw_fd(child_raw) };
                subdirs_to_clean.push((file_name_cstr.to_owned(), child_fd));
            }
        } else if is_reg {
            let name_lossy = String::from_utf8_lossy(file_name_bytes);
            let is_temp = name_lossy.contains("sage-tmp")
                || name_lossy.contains(".part-")
                || name_lossy.starts_with(".services-config-")
                || name_lossy.starts_with(".rendered-services-");
            let is_package = name_lossy.ends_with(".pkg.tar.zst");

            if is_temp || (all && is_package) {
                files_to_unlink.push((file_name_cstr.to_owned(), stat.st_size as u64));
            }
        }
    }

    // Clean subdirectories recursively
    for (name_cstr, child_fd) in subdirs_to_clean {
        clean_dir_fd(&child_fd, all, report)?;
        // Attempt to remove empty subdirectory
        let _ = unlinkat(
            Some(dir_fd.as_raw_fd()),
            name_cstr.as_c_str(),
            UnlinkatFlags::RemoveDir,
        );
    }

    // Unlink matching regular files
    for (name_cstr, size) in files_to_unlink {
        if unlinkat(
            Some(dir_fd.as_raw_fd()),
            name_cstr.as_c_str(),
            UnlinkatFlags::NoRemoveDir,
        )
        .is_ok()
        {
            report.files_removed += 1;
            report.bytes_freed += size;
        }
    }

    Ok(())
}

fn clean_temp_files_fd(dir_fd: &OwnedFd, report: &mut CleanReport) -> Result<(), SysError> {
    let dup_raw = openat(
        Some(dir_fd.as_raw_fd()),
        ".",
        OFlag::O_RDONLY | OFlag::O_DIRECTORY | OFlag::O_CLOEXEC,
        Mode::empty(),
    )?;
    let mut dir = match nix::dir::Dir::from_fd(dup_raw) {
        Ok(d) => d,
        Err(_) => {
            let _ = nix::unistd::close(dup_raw);
            return Ok(());
        }
    };

    let mut files_to_unlink = Vec::new();

    for entry in dir.iter() {
        let entry = match entry {
            Ok(e) => e,
            Err(_) => continue,
        };
        let file_name_bytes = entry.file_name().to_bytes();
        let file_name_cstr = entry.file_name();
        let stat = match fstatat(
            Some(dir_fd.as_raw_fd()),
            file_name_cstr,
            AtFlags::AT_SYMLINK_NOFOLLOW,
        ) {
            Ok(s) => s,
            Err(_) => continue,
        };

        let mode = stat.st_mode as nix::libc::mode_t;
        let is_reg = (mode & nix::libc::S_IFMT) == nix::libc::S_IFREG;

        if is_reg {
            let name_lossy = String::from_utf8_lossy(file_name_bytes);
            if name_lossy.starts_with(".services-config-")
                || name_lossy.starts_with(".rendered-services-")
            {
                files_to_unlink.push((file_name_cstr.to_owned(), stat.st_size as u64));
            }
        }
    }

    for (name_cstr, size) in files_to_unlink {
        if unlinkat(
            Some(dir_fd.as_raw_fd()),
            name_cstr.as_c_str(),
            UnlinkatFlags::NoRemoveDir,
        )
        .is_ok()
        {
            report.files_removed += 1;
            report.bytes_freed += size;
        }
    }

    Ok(())
}
