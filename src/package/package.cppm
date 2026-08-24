export module sage.package;

// Package domain model, partitioned by responsibility:
//   :version   epoch-ver-rel ordering algebra + release parsing
//   :deps      dependency constraints and satisfaction
//   :trigger   capability hooks + transaction triggers (+ TOML I/O)
//   :manifest  installed-package manifest + file entries + identity
//   :recipe    recipe.toml build配方
// The primary interface re-exports every partition under one module name.

export import :version;
export import :deps;
export import :trigger;
export import :manifest;
export import :recipe;
