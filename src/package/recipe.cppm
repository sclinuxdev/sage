export module sage.package:recipe;

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
    Make,
    Script,
};

inline std::expected<BuildSystem, std::string> parse_build_system(std::string_view name) {
    if (name == "autotools") return BuildSystem::Autotools;
    if (name == "cmake") return BuildSystem::CMake;
    if (name == "meson") return BuildSystem::Meson;
    if (name == "xmake") return BuildSystem::Xmake;
    if (name == "cargo") return BuildSystem::Cargo;
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
    std::optional<CMakeBackendSpec> cmake;
    std::optional<MesonBackendSpec> meson;
    std::optional<CargoBackendSpec> cargo;
    std::optional<AutotoolsBackendSpec> autotools;
    std::optional<MakeBackendSpec> make;
    std::optional<XmakeBackendSpec> xmake;
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

    static std::expected<Recipe, std::string> parse_toml(std::string_view toml_content) {
        auto tbl_res = vendor::toml::parse_string(toml_content);
        if (!tbl_res) return std::unexpected(tbl_res.error());
        const auto& tbl = *tbl_res;

        const auto reject_unknown = [](const vendor::toml::table& table,
                                       std::initializer_list<std::string_view> allowed,
                                       std::string_view scope)
            -> std::expected<void, std::string> {
            for (const auto& [key, _] : table) {
                if (!std::ranges::contains(allowed, key.str())) {
                    return std::unexpected(std::format(
                        "Unknown key '{}' in {}", key.str(), scope));
                }
            }
            return {};
        };
        const auto require_string = [](const vendor::toml::table& table,
                                       std::string_view key,
                                       std::string_view scope,
                                       bool required)
            -> std::expected<std::string, std::string> {
            const auto* node = table.get(key);
            if (!node) {
                if (required) return std::unexpected(std::format(
                    "Missing required string '{}.{}'", scope, key));
                return std::string{};
            }
            auto value = node->value<std::string_view>();
            if (!value) return std::unexpected(std::format(
                "'{}.{}' must be a string", scope, key));
            if (required && value->empty()) return std::unexpected(std::format(
                "'{}.{}' must not be empty", scope, key));
            return std::string(*value);
        };

        Recipe r;
        if (tbl.contains("schema_version")
            && !tbl["schema_version"].value<std::int64_t>())
            return std::unexpected("schema_version must be an integer");
        r.schema_version = static_cast<uint32_t>(tbl["schema_version"].value_or(1LL));
        if (r.schema_version != 1 && r.schema_version != 2) {
            return std::unexpected(std::format(
                "Unsupported recipe schema_version {}; expected 1 or 2",
                r.schema_version));
        }

        if (r.schema_version == 2) {
            if (auto result = reject_unknown(tbl,
                    {"schema_version", "package", "upstream", "source",
                     "build", "capability_hooks", "triggers"}, "recipe"); !result)
                return std::unexpected(result.error());
        }

        if (auto* pkg = tbl.get_as<vendor::toml::table>("package")) {
            if (r.schema_version == 2) {
                if (auto result = reject_unknown(*pkg,
                        {"name", "version", "release", "description", "license",
                         "channel", "arch", "dependencies", "build_dependencies",
                         "check_dependencies", "provides", "conflicts", "conffiles",
                         "upstream", "upstream_regex"}, "package"); !result)
                    return std::unexpected(result.error());
            }
            r.name = (*pkg)["name"].value_or("");
            std::string ver_str = std::string((*pkg)["version"].value_or(""));
            r.version = Version::parse(ver_str);
            auto release = parse_release_field(*pkg);
            if (!release) return std::unexpected(release.error());
            r.version.rel = std::move(*release);
            r.description = (*pkg)["description"].value_or("");
            r.license = (*pkg)["license"].value_or("");
            r.channel = (*pkg)["channel"].value_or("system");
            r.arch = (*pkg)["arch"].value_or("x86_64");
            r.upstream.url = (*pkg)["upstream"].value_or("");
            r.upstream.version_regex = (*pkg)["upstream_regex"].value_or("");
            auto architecture = validate_package_architecture(r.arch);
            if (!architecture) return std::unexpected(architecture.error());
        } else {
            return std::unexpected("Missing [package] section in recipe");
        }

        if (r.schema_version == 2 && tbl.contains("upstream")
            && !tbl.get_as<vendor::toml::table>("upstream"))
            return std::unexpected("recipe.upstream must be a table");
        if (auto* upstream = tbl.get_as<vendor::toml::table>("upstream")) {
            if (r.schema_version == 2) {
                if (auto result = reject_unknown(*upstream,
                        {"url", "version_regex"}, "upstream"); !result)
                    return std::unexpected(result.error());
            }
            r.upstream.url = (*upstream)["url"].value_or(r.upstream.url);
            r.upstream.version_regex =
                (*upstream)["version_regex"].value_or(r.upstream.version_regex);
        }
        if (r.upstream.url.empty() != r.upstream.version_regex.empty()) {
            return std::unexpected(
                "Upstream tracking requires both url and version_regex");
        }

        if (r.schema_version == 2) {
            auto pkg = tbl.get_as<vendor::toml::table>("package");
            for (const auto key : {"name", "version", "license", "channel", "arch"}) {
                auto value = require_string(*pkg, key, "package", true);
                if (!value) return std::unexpected(value.error());
            }
            for (const auto key : {"description", "upstream", "upstream_regex"}) {
                auto value = require_string(*pkg, key, "package", false);
                if (!value) return std::unexpected(value.error());
            }
            if (r.name.find('/') != std::string::npos || r.name == "." || r.name == "..")
                return std::unexpected("package.name must be a simple package name");
            if (r.version.ver.empty())
                return std::unexpected("package.version must not be empty");
            if (!r.upstream.version_regex.empty()) {
                try {
                    (void)std::regex(r.upstream.version_regex);
                } catch (const std::regex_error& error) {
                    return std::unexpected(std::format(
                        "Invalid upstream.version_regex: {}", error.what()));
                }
            }
        }

        // [source] is either one table (a single download) or an array of
        // tables (`[[source]]`): the first element is the primary archive that
        // gets unpacked to src/, any further elements are extra downloads.
        // TOML keys written after a [[source]] block land inside the last
        // element, so every scope-sensitive collector below must see all of
        // them -- src_scopes is the shared third layer.
        std::vector<const vendor::toml::table*> src_scopes;
        std::map<std::string, std::string> source_hashes;
        const auto normalize_sha256 = [](std::string value) {
            std::ranges::transform(value, value.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return value;
        };
        if (r.schema_version == 2 && tbl.contains("source")
            && !tbl.get_as<vendor::toml::array>("source")
            && !tbl.get_as<vendor::toml::table>("source"))
            return std::unexpected("recipe.source must be a table or array of tables");
        if (auto* arr = tbl.get_as<vendor::toml::array>("source")) {
            for (auto&& el : *arr) {
                const vendor::toml::table* t = el.as_table();
                if (!t) return std::unexpected("[[source]] entries must be tables");
                std::string url = (*t)["url"].value_or("");
                std::string sha = (*t)["sha256"].value_or("");
                if (!r.source_url.empty() || !src_scopes.empty()) {
                    r.extra_sources.push_back({std::move(url), std::move(sha)});
                } else {
                    r.source_url = std::move(url);
                    r.source_sha256 = std::move(sha);
                }
                src_scopes.push_back(t);
            }
        } else if (auto* src = tbl.get_as<vendor::toml::table>("source")) {
            r.source_url = (*src)["url"].value_or("");
            r.source_sha256 = (*src)["sha256"].value_or("");
            src_scopes.push_back(src);
        }

        if (r.schema_version == 2) {
            for (const auto* src : src_scopes) {
                if (auto result = reject_unknown(*src, {"url", "sha256"},
                                                 "source"); !result)
                    return std::unexpected(result.error());
                auto url = require_string(*src, "url", "source", true);
                auto sha = require_string(*src, "sha256", "source", true);
                if (!url || !sha) return std::unexpected(
                    (!url ? url.error() : sha.error()));
            }
            if (!src_scopes.empty() && r.source_url.empty())
                return std::unexpected(
                    "v2 source entries require a non-empty primary source URL");
            const auto source_basename = [](std::string_view url) {
                const auto path = url.substr(0, url.find_first_of("?#"));
                return std::filesystem::path(std::string(path)).filename().string();
            };
            std::set<std::string> source_names;
            const auto add_source_hash = [&](std::string_view url,
                                             std::string_view sha)
                -> std::expected<void, std::string> {
                const auto name = source_basename(url);
                if (name.empty()) return std::unexpected(
                    "v2 source URL must have a filename component");
                if (!source_names.insert(name).second) return std::unexpected(
                    "v2 source URLs must have unique filenames: " + name);
                source_hashes.emplace(name, normalize_sha256(std::string(sha)));
                return {};
            };
            if (!r.source_url.empty()) {
                if (auto result = add_source_hash(r.source_url, r.source_sha256);
                    !result)
                    return std::unexpected(result.error());
            }
            for (const auto& extra : r.extra_sources) {
                if (auto result = add_source_hash(extra.url, extra.sha256);
                    !result)
                    return std::unexpected(result.error());
            }
        }

        auto parse_deps = [&](const vendor::toml::table& t, const char* key, std::vector<Dependency>& target) {
            if (auto* arr = t.get_as<vendor::toml::array>(key)) {
                for (auto&& d : *arr) {
                    if (auto str = d.value<std::string_view>()) {
                        target.push_back(Dependency::parse(*str));
                    }
                }
            }
        };

        auto parse_strings = [&](const vendor::toml::table& t, const char* key, std::vector<std::string>& target) {
            if (auto* arr = t.get_as<vendor::toml::array>(key)) {
                for (auto&& p : *arr) {
                    if (auto str = p.value<std::string_view>()) {
                        target.emplace_back(*str);
                    }
                }
            }
        };

        const auto parse_string_array_strict = [&](const vendor::toml::table& table,
                                                   std::string_view key,
                                                   std::string_view scope,
                                                   std::vector<std::string>& target)
            -> std::expected<void, std::string> {
            const auto* node = table.get(key);
            if (!node) return {};
            const auto* array = node->as_array();
            if (!array) return std::unexpected(std::format(
                "'{}.{}' must be an array of strings", scope, key));
            for (const auto& element : *array) {
                auto value = element.value<std::string_view>();
                if (!value) return std::unexpected(std::format(
                    "'{}.{}' must contain only strings", scope, key));
                target.emplace_back(*value);
            }
            return {};
        };
        const auto parse_deps_strict = [&](const vendor::toml::table& table,
                                           std::string_view key,
                                           std::string_view scope,
                                           std::vector<Dependency>& target)
            -> std::expected<void, std::string> {
            const auto* node = table.get(key);
            if (!node) return {};
            const auto* array = node->as_array();
            if (!array) return std::unexpected(std::format(
                "'{}.{}' must be an array of dependency strings", scope, key));
            for (const auto& element : *array) {
                auto value = element.value<std::string_view>();
                if (!value || value->empty()) return std::unexpected(std::format(
                    "'{}.{}' must contain only non-empty strings", scope, key));
                target.push_back(Dependency::parse(*value));
            }
            return {};
        };

        parse_deps(tbl, "dependencies", r.host_deps);
        parse_deps(tbl, "conflicts", r.conflicts);
        parse_strings(tbl, "build_dependencies", r.build_deps);
        parse_strings(tbl, "check_dependencies", r.check_deps);
        parse_strings(tbl, "provides", r.provides);
        parse_strings(tbl, "conffiles", r.conffiles);

        if (auto* pkg = tbl.get_as<vendor::toml::table>("package")) {
            parse_deps(*pkg, "dependencies", r.host_deps);
            parse_deps(*pkg, "conflicts", r.conflicts);
            parse_strings(*pkg, "build_dependencies", r.build_deps);
            parse_strings(*pkg, "check_dependencies", r.check_deps);
            parse_strings(*pkg, "provides", r.provides);
            parse_strings(*pkg, "conffiles", r.conffiles);
        }

        for (const auto* src : src_scopes) {
            parse_deps(*src, "dependencies", r.host_deps);
            parse_strings(*src, "build_dependencies", r.build_deps);
            parse_strings(*src, "check_dependencies", r.check_deps);
            parse_strings(*src, "provides", r.provides);
            parse_strings(*src, "conffiles", r.conffiles);
        }

        auto extract_cmds = [&](const char* key, std::vector<std::string>& dest) {
            if (auto* arr = tbl.get_as<vendor::toml::array>(key)) {
                for (auto&& c : *arr) {
                    if (auto str = c.value<std::string_view>()) {
                        dest.emplace_back(*str);
                    }
                }
            }
            if (auto* pkg = tbl.get_as<vendor::toml::table>("package")) {
                if (auto* arr = pkg->get_as<vendor::toml::array>(key)) {
                    for (auto&& c : *arr) {
                        if (auto str = c.value<std::string_view>()) {
                            dest.emplace_back(*str);
                        }
                    }
                }
            }
            for (const auto* src : src_scopes) {
                if (auto* arr = src->get_as<vendor::toml::array>(key)) {
                    for (auto&& c : *arr) {
                        if (auto str = c.value<std::string_view>()) {
                            dest.emplace_back(*str);
                        }
                    }
                }
            }
            // The [build] table is the fourth scope. It exists mainly to carry
            // the per-recipe flag overrides below, and since a root-level
            // `build = [...]` array cannot coexist with a `[build]` table in
            // the same document (duplicate key), phase commands must be
            // expressible here too or the two features would exclude
            // each other.
            if (auto* bld = tbl.get_as<vendor::toml::table>("build")) {
                if (auto* arr = bld->get_as<vendor::toml::array>(key)) {
                    for (auto&& c : *arr) {
                        if (auto str = c.value<std::string_view>()) {
                            dest.emplace_back(*str);
                        }
                    }
                }
            }
        };

        if (r.schema_version == 1) {
            extract_cmds("prepare", r.prepare_cmds);
            extract_cmds("build", r.build_cmds);
            extract_cmds("install", r.install_cmds);

            // v1 remains source-compatible: its commands and toolchain
            // overrides are legacy inputs, but no longer enter manifests.
            if (auto* bld = tbl.get_as<vendor::toml::table>("build")) {
                if (auto v = (*bld)["cflags"].value<std::string_view>()) r.cflags = std::string(*v);
                if (auto v = (*bld)["cxxflags"].value<std::string_view>()) r.cxxflags = std::string(*v);
                if (auto v = (*bld)["cc"].value<std::string_view>()) r.cc = std::string(*v);
                if (auto v = (*bld)["cxx"].value<std::string_view>()) r.cxx = std::string(*v);
                if (auto v = (*bld)["ldflags"].value<std::string_view>()) r.ldflags = std::string(*v);
            }
        } else {
            auto pkg = tbl.get_as<vendor::toml::table>("package");
            r.host_deps.clear();
            r.conflicts.clear();
            r.build_deps.clear();
            r.check_deps.clear();
            r.provides.clear();
            r.conffiles.clear();
            if (auto result = parse_deps_strict(*pkg, "dependencies", "package",
                                                r.host_deps); !result)
                return std::unexpected(result.error());
            if (auto result = parse_deps_strict(*pkg, "conflicts", "package",
                                                r.conflicts); !result)
                return std::unexpected(result.error());
            if (auto result = parse_string_array_strict(*pkg, "build_dependencies",
                    "package", r.build_deps); !result)
                return std::unexpected(result.error());
            if (auto result = parse_string_array_strict(*pkg, "check_dependencies",
                    "package", r.check_deps); !result)
                return std::unexpected(result.error());
            if (auto result = parse_string_array_strict(*pkg, "provides",
                    "package", r.provides); !result)
                return std::unexpected(result.error());
            if (auto result = parse_string_array_strict(*pkg, "conffiles",
                    "package", r.conffiles); !result)
                return std::unexpected(result.error());
            const auto valid_sha256 = [](std::string_view value) {
                return value.size() == 64 && std::ranges::all_of(value, [](char c) {
                    return std::isxdigit(static_cast<unsigned char>(c));
                });
            };
            if (!r.source_url.empty() && !valid_sha256(r.source_sha256)) {
                return std::unexpected(
                    "Recipe v2 requires a 64-hex source.sha256 when source.url is present");
            }
            for (const auto& source : r.extra_sources) {
                if (source.url.empty() || !valid_sha256(source.sha256)) {
                    return std::unexpected(
                        "Recipe v2 [[source]] entries require a URL and a 64-hex sha256");
                }
            }
            auto* bld = tbl.get_as<vendor::toml::table>("build");
            if (!bld) return std::unexpected("Recipe v2 requires a [build] table");
            if (auto result = reject_unknown(*bld,
                    {"system", "payload", "kernel", "source_subdir", "build_dir",
                     "configure_options", "build_targets", "install_targets",
                     "install_files", "install_excludes", "optional_excludes",
                     "install_copies", "install_symlinks", "install_moves",
                     "install_removes", "install_generates", "file_permissions",
                     "outputs", "steps", "patches", "patch_checksums", "patch_strip",
                     "allowed_compilers", "allowed_linkers", "variables", "flag_env",
                     "tool_env", "toolchain", "cmake", "meson", "cargo",
                     "autotools", "make", "xmake"}, "build"); !result)
                return std::unexpected(result.error());
            auto has_commands = [&](const vendor::toml::table& scope) {
                return scope.get_as<vendor::toml::array>("prepare")
                    || scope.get_as<vendor::toml::array>("build")
                    || scope.get_as<vendor::toml::array>("install");
            };
            if (has_commands(tbl) || has_commands(*bld)) {
                return std::unexpected(
                    "Recipe v2 forbids prepare/build/install shell command arrays");
            }
            if (auto* pkg = tbl.get_as<vendor::toml::table>("package");
                pkg && has_commands(*pkg)) {
                return std::unexpected(
                    "Recipe v2 forbids prepare/build/install shell command arrays");
            }
            for (const auto* src : src_scopes) {
                if (has_commands(*src)) return std::unexpected(
                    "Recipe v2 forbids prepare/build/install shell command arrays");
            }
            auto system = parse_build_system((*bld)["system"].value_or(""));
            if (!system) return std::unexpected(system.error());
            r.managed_build.system = *system;
            const auto payload_value = (*bld)["payload"].value<std::string_view>();
            if (!payload_value) return std::unexpected(
                "Recipe v2 requires build.payload = \"all\", \"allowlist\", or \"outputs\"");
            if (*payload_value == "all") r.managed_build.payload = PayloadMode::All;
            else if (*payload_value == "allowlist")
                r.managed_build.payload = PayloadMode::Allowlist;
            else if (*payload_value == "outputs")
                r.managed_build.payload = PayloadMode::Outputs;
            else return std::unexpected(
                "build.payload must be \"all\", \"allowlist\", or \"outputs\"");
            if (auto value = (*bld)["kernel"].value<bool>())
                r.managed_build.kernel = *value;
            else if (bld->contains("kernel"))
                return std::unexpected("build.kernel must be a boolean");
            if (r.managed_build.kernel
                && r.managed_build.system != BuildSystem::Make)
                return std::unexpected(
                    "build.kernel=true requires system = \"make\"");
            auto source_subdir = require_string(*bld, "source_subdir", "build", false);
            auto build_dir = require_string(*bld, "build_dir", "build", false);
            if (!source_subdir || !build_dir)
                return std::unexpected(!source_subdir ? source_subdir.error()
                                                        : build_dir.error());
            r.managed_build.source_subdir = std::move(*source_subdir);
            r.managed_build.build_dir = build_dir->empty()
                ? (
                *system == BuildSystem::CMake || *system == BuildSystem::Meson
                    ? "build" : "")
                : std::move(*build_dir);
            if (bld->contains("patch_strip")
                && !(*bld)["patch_strip"].value<std::int64_t>())
                return std::unexpected("build.patch_strip must be an integer");
            r.managed_build.patch_strip = static_cast<int>((*bld)["patch_strip"].value_or(1LL));
            if (r.managed_build.patch_strip < 0 || r.managed_build.patch_strip > 9)
                return std::unexpected("build.patch_strip must be between 0 and 9");
            if (auto result = parse_string_array_strict(*bld, "configure_options",
                    "build", r.managed_build.configure_options); !result)
                return std::unexpected(result.error());
            if (auto result = parse_string_array_strict(*bld, "build_targets",
                    "build", r.managed_build.build_targets); !result)
                return std::unexpected(result.error());
            if (auto result = parse_string_array_strict(*bld, "install_targets",
                    "build", r.managed_build.install_targets); !result)
                return std::unexpected(result.error());
            if (auto result = parse_string_array_strict(*bld, "install_files",
                    "build", r.managed_build.install_files); !result)
                return std::unexpected(result.error());
            if (auto result = parse_string_array_strict(*bld, "install_excludes",
                    "build", r.managed_build.install_excludes); !result)
                return std::unexpected(result.error());
            const auto parse_string_map_strict = [&](const vendor::toml::table& table,
                                                     std::string_view key,
                                                     std::string_view scope,
                                                     std::map<std::string, std::string>& target)
                -> std::expected<void, std::string> {
                const auto* node = table.get(key);
                if (!node) return {};
                const auto* map = node->as_table();
                if (!map) return std::unexpected(std::format(
                    "'{}.{}' must be a table", scope, key));
                for (const auto& [k, v] : *map) {
                    if (auto s = v.value<std::string_view>()) {
                        target.emplace(k.str(), std::string(*s));
                    } else if (auto b = v.value<bool>()) {
                        target.emplace(k.str(), *b ? "ON" : "OFF");
                    } else if (auto i = v.value<std::int64_t>()) {
                        target.emplace(k.str(), std::to_string(*i));
                    } else {
                        return std::unexpected(std::format(
                            "'{}.{}.{}' must be a string, boolean or integer", scope, key, k.str()));
                    }
                }
                return {};
            };

            auto parse_copy_entries = [&](const vendor::toml::table& scope_tbl,
                                          const char* key,
                                          std::string_view scope_name,
                                          std::vector<InstallCopy>& target)
                -> std::expected<void, std::string> {
                if (auto* arr = scope_tbl.get_as<vendor::toml::array>(key)) {
                    for (auto&& element : *arr) {
                        auto* item = element.as_table();
                        if (!item) return std::unexpected(std::format(
                            "{}.{} entries must be inline tables", scope_name, key));
                        if (auto result = reject_unknown(*item, {"from", "to"},
                                std::format("{}.{}[]", scope_name, key)); !result)
                            return std::unexpected(result.error());
                        auto source = (*item)["from"].value<std::string_view>();
                        auto destination = (*item)["to"].value<std::string_view>();
                        if (!source || !destination || source->empty()
                            || destination->empty()) return std::unexpected(std::format(
                                "{}.{} entries require non-empty from and to", scope_name, key));
                        target.push_back({std::string(*source), std::string(*destination)});
                    }
                }
                return {};
            };
            for (const auto key : {"install_copies", "install_symlinks",
                                   "install_moves", "install_removes",
                                   "install_generates", "file_permissions",
                                   "optional_excludes"}) {
                if (bld->contains(key) && !bld->get_as<vendor::toml::array>(key))
                    return std::unexpected(std::format(
                        "build.{} must be an array", key));
            }
            auto parse_symlink_entries = [&](const vendor::toml::table& scope_tbl,
                                             const char* key,
                                             std::string_view scope_name,
                                             std::vector<InstallSymlink>& target)
                -> std::expected<void, std::string> {
                if (auto* arr = scope_tbl.get_as<vendor::toml::array>(key)) {
                    for (auto&& element : *arr) {
                        auto* item = element.as_table();
                        if (!item) return std::unexpected(std::format(
                            "{}.{} entries must be inline tables", scope_name, key));
                        if (auto result = reject_unknown(*item, {"path", "target"},
                                std::format("{}.{}[]", scope_name, key)); !result)
                            return std::unexpected(result.error());
                        auto path = (*item)["path"].value<std::string_view>();
                        auto target_path = (*item)["target"].value<std::string_view>();
                        if (!path || !target_path || path->empty()
                            || target_path->empty()) return std::unexpected(std::format(
                                "{}.{} entries require non-empty path and target", scope_name, key));
                        target.push_back({std::string(*path), std::string(*target_path)});
                    }
                }
                return {};
            };
            auto parse_move_entries = [&](const vendor::toml::table& scope_tbl,
                                          const char* key,
                                          std::string_view scope_name,
                                          std::vector<InstallMove>& target)
                -> std::expected<void, std::string> {
                if (auto* arr = scope_tbl.get_as<vendor::toml::array>(key)) {
                    for (auto&& element : *arr) {
                        auto* item = element.as_table();
                        if (!item) return std::unexpected(std::format(
                            "{}.{} entries must be inline tables", scope_name, key));
                        if (auto result = reject_unknown(*item, {"from", "to"},
                                std::format("{}.{}[]", scope_name, key)); !result)
                            return std::unexpected(result.error());
                        auto source = (*item)["from"].value<std::string_view>();
                        auto destination = (*item)["to"].value<std::string_view>();
                        if (!source || !destination || source->empty()
                            || destination->empty()) return std::unexpected(std::format(
                            "{}.{} entries require non-empty from and to", scope_name, key));
                        target.push_back({std::string(*source), std::string(*destination)});
                    }
                }
                return {};
            };
            auto parse_remove_entries = [&](const vendor::toml::table& scope_tbl,
                                            const char* key,
                                            std::string_view scope_name,
                                            std::vector<InstallRemove>& target)
                -> std::expected<void, std::string> {
                if (auto* arr = scope_tbl.get_as<vendor::toml::array>(key)) {
                    for (auto&& element : *arr) {
                        if (!element.is_string()) return std::unexpected(std::format(
                            "{}.{} entries must be strings", scope_name, key));
                        auto path = element.value<std::string_view>();
                        if (!path || path->empty()) return std::unexpected(std::format(
                            "{}.{} entries must be non-empty strings", scope_name, key));
                        target.push_back({std::string(*path)});
                    }
                }
                return {};
            };
            auto parse_generate_entries = [&](const vendor::toml::table& scope_tbl,
                                              const char* key,
                                              std::string_view scope_name,
                                              std::vector<InstallGenerate>& target)
                -> std::expected<void, std::string> {
                if (auto* arr = scope_tbl.get_as<vendor::toml::array>(key)) {
                    for (auto&& element : *arr) {
                        auto* item = element.as_table();
                        if (!item) return std::unexpected(std::format(
                            "{}.{} entries must be inline tables", scope_name, key));
                        if (auto result = reject_unknown(*item,
                                {"path", "content", "mode"},
                                std::format("{}.{}[]", scope_name, key)); !result)
                            return std::unexpected(result.error());
                        auto path = (*item)["path"].value<std::string_view>();
                        auto content = (*item)["content"].value<std::string_view>();
                        if (item->contains("mode")
                            && !(*item)["mode"].value<std::int64_t>())
                            return std::unexpected(std::format(
                                "{}.{}.mode must be an integer", scope_name, key));
                        auto mode = (*item)["mode"].value<std::int64_t>().value_or(0644);
                        if (!path || !content || path->empty() || mode < 0 || mode > 07777)
                            return std::unexpected(std::format(
                                "{}.{} entries require path/content and a valid mode", scope_name, key));
                        target.push_back({
                            std::string(*path), std::string(*content), static_cast<uint32_t>(mode)});
                    }
                }
                return {};
            };
            auto parse_file_permission_entries = [&](const vendor::toml::table& scope_tbl,
                                                     const char* key,
                                                     std::string_view scope_name,
                                                     std::vector<FilePermission>& target)
                -> std::expected<void, std::string> {
                if (auto* arr = scope_tbl.get_as<vendor::toml::array>(key)) {
                    for (auto&& element : *arr) {
                        auto* item = element.as_table();
                        if (!item) return std::unexpected(std::format(
                            "{}.{} entries must be inline tables", scope_name, key));
                        if (auto result = reject_unknown(*item,
                                {"path", "mode", "uid", "gid", "caps"},
                                std::format("{}.{}[]", scope_name, key)); !result)
                            return std::unexpected(result.error());
                        auto path = (*item)["path"].value<std::string_view>();
                        if (!path || path->empty()) return std::unexpected(std::format(
                            "{}.{} entries require non-empty path", scope_name, key));
                        if (item->contains("mode") && !(*item)["mode"].value<std::int64_t>())
                            return std::unexpected(std::format(
                                "{}.{}.mode must be an integer", scope_name, key));
                        if (item->contains("uid") && !(*item)["uid"].value<std::int64_t>())
                            return std::unexpected(std::format(
                                "{}.{}.uid must be an integer", scope_name, key));
                        if (item->contains("gid") && !(*item)["gid"].value<std::int64_t>())
                            return std::unexpected(std::format(
                                "{}.{}.gid must be an integer", scope_name, key));
                        auto mode = (*item)["mode"].value<std::int64_t>().value_or(0644);
                        auto uid = (*item)["uid"].value<std::int64_t>().value_or(0);
                        auto gid = (*item)["gid"].value<std::int64_t>().value_or(0);
                        auto caps = (*item)["caps"].value<std::string_view>().value_or("");
                        if (mode < 0 || mode > 07777 || uid < 0 || gid < 0)
                            return std::unexpected(std::format(
                                "{}.{} entries require valid mode, uid and gid", scope_name, key));
                        target.push_back(FilePermission{
                            .path = std::string(*path),
                            .mode = static_cast<uint32_t>(mode),
                            .uid = static_cast<uint32_t>(uid),
                            .gid = static_cast<uint32_t>(gid),
                            .caps = std::string(caps),
                        });
                    }
                }
                return {};
            };

            if (auto result = parse_copy_entries(*bld, "install_copies",
                                                 "build", r.managed_build.install_copies); !result)
                return std::unexpected(result.error());
            if (auto result = parse_symlink_entries(*bld, "install_symlinks",
                                                    "build", r.managed_build.install_symlinks); !result)
                return std::unexpected(result.error());
            if (auto result = parse_move_entries(*bld, "install_moves",
                                                "build", r.managed_build.install_moves); !result)
                return std::unexpected(result.error());
            if (auto result = parse_remove_entries(*bld, "install_removes",
                                                  "build", r.managed_build.install_removes); !result)
                return std::unexpected(result.error());
            if (auto result = parse_generate_entries(*bld, "install_generates",
                                                     "build", r.managed_build.install_generates); !result)
                return std::unexpected(result.error());
            if (auto result = parse_file_permission_entries(*bld, "file_permissions",
                                                            "build", r.managed_build.file_permissions); !result)
                return std::unexpected(result.error());
            if (auto result = parse_string_array_strict(*bld, "optional_excludes",
                    "build", r.managed_build.optional_excludes); !result)
                return std::unexpected(result.error());

            if (bld->contains("cmake") && !bld->get_as<vendor::toml::table>("cmake"))
                return std::unexpected("build.cmake must be a table");
            if (auto* spec = bld->get_as<vendor::toml::table>("cmake")) {
                if (auto result = reject_unknown(*spec,
                        {"definitions", "features", "build_type", "raw_options"},
                        "build.cmake"); !result)
                    return std::unexpected(result.error());
                CMakeBackendSpec cmake_spec;
                if (auto result = parse_string_map_strict(*spec, "definitions",
                        "build.cmake", cmake_spec.definitions); !result)
                    return std::unexpected(result.error());
                if (auto result = parse_string_array_strict(*spec, "features",
                        "build.cmake", cmake_spec.features); !result)
                    return std::unexpected(result.error());
                if (spec->contains("build_type")) {
                    auto bt = (*spec)["build_type"].value<std::string_view>();
                    if (!bt || bt->empty()) return std::unexpected("build.cmake.build_type must be a non-empty string");
                    cmake_spec.build_type = std::string(*bt);
                }
                if (auto result = parse_string_array_strict(*spec, "raw_options",
                        "build.cmake", cmake_spec.raw_options); !result)
                    return std::unexpected(result.error());
                r.managed_build.cmake = std::move(cmake_spec);
            }

            if (bld->contains("meson") && !bld->get_as<vendor::toml::table>("meson"))
                return std::unexpected("build.meson must be a table");
            if (auto* spec = bld->get_as<vendor::toml::table>("meson")) {
                if (auto result = reject_unknown(*spec,
                        {"options", "build_type", "raw_options"},
                        "build.meson"); !result)
                    return std::unexpected(result.error());
                MesonBackendSpec meson_spec;
                if (auto result = parse_string_map_strict(*spec, "options",
                        "build.meson", meson_spec.options); !result)
                    return std::unexpected(result.error());
                if (spec->contains("build_type")) {
                    auto bt = (*spec)["build_type"].value<std::string_view>();
                    if (!bt || bt->empty()) return std::unexpected("build.meson.build_type must be a non-empty string");
                    meson_spec.build_type = std::string(*bt);
                }
                if (auto result = parse_string_array_strict(*spec, "raw_options",
                        "build.meson", meson_spec.raw_options); !result)
                    return std::unexpected(result.error());
                r.managed_build.meson = std::move(meson_spec);
            }

            if (bld->contains("cargo") && !bld->get_as<vendor::toml::table>("cargo"))
                return std::unexpected("build.cargo must be a table");
            if (auto* spec = bld->get_as<vendor::toml::table>("cargo")) {
                if (auto result = reject_unknown(*spec,
                        {"features", "default_features", "locked", "raw_options"},
                        "build.cargo"); !result)
                    return std::unexpected(result.error());
                CargoBackendSpec cargo_spec;
                if (auto result = parse_string_array_strict(*spec, "features",
                        "build.cargo", cargo_spec.features); !result)
                    return std::unexpected(result.error());
                if (spec->contains("default_features")) {
                    auto df = (*spec)["default_features"].value<bool>();
                    if (!df) return std::unexpected("build.cargo.default_features must be a boolean");
                    cargo_spec.default_features = *df;
                }
                if (spec->contains("locked")) {
                    auto lk = (*spec)["locked"].value<bool>();
                    if (!lk) return std::unexpected("build.cargo.locked must be a boolean");
                    cargo_spec.locked = *lk;
                }
                if (auto result = parse_string_array_strict(*spec, "raw_options",
                        "build.cargo", cargo_spec.raw_options); !result)
                    return std::unexpected(result.error());
                r.managed_build.cargo = std::move(cargo_spec);
            }

            if (bld->contains("autotools") && !bld->get_as<vendor::toml::table>("autotools"))
                return std::unexpected("build.autotools must be a table");
            if (auto* spec = bld->get_as<vendor::toml::table>("autotools")) {
                if (auto result = reject_unknown(*spec,
                        {"enable", "disable", "with", "without", "raw_options"},
                        "build.autotools"); !result)
                    return std::unexpected(result.error());
                AutotoolsBackendSpec autotools_spec;
                if (auto result = parse_string_array_strict(*spec, "enable",
                        "build.autotools", autotools_spec.enable); !result)
                    return std::unexpected(result.error());
                if (auto result = parse_string_array_strict(*spec, "disable",
                        "build.autotools", autotools_spec.disable); !result)
                    return std::unexpected(result.error());
                if (auto result = parse_string_array_strict(*spec, "with",
                        "build.autotools", autotools_spec.with); !result)
                    return std::unexpected(result.error());
                if (auto result = parse_string_array_strict(*spec, "without",
                        "build.autotools", autotools_spec.without); !result)
                    return std::unexpected(result.error());
                if (auto result = parse_string_array_strict(*spec, "raw_options",
                        "build.autotools", autotools_spec.raw_options); !result)
                    return std::unexpected(result.error());
                r.managed_build.autotools = std::move(autotools_spec);
            }

            if (bld->contains("make") && !bld->get_as<vendor::toml::table>("make"))
                return std::unexpected("build.make must be a table");
            if (auto* spec = bld->get_as<vendor::toml::table>("make")) {
                if (auto result = reject_unknown(*spec,
                        {"targets", "install_targets", "variables", "raw_options"},
                        "build.make"); !result)
                    return std::unexpected(result.error());
                MakeBackendSpec make_spec;
                if (auto result = parse_string_array_strict(*spec, "targets",
                        "build.make", make_spec.targets); !result)
                    return std::unexpected(result.error());
                if (auto result = parse_string_array_strict(*spec, "install_targets",
                        "build.make", make_spec.install_targets); !result)
                    return std::unexpected(result.error());
                if (auto result = parse_string_map_strict(*spec, "variables",
                        "build.make", make_spec.variables); !result)
                    return std::unexpected(result.error());
                if (auto result = parse_string_array_strict(*spec, "raw_options",
                        "build.make", make_spec.raw_options); !result)
                    return std::unexpected(result.error());
                r.managed_build.make = std::move(make_spec);
            }

            if (bld->contains("xmake") && !bld->get_as<vendor::toml::table>("xmake"))
                return std::unexpected("build.xmake must be a table");
            if (auto* spec = bld->get_as<vendor::toml::table>("xmake")) {
                if (auto result = reject_unknown(*spec,
                        {"configs", "mode", "raw_options"},
                        "build.xmake"); !result)
                    return std::unexpected(result.error());
                XmakeBackendSpec xmake_spec;
                if (auto result = parse_string_map_strict(*spec, "configs",
                        "build.xmake", xmake_spec.configs); !result)
                    return std::unexpected(result.error());
                if (spec->contains("mode")) {
                    auto md = (*spec)["mode"].value<std::string_view>();
                    if (!md || md->empty()) return std::unexpected("build.xmake.mode must be a non-empty string");
                    xmake_spec.mode = std::string(*md);
                }
                if (auto result = parse_string_array_strict(*spec, "raw_options",
                        "build.xmake", xmake_spec.raw_options); !result)
                    return std::unexpected(result.error());
                r.managed_build.xmake = std::move(xmake_spec);
            }

            if (auto* arr = bld->get_as<vendor::toml::array>("outputs")) {
                for (auto&& element : *arr) {
                    auto* item = element.as_table();
                    if (!item) return std::unexpected(
                        "build.outputs entries must be inline tables");
                    if (auto result = reject_unknown(*item,
                            {"name", "description", "license", "version", "release",
                             "channel", "arch", "inherit", "dependencies",
                             "provides", "conflicts", "conffiles", "install_files",
                             "install_excludes", "optional_excludes", "install_copies",
                             "install_symlinks", "install_moves", "install_removes",
                             "install_generates", "file_permissions"}, "build.outputs[]"); !result)
                        return std::unexpected(result.error());
                    auto name = (*item)["name"].value<std::string_view>();
                    if (!name || name->empty()) return std::unexpected(
                        "build.outputs entries require a non-empty name");
                    InstallOutput output;
                    output.name = std::string(*name);
                    auto parse_output_string = [&](const char* key,
                                                   std::optional<std::string>& target)
                        -> std::expected<void, std::string> {
                        if (!item->contains(key)) return {};
                        auto value = (*item)[key].value<std::string_view>();
                        if (!value) return std::unexpected(std::format(
                            "build.outputs.{} must be a string", key));
                        target = std::string(*value);
                        return {};
                    };
                    if (auto result = parse_output_string("description",
                            output.description); !result)
                        return std::unexpected(result.error());
                    if (auto result = parse_output_string("license",
                            output.license); !result)
                        return std::unexpected(result.error());
                    if (auto result = parse_output_string("version",
                            output.version); !result)
                        return std::unexpected(result.error());
                    if (auto result = parse_output_string("release",
                            output.release); !result)
                        return std::unexpected(result.error());
                    if (auto result = parse_output_string("channel",
                            output.channel); !result)
                        return std::unexpected(result.error());
                    if (auto result = parse_output_string("arch",
                            output.arch); !result)
                        return std::unexpected(result.error());
                    if (auto result = parse_string_array_strict(*item, "inherit",
                            "build.outputs[]", output.inherit); !result)
                        return std::unexpected(result.error());
                    if (auto result = parse_string_array_strict(*item, "install_files",
                            "build.outputs[]", output.install_files); !result)
                        return std::unexpected(result.error());
                    if (auto result = parse_string_array_strict(*item, "install_excludes",
                            "build.outputs[]", output.install_excludes); !result)
                        return std::unexpected(result.error());
                    if (auto result = parse_string_array_strict(*item, "optional_excludes",
                            "build.outputs[]", output.optional_excludes); !result)
                        return std::unexpected(result.error());
                    if (auto result = parse_copy_entries(*item, "install_copies",
                            "build.outputs[]", output.install_copies); !result)
                        return std::unexpected(result.error());
                    if (auto result = parse_symlink_entries(*item, "install_symlinks",
                            "build.outputs[]", output.install_symlinks); !result)
                        return std::unexpected(result.error());
                    if (auto result = parse_move_entries(*item, "install_moves",
                            "build.outputs[]", output.install_moves); !result)
                        return std::unexpected(result.error());
                    if (auto result = parse_remove_entries(*item, "install_removes",
                            "build.outputs[]", output.install_removes); !result)
                        return std::unexpected(result.error());
                    if (auto result = parse_generate_entries(*item, "install_generates",
                            "build.outputs[]", output.install_generates); !result)
                        return std::unexpected(result.error());
                    if (auto result = parse_file_permission_entries(*item, "file_permissions",
                            "build.outputs[]", output.file_permissions); !result)
                        return std::unexpected(result.error());
                    if (item->contains("dependencies")) {
                        output.dependencies.emplace();
                        if (auto result = parse_deps_strict(*item, "dependencies",
                                "build.outputs[]", *output.dependencies); !result)
                            return std::unexpected(result.error());
                    }
                    if (item->contains("conflicts")) {
                        output.conflicts.emplace();
                        if (auto result = parse_deps_strict(*item, "conflicts",
                                "build.outputs[]", *output.conflicts); !result)
                            return std::unexpected(result.error());
                    }
                    if (item->contains("provides")) {
                        output.provides.emplace();
                        if (auto result = parse_string_array_strict(*item, "provides",
                                "build.outputs[]", *output.provides); !result)
                            return std::unexpected(result.error());
                    }
                    if (item->contains("conffiles")) {
                        output.conffiles.emplace();
                        if (auto result = parse_string_array_strict(*item, "conffiles",
                                "build.outputs[]", *output.conffiles); !result)
                            return std::unexpected(result.error());
                    }
                    r.managed_build.outputs.push_back(std::move(output));
                }
            }
            if (bld->contains("outputs") && !bld->get_as<vendor::toml::array>("outputs"))
                return std::unexpected("build.outputs must be an array");
            if (r.managed_build.system == BuildSystem::Script
                && r.managed_build.install_files.empty()
                && r.managed_build.outputs.empty())
                return std::unexpected(
                    "Script recipes require an explicit install_files boundary or outputs");
            if (r.managed_build.system == BuildSystem::Script
                && r.managed_build.payload == PayloadMode::All)
                return std::unexpected(
                    "Script recipes cannot use build.payload=all");
            if (bld->contains("steps") && !bld->get_as<vendor::toml::array>("steps"))
                return std::unexpected("build.steps must be an array");
            if (auto* arr = bld->get_as<vendor::toml::array>("steps")) {
                static constexpr std::array<std::string_view, 7> phases{
                    "prepare", "pre-build", "post-build", "check",
                    "pre-install", "install", "post-install"};
                static constexpr std::array<std::string_view, 3> directories{
                    "source", "build", "package"};
                for (auto&& element : *arr) {
                    auto* item = element.as_table();
                    if (!item) return std::unexpected(
                        "build.steps entries must be inline tables");
                    if (auto result = reject_unknown(*item,
                            {"name", "phase", "cwd", "command", "unsafe_shell"},
                            "build.steps[]"); !result)
                        return std::unexpected(result.error());
                    auto name = (*item)["name"].value<std::string_view>();
                    auto phase = (*item)["phase"].value<std::string_view>();
                    auto cwd = (*item)["cwd"].value<std::string_view>().value_or("source");
                    auto command = (*item)["command"].value<std::string_view>();
                    if (!name || !phase || !command || name->empty()
                        || phase->empty() || command->empty())
                        return std::unexpected(
                            "build.steps entries require name, phase and command");
                    if (!std::ranges::contains(phases, *phase))
                        return std::unexpected(std::format(
                            "build.steps has unsupported phase '{}'; expected prepare, pre-build, post-build, check, pre-install, install or post-install",
                            *phase));
                    if (!std::ranges::contains(directories, cwd))
                        return std::unexpected(std::format(
                            "build.steps '{}' has unsupported cwd '{}'; expected source, build or package",
                            *name, cwd));
                    bool unsafe_shell = false;
                    if (item->contains("unsafe_shell")) {
                        auto us = (*item)["unsafe_shell"].value<bool>();
                        if (!us) return std::unexpected("build.steps.unsafe_shell must be a boolean");
                        unsafe_shell = *us;
                    }
                    r.managed_build.steps.push_back({
                        .name = std::string(*name),
                        .phase = std::string(*phase),
                        .cwd = std::string(cwd),
                        .command = std::string(*command),
                        .unsafe_shell = unsafe_shell});
                }
            }
            const bool has_check_step = std::ranges::any_of(
                r.managed_build.steps,
                [](const ManagedBuildStep& step) { return step.phase == "check"; });
            if (std::ranges::any_of(r.check_deps, [](const std::string& dep) {
                    return dep.empty();
                }))
                return std::unexpected(
                    "package.check_dependencies entries must be non-empty strings");
            if (!r.check_deps.empty() && !has_check_step)
                return std::unexpected(
                    "package.check_dependencies require at least one build.steps phase='check'");
            if (r.managed_build.system == BuildSystem::Script
                && r.managed_build.steps.empty())
                return std::unexpected(
                    "Script recipes require at least one build.steps entry");
            std::set<std::string> patch_names;
            auto add_patch = [&](std::string file, int strip, std::string sha)
                -> std::expected<void, std::string> {
                if (file.empty() || std::filesystem::path(file).filename() != file)
                    return std::unexpected(
                        "build.patches entries require a basename 'file' string");
                if (!patch_names.insert(file).second)
                    return std::unexpected(
                        "build.patches cannot declare the same file more than once: "
                        + file);
                r.managed_build.patches.push_back(file);
                r.managed_build.patches_spec.push_back(PatchSpec{
                    .file = std::move(file),
                    .strip = strip,
                    .sha256 = std::move(sha)});
                return {};
            };
            if (bld->contains("patches")) {
                auto* arr = bld->get_as<vendor::toml::array>("patches");
                if (!arr) return std::unexpected("build.patches must be an array");
                for (const auto& elem : *arr) {
                    if (auto str = elem.value<std::string_view>()) {
                        if (auto result = add_patch(std::string(*str),
                                r.managed_build.patch_strip, {}); !result)
                            return std::unexpected(result.error());
                    } else if (auto* ptbl = elem.as_table()) {
                        if (auto result = reject_unknown(
                                *ptbl, {"file", "strip", "sha256"},
                                "build.patches[]"); !result)
                            return std::unexpected(result.error());
                        auto f = (*ptbl)["file"].value<std::string_view>();
                        if (!f) return std::unexpected(
                            "build.patches entries require a basename 'file' string");
                        int strip = r.managed_build.patch_strip;
                        if (ptbl->contains("strip")) {
                            auto strip_value = (*ptbl)["strip"].value<std::int64_t>();
                            if (!strip_value || *strip_value < 0 || *strip_value > 9)
                                return std::unexpected(
                                    "build.patches entry 'strip' must be between 0 and 9");
                            strip = static_cast<int>(*strip_value);
                        }
                        if (!ptbl->contains("sha256"))
                            return std::unexpected(
                                "structured build.patches entries require sha256");
                        auto sha_value = (*ptbl)["sha256"].value<std::string_view>();
                        if (!sha_value || !valid_sha256(*sha_value))
                            return std::unexpected(
                                "build.patches entry 'sha256' must be a 64-hex SHA-256");
                        if (auto result = add_patch(std::string(*f), strip,
                                normalize_sha256(std::string(*sha_value))); !result)
                            return std::unexpected(result.error());
                    } else {
                        return std::unexpected(
                            "build.patches must be an array of strings or tables");
                    }
                }
            }
            if (bld->contains("patch_checksums")
                && !bld->get_as<vendor::toml::table>("patch_checksums"))
                return std::unexpected("build.patch_checksums must be a table");
            if (auto* checksums = bld->get_as<vendor::toml::table>("patch_checksums")) {
                for (const auto& [name, value] : *checksums) {
                    auto sha = value.value<std::string_view>();
                    if (name.str().empty()
                        || std::filesystem::path(name.str()).filename() != name.str()
                        || !sha || !valid_sha256(*sha))
                        return std::unexpected(
                            "build.patch_checksums entries require a basename and 64-hex SHA-256");
                    r.managed_build.patch_checksums.emplace(
                        name.str(), normalize_sha256(std::string(*sha)));
                }
            }
            for (const auto& [name, _] : r.managed_build.patch_checksums) {
                if (!patch_names.contains(name))
                    return std::unexpected(
                        "build.patch_checksums names an undeclared patch: " + name);
            }
            for (auto& patch : r.managed_build.patches_spec) {
                const auto source = source_hashes.find(patch.file);
                const auto checksum = r.managed_build.patch_checksums.find(patch.file);
                if (source != source_hashes.end() && checksum != r.managed_build.patch_checksums.end()
                    && source->second != checksum->second)
                    return std::unexpected(std::format(
                        "Patch '{}' has conflicting source and patch_checksums SHA-256 declarations",
                        patch.file));
                if (source != source_hashes.end() && !patch.sha256.empty()
                    && source->second != patch.sha256)
                    return std::unexpected(std::format(
                        "Patch '{}' SHA-256 does not match its [[source]] declaration",
                        patch.file));
                if (checksum != r.managed_build.patch_checksums.end()
                    && !patch.sha256.empty() && checksum->second != patch.sha256)
                    return std::unexpected(std::format(
                        "Patch '{}' SHA-256 conflicts with build.patch_checksums",
                        patch.file));
                if (patch.sha256.empty()) {
                    if (checksum != r.managed_build.patch_checksums.end())
                        patch.sha256 = checksum->second;
                    else if (source != source_hashes.end())
                        patch.sha256 = source->second;
                }
                if (patch.sha256.empty())
                    return std::unexpected(
                        "Every build.patches entry requires a SHA-256 declaration");
            }
            if (auto result = parse_string_array_strict(*bld, "allowed_compilers",
                    "build", r.managed_build.allowed_compilers); !result)
                return std::unexpected(result.error());
            if (auto result = parse_string_array_strict(*bld, "allowed_linkers",
                    "build", r.managed_build.allowed_linkers); !result)
                return std::unexpected(result.error());
            if (bld->contains("variables") && !bld->get_as<vendor::toml::table>("variables"))
                return std::unexpected("build.variables must be a table");
            if (auto* vars = bld->get_as<vendor::toml::table>("variables")) {
                for (auto&& [key, value] : *vars) {
                    if (auto s = value.value<std::string_view>())
                        r.managed_build.variables.emplace(std::string(key.str()), *s);
                    else return std::unexpected("build.variables values must be strings");
                }
            }
            if (bld->contains("flag_env") && !bld->get_as<vendor::toml::table>("flag_env"))
                return std::unexpected("build.flag_env must be a table");
            if (auto* flags = bld->get_as<vendor::toml::table>("flag_env")) {
                if (auto result = reject_unknown(*flags,
                        {"cflags", "cxxflags", "cppflags", "ldflags", "rustflags"},
                        "build.flag_env"); !result)
                    return std::unexpected(result.error());
                if (auto result = parse_string_array_strict(*flags, "cflags",
                        "build.flag_env", r.managed_build.cflags_env); !result)
                    return std::unexpected(result.error());
                if (auto result = parse_string_array_strict(*flags, "cxxflags",
                        "build.flag_env", r.managed_build.cxxflags_env); !result)
                    return std::unexpected(result.error());
                if (auto result = parse_string_array_strict(*flags, "cppflags",
                        "build.flag_env", r.managed_build.cppflags_env); !result)
                    return std::unexpected(result.error());
                if (auto result = parse_string_array_strict(*flags, "ldflags",
                        "build.flag_env", r.managed_build.ldflags_env); !result)
                    return std::unexpected(result.error());
                if (auto result = parse_string_array_strict(*flags, "rustflags",
                        "build.flag_env", r.managed_build.rustflags_env); !result)
                    return std::unexpected(result.error());
            }
            if (bld->contains("tool_env") && !bld->get_as<vendor::toml::table>("tool_env"))
                return std::unexpected("build.tool_env must be a table");
            if (auto* tools = bld->get_as<vendor::toml::table>("tool_env")) {
                if (auto result = reject_unknown(*tools, {"cc", "cxx", "linker"},
                        "build.tool_env"); !result)
                    return std::unexpected(result.error());
                if (auto result = parse_string_array_strict(*tools, "cc",
                        "build.tool_env", r.managed_build.cc_env); !result)
                    return std::unexpected(result.error());
                if (auto result = parse_string_array_strict(*tools, "cxx",
                        "build.tool_env", r.managed_build.cxx_env); !result)
                    return std::unexpected(result.error());
                if (auto result = parse_string_array_strict(*tools, "linker",
                        "build.tool_env", r.managed_build.linker_env); !result)
                    return std::unexpected(result.error());
            }

            const auto validate_payload_patterns = [](const std::vector<std::string>& patterns,
                                                       std::string_view field)
                -> std::expected<void, std::string> {
                for (const auto& pattern : patterns) {
                    if (pattern.empty() || pattern.starts_with('/')
                        || std::filesystem::path(pattern).has_root_path()
                        || std::ranges::any_of(std::filesystem::path(pattern),
                            [](const auto& part) { return part == ".."; })) {
                        return std::unexpected(std::format(
                            "build.{} entries must be non-empty relative paths without '..': {}",
                            field, pattern));
                    }
                    if (pattern.starts_with("data/") || pattern == "data") {
                        return std::unexpected(std::format(
                            "build.{} addresses the archive metadata prefix 'data/': {}",
                            field, pattern));
                    }
                }
                return {};
            };
            if (auto result = validate_payload_patterns(r.managed_build.install_files,
                                                         "install_files"); !result)
                return std::unexpected(result.error());
            if (auto result = validate_payload_patterns(r.managed_build.install_excludes,
                                                         "install_excludes"); !result)
                return std::unexpected(result.error());
            if (auto result = validate_payload_patterns(r.managed_build.optional_excludes,
                                                         "optional_excludes"); !result)
                return std::unexpected(result.error());
            const auto validate_payload_path = [](std::string_view path,
                                                  std::string_view field,
                                                  bool allow_absolute) -> std::expected<void, std::string> {
                const std::filesystem::path candidate(path);
                if (path.empty() || (!allow_absolute && candidate.is_absolute())
                    || std::ranges::any_of(candidate,
                        [](const auto& part) { return part == ".."; })) {
                    return std::unexpected(std::format(
                        "build.{} path must stay within the source/staging root: {}",
                        field, path));
                }
                return {};
            };
            for (const auto& perm : r.managed_build.file_permissions) {
                if (auto result = validate_payload_path(perm.path, "file_permissions.path", false); !result)
                    return std::unexpected(result.error());
            }
            for (const auto& copy : r.managed_build.install_copies) {
                if (auto result = validate_payload_path(copy.source, "install_copies.from", false);
                    !result) return std::unexpected(result.error());
                if (auto result = validate_payload_path(copy.destination,
                        "install_copies.to", false); !result)
                    return std::unexpected(result.error());
            }
            for (const auto& move : r.managed_build.install_moves) {
                if (auto result = validate_payload_path(move.source,
                        "install_moves.from", false); !result)
                    return std::unexpected(result.error());
                if (auto result = validate_payload_path(move.destination,
                        "install_moves.to", false); !result)
                    return std::unexpected(result.error());
            }
            for (const auto& remove : r.managed_build.install_removes) {
                if (auto result = validate_payload_patterns({remove.path},
                        "install_removes"); !result)
                    return std::unexpected(result.error());
            }
            for (const auto& generate : r.managed_build.install_generates) {
                if (auto result = validate_payload_path(generate.path,
                        "install_generates.path", false); !result)
                    return std::unexpected(result.error());
            }
            std::set<std::string> output_names;
            for (const auto& output : r.managed_build.outputs) {
                if (!output_names.insert(output.name).second)
                    return std::unexpected("build.outputs contains duplicate name: "
                                           + output.name);
                if (output.name.find('/') != std::string::npos
                    || output.name == "." || output.name == "..")
                    return std::unexpected("build.outputs names must be simple package names");
                if (auto result = validate_payload_patterns(output.install_files,
                        "outputs.install_files"); !result)
                    return std::unexpected(result.error());
                if (auto result = validate_payload_patterns(output.install_excludes,
                        "outputs.install_excludes"); !result)
                    return std::unexpected(result.error());
                if (auto result = validate_payload_patterns(output.optional_excludes,
                        "outputs.optional_excludes"); !result)
                    return std::unexpected(result.error());
                if (output.install_files.empty())
                    return std::unexpected("build.outputs entries require install_files");
                for (const auto& copy : output.install_copies) {
                    if (auto result = validate_payload_path(copy.source, "outputs.install_copies.from", false); !result)
                        return std::unexpected(result.error());
                    if (auto result = validate_payload_path(copy.destination, "outputs.install_copies.to", false); !result)
                        return std::unexpected(result.error());
                }
                for (const auto& move : output.install_moves) {
                    if (auto result = validate_payload_path(move.source, "outputs.install_moves.from", false); !result)
                        return std::unexpected(result.error());
                    if (auto result = validate_payload_path(move.destination, "outputs.install_moves.to", false); !result)
                        return std::unexpected(result.error());
                }
                for (const auto& remove : output.install_removes) {
                    if (auto result = validate_payload_patterns({remove.path}, "outputs.install_removes"); !result)
                        return std::unexpected(result.error());
                }
                for (const auto& generate : output.install_generates) {
                    if (auto result = validate_payload_path(generate.path, "outputs.install_generates.path", false); !result)
                        return std::unexpected(result.error());
                }
                for (const auto& perm : output.file_permissions) {
                    if (auto result = validate_payload_path(perm.path, "outputs.file_permissions.path", false); !result)
                        return std::unexpected(result.error());
                }
                for (const auto& link : output.install_symlinks) {
                    if (auto result = validate_payload_path(link.path, "outputs.install_symlinks.path", false); !result)
                        return std::unexpected(result.error());
                    const std::filesystem::path target(link.target);
                    const auto parent = std::filesystem::path(link.path).parent_path();
                    const auto resolved = (parent / target).lexically_normal();
                    if (link.target.empty() || target.is_absolute() || target.has_root_path()
                        || resolved.is_absolute()
                        || std::ranges::any_of(resolved, [](const auto& part) {
                            return part == "..";
                        }))
                        return std::unexpected(std::format(
                            "build.outputs.install_symlinks.target must remain inside the staging root: {}",
                            link.target));
                }
            }
            std::set<std::string> step_names;
            for (const auto& step : r.managed_build.steps) {
                if (!step_names.insert(step.name).second)
                    return std::unexpected("build.steps contains duplicate name: "
                                           + step.name);
                if (step.command.find('\0') != std::string::npos)
                    return std::unexpected("build.steps command contains NUL: "
                                           + step.name);
            }
            if (!r.managed_build.outputs.empty()
                && (!r.managed_build.install_files.empty()
                    || !r.managed_build.install_excludes.empty()))
                return std::unexpected(
                    "build.outputs and top-level install_files/install_excludes are mutually exclusive");
            if (r.managed_build.payload == PayloadMode::All
                && (!r.managed_build.install_files.empty()
                    || !r.managed_build.install_excludes.empty()
                    || !r.managed_build.outputs.empty()))
                return std::unexpected(
                    "build.payload=all cannot be combined with an allowlist or outputs");
            if (r.managed_build.payload == PayloadMode::Allowlist
                && r.managed_build.install_files.empty())
                return std::unexpected(
                    "build.payload=allowlist requires non-empty build.install_files");
            if (r.managed_build.payload == PayloadMode::Outputs
                && r.managed_build.outputs.empty())
                return std::unexpected(
                    "build.payload=outputs requires at least one build.outputs entry");
            if (!r.managed_build.outputs.empty()
                && r.managed_build.payload != PayloadMode::Outputs)
                return std::unexpected(
                    "build.outputs requires build.payload=outputs");
            for (const auto& link : r.managed_build.install_symlinks) {
                if (auto result = validate_payload_path(link.path,
                        "install_symlinks.path", false); !result)
                    return std::unexpected(result.error());
                const std::filesystem::path target(link.target);
                const auto parent = std::filesystem::path(link.path).parent_path();
                const auto resolved = (parent / target).lexically_normal();
                if (link.target.empty() || target.is_absolute() || target.has_root_path()
                    || resolved.is_absolute()
                    || std::ranges::any_of(resolved, [](const auto& part) {
                        return part == "..";
                    }))
                    return std::unexpected(std::format(
                        "build.install_symlinks.target must remain inside the staging root: {}",
                        link.target));
            }
            if ((r.name.ends_with("-libs") || r.name.ends_with("-dev"))
                && r.managed_build.payload != PayloadMode::Outputs
                && r.managed_build.install_files.empty()) {
                return std::unexpected(std::format(
                    "Recipe v2 split package '{}' must declare build.install_files; "
                    "the backend install tree is not an implicit package boundary",
                    r.name));
            }
            if (bld->contains("toolchain") && !bld->get_as<vendor::toml::table>("toolchain"))
                return std::unexpected("build.toolchain must be a table");
            if (auto* suite = bld->get_as<vendor::toml::table>("toolchain")) {
                if (auto result = reject_unknown(*suite,
                        {"compiler", "linker", "rust"}, "build.toolchain"); !result)
                    return std::unexpected(result.error());
                auto parse_tool = [&](std::string_view kind,
                        ToolRequirement& requirement,
                                      std::initializer_list<std::string_view> families)
                    -> std::expected<void, std::string> {
                    if (suite->contains(kind)
                        && !suite->get_as<vendor::toml::table>(kind))
                        return std::unexpected(std::format(
                            "build.toolchain.{} must be a table", kind));
                    auto* tool = suite->get_as<vendor::toml::table>(kind);
                    if (!tool) return {};
                    if (auto result = reject_unknown(*tool,
                            {"family", "package", "minimum_version"},
                            std::string("build.toolchain.") + std::string(kind)); !result)
                        return std::unexpected(result.error());
                    requirement.family = (*tool)["family"].value_or("");
                    requirement.package = (*tool)["package"].value_or("");
                    requirement.minimum_version =
                        (*tool)["minimum_version"].value_or("");
                    if (requirement.family.empty() || requirement.package.empty()
                        || requirement.minimum_version.empty()) {
                        return std::unexpected(std::format(
                            "build.toolchain.{} requires family, package and minimum_version",
                            kind));
                    }
                    if (!std::ranges::contains(families, requirement.family)) {
                        return std::unexpected(std::format(
                            "Unsupported build.toolchain.{} family '{}'", kind,
                            requirement.family));
                    }
                    return {};
                };
                if (auto result = parse_tool("compiler", r.managed_build.compiler,
                        {"clang", "gcc"}); !result)
                    return std::unexpected(result.error());
                if (auto result = parse_tool("linker", r.managed_build.linker,
                        {"lld", "mold", "ld"}); !result)
                    return std::unexpected(result.error());
                if (suite->contains("rust")) {
                    if (r.managed_build.system != BuildSystem::Cargo)
                        return std::unexpected(
                            "build.toolchain.rust is valid only for Cargo recipes");
                    if (auto result = parse_tool("rust", r.managed_build.rust,
                            {"rustc"}); !result)
                        return std::unexpected(result.error());
                }

                if (!r.managed_build.compiler.family.empty()
                    && !r.managed_build.allowed_compilers.empty()
                    && !std::ranges::contains(r.managed_build.allowed_compilers,
                                              r.managed_build.compiler.family))
                    return std::unexpected(
                        "Default compiler family is absent from allowed_compilers");
                if (!r.managed_build.linker.family.empty()
                    && !r.managed_build.allowed_linkers.empty()
                    && !std::ranges::contains(r.managed_build.allowed_linkers,
                                              r.managed_build.linker.family))
                    return std::unexpected(
                        "Default linker family is absent from allowed_linkers");

                auto require_package = [&](const ToolRequirement& requirement) {
                    const auto minimum = Version::parse(requirement.minimum_version);
                    bool found = false;
                    for (const auto& raw : r.build_deps) {
                        const auto dependency = Dependency::parse(raw);
                        if (dependency.name != requirement.package) continue;
                        found = true;
                        const bool strong_enough =
                            (dependency.op == ConstraintOp::Equal
                                || dependency.op == ConstraintOp::GreaterEqual)
                            && dependency.version >= minimum;
                        if (!strong_enough) return std::expected<void, std::string>(
                            std::unexpected(std::format(
                                "build dependency '{}' does not guarantee toolchain minimum {} >= {}",
                                raw, requirement.package, requirement.minimum_version)));
                    }
                    if (!found) r.build_deps.push_back(std::format(
                        "{} >= {}", requirement.package,
                        requirement.minimum_version));
                    return std::expected<void, std::string>{};
                };
                if (!r.managed_build.compiler.family.empty()) {
                    if (auto result = require_package(r.managed_build.compiler); !result)
                        return std::unexpected(result.error());
                }
                if (!r.managed_build.linker.family.empty()) {
                    if (auto result = require_package(r.managed_build.linker); !result)
                        return std::unexpected(result.error());
                }
                if (!r.managed_build.rust.family.empty()) {
                    if (auto result = require_package(r.managed_build.rust); !result)
                        return std::unexpected(result.error());
                }
            }
        }

        parse_capability_hooks(tbl, r.capability_hooks);
        if (auto trig_res = parse_triggers(tbl, r.triggers); !trig_res) {
            return std::unexpected(trig_res.error());
        }

        return r;
    }
};

} // namespace sage::package
