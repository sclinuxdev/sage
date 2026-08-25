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
cc = "clang"
cxx = "clang++"
linker = "lld"                 # lld | mold | ld, or an executable path
fallback_cc = "gcc"
fallback_cxx = "g++"
fallback_linker = "ld"
rustc = "rustc"                 # exact Rust compiler command/path
cflags = "-O3 -march=x86-64-v3"
cxxflags = ""                 # empty mirrors cflags
cppflags = ""
ldflags = "-Wl,--as-needed"
rustflags = "-C target-cpu=x86-64-v3"
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

This does not grant privileges and is not a sandbox or container. It
virtualizes selected file metadata calls through `LD_PRELOAD`. The upstream
fakeroot manual also warns that configure-style system probes can be confused
inside fakeroot; recipes which hit that implementation limitation must fail
visibly rather than bypassing the configured environment.

For v2, a recipe may restrict compatible compiler/linker families and may
declare one package-specific default suite as described below. It never names
executable paths or supplies flags. Sage uses the configured fallback only
when it is the triple matching that declaration; it never changes compiler
after a build has started. The selected linker is exported as `LD`; Sage also
adds the matching compiler-driver option (`-fuse-ld=lld`, `mold`, or `bfd`) to
managed linker/Rust arguments.

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
system = "cmake"               # autotools | cmake | meson | xmake | cargo | make
source_subdir = "src"          # optional, relative to unpacked source root
build_dir = "build"            # cmake/meson build directory
configure_options = []         # project feature choices, never compiler flags
build_targets = []
install_targets = []
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

After a successful managed v2 build, Sage writes only its direct observations
to the package manifest as `[[managed_build_tools]]`: role, exact configured
executable, detected family, parsed version, and `version_argument = "--version"`.
C/C++ builds record `cc`, `cxx`, and `linker`; Cargo also records
`rustc`. These fields are absent from v1 and upstream-prebuilt repackages. They
mean “configured and probed by Sage”, not “inferred as the producer of every
payload file”.

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

Sage runs CMake with Ninja, `/usr` prefix, Release mode, parallel build and a
`DESTDIR` install. It passes the selected C/C++ compiler, linker, and all flag
classes as Sage-generated CMake cache arguments. `install_targets`, when
present, names custom CMake build targets to run under `DESTDIR` instead of
the normal `cmake --install` step.

## Meson

```toml
[build]
system = "meson"
configure_options = [
  "-Dtests=disabled",
  "-Ddefault_library=shared",
]
```

Sage runs `meson setup`, `meson compile`, and `meson install`; compiler and
linker variables are present before setup so Meson observes one toolchain.
Meson recipes leave `install_targets` empty because `meson install` does not
accept target names.

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
