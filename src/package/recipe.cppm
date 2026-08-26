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
};

// One output is a named view of the common DESTDIR.  Sage still emits one
// archive per recipe invocation; output names let a recipe describe the
// split-package boundary once and let the build driver select an output with
// `--output` in a later phase.  The default output is the recipe package.
struct InstallOutput {
    std::string name;
    std::vector<std::string> install_files;
    std::vector<std::string> install_excludes;
};

struct ManagedBuildSpec {
    BuildSystem system{BuildSystem::Legacy};
    std::string source_subdir;
    // Empty means in-tree for Autotools/Make/Xmake/Cargo. CMake and Meson
    // receive a backend-specific `build` default in the parser below.
    std::string build_dir;
    std::vector<std::string> configure_options;
    std::vector<std::string> build_targets;
    std::vector<std::string> install_targets;
    // Paths that may survive the backend's install step.  These are package
    // payload paths relative to the staging root (for example
    // `usr/lib/libfoo.so.*`), not DESTDIR-prefixed filesystem paths.  An
    // empty list preserves the historical "all installed files" behaviour;
    // split `-libs`/`-dev` recipes are required to spell this allowlist out.
    std::vector<std::string> install_files;
    // Optional path globs removed after the allowlist is applied.  Excludes
    // are useful for a package that takes most of an upstream install but
    // deliberately leaves a sub-tree to a sibling package.
    std::vector<std::string> install_excludes;
    std::vector<InstallCopy> install_copies;
    std::vector<InstallSymlink> install_symlinks;
    std::vector<InstallMove> install_moves;
    std::vector<InstallRemove> install_removes;
    std::vector<InstallGenerate> install_generates;
    std::vector<InstallOutput> outputs;
    std::vector<ManagedBuildStep> steps;
    std::vector<std::string> patches;
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
    std::vector<std::string> build_deps;
    std::vector<Dependency> host_deps;
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

        Recipe r;
        r.schema_version = static_cast<uint32_t>(tbl["schema_version"].value_or(1LL));
        if (r.schema_version != 1 && r.schema_version != 2) {
            return std::unexpected(std::format(
                "Unsupported recipe schema_version {}; expected 1 or 2",
                r.schema_version));
        }

        if (auto* pkg = tbl.get_as<vendor::toml::table>("package")) {
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

        if (auto* upstream = tbl.get_as<vendor::toml::table>("upstream")) {
            r.upstream.url = (*upstream)["url"].value_or(r.upstream.url);
            r.upstream.version_regex =
                (*upstream)["version_regex"].value_or(r.upstream.version_regex);
        }
        if (r.upstream.url.empty() != r.upstream.version_regex.empty()) {
            return std::unexpected(
                "Upstream tracking requires both url and version_regex");
        }

        // [source] is either one table (a single download) or an array of
        // tables (`[[source]]`): the first element is the primary archive that
        // gets unpacked to src/, any further elements are extra downloads.
        // TOML keys written after a [[source]] block land inside the last
        // element, so every scope-sensitive collector below must see all of
        // them -- src_scopes is the shared third layer.
        std::vector<const vendor::toml::table*> src_scopes;
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

        parse_deps(tbl, "dependencies", r.host_deps);
        parse_strings(tbl, "build_dependencies", r.build_deps);
        parse_strings(tbl, "provides", r.provides);
        parse_strings(tbl, "conffiles", r.conffiles);

        if (auto* pkg = tbl.get_as<vendor::toml::table>("package")) {
            parse_deps(*pkg, "dependencies", r.host_deps);
            parse_strings(*pkg, "build_dependencies", r.build_deps);
            parse_strings(*pkg, "provides", r.provides);
            parse_strings(*pkg, "conffiles", r.conffiles);
        }

        for (const auto* src : src_scopes) {
            parse_deps(*src, "dependencies", r.host_deps);
            parse_strings(*src, "build_dependencies", r.build_deps);
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
            r.managed_build.source_subdir = (*bld)["source_subdir"].value_or("");
            r.managed_build.build_dir = (*bld)["build_dir"].value_or(
                *system == BuildSystem::CMake || *system == BuildSystem::Meson
                    ? "build" : "");
            r.managed_build.patch_strip = static_cast<int>((*bld)["patch_strip"].value_or(1LL));
            if (r.managed_build.patch_strip < 0 || r.managed_build.patch_strip > 9)
                return std::unexpected("build.patch_strip must be between 0 and 9");
            parse_strings(*bld, "configure_options", r.managed_build.configure_options);
            parse_strings(*bld, "build_targets", r.managed_build.build_targets);
            parse_strings(*bld, "install_targets", r.managed_build.install_targets);
            parse_strings(*bld, "install_files", r.managed_build.install_files);
            parse_strings(*bld, "install_excludes", r.managed_build.install_excludes);
            auto parse_copy_entries = [&](const char* key,
                                          std::vector<InstallCopy>& target)
                -> std::expected<void, std::string> {
                if (auto* arr = bld->get_as<vendor::toml::array>(key)) {
                    for (auto&& element : *arr) {
                        auto* item = element.as_table();
                        if (!item) return std::unexpected(std::format(
                            "build.{} entries must be inline tables", key));
                        auto source = (*item)["from"].value<std::string_view>();
                        auto destination = (*item)["to"].value<std::string_view>();
                        if (!source || !destination || source->empty()
                            || destination->empty()) return std::unexpected(std::format(
                                "build.{} entries require non-empty from and to", key));
                        target.push_back({std::string(*source), std::string(*destination)});
                    }
                }
                return {};
            };
            auto parse_symlink_entries = [&](const char* key,
                                             std::vector<InstallSymlink>& target)
                -> std::expected<void, std::string> {
                if (auto* arr = bld->get_as<vendor::toml::array>(key)) {
                    for (auto&& element : *arr) {
                        auto* item = element.as_table();
                        if (!item) return std::unexpected(std::format(
                            "build.{} entries must be inline tables", key));
                        auto path = (*item)["path"].value<std::string_view>();
                        auto target_path = (*item)["target"].value<std::string_view>();
                        if (!path || !target_path || path->empty()
                            || target_path->empty()) return std::unexpected(std::format(
                                "build.{} entries require non-empty path and target", key));
                        target.push_back({std::string(*path), std::string(*target_path)});
                    }
                }
                return {};
            };
            if (auto result = parse_copy_entries("install_copies",
                                                 r.managed_build.install_copies); !result)
                return std::unexpected(result.error());
            if (auto result = parse_symlink_entries("install_symlinks",
                                                    r.managed_build.install_symlinks); !result)
                return std::unexpected(result.error());
            auto parse_move_entries = [&](const char* key,
                                          std::vector<InstallMove>& target)
                -> std::expected<void, std::string> {
                if (auto* arr = bld->get_as<vendor::toml::array>(key)) {
                    for (auto&& element : *arr) {
                        auto* item = element.as_table();
                        if (!item) return std::unexpected(std::format(
                            "build.{} entries must be inline tables", key));
                        auto source = (*item)["from"].value<std::string_view>();
                        auto destination = (*item)["to"].value<std::string_view>();
                        if (!source || !destination || source->empty()
                            || destination->empty()) return std::unexpected(std::format(
                            "build.{} entries require non-empty from and to", key));
                        target.push_back({std::string(*source), std::string(*destination)});
                    }
                }
                return {};
            };
            auto parse_remove_entries = [&](const char* key,
                                            std::vector<InstallRemove>& target)
                -> std::expected<void, std::string> {
                if (auto* arr = bld->get_as<vendor::toml::array>(key)) {
                    for (auto&& element : *arr) {
                        auto path = element.value<std::string_view>();
                        if (!path || path->empty()) return std::unexpected(std::format(
                            "build.{} entries must be non-empty strings", key));
                        target.push_back({std::string(*path)});
                    }
                }
                return {};
            };
            if (auto result = parse_move_entries("install_moves",
                                                r.managed_build.install_moves); !result)
                return std::unexpected(result.error());
            if (auto result = parse_remove_entries("install_removes",
                                                  r.managed_build.install_removes); !result)
                return std::unexpected(result.error());
            if (auto* arr = bld->get_as<vendor::toml::array>("install_generates")) {
                for (auto&& element : *arr) {
                    auto* item = element.as_table();
                    if (!item) return std::unexpected(
                        "build.install_generates entries must be inline tables");
                    auto path = (*item)["path"].value<std::string_view>();
                    auto content = (*item)["content"].value<std::string_view>();
                    auto mode = (*item)["mode"].value<std::int64_t>().value_or(0644);
                    if (!path || !content || path->empty() || mode < 0 || mode > 07777)
                        return std::unexpected(
                            "build.install_generates entries require path/content and a valid mode");
                    r.managed_build.install_generates.push_back({
                        std::string(*path), std::string(*content), static_cast<uint32_t>(mode)});
                }
            }
            if (auto* arr = bld->get_as<vendor::toml::array>("outputs")) {
                for (auto&& element : *arr) {
                    auto* item = element.as_table();
                    if (!item) return std::unexpected(
                        "build.outputs entries must be inline tables");
                    auto name = (*item)["name"].value<std::string_view>();
                    if (!name || name->empty()) return std::unexpected(
                        "build.outputs entries require a non-empty name");
                    InstallOutput output{.name = std::string(*name)};
                    if (auto* files = item->get_as<vendor::toml::array>("install_files"))
                        for (auto&& file : *files)
                            if (auto value = file.value<std::string_view>())
                                output.install_files.emplace_back(*value);
                    if (auto* excludes = item->get_as<vendor::toml::array>("install_excludes"))
                        for (auto&& exclude : *excludes)
                            if (auto value = exclude.value<std::string_view>())
                                output.install_excludes.emplace_back(*value);
                    r.managed_build.outputs.push_back(std::move(output));
                }
            }
            if (r.managed_build.system == BuildSystem::Script
                && r.managed_build.install_files.empty()
                && r.managed_build.outputs.empty())
                return std::unexpected(
                    "Script recipes require an explicit install_files boundary or outputs");
            if (auto* arr = bld->get_as<vendor::toml::array>("steps")) {
                static constexpr std::array<std::string_view, 6> phases{
                    "prepare", "pre-build", "post-build", "pre-install",
                    "install", "post-install"};
                static constexpr std::array<std::string_view, 3> directories{
                    "source", "build", "package"};
                for (auto&& element : *arr) {
                    auto* item = element.as_table();
                    if (!item) return std::unexpected(
                        "build.steps entries must be inline tables");
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
                            "build.steps has unsupported phase '{}'; expected prepare, pre-build, post-build, pre-install, install or post-install",
                            *phase));
                    if (!std::ranges::contains(directories, cwd))
                        return std::unexpected(std::format(
                            "build.steps '{}' has unsupported cwd '{}'; expected source, build or package",
                            *name, cwd));
                    r.managed_build.steps.push_back({
                        .name = std::string(*name),
                        .phase = std::string(*phase),
                        .cwd = std::string(cwd),
                        .command = std::string(*command)});
                }
            }
            if (r.managed_build.system == BuildSystem::Script
                && r.managed_build.steps.empty())
                return std::unexpected(
                    "Script recipes require at least one build.steps entry");
            parse_strings(*bld, "patches", r.managed_build.patches);
            parse_strings(*bld, "allowed_compilers", r.managed_build.allowed_compilers);
            parse_strings(*bld, "allowed_linkers", r.managed_build.allowed_linkers);
            if (auto* vars = bld->get_as<vendor::toml::table>("variables")) {
                for (auto&& [key, value] : *vars) {
                    if (auto s = value.value<std::string_view>())
                        r.managed_build.variables.emplace(std::string(key.str()), *s);
                    else return std::unexpected("build.variables values must be strings");
                }
            }
            if (auto* flags = bld->get_as<vendor::toml::table>("flag_env")) {
                parse_strings(*flags, "cflags", r.managed_build.cflags_env);
                parse_strings(*flags, "cxxflags", r.managed_build.cxxflags_env);
                parse_strings(*flags, "cppflags", r.managed_build.cppflags_env);
                parse_strings(*flags, "ldflags", r.managed_build.ldflags_env);
                parse_strings(*flags, "rustflags", r.managed_build.rustflags_env);
            }
            if (auto* tools = bld->get_as<vendor::toml::table>("tool_env")) {
                parse_strings(*tools, "cc", r.managed_build.cc_env);
                parse_strings(*tools, "cxx", r.managed_build.cxx_env);
                parse_strings(*tools, "linker", r.managed_build.linker_env);
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
                if (output.install_files.empty())
                    return std::unexpected("build.outputs entries require install_files");
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
                && r.managed_build.install_files.empty()) {
                return std::unexpected(std::format(
                    "Recipe v2 split package '{}' must declare build.install_files; "
                    "the backend install tree is not an implicit package boundary",
                    r.name));
            }
            if (auto* suite = bld->get_as<vendor::toml::table>("toolchain")) {
                auto parse_tool = [&](std::string_view kind,
                                      ToolRequirement& requirement,
                                      std::initializer_list<std::string_view> families)
                    -> std::expected<void, std::string> {
                    auto* tool = suite->get_as<vendor::toml::table>(kind);
                    if (!tool) return {};
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
