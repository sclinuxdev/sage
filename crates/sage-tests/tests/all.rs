//! Integration tests for every Sage production crate.
//!
//! Keeping these tests in one package ensures production targets compile without test modules.

#[path = "cases/archive.rs"]
mod archive_tests;
#[path = "cases/build.rs"]
mod build_tests;
#[path = "cases/core.rs"]
mod core_tests;
#[path = "cases/db.rs"]
mod db_tests;
#[path = "cases/repo.rs"]
mod repo_tests;
#[path = "cases/sage.rs"]
mod sage_tests;
#[path = "cases/solver.rs"]
mod solver_tests;
#[path = "cases/sys.rs"]
mod sys_tests;
