//! Advisory host locking and sysroot path helpers.

use crate::error::CoreError;
use nix::errno::Errno;
use nix::fcntl::{Flock, FlockArg, OFlag, open, openat};
use nix::sys::stat::{Mode, fchmod, fstat, mkdirat};
use nix::unistd::{fchown, geteuid};
use std::fs::File;
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd};
use std::os::unix::fs::{MetadataExt, PermissionsExt};
use std::path::{Component, Path, PathBuf};

/// Resolves a path relative to a target sysroot prefix.
pub fn under_root(root: &Path, path: &Path) -> PathBuf {
    root.join(path.strip_prefix("/").unwrap_or(path))
}

/// RAII guard around a host-wide advisory file lock.
pub struct HostLock {
    _file: Flock<File>,
    path: PathBuf,
}

impl HostLock {
    pub fn acquire_shared(path: impl AsRef<Path>) -> Result<Self, CoreError> {
        Self::acquire(path, false)
    }

    pub fn acquire_exclusive(path: impl AsRef<Path>) -> Result<Self, CoreError> {
        Self::acquire(path, true)
    }

    fn acquire(path: impl AsRef<Path>, exclusive: bool) -> Result<Self, CoreError> {
        let path = path.as_ref().to_path_buf();
        let file = open_lock_file(&path)?;
        let arg = if exclusive {
            FlockArg::LockExclusive
        } else {
            FlockArg::LockShared
        };
        let _file = Flock::lock(file, arg).map_err(|(_, errno)| CoreError::LockFailed {
            path: path.clone(),
            source: std::io::Error::from_raw_os_error(errno as i32),
        })?;
        Ok(Self { _file, path })
    }

    pub fn path(&self) -> &Path {
        &self.path
    }
}

/// Opens the lock through an anchored directory walk. Every component after
/// the filesystem root is opened with `O_NOFOLLOW`, so an attacker cannot
/// redirect two Sage processes onto different lock inodes through a symlink.
fn open_lock_file(path: &Path) -> Result<File, CoreError> {
    let parent = path
        .parent()
        .ok_or_else(|| CoreError::InvalidMetadata("operation lock has no parent".into()))?;
    let file_name = path
        .file_name()
        .ok_or_else(|| CoreError::InvalidMetadata("operation lock has no file name".into()))?;
    let base = if path.is_absolute() {
        Path::new("/")
    } else {
        Path::new(".")
    };
    let raw = open(
        base,
        OFlag::O_RDONLY | OFlag::O_DIRECTORY | OFlag::O_CLOEXEC,
        Mode::empty(),
    )
    .map_err(errno_io)?;
    // SAFETY: `open` returned a fresh descriptor transferred exactly once.
    let mut current = unsafe { OwnedFd::from_raw_fd(raw) };
    let normal_components = parent
        .components()
        .filter(|component| matches!(component, Component::Normal(_)))
        .count();
    let mut normal_index = 0_usize;
    for component in parent.components() {
        validate_lock_ancestor(&current, path)?;
        let name = match component {
            Component::RootDir | Component::CurDir => continue,
            Component::Normal(name) => {
                normal_index += 1;
                name
            }
            _ => {
                return Err(CoreError::InvalidMetadata(format!(
                    "unsafe operation lock path {}",
                    path.display()
                )));
            }
        };
        match mkdirat(
            Some(current.as_raw_fd()),
            name,
            Mode::from_bits_truncate(if normal_index == normal_components {
                0o700
            } else {
                0o755
            }),
        ) {
            Ok(()) | Err(Errno::EEXIST) => {}
            Err(error) => return Err(errno_io(error)),
        }
        let next = openat(
            Some(current.as_raw_fd()),
            name,
            OFlag::O_RDONLY | OFlag::O_DIRECTORY | OFlag::O_CLOEXEC | OFlag::O_NOFOLLOW,
            Mode::empty(),
        )
        .map_err(errno_io)?;
        // SAFETY: `openat` returned a fresh descriptor transferred exactly once.
        let next = unsafe { OwnedFd::from_raw_fd(next) };
        if normal_index == normal_components {
            harden_lock_directory(&next, path)?;
        }
        current = next;
    }
    let raw = openat(
        Some(current.as_raw_fd()),
        file_name,
        OFlag::O_RDWR | OFlag::O_CREAT | OFlag::O_CLOEXEC | OFlag::O_NOFOLLOW,
        Mode::from_bits_truncate(0o600),
    )
    .map_err(errno_io)?;
    // SAFETY: `openat` returned a fresh descriptor transferred exactly once.
    let file = unsafe { File::from_raw_fd(raw) };
    let metadata = file.metadata()?;
    if !metadata.is_file() || metadata.uid() != geteuid().as_raw() || metadata.nlink() != 1 {
        return Err(CoreError::InvalidMetadata(format!(
            "operation lock is not a private regular file: {}",
            path.display()
        )));
    }
    file.set_permissions(std::fs::Permissions::from_mode(0o600))?;
    Ok(file)
}

fn validate_lock_ancestor(directory: &OwnedFd, path: &Path) -> Result<(), CoreError> {
    let metadata = fstat(directory.as_raw_fd()).map_err(errno_io)?;
    let trusted_owner = metadata.st_uid == 0 || metadata.st_uid == geteuid().as_raw();
    let unsafe_writable = metadata.st_mode & 0o022 != 0 && metadata.st_mode & 0o1000 == 0;
    if !trusted_owner || unsafe_writable {
        return Err(CoreError::InvalidMetadata(format!(
            "operation lock parent is not trusted: {}",
            path.parent()
                .and_then(Path::parent)
                .unwrap_or(path)
                .display()
        )));
    }
    Ok(())
}

fn harden_lock_directory(directory: &OwnedFd, path: &Path) -> Result<(), CoreError> {
    let expected_owner = geteuid();
    let mut metadata = fstat(directory.as_raw_fd()).map_err(errno_io)?;
    if metadata.st_uid != expected_owner.as_raw() {
        fchown(directory.as_raw_fd(), Some(expected_owner), None).map_err(errno_io)?;
        metadata = fstat(directory.as_raw_fd()).map_err(errno_io)?;
    }
    if metadata.st_mode & 0o7777 != 0o700 {
        fchmod(directory.as_raw_fd(), Mode::from_bits_truncate(0o700)).map_err(errno_io)?;
        metadata = fstat(directory.as_raw_fd()).map_err(errno_io)?;
    }
    if metadata.st_uid != expected_owner.as_raw() || metadata.st_mode & 0o7777 != 0o700 {
        return Err(CoreError::InvalidMetadata(format!(
            "operation lock directory is not private: {}",
            path.parent().unwrap_or(path).display()
        )));
    }
    Ok(())
}

fn errno_io(error: Errno) -> CoreError {
    CoreError::Io(std::io::Error::from_raw_os_error(error as i32))
}
