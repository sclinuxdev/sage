# Recipe schema v2

Recipe v2 describes the project; Sage owns the build. It removes arbitrary
phase commands and never records guessed compiler provenance in package
manifests. Recipe v1 build execution remains compatible for existing recipes.

**Scope.** Recipe v2 currently targets **native amd64 builds** (plus `arch =
"any"` payloads). Cross compilation stays a v1 capability; the v2 attestation
fields `target_triplet`/`target_arch` record the native target, and a v2
recipe whose architecture differs from the builder is rejected instead of
being silently cross-built.

Upstream prebuilt archives such as `rust-bin` are repackaging inputs, not Sage
compilations. They must not declare `[build.toolchain]`, and their package
manifest/index contains no compiler, linker, version, or flag claim. Finding a
producer-looking string in an ELF comment/debug section is not proof of the
complete upstream build, so Sage neither scans nor records it. Old inferred
build fields are discarded if such a package is read and serialized again.

## Sage toolchain policy

`/etc/sage/build.toml` is the single source of compiler, linker and flag
selection:

```toml
schema_version = 1
fakeroot = "fakeroot"            # exact command/path; required for recipe execution
sysroot = "/"                    # complete read-only root exposed to v2
cc = "clang"
cxx = "clang++"
linker = "ld.lld"              # lld/ld.lld | mold | ld, or an executable path
fallback_cc = "gcc"
fallback_cxx = "g++"
fallback_linker = "ld"
rustc = "rustc"                 # exact Rust compiler command/path
cflags = "-O3 -march=x86-64-v3"
cxxflags = ""                 # empty mirrors cflags
cppflags = ""
ldflags = "-Wl,--as-needed"
rustflags = "-C target-cpu=x86-64-v3"
source_date_epoch = 0            # fixed timestamp exported to every phase
jobs = 0                         # concurrent package fetch/inspection; 0 = hardware
compile_jobs = 0                 # threads inside one package build; 0 = hardware
compiler_cache = "none"          # none | auto | ccache | sccache
ccache_dir = "/var/cache/sage/ccache"
memory_limit = ""                # cgroup-v2 bytes, or "max"; empty = off
pids_limit = 0                   # cgroup-v2 process ceiling; 0 = off
```

`compile_jobs` controls the exact parallelism Sage supplies to a single build:
`MAKEFLAGS` for Make/Autotools, `CARGO_BUILD_JOBS` for Cargo, `--parallel` for
CMake, and `-j` for Meson/Xmake. If it is omitted, Sage inherits `jobs` for
compatibility with older `build.toml` files. Setting `compile_jobs = 0`
explicitly selects the online hardware-thread count.

`compiler_cache` is resolved once before the build. `none` disables cache
mounts and wrapper injection. Explicit `ccache` and `sccache` require that
executable inside the configured build sysroot; failure is fatal. `auto`
prefers `sccache` for Cargo and `ccache` for other backends, then tries the
other implementation; if neither exists Sage emits a warning and continues
without a cache. C/C++ backends receive compiler-specific cache wrappers
(`CC`/`CXX`, plus CMake launcher settings); Cargo receives `RUSTC_WRAPPER`
when the selected cache is sccache. The wrapper always invokes Sage's audit
compiler, so caching cannot bypass tool provenance.

`memory_limit` is a byte count or `max`, and `pids_limit` is a positive
process count (`0` disables the limit). If either is configured, Sage must
create a cgroups v2 scope, write every requested controller, and move the
audit supervisor into it before running a managed step. Any unavailable
controller, failed control-file write, or failed process move aborts the
build; Sage never silently downgrades a requested limit. `RLIMIT_NPROC` is
also applied for `pids_limit` as a defense in depth, not as a cgroup
replacement.

Every v1 recipe phase and every v2 managed step is
executed as a child of the configured `fakeroot`. Sage probes that exact
executable with `--version` first and aborts the build if it is unavailable;
there is no silent unvirtualized fallback. `fakeroot` is execution machinery,
not compiler provenance, so it is never written to the package's managed
compiler/linker records.

Fakeroot itself does not grant privileges and is not a sandbox or container; it
virtualizes selected file metadata calls through `LD_PRELOAD`. The upstream
fakeroot manual also warns that configure-style system probes can be confused
inside fakeroot; recipes which hit that implementation limitation must fail
visibly rather than bypassing the configured environment.

For v2, `sysroot` is the complete read-only filesystem mounted as `/` inside
bubblewrap. `/` is only the native bootstrap default; a distributor should
point it at a populated package sysroot to prevent host files outside that tree
from entering a build. Sage requires this path to exist and requires the
configured toolchain and build utilities to be present in it.


Cross builds pass `--target <triplet>` to the backend-specific target
interface (`--host` for Autotools, CMake processor, Cargo target, Kbuild
`ARCH`/`CROSS_COMPILE`, `CHOST`, and target pkg-config paths). The package
architecture is derived from that triplet and must be one of `amd64`,
`aarch64`, `riscv64`, or `armv7` (or `any` for architecture-independent
payloads). A cross build without an explicit target triplet is rejected when
the recipe architecture differs from the builder. Every managed v2 attestation
records both `host_arch`/`host_triplet` and `target_arch`/`target_triplet`;
they are never copied from one another.
For v2, a recipe may restrict compatible compiler/linker families and may
declare one package-specific default suite as described below. It never names
executable paths or supplies flags. Sage uses the configured fallback only
when it is the triple matching that declaration; it never changes compiler
after a build has started. The selected linker is exported as `LD`; Sage also
adds the matching compiler-driver option (`-fuse-ld=lld`, `mold`, or `bfd`) to
managed linker/Rust arguments.

That makes `build.toml` the authority for selection, not by itself proof of
execution. A `clang --version` probe proves the identity and minimum version
of the candidate Sage selected; the ptrace/seccomp audit below separately
proves that the same executable actually crossed `execve` during this build.
Both checks are required before v2 provenance is written.

## Common metadata

```toml
schema_version = 2

[package]
name = "example"
version = "1.2.3"
release = "1"
description = "Example package"
license = "MIT"
channel = "system"
check_dependencies = ["pkg-config >= 2.0"]
arch = "amd64"
dependencies = ["zlib >= 1.3"]
build_dependencies = ["cmake", "ninja"]

[upstream]
url = "https://github.com/example/example/tags"
version_regex = 'v(\d+\.\d+\.\d+)'

[source]
url = "https://github.com/example/example/archive/v1.2.3.tar.gz"
sha256 = "..."
```

`[upstream]` implements the metadata requested by
[recipes.amd64 issue #1](https://github.com/sclinuxdev/recipes.amd64/issues/1).
Both fields are optional as a pair. For compatibility with the issue's first
proposal, `package.upstream` and `package.upstream_regex` are also accepted;
new recipes should use the dedicated table.
Additional `[[source]]` entries work as in v1. They are verified and staged in
`src/distfiles/`. In v2 every source URL, including additional sources, must
have a SHA-256. A patch is always identified by one basename; directory
components, absolute paths, and duplicate declarations are rejected.

The canonical declaration is structured and self-contained:

```toml
[build]
system = "cmake"
payload = "all"                 # all | allowlist | outputs (required)
patches = [
    { file = "fix-musl.patch", strip = 1,
      sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef" },
]
```

The compatibility declaration remains accepted:

```toml
patches = ["fix-musl.patch"]

[build.patch_checksums]
fix-musl.patch = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
```

For a string entry, Sage obtains the checksum from `build.patch_checksums`,
or from the unique `[[source]]` whose URL basename is the patch name. A
structured entry must contain `sha256`. `build.patch_checksums` is a
cross-check, not an override: if it exists alongside a structured hash or a
matching source hash, all declarations must be identical (case-insensitive),
otherwise parsing fails. Keys that do not name a patch are errors. A local
patch beside `recipe.toml` or under `distfiles/` is hashed before it is staged;
a remote patch source is verified by its source hash. Every patch therefore
has exactly one effective SHA-256 before the `patch` command runs.
```

## Managed build table

```toml
[build]
system = "cmake"               # autotools | cmake | meson | xmake | cargo | go | make | script
payload = "all"                 # required payload boundary
header_only = false             # exempt pure headers/data from compiler audit; forbids ELF binaries
kernel = false                  # true for Linux Kbuild projects using the Make backend
source_subdir = "src"          # optional, relative to unpacked source root
build_dir = "build"            # cmake/meson build directory
configure_options = []         # project feature choices, never compiler flags
build_targets = []
install_targets = []
install_files = []           # post-install payload allowlist, relative to DESTDIR
install_excludes = []        # optional post-install payload exclusions
install_copies = []          # [{ from = "built/file", to = "usr/bin/file" }]
install_symlinks = []        # [{ path = "usr/bin/sh", target = "bash" }]
install_moves = []           # [{ from = "usr/libexec/foo", to = "usr/bin/foo" }]
install_removes = []         # relative globs removed after install
install_generates = []       # [{ path = "usr/lib/foo.conf", content = "...", mode = 420 }]
outputs = []                 # [{ name = "foo-libs", install_files = [...] }]
patches = []
patch_strip = 1
allowed_compilers = ["clang", "gcc"]
allowed_linkers = ["lld", "mold", "ld"]
tools = false                # script only: opt into the managed C/C++ toolchain
network = false              # opt-in network for the build sandbox (default off)
# [build.flag_policy] and [build.content] are documented in their own sections.
```

### Declarative vendor pre-fetching (`[[vendor]]`)

To support offline, reproducible builds for ecosystems with vendored dependencies (e.g. Go, Cargo, Node.js), recipes can declare `[[vendor]]` archives:

```toml
[[vendor]]
url = "https://example.com/ripgrep-vendor-14.1.0.tar.zst"
sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
target = "vendor"              # optional target subdirectory, defaults to "vendor"
```

Sage pre-fetches and verifies the vendor archive SHA-256 before entering the build sandbox. During build staging, the archive is unpacked into `target` (default `vendor/` in the source root). For Cargo builds, Sage automatically configures offline crates-io source replacement via `.cargo/config.toml`; for Go builds, `-mod=vendor` is enabled automatically.

`file_permissions` entries accept either numeric `uid`/`gid` or symbolic
`user`/`group` names (resolved at build time, mutually exclusive with the
numeric form):

```toml
[[build.file_permissions]]
path = "usr/bin/foo"
mode = 4755
user = "root"
group = "shadow"
```

Recipe v2 rejects `prepare = [...]`, `build = [...]`, and `install = [...]`
shell arrays. `configure_options` and targets are passed as individual quoted
arguments, so they cannot replace the managed build procedure. Sage also
rejects compiler/linker/flag assignments and each backend's toolchain override
options. `[build.variables]` cannot use a Sage-managed name.

`install_targets` names a target understood by the selected build backend; it
does not define the package boundary. The backend first installs into Sage's
private `DESTDIR`, then Sage applies `install_files` and
`install_excludes` against canonical paths below that directory. Globs use the
same shell-style matching as Sage's query commands. Patterns are relative (for
example `usr/bin/example` or `usr/lib/libexample.so.*`) and may not contain
`..`, an absolute path, or the archive metadata prefix `data/`. Every
`install_files` pattern must match at least one file, and an empty result is a
build error. This is the mechanism that keeps `foo`, `foo-libs`, and `foo-dev`
from silently shipping the same upstream install tree. Split package recipes
whose names end in `-libs` or `-dev` must declare a non-empty allowlist; Sage
rejects an unbounded split recipe before running the build.

`payload = "all"` is valid only when both top-level lists and `outputs` are
empty. If a package needs exclusions, use `payload = "allowlist"` and spell
out an include set. A broad `"**"` include is accepted for an intentionally
complete package, but it is not a substitute for reviewing the resulting
payload.

For a normal package that intentionally ships the complete upstream install,
leave both lists empty. For a split package, make the boundary explicit:

```toml
[build]
system = "autotools"
payload = "allowlist"
install_targets = ["install"]
install_files = [
    "usr/lib/libexample.so.*",
    "usr/lib/libexample.so",       # retain a deliberate linker symlink only when needed
]
```

The filter runs before ELF dependency scanning, manifest creation and archive
packing. It never follows symlinks outside the staging root and removes only
the selected package's private staging files.

Autotools recipes may request an out-of-tree build with `build_dir = "build"`.
Sage then configures from that directory with `../configure`, runs the build
and install there, and still owns the canonical `/usr` prefix and `/usr/lib`
library directory. Leaving `build_dir` empty keeps the upstream in-tree layout
for projects whose generated files are expected beside their sources.

When an upstream build does not install a required artifact, v2 can express a
small, deterministic payload transform without reopening arbitrary shell
execution:

```toml
install_copies = [
    { from = "libbz2.so.1.0.8", to = "usr/lib/libbz2.so.1.0.8" },
]
install_symlinks = [
    { path = "usr/lib/libbz2.so.1", target = "libbz2.so.1.0.8" },
]
```

Copy sources are relative to the selected source subdirectory; destinations
are relative to `DESTDIR`. Symlink targets are recorded exactly as declared,
while their link paths remain anchored below the staging root. Missing copy
sources are hard errors, so a recipe cannot silently publish an incomplete
split package.

`install_moves` moves an existing staged artifact, `install_removes` removes
matching staged paths, and `install_generates` writes a deterministic file with
the declared mode. All paths stay relative to the package staging root and are
checked before the operation; existing parent symlinks that resolve outside the
source or package root are rejected, and copy/generate never follow a symlink
at their destination. Operations run in the order copy, move, remove, generate,
symlink, then the payload allowlist.

Several outputs can share one build:

```toml
[build]
system = "autotools"
payload = "outputs"
outputs = [
  { name = "foo-libs", install_files = ["usr/lib/libfoo.so.*"] },
  { name = "foo-dev", install_files = ["usr/include/**", "usr/lib/libfoo.so"] },
]
```

Sage copies the common staging tree before filtering each output and writes one
archive per `name`. Top-level `install_files`/`install_excludes` cannot be
combined with `outputs`, so one output cannot delete another output's files.
Every upstream symlink is checked during extraction and packing: absolute
targets and normalized targets that leave the data root are rejected.

### Arbitrary package logic

The `script` backend is the v2 escape hatch for packages whose build or split
layout cannot be reduced to a backend target and the declarative transforms.
It still has an explicit payload boundary, and every command runs under Sage's
same fakeroot, bubblewrap namespace, clean environment and fixed timestamp:

```toml
[build]
system = "script"
payload = "allowlist"
install_files = ["usr/lib/myapp/**", "usr/share/myapp/**"]

[[build.steps]]
name = "generate-config"
phase = "prepare"                 # prepare | pre-build | post-build | check |
                                   # pre-install | install | post-install
cwd = "source"                    # source | build | package
command = "./configure-local --output build/config"

[[build.steps]]
name = "split-and-fixup"
phase = "install"
cwd = "package"
command = "rm -rf usr/share/doc; mv usr/libexec/myapp usr/lib/myapp"
```
The `check` phase runs after the backend build and `post-build` steps, before
`pre-install`. It runs in the same read-only build sysroot and sandbox as every
other managed step, with the writable source/build/package staging mounts.
`package` contains only whatever a prior step placed there; normal backend
installation has not run yet. A non-zero check command aborts the build, so no
archive or manifest is produced.

`package.check_dependencies` is a build-time root request. Before any source
work, Sage resolves every request against the installed-package database of
`build.sysroot`; the matching package payloads and their `provides` are already
visible in the read-only namespace. Sage does not mutate that sysroot or
install test packages as a side effect of `sage build`. A missing database,
unsatisfied constraint, or missing check phase is a hard error. Check
dependencies never become runtime `package.dependencies`; they are copied into
the v2 build attestation as `attestation.check_dependencies` so the test
environment is auditable.

Steps execute in TOML order within each phase. `source` is the unpacked or
copy-on-write source tree, `build` is the recipe's private build directory,
and `package` is the only directory that becomes payload. The recipe tree and
host root remain read-only; writes outside the staged source/package roots
therefore fail in the namespace. `script` deliberately does not create
compiler/linker provenance. If a script executes a fenced compiler, Rust
compiler or linker, Sage rejects the build; use a managed backend for compiled
packages.

## Package-specific default toolchain

Omit these tables to use Sage's global primary/fallback policy. A package that
requires a particular suite may declare both halves:

```toml
[build.toolchain.compiler]
family = "gcc"                 # clang | gcc
package = "gcc"                # package-manager identity, not an executable
minimum_version = "15.1"

[build.toolchain.linker]
family = "mold"                # lld | mold | ld
package = "mold"
minimum_version = "2.40"
```

Cargo recipes may additionally constrain the Rust compiler Sage configures:

```toml
[build.toolchain.rust]
family = "rustc"
package = "rust"
minimum_version = "1.90"
```

When present, this is a mandatory package default: Sage selects only a
configured triple with these two families and verifies the versions reported
by the actual compiler/linker executables. A missing, mismatched, or too-old
triple fails before any build step. `package` and `minimum_version` also become
derived build dependencies (`gcc >= 15.1`, `mold >= 2.40`); do not repeat them
unless the explicit `build_dependencies` constraint is at least as strong.
The executable names still come exclusively from `/etc/sage/build.toml`.

After a successful managed v2 build, Sage writes only roles whose wrappers
actually executed to `[[managed_build_tools]]`: role, exact configured
executable, detected family, parsed version, execution count, and
`version_argument = "--version"`. A pure-C build therefore need not contain a
synthetic `cxx` entry. C/C++ builds may record `cc`, `cxx`, `linker-driver`, and
`linker`; Cargo records `rustc`, its linker-driver, and (when it links) the
selected linker. These fields are absent from v1 and upstream-prebuilt
repackages. They mean “this Sage-probed executable was observed executing in
this build”, not “inferred as the producer of every payload file”.

The same v2 attestation records `host_arch`, `host_triplet`, `target_arch`,
`target_triplet`, and the raw `check_dependencies` requests. These are
provenance inputs, not runtime dependency edges. The package manifest's
`dependencies` remain the only dependencies consumed by installation.

## CMake

```toml
[build]
system = "cmake"
payload = "all"
configure_options = [
  "-DBUILD_TESTING=OFF",
  "-DENABLE_SHARED=ON",
]
allowed_compilers = ["clang", "gcc"]
allowed_linkers = ["lld", "mold", "ld"]
```

Sage runs CMake with Ninja, `/usr` prefix, the canonical `/usr/lib` library
directory, Release mode, parallel build and a `DESTDIR` install. It passes the
selected C/C++ compiler, linker, and all flag classes as Sage-generated CMake
cache arguments. `install_targets`, when present, names custom CMake build
targets to run under `DESTDIR` instead of the normal `cmake --install` step.

## Meson

```toml
[build]
system = "meson"
payload = "all"
configure_options = [
  "-Dtests=disabled",
  "-Ddefault_library=shared",
]
```

Sage runs `meson setup --prefix=/usr --libdir=lib`, `meson compile`, and
`meson install`; compiler and linker variables are present before setup so
Meson observes one toolchain. Recipes cannot override the prefix or library
directory. Meson recipes leave `install_targets` empty because `meson install`
does not accept target names.

## Xmake

```toml
[build]
system = "xmake"
payload = "all"
configure_options = ["--shared=y"]
build_targets = ["example"]
```

Sage supplies Xmake's `--cc`, `--cxx`, `--ld`, and compiler/linker flag
configuration and installs under the package staging root. Xmake recipes leave
`install_targets` empty.

## Cargo

```toml
[build]
system = "cargo"
payload = "all"
build_targets = ["--features", "system-zlib"]
install_targets = ["--bin", "example"]
allowed_linkers = ["lld", "mold", "ld"]
```

Sage runs locked release build/install commands with Cargo tracking metadata
disabled in the staged root. `RUSTFLAGS`, the Rust linker driver and its
`-fuse-ld` argument all come from Sage's build policy. Cargo has no
`configure_options`; its two target arrays hold Cargo build/install arguments.
`--config`, `--config=...`, `--root`, `--root=...`, `--target-dir`, and
`--target-dir=...` are rejected because they can redirect either toolchain
selection or the install tree.

## Reproducibility and execution evidence

Managed v2 steps run through `fakeroot` and a required bubblewrap namespace in
a clean environment. Sage sets
`LC_ALL=C`, `LANG=C`, `TZ=UTC`, `SOURCE_DATE_EPOCH` (default 0), `umask 022`,
private `HOME`/`TMPDIR`/Cargo directories, and disables global Git
configuration. Caller PATH, proxy, locale and flag variables are not inherited.
Before a backend starts, Sage normalizes source and generated-file mtimes to
`SOURCE_DATE_EPOCH`; local projects are copied out of the read-only recipe
tree into a private writable source root. Package archives are then packed
with the same epoch and stable ordering, so repeated clean builds have a
stable byte-level input boundary.
Compiler and linker wrappers record actual child invocations; common bare tool
names in PATH are fenced, and a `-B` audit prefix makes compiler-driver links
reach the selected linker wrapper. Bubblewrap also masks the canonical paths
of common compiler/linker aliases (including versioned names), so an absolute
path cannot silently select a second toolchain. Every managed step is also
run under a ptrace supervisor with a seccomp filter for `execve` and
`execveat`; the supervisor follows fork/clone descendants and records every
successful executable transition in a temporary `process-exec.log`. The build
fails if the selected executable never reaches a real exec transition or if a
compiler-like executable outside Sage's selected paths is observed. If
bubblewrap, ptrace, or seccomp is unavailable, Sage refuses the v2 build.
The temporary full-process audit is deliberately not copied into the package
manifest because PIDs and scheduling order would make otherwise identical
archives differ; the manifest retains only deterministic tool execution
counts, Sage-supplied flag channels and wrapper command lines. v1 and
upstream-binary repackaging intentionally retain no compiler provenance.

When limits are configured, the ptrace supervisor itself enters the cgroups
v2 scope before the first command. The scope is recreated for the
reproducibility pass, so a second pass cannot accidentally run unbounded.

## Go

```toml
[build]
system = "go"
payload = "all"
build_targets = []          # extra `go build` arguments (optional)
install_targets = []        # package patterns for `go install`; default "./..."
```

Sage runs `go build`/`go install` from the module root with `GOBIN` pointed at
the private staging `usr/bin`, `GOTOOLCHAIN=local` (so `go` never fetches a
different toolchain), and private `GOPATH`/`GOCACHE`/`GOMODCACHE` under the
hermetic HOME. `go -C`, `-mod` and `-overlay` arguments are Sage-managed and
rejected in the target arrays. A recipe may pin the toolchain with
`[build.toolchain.go]` (`family = "go"`); the probed `go` executable and its
observed executions are recorded in `[[managed_build_tools]]` with
`version_argument = "version"`.

**Module dependencies.** By default the build sandbox has no network, so Go
module dependencies must be build inputs: either a committed `vendor/` tree
(Sage sets `GOFLAGS=-mod=vendor` automatically when it exists), or a module
cache staged from `[[source]]` entries by a `prepare` step. `GOPROXY=off`
fails fast instead of hanging on a dead network. A recipe that genuinely
needs to fetch modules during the build declares it:

```toml
[build]
system = "go"
network = true         # opt-in network for the build sandbox

[[build.steps]]
name = "fetch-deps"
phase = "prepare"
cwd = "source"
command = "go mod download"
```

`network = true` is the only way to reach the network from a v2 build; it is
per-recipe and off by default, so the hermetic boundary remains the default
for everything else. It merely drops the sandbox's `--unshare-net` and stops
forcing `GOPROXY=off` — the read-only sysroot, private `/tmp` and the tool
audit fence all stay. It does **not** make a build reproducible by itself: a
networked Go build must still pin every module version in `go.sum` (and
Cargo in `Cargo.lock`) for the reproducibility pass and for reviewer
confidence.

The same vendored-input convention applies to Cargo: crates are `[[source]]`
entries staged into `vendor/` by a prepare step, or a committed vendor tree; a
`network = true` Cargo recipe may instead fetch from crates.io and rely on
`Cargo.lock`.

## Script recipes that need a toolchain

The `script` backend normally forbids compiler execution. A language runtime
or package whose build genuinely needs the managed C/C++ toolchain (for
example a Python package with native extensions) may declare:

```toml
[build]
system = "script"
tools = true
```

Such a recipe is audited exactly like a managed backend: the build.toml
toolchain is selected, fenced and provenance-recorded, and the compiler must
actually execute. Repackaging-only recipes stay `tools`-free.

## Flag downgrade

A recipe may not add flags, but a package that genuinely cannot take the
global policy may name the flag classes Sage must remove from every managed
channel (`CFLAGS`/`CXXFLAGS`/`CPPFLAGS`/`LDFLAGS`/`RUSTFLAGS`, their
`flag_env` aliases, and the Kbuild channels):

```toml
[build.flag_policy]
lto = false        # drop -flto* / -ffat-lto-*
march = false      # drop -march= / -mtune= / -mcpu= (and -C target-cpu=)
as-needed = false  # drop -Wl,--as-needed
```

Only `false` is accepted; the keys declare downgrades, never upgrades. The
effective (filtered) flags are what the plan exports and what the manifest's
tool parameters record.

## Content policy

Deterministic payload post-processing, applied to the staged install tree
after the backend install and the declarative transforms, before ELF scanning
and packing. Every output view inherits the processed tree.

```toml
[build.content]
strip = "unneeded"        # none (default) | unneeded | debug
man_compress = "gzip"     # none (default) | gzip (gzip -n, deterministic)
shebangs = "absolute"     # rewrite `#!/usr/bin/env X` to `#!/usr/bin/X` for
                          # sh, bash, python3, perl, awk; unknown interpreters
                          # are a build error
locales = ["en", "zh_CN"] # prune every other usr/share/locale subtree
```

## System users

A package never runs `useradd` itself. It declares the accounts it needs and
Sage applies them inside the target root at install time (and ships a
sysusers.d fragment in the payload, injected after the payload filter so a
split package cannot drop its own declaration):

```toml
[[sysusers]]
type = "user"
name = "dbus"
id = 81
description = "System D-Bus"
home = "/var/run/dbus"
shell = "/usr/bin/nologin"

[[sysusers]]
type = "group"
name = "dbus"
id = 81
```

`id` is optional (system-allocated). Users may reference a primary `group` by
name; groups are created first. The entries travel on the package manifest
and are re-applied by the post-transaction pass.

## Alternatives

Multiple packages may offer the same symlink path (editors, `cc`, awk).
Sage arbitrates at install/remove time: the link always points at the
installed provider with the highest priority (package name breaks ties), and
a link whose last provider left is removed. The link path must stay free in
the payload -- a package occupying its own alternative path is a build error.

```toml
[[alternatives]]
link = "usr/bin/vi"
target = "vim"
priority = 10
```

## Versioned provides

`provides` entries may carry a version. The version participates in
dependency satisfaction: a request `virtual/libc >= 2.38` is satisfied only by
a provider whose provides entry for `virtual/libc` meets the constraint.

```toml
provides = ["virtual/libc = 2.44", "so:libc.so.6"]
```

Entries must parse as `name` or `name <op> version`; `=` is the canonical
form for a versioned provides.

## Trigger policy

Post-transaction regeneration is split between Sage built-ins and
package-declared `[[triggers]]`:

* **Built-in, always on** (warn+skip when the tool is absent, except
  ldconfig which is required): `ldconfig` (`usr/lib`, `.so`), `ca-certificates`,
  `mime-database`, `glib-schemas` (`usr/share/glib-2.0/schemas`),
  `desktop-database` (`usr/share/applications`), `icon-cache`
  (`usr/share/icons`, hicolor), `font-cache` (`usr/share/fonts`), `initramfs`
  and `bootloader` (capability-driven).
* **Built-in, capability-driven**: `depmod` runs once per touched
  `usr/lib/modules/<version>/` directory and resolves the executable through
  the installed provider of `virtual/depmod` (kmod publishes a
  `[[capability_hooks]]` entry for it). Kernel module compression is a
  packaging choice in the recipe's install steps, not a trigger concern.
* **Recipe-declared**: anything project-specific, via `[[triggers]]`
  (`on_paths`/`on_capability`, fixed `exec` or `run_capability`).

Recipes therefore never declare the standard caches; declaring them again is
redundant, and forgetting them is harmless because Sage infers them from the
transaction's touched paths.

## Make and non-standard projects

`make` is the structured adapter for projects that do not use a supported
configure frontend. The recipe may declare targets and project variables;
compiler/linker flags remain owned by Sage.

```toml
[build]
system = "make"
payload = "all"
build_targets = ["all"]
install_targets = ["install"]

[build.variables]
INSTALL_ROOT = "{destdir}"
PROJECT_PREFIX = "{prefix}"
```

Available placeholders are `{prefix}`, `{destdir}`, `{srcdir}`, and
`{builddir}`.

Some build systems use non-standard names for standard flag classes. The
recipe declares only the channel names; Sage supplies their values:

```toml
[build.flag_env]
cflags = ["PROJECT_CFLAGS"]
cxxflags = ["PROJECT_CXXFLAGS"]
cppflags = ["PROJECT_CPPFLAGS"]
ldflags = ["PROJECT_LDFLAGS"]
rustflags = ["PROJECT_RUSTFLAGS"]
```

This mechanism is not kernel-specific. A Linux kernel recipe is one user:

```toml
schema_version = 2

[package]
name = "linux"
version = "6.18.1"
release = "1"
license = "GPL-2.0-only"
channel = "system"
arch = "amd64"

[upstream]
url = "https://www.kernel.org/releases.json"
version_regex = '"version"\s*:\s*"(\d+\.\d+(?:\.\d+)?)"'

[source]
url = "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.18.1.tar.xz"
sha256 = "..."

[build]
system = "make"
payload = "allowlist"
kernel = true
build_targets = ["all"]
install_targets = ["modules_install", "install"]

[build.variables]
INSTALL_MOD_PATH = "{destdir}"
INSTALL_PATH = "{destdir}/boot"

[build.tool_env]
cc = ["HOSTCC"]
cxx = ["HOSTCXX"]
linker = ["HOSTLD"]
```

With `kernel = true`, Sage recognizes the Make project as Linux Kbuild. It
derives `LLVM = "1"` only when the Sage-selected and probed C compiler is
Clang; GCC builds leave `LLVM` unset. The global build configuration is mapped
to Kbuild's native channels automatically: `cflags` → `KCFLAGS`, `cppflags` →
`KCPPFLAGS`, `ldflags` → `KBUILD_LDFLAGS`, and `rustflags` → `KRUSTFLAGS`.
Therefore a kernel recipe should not name a compiler/linker, force `LLVM`, or
repeat these mappings. The same `flag_env` facility remains available for
non-kernel projects with custom flag variable names. Tool aliases work the
same way:

```toml
[build.tool_env]
cc = ["HOSTCC"]
cxx = ["HOSTCXX"]
linker = ["HOSTLD"]
```

For GNU Make adapters Sage passes the standard variables and every declared
alias on the Make command line, so Makefile defaults cannot override the
policy. `PREFIX` and `DESTDIR` are already standard Sage-managed channels and
must not be repeated under `[build.variables]`. These facilities convey no
tool or flag values and create no manifest claims; they only map Sage's
managed values into documented project inputs.
