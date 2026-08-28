pub use sage_repo::{ReleaseLocation, ReleaseSource};

async fn sync_channels(root: &Path, selected: Option<&str>, dry_run: bool) -> Result<()> {
    let config_path = under_root(root, Path::new("/etc/sage/channels.toml"));
    let config = sage_repo::ChannelsConfig::load(&config_path)
        .with_context(|| format!("failed to load {}", config_path.display()))?;
    let cache = under_root(root, Path::new("/var/cache/sage/channels"));
    let engine = sage_repo::DownloadEngine::new(&cache)?;
    let mut matched = false;
    for (channel_name, channel) in &config.channels {
        if !channel.enabled {
            continue;
        }
        for (sub_name, subchannel) in &channel.subchannels {
            if !subchannel.enabled {
                continue;
            }
            let alias = subchannel.alias.as_deref().unwrap_or(sub_name);
            let canonical = format!("{channel_name}/{alias}");
            if selected.is_some_and(|value| value != alias && value != canonical) {
                continue;
            }
            matched = true;
            let destination = cache.join(channel_name).join(alias).join("index.mdb");
            let url = sage_repo::subchannel_url(channel, sub_name, subchannel);
            let key = under_root(root, &channel.signing_key);
            if dry_run {
                println!("Would sync {canonical} from {url}");
            } else {
                let changed = engine.sync_index(&url, &key, &destination).await?;
                println!(
                    "{canonical}: {}",
                    if changed { "updated" } else { "current" }
                );
            }
        }
    }
    if selected.is_some() && !matched {
        bail!("selected channel was not found or is disabled");
    }
    Ok(())
}
fn under_root(root: &Path, path: &Path) -> PathBuf {
    root.join(path.strip_prefix("/").unwrap_or(path))
}

fn alternative_declaration_path(key: &sage_core::PackageKey) -> PathBuf {
    let digest = hex::encode(Sha256::digest(key.canonical_id().as_bytes()));
    PathBuf::from("usr/share/sage/alternatives").join(format!("{digest}.toml"))
}

fn sysusers_declaration_path(key: &sage_core::PackageKey) -> PathBuf {
    let digest = hex::encode(Sha256::digest(key.canonical_id().as_bytes()));
    PathBuf::from("usr/share/sage/sysusers").join(format!("{digest}.toml"))
}

fn read_documents(root: &Path, relative: &Path) -> Result<Vec<Vec<u8>>> {
    let directory = under_root(root, relative);
    if !directory.exists() {
        return Ok(Vec::new());
    }
    let mut entries = std::fs::read_dir(directory)?.collect::<Result<Vec<_>, _>>()?;
    entries.sort_by_key(std::fs::DirEntry::file_name);
    entries
        .into_iter()
        .filter(|entry| entry.path().extension().and_then(|value| value.to_str()) == Some("toml"))
        .map(|entry| std::fs::read(entry.path()).map_err(Into::into))
        .collect()
}

fn alternatives_from_documents(documents: &[Vec<u8>]) -> Result<Vec<sage_sys::Alternative>> {
    let mut alternatives = Vec::new();
    for document in documents {
        alternatives.extend(sage_sys::AlternativesDocument::parse(document)?.alternatives());
    }
    Ok(alternatives)
}

fn trigger_documents(root: &Path) -> Result<Vec<Vec<u8>>> {
    sage_sys::TriggerEngine::load_triggers(root)?
        .into_iter()
        .map(|trigger| Ok(toml::to_string(&trigger)?.into_bytes()))
        .collect()
}

/// One-shot sysroot-local crash injection; the next startup exercises recovery.
fn crash_point(root: &Path, stage: &str) -> Result<()> {
    let marker = under_root(root, Path::new("/run/sage/crash-point"));
    if std::fs::read_to_string(&marker)
        .ok()
        .is_some_and(|value| value.trim() == stage)
    {
        std::fs::remove_file(marker)?;
        bail!("injected crash after {stage}");
    }
    Ok(())
}

async fn settle_journals(root: &Path) -> Result<()> {
    let path = under_root(root, Path::new("/var/lib/sage"));
    if !path.exists() {
        return Ok(());
    }
    let database = sage_db::SageDatabase::open(path)?;
    for mut journal in database.pending_journals()? {
        journal.validate()?;
        eprintln!(
            "Recovering unfinished operation {} ({})",
            journal.op_id, journal.stage
        );
        match &journal.action {
            sage_db::JournalAction::Install { architecture, .. } => {
                let architecture = architecture.clone();
                let available = load_available_with_pool(root, Some(&architecture), None)?;
                resume_install(root, &database, &available, &mut journal).await?;
            }
            sage_db::JournalAction::Remove { .. } => {
                resume_remove(root, &database, &mut journal)?;
            }
        }
        eprintln!("Recovered operation {}", journal.op_id);
    }
    Ok(())
}

pub struct AvailablePackages {
    pub universe: sage_solver::PackageUniverse,
    pub releases: BTreeMap<(sage_core::PackageKey, sage_core::Version), ReleaseSource>,
    pub aliases: BTreeMap<String, String>,
}

pub fn load_available_with_pool(
    root: &Path,
    architecture: Option<&str>,
    local_pool: Option<&Path>,
) -> Result<AvailablePackages> {
    let config =
        sage_repo::ChannelsConfig::load(under_root(root, Path::new("/etc/sage/channels.toml")))?;
    let cache = under_root(root, Path::new("/var/cache/sage/channels"));
    let mut universe = sage_solver::PackageUniverse::default();
    let mut releases = BTreeMap::new();
    let mut aliases = BTreeMap::new();
    let mut target_roots = BTreeMap::new();
    for (channel_name, channel) in config.channels {
        if !channel.enabled {
            continue;
        }
        for (sub_name, subchannel) in &channel.subchannels {
            if !subchannel.enabled {
                continue;
            }
            let alias = subchannel.alias.as_deref().unwrap_or(sub_name);
            let canonical = format!("{channel_name}/{alias}");
            let index_path = cache.join(&channel_name).join(alias).join("index.mdb");
            if !index_path.exists() {
                continue;
            }
            aliases.insert(alias.into(), canonical.clone());
            aliases.insert(canonical.clone(), canonical.clone());
            target_roots.insert(canonical.clone(), subchannel.target_root.clone());
            let url = sage_repo::subchannel_url(&channel, sub_name, subchannel);
            for release in sage_repo::RepositoryIndex::open(&index_path)?.all_releases()? {
                if architecture.is_some_and(|wanted| {
                    release.package.arch != wanted
                        && release.package.arch != "any"
                        && release.package.arch != "noarch"
                }) {
                    continue;
                }
                let coordinate = release.coordinate_for_channel(&canonical);
                universe.insert(release.package.for_channel(&canonical));
                releases.insert(
                    (coordinate.key, coordinate.version),
                    ReleaseSource {
                        release,
                        location: ReleaseLocation::Remote(url.clone()),
                        target_root: subchannel.target_root.clone(),
                    },
                );
            }
        }
    }
    if let Some(pool) = local_pool.filter(|pool| pool.exists()) {
        let mut packages: Vec<_> = std::fs::read_dir(pool)?
            .collect::<Result<Vec<_>, _>>()?
            .into_iter()
            .map(|entry| entry.path())
            .filter(|path| {
                path.file_name()
                    .and_then(|name| name.to_str())
                    .is_some_and(|name| name.ends_with(".pkg.tar.zst"))
            })
            .collect();
        packages.sort();
        for path in packages {
            let inspection = sage_archive::inspect_package(&path)?;
            if architecture.is_some_and(|wanted| {
                inspection.manifest.arch != wanted
                    && inspection.manifest.arch != "any"
                    && inspection.manifest.arch != "noarch"
            }) {
                continue;
            }
            let canonical = aliases
                .get(&inspection.manifest.channel)
                .cloned()
                .unwrap_or_else(|| {
                    if inspection.manifest.channel.contains('/') {
                        inspection.manifest.channel.clone()
                    } else {
                        format!("main/{}", inspection.manifest.channel)
                    }
                });
            aliases
                .entry(inspection.manifest.channel.clone())
                .or_insert_with(|| canonical.clone());
            aliases
                .entry(canonical.clone())
                .or_insert_with(|| canonical.clone());
            let package = inspection.manifest.for_channel(&canonical);
            let coordinate = package.coordinate();
            universe.insert(package);
            releases.insert(
                (coordinate.key, coordinate.version),
                ReleaseSource {
                    release: sage_repo::IndexedRelease {
                        package: inspection.manifest,
                        archive: path.file_name().unwrap().to_string_lossy().into_owned(),
                        sha256: String::new(),
                    },
                    location: ReleaseLocation::Local(path),
                    target_root: target_roots
                        .get(&canonical)
                        .cloned()
                        .unwrap_or_else(|| PathBuf::from("/")),
                },
            );
        }
    }
    Ok(AvailablePackages {
        universe,
        releases,
        aliases,
    })
}

fn canonical_channel(available: &AvailablePackages, selected: Option<&str>) -> Result<String> {
    let selected = selected.unwrap_or("system");
    available
        .aliases
        .get(selected)
        .cloned()
        .with_context(|| format!("channel '{selected}' has no synchronized index"))
}

async fn apply_packages(
    root: &Path,
    names: &[String],
    channel: Option<&str>,
    prefer_latest: bool,
    save: bool,
    dry_run: bool,
) -> Result<()> {
    let config_path = under_root(root, Path::new("/etc/sage/system.toml"));
    let mut config = sage_sys::SystemConfig::load(&config_path)?;
    let available = load_available_with_pool(root, Some(&config.system.architecture), None)?;
    let channel = canonical_channel(&available, channel)?;
    let requested: Vec<_> = names
        .iter()
        .map(|name| sage_core::PackageKey::in_channel(&channel, name))
        .collect::<Result<_, _>>()?;
    let db_path = under_root(root, Path::new("/var/lib/sage"));
    let installed = if dry_run {
        sage_db::read_packages(&db_path)?
    } else {
        sage_db::SageDatabase::open(&db_path)?.packages()?
    };
    let solution = if prefer_latest {
        sage_solver::SageSolver::new(&available.universe)
            .prefer_providers(config.provider_preferences("main/system")?)
            .resolve(&requested)?
    } else {
        let locks = installed
            .iter()
            .map(|package| (package.key.clone(), package.version.clone()));
        sage_solver::SageSolver::with_locked(&available.universe, locks)
            .prefer_providers(config.provider_preferences("main/system")?)
            .resolve(&requested)?
    };
    let current: BTreeMap<_, _> = installed
        .iter()
        .map(|package| (package.key.clone(), package.version.clone()))
        .collect();
    let changes = solution
        .into_iter()
        .filter(|(key, version)| current.get(key) != Some(version))
        .collect();
    let changes = installation_order(&available, changes)?;
    for (key, version) in &changes {
        println!(
            "{} {} {}",
            if current.contains_key(key) {
                "Upgrade"
            } else {
                "Install"
            },
            key,
            version
        );
    }
    if dry_run {
        return Ok(());
    }
    if !changes.is_empty() {
        let database = sage_db::SageDatabase::open(&db_path)?;
        publish_packages(
            root,
            &database,
            &available,
            &config.system.architecture,
            &changes,
        )
        .await?;
    }
    if save && channel == "main/system" {
        config.packages.extend(names.iter().cloned());
        write_atomic_under_root(
            root,
            Path::new("etc/sage/system.toml"),
            toml::to_string_pretty(&config)?.as_bytes(),
        )?;
    }
    Ok(())
}

fn installation_order(
    available: &AvailablePackages,
    changes: BTreeMap<sage_core::PackageKey, sage_core::Version>,
) -> Result<Vec<(sage_core::PackageKey, sage_core::Version)>> {
    let mut pending = changes;
    let mut ordered = Vec::with_capacity(pending.len());
    while !pending.is_empty() {
        let ready = pending.iter().find_map(|(key, version)| {
            let package = &available.releases[&(key.clone(), version.clone())]
                .release
                .package;
            let blocked = package.dependencies.iter().any(|dependency| {
                pending.keys().any(|candidate| {
                    if candidate == key {
                        return false;
                    }
                    if dependency.name.starts_with("virtual/") || dependency.name.starts_with("so:")
                    {
                        return pending
                            .get(candidate)
                            .and_then(|candidate_version| {
                                available
                                    .releases
                                    .get(&(candidate.clone(), candidate_version.clone()))
                            })
                            .is_some_and(|source| {
                                source.release.package.provides.contains(&dependency.name)
                            });
                    }
                    candidate.name == dependency.name
                        && candidate.slot
                            == dependency.slot.as_deref().unwrap_or(sage_core::DEFAULT_SLOT)
                        && candidate.channel
                            == dependency.channel.as_deref().unwrap_or(&key.channel)
                })
            });
            (!blocked).then(|| key.clone())
        });
        let Some(key) = ready else {
            bail!("selected package graph contains an installation cycle");
        };
        ordered.push((key.clone(), pending.remove(&key).unwrap()));
    }
    Ok(ordered)
}

async fn publish_packages(
    root: &Path,
    database: &sage_db::SageDatabase,
    available: &AvailablePackages,
    architecture: &str,
    changes: &[(sage_core::PackageKey, sage_core::Version)],
) -> Result<()> {
    let timestamp = unix_timestamp()?;
    let op_id = format!("install-{}-{timestamp}", std::process::id());
    let mut journal = sage_db::JournalRecord::new(
        op_id,
        "packages",
        sage_db::JournalAction::Install {
            architecture: architecture.into(),
            changes: changes.to_vec(),
            modified_paths: Vec::new(),
            previous_alternative_documents: read_documents(
                root,
                Path::new("usr/share/sage/alternatives"),
            )?,
        },
    );
    database.write_journal(&journal)?;
    resume_install(root, database, available, &mut journal).await
}

async fn resume_install(
    root: &Path,
    database: &sage_db::SageDatabase,
    available: &AvailablePackages,
    journal: &mut sage_db::JournalRecord,
) -> Result<()> {
    journal.validate()?;
    let (architecture, changes) = match &journal.action {
        sage_db::JournalAction::Install {
            architecture,
            changes,
            ..
        } => (architecture.clone(), changes.clone()),
        _ => bail!("install recovery received a removal journal"),
    };
    let package_cache = under_root(root, Path::new("/var/cache/sage/packages"));
    let engine = sage_repo::DownloadEngine::new(&package_cache)?;
    let mut modified = Vec::new();
    if journal.stage == "packages" {
    for (key, version) in &changes {
        let source = available
            .releases
            .get(&(key.clone(), version.clone()))
            .with_context(|| format!("index record disappeared for {key} {version}"))?;
        let archive = obtain_release_archive(&engine, &package_cache, source).await?;
        let inspection = sage_archive::inspect_package(&archive)?;
        if inspection.manifest.arch != architecture
            && inspection.manifest.arch != "any"
            && inspection.manifest.arch != "noarch"
        {
            bail!(
                "package {} has architecture {}, expected {}",
                key,
                inspection.manifest.arch,
                architecture
            );
        }
        let target = under_root(root, &source.target_root);
        std::fs::create_dir_all(&target)?;
        let prefix = source
            .target_root
            .strip_prefix("/")
            .unwrap_or(&source.target_root);
        let mut ownership: Vec<_> = inspection
            .files
            .iter()
            .map(|record| prefix.join(&record.path).to_string_lossy().into_owned())
            .collect();
        let services = if let Some(bytes) = inspection.optional.get(".METADATA/service.toml") {
            sage_sys::ServiceDocument::parse(bytes)?
                .into_services()
                .into_iter()
                .map(|service| {
                    let bytes = toml::to_string_pretty(&sage_sys::ServiceDocument {
                        schema_version: sage_core::SCHEMA_VERSION,
                        service: Some(service.clone()),
                        services: Vec::new(),
                    })?
                    .into_bytes();
                    Ok((service, bytes))
                })
                .collect::<Result<Vec<_>>>()?
        } else {
            Vec::new()
        };
        let trigger = if let Some(bytes) = inspection.optional.get(".METADATA/triggers.toml") {
            Some((sage_sys::TriggerSpec::parse(bytes)?, bytes.clone()))
        } else {
            None
        };
        let alternatives =
            if let Some(bytes) = inspection.optional.get(".METADATA/alternatives.toml") {
                let mut document = sage_sys::AlternativesDocument::parse(bytes)?;
                document.package.clone_from(key);
                let bytes = toml::to_string_pretty(&document)?.into_bytes();
                Some((document, bytes))
            } else {
                None
            };
        let sysusers = if let Some(bytes) = inspection.optional.get(".METADATA/sysusers.toml") {
            let mut document = sage_sys::SysusersDocument::parse(bytes)?;
            document.package.clone_from(key);
            let bytes = toml::to_string_pretty(&document)?.into_bytes();
            Some((document, bytes))
        } else {
            None
        };
        for (service, _) in &services {
            ownership.push(format!("usr/share/sage/services/{}.toml", service.name));
        }
        if let Some((trigger, _)) = &trigger {
            ownership.push(format!("usr/share/sage/triggers/{}.toml", trigger.name));
        }
        if alternatives.is_some() {
            ownership.push(
                alternative_declaration_path(key)
                    .to_string_lossy()
                    .into_owned(),
            );
        }
        if sysusers.is_some() {
            ownership.push(
                sysusers_declaration_path(key)
                    .to_string_lossy()
                    .into_owned(),
            );
        }
        for path in &ownership {
            let owners = database.owners(path)?;
            if owners.iter().any(|owner| owner != key) {
                bail!("file conflict for {path}: {owners:?}");
            }
        }
        let previous_package = database.package(key)?;
        let previous = previous_package
            .as_ref()
            .map(|package| package.config_hashes.clone())
            .unwrap_or_default();
        let report = sage_archive::extract_package_with_config(
            &archive,
            &target,
            &inspection.files,
            &previous,
        )?;
        crash_point(root, "extraction")?;
        for (service, bytes) in services {
            write_atomic_under_root(
                root,
                &Path::new("usr/share/sage/services").join(format!("{}.toml", service.name)),
                &bytes,
            )?;
        }
        if let Some((trigger, bytes)) = trigger {
            write_atomic_under_root(
                root,
                &Path::new("usr/share/sage/triggers").join(format!("{}.toml", trigger.name)),
                &bytes,
            )?;
        }
        if let Some((_, bytes)) = alternatives {
            write_atomic_under_root(root, &alternative_declaration_path(key), &bytes)?;
        }
        if let Some((_, bytes)) = sysusers {
            write_atomic_under_root(root, &sysusers_declaration_path(key), &bytes)?;
        }
        modified.extend(ownership.iter().map(PathBuf::from));
        let config_hashes = inspection
            .files
            .iter()
            .filter(|record| record.path.starts_with("etc"))
            .map(|record| {
                (
                    record.path.to_string_lossy().into_owned(),
                    record.sha256.clone(),
                )
            })
            .collect();
        database.install(
            &sage_db::InstalledPackage {
                key: key.clone(),
                version: version.clone(),
                arch: source.release.package.arch.clone(),
                installed_size: source.release.package.installed_size,
                dependencies: source.release.package.dependencies.clone(),
                provides: source.release.package.provides.clone(),
                files: ownership.clone(),
                config_hashes,
            },
            false,
        )?;
        crash_point(root, "lmdb-publication")?;
        if let Some(previous_package) = previous_package {
            for obsolete in previous_package
                .files
                .iter()
                .filter(|path| !ownership.contains(path))
            {
                if database.owners(obsolete)?.is_empty() {
                    let path = under_root(root, Path::new(obsolete));
                    if !should_preserve_config(&path, obsolete, &previous_package.config_hashes)? {
                        remove_file_beneath(root, &path)?;
                        modified.push(PathBuf::from(obsolete));
                    }
                }
            }
        }
        for path in report.sage_new {
            eprintln!("Configuration update requires review: {}", path.display());
        }
    }
    if let sage_db::JournalAction::Install { modified_paths, .. } = &mut journal.action {
        *modified_paths = modified
            .iter()
            .map(|path| path.to_string_lossy().into_owned())
            .collect();
    }
    journal.advance("alternatives");
    database.write_journal(journal)?;
    }
    let modified: Vec<PathBuf> = match &journal.action {
        sage_db::JournalAction::Install { modified_paths, .. } => {
            modified_paths.iter().map(PathBuf::from).collect()
        }
        _ => unreachable!(),
    };
    if journal.stage == "alternatives" {
    let previous_alternatives = alternatives_from_documents(match &journal.action {
        sage_db::JournalAction::Install {
            previous_alternative_documents,
            ..
        } => previous_alternative_documents,
        _ => unreachable!(),
    })?;
    let current_alternatives = sage_sys::AlternativesDocument::load_installed(root)?;
    sage_sys::ProfileEngine::reconcile_alternatives(
        root,
        &previous_alternatives,
        &current_alternatives,
    )?;
    let accounts = sage_sys::SysusersDocument::load_installed(root)?;
    sage_sys::SysusersEngine::reconcile(root, &accounts)?;
    journal.advance("triggers");
    database.write_journal(journal)?;
    crash_point(root, "alternatives")?;
    }
    if journal.stage == "triggers" {
    let triggers = sage_sys::TriggerEngine::load_triggers(root)?;
    crash_point(root, "triggers")?;
    sage_sys::TriggerEngine::execute_triggers_for(
        &triggers,
        &modified,
        root,
        sage_sys::TriggerEvent::PostChange,
    )?;
    }
    database.finish_journal(&journal.op_id)?;
    Ok(())
}

async fn upgrade_packages(
    root: &Path,
    names: &[String],
    channel: Option<&str>,
    dry_run: bool,
) -> Result<()> {
    let config = sage_sys::SystemConfig::load(under_root(
        root,
        Path::new("/etc/sage/system.toml"),
    ))?;
    let available = load_available_with_pool(root, Some(&config.system.architecture), None)?;
    let canonical = canonical_channel(&available, channel)?;
    let names = if names.is_empty() {
        let db_path = under_root(root, Path::new("/var/lib/sage"));
        let packages = if dry_run {
            sage_db::read_packages(&db_path)?
        } else {
            sage_db::SageDatabase::open(&db_path)?.packages()?
        };
        packages
            .into_iter()
            .filter(|package| package.key.channel == canonical)
            .map(|package| format!("{}:{}", package.key.name, package.key.slot))
            .collect()
    } else {
        names.to_vec()
    };
    apply_packages(root, &names, Some(&canonical), true, false, dry_run).await
}

fn remove_packages(
    root: &Path,
    names: &[String],
    channel: Option<&str>,
    update_saved: bool,
    dry_run: bool,
) -> Result<()> {
    let db_path = under_root(root, Path::new("/var/lib/sage"));
    let installed = if dry_run {
        sage_db::read_packages(&db_path)?
    } else {
        sage_db::SageDatabase::open(&db_path)?.packages()?
    };
    let canonical = channel.map_or_else(
        || "main/system".into(),
        |value| {
            if value.contains('/') {
                value.into()
            } else {
                format!("main/{value}")
            }
        },
    );
    let requested = names
        .iter()
        .map(|selector| sage_core::PackageKey::in_channel(&canonical, selector))
        .collect::<Result<Vec<_>, _>>()?;
    let selected: Vec<_> = installed
        .iter()
        .filter(|package| requested.contains(&package.key))
        .cloned()
        .collect();
    if selected.len() != names.len() {
        bail!("one or more requested packages are not installed in {canonical}");
    }
    for dependent in &installed {
        if selected.iter().any(|removed| {
            dependent.key != removed.key
                && dependent.dependencies.iter().any(|dependency| {
                    (dependency.name == removed.key.name
                        || removed.provides.contains(&dependency.name))
                        && dependency
                            .slot
                            .as_deref()
                            .is_none_or(|slot| slot == removed.key.slot)
                        && dependency
                            .channel
                            .as_deref()
                            .is_none_or(|channel| channel == removed.key.channel)
                })
        }) {
            bail!("cannot remove packages required by {}", dependent.key);
        }
    }
    for package in &selected {
        println!("Remove {} {}", package.key, package.version);
    }
    if dry_run {
        return Ok(());
    }
    if update_saved && canonical == "main/system" {
        let config_path = under_root(root, Path::new("/etc/sage/system.toml"));
        let mut config = sage_sys::SystemConfig::load(&config_path)?;
        for name in names {
            config.packages.remove(name);
        }
        write_atomic_under_root(
            root,
            Path::new("etc/sage/system.toml"),
            toml::to_string_pretty(&config)?.as_bytes(),
        )?;
    }
    let database = sage_db::SageDatabase::open(&db_path)?;
    let timestamp = unix_timestamp()?;
    let op_id = format!("remove-{}-{timestamp}", std::process::id());
    let mut journal = sage_db::JournalRecord::new(
        op_id,
        "packages",
        sage_db::JournalAction::Remove {
            packages: selected,
            modified_paths: Vec::new(),
            trigger_documents: trigger_documents(root)?,
            alternative_documents: read_documents(
                root,
                Path::new("usr/share/sage/alternatives"),
            )?,
        },
    );
    database.write_journal(&journal)?;
    resume_remove(root, &database, &mut journal)
}

fn resume_remove(
    root: &Path,
    database: &sage_db::SageDatabase,
    journal: &mut sage_db::JournalRecord,
) -> Result<()> {
    journal.validate()?;
    if journal.stage == "packages" {
    let packages = match &journal.action {
        sage_db::JournalAction::Remove { packages, .. } => packages.clone(),
        _ => bail!("remove recovery received an install journal"),
    };
    let mut modified = Vec::new();
    for package in packages {
        database.remove(&package.key)?;
        crash_point(root, "removal")?;
        for relative in &package.files {
            if !database.owners(relative)?.is_empty() {
                continue;
            }
            let path = under_root(root, Path::new(relative));
            if should_preserve_config(&path, relative, &package.config_hashes)? {
                eprintln!("Preserving modified configuration {}", path.display());
                continue;
            }
            remove_file_beneath(root, &path)?;
            modified.push(PathBuf::from(relative));
        }
    }
    if let sage_db::JournalAction::Remove { modified_paths, .. } = &mut journal.action {
        *modified_paths = modified
            .iter()
            .map(|path| path.to_string_lossy().into_owned())
            .collect();
    }
    journal.advance("alternatives");
    database.write_journal(journal)?;
    }
    let modified: Vec<PathBuf> = match &journal.action {
        sage_db::JournalAction::Remove { modified_paths, .. } => {
            modified_paths.iter().map(PathBuf::from).collect()
        }
        _ => unreachable!(),
    };
    if journal.stage == "alternatives" {
    let previous_alternatives = alternatives_from_documents(match &journal.action {
        sage_db::JournalAction::Remove {
            alternative_documents,
            ..
        } => alternative_documents,
        _ => unreachable!(),
    })?;
    let current_alternatives = sage_sys::AlternativesDocument::load_installed(root)?;
    sage_sys::ProfileEngine::reconcile_alternatives(
        root,
        &previous_alternatives,
        &current_alternatives,
    )?;
    let accounts = sage_sys::SysusersDocument::load_installed(root)?;
    sage_sys::SysusersEngine::reconcile(root, &accounts)?;
    journal.advance("triggers");
    database.write_journal(journal)?;
    crash_point(root, "alternatives")?;
    }
    if journal.stage == "triggers" {
    let triggers = match &journal.action {
        sage_db::JournalAction::Remove {
            trigger_documents, ..
        } => trigger_documents
            .iter()
            .map(|bytes| sage_sys::TriggerSpec::parse(bytes))
            .collect::<Result<Vec<_>, _>>()?,
        _ => unreachable!(),
    };
    crash_point(root, "triggers")?;
    sage_sys::TriggerEngine::execute_triggers_for(
        &triggers,
        &modified,
        root,
        sage_sys::TriggerEvent::PostRemove,
    )?;
    }
    database.finish_journal(&journal.op_id)?;
    Ok(())
}

fn should_preserve_config(
    path: &Path,
    physical: &str,
    hashes: &BTreeMap<String, String>,
) -> Result<bool> {
    let Some((_, expected)) = hashes
        .iter()
        .find(|(relative, _)| Path::new(physical).ends_with(relative))
    else {
        return Ok(false);
    };
    if !path.exists() {
        return Ok(false);
    }
    let bytes = std::fs::read(path)?;
    Ok(hex::encode(Sha256::digest(bytes)) != *expected)
}

fn remove_file_beneath(root: &Path, path: &Path) -> Result<()> {
    let parent = path.parent().context("installed path has no parent")?;
    let canonical_root = std::fs::canonicalize(root)?;
    let canonical_parent = std::fs::canonicalize(parent)?;
    if !canonical_parent.starts_with(canonical_root) {
        bail!(
            "refusing to remove path outside sysroot: {}",
            path.display()
        );
    }
    match std::fs::remove_file(path) {
        Ok(()) => Ok(()),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(()),
        Err(error) => Err(error.into()),
    }
}

fn write_atomic_under_root(root: &Path, relative: &Path, bytes: &[u8]) -> Result<()> {
    if relative.components().any(|component| {
        matches!(
            component,
            std::path::Component::ParentDir | std::path::Component::RootDir
        )
    }) {
        bail!("unsafe state path {}", relative.display());
    }
    let target = root.join(relative);
    let parent = target.parent().context("state path has no parent")?;
    std::fs::create_dir_all(parent)?;
    let canonical_root = std::fs::canonicalize(root)?;
    let canonical_parent = std::fs::canonicalize(parent)?;
    if !canonical_parent.starts_with(canonical_root) {
        bail!("state path escapes sysroot: {}", target.display());
    }
    let temporary = parent.join(format!(".sage-state-{}", std::process::id()));
    let mut options = std::fs::OpenOptions::new();
    use std::io::Write as _;
    options
        .write(true)
        .create_new(true)
        .open(&temporary)?
        .write_all(bytes)?;
    std::fs::rename(temporary, target)?;
    Ok(())
}

async fn rebuild_system(root: &Path, no_prune: bool, dry_run: bool) -> Result<()> {
    let config_path = under_root(root, Path::new("/etc/sage/system.toml"));
    let config = sage_sys::SystemConfig::load(&config_path)?;
    let mut desired: Vec<_> = config.packages.iter().cloned().collect();
    desired.extend(config.providers.values().cloned());
    desired.sort();
    desired.dedup();
    apply_packages(root, &desired, Some("system"), false, false, dry_run).await?;
    let available = load_available_with_pool(root, Some(&config.system.architecture), None)?;
    let db_path = under_root(root, Path::new("/var/lib/sage"));
    let installed = if dry_run {
        sage_db::read_packages(&db_path)?
    } else {
        sage_db::SageDatabase::open(&db_path)?.packages()?
    };
    let plan =
        sage_sys::ReconcilePlan::compute(&config, &installed, &available.universe, no_prune)?;
    let names: Vec<_> = plan
        .remove
        .into_iter()
        .map(|key| format!("{}:{}", key.name, key.slot))
        .collect();
    if !names.is_empty() {
        remove_packages(root, &names, Some("main/system"), false, dry_run)?;
    }
    let database = if dry_run {
        None
    } else {
        Some(sage_db::SageDatabase::open(&db_path)?)
    };
    for (interface, key) in plan.provider_bindings {
        if dry_run {
            println!("Would bind virtual/{interface} to {key}");
        } else {
            database
                .as_ref()
                .expect("non-dry rebuild opens the database")
                .set_system_provider(&interface, &key)?;
        }
    }
    render_services(root, &config, dry_run)?;
    if !dry_run {
        let triggers = sage_sys::TriggerEngine::load_triggers(root)?;
        sage_sys::TriggerEngine::execute_triggers_for(
            &triggers,
            &[PathBuf::from("etc/sage/system.toml")],
            root,
            sage_sys::TriggerEvent::Rebuild,
        )?;
    }
    Ok(())
}

fn render_services(root: &Path, config: &sage_sys::SystemConfig, dry_run: bool) -> Result<()> {
    let provider = config
        .providers
        .get("init")
        .context("system providers must select an init implementation")?;
    let rclass = under_root(
        root,
        &Path::new("/usr/share/sage/rclass").join(format!("init-{provider}.toml")),
    );
    let generator = sage_sys::TemplateServiceGenerator::from_rclass(&rclass)?;
    let service_dir = under_root(root, Path::new("/usr/share/sage/services"));
    let mut installed = Vec::new();
    if service_dir.exists() {
        let mut declarations = std::fs::read_dir(&service_dir)?.collect::<Result<Vec<_>, _>>()?;
        declarations.sort_by_key(std::fs::DirEntry::file_name);
        for declaration in declarations {
            if declaration.path().extension().and_then(|value| value.to_str()) == Some("toml") {
                installed.push(sage_sys::ServiceSpec::load(declaration.path())?);
            }
        }
    }
    let installed_names = installed
        .iter()
        .map(|service| service.name.as_str())
        .collect::<std::collections::BTreeSet<_>>();
    if let Some(service) = config
        .services
        .iter()
        .find(|service| !installed_names.contains(service.as_str()))
    {
        bail!("enabled service '{service}' has no installed declaration");
    }
    let state_relative = Path::new("var/lib/sage/rendered-services.toml");
    let state_path = under_root(root, state_relative);
    let previous = if state_path.exists() {
        Some(sage_sys::RenderedServicesState::load(&state_path)?)
    } else {
        None
    };
    if dry_run {
        if let Some(previous) = &previous {
            for service in &previous.services {
                let provider_changed = previous.provider != *provider;
                let removed = !installed
                    .iter()
                    .any(|candidate| candidate.name == service.name);
                let disabled = previous.enabled.contains(&service.name)
                    && !config.services.contains(&service.name);
                if provider_changed || removed || disabled {
                    println!(
                        "Would disable service {} from init provider {}",
                        service.name, previous.provider
                    );
                }
                if provider_changed || removed {
                    println!(
                        "Would remove stale native definition for service {}",
                        service.name
                    );
                }
            }
        }
        for service in &installed {
            println!("Would render service {} with {}", service.name, rclass.display());
        }
        return Ok(());
    }
    if let Some(previous) = &previous {
        let previous_rclass = under_root(
            root,
            &Path::new("/usr/share/sage/rclass")
                .join(format!("init-{}.toml", previous.provider)),
        );
        let previous_generator =
            sage_sys::TemplateServiceGenerator::from_rclass(&previous_rclass)?;
        for service in &previous.services {
            let provider_changed = previous.provider != *provider;
            let removed = !installed
                .iter()
                .any(|candidate| candidate.name == service.name);
            let disabled = previous.enabled.contains(&service.name)
                && !config.services.contains(&service.name);
            if provider_changed || removed || disabled {
                previous_generator.disable_service(service, root)?;
            }
            if provider_changed || removed {
                previous_generator.remove_service(service, root)?;
            }
        }
    }
    generator.render_service_set(&installed, root)?;
    for service in &installed {
        if config.services.contains(&service.name) {
            generator.enable_service(service, root)?;
        }
    }
    let state = sage_sys::RenderedServicesState {
        schema_version: sage_core::SCHEMA_VERSION,
        provider: provider.clone(),
        services: installed,
        enabled: config.services.clone(),
    };
    write_atomic_under_root(root, state_relative, toml::to_string_pretty(&state)?.as_bytes())?;
    Ok(())
}

fn unix_timestamp() -> Result<u64> {
    Ok(std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)?
        .as_secs())
}

fn list_channels(root: &Path) -> Result<()> {
    let config =
        sage_repo::ChannelsConfig::load(under_root(root, Path::new("/etc/sage/channels.toml")))?;
    for (name, channel) in config.channels {
        println!(
            "{}\t{}\t{}",
            name,
            if channel.enabled {
                "enabled"
            } else {
                "disabled"
            },
            channel.url
        );
        for (sub_name, subchannel) in channel.subchannels {
            println!(
                "  {}/{}\t{}\t{}",
                name,
                subchannel.alias.as_deref().unwrap_or(&sub_name),
                if subchannel.enabled {
                    "enabled"
                } else {
                    "disabled"
                },
                subchannel.target_root.display()
            );
        }
    }
    Ok(())
}

fn use_toolchain(root: &Path, channel: &str, dry_run: bool) -> Result<()> {
    if channel.is_empty()
        || !channel
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'-' | b'_'))
    {
        bail!("invalid toolchain channel '{channel}'");
    }
    let source = under_root(root, &Path::new("/opt/channels").join(channel).join("bin"));
    let mut entries: Vec<_> = std::fs::read_dir(&source)
        .with_context(|| format!("toolchain has no bin directory: {}", source.display()))?
        .collect::<Result<_, _>>()?;
    entries.sort_by_key(|entry| entry.file_name());
    let links: BTreeMap<_, _> = entries
        .into_iter()
        .filter_map(|entry| {
            entry
                .file_type()
                .ok()
                .filter(|kind| kind.is_file() || kind.is_symlink())
                .map(|_| {
                    (
                        PathBuf::from("bin").join(entry.file_name()),
                        Path::new("/opt/channels")
                            .join(channel)
                            .join("bin")
                            .join(entry.file_name()),
                    )
                })
        })
        .collect();
    let system =
        sage_sys::SystemConfig::load(under_root(root, Path::new("/etc/sage/system.toml")))?;
    if dry_run {
        println!(
            "Would activate {} tools from {} in profile {}",
            links.len(),
            channel,
            system.system.profile
        );
    } else {
        sage_sys::ProfileEngine::apply_profile(root, &system.system.profile, &links)?;
        println!("Activated toolchain {channel}");
    }
    Ok(())
}

fn query_state(root: &Path, action: QueryAction) -> Result<()> {
    let db_path = under_root(root, Path::new("/var/lib/sage"));
    match action {
        QueryAction::Installed => {
            for package in sage_db::read_packages(&db_path)? {
                println!("{}\t{}\t{}", package.key, package.version, package.arch);
            }
        }
        QueryAction::Owner { path } => {
            let relative = path.strip_prefix(root).unwrap_or(&path);
            let relative = relative.strip_prefix("/").unwrap_or(relative);
            for owner in sage_db::read_owners(&db_path, &relative.to_string_lossy())? {
                println!("{owner}");
            }
        }
        QueryAction::Info { package, channel } => {
            let channel = if channel.contains('/') {
                channel
            } else {
                format!("main/{channel}")
            };
            let key = sage_core::PackageKey::in_channel(channel, &package)?;
            let record = sage_db::read_packages(&db_path)?
                .into_iter()
                .find(|record| record.key == key)
                .with_context(|| format!("package {key} is not installed"))?;
            println!("Package: {}", record.key);
            println!("Version: {}", record.version);
            println!("Architecture: {}", record.arch);
            println!("Installed size: {}", record.installed_size);
            println!("Files: {}", record.files.len());
            for dependency in record.dependencies {
                println!("Depends: {}", dependency.name);
            }
        }
    }
    Ok(())
}
