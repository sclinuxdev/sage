//! Deterministic tar.zst packaging, constant-cost inspection, and safe extraction.

pub mod error;
pub mod extract;
pub mod format;
pub mod pack;
pub mod security;

pub use error::ArchiveError;
pub use extract::{
    extract_package, extract_package_with_config, extract_prevalidated_package,
    payload_link_target, read_payload_file, validate_package_payload,
};
pub use format::{
    ExtractionReport, FileRecord, ManagedBuildTool, PackageInspection, PackageManifest,
    build_file_index, format_file_index, inspect_package, parse_file_index,
};
pub use pack::create_package;
pub use security::clean_archive_path;
