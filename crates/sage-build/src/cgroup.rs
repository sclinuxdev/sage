//! Cgroup v2 resource limits enforcement (memory_limit, pids_limit) for sandbox builds.

use crate::BuildError;
use std::fs;
use std::os::fd::AsRawFd;
use std::os::unix::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::{Child, Command};
use std::sync::atomic::{AtomicU64, Ordering};

static CGROUP_SEQ: AtomicU64 = AtomicU64::new(0);

/// Manages a temporary cgroup v2 scope for confining build sandbox resources.
pub struct CgroupScope {
    scope_path: Option<PathBuf>,
}

impl CgroupScope {
    /// Creates and configures a cgroup v2 slice if resource limits are specified.
    ///
    /// If no limits are configured (`memory_limit` is empty or "max", and `pids_limit` == 0),
    /// this function returns an inactive `CgroupScope` without touching the filesystem.
    ///
    /// When limits are specified, this method attempts to find a permitted cgroup v2 hierarchy
    /// (such as the caller's own cgroup, systemd user-delegated `app.slice`, or the root cgroup),
    /// creates a unique sub-slice, and writes the resource bounds into `memory.max` and `pids.max`.
    /// Any failure during creation or limit application is reported as an error.
    pub fn new(memory_limit: &str, pids_limit: u32) -> Result<Self, BuildError> {
        let mem_bytes = parse_memory_limit(memory_limit);
        if mem_bytes.is_none()
            && !memory_limit.trim().is_empty()
            && !memory_limit.trim().eq_ignore_ascii_case("max")
        {
            return Err(BuildError::CgroupFailed(format!(
                "invalid memory limit: {memory_limit}"
            )));
        }
        if mem_bytes.is_none() && pids_limit == 0 {
            return Ok(Self { scope_path: None });
        }

        let scope_path = Self::setup_scope(mem_bytes, pids_limit)?;
        Ok(Self {
            scope_path: Some(scope_path),
        })
    }

    /// Returns true if this scope is active and enforcing resource limits.
    pub fn is_active(&self) -> bool {
        self.scope_path.is_some()
    }

    /// Returns the filesystem path to the cgroup v2 directory, if active.
    pub fn path(&self) -> Option<&Path> {
        self.scope_path.as_deref()
    }

    fn setup_scope(mem_bytes: Option<u64>, pids_limit: u32) -> Result<PathBuf, BuildError> {
        // Verify host cgroup v2 unified hierarchy mount
        let cgroup_root = Path::new("/sys/fs/cgroup");
        if !cgroup_root.join("cgroup.controllers").exists() {
            return Err(BuildError::CgroupFailed(
                "cgroup v2 controllers file /sys/fs/cgroup/cgroup.controllers not found".into(),
            ));
        }

        let self_cgroup = Self::get_self_cgroup(cgroup_root);
        let candidates = Self::find_candidate_bases(cgroup_root, &self_cgroup);
        let seq = CGROUP_SEQ.fetch_add(1, Ordering::Relaxed);
        let scope_name = format!("sage-build-{}-{}", std::process::id(), seq);

        let mut last_error = None;
        for base_dir in candidates {
            if !base_dir.is_dir() {
                continue;
            }

            // Verify process migration permission under cgroup v2 rules
            if !Self::can_migrate_to(&self_cgroup, &base_dir) {
                continue;
            }

            let scope_dir = base_dir.join(&scope_name);
            if let Err(err) = fs::create_dir(&scope_dir) {
                last_error = Some(format!(
                    "cannot create cgroup slice in {}: {err}",
                    base_dir.display()
                ));
                continue;
            }

            // Verify the created slice's cgroup.procs can be written
            let procs_file = scope_dir.join("cgroup.procs");
            if fs::OpenOptions::new()
                .write(true)
                .open(&procs_file)
                .is_err()
            {
                let _ = fs::remove_dir(&scope_dir);
                last_error = Some(format!(
                    "cannot open cgroup.procs for writing in {}",
                    scope_dir.display()
                ));
                continue;
            }

            match Self::configure_limits(&scope_dir, mem_bytes, pids_limit) {
                Ok(()) => return Ok(scope_dir),
                Err(err) => {
                    let _ = fs::remove_dir(&scope_dir);
                    last_error = Some(err.to_string());
                }
            }
        }

        Err(BuildError::CgroupFailed(format!(
            "unable to initialize cgroup v2 slice for requested resource limits: {}",
            last_error.unwrap_or_else(|| "no writable cgroup base directory discovered with process migration permissions".into())
        )))
    }

    /// Reads the current process's cgroup path relative to the cgroup root.
    fn get_self_cgroup(cgroup_root: &Path) -> PathBuf {
        if let Ok(self_cgroup) = fs::read_to_string("/proc/self/cgroup") {
            for line in self_cgroup.lines() {
                let mut parts = line.splitn(3, ':');
                let _id = parts.next();
                let _controllers = parts.next();
                if let Some(path) = parts.next() {
                    let rel = path.trim_start_matches('/');
                    if !rel.is_empty() {
                        return cgroup_root.join(rel);
                    }
                }
            }
        }
        cgroup_root.to_path_buf()
    }

    /// Computes the longest common ancestor directory of two paths.
    fn common_ancestor(a: &Path, b: &Path) -> PathBuf {
        let mut common = PathBuf::new();
        for (ca, cb) in a.components().zip(b.components()) {
            if ca == cb {
                common.push(ca);
            } else {
                break;
            }
        }
        common
    }

    /// Verifies whether processes from `self_cgroup` can be migrated to `base_dir`.
    ///
    /// Under the Linux cgroup v2 delegation model, migrating a process from cgroup A to
    /// cgroup B requires write access to `cgroup.procs` of the common ancestor of A and B.
    fn can_migrate_to(self_cgroup: &Path, base_dir: &Path) -> bool {
        let ancestor = Self::common_ancestor(self_cgroup, base_dir);
        let procs = ancestor.join("cgroup.procs");
        if !procs.exists() {
            return false;
        }
        fs::OpenOptions::new().write(true).open(&procs).is_ok()
    }

    /// Discovers viable base directories for creating child cgroup slices.
    ///
    /// Checks:
    /// 1. The process's own cgroup from `/proc/self/cgroup`
    /// 2. User-delegated slices under `/sys/fs/cgroup/user.slice/user-<uid>.slice/user@<uid>.service/app.slice`
    /// 3. The unified root `/sys/fs/cgroup` (usable when running as root or containerized)
    fn find_candidate_bases(cgroup_root: &Path, self_cgroup: &Path) -> Vec<PathBuf> {
        let mut candidates = Vec::new();

        // 1. Process's current cgroup
        candidates.push(self_cgroup.to_path_buf());

        // 2. User service app.slice delegated to current non-root UID
        let uid = nix::unistd::getuid().as_raw();
        if uid != 0 {
            candidates.push(cgroup_root.join(format!(
                "user.slice/user-{uid}.slice/user@{uid}.service/app.slice"
            )));
            candidates
                .push(cgroup_root.join(format!("user.slice/user-{uid}.slice/user@{uid}.service")));
        }

        // 3. Fallback to unified cgroup root
        candidates.push(cgroup_root.to_path_buf());

        candidates
    }

    /// Writes configured memory and PID limits into the target cgroup directory.
    fn configure_limits(
        dir: &Path,
        mem_bytes: Option<u64>,
        pids_limit: u32,
    ) -> Result<(), BuildError> {
        if let Some(bytes) = mem_bytes {
            let memory_file = dir.join("memory.max");
            write_limit(&memory_file, &bytes.to_string()).map_err(|err| {
                BuildError::CgroupFailed(format!(
                    "failed writing memory limit ({bytes} bytes) to {}: {err}",
                    memory_file.display()
                ))
            })?;
        }

        if pids_limit > 0 {
            let pids_file = dir.join("pids.max");
            write_limit(&pids_file, &pids_limit.to_string()).map_err(|err| {
                BuildError::CgroupFailed(format!(
                    "failed writing pids limit ({pids_limit}) to {}: {err}",
                    pids_file.display()
                ))
            })?;
        }

        Ok(())
    }

    /// Spawns a command after the child has joined this scope, or fails before exec.
    pub fn spawn(&self, mut command: Command) -> Result<Child, BuildError> {
        if let Some(dir) = &self.scope_path {
            // Open in the parent, without O_CREAT: a missing controller is an error.
            let procs = fs::OpenOptions::new()
                .write(true)
                .open(dir.join("cgroup.procs"))?;
            // SAFETY: after fork the closure only calls write and inspects errno.
            // Writing "0" moves the calling process. No parent/child handshake is
            // needed: Command::spawn waits for exec, so waiting for the parent here
            // would deadlock. The owned file keeps the descriptor valid until exec.
            unsafe {
                command.pre_exec(move || {
                    loop {
                        let written = nix::libc::write(procs.as_raw_fd(), b"0".as_ptr().cast(), 1);
                        if written == 1 {
                            return Ok(());
                        }
                        let error = std::io::Error::last_os_error();
                        if written < 0 && error.raw_os_error() == Some(nix::libc::EINTR) {
                            continue;
                        }
                        return Err(if written < 0 {
                            error
                        } else {
                            std::io::Error::from_raw_os_error(nix::libc::EIO)
                        });
                    }
                });
            }
        }
        Ok(command.spawn()?)
    }

    /// Attaches the target process PID to the cgroup scope.
    ///
    /// This writes the given PID into `cgroup.procs`. If the write fails,
    /// returns an error so the caller can abort execution immediately.
    pub fn attach_pid(&self, pid: u32) -> Result<(), BuildError> {
        if let Some(ref dir) = self.scope_path {
            let procs_file = dir.join("cgroup.procs");
            fs::write(&procs_file, pid.to_string()).map_err(|err| {
                BuildError::CgroupFailed(format!(
                    "failed attaching PID {pid} to {}: {err}",
                    procs_file.display()
                ))
            })?;
        }
        Ok(())
    }
}

fn write_limit(path: &Path, value: &str) -> std::io::Result<()> {
    use std::io::Write;
    fs::OpenOptions::new()
        .write(true)
        .open(path)?
        .write_all(value.as_bytes())
}

impl Drop for CgroupScope {
    fn drop(&mut self) {
        if let Some(ref dir) = self.scope_path {
            // Attempt to kill any surviving processes in the slice before removing the directory
            let kill_file = dir.join("cgroup.kill");
            if kill_file.exists() {
                let _ = fs::write(kill_file, "1");
            }
            let _ = fs::remove_dir(dir);
        }
    }
}

/// Parses a human-readable memory string (e.g., "4G", "512M", "1024K", "1048576") into byte count.
pub fn parse_memory_limit(s: &str) -> Option<u64> {
    let s = s.trim();
    if s.is_empty() || s.eq_ignore_ascii_case("max") {
        return None;
    }

    let (num_part, multiplier) = if s.ends_with(|c: char| c.is_ascii_alphabetic()) {
        let last = s.chars().last()?;
        let mult: u64 = match last.to_ascii_uppercase() {
            'K' => 1024,
            'M' => 1024 * 1024,
            'G' => 1024 * 1024 * 1024,
            'T' => 1024 * 1024 * 1024 * 1024,
            _ => return None,
        };
        (&s[..s.len() - 1], mult)
    } else {
        (s, 1)
    };

    num_part
        .trim()
        .parse::<u64>()
        .ok()
        .and_then(|val| val.checked_mul(multiplier))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_memory_limits_correctly() {
        assert_eq!(parse_memory_limit(""), None);
        assert_eq!(parse_memory_limit("max"), None);
        assert_eq!(parse_memory_limit("MAX"), None);
        assert_eq!(parse_memory_limit("1024"), Some(1024));
        assert_eq!(parse_memory_limit("16K"), Some(16 * 1024));
        assert_eq!(parse_memory_limit("512M"), Some(512 * 1024 * 1024));
        assert_eq!(parse_memory_limit("4G"), Some(4 * 1024 * 1024 * 1024));
    }
    #[test]
    fn rejects_invalid_limits_and_missing_controllers() {
        for value in ["oops", "4GB", "18446744073709551615T"] {
            assert!(CgroupScope::new(value, 0).is_err());
        }
        let directory = tempfile::tempdir().unwrap();
        assert!(CgroupScope::configure_limits(directory.path(), Some(1024), 0).is_err());
        assert!(!directory.path().join("memory.max").exists());
        assert!(CgroupScope::configure_limits(directory.path(), None, 8).is_err());
        assert!(!directory.path().join("pids.max").exists());
    }

    #[test]
    fn spawn_confines_before_exec_and_fails_closed_without_deadlock() {
        // A subprocess deadline turns a pre-exec regression into a test failure
        // instead of hanging the entire test runner indefinitely.
        const CHILD: &str = "SAGE_CGROUP_SPAWN_TEST";
        if std::env::var_os(CHILD).is_none() {
            let mut child = Command::new(std::env::current_exe().unwrap())
                .args([
                    "--exact",
                    "cgroup::tests::spawn_confines_before_exec_and_fails_closed_without_deadlock",
                ])
                .env(CHILD, "1")
                .process_group(0)
                .spawn()
                .unwrap();
            let deadline = std::time::Instant::now() + std::time::Duration::from_secs(10);
            loop {
                if let Some(status) = child.try_wait().unwrap() {
                    assert!(status.success());
                    return;
                }
                if std::time::Instant::now() >= deadline {
                    nix::sys::signal::killpg(
                        nix::unistd::Pid::from_raw(child.id() as i32),
                        nix::sys::signal::Signal::SIGKILL,
                    )
                    .unwrap();
                    child.wait().unwrap();
                    panic!("cgroup spawn failed to return before its deadline");
                }
                std::thread::sleep(std::time::Duration::from_millis(10));
            }
        }
        let directory = tempfile::tempdir().unwrap();
        let procs = directory.path().join("cgroup.procs");
        fs::write(&procs, b"").unwrap();
        let scope = CgroupScope {
            scope_path: Some(directory.path().into()),
        };
        let mut command = Command::new("/bin/sh");
        command
            .args(["-c", "test \"$(cat \"$1\")\" = 0", "sh"])
            .arg(&procs);
        assert!(scope.spawn(command).unwrap().wait().unwrap().success());
        assert_eq!(fs::read(&procs).unwrap(), b"0");
        assert!(
            scope
                .spawn(Command::new("/sage-test-missing-executable"))
                .is_err()
        );
        fs::remove_file(&procs).unwrap();
        std::os::unix::fs::symlink("/dev/full", &procs).unwrap();
        let marker = directory.path().join("escaped");
        let mut command = Command::new("/bin/sh");
        command.args(["-c", "touch \"$1\"", "sh"]).arg(&marker);
        assert!(scope.spawn(command).is_err());
        assert!(!marker.exists());
        fs::remove_file(&procs).unwrap();
        assert!(scope.spawn(Command::new("/bin/true")).is_err());
    }
    #[test]
    #[ignore = "requires writable cgroup v2 memory and pids controllers; exercised by Linux CI"]
    fn delegated_scope_confines_exec_and_descendants() {
        let scope = CgroupScope::new("32M", 16).unwrap();
        let path = scope.path().unwrap().to_path_buf();
        assert_eq!(
            fs::read_to_string(path.join("memory.max")).unwrap().trim(),
            "33554432"
        );
        assert_eq!(
            fs::read_to_string(path.join("pids.max")).unwrap().trim(),
            "16"
        );
        let directory = tempfile::tempdir().unwrap();
        let membership = directory.path().join("membership");
        let mut command = Command::new("/bin/sh");
        command
            .args([
                "-c",
                "cat /proc/self/cgroup > \"$1\"; /bin/sh -c 'cat /proc/self/cgroup' >> \"$1\"",
                "sh",
            ])
            .arg(&membership);
        assert!(scope.spawn(command).unwrap().wait().unwrap().success());
        let lines = fs::read_to_string(&membership).unwrap();
        assert_eq!(lines.lines().count(), 2);
        let name = path.file_name().unwrap().to_str().unwrap();
        assert!(
            lines
                .lines()
                .all(|line| line.starts_with("0::") && line.ends_with(name))
        );
        drop(scope);
        assert!(!path.exists());
    }
}
