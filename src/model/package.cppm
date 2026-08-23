export module sage.package;

import std;
import sage.vendor.toml;
import sage.util;

export namespace sage::package {

using std::uint8_t;
using std::uint32_t;
using std::uint64_t;
using std::size_t;

// ============================================================================
// Version Model with standard epoch-ver-rel ordering
// ============================================================================

struct Version {
    uint32_t epoch{0};
    std::string ver;
    std::string rel{"1"};

    static Version parse(std::string_view s) {
        Version v;
        if (s.empty()) return v;

        // Check for epoch (e.g. "1:2.0.0-1")
        if (auto colon = s.find(':'); colon != std::string_view::npos) {
            uint32_t ep = 0;
            for (char c : s.substr(0, colon)) {
                if (std::isdigit(static_cast<unsigned char>(c))) ep = ep * 10 + (c - '0');
            }
            v.epoch = ep;
            s = s.substr(colon + 1);
        }

        // Check for release (e.g. "2.0.0-1")
        if (auto dash = s.rfind('-'); dash != std::string_view::npos) {
            v.ver = std::string(s.substr(0, dash));
            v.rel = std::string(s.substr(dash + 1));
        } else {
            v.ver = std::string(s);
            v.rel = "1";
        }
        return v;
    }

    [[nodiscard]] std::string to_string() const {
        if (epoch > 0) {
            return std::format("{}:{}-{}", epoch, ver, rel);
        }
        return std::format("{}-{}", ver, rel);
    }

    // Alphanumeric segment comparator (vercmp)
    static int compare_segments(std::string_view a, std::string_view b) noexcept {
        size_t i = 0, j = 0;
        while (i < a.size() || j < b.size()) {
            while (i < a.size() && !std::isalnum(static_cast<unsigned char>(a[i]))) ++i;
            while (j < b.size() && !std::isalnum(static_cast<unsigned char>(b[j]))) ++j;
            if (i >= a.size() || j >= b.size()) {
                if (i >= a.size() && j >= b.size()) return 0;
                return (i >= a.size()) ? -1 : 1;
            }

            bool a_digit = std::isdigit(static_cast<unsigned char>(a[i]));
            bool b_digit = std::isdigit(static_cast<unsigned char>(b[j]));

            if (a_digit && b_digit) {
                // Numeric comparison
                size_t start_i = i, start_j = j;
                while (i < a.size() && std::isdigit(static_cast<unsigned char>(a[i]))) ++i;
                while (j < b.size() && std::isdigit(static_cast<unsigned char>(b[j]))) ++j;

                std::string_view sa = a.substr(start_i, i - start_i);
                std::string_view sb = b.substr(start_j, j - start_j);
                // Strip leading zeros
                while (sa.size() > 1 && sa.front() == '0') sa.remove_prefix(1);
                while (sb.size() > 1 && sb.front() == '0') sb.remove_prefix(1);

                if (sa.size() != sb.size()) {
                    return sa.size() < sb.size() ? -1 : 1;
                }
                if (sa != sb) {
                    return sa < sb ? -1 : 1;
                }
            } else if (!a_digit && !b_digit) {
                // Alpha segment comparison
                size_t start_i = i, start_j = j;
                while (i < a.size() && std::isalpha(static_cast<unsigned char>(a[i]))) ++i;
                while (j < b.size() && std::isalpha(static_cast<unsigned char>(b[j]))) ++j;

                std::string_view sa = a.substr(start_i, i - start_i);
                std::string_view sb = b.substr(start_j, j - start_j);
                if (sa != sb) {
                    return sa < sb ? -1 : 1;
                }
            } else {
                return a_digit ? 1 : -1;
            }
        }
        return 0;
    }

    std::strong_ordering operator<=>(const Version& other) const noexcept {
        if (epoch != other.epoch) {
            return epoch <=> other.epoch;
        }
        int vc = compare_segments(ver, other.ver);
        if (vc != 0) {
            return vc < 0 ? std::strong_ordering::less : std::strong_ordering::greater;
        }
        int rc = compare_segments(rel, other.rel);
        if (rc != 0) {
            return rc < 0 ? std::strong_ordering::less : std::strong_ordering::greater;
        }
        return std::strong_ordering::equal;
    }

    bool operator==(const Version& other) const noexcept {
        return (*this <=> other) == std::strong_ordering::equal;
    }
};

// Releases participate in monotonic publication identity selection, so their
// textual representation is intentionally narrower than upstream versions.
// Keep the parser here so recipes, manifests, and repository indexes enforce
// exactly the same positive-decimal contract.
inline std::expected<uint64_t, std::string> parse_release(std::string_view release) {
    uint64_t value = 0;
    if (release.empty()) return std::unexpected("Release must be a positive decimal integer");
    auto [end, error] = std::from_chars(release.data(), release.data() + release.size(), value);
    if (error != std::errc{} || end != release.data() + release.size() || value == 0) {
        return std::unexpected(std::format(
            "Invalid release '{}' (expected a positive decimal integer)", release));
    }
    return value;
}

inline std::expected<std::string, std::string> parse_release_field(
    const vendor::toml::table& table,
    std::string_view fallback = "1")
{
    const auto* node = table.get("release");
    if (!node) return std::string(fallback);
    auto release = node->value<std::string_view>();
    if (!release) return std::unexpected("Release must be a TOML string when present");
    if (auto parsed = parse_release(*release); !parsed) {
        return std::unexpected(parsed.error());
    }
    return std::string(*release);
}

// ============================================================================
// Dependency Model
// ============================================================================

enum class ConstraintOp {
    Any,
    Equal,
    NotEqual,
    GreaterEqual,
    LessEqual,
    Greater,
    Less
};

struct Dependency {
    std::string name;
    ConstraintOp op{ConstraintOp::Any};
    Version version;

    static Dependency parse(std::string_view s) {
        s = util::trim(s);
        Dependency d;
        if (s.empty()) return d;

        size_t op_pos = std::string_view::npos;
        std::string_view op_str;

        static const std::pair<std::string_view, ConstraintOp> ops[] = {
            {">=", ConstraintOp::GreaterEqual},
            {"<=", ConstraintOp::LessEqual},
            {"!=", ConstraintOp::NotEqual},
            {"==", ConstraintOp::Equal},
            {"=",  ConstraintOp::Equal},
            {">",  ConstraintOp::Greater},
            {"<",  ConstraintOp::Less}
        };

        for (const auto& [str, op] : ops) {
            if (auto pos = s.find(str); pos != std::string_view::npos) {
                op_pos = pos;
                op_str = str;
                d.op = op;
                break;
            }
        }

        if (op_pos == std::string_view::npos) {
            d.name = std::string(s);
            d.op = ConstraintOp::Any;
        } else {
            d.name = std::string(util::trim(s.substr(0, op_pos)));
            d.version = Version::parse(util::trim(s.substr(op_pos + op_str.size())));
        }
        return d;
    }

    [[nodiscard]] bool satisfies(const Version& target_ver) const noexcept {
        if (op == ConstraintOp::Any) return true;
        auto cmp = target_ver <=> version;
        switch (op) {
            case ConstraintOp::Equal:        return cmp == 0;
            case ConstraintOp::NotEqual:     return cmp != 0;
            case ConstraintOp::GreaterEqual: return cmp >= 0;
            case ConstraintOp::LessEqual:    return cmp <= 0;
            case ConstraintOp::Greater:      return cmp > 0;
            case ConstraintOp::Less:         return cmp < 0;
            default:                         return true;
        }
    }

    [[nodiscard]] std::string to_string() const {
        if (op == ConstraintOp::Any) return name;
        std::string_view op_sym = "=";
        switch (op) {
            case ConstraintOp::GreaterEqual: op_sym = ">="; break;
            case ConstraintOp::LessEqual:    op_sym = "<="; break;
            case ConstraintOp::NotEqual:     op_sym = "!="; break;
            case ConstraintOp::Greater:      op_sym = ">"; break;
            case ConstraintOp::Less:         op_sym = "<"; break;
            default: op_sym = "="; break;
        }
        return std::format("{} {} {}", name, op_sym, version.to_string());
    }
};

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

inline std::string escape_toml_basic_string(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (unsigned char ch : value) {
        switch (ch) {
            case '"': escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\b': escaped += "\\b"; break;
            case '\t': escaped += "\\t"; break;
            case '\n': escaped += "\\n"; break;
            case '\f': escaped += "\\f"; break;
            case '\r': escaped += "\\r"; break;
            default:
                if (ch < 0x20 || ch == 0x7f) {
                    escaped += std::format("\\u{:04X}", ch);
                } else {
                    escaped.push_back(static_cast<char>(ch));
                }
        }
    }
    return escaped;
}

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

// ============================================================================
// Capability Hooks
// ============================================================================
//
// A package that provides a capability may also declare *how* that capability
// is invoked. Without this, a trigger wanting "regenerate the initramfs" would
// have to hardcode a tool path and silently do nothing the moment the admin
// switched from mkinitcpio to dracut.

struct CapabilityHook {
    std::string capability;          // e.g. "virtual/initramfs-generator"
    std::string exec;                // absolute path inside the target root
    std::vector<std::string> args;

    [[nodiscard]] std::string command_line() const {
        std::string cmd = exec;
        for (const auto& a : args) {
            cmd += " ";
            cmd += a;
        }
        return cmd;
    }
};

// ============================================================================
// Package Triggers
// ============================================================================
//
// Declared by the package, evaluated once per transaction. A trigger fires
// when either condition matches:
//
//   on_paths      -- some file touched by the transaction sits under one of
//                    these relative prefixes ("usr/lib/modules/").
//   on_capability -- some package taking part in the transaction provides one
//                    of these capabilities ("virtual/kernel").
//
// and runs either a fixed `exec`, or -- preferably -- `run_capability`, which
// is resolved at fire time through the active provider's CapabilityHook.

struct Trigger {
    std::string name;
    std::vector<std::string> on_paths;
    std::vector<std::string> on_capability;
    std::string exec;                 // absolute path inside the target root
    std::vector<std::string> args;
    std::string run_capability;       // resolved via the provider's hook
    // Lower runs first. Ordering is not cosmetic: the initramfs must exist
    // before the bootloader is asked to reference it, and ldconfig must have
    // run before anything that dlopen()s.
    int priority{50};

    [[nodiscard]] bool matches_path(std::string_view rel_path) const {
        for (const auto& prefix : on_paths) {
            if (rel_path.starts_with(prefix)) return true;
        }
        return false;
    }

    [[nodiscard]] bool matches_capability(std::string_view cap) const {
        return std::ranges::find(on_capability, cap) != on_capability.end();
    }

    [[nodiscard]] bool is_valid() const {
        if (name.empty()) return false;
        if (on_paths.empty() && on_capability.empty()) return false;
        return !exec.empty() || !run_capability.empty();
    }
};

// Shared TOML readers for the two array-of-tables sections. Both are accepted
// at the document root and inside [package], matching how sage merges the
// other recipe arrays across scopes.

inline void parse_capability_hooks(const vendor::toml::table& tbl, std::vector<CapabilityHook>& out) {
    auto read = [&](const vendor::toml::table& scope) {
        auto* arr = scope.get_as<vendor::toml::array>("capability_hooks");
        if (!arr) return;
        for (auto&& item : *arr) {
            auto* t = item.as_table();
            if (!t) continue;
            CapabilityHook h;
            h.capability = (*t)["capability"].value_or("");
            h.exec = (*t)["exec"].value_or("");
            if (h.capability.empty() || h.exec.empty()) continue;
            if (auto* args = t->get_as<vendor::toml::array>("args")) {
                for (auto&& a : *args) {
                    if (auto str = a.value<std::string_view>()) h.args.emplace_back(*str);
                }
            }
            out.push_back(std::move(h));
        }
    };
    read(tbl);
    if (auto* pkg = tbl.get_as<vendor::toml::table>("package")) read(*pkg);
}

inline std::expected<void, std::string> parse_triggers(const vendor::toml::table& tbl, std::vector<Trigger>& out) {
    auto read_strings = [](const vendor::toml::table& t, const char* key, std::vector<std::string>& dest) {
        if (auto* arr = t.get_as<vendor::toml::array>(key)) {
            for (auto&& v : *arr) {
                if (auto str = v.value<std::string_view>()) dest.emplace_back(*str);
            }
        }
    };

    auto read = [&](const vendor::toml::table& scope) -> std::expected<void, std::string> {
        auto* arr = scope.get_as<vendor::toml::array>("triggers");
        if (!arr) return {};
        for (auto&& item : *arr) {
            auto* t = item.as_table();
            if (!t) continue;
            Trigger tr;
            tr.name = (*t)["name"].value_or("");
            tr.exec = (*t)["exec"].value_or("");
            tr.run_capability = (*t)["run_capability"].value_or("");
            tr.priority = static_cast<int>((*t)["priority"].value_or(50LL));
            read_strings(*t, "on_paths", tr.on_paths);
            read_strings(*t, "on_capability", tr.on_capability);
            read_strings(*t, "args", tr.args);
            if (!tr.is_valid()) {
                return std::unexpected(std::format(
                    "Invalid trigger '{}': needs a name, at least one of on_paths/on_capability, "
                    "and one of exec/run_capability",
                    tr.name.empty() ? "<unnamed>" : tr.name));
            }
            out.push_back(std::move(tr));
        }
        return {};
    };

    if (auto r = read(tbl); !r) return r;
    if (auto* pkg = tbl.get_as<vendor::toml::table>("package")) {
        if (auto r = read(*pkg); !r) return r;
    }
    return {};
}

// Parse a standalone `.METADATA/triggers.toml` document.
inline std::expected<std::vector<Trigger>, std::string> parse_triggers_toml(std::string_view toml_content) {
    auto tbl_res = vendor::toml::parse_string(toml_content);
    if (!tbl_res) return std::unexpected(tbl_res.error());
    std::vector<Trigger> out;
    if (auto r = parse_triggers(*tbl_res, out); !r) return std::unexpected(r.error());
    return out;
}

inline std::string serialize_triggers_toml(const std::vector<Trigger>& triggers) {
    const auto quote = [](std::string_view value) {
        return escape_toml_basic_string(value);
    };
    std::ostringstream ss;
    ss << "schema_version = 1\n";
    for (const auto& t : triggers) {
        ss << "\n[[triggers]]\n";
        ss << "name = \"" << quote(t.name) << "\"\n";
        if (!t.on_paths.empty()) {
            ss << "on_paths = [";
            for (size_t i = 0; i < t.on_paths.size(); ++i) {
                ss << (i ? ", " : "") << "\"" << quote(t.on_paths[i]) << "\"";
            }
            ss << "]\n";
        }
        if (!t.on_capability.empty()) {
            ss << "on_capability = [";
            for (size_t i = 0; i < t.on_capability.size(); ++i) {
                ss << (i ? ", " : "") << "\"" << quote(t.on_capability[i]) << "\"";
            }
            ss << "]\n";
        }
        if (!t.run_capability.empty()) {
            ss << "run_capability = \"" << quote(t.run_capability) << "\"\n";
        }
        if (!t.exec.empty()) {
            ss << "exec = \"" << quote(t.exec) << "\"\n";
        }
        ss << "priority = " << t.priority << "\n";
        if (!t.args.empty()) {
            ss << "args = [";
            for (size_t i = 0; i < t.args.size(); ++i) {
                ss << (i ? ", " : "") << "\"" << quote(t.args[i]) << "\"";
            }
            ss << "]\n";
        }
    }
    return ss.str();
}

// ============================================================================
// Package Manifest Model
// ============================================================================

inline bool package_architecture_matches(
    std::string_view package_architecture,
    std::string_view target_architecture)
{
    if (package_architecture == "any") return true;
    const auto canonical = [](std::string_view architecture) {
        return architecture == "x86_64" ? std::string_view{"amd64"} : architecture;
    };
    return canonical(package_architecture) == canonical(target_architecture);
}

inline std::expected<void, std::string> validate_package_architecture(
    std::string_view architecture)
{
    if (architecture == "amd64"
        || architecture == "x86_64"
        || architecture == "aarch64"
        || architecture == "any") {
        return {};
    }
    return std::unexpected(std::format(
        "Unsupported package architecture '{}'; expected amd64 (or legacy x86_64), aarch64, or any",
        architecture));
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

    // Build provenance, stamped by cmd_build after a successful build: the
    // compiler actually used (after any fallback) and the effective flags.
    // Rides along through serialization -- which is also the LMDB record and
    // the in-archive .METADATA/manifest.toml -- and is republished by the
    // repository index. All empty for packages built before this existed.
    std::string build_compiler;
    std::string build_compiler_version;
    std::string build_cflags;
    std::string build_cxxflags;
    std::string build_ldflags;

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

// ============================================================================
// Recipe Model for Package Building (`recipe.toml`)
// ============================================================================

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
            if (auto v = (*bld)["cxxflags"].value<std::string_view>()) r.cxxflags = std::string(*v);
            if (auto v = (*bld)["cc"].value<std::string_view>()) r.cc = std::string(*v);
            if (auto v = (*bld)["cxx"].value<std::string_view>()) r.cxx = std::string(*v);
        }

        parse_capability_hooks(tbl, r.capability_hooks);
        if (auto trig_res = parse_triggers(tbl, r.triggers); !trig_res) {
            return std::unexpected(trig_res.error());
        }

        return r;
    }
};

} // namespace sage::package
