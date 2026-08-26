# Recipe schema v2

Recipe v2 describes the project; Sage owns the build. It removes arbitrary
phase commands and never records guessed compiler provenance in package
manifests. Recipe v1 build execution remains compatible for existing recipes.

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
jobs = 0                       # concurrent package fetch/inspection; 0 = hardware
compile_jobs = 0               # threads inside one package build; 0 = hardware
```

`compile_jobs` controls the exact parallelism Sage supplies to a single build:
`MAKEFLAGS` for Make/Autotools, `CARGO_BUILD_JOBS` for Cargo, `--parallel` for
CMake, and `-j` for Meson/Xmake. If it is omitted, Sage inherits `jobs` for
compatibility with older `build.toml` files. Setting `compile_jobs = 0`
explicitly selects the online hardware-thread count.

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
`src/distfiles/`. Structured patches name files from that directory:

In v2 every source URL, including additional sources, must have a SHA-256.

```toml
[build]
system = "cmake"
patches = ["fix-musl.patch"]
patch_strip = 1
```

## Managed build table

```toml
[build]
system = "cmake"               # autotools | cmake | meson | xmake | cargo | make | script
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

For a normal package that intentionally ships the complete upstream install,
leave both lists empty. For a split package, make the boundary explicit:

```toml
[build]
system = "autotools"
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
checked before the operation. Operations run in the order copy, move, remove,
generate, symlink, then the payload allowlist.

Several outputs can share one build:

```toml
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
install_files = ["usr/lib/myapp/**", "usr/share/myapp/**"]

[[build.steps]]
name = "generate-config"
phase = "prepare"                 # prepare | pre-build | post-build |
                                   # pre-install | install | post-install
cwd = "source"                    # source | build | package
command = "./configure-local --output build/config"

[[build.steps]]
name = "split-and-fixup"
phase = "install"
cwd = "package"
command = "rm -rf usr/share/doc; mv usr/libexec/myapp usr/lib/myapp"
```

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

## CMake

```toml
[build]
system = "cmake"
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

## Make and non-standard projects

`make` is the structured adapter for projects that do not use a supported
configure frontend. The recipe may declare targets and project variables;
compiler/linker flags remain owned by Sage.

```toml
[build]
system = "make"
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

[upstream]
url = "https://www.kernel.org/releases.json"
version_regex = '"version"\s*:\s*"(\d+\.\d+(?:\.\d+)?)"'

[source]
url = "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.18.1.tar.xz"
sha256 = "..."

[build]
system = "make"
build_targets = ["all"]
install_targets = ["modules_install", "install"]
allowed_compilers = ["clang", "gcc"]
allowed_linkers = ["lld", "mold", "ld"]

[build.variables]
INSTALL_MOD_PATH = "{destdir}"
INSTALL_PATH = "{destdir}/boot"

[build.flag_env]
cflags = ["KCFLAGS"]
cppflags = ["KCPPFLAGS"]
ldflags = ["KBUILD_LDFLAGS"]

[build.tool_env]
cc = ["HOSTCC"]
cxx = ["HOSTCXX"]
linker = ["HOSTLD"]
```

The same `flag_env` facility covers any project with custom flag variable
names. Tool aliases work the same way:

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
