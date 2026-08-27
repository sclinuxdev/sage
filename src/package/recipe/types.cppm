export module sage.package:recipe_types;

import std;
import sage.vendor.toml;
import :version;
import :deps;
import :trigger;

export namespace sage::package {

// Recipe model for package building (`recipe.toml`).

// A secondary `[[source]]` entry: downloaded and sha256-verified beside the
// primary archive, then staged at src/distfiles/<filename> so prepare/build
// can consume patches and auxiliary tarballs alongside the unpacked tree.
struct ExtraSource {
    std::string url;
    std::string sha256;
};

enum class BuildSystem {
    Legacy,
    Autotools,
    CMake,
    Meson,
    Xmake,
    Cargo,
    Go,
    Make,
    Script,
};

inline std::expected<BuildSystem, std::string> parse_build_system(std::string_view name) {
    if (name == "autotools") return BuildSystem::Autotools;
    if (name == "cmake") return BuildSystem::CMake;
    if (name == "meson") return BuildSystem::Meson;
    if (name == "xmake") return BuildSystem::Xmake;
    if (name == "cargo") return BuildSystem::Cargo;
    if (name == "go") return BuildSystem::Go;
    if (name == "make") return BuildSystem::Make;
    if (name == "script") return BuildSystem::Script;
    return std::unexpected("Unsupported recipe v2 build system: " + std::string(name));
}

struct UpstreamSpec {
    std::string url;
    std::string version_regex;
};

struct FilePermission {
    std::string path;
    uint32_t mode{0644};
    uint32_t uid{0};
    uint32_t gid{0};
    std::string caps;
    // Symbolic owner spellings resolved at build time: "root", "dbus", ...
    // Mutually exclusive with the numeric uid/gid fields.
    std::string user;
    std::string group;
};

// Per-recipe flag downgrade. A recipe cannot inject flags, but a package that
// genuinely cannot take the global optimization policy may name the flag
// classes Sage must remove from every managed channel (CFLAGS/CXXFLAGS/
// LDFLAGS/RUSTFLAGS and their aliases, including the Kbuild ones).
struct FlagPolicy {
    bool no_lto{false};
    bool no_march{false};
    bool no_as_needed{false};

    [[nodiscard]] bool empty() const noexcept {
        return !no_lto && !no_march && !no_as_needed;
    }
};

// Deterministic payload post-processing applied to the staged install tree
// after the backend install and transforms, before ELF scanning and packing.
struct ContentPolicy {
    std::string strip{"none"};        // none | unneeded | debug
    std::string man_compress{"none"}; // none | gzip
    std::string shebangs;             // "" | "absolute"
    // Locale keep-list for usr/share/locale: empty keeps every locale.
    std::vector<std::string> locales;

    [[nodiscard]] bool empty() const noexcept {
        return strip == "none" && man_compress == "none" && shebangs.empty()
            && locales.empty();
    }
};

// Declarative system user/group request. Sage ships the equivalent
// sysusers.d fragment inside the payload and applies it at install time; a
// recipe never runs useradd itself.
struct SysUserEntry {
    std::string type;     // user | group
    std::string name;
    std::optional<uint32_t> id;  // numeric uid/gid; absent = system-allocated
    std::string description;
    std::string home;     // users only
    std::string shell;    // users only
    std::string group;    // primary group name, users only
};

// Cross-package symlink arbitration. Multiple installed packages may offer
// the same link path; Sage points it at the provider with the highest
// priority (name breaks ties) and re-points it when that provider leaves.
struct AlternativeEntry {
    std::string link;     // payload-relative link path, e.g. "usr/bin/vi"
    std::string target;   // symlink value, e.g. "vim"
    int priority{50};
};

struct CMakeBackendSpec {
    std::map<std::string, std::string> definitions;
    std::vector<std::string> features;
    std::string build_type{"Release"};
    std::vector<std::string> raw_options;
};

struct MesonBackendSpec {
    std::map<std::string, std::string> options;
    std::string build_type{"release"};
    std::vector<std::string> raw_options;
};

struct CargoBackendSpec {
    std::vector<std::string> features;
    std::optional<bool> default_features;
    bool locked{true};
    std::vector<std::string> raw_options;
};

struct AutotoolsBackendSpec {
    std::vector<std::string> enable;
    std::vector<std::string> disable;
    std::vector<std::string> with;
    std::vector<std::string> without;
    std::vector<std::string> raw_options;
};

struct MakeBackendSpec {
    std::vector<std::string> targets;
    std::vector<std::string> install_targets;
    std::map<std::string, std::string> variables;
    std::vector<std::string> raw_options;
};

struct XmakeBackendSpec {
    std::map<std::string, std::string> configs;
    std::string mode{"release"};
    std::vector<std::string> raw_options;
};

struct ToolRequirement {
    std::string family;
    std::string package;
    std::string minimum_version;
};

struct InstallCopy {
    std::string source;
    std::string destination;
};

struct InstallSymlink {
    std::string path;
    std::string target;
};

struct InstallMove {
    std::string source;
    std::string destination;
};

struct InstallRemove {
    std::string path;
};

struct InstallGenerate {
    std::string path;
    std::string content;
    uint32_t mode{0644};
};

// A recipe v2 step is an intentionally arbitrary shell operation, but Sage
// still owns its shell, environment, cwd and sandbox.  This is the escape
// hatch for package-specific fixups that cannot be reduced to a copy/move or
// glob operation without reopening the v1 escape routes.
struct ManagedBuildStep {
    std::string name;
    std::string phase;
    std::string cwd{"source"};
    std::string command;
    bool unsafe_shell{false};
};

// One output is a named view of the common DESTDIR.  Sage still emits one
// archive per recipe invocation; output names let a recipe describe the
// split-package boundary once and let the build driver select an output with
// `--output` in a later phase.  The default output is the recipe package.
struct InstallOutput {
    std::string name;
    std::optional<std::string> description;
    std::optional<std::string> license;
    std::optional<std::string> version;
    std::optional<std::string> release;
    std::optional<std::string> channel;
    std::optional<std::string> arch;
    std::vector<std::string> inherit;
    std::optional<std::vector<Dependency>> dependencies;
    std::optional<std::vector<std::string>> provides;
    std::optional<std::vector<Dependency>> conflicts;
    std::optional<std::vector<std::string>> conffiles;
    std::vector<std::string> install_files;
    std::vector<std::string> install_excludes;
    std::vector<std::string> optional_excludes;
    std::vector<InstallCopy> install_copies;
    std::vector<InstallSymlink> install_symlinks;
    std::vector<InstallMove> install_moves;
    std::vector<InstallRemove> install_removes;
    std::vector<InstallGenerate> install_generates;
    std::vector<FilePermission> file_permissions;
};

enum class PayloadMode {
    All,
    Allowlist,
    Outputs,
};

struct PatchSpec {
    std::string file;
    int strip{1};
    std::string sha256;
};

struct ManagedBuildSpec {
    BuildSystem system{BuildSystem::Legacy};
    // v2 requires the author to state whether the complete install tree,
    // an explicit allowlist, or named outputs become package payload.  The
    // explicit mode prevents a misspelled install_files key from silently
    // widening a split package to the whole DESTDIR.
    PayloadMode payload{PayloadMode::All};
    // Kbuild-compatible Make project. Sage derives kernel-specific channels
    // from the selected toolchain: clang enables LLVM=1, while the global
    // flag classes are mapped to KCFLAGS/KCPPFLAGS/KBUILD_LDFLAGS/KRUSTFLAGS.
    // The recipe therefore never needs to name a compiler or force LLVM.
    bool kernel{false};
    std::string source_subdir;
    // Empty means in-tree for Autotools/Make/Xmake/Cargo. CMake and Meson
    // receive a backend-specific `build` default in the parser below.
    std::string build_dir;
    std::vector<std::string> configure_options;
    std::vector<std::string> build_targets;
    std::vector<std::string> install_targets;
    // Paths that may survive the backend's install step.  These are package
    // payload paths relative to the staging root (for example
    // `usr/lib/libfoo.so.*`), not DESTDIR-prefixed filesystem paths.  The
    // `all` payload mode intentionally leaves this empty; `allowlist` requires
    // at least one pattern so a split package cannot widen itself silently.
    std::vector<std::string> install_files;
    // Optional path globs removed after the allowlist is applied.  Excludes
    // are useful for a package that takes most of an upstream install but
    // deliberately leaves a sub-tree to a sibling package.
    std::vector<std::string> install_excludes;
    std::vector<std::string> optional_excludes;
    std::vector<InstallCopy> install_copies;
    std::vector<InstallSymlink> install_symlinks;
    std::vector<InstallMove> install_moves;
    std::vector<InstallRemove> install_removes;
    std::vector<InstallGenerate> install_generates;
    std::vector<FilePermission> file_permissions;
    std::vector<InstallOutput> outputs;
    std::vector<ManagedBuildStep> steps;
    std::vector<std::string> patches;
    std::vector<PatchSpec> patches_spec;
    std::map<std::string, std::string> patch_checksums;
    int patch_strip{1};
    std::map<std::string, std::string> variables;
    std::vector<std::string> allowed_compilers;
    std::vector<std::string> allowed_linkers;
    std::vector<std::string> cflags_env;
    std::vector<std::string> cxxflags_env;
    std::vector<std::string> cppflags_env;
    std::vector<std::string> ldflags_env;
    std::vector<std::string> rustflags_env;
    std::vector<std::string> cc_env;
    std::vector<std::string> cxx_env;
    std::vector<std::string> linker_env;
    ToolRequirement compiler;
    ToolRequirement linker;
    ToolRequirement rust;
    ToolRequirement go;
    // Script-only escape hatch: the recipe's steps need the managed C/C++
    // toolchain (language runtimes with native extensions). Sage then audits
    // the build exactly like any managed backend instead of rejecting it.
    bool script_managed_tools{false};
    // Header-only / pure-data package: exempts the build from mandatory compiler
    // execution audit while asserting that no compiled ELF binaries are produced.
    bool header_only{false};
    // Opt-in network for the build sandbox. Default false keeps the hermetic
    // no-network boundary; declaring true is what lets a Go/Cargo recipe fetch
    // its module dependencies during the build.
    bool network{false};
    FlagPolicy flag_policy;
    ContentPolicy content;
    std::optional<CMakeBackendSpec> cmake;
    std::optional<MesonBackendSpec> meson;
    std::optional<CargoBackendSpec> cargo;
    std::optional<AutotoolsBackendSpec> autotools;
    std::optional<MakeBackendSpec> make;
    std::optional<XmakeBackendSpec> xmake;
};

// Declarative vendor dependency archive (e.g. for offline Go/Cargo/Node builds).
// Verified by SHA-256 and unpacked into `target` directory (default "vendor").
struct VendorSpec {
    std::string url;
    std::string sha256;
    std::string target{"vendor"};
};

struct Recipe {
    uint32_t schema_version{1};
    std::string name;
    Version version;
    std::string description;
    std::string license;
    std::string channel{"system"};
    // "any" marks an architecture-independent package (scripts, fonts, docs).
    std::string arch{"x86_64"};
    std::string source_url;
    std::string source_sha256;
    std::vector<ExtraSource> extra_sources;
    std::vector<VendorSpec> vendors;
    UpstreamSpec upstream;
    ManagedBuildSpec managed_build;
    // Build-only requirements; check_deps are resolved against the configured
    // read-only build sysroot and require at least one v2 phase named "check".
    std::vector<std::string> build_deps;
    std::vector<std::string> check_deps;
    std::vector<Dependency> host_deps;
    std::vector<Dependency> conflicts;
    std::vector<std::string> provides;
    // Absolute paths of shipped files protected from clobbering on reinstall;
    // copied verbatim onto the built manifest.
    std::vector<std::string> conffiles;
    std::vector<std::string> prepare_cmds;
    std::vector<std::string> build_cmds;
    std::vector<std::string> install_cmds;
    // Per-recipe compiler flags from the optional [build] table. Non-empty
    // values replace the global baseline from /etc/sage/build.toml -- the
    // declared downgrade for packages that cannot take the official one.
    // cxxflags empty mirrors cflags, mirroring BuildConfig's own rule.
    std::string cflags;
    std::string cxxflags;
    // Optional per-recipe linker-flag override from the [build] table.
    // Present-but-empty means "inject nothing" for legacy v1 recipes.
    std::optional<std::string> ldflags;
    // A non-empty `cc` pins the toolchain: exactly this pair is used and the
    // global fallback never runs. Core system packages (glibc, systemd) pin
    // "gcc" because they must not silently rebuild under clang.
    std::string cc;
    std::string cxx;
    std::vector<CapabilityHook> capability_hooks;
    std::vector<Trigger> triggers;
    // System user/group requests (v2, root [[sysusers]] array).
    std::vector<SysUserEntry> sysusers;
    // Cross-package symlink arbitration (v2, root [[alternatives]] array).
    std::vector<AlternativeEntry> alternatives;

    static std::expected<Recipe, std::string> parse_toml(std::string_view toml_content);
};

} // namespace sage::package
