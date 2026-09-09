//! Package creation, deterministic TAR ordering, and Zstandard compression.

use crate::error::ArchiveError;
use crate::security::validate_metadata_path;
use std::fs::{self, File};
use std::io::{self, Write};
use std::os::unix::fs::MetadataExt;
use std::path::{Path, PathBuf};

/// Creates a deterministic archive with metadata entries before payload entries.
pub fn create_package(
    source_dir: impl AsRef<Path>,
    output: impl AsRef<Path>,
    compression_level: i32,
) -> Result<(), ArchiveError> {
    let source = source_dir.as_ref();
    for required in [".METADATA/manifest.toml", ".METADATA/files.idx", "data"] {
        if !source.join(required).exists() {
            return Err(ArchiveError::InvalidMetadata(format!("missing {required}")));
        }
    }
    let mut paths = collect_paths(source)?;
    for path in &paths {
        let relative = path
            .strip_prefix(source)
            .expect("collected path stays below source");
        if relative.starts_with(".METADATA") && path.is_file() {
            validate_metadata_path(relative)?;
        }
    }
    paths.sort_by_key(|path| {
        let relative = path.strip_prefix(source).unwrap();
        (!relative.starts_with(".METADATA"), relative.to_path_buf())
    });
    let file = File::create(output)?;
    let mut encoder = zstd::Encoder::new(file, compression_level)?;
    encoder.include_checksum(true)?;
    let mut builder = tar::Builder::new(encoder);
    builder.mode(tar::HeaderMode::Deterministic);
    for path in paths {
        append_deterministic(&mut builder, source, &path)?;
    }
    let encoder = builder.into_inner()?;
    encoder.finish()?;
    Ok(())
}

fn collect_paths(root: &Path) -> Result<Vec<PathBuf>, ArchiveError> {
    fn visit(dir: &Path, paths: &mut Vec<PathBuf>) -> io::Result<()> {
        let mut entries: Vec<_> = fs::read_dir(dir)?.collect::<Result<_, _>>()?;
        entries.sort_by_key(|entry| entry.file_name());
        for entry in entries {
            let path = entry.path();
            if entry.file_type()?.is_dir() {
                visit(&path, paths)?;
            } else {
                paths.push(path);
            }
        }
        Ok(())
    }
    let mut paths = Vec::new();
    visit(root, &mut paths)?;
    Ok(paths)
}

fn append_deterministic<W: Write>(
    builder: &mut tar::Builder<W>,
    root: &Path,
    path: &Path,
) -> Result<(), ArchiveError> {
    let relative = path
        .strip_prefix(root)
        .expect("collected path stays below root");
    let metadata = fs::symlink_metadata(path)?;
    let mut header = tar::Header::new_gnu();
    header.set_uid(0);
    header.set_gid(0);
    header.set_mtime(0);
    header.set_mode(metadata.mode() & 0o7777);
    if metadata.is_file() {
        header.set_entry_type(tar::EntryType::Regular);
        header.set_size(metadata.len());
        header.set_cksum();
        builder.append_data(&mut header, relative, File::open(path)?)?;
    } else if metadata.file_type().is_symlink() {
        header.set_entry_type(tar::EntryType::Symlink);
        header.set_size(0);
        header.set_link_name(fs::read_link(path)?)?;
        header.set_cksum();
        builder.append_data(&mut header, relative, io::empty())?;
    } else {
        return Err(ArchiveError::UnsafePath(relative.display().to_string()));
    }
    Ok(())
}
