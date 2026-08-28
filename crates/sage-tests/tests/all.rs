//! Integration tests for every Sage production crate.
//!
//! Keeping these tests in one package ensures production targets compile without test modules.

include!("cases/archive.rs");
include!("cases/build.rs");
include!("cases/core.rs");
include!("cases/db.rs");
include!("cases/repo.rs");
include!("cases/solver.rs");
include!("cases/sys.rs");
include!("cases/sage.rs");
