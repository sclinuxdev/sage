# Specification: source graph rebuild and bootstrap (`build pipeline` v1)

## 1. Mass rebuild

`sage mass-rebuild <recipe-root> [--output <pool>] [--jobs N]` recursively
discovers `recipe.toml` files. Main packages, subpackages, and provide symbols are
producers; runtime, build, target, default-feature, and inherited rclass
dependencies become edges.

Sage rejects duplicate package producers and cycles, then applies Kahn's
algorithm to produce deterministic maximal parallel layers. Package concurrency
is bounded by `--jobs`; the global CPU budget is divided between active builds.
Artifacts from a completed chunk are atomically moved into the local package pool.
The next layer inserts that pool into the ordinary PubGrub universe and locks
satisfying local releases ahead of repository binaries. A rebuild therefore consumes
preceding source outputs rather than merely ordering commands.

Mass rebuild enforces `resume_existing = false`: it unconditionally schedules and
rebuilds every discovered unit, overwriting any pre-existing output archive with
the same versioned filename in the target pool. This guarantees that source
tree updates and global configuration changes are fully incorporated.

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
artifacts remain locked in the transient package pool.

Unlike mass-rebuild, `bootstrap` runs with `resume_existing = true`: units whose
expected versioned package archives already exist in the target pool are skipped,
allowing lengthy multi-stage bootstraps to resume safely after interruptions.
Rebuilding a package in a later stage (such as self-hosting compilers replacing
seed compilers) explicitly builds and supersedes the earlier seed artifact in the pool.

## 3. Layer scheduling and symbol resolution

- **Channel-Scoped Provided Symbols**: Virtual `provides` symbols are scoped by
  channel identity (`{ channel, name }`), preventing virtual symbols from conflicting
  across different channels.
- **Multi-Producer Fault Tolerance**: The scheduler indexes all candidate producers
  per symbol across layers. A provided symbol is only marked blocked if *all* candidate
  producers fail; if at least one candidate succeeds, dependent downstream units
  proceed normally.
- **Fatal Task Failure**: Any asynchronous worker task panic or cancellation is
  treated as a fatal error, aborting the pipeline immediately rather than scheduling
  downstream units with indeterminate dependencies.

