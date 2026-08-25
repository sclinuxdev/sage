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
    // Present-but-empty means "inject nothing" (kernel-style recipes whose
    // link line ignores LDFLAGS must not inherit a false provenance stamp).
    std::optional<std::string> ldflags;
    // Env channels the recipe itself forwards flags through -- the kernel's
    // KCFLAGS=/KAFLAGS= passthrough and kin. Purely an annotation: stamped
    // onto the manifest so build_cflags equality with artifact-verified
    // switches is explainable instead of suspicious.
    std::vector<std::string> passthrough_flags;
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
            auto architecture = validate_package_architecture(r.arch);
            if (!architecture) return std::unexpected(architecture.error());
        } else {
            return std::unexpected("Missing [package] section in recipe");
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

        extract_cmds("prepare", r.prepare_cmds);
        extract_cmds("build", r.build_cmds);
        extract_cmds("install", r.install_cmds);

        // Flag overrides are presence-respecting: an explicit `cflags = ""`
        // clears the baseline rather than falling back to it. A non-empty
        // `cc` pins the compiler pair outright.
        if (auto* bld = tbl.get_as<vendor::toml::table>("build")) {
            if (auto v = (*bld)["cflags"].value<std::string_view>()) r.cflags = std::string(*v);
            if (auto* passthrough = bld->get_as<vendor::toml::array>("passthrough_flags")) {
                for (auto&& el : *passthrough) {
                    if (auto s = el.value<std::string_view>(); s && !s->empty()) {
                        r.passthrough_flags.emplace_back(*s);
                    }
                }
            }
            if (auto v = (*bld)["cxxflags"].value<std::string_view>()) r.cxxflags = std::string(*v);
            if (auto v = (*bld)["cc"].value<std::string_view>()) r.cc = std::string(*v);
            if (auto v = (*bld)["cxx"].value<std::string_view>()) r.cxx = std::string(*v);
            if (auto v = (*bld)["ldflags"].value<std::string_view>()) r.ldflags = std::string(*v);
        }

        parse_capability_hooks(tbl, r.capability_hooks);
        if (auto trig_res = parse_triggers(tbl, r.triggers); !trig_res) {
            return std::unexpected(trig_res.error());
        }

        return r;
    }
};

} // namespace sage::package
