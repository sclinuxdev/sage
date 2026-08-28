# Specification: source graph rebuild and bootstrap (`build pipeline` v1)

## 1. Mass rebuild

`sage mass-rebuild <recipe-root> [--output <pool>] [--jobs N]` recursively
discovers `recipe.toml` files. Main packages, subpackages, and provide symbols are
producers; runtime, build, target, default-feature, and inherited rclass
dependencies become edges.

Sage rejects duplicate package producers and cycles, then applies Kahn's
algorithm to produce deterministic maximal parallel layers. Package concurrency
is bounded by `--jobs`; the global CPU budget is divided between active builds.
Artifacts from a completed chunk are atomically moved into an initially empty
local pool. The next layer inserts that pool into the ordinary PubGrub universe
and locks satisfying local releases ahead of repository binaries. A rebuild
therefore consumes preceding source outputs rather than merely ordering commands.

`--dry-run` prints the complete layer plan without creating the output pool.

## 2. Bootstrap stages

Compiler, libc, and language runtime graphs can contain genuine self-hosting
cycles. `sage bootstrap <bootstrap.toml>` provides explicit seed boundaries:

```toml
schema_version = 1

[[stages]]
name = "seed-toolchain"
recipes = ["recipes/compiler-seed/recipe.toml"]

[[stages]]
name = "system-runtime"
recipes = ["recipes/libc/recipe.toml", "recipes/binutils/recipe.toml"]

[[stages]]
name = "self-host"
recipes = ["recipes/compiler/recipe.toml"]
```

Paths are relative to the plan. Stage names must be unique and recipe lists must
be non-empty. Each stage is topologically scheduled internally, while prior-stage
artifacts remain locked in the transient package pool. Rebuilding a package in a
later stage atomically supersedes its seed artifact. The pool must be empty at
pipeline start, preventing stale binaries from silently entering a bootstrap.
