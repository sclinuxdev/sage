//! High-level service management lifecycle, transactions, and state queries.

use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::path::Path;

use super::drift::ServiceStatusInfo;
use super::generator::TemplateServiceGenerator;
use super::spec::{RenderedServicesState, ServiceDocument, ServiceSpec, ServicesConfig};
use crate::SysError;
use crate::recovery::{crash_point, operation_id};

/// Loads all available service specifications from `/usr/share/sage/services/`
/// and the active rendered services state.
pub fn load_available_services(root: &Path) -> Result<Vec<ServiceSpec>, SysError> {
    let mut services_map = BTreeMap::new();
    let rendered_path = root.join("var/lib/sage/rendered-services.toml");
    if rendered_path.exists() {
        let rendered = RenderedServicesState::load(&rendered_path).map_err(|error| {
            SysError::Invalid(format!(
                "invalid rendered services state {}: {error}",
                rendered_path.display()
            ))
        })?;
        for svc in rendered.services {
            services_map.insert(svc.name.clone(), svc);
        }
    }
    let services_dir = root.join("usr/share/sage/services");
    if services_dir.exists() {
        let mut installed_names = BTreeSet::new();
        for entry in fs::read_dir(&services_dir)? {
            let path = entry?.path();
            if path.extension().is_some_and(|ext| ext == "toml") {
                let bytes = fs::read(&path).map_err(|error| {
                    SysError::Invalid(format!(
                        "could not read service document {}: {error}",
                        path.display()
                    ))
                })?;
                let doc = ServiceDocument::parse(&bytes).map_err(|error| {
                    SysError::Invalid(format!(
                        "invalid service document {}: {error}",
                        path.display()
                    ))
                })?;
                for svc in doc.into_services() {
                    if !installed_names.insert(svc.name.clone()) {
                        return Err(SysError::Invalid(format!(
                            "duplicate installed service {}",
                            svc.name
                        )));
                    }
                    services_map.entry(svc.name.clone()).or_insert(svc);
                }
            }
        }
    }
    Ok(services_map.into_values().collect())
}

/// Loads the active init provider name and generator.
pub fn load_active_generator(root: &Path) -> Result<(String, TemplateServiceGenerator), SysError> {
    let rendered_path = root.join("var/lib/sage/rendered-services.toml");
    if rendered_path.exists() {
        let rendered = RenderedServicesState::load(&rendered_path)?;
        return Ok((rendered.provider.name, rendered.generator));
    }
    let system_config_path = root.join("etc/sage/system.toml");
    if !system_config_path.is_file() {
        return Err(SysError::Invalid(
            "no active init provider is known; run sage rebuild first".into(),
        ));
    }
    let config = crate::state::SystemConfig::load(&system_config_path)?;
    let provider_name = config
        .provider_preferences("main/system")?
        .remove("virtual/init")
        .map(|key| key.name)
        .ok_or_else(|| {
            SysError::Invalid("no active init provider is known; run sage rebuild first".into())
        })?;
    let rclass_path = root.join(format!("usr/share/sage/rclass/init-{provider_name}.toml"));
    if rclass_path.is_file() {
        let generator = TemplateServiceGenerator::from_rclass(&rclass_path)?;
        return Ok((provider_name, generator));
    }
    Err(SysError::Invalid(format!(
        "could not find init generator for provider '{provider_name}'"
    )))
}

fn optional_file_bytes(path: &Path) -> Result<Option<Vec<u8>>, SysError> {
    match fs::read(path) {
        Ok(bytes) => Ok(Some(bytes)),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(None),
        Err(error) => Err(error.into()),
    }
}

fn lifecycle_mutations(
    root: &Path,
    config: &ServicesConfig,
    spec: &ServiceSpec,
    enabled: bool,
) -> Result<Vec<sage_db::FileMutation>, SysError> {
    let config_path = root.join("etc/sage/services.toml");
    let config_previous = optional_file_bytes(&config_path)?;
    let config_next = toml::to_string_pretty(config)
        .map_err(|error| SysError::Invalid(error.to_string()))?
        .into_bytes();
    let mut mutations = Vec::new();
    if config_previous.as_deref() != Some(config_next.as_slice()) {
        mutations.push(sage_db::FileMutation {
            path: "etc/sage/services.toml".into(),
            previous: config_previous,
            next: Some(config_next),
        });
    }

    let rendered_path = root.join("var/lib/sage/rendered-services.toml");
    if !rendered_path.exists() {
        return Ok(mutations);
    }
    let rendered_previous = fs::read(&rendered_path)?;
    let mut rendered = RenderedServicesState::load(&rendered_path)?;
    if enabled {
        rendered.enabled.insert(spec.name.clone());
        if !rendered
            .services
            .iter()
            .any(|service| service.name == spec.name)
        {
            rendered.services.push(spec.clone());
        }
    } else {
        rendered.enabled.remove(&spec.name);
    }
    let rendered_next = toml::to_string_pretty(&rendered)
        .map_err(|error| SysError::Invalid(error.to_string()))?
        .into_bytes();
    if rendered_previous != rendered_next {
        mutations.push(sage_db::FileMutation {
            path: "var/lib/sage/rendered-services.toml".into(),
            previous: Some(rendered_previous),
            next: Some(rendered_next),
        });
    }
    Ok(mutations)
}

fn apply_lifecycle_mutation(root: &Path, mutation: &sage_db::FileMutation) -> Result<(), SysError> {
    if !matches!(
        mutation.path.as_str(),
        "etc/sage/services.toml" | "var/lib/sage/rendered-services.toml"
    ) {
        return Err(SysError::Invalid(format!(
            "invalid service lifecycle path {}",
            mutation.path
        )));
    }
    let target = root.join(&mutation.path);
    let current = optional_file_bytes(&target)?;
    if current == mutation.next {
        return Ok(());
    }
    if current != mutation.previous {
        return Err(SysError::Invalid(format!(
            "service lifecycle file {} changed outside Sage; refusing to overwrite",
            mutation.path
        )));
    }
    match &mutation.next {
        Some(bytes) => {
            crate::transaction::write_atomic_under_root(root, Path::new(&mutation.path), bytes)
                .map_err(|error| SysError::Invalid(error.to_string()))
        }
        None => crate::recovery::remove_file_beneath(root, &target)
            .map_err(|error| SysError::Invalid(error.to_string())),
    }
}

/// Resumes a journaled service transition through provider and declaration stages.
pub(crate) fn resume_service_lifecycle(
    root: &Path,
    database: &sage_db::SageDatabase,
    journal: &mut sage_db::JournalRecord,
) -> Result<(), SysError> {
    journal.validate()?;
    let (service, generator, provider_action, mutations) = match &journal.action {
        sage_db::JournalAction::ServiceLifecycle {
            service,
            generator,
            provider_action,
            mutations,
        } => (
            service.clone(),
            generator.clone(),
            *provider_action,
            mutations.clone(),
        ),
        _ => {
            return Err(SysError::Invalid(
                "service recovery received a non-service journal".into(),
            ));
        }
    };
    let service_text = std::str::from_utf8(&service)
        .map_err(|error| SysError::Invalid(format!("journaled service is not UTF-8: {error}")))?;
    let generator_text = std::str::from_utf8(&generator)
        .map_err(|error| SysError::Invalid(format!("journaled generator is not UTF-8: {error}")))?;
    let service: ServiceSpec = toml::from_str(service_text)?;
    let generator: TemplateServiceGenerator = toml::from_str(generator_text)?;
    service.validate()?;

    if journal.stage == "provider" {
        match provider_action {
            sage_db::ServiceProviderAction::Enable => {
                generator.render_service(&service, root)?;
                if generator.is_service_enabled(&service, root)? != Some(true) {
                    generator.enable_service(&service, root)?;
                }
            }
            sage_db::ServiceProviderAction::Disable => {
                // When the init provider explicitly reports that the service is disabled
                // (exit status 1), the provider disable action can be skipped safely.
                // If the provider query is not available (None) or reports enabled (Some(true)),
                // execute the retry-safe disable command. Any query error leaves the journal
                // pending so that we never publish a managed-disabled declaration while the
                // service may still remain active.
                match generator.is_service_enabled(&service, root)? {
                    Some(false) => {}
                    Some(true) | None => {
                        generator.disable_service(&service, root)?;
                    }
                }
            }
            sage_db::ServiceProviderAction::Adopt => {
                match generator.is_service_enabled(&service, root)? {
                    Some(true) => {}
                    Some(false) => {
                        return Err(SysError::Invalid(
                            "service is not externally enabled".into(),
                        ));
                    }
                    None => {
                        return Err(SysError::Invalid("cannot determine service state".into()));
                    }
                }
                generator.render_service(&service, root)?;
            }
        }
        journal.advance("declaration");
        database.write_journal(journal)?;
        crash_point(root, "service-provider")
            .map_err(|error| SysError::Invalid(error.to_string()))?;
    }
    if journal.stage == "declaration" {
        for mutation in &mutations {
            apply_lifecycle_mutation(root, mutation)?;
        }
        journal.advance("complete");
        database.write_journal(journal)?;
        crash_point(root, "service-declaration")
            .map_err(|error| SysError::Invalid(error.to_string()))?;
    }
    if journal.stage != "complete" {
        return Err(SysError::Invalid(format!(
            "invalid service lifecycle stage {}",
            journal.stage
        )));
    }
    database.finish_journal(&journal.op_id)?;
    Ok(())
}

fn execute_service_lifecycle(
    root: &Path,
    service: &ServiceSpec,
    generator: &TemplateServiceGenerator,
    provider_action: sage_db::ServiceProviderAction,
    mutations: Vec<sage_db::FileMutation>,
) -> Result<(), SysError> {
    let service = toml::to_string(service)
        .map_err(|error| SysError::Invalid(error.to_string()))?
        .into_bytes();
    let generator = toml::to_string(generator)
        .map_err(|error| SysError::Invalid(error.to_string()))?
        .into_bytes();
    let database = sage_db::SageDatabase::open(root.join("var/lib/sage"))?;
    let mut journal = sage_db::JournalRecord::new(
        operation_id("service").map_err(|error| SysError::Invalid(error.to_string()))?,
        "provider",
        sage_db::JournalAction::ServiceLifecycle {
            service,
            generator,
            provider_action,
            mutations,
        },
    );
    database.write_journal(&journal)?;
    resume_service_lifecycle(root, &database, &mut journal)
}

/// Enables a service in `/etc/sage/services.toml` and activates it in the init provider.
///
/// If `dry_run` is set to `true`, validates generator compatibility and executes the
/// provider's read-only state query (`is_enabled_cmd`), including for definitions
/// that have not been rendered yet. This surfaces query failures (such as non-zero
/// error exit codes) during preview without modifying declarations or persisting journals.
///
/// When `dry_run` is `false`, persists a lifecycle journal, renders the definition,
/// activates the service in the provider if needed, and publishes declarative mutations.
pub fn service_enable(root: &Path, service_name: &str, dry_run: bool) -> Result<(), SysError> {
    let services = load_available_services(root)?;
    let spec = services
        .into_iter()
        .find(|s| s.name == service_name)
        .ok_or_else(|| {
            SysError::Invalid(format!(
                "service '{service_name}' not found in installed packages"
            ))
        })?;
    let config_path = root.join("etc/sage/services.toml");
    let mut config = ServicesConfig::load(&config_path)?;
    if config.enabled.contains(service_name) {
        println!("Service '{service_name}' is already enabled in services.toml");
    }
    let (_provider, generator) = load_active_generator(root)?;
    generator.validate_service_set(std::slice::from_ref(&spec), root)?;
    if dry_run {
        let _ = generator.is_service_enabled(&spec, root)?;
    } else {
        config.enabled.insert(service_name.to_string());
        config.disabled.remove(service_name);
        let mutations = lifecycle_mutations(root, &config, &spec, true)?;
        execute_service_lifecycle(
            root,
            &spec,
            &generator,
            sage_db::ServiceProviderAction::Enable,
            mutations,
        )?;
    }
    println!("Enabled service '{service_name}' (managed-enabled)");
    Ok(())
}

/// Disables a service in `/etc/sage/services.toml` and deactivates it in the init provider.
pub fn service_disable(root: &Path, service_name: &str, dry_run: bool) -> Result<(), SysError> {
    let services = load_available_services(root)?;
    let spec = services
        .into_iter()
        .find(|service| service.name == service_name)
        .ok_or_else(|| {
            SysError::Invalid(format!(
                "service '{service_name}' not found in installed packages"
            ))
        })?;
    let config_path = root.join("etc/sage/services.toml");
    let mut config = ServicesConfig::load(&config_path)?;
    if !config.enabled.contains(service_name) && config.disabled.contains(service_name) {
        println!("Service '{service_name}' was not enabled in services.toml");
    }
    let (_provider, generator) = load_active_generator(root)?;
    generator.validate_service_set(std::slice::from_ref(&spec), root)?;
    if dry_run {
        let _ = generator.is_service_enabled(&spec, root)?;
    } else {
        config.enabled.remove(service_name);
        config.disabled.insert(service_name.to_string());
        let mutations = lifecycle_mutations(root, &config, &spec, false)?;
        execute_service_lifecycle(
            root,
            &spec,
            &generator,
            sage_db::ServiceProviderAction::Disable,
            mutations,
        )?;
    }
    println!("Disabled service '{service_name}' (managed-disabled)");
    Ok(())
}

/// Adopts an externally enabled service into `/etc/sage/services.toml` without modifying host state.
pub fn service_adopt(root: &Path, service_name: &str, dry_run: bool) -> Result<(), SysError> {
    let services = load_available_services(root)?;
    let spec = services
        .into_iter()
        .find(|s| s.name == service_name)
        .ok_or_else(|| {
            SysError::Invalid(format!(
                "service '{service_name}' not found in installed packages"
            ))
        })?;
    let config_path = root.join("etc/sage/services.toml");
    let mut config = ServicesConfig::load(&config_path)?;
    if config.enabled.contains(service_name) {
        println!("Service '{service_name}' is already managed-enabled in services.toml");
        return Ok(());
    }
    let (_provider, generator) = load_active_generator(root)?;
    generator.validate_service_set(std::slice::from_ref(&spec), root)?;
    match generator.is_service_enabled(&spec, root)? {
        Some(true) => {}
        Some(false) => {
            return Err(SysError::Invalid(
                "service is not externally enabled".into(),
            ));
        }
        None => return Err(SysError::Invalid("cannot determine service state".into())),
    }
    if !dry_run {
        config.enabled.insert(service_name.to_string());
        config.disabled.remove(service_name);
        let mutations = lifecycle_mutations(root, &config, &spec, true)?;
        execute_service_lifecycle(
            root,
            &spec,
            &generator,
            sage_db::ServiceProviderAction::Adopt,
            mutations,
        )?;
    }
    println!("Adopted service '{service_name}' into Sage declarative management (managed-enabled)");
    Ok(())
}

/// Lists all known services and their management lifecycle status.
pub fn list_services(root: &Path) -> Result<Vec<ServiceStatusInfo>, SysError> {
    let services = load_available_services(root)?;
    let config_path = root.join("etc/sage/services.toml");
    let config = ServicesConfig::load(&config_path)?;
    let rendered_path = root.join("var/lib/sage/rendered-services.toml");
    let previous_rendered = if rendered_path.exists() {
        RenderedServicesState::load(&rendered_path).ok()
    } else {
        None
    };
    let active_gen = load_active_generator(root).ok();
    let default_provider = "unknown".to_string();
    let provider_name = active_gen
        .as_ref()
        .map(|(name, _)| name.as_str())
        .unwrap_or(&default_provider);

    let mut result = Vec::new();
    for service in services {
        let is_init_enabled = active_gen.as_ref().and_then(|(_, generator)| {
            let rendered = generator.rendered_path(&service, root).ok()?;
            if rendered.is_file() {
                generator.is_service_enabled(&service, root).ok().flatten()
            } else {
                None
            }
        });
        let managed_disabled = config.disabled.contains(&service.name)
            || previous_rendered
                .as_ref()
                .is_some_and(|rendered| rendered.enabled.contains(&service.name));
        let state = if config.enabled.contains(&service.name) {
            "managed-enabled".to_string()
        } else if managed_disabled {
            if is_init_enabled == Some(true) {
                "managed-disabled (drift)".to_string()
            } else {
                "managed-disabled".to_string()
            }
        } else if is_init_enabled == Some(true) {
            "unmanaged (drift)".to_string()
        } else {
            "unmanaged".to_string()
        };

        result.push(ServiceStatusInfo {
            name: service.name,
            package: if service.package.is_empty() {
                "system".to_string()
            } else {
                service.package
            },
            state,
            provider: provider_name.to_string(),
        });
    }
    Ok(result)
}
