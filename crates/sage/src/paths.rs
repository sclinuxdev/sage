//! Sysroot path helpers shared by command modules.

use std::path::{Path, PathBuf};

pub(crate) fn under_root(root: &Path, path: &Path) -> PathBuf {
    root.join(path.strip_prefix("/").unwrap_or(path))
}
