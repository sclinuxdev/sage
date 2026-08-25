export module sage.package:manifest;

import std;
import sage.vendor.toml;
import :version;
import :deps;
import :trigger;

export namespace sage::package {

using std::uint32_t;
using std::uint64_t;

// ============================================================================
// File Entry in Package Manifest
// ============================================================================

enum class FileType {
    Regular,
    Directory,
    Symlink
};

struct FileEntry {
    std::string path; // Clean relative path (e.g. "usr/bin/rg")
    uint64_t size{0};
    uint32_t mode{0644};
    std::string sha256;
    FileType type{FileType::Regular};
    std::string link_target;
};

inline std::string_view to_string(FileType t) noexcept {
    switch (t) {
        case FileType::Directory: return "dir";
        case FileType::Symlink:   return "link";
        default:                  return "file";
    }
}

inline FileType parse_file_type(std::string_view s) noexcept {
    if (s == "dir")  return FileType::Directory;
    if (s == "link") return FileType::Symlink;
    return FileType::Regular;
}

struct PackageManifest {
    uint32_t schema_version{1};
    std::string name;
    Version version;
    std::string description;
    std::string license;
    std::string channel{"system"};
    std::string arch{"x86_64"};
    uint64_t installed_size{0};
    std::string file;  // Relative path in repository (e.g. "acl/acl-2.4.0-2-x86_64.pkg.tar.zst")

    // Build provenance, stamped by cmd_build after a successful build.
    // build_compiler(_version): the primary producer by precedence, as the
    // artifacts themselves identify it. build_cflags/cxxflags/ldflags: the
    // environment the phase shells were invoked with (a true fact; some
    // build systems ignore CFLAGS yet consume the same string through their
    // own channel -- kernel KCFLAGS -- so this layer never claims
    // consumption). Rides along through serialization -- which is also the
    // LMDB record and the in-archive .METADATA/manifest.toml -- and is
    // republished by the repository index. All empty for packages built
    // before this existed.
    std::string build_compiler;
    std::string build_compiler_version;
    std::string build_cflags;
    std::string build_cxxflags;
    std::string build_ldflags;
    // 实际传给 rustc 的 RUSTFLAGS（仅 rust 产物且本地真实编译时非空）。
    std::string build_rustflags;

    // Every compiler family the artifacts name -- mixed rust+C++ builds get
    // one entry each. flags = switches artifact-verified from DWARF
    // DW_AT_producer strings (empty when the build recorded none: honesty
    // over completeness); versions as distinct strings when several objects
    // disagree. Empty vector for pre-0.2.1 packages.
    struct BuildProducer {
        std::string name;
        std::vector<std::string> versions;
        std::string flags;
    };
    std::vector<BuildProducer> build_producers;
    // Env channels the recipe itself forwarded flags through (kernel
    // KCFLAGS/KAFLAGS and kin). Annotation only: it explains why the injected
    // build_cflags may equal the artifact-verified producer switches even
    // when a build system nominally ignores CFLAGS.
    std::vector<std::string> build_flag_passthrough;

    // Raw universal service definition (a full service.toml document) for
    // daemon packages: `sage rebuild` parses it and regenerates native
    // scripts for whichever init system is active. Empty for non-daemons.
    std::string service_toml;

    std::vector<Dependency> dependencies;
    std::vector<std::string> provides; // e.g. "virtual/init", "so:libz.so.1"
    // Configuration files the package ships but does not own outright: on
    // upgrade, a locally modified conffile is kept and the new payload lands
    // beside it as "<path>.new".
    std::vector<std::string> conffiles;
    std::vector<Dependency> conflicts;
    std::vector<FileEntry> files;
    std::vector<CapabilityHook> capability_hooks;
    std::vector<Trigger> triggers;

    // The hook this package publishes for `cap`, if any.
    [[nodiscard]] const CapabilityHook* hook_for(std::string_view cap) const {
        for (const auto& h : capability_hooks) {
            if (h.capability == cap) return &h;
        }
        return nullptr;
    }

    [[nodiscard]] bool provides_capability(std::string_view cap) const {
        return std::ranges::find(provides, cap) != provides.end();
    }

    static std::expected<PackageManifest, std::string> parse_toml(std::string_view toml_content) {
        auto tbl_res = vendor::toml::parse_string(toml_content);
        if (!tbl_res) return std::unexpected(tbl_res.error());
        const auto& tbl = *tbl_res;

        PackageManifest m;
        m.schema_version = static_cast<uint32_t>(tbl["schema_version"].value_or(1LL));

        if (auto* pkg = tbl.get_as<vendor::toml::table>("package")) {
            m.name = (*pkg)["name"].value_or("");
            std::string ver_str = std::string((*pkg)["version"].value_or(""));
            m.version = Version::parse(ver_str);
            auto release = parse_release_field(*pkg, m.version.rel);
            if (!release) return std::unexpected(release.error());
            m.version.rel = std::move(*release);
            if (auto epoch = (*pkg)["epoch"].value<long long>()) {
                if (*epoch < 0
                    || static_cast<unsigned long long>(*epoch)
                        > std::numeric_limits<uint32_t>::max()) {
                    return std::unexpected("Package epoch is outside the uint32 range");
                }
                m.version.epoch = static_cast<uint32_t>(*epoch);
            }
            m.description = (*pkg)["description"].value_or("");
            m.license = (*pkg)["license"].value_or("");
            m.channel = (*pkg)["channel"].value_or("system");
            m.arch = (*pkg)["arch"].value_or("x86_64");
            auto architecture = validate_package_architecture(m.arch);
            if (!architecture) return std::unexpected(architecture.error());
            m.installed_size = (*pkg)["installed_size"].value_or(0ULL);
            m.build_compiler = (*pkg)["build_compiler"].value_or("");
            m.build_compiler_version = (*pkg)["build_compiler_version"].value_or("");
            m.build_cflags = (*pkg)["build_cflags"].value_or("");
            m.build_cxxflags = (*pkg)["build_cxxflags"].value_or("");
            m.service_toml = (*pkg)["service_toml"].value_or("");
            m.build_ldflags = (*pkg)["build_ldflags"].value_or("");
            m.build_rustflags = (*pkg)["build_rustflags"].value_or("");
        } else {
            return std::unexpected("Missing [package] section in manifest");
        }

        if (m.name.empty()) return std::unexpected("Package name cannot be empty");

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

        // Parse dependencies (root or package section)
        parse_deps(tbl, "dependencies", m.dependencies);
        if (auto* pkg = tbl.get_as<vendor::toml::table>("package")) {
            parse_deps(*pkg, "dependencies", m.dependencies);
        }

        // Parse provides
        parse_strings(tbl, "provides", m.provides);
        if (auto* pkg = tbl.get_as<vendor::toml::table>("package")) {
            parse_strings(*pkg, "provides", m.provides);
        }
        if (auto* src = tbl.get_as<vendor::toml::table>("source")) {
            parse_strings(*src, "provides", m.provides);
        }

        // Parse conffiles
        parse_strings(tbl, "conffiles", m.conffiles);
        if (auto* pkg = tbl.get_as<vendor::toml::table>("package")) {
            parse_strings(*pkg, "conffiles", m.conffiles);
        }
        if (auto* src = tbl.get_as<vendor::toml::table>("source")) {
            parse_strings(*src, "conffiles", m.conffiles);
        }

        // Parse conflicts
        parse_deps(tbl, "conflicts", m.conflicts);
        if (auto* pkg = tbl.get_as<vendor::toml::table>("package")) {
            parse_deps(*pkg, "conflicts", m.conflicts);
        }
        if (auto* src = tbl.get_as<vendor::toml::table>("source")) {
            parse_deps(*src, "conflicts", m.conflicts);
        }

        // Parse files. Two spellings are accepted: a bare array of path
        // strings (what older manifests carry), and an array of tables with
        // the full FileEntry metadata, which is what round-trips through LMDB
        // and backs `sage query files` / `sage verify`.
        auto parse_files = [&](const vendor::toml::array& arr) {
            for (auto&& f : arr) {
                if (auto str = f.value<std::string_view>()) {
                    FileEntry fe;
                    fe.path = std::string(*str);
                    m.files.push_back(std::move(fe));
                } else if (auto* ftab = f.as_table()) {
                    FileEntry fe;
                    fe.path = (*ftab)["path"].value_or("");
                    if (fe.path.empty()) continue;
                    fe.size = (*ftab)["size"].value_or(0ULL);
                    fe.mode = static_cast<uint32_t>((*ftab)["mode"].value_or(0644LL));
                    fe.sha256 = (*ftab)["sha256"].value_or("");
                    fe.type = parse_file_type((*ftab)["type"].value_or("file"));
                    fe.link_target = (*ftab)["link_target"].value_or("");
                    m.files.push_back(std::move(fe));
                }
            }
        };

        if (auto* fls = tbl.get_as<vendor::toml::array>("files")) {
            parse_files(*fls);
        }
        if (auto* pkg = tbl.get_as<vendor::toml::table>("package")) {
            if (auto* fls = pkg->get_as<vendor::toml::array>("files")) {
                parse_files(*fls);
            }
        }

        // [[build_producers]] -- one entry per compiler family the artifacts
        // name. Absent entirely in pre-0.2.1 manifests.
        // Historical quirk: these arrays may sit at root scope or nested
        // inside [package]; accept either (mirrors the files dual-read).
        auto producers_tbl = tbl.get_as<vendor::toml::array>("build_producers");
        if (!producers_tbl)
            if (auto* pkg2 = tbl.get_as<vendor::toml::table>("package"))
                producers_tbl = pkg2->get_as<vendor::toml::array>("build_producers");
        // Salvage for pre-0.2.3 serializers: those scoped package arrays
        // (dependencies/provides/conflicts/conffiles) under the last
        // [[build_producers]] element instead of root/[package], so legacy
        // manifests carry stray keys inside producer tables. Probe each raw
        // element and stash what turns up; adoption below only fills fields
        // still empty after the normal reads above.
        std::vector<Dependency> salvaged_deps;
        std::vector<std::string> salvaged_provides;
        std::vector<Dependency> salvaged_conflicts;
        std::vector<std::string> salvaged_conffiles;
        if (producers_tbl) {
            for (auto&& el : *producers_tbl) {
                auto* t = el.as_table();
                if (!t) continue;
                parse_deps(*t, "dependencies", salvaged_deps);
                parse_strings(*t, "provides", salvaged_provides);
                parse_deps(*t, "conflicts", salvaged_conflicts);
                parse_strings(*t, "conffiles", salvaged_conffiles);
                BuildProducer p;
                p.name = (*t)["name"].value_or("");
                if (p.name.empty()) continue;
                if (auto* vs = t->get_as<vendor::toml::array>("versions")) {
                    for (auto&& v : *vs) {
                        if (auto s = v.value<std::string_view>()) {
                            p.versions.emplace_back(*s);
                        }
                    }
                }
                p.flags = (*t)["flags"].value_or("");
                m.build_producers.push_back(std::move(p));
            }
        }
        if (m.dependencies.empty()) m.dependencies = std::move(salvaged_deps);
        if (m.provides.empty()) m.provides = std::move(salvaged_provides);
        if (m.conflicts.empty()) m.conflicts = std::move(salvaged_conflicts);
        if (m.conffiles.empty()) m.conffiles = std::move(salvaged_conffiles);
        auto pt_arr = tbl.get_as<vendor::toml::array>("build_flag_passthrough");
        if (!pt_arr)
            if (auto* pkg3 = tbl.get_as<vendor::toml::table>("package"))
                pt_arr = pkg3->get_as<vendor::toml::array>("build_flag_passthrough");
        if (pt_arr) {
            for (auto&& el : *pt_arr) {
                if (auto s = el.value<std::string_view>(); s && !s->empty()) {
                    m.build_flag_passthrough.emplace_back(*s);
                }
            }
        }
        parse_capability_hooks(tbl, m.capability_hooks);
        auto trig_res = parse_triggers(tbl, m.triggers);
        if (!trig_res) return std::unexpected(trig_res.error());

        return m;
    }

    [[nodiscard]] std::string serialize_toml() const {
        const auto quote = [](std::string_view value) {
            return escape_toml_basic_string(value);
        };
        std::ostringstream ss;
        ss << "schema_version = " << schema_version << "\n\n";
        ss << "[package]\n";
        ss << "name = \"" << quote(name) << "\"\n";
        ss << "version = \"" << quote(version.ver) << "\"\n";
        ss << "release = \"" << quote(version.rel) << "\"\n";
        if (version.epoch > 0) ss << "epoch = " << version.epoch << "\n";
        ss << "description = \"" << quote(description) << "\"\n";
        ss << "license = \"" << quote(license) << "\"\n";
        ss << "channel = \"" << quote(channel) << "\"\n";
        ss << "arch = \"" << quote(arch) << "\"\n";
        // Provenance is omitted entirely when unknown so that packages built
        // before it existed keep their byte-identical manifests.
        if (!build_compiler.empty()) ss << "build_compiler = \"" << quote(build_compiler) << "\"\n";
        if (!build_compiler_version.empty()) ss << "build_compiler_version = \"" << quote(build_compiler_version) << "\"\n";
        if (!build_cflags.empty()) ss << "build_cflags = \"" << quote(build_cflags) << "\"\n";
        if (!build_cxxflags.empty()) ss << "build_cxxflags = \"" << quote(build_cxxflags) << "\"\n";
        if (!build_ldflags.empty()) ss << "build_ldflags = \"" << quote(build_ldflags) << "\"\n";
        if (!build_rustflags.empty()) ss << "build_rustflags = \"" << quote(build_rustflags) << "\"\n";
        if (!service_toml.empty()) ss << "service_toml = \"" << quote(service_toml) << "\"\n";
        ss << "installed_size = " << installed_size << "\n\n";
        // Everything below lives at the ROOT scope: [package] closed above.
        // Flag passthrough annotations: env channels the recipe itself
        // forwarded flags through (kernel KCFLAGS/KAFLAGS and kin).
        if (!build_flag_passthrough.empty()) {
            ss << "build_flag_passthrough = [\n";
            for (const auto& channel : build_flag_passthrough) {
                ss << "    \"" << quote(channel) << "\",\n";
            }
            ss << "]\n";
        }
        ss << "dependencies = [\n";
        for (const auto& d : dependencies) {
            ss << "    \"" << quote(d.to_string()) << "\",\n";
        }
        ss << "]\n\n";

        ss << "provides = [\n";
        for (const auto& p : provides) {
            ss << "    \"" << quote(p) << "\",\n";
        }
        ss << "]\n\n";

        ss << "conflicts = [\n";
        for (const auto& c : conflicts) {
            ss << "    \"" << quote(c.to_string()) << "\",\n";
        }
        ss << "]\n\n";

        // Omitted entirely when empty so manifests of packages that declare
        // no conffiles stay byte-identical to their pre-conffile output.
        if (!conffiles.empty()) {
            ss << "conffiles = [\n";
            for (const auto& c : conffiles) {
                ss << "    \"" << quote(c) << "\",\n";
            }
            ss << "]\n\n";
        }

        for (const auto& h : capability_hooks) {
            ss << "[[capability_hooks]]\n";
            ss << "capability = \"" << quote(h.capability) << "\"\n";
            ss << "exec = \"" << quote(h.exec) << "\"\n";
            ss << "args = [";
            for (size_t i = 0; i < h.args.size(); ++i) {
                ss << (i ? ", " : "") << "\"" << quote(h.args[i]) << "\"";
            }
            ss << "]\n\n";
        }

        for (const auto& t : triggers) {
            ss << "[[triggers]]\n";
            ss << "name = \"" << quote(t.name) << "\"\n";
            ss << "on_paths = [";
            for (size_t i = 0; i < t.on_paths.size(); ++i) {
                ss << (i ? ", " : "") << "\"" << quote(t.on_paths[i]) << "\"";
            }
            ss << "]\n";
            ss << "on_capability = [";
            for (size_t i = 0; i < t.on_capability.size(); ++i) {
                ss << (i ? ", " : "") << "\"" << quote(t.on_capability[i]) << "\"";
            }
            ss << "]\n";
            ss << "run_capability = \"" << quote(t.run_capability) << "\"\n";
            ss << "exec = \"" << quote(t.exec) << "\"\n";
            if (t.required) ss << "required = true\n";
            ss << "priority = " << t.priority << "\n";
            ss << "args = [";
            for (size_t i = 0; i < t.args.size(); ++i) {
                ss << (i ? ", " : "") << "\"" << quote(t.args[i]) << "\"";
            }
            ss << "]\n\n";
        }

        // Array-of-tables, not bare paths: the per-file hash and mode are what
        // make `sage query files` and integrity verification possible at all,
        // and this serialization is also the LMDB record.
        for (const auto& f : files) {
            ss << "[[files]]\n";
            ss << "path = \"" << quote(f.path) << "\"\n";
            ss << "type = \"" << to_string(f.type) << "\"\n";
            ss << "mode = " << f.mode << "\n";
            ss << "size = " << f.size << "\n";
            if (!f.sha256.empty()) ss << "sha256 = \"" << quote(f.sha256) << "\"\n";
            if (!f.link_target.empty()) ss << "link_target = \"" << quote(f.link_target) << "\"\n";
            ss << "\n";
        }

        // Array tables change the active TOML scope. Keep producer records
        // last so package arrays written above cannot become fields of the
        // final [[build_producers]] entry.
        for (const auto& p : build_producers) {
            if (p.name.empty()) continue;
            ss << "[[build_producers]]\n";
            ss << "name = \"" << quote(p.name) << "\"\n";
            ss << "versions = [\n";
            for (const auto& v : p.versions) {
                ss << "    \"" << quote(v) << "\",\n";
            }
            ss << "]\n";
            if (!p.flags.empty()) {
                ss << "flags = \"" << quote(p.flags) << "\"\n";
            }
            ss << "\n";
        }

        return ss.str();
    }
};

struct PackageIdentity {
    std::string name;
    Version version;
    std::string arch;
    std::string channel;

    // Spelled out: a deduced return type does not survive module import
    // boundaries in GCC 15 and breaks comparison in importing units.
    std::strong_ordering operator<=>(const PackageIdentity&) const = default;
};

inline PackageIdentity package_identity(const PackageManifest& manifest) {
    return PackageIdentity{
        .name = manifest.name,
        .version = manifest.version,
        .arch = manifest.arch,
        .channel = manifest.channel,
    };
}

} // namespace sage::package
