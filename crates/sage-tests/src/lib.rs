//! Hermetic, model-checked fixtures for Sage package-manager torture testing.
use anyhow::{bail, Context, Result};
use rand::{rngs::SmallRng, Rng, SeedableRng};
use sage::{Cli, Commands};
use sage_core::{Dependency, Package, PackageKey, Version, SCHEMA_VERSION};
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::path::{Path, PathBuf};
use tempfile::TempDir;

/// Declarative package fixture that is converted into a real signed Sage archive.
#[derive(Debug, Clone)]
pub struct PackageSpec {
    pub channel: String,
    pub name: String,
    pub slot: String,
    pub version: u32,
    pub dependencies: Vec<String>,
    pub conflicts: Vec<String>,
    pub files: BTreeMap<String, Vec<u8>>,
}

impl PackageSpec {
    pub fn new(channel: &str, name: &str, version: u32, path: &str, content: &str) -> Self {
        Self {
            channel: channel.into(),
            name: name.into(),
            slot: "0".into(),
            version,
            dependencies: Vec::new(),
            conflicts: Vec::new(),
            files: BTreeMap::from([(path.into(), content.as_bytes().to_vec())]),
        }
    }

    fn key(&self) -> PackageKey {
        PackageKey::new(format!("main/{}", self.channel), &self.name, &self.slot)
    }

    fn ordered_version(&self) -> Version {
        Version::new(0, self.version.to_string(), 1)
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct StateSnapshot {
    pub packages: BTreeMap<String, String>,
    pub files: BTreeMap<String, Vec<u8>>,
    pub owners: BTreeMap<String, Vec<String>>,
}

/// One completely isolated target root, package pool, state database, and action log.
pub struct TortureLab {
    root: TempDir,
    pools: BTreeMap<String, TempDir>,
    catalog: BTreeMap<(PackageKey, Version), BTreeMap<String, Vec<u8>>>,
    archives: BTreeMap<(PackageKey, Version), PathBuf>,
    steps: Vec<String>,
}

impl TortureLab {
    pub fn new() -> Result<Self> {
        let root = tempfile::tempdir()?;
        let pools = ["system", "runtime", "toolchain"]
            .into_iter()
            .map(|channel| Ok((channel.into(), tempfile::tempdir()?)))
            .collect::<Result<BTreeMap<_, _>>>()?;
        let lab = Self {
            root,
            pools,
            catalog: BTreeMap::new(),
            archives: BTreeMap::new(),
            steps: Vec::new(),
        };
        lab.configure()?;
        Ok(lab)
    }

    pub fn root(&self) -> &Path {
        self.root.path()
    }

    pub fn steps(&self) -> &[String] {
        &self.steps
    }

    fn configure(&self) -> Result<()> {
        fs::create_dir_all(self.root().join("etc/sage"))?;
        fs::create_dir_all(self.root().join("run/sage"))?;
        fs::write(
            self.root().join("etc/sage/system.toml"),
            "schema_version=1\n[system]\narchitecture=\"amd64\"\nprofile=\"default\"\n",
        )?;
        fs::write(
            self.root().join("etc/sage/channels.toml"),
            r#"schema_version=1
[channels.main]
url="https://invalid.example/torture"
priority=100
signing_key="/etc/sage/repo.pub"
[channels.main.subchannels.system]
scope="system"
target_root="/"
[channels.main.subchannels.runtime]
scope="runtime"
target_root="/usr/lib/runtimes/torture"
[channels.main.subchannels.toolchain]
scope="toolchain"
target_root="/opt/channels/torture/1"
"#,
        )?;
        Ok(())
    }

    /// Builds one deterministic package through `sage-archive` and records its model payload.
    pub fn add_package(&mut self, spec: PackageSpec) -> Result<PathBuf> {
        let stage = tempfile::tempdir()?;
        let data = stage.path().join("data");
        fs::create_dir_all(stage.path().join(".METADATA"))?;
        fs::create_dir_all(&data)?;
        for (relative, content) in &spec.files {
            let target = data.join(relative);
            fs::create_dir_all(target.parent().context("fixture path has no parent")?)?;
            fs::write(target, content)?;
        }
        let records = sage_archive::build_file_index(&data)?;
        fs::write(
            stage.path().join(".METADATA/files.idx"),
            sage_archive::format_file_index(&records),
        )?;
        let dependencies = spec
            .dependencies
            .iter()
            .map(|value| value.parse::<Dependency>())
            .collect::<Result<Vec<_>, _>>()?;
        let installed_size = spec.files.values().map(Vec::len).sum::<usize>() as u64;
        let manifest = Package {
            schema_version: SCHEMA_VERSION,
            name: spec.name.clone(),
            slot: spec.slot.clone(),
            version: spec.version.to_string(),
            release: 1,
            epoch: 0,
            arch: "noarch".into(),
            channel: spec.channel.clone(),
            description: format!("torture fixture {}", spec.name),
            license: "MIT".into(),
            dependencies,
            provides: Vec::new(),
            conflicts: spec.conflicts.clone(),
            features: Vec::new(),
            installed_size,
            build_time: 1,
            managed_build_tools: Vec::new(),
        };
        fs::write(
            stage.path().join(".METADATA/manifest.toml"),
            toml::to_string_pretty(&manifest)?,
        )?;
        let pool = self
            .pools
            .get(&spec.channel)
            .with_context(|| format!("unknown fixture channel {}", spec.channel))?;
        let archive = pool.path().join(format!(
            "{}-{}-{}-1-noarch.pkg.tar.zst",
            spec.name, spec.slot, spec.version
        ));
        sage_archive::create_package(stage.path(), &archive, 1)?;
        let prefix = channel_target(&spec.channel)?;
        let coordinate = (spec.key(), spec.ordered_version());
        let physical = spec
            .files
            .into_iter()
            .map(|(path, content)| {
                (
                    prefix
                        .strip_prefix("/")
                        .unwrap_or(prefix)
                        .join(path)
                        .to_string_lossy()
                        .into_owned(),
                    content,
                )
            })
            .collect();
        self.catalog.insert(coordinate.clone(), physical);
        self.archives.insert(coordinate, archive.clone());
        Ok(archive)
    }

    pub fn add(
        &mut self,
        channel: &str,
        name: &str,
        version: u32,
        path: &str,
        content: &str,
    ) -> Result<PathBuf> {
        self.add_package(PackageSpec::new(channel, name, version, path, content))
    }

    /// Rebuilds each channel index and primes the content-addressed package cache.
    pub fn publish(&mut self) -> Result<()> {
        let key = self.root().join("signing.key");
        fs::write(&key, [23_u8; 32])?;
        for (channel, pool) in &self.pools {
            let has_packages = fs::read_dir(pool.path())?.any(|entry| {
                entry.ok().is_some_and(|entry| {
                    entry
                        .file_name()
                        .to_string_lossy()
                        .ends_with(".pkg.tar.zst")
                })
            });
            if !has_packages {
                continue;
            }
            sage_repo::build_index(pool.path(), pool.path(), &key)?;
            let destination = self
                .root()
                .join("var/cache/sage/channels/main")
                .join(channel)
                .join("index.mdb");
            fs::create_dir_all(destination.parent().unwrap())?;
            fs::copy(pool.path().join("index.mdb"), destination)?;
        }
        let cache = self.root().join("var/cache/sage/packages");
        fs::create_dir_all(&cache)?;
        for archive in self.archives.values() {
            if archive.exists() {
                let bytes = fs::read(archive)?;
                fs::copy(archive, cache.join(hex::encode(Sha256::digest(bytes))))?;
            }
        }
        self.steps.push("publish".into());
        Ok(())
    }

    pub async fn install(&mut self, name: &str, channel: &str) -> Result<()> {
        self.steps.push(format!("install {channel}/{name}"));
        sage::execute(cli(
            self.root(),
            Commands::Install {
                packages: vec![name.into()],
                channel: Some(channel.into()),
                no_save: true,
            },
        ))
        .await
    }

    pub async fn upgrade(&mut self, name: &str, channel: &str) -> Result<()> {
        self.steps.push(format!("upgrade {channel}/{name}"));
        sage::execute(cli(
            self.root(),
            Commands::Upgrade {
                packages: vec![name.into()],
                channel: Some(channel.into()),
                sync: false,
            },
        ))
        .await
    }

    pub async fn remove(&mut self, name: &str, channel: &str) -> Result<()> {
        self.steps.push(format!("remove {channel}/{name}"));
        sage::execute(cli(
            self.root(),
            Commands::Remove {
                packages: vec![name.into()],
                channel: Some(channel.into()),
            },
        ))
        .await
    }

    pub fn inject(&mut self, point: &str) -> Result<()> {
        self.steps.push(format!("fault {point}"));
        fs::write(self.root().join("run/sage/crash-point"), point)?;
        Ok(())
    }

    pub fn install_counting_trigger(&self) -> Result<PathBuf> {
        use std::os::unix::fs::PermissionsExt as _;
        let binary = self.root().join("usr/bin/torture-trigger");
        fs::create_dir_all(binary.parent().unwrap())?;
        fs::write(
            &binary,
            "#!/bin/sh\nprintf x >> \"$SAGE_SYSROOT/var/lib/sage/torture-trigger-count\"\n",
        )?;
        fs::set_permissions(&binary, fs::Permissions::from_mode(0o755))?;
        let trigger = self.root().join("etc/sage/triggers.d/torture.toml");
        fs::create_dir_all(trigger.parent().unwrap())?;
        fs::write(
            trigger,
            r#"schema_version=1
name="torture-counter"
description="Counts completed Torture Lab trigger executions"
on_paths=["usr/lib/torture/**"]
exec=["/usr/bin/torture-trigger"]
priority=1
events=["post-change"]
ignore_missing_binary=false
"#,
        )?;
        Ok(self.root().join("var/lib/sage/torture-trigger-count"))
    }

    pub fn snapshot(&self) -> Result<StateSnapshot> {
        let db_path = self.root().join("var/lib/sage");
        let installed = sage_db::read_packages(&db_path)?;
        let packages = installed
            .iter()
            .map(|package| (package.key.canonical_id(), package.version.to_string()))
            .collect();
        let mut files = BTreeMap::new();
        let mut owners = BTreeMap::new();
        for package in &installed {
            for relative in &package.files {
                let target = self.root().join(relative);
                let metadata = fs::symlink_metadata(&target)
                    .with_context(|| format!("missing managed path {relative}"))?;
                let content = if metadata.file_type().is_symlink() {
                    fs::read_link(&target)?
                        .as_os_str()
                        .as_encoded_bytes()
                        .to_vec()
                } else {
                    fs::read(&target)?
                };
                files.insert(relative.clone(), content);
                owners.insert(
                    relative.clone(),
                    sage_db::read_owners(&db_path, relative)?
                        .into_iter()
                        .map(|owner| owner.canonical_id())
                        .collect(),
                );
            }
        }
        Ok(StateSnapshot {
            packages,
            files,
            owners,
        })
    }

    /// Checks LMDB ownership, payload contents, pending journals, and unmanaged debris.
    pub fn audit(&self) -> Result<StateSnapshot> {
        let snapshot = self.snapshot()?;
        let installed = sage_db::read_packages(&self.root().join("var/lib/sage"))?;
        let mut expected = BTreeMap::new();
        for package in installed {
            let payload = self
                .catalog
                .get(&(package.key.clone(), package.version.clone()))
                .with_context(|| format!("model lacks {} {}", package.key, package.version))?;
            expected.extend(payload.clone());
            for path in &package.files {
                let owners = snapshot
                    .owners
                    .get(path)
                    .with_context(|| format!("ownership index lacks {path}"))?;
                if owners != &[package.key.canonical_id()] {
                    bail!("unexpected owners for {path}: {owners:?}");
                }
            }
        }
        if snapshot.files != expected {
            bail!(
                "model/filesystem divergence\nexpected={expected:?}\nactual={:?}",
                snapshot.files
            );
        }
        let database = sage_db::SageDatabase::open(self.root().join("var/lib/sage"))?;
        if !database.pending_journals()?.is_empty() {
            bail!("successful step left a pending operation journal");
        }
        let actual_managed = collect_managed_files(self.root())?;
        let expected_paths = expected.keys().cloned().collect::<BTreeSet<_>>();
        if actual_managed != expected_paths {
            bail!(
                "managed tree contains missing or unowned paths\nexpected={expected_paths:?}\nactual={actual_managed:?}"
            );
        }
        Ok(snapshot)
    }

    pub fn remove_archive(&mut self, channel: &str, name: &str, version: u32) -> Result<()> {
        let coordinate = (
            PackageKey::new(format!("main/{channel}"), name, "0"),
            Version::new(0, version.to_string(), 1),
        );
        let archive = self
            .archives
            .remove(&coordinate)
            .context("archive version is not present")?;
        fs::remove_file(archive)?;
        Ok(())
    }
}

/// Stable release-gate scenario covering dependencies, conflicts, channels, retries, and upgrades.
pub async fn run_quick() -> Result<Vec<String>> {
    let mut lab = TortureLab::new()?;
    lab.add("system", "base", 1, "usr/lib/torture/base", "base-v1")?;
    let mut app = PackageSpec::new("system", "app", 1, "usr/lib/torture/app", "app-v1");
    app.dependencies.push("base".into());
    lab.add_package(app)?;
    lab.add("system", "rival", 1, "usr/lib/torture/app", "rival")?;
    lab.add("runtime", "shared", 1, "bin/shared", "runtime")?;
    lab.add("toolchain", "shared", 1, "bin/shared", "toolchain")?;
    lab.publish()?;

    lab.install("app", "system").await?;
    let installed = lab.audit()?;
    lab.install("app", "system").await?;
    if lab.audit()? != installed {
        bail!("reinstall changed an already converged state");
    }
    let before_conflict = lab.audit()?;
    if lab.install("rival", "system").await.is_ok() || lab.audit()? != before_conflict {
        bail!("conflicting package changed committed state");
    }
    lab.install("shared", "runtime").await?;
    lab.install("shared", "toolchain").await?;
    lab.audit()?;
    if lab.remove("base", "system").await.is_ok() {
        bail!("dependency-owned package removal unexpectedly succeeded");
    }

    let mut app2 = PackageSpec::new("system", "app", 2, "usr/lib/torture/app", "app-v2");
    app2.dependencies.push("base".into());
    lab.add_package(app2)?;
    lab.publish()?;
    lab.inject("before-lmdb-write")?;
    if lab.upgrade("app", "system").await.is_ok() {
        bail!("fault-injected upgrade unexpectedly succeeded");
    }
    lab.upgrade("app", "system").await?;
    let upgraded = lab.audit()?;
    if upgraded.packages["main/system:app:0"] != "2-1" {
        bail!("retry did not converge on app v2");
    }

    let counter = lab.install_counting_trigger()?;
    let mut app3 = PackageSpec::new("system", "app", 3, "usr/lib/torture/app", "app-v3");
    app3.dependencies.push("base".into());
    lab.add_package(app3)?;
    lab.publish()?;
    lab.inject("trigger-complete")?;
    if lab.upgrade("app", "system").await.is_ok() {
        bail!("post-trigger checkpoint fault unexpectedly succeeded");
    }
    lab.upgrade("app", "system").await?;
    let upgraded = lab.audit()?;
    if fs::read(&counter)? != b"x" {
        bail!("completed trigger replayed after its journal checkpoint");
    }

    lab.remove_archive("system", "app", 3)?;
    lab.publish()?;
    lab.upgrade("app", "system").await?;
    if lab.audit()? != upgraded {
        bail!("repository rollback silently downgraded the installed package");
    }
    Ok(lab.steps)
}

#[derive(Debug, Clone)]
enum RandomOperation {
    Install(usize),
    Remove(usize),
    Conflict,
    FaultRetry(usize),
    Upgrade(usize),
    RollbackAttempt(usize),
    Toggle(&'static str),
    DependencyReplacement,
}

/// Model-driven random sequence. Every mutation is followed by a full state audit.
pub async fn run_random(seed: u64, operations: usize) -> Result<Vec<String>> {
    let mut rng = SmallRng::seed_from_u64(seed);
    let sequence = (0..operations)
        .map(|_| {
            let package = rng.gen_range(0..8);
            match rng.gen_range(0..9) {
                0 => RandomOperation::Install(package),
                1 => RandomOperation::Remove(package),
                2 => RandomOperation::Conflict,
                3 => RandomOperation::FaultRetry(package),
                4 => RandomOperation::Upgrade(package),
                5 => RandomOperation::RollbackAttempt(package),
                6 => RandomOperation::Toggle("runtime"),
                7 => RandomOperation::Toggle("toolchain"),
                _ => RandomOperation::DependencyReplacement,
            }
        })
        .collect::<Vec<_>>();
    execute_random_sequence(seed, &sequence)
        .await
        .with_context(|| format!("seed {seed} operations={sequence:?}"))
}

async fn execute_random_sequence(seed: u64, sequence: &[RandomOperation]) -> Result<Vec<String>> {
    let mut lab = TortureLab::new()?;
    let package_count = 8;
    for index in 0..package_count {
        let name = format!("p{index}");
        lab.add(
            "system",
            &name,
            1,
            &format!("usr/lib/torture/{name}"),
            &format!("{name}-v1"),
        )?;
    }
    lab.add("system", "intruder", 1, "usr/lib/torture/p0", "intruder")?;
    lab.add("runtime", "switchable", 1, "bin/switchable", "runtime")?;
    lab.add("toolchain", "switchable", 1, "bin/switchable", "toolchain")?;
    lab.add("system", "dep-a", 1, "usr/lib/torture/dep-a", "dep-a")?;
    lab.add("system", "dep-b", 1, "usr/lib/torture/dep-b", "dep-b")?;
    let mut consumer = PackageSpec::new(
        "system",
        "consumer",
        1,
        "usr/lib/torture/consumer",
        "consumer-v1",
    );
    consumer.dependencies.push("dep-a".into());
    lab.add_package(consumer)?;
    lab.publish()?;
    let mut model = BTreeMap::<String, u32>::new();
    let mut version_two = BTreeSet::new();
    let mut dependency_replaced = false;
    for (step, operation) in sequence.iter().enumerate() {
        let before = lab.snapshot()?;
        match operation {
            RandomOperation::Install(index) => {
                let name = format!("p{index}");
                lab.install(&name, "system").await?;
                model.insert(
                    model_key("system", &name),
                    if version_two.contains(index) { 2 } else { 1 },
                );
                let converged = lab.audit()?;
                lab.install(&name, "system").await?;
                if lab.audit()? != converged {
                    bail!("seed {seed} step {step}: repeated install was not idempotent");
                }
            }
            RandomOperation::Remove(index) => {
                let name = format!("p{index}");
                let key = model_key("system", &name);
                if model.contains_key(&key) {
                    lab.remove(&name, "system").await?;
                    model.remove(&key);
                    let removed = lab.audit()?;
                    if lab.remove(&name, "system").await.is_ok() || lab.snapshot()? != removed {
                        bail!("seed {seed} step {step}: repeated remove changed state");
                    }
                } else if lab.remove(&name, "system").await.is_ok() || lab.snapshot()? != before {
                    bail!("seed {seed} step {step}: absent remove changed state");
                }
            }
            RandomOperation::Conflict => {
                lab.install("p0", "system").await?;
                model.insert(
                    model_key("system", "p0"),
                    if version_two.contains(&0) { 2 } else { 1 },
                );
                let conflict_baseline = lab.audit()?;
                if lab.install("intruder", "system").await.is_ok()
                    || lab.audit()? != conflict_baseline
                {
                    bail!("seed {seed} step {step}: conflict was not atomic");
                }
            }
            RandomOperation::FaultRetry(index) => {
                let name = format!("p{index}");
                let key = model_key("system", &name);
                match model.entry(key) {
                    std::collections::btree_map::Entry::Vacant(entry) => {
                        lab.inject("before-lmdb-write")?;
                        if lab.install(&name, "system").await.is_ok() {
                            bail!("seed {seed} step {step}: injected failure succeeded");
                        }
                        lab.install(&name, "system").await?;
                        entry.insert(if version_two.contains(index) { 2 } else { 1 });
                    }
                    std::collections::btree_map::Entry::Occupied(_) => {
                        lab.install(&name, "system").await?;
                    }
                }
            }
            RandomOperation::Upgrade(index) => {
                let name = format!("p{index}");
                if version_two.insert(*index) {
                    lab.add(
                        "system",
                        &name,
                        2,
                        &format!("usr/lib/torture/{name}"),
                        &format!("{name}-v2"),
                    )?;
                    lab.publish()?;
                }
                lab.upgrade(&name, "system").await?;
                model.insert(model_key("system", &name), 2);
                let converged = lab.audit()?;
                lab.upgrade(&name, "system").await?;
                if lab.audit()? != converged {
                    bail!("seed {seed} step {step}: repeated upgrade changed state");
                }
            }
            RandomOperation::RollbackAttempt(index) => {
                let name = format!("p{index}");
                let key = model_key("system", &name);
                if version_two.contains(index) && model.get(&key) == Some(&2) {
                    let stable = lab.audit()?;
                    lab.remove_archive("system", &name, 2)?;
                    lab.publish()?;
                    lab.upgrade(&name, "system").await?;
                    if lab.audit()? != stable {
                        bail!("seed {seed} step {step}: repository rollback downgraded state");
                    }
                    lab.add(
                        "system",
                        &name,
                        2,
                        &format!("usr/lib/torture/{name}"),
                        &format!("{name}-v2"),
                    )?;
                    lab.publish()?;
                } else {
                    lab.install(&name, "system").await?;
                    model.insert(key, if version_two.contains(index) { 2 } else { 1 });
                }
            }
            RandomOperation::Toggle(channel) => {
                toggle_channel(&mut lab, &mut model, channel).await?;
            }
            RandomOperation::DependencyReplacement if !dependency_replaced => {
                lab.install("consumer", "system").await?;
                model.insert(model_key("system", "consumer"), 1);
                model.insert(model_key("system", "dep-a"), 1);
                let mut replacement = PackageSpec::new(
                    "system",
                    "consumer",
                    2,
                    "usr/lib/torture/consumer",
                    "consumer-v2",
                );
                replacement.dependencies.push("dep-b".into());
                lab.add_package(replacement)?;
                lab.publish()?;
                lab.upgrade("consumer", "system").await?;
                model.insert(model_key("system", "consumer"), 2);
                model.insert(model_key("system", "dep-b"), 1);
                lab.remove("dep-a", "system").await?;
                model.remove(&model_key("system", "dep-a"));
                dependency_replaced = true;
            }
            RandomOperation::DependencyReplacement => {
                lab.install("consumer", "system").await?;
                model.insert(model_key("system", "consumer"), 2);
                model.insert(model_key("system", "dep-b"), 1);
            }
        }
        let state = lab
            .audit()
            .with_context(|| format!("seed {seed} step {step}; reproduction: {:?}", lab.steps()))?;
        let actual = state
            .packages
            .into_iter()
            .map(|(key, version)| {
                let release = version
                    .split('-')
                    .next()
                    .context("installed version has no release value")?
                    .parse::<u32>()?;
                Ok((key, release))
            })
            .collect::<Result<BTreeMap<_, _>>>()?;
        if actual != model {
            bail!("seed {seed} step {step}: model={model:?}, actual={actual:?}");
        }
    }
    Ok(lab.steps)
}

fn model_key(channel: &str, name: &str) -> String {
    format!("main/{channel}:{name}:0")
}

async fn toggle_channel(
    lab: &mut TortureLab,
    model: &mut BTreeMap<String, u32>,
    channel: &str,
) -> Result<()> {
    let key = model_key(channel, "switchable");
    if model.get(&key).is_some() {
        lab.remove("switchable", channel).await?;
        model.remove(&key);
    } else {
        lab.install("switchable", channel).await?;
        model.insert(key, 1);
    }
    Ok(())
}

fn cli(root: &Path, command: Commands) -> Cli {
    Cli {
        verbose: false,
        dry_run: false,
        root: root.into(),
        command,
    }
}

fn channel_target(channel: &str) -> Result<&'static Path> {
    match channel {
        "system" => Ok(Path::new("/")),
        "runtime" => Ok(Path::new("/usr/lib/runtimes/torture")),
        "toolchain" => Ok(Path::new("/opt/channels/torture/1")),
        _ => bail!("unknown test channel {channel}"),
    }
}

fn collect_managed_files(root: &Path) -> Result<BTreeSet<String>> {
    fn visit(root: &Path, directory: &Path, output: &mut BTreeSet<String>) -> Result<()> {
        if !directory.exists() {
            return Ok(());
        }
        let mut entries = fs::read_dir(directory)?.collect::<Result<Vec<_>, _>>()?;
        entries.sort_by_key(fs::DirEntry::file_name);
        for entry in entries {
            let metadata = fs::symlink_metadata(entry.path())?;
            if metadata.is_dir() {
                visit(root, &entry.path(), output)?;
            } else {
                output.insert(
                    entry
                        .path()
                        .strip_prefix(root)
                        .expect("managed walk stays below root")
                        .to_string_lossy()
                        .into_owned(),
                );
            }
        }
        Ok(())
    }
    let mut output = BTreeSet::new();
    for relative in [
        "usr/lib/torture",
        "usr/lib/runtimes/torture",
        "opt/channels/torture",
    ] {
        visit(root, &root.join(relative), &mut output)?;
    }
    Ok(output)
}
