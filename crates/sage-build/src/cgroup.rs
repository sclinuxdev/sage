//! Cgroup v2 resource limits enforcement (memory_limit, pids_limit) for sandbox builds.

use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

static CGROUP_SEQ: AtomicU64 = AtomicU64::new(0);

/// Manages a temporary cgroup v2 scope for confining build resources.
pub struct CgroupScope {
    scope_path: Option<PathBuf>,
}

impl CgroupScope {
    /// Creates and configures a cgroup v2 slice if supported and permitted by the host.
    pub fn new(memory_limit: &str, pids_limit: u32) -> Self {
        let scope_path = Self::setup_scope(memory_limit, pids_limit);
        Self { scope_path }
    }

    fn setup_scope(memory_limit: &str, pids_limit: u32) -> Option<PathBuf> {
        // Cgroup v2 unified hierarchy check
        let cgroup_root = Path::new("/sys/fs/cgroup");
        if !cgroup_root.join("cgroup.controllers").exists() {
            return None;
        }

        // Determine parent cgroup from /proc/self/cgroup
        let self_cgroup = fs::read_to_string("/proc/self/cgroup").ok()?;
        let self_rel_path = self_cgroup
            .lines()
            .find_map(|line| {
                let mut parts = line.splitn(3, ':');
                let _id = parts.next()?;
                let _controllers = parts.next()?;
                let path = parts.next()?;
                Some(path.trim_start_matches('/'))
            })
            .unwrap_or("");

        let base_dir = if !self_rel_path.is_empty() {
            cgroup_root.join(self_rel_path)
        } else {
            cgroup_root.to_path_buf()
        };

        let seq = CGROUP_SEQ.fetch_add(1, Ordering::Relaxed);
        let scope_dir = base_dir.join(format!("sage-build-{}-{}", std::process::id(), seq));

        if fs::create_dir(&scope_dir).is_err() {
            // If creating under self cgroup fails, attempt directly at cgroup root
            let fallback = cgroup_root.join(format!("sage-build-{}-{}", std::process::id(), seq));
            if fs::create_dir(&fallback).is_err() {
                return None;
            }
            return Self::configure_limits(fallback, memory_limit, pids_limit);
        }

        Self::configure_limits(scope_dir, memory_limit, pids_limit)
    }

    fn configure_limits(dir: PathBuf, memory_limit: &str, pids_limit: u32) -> Option<PathBuf> {
        // Apply memory limit if specified
        if let Some(bytes) = parse_memory_limit(memory_limit) {
            let _ = fs::write(dir.join("memory.max"), bytes.to_string());
        }

        // Apply pids limit if specified
        if pids_limit > 0 {
            let _ = fs::write(dir.join("pids.max"), pids_limit.to_string());
        }

        Some(dir)
    }

    /// Attaches the target process PID to the cgroup scope.
    pub fn attach_pid(&self, pid: u32) {
        if let Some(ref dir) = self.scope_path {
            let procs_file = dir.join("cgroup.procs");
            let _ = fs::write(procs_file, pid.to_string());
        }
    }
}

impl Drop for CgroupScope {
    fn drop(&mut self) {
        if let Some(ref dir) = self.scope_path {
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
        .map(|val| val.saturating_mul(multiplier))
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
}
