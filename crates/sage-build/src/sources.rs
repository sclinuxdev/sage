impl SourceSpec {
    fn validate(&self) -> Result<(), BuildError> {
        match self.kind {
            SourceKind::Archive => {
                if self.sha256.len() != 64
                    || !self
                        .sha256
                        .bytes()
                        .all(|byte| byte.is_ascii_digit() || matches!(byte, b'a'..=b'f'))
                    || !self.commit.is_empty()
                    || self.submodules
                {
                    return Err(BuildError::InvalidSpec(
                        "archive sources require one SHA-256 and no Git fields".into(),
                    ));
                }
            }
            SourceKind::Git => {
                let exact_commit = matches!(self.commit.len(), 40 | 64)
                    && self.commit.bytes().all(|byte| byte.is_ascii_hexdigit());
                let network_url = self.url.starts_with("https://")
                    || self.url.starts_with("http://")
                    || self.url.starts_with("ssh://")
                    || self.url.starts_with("git://")
                    || (self.url.starts_with("git@") && self.url.contains(':'));
                if !exact_commit
                    || !network_url
                    || !self.sha256.is_empty()
                    || self.strip_components.is_some()
                {
                    return Err(BuildError::InvalidSpec(
                        "Git sources require a full commit ID and a network URL".into(),
                    ));
                }
            }
            SourceKind::File => {
                if self.sha256.len() != 64
                    || !self
                        .sha256
                        .bytes()
                        .all(|byte| byte.is_ascii_digit() || matches!(byte, b'a'..=b'f'))
                    || !self.commit.is_empty()
                    || self.submodules
                    || self.strip_components.is_some()
                    || self.destination == Path::new(".")
                {
                    return Err(BuildError::InvalidSpec(
                        "file sources require one SHA-256, a destination, and no extraction fields"
                            .into(),
                    ));
                }
            }
        }
        Ok(())
    }
}

/// Fetches an exact Git object and exports its worktree without VCS metadata.
pub fn fetch_git_source(
    git: &Path,
    source: &SourceSpec,
    checkout: &Path,
    destination: &Path,
) -> Result<(), BuildError> {
    source.validate()?;
    if source.kind != SourceKind::Git {
        return Err(BuildError::InvalidSpec(
            "fetch_git_source requires kind='git'".into(),
        ));
    }
    run_git(git, checkout, "init", ["init", "--quiet"])?;
    run_git(
        git,
        checkout,
        "remote add",
        ["remote", "add", "origin", source.url.as_str()],
    )?;
    run_git(
        git,
        checkout,
        "fetch commit",
        ["fetch", "--quiet", "--depth=1", "origin", &source.commit],
    )?;
    run_git(
        git,
        checkout,
        "checkout commit",
        ["checkout", "--quiet", "--detach", "FETCH_HEAD"],
    )?;
    if source.submodules {
        run_git(
            git,
            checkout,
            "submodule checkout",
            [
                "submodule",
                "update",
                "--quiet",
                "--init",
                "--recursive",
                "--depth=1",
            ],
        )?;
    }
    export_git_tree(checkout, destination)
}

fn run_git<const N: usize>(
    git: &Path,
    checkout: &Path,
    operation: &str,
    arguments: [&str; N],
) -> Result<(), BuildError> {
    fs::create_dir_all(checkout)?;
    let status = Command::new(git)
        .args(["-c", "protocol.file.allow=never", "-C"])
        .arg(checkout)
        .args(arguments)
        .env_clear()
        .env("PATH", "/usr/bin:/bin")
        .env("GIT_CONFIG_NOSYSTEM", "1")
        .env("HOME", checkout)
        .status()?;
    if status.success() {
        Ok(())
    } else {
        Err(BuildError::GitFailed {
            operation: operation.into(),
            status,
        })
    }
}

pub fn export_git_tree(checkout: &Path, destination: &Path) -> Result<(), BuildError> {
    fs::create_dir_all(destination)?;
    for entry in walkdir::WalkDir::new(checkout).follow_links(false) {
        let entry = entry?;
        let relative = entry
            .path()
            .strip_prefix(checkout)
            .expect("Git walk remains below checkout");
        if relative.as_os_str().is_empty()
            || relative.components().any(|part| part.as_os_str() == ".git")
        {
            continue;
        }
        let first = relative.components().next().map(|part| part.as_os_str());
        if matches!(first, Some(name) if name == ".distfiles" || name == ".patches") {
            return Err(BuildError::InvalidSpec(
                "Git source uses a reserved build-input path".into(),
            ));
        }
        let target = destination.join(relative);
        if entry.file_type().is_dir() {
            fs::create_dir_all(target)?;
        } else if entry.file_type().is_symlink() {
            if let Some(parent) = target.parent() {
                fs::create_dir_all(parent)?;
            }
            std::os::unix::fs::symlink(fs::read_link(entry.path())?, target)?;
        } else if entry.file_type().is_file() {
            if let Some(parent) = target.parent() {
                fs::create_dir_all(parent)?;
            }
            fs::copy(entry.path(), &target)?;
            fs::set_permissions(&target, fs::metadata(entry.path())?.permissions())?;
        }
    }
    Ok(())
}

fn valid_feature_name(name: &str) -> bool {
    !name.is_empty()
        && name.bytes().all(|byte| {
            byte.is_ascii_lowercase() || byte.is_ascii_digit() || matches!(byte, b'_' | b'-')
        })
}

/// Stable sandbox filename for an independently verified source archive.
pub fn source_archive_name(index: usize) -> String {
    format!("{index:03}-source")
}

fn validate_source_destination(path: &Path) -> Result<(), BuildError> {
    let valid = !path.as_os_str().is_empty()
        && path.components().all(|component| {
            matches!(
                component,
                std::path::Component::CurDir | std::path::Component::Normal(_)
            )
        })
        && !path.starts_with(".distfiles")
        && !path.starts_with(".patches")
        && !path
            .as_os_str()
            .as_encoded_bytes()
            .iter()
            .any(|byte| matches!(byte, b'\t' | b'\n' | b'\r' | b'\0'));
    if valid {
        Ok(())
    } else {
        Err(BuildError::InvalidSpec(format!(
            "source destination must stay below the source root: {}",
            path.display()
        )))
    }
}
