export module sage.config;

import std;
import sage.vendor.toml;
import sage.util;

export namespace sage::config {

using std::uint32_t;
using std::size_t;

struct ChannelConfig {
    std::string name;
    std::string url;
    std::string scope{"system"}; // "system", "runtime", "toolchain", "user"
    int priority{50};
    bool enabled{true};
};

// ============================================================================
// Virtual capabilities
// ============================================================================
//
// A *capability* is a name in a package's `provides` that is not a package
// name: "virtual/init", "virtual/initramfs-generator", "so:libz.so.1". It is
// the contract other packages depend on.
//
// A *provider* is the binding of one capability to one concrete package. The
// binding lives in system.toml `[providers]`, never in a package.
//
// The axis that matters is whether the capability is exclusive:
//
//   Exclusive -- at most one provider may be installed. Swapping providers is
//                a system reconfiguration and goes through `sage rebuild`.
//                virtual/init, virtual/udev, virtual/libc.
//   Shared    -- any number of providers may coexist. A `[providers]` entry is
//                merely the preferred default the solver reaches for first.
//                virtual/initramfs-generator, virtual/kernel, every so:*.
//
// Anything not named in `[capabilities]` defaults to Shared: exclusivity is
// opt-in, because getting it wrong turns a coexisting pair into a hard
// resolution failure.
enum class CapabilityKind {
    Shared,
    Exclusive
};

inline std::string_view to_string(CapabilityKind k) noexcept {
    return k == CapabilityKind::Exclusive ? "exclusive" : "shared";
}

inline std::optional<CapabilityKind> parse_capability_kind(std::string_view s) noexcept {
    if (s == "exclusive") return CapabilityKind::Exclusive;
    if (s == "shared") return CapabilityKind::Shared;
    return std::nullopt;
}

// Capability names are stored fully qualified. system.toml may spell them
// either way ("init" or "virtual/init"); so: symbols are passed through.
inline std::string normalize_capability(std::string_view name) {
    if (name.starts_with("virtual/") || name.starts_with("so:")) {
        return std::string(name);
    }
    return "virtual/" + std::string(name);
}

// ============================================================================
// Build environment
// ============================================================================
//
// Pure build-environment configuration, read from /etc/sage/build.toml when
// present. A missing file means exactly these defaults (and the distro's
// `sage` package ships a build.toml that spells them out); a malformed file
// is warned about and ignored rather than failing every command.
//
// `cxxflags` left empty mirrors `cflags` at use time unless spelled out. The
// fallback pair exists for the clang-by-default policy: when a build fails
// under `cc`, cmd_build retries the whole recipe under `fallback_cc`.
struct BuildConfig {
    uint32_t schema_version{1};
    std::string cc{"clang"};
    std::string cxx{"clang++"};
    std::string fallback_cc{"gcc"};
    std::string fallback_cxx{"g++"};
    std::string cflags{"-O3 -march=x86-64-v3"};
    std::string cxxflags;
    std::string cppflags;
    std::string ldflags;
    // Parallel job count injected as MAKEFLAGS/CARGO_BUILD_JOBS. 0 (the
    // default) means auto: one job per hardware thread.
    int jobs{0};

    bool operator==(const BuildConfig&) const = default;

    static std::expected<BuildConfig, std::string> parse_toml(std::string_view toml_str) {
        auto tbl_res = vendor::toml::parse_string(toml_str);
        if (!tbl_res) return std::unexpected(tbl_res.error());
        const auto& tbl = *tbl_res;

        BuildConfig cfg;
        cfg.schema_version = static_cast<uint32_t>(tbl["schema_version"].value_or(1LL));
        if (auto v = tbl["cc"].value<std::string_view>()) cfg.cc = std::string(*v);
        if (auto v = tbl["cxx"].value<std::string_view>()) cfg.cxx = std::string(*v);
        if (auto v = tbl["fallback_cc"].value<std::string_view>()) cfg.fallback_cc = std::string(*v);
        if (auto v = tbl["fallback_cxx"].value<std::string_view>()) cfg.fallback_cxx = std::string(*v);
        if (auto v = tbl["cflags"].value<std::string_view>()) cfg.cflags = std::string(*v);
        if (auto v = tbl["cxxflags"].value<std::string_view>()) cfg.cxxflags = std::string(*v);
        if (auto v = tbl["cppflags"].value<std::string_view>()) cfg.cppflags = std::string(*v);
        if (auto v = tbl["ldflags"].value<std::string_view>()) cfg.ldflags = std::string(*v);
        cfg.jobs = static_cast<int>(tbl["jobs"].value_or(0LL));
        return cfg;
    }
};

inline std::string native_package_architecture() {
#if defined(__x86_64__)
    return "amd64";
#elif defined(__aarch64__)
    return "aarch64";
#else
#error "Sage supports amd64 and aarch64 targets"
#endif
}

struct SystemConfig {
    uint32_t schema_version{1};
    std::filesystem::path root_dir{"/"};
    std::filesystem::path db_path{"/var/lib/sage/data.mdb"};
    std::filesystem::path cache_dir{"/var/cache/sage"};
    std::filesystem::path config_dir{"/etc/sage"};
    std::filesystem::path system_config_path{"/etc/sage/system.toml"};
    std::filesystem::path channels_config_path{"/etc/sage/channels.toml"};
    std::filesystem::path build_config_path{"/etc/sage/build.toml"};
    std::string architecture{native_package_architecture()};

    // Capability -> concrete provider binding.
    // e.g. "virtual/init" -> "systemd", "virtual/initramfs-generator" -> "mkinitcpio"
    // For exclusive capabilities this is a lock; for shared ones, a default.
    std::map<std::string, std::string> providers;

    // Capability -> exclusivity. Absent means CapabilityKind::Shared.
    std::map<std::string, CapabilityKind> capabilities;

    std::vector<ChannelConfig> channels;

    // Build-time tuning knobs (compiler, flags), see BuildConfig above.
    BuildConfig build;

    static SystemConfig default_config() {
        SystemConfig cfg;
        cfg.providers["virtual/init"] = "openrc";
        cfg.providers["virtual/udev"] = "eudev";
        cfg.providers["virtual/libc"] = "glibc";
        // Core userland is exclusive like libc: every implementation claims
        // /usr/bin/ls, so exactly one may own the system at a time.
        cfg.providers["virtual/coreutils"] = "coreutils";
        // The initramfs builder is a *default*, not a lock: dracut and
        // mkinitcpio can sit on disk together, and an admin who wants the
        // other one only has to retarget this one line.
        cfg.providers["virtual/initramfs-generator"] = "mkinitcpio";

        cfg.capabilities["virtual/init"] = CapabilityKind::Exclusive;
        cfg.capabilities["virtual/udev"] = CapabilityKind::Exclusive;
        cfg.capabilities["virtual/libc"] = CapabilityKind::Exclusive;
        cfg.capabilities["virtual/coreutils"] = CapabilityKind::Exclusive;

        ChannelConfig core;
        core.name = "core";
        core.url = "https://pkg.sage-linux.org/core";
        core.scope = "system";
        core.priority = 100;
        core.enabled = true;
        cfg.channels.push_back(std::move(core));

        return cfg;
    }

    static std::expected<SystemConfig, std::string> parse_system_toml(std::string_view toml_str) {
        auto tbl_res = vendor::toml::parse_string(toml_str);
        if (!tbl_res) return std::unexpected(tbl_res.error());
        const auto& tbl = *tbl_res;

        SystemConfig cfg;
        // Existing system.toml files predate [capabilities]. Preserve the
        // historic single-provider semantics for the core interfaces;
        // explicit entries below may still override any of these to shared.
        cfg.capabilities["virtual/init"] = CapabilityKind::Exclusive;
        cfg.capabilities["virtual/udev"] = CapabilityKind::Exclusive;
        cfg.capabilities["virtual/libc"] = CapabilityKind::Exclusive;
        cfg.capabilities["virtual/coreutils"] = CapabilityKind::Exclusive;

        if (auto* sys = tbl.get_as<vendor::toml::table>("system")) {
            cfg.root_dir = (*sys)["root_dir"].value_or("/");
            cfg.db_path = (*sys)["db_path"].value_or("/var/lib/sage/data.mdb");
            cfg.cache_dir = (*sys)["cache_dir"].value_or("/var/cache/sage");
            cfg.architecture = (*sys)["architecture"].value_or(native_package_architecture());
            if (cfg.architecture != "amd64"
                && cfg.architecture != "x86_64"
                && cfg.architecture != "aarch64") {
                return std::unexpected(std::format(
                    "Unsupported system package architecture '{}' (expected amd64 or aarch64)",
                    cfg.architecture));
            }
            if (auto* cfg_d = sys->get("config_dir")) {
                cfg.config_dir = cfg_d->value_or("/etc/sage");
                cfg.system_config_path = cfg.config_dir / "system.toml";
                cfg.channels_config_path = cfg.config_dir / "channels.toml";
                cfg.build_config_path = cfg.config_dir / "build.toml";
            }
        }

        if (auto* prov = tbl.get_as<vendor::toml::table>("providers")) {
            for (auto&& [k, v] : *prov) {
                if (auto val_str = v.value<std::string_view>()) {
                    cfg.providers[normalize_capability(k.str())] = std::string(*val_str);
                }
            }
        }

        if (auto* caps = tbl.get_as<vendor::toml::table>("capabilities")) {
            for (auto&& [k, v] : *caps) {
                auto val_str = v.value<std::string_view>();
                if (!val_str) continue;
                auto kind = parse_capability_kind(*val_str);
                if (!kind) {
                    return std::unexpected(std::format(
                        "Unknown capability kind '{}' for '{}' (expected \"exclusive\" or \"shared\")",
                        *val_str, k.str()));
                }
                cfg.capabilities[normalize_capability(k.str())] = *kind;
            }
        }

        // Also check if channels are defined inside system.toml
        if (auto* chs = tbl.get_as<vendor::toml::array>("channels")) {
            for (auto&& item : *chs) {
                if (auto* ctab = item.as_table()) {
                    ChannelConfig ch;
                    ch.name = (*ctab)["name"].value_or("");
                    ch.url = (*ctab)["url"].value_or("");
                    ch.scope = (*ctab)["scope"].value_or("system");
                    ch.priority = static_cast<int>((*ctab)["priority"].value_or(50LL));
                    ch.enabled = (*ctab)["enabled"].value_or(true);
                    if (!ch.name.empty()) {
                        cfg.channels.push_back(std::move(ch));
                    }
                }
            }
        }

        return cfg;
    }

    static std::expected<std::vector<ChannelConfig>, std::string> parse_channels_toml(std::string_view toml_str) {
        auto tbl_res = vendor::toml::parse_string(toml_str);
        if (!tbl_res) return std::unexpected(tbl_res.error());
        const auto& tbl = *tbl_res;

        std::vector<ChannelConfig> list;
        if (auto* chs = tbl.get_as<vendor::toml::array>("channels")) {
            for (auto&& item : *chs) {
                if (auto* ctab = item.as_table()) {
                    ChannelConfig ch;
                    ch.name = (*ctab)["name"].value_or("");
                    ch.url = (*ctab)["url"].value_or("");
                    ch.scope = (*ctab)["scope"].value_or("system");
                    ch.priority = static_cast<int>((*ctab)["priority"].value_or(50LL));
                    ch.enabled = (*ctab)["enabled"].value_or(true);
                    if (!ch.name.empty()) {
                        list.push_back(std::move(ch));
                    }
                }
            }
        }
        return list;
    }

    static std::expected<SystemConfig, std::string> load_or_default(const std::filesystem::path& config_dir = "/etc/sage") {
        std::filesystem::path sys_path = config_dir / "system.toml";
        std::filesystem::path chan_path = config_dir / "channels.toml";

        SystemConfig cfg = default_config();
        cfg.config_dir = config_dir;
        cfg.system_config_path = sys_path;
        cfg.channels_config_path = chan_path;
        cfg.build_config_path = config_dir / "build.toml";

        if (std::filesystem::exists(sys_path)) {
            std::ifstream f(sys_path);
            std::stringstream ss;
            ss << f.rdbuf();
            auto sys_res = parse_system_toml(ss.str());
            if (!sys_res) return sys_res;
            cfg = std::move(*sys_res);
            cfg.config_dir = config_dir;
            cfg.system_config_path = sys_path;
            cfg.channels_config_path = chan_path;
            cfg.build_config_path = config_dir / "build.toml";
        }

        if (std::filesystem::exists(chan_path)) {
            std::ifstream f(chan_path);
            std::stringstream ss;
            ss << f.rdbuf();
            auto ch_res = parse_channels_toml(ss.str());
            if (ch_res && !ch_res->empty()) {
                cfg.channels = std::move(*ch_res);
            }
        } else if (config_dir != "/etc/sage" && std::filesystem::exists("/etc/sage/channels.toml")) {
            std::ifstream f("/etc/sage/channels.toml");
            std::stringstream ss;
            ss << f.rdbuf();
            auto ch_res = parse_channels_toml(ss.str());
            if (ch_res && !ch_res->empty()) {
                cfg.channels = std::move(*ch_res);
            }
        }

        // Build settings overlay last: they are leaf tuning knobs, never
        // structural, so even a broken file degrades to the defaults with a
        // warning instead of taking every command down with it.
        if (std::filesystem::exists(cfg.build_config_path)) {
            std::ifstream bf(cfg.build_config_path);
            std::stringstream bs;
            bs << bf.rdbuf();
            auto b_res = BuildConfig::parse_toml(bs.str());
            if (b_res) {
                cfg.build = std::move(*b_res);
            } else {
                sage::util::log_warn("Ignoring invalid {}: {}", cfg.build_config_path.string(), b_res.error());
            }
        }

        return cfg;
    }

    static std::expected<SystemConfig, std::string> load_from_root(const std::filesystem::path& target_root = "/") {
        std::filesystem::path norm_root = target_root.empty() ? "/" : target_root;
        std::filesystem::path config_dir = (norm_root == "/") ? "/etc/sage" : (norm_root / "etc/sage");
        auto res = load_or_default(config_dir);
        if (!res) return res;

        res->root_dir = norm_root;
        if (norm_root != "/") {
            res->db_path = norm_root / "var/lib/sage/data.mdb";
            res->cache_dir = norm_root / "var/cache/sage";
            res->config_dir = config_dir;
            res->system_config_path = config_dir / "system.toml";
            res->channels_config_path = config_dir / "channels.toml";
            res->build_config_path = config_dir / "build.toml";
        }
        return res;
    }

    // Exclusivity is opt-in: an undeclared capability is shared.
    [[nodiscard]] CapabilityKind capability_kind(std::string_view cap) const {
        auto it = capabilities.find(normalize_capability(cap));
        return it == capabilities.end() ? CapabilityKind::Shared : it->second;
    }

    [[nodiscard]] bool is_exclusive_capability(std::string_view cap) const {
        return capability_kind(cap) == CapabilityKind::Exclusive;
    }

    // The bound provider, if the admin declared one. Shared capabilities with
    // no binding resolve through the solver instead.
    [[nodiscard]] std::optional<std::string> provider_for(std::string_view cap) const {
        auto it = providers.find(normalize_capability(cap));
        if (it == providers.end() || it->second.empty()) return std::nullopt;
        return it->second;
    }

    // Only exclusive capabilities take part in `sage rebuild` reconcile; a
    // shared default changing is not a system state change.
    [[nodiscard]] std::map<std::string, std::string> exclusive_providers() const {
        std::map<std::string, std::string> out;
        for (const auto& [cap, prov] : providers) {
            if (is_exclusive_capability(cap)) out.emplace(cap, prov);
        }
        return out;
    }

    [[nodiscard]] std::string serialize_system_toml() const {
        std::ostringstream ss;
        ss << "schema_version = " << schema_version << "\n\n";
        ss << "[system]\n";
        ss << "root_dir = \"" << root_dir.string() << "\"\n";
        ss << "db_path = \"" << db_path.string() << "\"\n";
        ss << "cache_dir = \"" << cache_dir.string() << "\"\n";
        ss << "config_dir = \"" << config_dir.string() << "\"\n";
        ss << "architecture = \"" << architecture << "\"\n\n";

        ss << "[providers]\n";
        for (const auto& [k, v] : providers) {
            std::string short_key = k.starts_with("virtual/") ? k.substr(8) : k;
            ss << short_key << " = \"" << v << "\"\n";
        }

        if (!capabilities.empty()) {
            ss << "\n[capabilities]\n";
            for (const auto& [k, v] : capabilities) {
                std::string short_key = k.starts_with("virtual/") ? k.substr(8) : k;
                ss << short_key << " = \"" << to_string(v) << "\"\n";
            }
        }
        return ss.str();
    }

    [[nodiscard]] std::string serialize_channels_toml() const {
        std::ostringstream ss;
        ss << "schema_version = 1\n\n";
        for (const auto& ch : channels) {
            ss << "[[channels]]\n";
            ss << "name = \"" << ch.name << "\"\n";
            ss << "url = \"" << ch.url << "\"\n";
            ss << "scope = \"" << ch.scope << "\"\n";
            ss << "priority = " << ch.priority << "\n";
            ss << "enabled = " << (ch.enabled ? "true" : "false") << "\n\n";
        }
        return ss.str();
    }
};

} // namespace sage::config
