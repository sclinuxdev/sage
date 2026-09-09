//! Detection and reporting of service enablement drift between Sage and the init system.

use std::collections::BTreeSet;
use std::path::Path;

use serde::{Deserialize, Serialize};

use super::generator::TemplateServiceGenerator;
use super::spec::ServiceSpec;

/// Detected drift between actual init state and Sage declarative management.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ServiceDrift {
    pub service: String,
    pub provider: String,
    pub init_state: String,
    pub sage_state: String,
}

/// Information about a known service's management status.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ServiceStatusInfo {
    pub name: String,
    pub package: String,
    pub state: String,
    pub provider: String,
}

/// Scans installed services for external enablement that conflicts with Sage's
/// declaration. Managed-disabled services are reported separately so an
/// administrator's manual enable is visible without being reverted.
pub fn detect_service_drift(
    root: &Path,
    generator: &TemplateServiceGenerator,
    provider_name: &str,
    services: &[ServiceSpec],
    enabled: &BTreeSet<String>,
    disabled: &BTreeSet<String>,
) -> Vec<ServiceDrift> {
    let mut drifts = Vec::new();
    for service in services {
        if enabled.contains(&service.name) {
            continue;
        }
        if let Ok(Some(true)) = generator.is_service_enabled(service, root) {
            let sage_state = if disabled.contains(&service.name) {
                "managed-disabled"
            } else {
                "unmanaged"
            };
            drifts.push(ServiceDrift {
                service: service.name.clone(),
                provider: provider_name.to_string(),
                init_state: "enabled".to_string(),
                sage_state: sage_state.to_string(),
            });
        }
    }
    drifts
}

/// Emits human-friendly warning and action hint when drift is detected.
pub fn warn_service_drift(
    drifts: &[ServiceDrift],
    generator: Option<&TemplateServiceGenerator>,
    root: &Path,
) {
    for drift in drifts {
        println!(
            "warning: service '{}' is enabled outside Sage",
            drift.service
        );
        println!("         {}: {}", drift.provider, drift.init_state);
        println!("         services.toml: {}", drift.sage_state);
        println!("Hint:");
        println!("  sage service adopt {}", drift.service);
        println!("  no automatic provider state change was made");
        if let Some(generator_inst) = generator
            && let Some(cmd) = &generator_inst.disable_command
        {
            let hint = cmd
                .replace("${service.name}", &drift.service)
                .replace("${SYSROOT}", &root.display().to_string());
            println!("  {hint}");
        }
    }
}
