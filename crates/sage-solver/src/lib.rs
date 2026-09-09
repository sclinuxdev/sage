//! Slot-aware PubGrub dependency resolution with channel inheritance.

pub mod adapter;
pub mod error;
pub mod universe;

pub use adapter::SageSolver;
pub use error::SolverError;
pub use universe::{PackageRelease, PackageUniverse, Solution};
