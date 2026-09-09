//! Repository and channel configuration models.

use serde::Deserialize;
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use crate::error::RepoError;

/// One root repository and its nested subchannels.
#[derive(Debug, Clone, Deserialize)]
pub struct ChannelConfig {
    pub url: String,
    pub priority: i32,
    pub signing_key: PathBuf,
    #[serde(default = "enabled")]
    pub enabled: bool,
    #[serde(default)]
    pub subchannels: BTreeMap<String, SubchannelConfig>,
}

#[derive(Debug, Clone, Deserialize)]
pub struct SubchannelConfig {
    pub alias: Option<String>,
    #[serde(rename = "type")]
    pub channel_type: Option<String>,
    pub scope: String,
    pub target_root: PathBuf,
    #[serde(default = "enabled")]
    pub enabled: bool,
}

/// Complete `/etc/sage/channels.toml` document.
#[derive(Debug, Clone, Deserialize)]
pub struct ChannelsConfig {
    pub schema_version: u32,
    pub channels: BTreeMap<String, ChannelConfig>,
}

impl ChannelsConfig {
    pub fn load(path: impl AsRef<Path>) -> Result<Self, RepoError> {
        let config: Self = toml::from_str(&std::fs::read_to_string(path)?)
            .map_err(|error| RepoError::InvalidConfig(error.to_string()))?;
        if config.schema_version != sage_core::SCHEMA_VERSION {
            return Err(RepoError::InvalidConfig(format!(
                "unsupported schema version {}",
                config.schema_version
            )));
        }
        for (name, channel) in &config.channels {
            if !valid_identifier(name)
                || channel.url.is_empty()
                || !safe_absolute_path(&channel.signing_key)
            {
                return Err(RepoError::InvalidConfig(format!(
                    "unsafe or incomplete channel '{name}'"
                )));
            }
            for (sub_name, subchannel) in &channel.subchannels {
                if !valid_identifier(sub_name)
                    || subchannel
                        .alias
                        .as_deref()
                        .is_some_and(|alias| !valid_identifier(alias))
                    || !safe_absolute_path(&subchannel.target_root)
                {
                    return Err(RepoError::InvalidConfig(format!(
                        "unsafe subchannel '{name}/{sub_name}'"
                    )));
                }
            }
        }
        Ok(config)
    }
}

fn valid_identifier(value: &str) -> bool {
    !value.is_empty()
        && !matches!(value, "." | "..")
        && value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'_' | b'-'))
}

fn safe_absolute_path(path: &Path) -> bool {
    path.is_absolute()
        && path.components().all(|component| {
            matches!(
                component,
                std::path::Component::RootDir | std::path::Component::Normal(_)
            )
        })
}

/// Derives a subchannel URL solely from configuration values.
pub fn subchannel_url(
    channel: &ChannelConfig,
    name: &str,
    subchannel: &SubchannelConfig,
) -> String {
    join_url(&channel.url, subchannel.alias.as_deref().unwrap_or(name))
}

pub(crate) fn join_url(base: &str, relative: &str) -> String {
    format!(
        "{}/{}",
        base.trim_end_matches('/'),
        relative.trim_start_matches('/')
    )
}

fn enabled() -> bool {
    true
}
