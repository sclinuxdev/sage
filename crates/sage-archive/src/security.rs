//! Security boundaries, directory traversal protection, and safe dirfd operations.

use crate::error::ArchiveError;
use nix::errno::Errno;
use nix::fcntl::{OFlag, openat};
use nix::sys::stat::{Mode, mkdirat};
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd};
use std::path::{Component, Path, PathBuf};

pub(crate) struct TempGuard {
    pub dirfd: i32,
    pub name: String,
    pub active: bool,
}

impl Drop for TempGuard {
    fn drop(&mut self) {
        if self.active {
            let _ = nix::unistd::unlinkat(
                Some(self.dirfd),
                self.name.as_str(),
                nix::unistd::UnlinkatFlags::NoRemoveDir,
            );
        }
    }
}

pub(crate) fn archive_mode(mode: u32) -> Mode {
    Mode::from_bits_truncate(mode as nix::libc::mode_t)
}

pub(crate) fn ensure_directory(root: &OwnedFd, path: &Path) -> Result<OwnedFd, ArchiveError> {
    let duplicate = openat(
        Some(root.as_raw_fd()),
        ".",
        OFlag::O_RDONLY | OFlag::O_DIRECTORY | OFlag::O_CLOEXEC,
        Mode::empty(),
    )?;
    // SAFETY: the newly duplicated descriptor has one owner.
    let mut current = unsafe { OwnedFd::from_raw_fd(duplicate) };
    for component in path.components() {
        let Component::Normal(name) = component else {
            return Err(ArchiveError::UnsafePath(path.display().to_string()));
        };
        match mkdirat(
            Some(current.as_raw_fd()),
            name,
            Mode::from_bits_truncate(0o755),
        ) {
            Ok(()) | Err(Errno::EEXIST) => {}
            Err(error) => return Err(error.into()),
        }
        let next = openat(
            Some(current.as_raw_fd()),
            name,
            OFlag::O_RDONLY | OFlag::O_DIRECTORY | OFlag::O_CLOEXEC | OFlag::O_NOFOLLOW,
            Mode::empty(),
        )?;
        // SAFETY: `openat` returned a new descriptor replacing the previous guard.
        current = unsafe { OwnedFd::from_raw_fd(next) };
    }
    Ok(current)
}

pub fn clean_archive_path(path: &Path) -> Result<PathBuf, ArchiveError> {
    let mut clean = PathBuf::new();
    for component in path.components() {
        match component {
            Component::CurDir => {}
            Component::Normal(value) => clean.push(value),
            _ => return Err(ArchiveError::UnsafePath(path.display().to_string())),
        }
    }
    if clean.as_os_str().is_empty() {
        return Err(ArchiveError::UnsafePath(path.display().to_string()));
    }
    Ok(clean)
}

pub(crate) fn validate_link_target(link: &Path, target: &Path) -> Result<(), ArchiveError> {
    if target.is_absolute() {
        return Err(ArchiveError::UnsafePath(target.display().to_string()));
    }
    let mut depth = link
        .parent()
        .map_or(0, |parent| parent.components().count());
    for component in target.components() {
        match component {
            Component::CurDir => {}
            Component::Normal(_) => depth += 1,
            Component::ParentDir if depth > 0 => depth -= 1,
            _ => return Err(ArchiveError::UnsafePath(target.display().to_string())),
        }
    }
    Ok(())
}

pub(crate) fn validate_metadata_path(path: &Path) -> Result<(), ArchiveError> {
    const ALLOWED: &[&str] = &[
        ".METADATA/manifest.toml",
        ".METADATA/files.idx",
        ".METADATA/service.toml",
        ".METADATA/triggers.toml",
        ".METADATA/alternatives.toml",
        ".METADATA/sysusers.toml",
    ];
    if ALLOWED.iter().any(|allowed| path == Path::new(allowed)) {
        Ok(())
    } else {
        Err(ArchiveError::InvalidMetadata(format!(
            "unsupported metadata entry {}; lifecycle scripts are not permitted",
            path.display()
        )))
    }
}
