//! Live-upgrade safety audit: scans running processes for deleted shared libraries and binaries.

use std::collections::BTreeSet;
use std::fs;
use std::path::Path;

/// Audit record of a running process referencing replaced/deleted binaries or libraries on disk.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ProcessAudit {
    pub pid: u32,
    pub comm: String,
    pub deleted_files: Vec<String>,
}

/// Audits running processes under `/proc` to detect processes holding deleted files (e.g., upgraded libraries).
pub fn audit_running_processes(sysroot: &Path) -> Vec<ProcessAudit> {
    // Audit only inspects the live host's /proc when targeting host root or when /proc is mounted.
    let proc_dir = if sysroot == Path::new("/") {
        Path::new("/proc")
    } else {
        let guest_proc = sysroot.join("proc");
        if guest_proc.is_dir() {
            // If sysroot has /proc mounted
            return scan_proc(&guest_proc, sysroot);
        }
        Path::new("/proc")
    };

    scan_proc(proc_dir, sysroot)
}

fn scan_proc(proc_dir: &Path, sysroot: &Path) -> Vec<ProcessAudit> {
    let entries = match fs::read_dir(proc_dir) {
        Ok(entries) => entries,
        Err(_) => return Vec::new(),
    };

    let mut results = Vec::new();
    let sysroot_prefix = sysroot.to_str().unwrap_or("/");

    for entry in entries.flatten() {
        let file_name = entry.file_name();
        let name_str = file_name.to_string_lossy();
        let pid: u32 = match name_str.parse() {
            Ok(p) => p,
            Err(_) => continue,
        };

        let pid_dir = entry.path();
        let maps_path = pid_dir.join("maps");
        let maps_content = match fs::read_to_string(&maps_path) {
            Ok(content) => content,
            Err(_) => continue,
        };

        let mut deleted_files = BTreeSet::new();
        for line in maps_content.lines() {
            if !line.ends_with("(deleted)") {
                continue;
            }
            // Format: address perms offset dev inode pathname (deleted)
            let parts: Vec<&str> = line.split_whitespace().collect();
            if parts.len() < 6 {
                continue;
            }
            let file_index = 5;
            let path_str = parts[file_index..parts.len() - 1].join(" ");

            // Only report deleted shared libraries (.so) or binaries in system directories
            if is_system_library_or_binary(&path_str, sysroot_prefix) {
                deleted_files.insert(path_str);
            }
        }

        if !deleted_files.is_empty() {
            let comm = fs::read_to_string(pid_dir.join("comm"))
                .map(|s| s.trim().to_string())
                .unwrap_or_else(|_| format!("pid-{pid}"));

            results.push(ProcessAudit {
                pid,
                comm,
                deleted_files: deleted_files.into_iter().collect(),
            });
        }
    }

    results.sort_by_key(|r| r.pid);
    results
}

fn is_system_library_or_binary(path: &str, sysroot_prefix: &str) -> bool {
    let trimmed = path.strip_prefix(sysroot_prefix).unwrap_or(path);
    let is_sys_path = trimmed.starts_with("/usr/lib")
        || trimmed.starts_with("/usr/bin")
        || trimmed.starts_with("/usr/sbin")
        || trimmed.starts_with("/lib")
        || trimmed.starts_with("/bin")
        || trimmed.starts_with("/sbin")
        || trimmed.starts_with("usr/lib")
        || trimmed.starts_with("usr/bin")
        || trimmed.starts_with("usr/sbin")
        || trimmed.starts_with("lib")
        || trimmed.starts_with("bin")
        || trimmed.starts_with("sbin");

    is_sys_path && (path.contains(".so") || !path.contains('.'))
}

/// Pretty prints the live-upgrade process audit to warn the administrator.
pub fn print_process_audit(audits: &[ProcessAudit]) {
    if audits.is_empty() {
        return;
    }
    println!("\n[Live-Upgrade Warning] Running processes referencing deleted libraries/binaries:");
    for audit in audits {
        println!("  - PID {} ({}):", audit.pid, audit.comm);
        for file in &audit.deleted_files {
            println!("      {file} (deleted)");
        }
    }
    println!("Hint: Restart the affected services/processes to complete the update.\n");
}
