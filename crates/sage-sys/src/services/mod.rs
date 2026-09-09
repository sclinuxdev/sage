//! Declarative service management, templating, drift tracking, and lifecycle operations.

pub mod drift;
pub mod generator;
pub mod manager;
pub mod spec;

pub use drift::{ServiceDrift, ServiceStatusInfo, detect_service_drift, warn_service_drift};
pub use generator::TemplateServiceGenerator;
pub(crate) use generator::{ensure_directory_beneath, ensure_existing_beneath, target_path};
pub(crate) use manager::resume_service_lifecycle;
pub use manager::{
    list_services, load_active_generator, load_available_services, service_adopt, service_disable,
    service_enable,
};
pub(crate) use spec::valid_declaration_name;
pub use spec::{RenderedServicesState, ServiceDocument, ServiceSpec, ServicesConfig};
