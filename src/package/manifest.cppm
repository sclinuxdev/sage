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

// One tool Sage configured and directly probed for a managed recipe-v2
// build. This is an execution observation, not inferred producer provenance:
// `version` is parsed from this exact executable's `--version` output.
struct ManagedBuildTool {
    std::string role;       // cc | cxx | linker | rustc
    std::string executable; // exact build.toml command/path Sage invoked
    std::string family;     // gcc | clang | ld | lld | mold
    std::string version;

    bool operator==(const ManagedBuildTool&) const = default;
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
    // Empty for recipe v1 and upstream-binary repackaging. Sage populates it
    // only after a managed recipe-v2 build has completed successfully.
    std::vector<ManagedBuildTool> managed_build_tools;

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
            m.service_toml = (*pkg)["service_toml"].value_or("");
            m.installed_size = (*pkg)["installed_size"].value_or(0ULL);
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

        parse_capability_hooks(tbl, m.capability_hooks);
        auto trig_res = parse_triggers(tbl, m.triggers);
        if (!trig_res) return std::unexpected(trig_res.error());

        if (auto* tools = tbl.get_as<vendor::toml::array>("managed_build_tools")) {
            if (m.schema_version < 2) return std::unexpected(
                "managed_build_tools are valid only in package manifest schema v2");
            std::set<std::string> roles;
            for (auto&& item : *tools) {
                auto* tool = item.as_table();
                if (!tool) return std::unexpected(
                    "managed_build_tools entries must be tables");
                ManagedBuildTool observed;
                observed.role = (*tool)["role"].value_or("");
                observed.executable = (*tool)["executable"].value_or("");
                observed.family = (*tool)["family"].value_or("");
                observed.version = (*tool)["version"].value_or("");
                const std::string version_argument =
                    (*tool)["version_argument"].value_or("");
                const bool known_role = observed.role == "cc"
                    || observed.role == "cxx" || observed.role == "linker"
                    || observed.role == "rustc";
                const bool known_family =
                    ((observed.role == "cc" || observed.role == "cxx")
                        && (observed.family == "gcc" || observed.family == "clang"))
                    || (observed.role == "linker"
                        && (observed.family == "ld" || observed.family == "lld"
                            || observed.family == "mold"))
                    || (observed.role == "rustc" && observed.family == "rustc");
                if (!known_role || observed.executable.empty()
                    || !known_family || observed.version.empty()
                    || version_argument != "--version") {
                    return std::unexpected(
                        "managed_build_tools require a unique cc/cxx/linker/rustc role, executable, family, version and version_argument='--version'");
                }
                if (!roles.insert(observed.role).second) return std::unexpected(
                    "managed_build_tools contains a duplicate role: "
                    + observed.role);
                m.managed_build_tools.push_back(std::move(observed));
            }
        }

        return m;
    }

    // LMDB stores Sage's own canonical serialization, whose [[files]] blocks
    // are deliberately last. Solver, service and trigger planning need package
    // metadata but not tens of thousands of file records; truncate before the
    // TOML parser so their cost is proportional to package count, not payload
    // file count. Archive parsing still uses parse_toml() and validates all
    // file entries.
    static std::expected<PackageManifest, std::string> parse_summary_toml(
        std::string_view toml_content)
    {
        constexpr std::string_view marker = "[[files]]";
        const auto files = toml_content.find(marker);
        return parse_toml(files == std::string_view::npos
            ? toml_content : toml_content.substr(0, files));
    }

    [[nodiscard]] std::string serialize_toml(bool include_files = true) const {
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
        if (!service_toml.empty()) ss << "service_toml = \"" << quote(service_toml) << "\"\n";
        ss << "installed_size = " << installed_size << "\n\n";
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

        for (const auto& tool : managed_build_tools) {
            ss << "[[managed_build_tools]]\n";
            ss << "role = \"" << quote(tool.role) << "\"\n";
            ss << "executable = \"" << quote(tool.executable) << "\"\n";
            ss << "family = \"" << quote(tool.family) << "\"\n";
            ss << "version = \"" << quote(tool.version) << "\"\n";
            ss << "version_argument = \"--version\"\n\n";
        }

        // Array-of-tables, not bare paths: the per-file hash and mode are what
        // make `sage query files` and integrity verification possible at all,
        // and this serialization is also the LMDB record.
        if (include_files) {
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
        }

        return ss.str();
    }

    [[nodiscard]] std::string serialize_summary_toml() const {
        return serialize_toml(false);
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
