//! Minimal recursive directory tree walker without following symlinks.

use std::fs;
use std::path::{Path, PathBuf};

/// A directory walker that yields entries without following symlinks.
#[derive(Debug, Clone)]
pub struct WalkDir {
    root: PathBuf,
}

/// Iterator over directory entries.
pub struct IntoIter {
    stack: Vec<PathBuf>,
}

/// An entry yielded by `WalkDir`.
#[derive(Debug, Clone)]
pub struct DirEntry {
    path: PathBuf,
    file_type: fs::FileType,
}

impl DirEntry {
    pub fn path(&self) -> &Path {
        &self.path
    }
    pub fn into_path(self) -> PathBuf {
        self.path
    }
    pub fn file_type(&self) -> fs::FileType {
        self.file_type
    }
    pub fn file_name(&self) -> &std::ffi::OsStr {
        self.path.file_name().unwrap_or_default()
    }
}

impl WalkDir {
    pub fn new(root: impl AsRef<Path>) -> Self {
        Self {
            root: root.as_ref().to_path_buf(),
        }
    }

    pub fn follow_links(self, _follow: bool) -> Self {
        self
    }
}

impl IntoIterator for WalkDir {
    type Item = std::io::Result<DirEntry>;
    type IntoIter = IntoIter;

    fn into_iter(self) -> Self::IntoIter {
        IntoIter {
            stack: vec![self.root],
        }
    }
}

impl Iterator for IntoIter {
    type Item = std::io::Result<DirEntry>;

    fn next(&mut self) -> Option<Self::Item> {
        let path = self.stack.pop()?;
        let metadata = match fs::symlink_metadata(&path) {
            Ok(m) => m,
            Err(err) => return Some(Err(err)),
        };
        let ft = metadata.file_type();
        if ft.is_dir() {
            match fs::read_dir(&path) {
                Ok(read_dir) => {
                    let mut entries = Vec::new();
                    for entry in read_dir {
                        match entry {
                            Ok(e) => entries.push(e.path()),
                            Err(err) => return Some(Err(err)),
                        }
                    }
                    // Reverse sort so popping from stack processes in alphabetical order
                    entries.sort_by(|a, b| b.cmp(a));
                    self.stack.extend(entries);
                }
                Err(err) => return Some(Err(err)),
            }
        }
        Some(Ok(DirEntry {
            path,
            file_type: ft,
        }))
    }
}
