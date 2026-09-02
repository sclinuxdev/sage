/// Desired system state from `/etc/sage/system.toml`.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SystemConfig {
    pub schema_version: u32,
    pub system: SystemMetadata,
    #[serde(default)]
    pub providers: BTreeMap<String, String>,
    #[serde(default)]
    pub packages: BTreeSet<String>,
    #[serde(default)]
    pub services: BTreeSet<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SystemMetadata {
    pub architecture: String,
    pub profile: String,
}

impl SystemConfig {
    pub fn load(path: impl AsRef<Path>) -> Result<Self, SysError> {
        let config: Self = toml::from_str(&fs::read_to_string(path)?)?;
        validate_schema(config.schema_version)?;
        Ok(config)
    }

    /// Expands declarative `name[:slot]` roots into exact solver identities.
    pub fn package_keys(&self, channel: &str) -> Result<Vec<sage_core::PackageKey>, SysError> {
        self.packages
            .iter()
            .map(|selector| {
                sage_core::PackageKey::in_channel(channel, selector)
                    .map_err(|error| SysError::Invalid(error.to_string()))
            })
            .collect()
    }

    /// Returns virtual symbols and their configured concrete package choices.
    pub fn provider_preferences(
        &self,
        channel: &str,
    ) -> Result<BTreeMap<String, sage_core::PackageKey>, SysError> {
        self.providers
            .iter()
            .map(|(interface, selector)| {
                let symbol = if interface.starts_with("virtual/") || interface.starts_with("so:") {
                    interface.clone()
                } else {
                    format!("virtual/{interface}")
                };
                sage_core::PackageKey::in_channel(channel, selector)
                    .map(|key| (symbol, key))
                    .map_err(|error| SysError::Invalid(error.to_string()))
            })
            .collect()
    }
}

/// Minimal transaction needed to converge installed state on the declaration.
#[derive(Debug, Default, PartialEq, Eq)]
pub struct ReconcilePlan {
    pub install: Vec<(sage_core::PackageKey, sage_core::Version)>,
    pub remove: Vec<sage_core::PackageKey>,
    pub provider_bindings: BTreeMap<String, sage_core::PackageKey>,
    pub services: BTreeSet<String>,
}

impl ReconcilePlan {
    /// Solves desired roots, then computes deterministic install/remove differences.
    pub fn compute(
        config: &SystemConfig,
        installed: &[sage_db::InstalledPackage],
        universe: &sage_solver::PackageUniverse,
        no_prune: bool,
    ) -> Result<Self, SysError> {
        let retained = installed
            .iter()
            .filter(|package| no_prune || package.key.channel != "main/system")
            .collect::<Vec<_>>();
        let retained_universe = (!retained.is_empty()).then(|| {
            let mut universe = universe.clone();
            for package in &retained {
                if universe.release(&package.key, &package.version).is_none() {
                    let mut release = sage_core::Package::from_release(
                        package.key.clone(),
                        package.version.clone(),
                        package.dependencies.clone(),
                        package.provides.clone(),
                    );
                    release.conflicts.clone_from(&package.conflicts);
                    universe.insert(release);
                }
            }
            universe
        });
        let universe = retained_universe.as_ref().unwrap_or(universe);
        let mut roots = config.package_keys("main/system")?;
        let preferences = config.provider_preferences("main/system")?;
        if !retained.is_empty() {
            roots.extend(retained.iter().map(|package| package.key.clone()));
            roots.sort();
            roots.dedup();
        }
        let locks = installed
            .iter()
            .map(|package| (package.key.clone(), package.version.clone()))
            .collect::<Vec<_>>();
        let exact = retained
            .iter()
            .map(|package| (package.key.clone(), package.version.clone()))
            .collect::<BTreeMap<_, _>>();
        let resolve = |roots: &[sage_core::PackageKey]| {
            sage_solver::SageSolver::with_locked(universe, locks.clone())
                .prefer_providers(preferences.clone())
                .resolve_with_provider_bindings(roots, &exact)
        };
        let (mut solution, mut selected_providers) = resolve(&roots)?;
        let missing = preferences
            .iter()
            .filter(|(symbol, _)| !selected_providers.contains_key(*symbol))
            .map(|(_, key)| key.clone())
            .collect::<Vec<_>>();
        if !missing.is_empty() {
            roots.extend(missing);
            roots.sort();
            roots.dedup();
            (solution, selected_providers) = resolve(&roots)?;
        }
        let current: BTreeMap<_, _> = installed
            .iter()
            .map(|package| (package.key.clone(), package.version.clone()))
            .collect();
        let install = solution
            .iter()
            .filter(|(key, version)| current.get(*key) != Some(*version))
            .map(|(key, version)| (key.clone(), version.clone()))
            .collect();
        let remove = if no_prune {
            Vec::new()
        } else {
            current
                .keys()
                .filter(|key| key.channel == "main/system" && !solution.contains_key(*key))
                .cloned()
                .collect()
        };
        let provider_bindings = preferences
            .iter()
            .map(|(symbol, preferred)| {
                let selected = selected_providers.get(symbol).unwrap_or(preferred);
                let release = solution
                    .get(selected)
                    .and_then(|version| universe.release(selected, version));
                if !release.is_some_and(|package| package.provides.contains(symbol)) {
                    return Err(SysError::Invalid(format!(
                        "configured provider {selected} does not provide {symbol}"
                    )));
                }
                Ok((
                    symbol.strip_prefix("virtual/").unwrap_or(symbol).into(),
                    selected.clone(),
                ))
            })
            .collect::<Result<_, _>>()?;
        Ok(Self {
            install,
            remove,
            provider_bindings,
            services: config.services.clone(),
        })
    }
}

/// One candidate for a declarative alternatives link.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Alternative {
    pub package: sage_core::PackageKey,
    pub link: PathBuf,
    pub target: PathBuf,
    pub priority: i32,
}

/// Recipe/archive representation without an installed package identity.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct AlternativeDeclaration {
    pub link: PathBuf,
    pub target: PathBuf,
    pub priority: i32,
}

/// Installed alternatives declaration. The package key is bound at publish
/// time so candidates from different channels and slots remain independent.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct AlternativesDocument {
    pub schema_version: u32,
    pub package: sage_core::PackageKey,
    pub alternatives: Vec<AlternativeDeclaration>,
}

impl AlternativesDocument {
    pub fn parse(bytes: &[u8]) -> Result<Self, SysError> {
        let text = std::str::from_utf8(bytes)
            .map_err(|_| SysError::Invalid("alternatives document is not UTF-8".into()))?;
        let document: Self = toml::from_str(text)?;
        validate_schema(document.schema_version)?;
        if document.alternatives.is_empty() {
            return Err(SysError::Invalid(
                "alternatives document must contain at least one declaration".into(),
            ));
        }
        for alternative in &document.alternatives {
            validate_alternative_path(&alternative.link, false)?;
            validate_alternative_path(&alternative.target, true)?;
        }
        Ok(document)
    }

    pub fn alternatives(&self) -> Vec<Alternative> {
        self.alternatives
            .iter()
            .map(|declaration| Alternative {
                package: self.package.clone(),
                link: declaration.link.clone(),
                target: declaration.target.clone(),
                priority: declaration.priority,
            })
            .collect()
    }

    pub fn load_installed(sysroot: &Path) -> Result<Vec<Alternative>, SysError> {
        let directory = sysroot.join("usr/share/sage/alternatives");
        if !directory.exists() {
            return Ok(Vec::new());
        }
        let mut entries: Vec<_> = fs::read_dir(directory)?.collect::<Result<_, _>>()?;
        entries.sort_by_key(|entry| entry.file_name());
        let mut alternatives = Vec::new();
        for entry in entries {
            if entry.path().extension().and_then(|value| value.to_str()) != Some("toml") {
                continue;
            }
            alternatives.extend(Self::parse(&fs::read(entry.path())?)?.alternatives());
        }
        Ok(alternatives)
    }
}

/// One init-independent system account owned by an installed package.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct SysuserDeclaration {
    #[serde(rename = "type")]
    pub kind: String,
    pub name: String,
    pub id: Option<u32>,
    #[serde(default)]
    pub description: String,
    pub home: String,
    pub shell: String,
}

/// Installed Sage account declarations bound to one exact package identity.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct SysusersDocument {
    pub schema_version: u32,
    pub package: sage_core::PackageKey,
    pub accounts: Vec<SysuserDeclaration>,
}

impl SysusersDocument {
    pub fn parse(bytes: &[u8]) -> Result<Self, SysError> {
        let text = std::str::from_utf8(bytes)
            .map_err(|_| SysError::Invalid("sysusers document is not UTF-8".into()))?;
        let document: Self = toml::from_str(text)?;
        validate_schema(document.schema_version)?;
        if document.accounts.is_empty() {
            return Err(SysError::Invalid(
                "sysusers document must contain at least one declaration".into(),
            ));
        }
        for account in &document.accounts {
            account.validate()?;
        }
        Ok(document)
    }

    pub fn load_installed(sysroot: &Path) -> Result<Vec<SysuserDeclaration>, SysError> {
        let directory = sysroot.join("usr/share/sage/sysusers");
        if !directory.exists() {
            return Ok(Vec::new());
        }
        let mut entries: Vec<_> = fs::read_dir(directory)?.collect::<Result<_, _>>()?;
        entries.sort_by_key(|entry| entry.file_name());
        let mut accounts = Vec::new();
        for entry in entries {
            if entry.path().extension().and_then(|value| value.to_str()) != Some("toml") {
                continue;
            }
            accounts.extend(Self::parse(&fs::read(entry.path())?)?.accounts);
        }
        Ok(accounts)
    }
}

impl SysuserDeclaration {
    fn validate(&self) -> Result<(), SysError> {
        if !matches!(self.kind.as_str(), "user" | "group")
            || !valid_declaration_name(&self.name)
            || self.description.contains(['\n', '\r', '\0', ':'])
            || !Path::new(&self.home).is_absolute()
            || !Path::new(&self.shell).is_absolute()
            || [&self.home, &self.shell]
                .iter()
                .any(|value| value.contains(['\n', '\r', '\0', ':']))
        {
            return Err(SysError::Invalid(format!(
                "invalid system account {}",
                self.name
            )));
        }
        Ok(())
    }
}

/// Applies Sage-owned account declarations without depending on an init suite.
pub struct SysusersEngine;

impl SysusersEngine {
    pub fn reconcile(sysroot: &Path, declarations: &[SysuserDeclaration]) -> Result<(), SysError> {
        let mut declarations = declarations.to_vec();
        declarations.sort_by_key(|account| (account.kind.clone(), account.name.clone()));
        let mut unique = BTreeMap::new();
        for account in declarations {
            account.validate()?;
            if let Some(previous) = unique.insert(
                (account.kind.clone(), account.name.clone()),
                account.clone(),
            ) {
                if previous != account {
                    return Err(SysError::Invalid(format!(
                        "conflicting declarations for system account {}",
                        account.name
                    )));
                }
            }
        }

        let etc = target_path(sysroot, Path::new("etc"))?;
        ensure_directory_beneath(sysroot, &etc)?;
        let passwd_path = etc.join("passwd");
        let group_path = etc.join("group");
        let shadow_path = etc.join("shadow");
        let mut passwd = read_account_file(&passwd_path)?;
        let mut group = read_account_file(&group_path)?;
        let mut shadow = read_account_file(&shadow_path)?;
        let mut users = parse_account_ids(&passwd, "passwd")?;
        let mut groups = parse_account_ids(&group, "group")?;

        for account in unique.values().filter(|account| account.kind == "group") {
            ensure_group(account, &mut group, &mut groups)?;
        }
        for account in unique.values().filter(|account| account.kind == "user") {
            let uid = ensure_user_group(account, &mut group, &mut groups)?;
            ensure_user(account, uid, &mut passwd, &mut shadow, &mut users)?;
        }

        write_account_file(&passwd_path, &passwd, 0o644)?;
        write_account_file(&group_path, &group, 0o644)?;
        write_account_file(&shadow_path, &shadow, 0o600)?;
        Ok(())
    }
}

fn read_account_file(path: &Path) -> Result<String, SysError> {
    match fs::read_to_string(path) {
        Ok(mut content) => {
            if !content.is_empty() && !content.ends_with('\n') {
                content.push('\n');
            }
            Ok(content)
        }
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(String::new()),
        Err(error) => Err(error.into()),
    }
}

fn parse_account_ids(content: &str, kind: &str) -> Result<BTreeMap<String, u32>, SysError> {
    let mut result = BTreeMap::new();
    let id_index = 2;
    for line in content.lines().filter(|line| !line.is_empty()) {
        let fields: Vec<_> = line.split(':').collect();
        let name = fields.first().copied().unwrap_or_default();
        let id = fields
            .get(id_index)
            .ok_or_else(|| SysError::Invalid(format!("malformed /etc/{kind} entry")))?
            .parse::<u32>()
            .map_err(|_| SysError::Invalid(format!("invalid /etc/{kind} ID for {name}")))?;
        if result.insert(name.into(), id).is_some() {
            return Err(SysError::Invalid(format!(
                "duplicate /etc/{kind} entry for {name}"
            )));
        }
    }
    Ok(result)
}

fn ensure_group(
    account: &SysuserDeclaration,
    content: &mut String,
    groups: &mut BTreeMap<String, u32>,
) -> Result<u32, SysError> {
    if let Some(existing) = groups.get(&account.name).copied() {
        if account.id.is_some_and(|id| id != existing) {
            return Err(SysError::Invalid(format!(
                "group {} already has ID {existing}",
                account.name
            )));
        }
        return Ok(existing);
    }
    let id = allocate_account_id(account.id, groups, "group", &account.name)?;
    content.push_str(&format!("{}:x:{id}:\n", account.name));
    groups.insert(account.name.clone(), id);
    Ok(id)
}

fn ensure_user_group(
    account: &SysuserDeclaration,
    content: &mut String,
    groups: &mut BTreeMap<String, u32>,
) -> Result<u32, SysError> {
    let group = SysuserDeclaration {
        kind: "group".into(),
        name: account.name.clone(),
        id: account.id,
        description: String::new(),
        home: "/".into(),
        shell: "/usr/bin/nologin".into(),
    };
    ensure_group(&group, content, groups)
}

fn ensure_user(
    account: &SysuserDeclaration,
    primary_group: u32,
    passwd: &mut String,
    shadow: &mut String,
    users: &mut BTreeMap<String, u32>,
) -> Result<(), SysError> {
    if let Some(existing) = users.get(&account.name).copied() {
        if account.id.is_some_and(|id| id != existing) {
            return Err(SysError::Invalid(format!(
                "user {} already has ID {existing}",
                account.name
            )));
        }
        return Ok(());
    }
    let uid = allocate_account_id(account.id, users, "user", &account.name)?;
    passwd.push_str(&format!(
        "{}:x:{uid}:{primary_group}:{}:{}:{}\n",
        account.name, account.description, account.home, account.shell
    ));
    shadow.push_str(&format!("{}:!*:::::::\n", account.name));
    users.insert(account.name.clone(), uid);
    Ok(())
}

fn allocate_account_id(
    requested: Option<u32>,
    accounts: &BTreeMap<String, u32>,
    kind: &str,
    name: &str,
) -> Result<u32, SysError> {
    if let Some(id) = requested {
        if let Some(owner) = accounts
            .iter()
            .find_map(|(owner, existing)| (*existing == id).then_some(owner))
        {
            return Err(SysError::Invalid(format!(
                "{kind} ID {id} for {name} is already owned by {owner}"
            )));
        }
        return Ok(id);
    }
    (61184..=65519)
        .find(|candidate| !accounts.values().any(|existing| existing == candidate))
        .ok_or_else(|| SysError::Invalid(format!("no dynamic {kind} IDs remain")))
}

fn write_account_file(path: &Path, content: &str, mode: u32) -> Result<(), SysError> {
    let parent = path
        .parent()
        .ok_or_else(|| SysError::Invalid("account file has no parent".into()))?;
    let temporary = parent.join(format!(
        ".sage-account-{}-{}",
        std::process::id(),
        TEMP_ID.fetch_add(1, Ordering::Relaxed)
    ));
    let mut options = fs::OpenOptions::new();
    options.write(true).create_new(true);
    std::io::Write::write_all(&mut options.open(&temporary)?, content.as_bytes())?;
    fs::set_permissions(&temporary, fs::Permissions::from_mode(mode))?;
    fs::rename(temporary, path)?;
    Ok(())
}

fn validate_alternative_path(path: &Path, target: bool) -> Result<(), SysError> {
    if path.as_os_str().is_empty()
        || path.is_absolute()
        || path.components().any(|component| {
            matches!(
                component,
                Component::ParentDir | Component::RootDir | Component::Prefix(_)
            )
        })
    {
        return Err(SysError::Invalid(format!(
            "unsafe alternative {} {}",
            if target { "target" } else { "link" },
            path.display()
        )));
    }
    Ok(())
}

pub struct ProfileEngine;

impl ProfileEngine {
    /// Selects the highest priority candidate for each link and publishes symlinks atomically.
    pub fn apply_alternatives(
        sysroot: &Path,
        alternatives: &[Alternative],
    ) -> Result<BTreeMap<PathBuf, PathBuf>, SysError> {
        let mut selected: BTreeMap<PathBuf, &Alternative> = BTreeMap::new();
        for candidate in alternatives {
            selected
                .entry(candidate.link.clone())
                .and_modify(|current| {
                    if (candidate.priority, &candidate.package)
                        > (current.priority, &current.package)
                    {
                        *current = candidate;
                    }
                })
                .or_insert(candidate);
        }
        let mut active = BTreeMap::new();
        for (link, candidate) in selected {
            atomic_symlink(sysroot, &link, &candidate.target)?;
            active.insert(link, candidate.target.clone());
        }
        Ok(active)
    }

    /// Reconciles a complete before/after candidate set, including removal of
    /// links whose final provider disappeared. A stale link is removed only
    /// when it still points to Sage's previously selected target.
    pub fn reconcile_alternatives(
        sysroot: &Path,
        previous: &[Alternative],
        current: &[Alternative],
    ) -> Result<BTreeMap<PathBuf, PathBuf>, SysError> {
        let previous = selected_alternatives(previous);
        let active = Self::apply_alternatives(sysroot, current)?;
        for (link, old_target) in previous {
            if active.contains_key(&link) {
                continue;
            }
            let path = target_path(sysroot, &link)?;
            match fs::read_link(&path) {
                Ok(target) if target == old_target => fs::remove_file(path)?,
                Ok(_) => {}
                Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
                Err(error) => return Err(error.into()),
            }
        }
        Ok(active)
    }

    /// Atomically refreshes an active profile from a complete link map.
    pub fn apply_profile(
        sysroot: &Path,
        profile: &str,
        links: &BTreeMap<PathBuf, PathBuf>,
    ) -> Result<(), SysError> {
        if profile.is_empty()
            || !profile
                .bytes()
                .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_'))
        {
            return Err(SysError::Invalid(format!(
                "invalid profile name '{profile}'"
            )));
        }
        let base = PathBuf::from("etc/sage/profiles").join(profile);
        for (link, target) in links {
            atomic_symlink(sysroot, &base.join(link), target)?;
        }
        Ok(())
    }
}

fn selected_alternatives(alternatives: &[Alternative]) -> BTreeMap<PathBuf, PathBuf> {
    let mut selected: BTreeMap<PathBuf, &Alternative> = BTreeMap::new();
    for candidate in alternatives {
        selected
            .entry(candidate.link.clone())
            .and_modify(|current| {
                if (candidate.priority, &candidate.package) > (current.priority, &current.package) {
                    *current = candidate;
                }
            })
            .or_insert(candidate);
    }
    selected
        .into_iter()
        .map(|(link, candidate)| (link, candidate.target.clone()))
        .collect()
}

fn atomic_symlink(sysroot: &Path, declared: &Path, target: &Path) -> Result<(), SysError> {
    if target
        .components()
        .any(|component| component == Component::ParentDir)
    {
        return Err(SysError::Invalid(format!(
            "unsafe symlink target {}",
            target.display()
        )));
    }
    let link = target_path(sysroot, declared)?;
    let parent = link
        .parent()
        .ok_or_else(|| SysError::Invalid("symlink has no parent".into()))?;
    ensure_directory_beneath(sysroot, parent)?;
    let temporary = parent.join(format!(
        ".sage-link-{}-{}",
        std::process::id(),
        TEMP_ID.fetch_add(1, Ordering::Relaxed)
    ));
    std::os::unix::fs::symlink(target, &temporary)?;
    fs::rename(temporary, link)?;
    Ok(())
}
