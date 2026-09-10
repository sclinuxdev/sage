//! Conditional repository synchronization, signature verification, and range downloads.

pub mod config;
pub mod download;
pub mod error;
pub mod index;
pub mod sign;

pub use config::{ChannelConfig, ChannelsConfig, SubchannelConfig, subchannel_url};
pub use download::DownloadEngine;
pub use error::RepoError;
pub use index::{
    IndexArtifacts, IndexedRelease, ReleaseLocation, ReleaseSource, RepositoryIndex, build_index,
    decompress, open_index, read_index_timestamp,
};
pub use sign::{SigningKey, VerifyingKey, decode_fixed, sign_file};
