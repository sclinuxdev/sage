export module sage.channel;

import std;
import sage.vendor.toml;
import sage.vendor.curl;
import sage.package;
import sage.util;

export namespace sage::channel {

enum class IndexRefresh {
    Always,
    IfMissing,
};

using std::size_t;
using std::uint32_t;

enum class ChannelScope {
    System,
    Runtime,
    Toolchain,
    User
};

inline ChannelScope parse_scope(std::string_view s) noexcept {
    if (s == "runtime" || s == "Runtime") return ChannelScope::Runtime;
    if (s == "toolchain" || s == "Toolchain") return ChannelScope::Toolchain;
    if (s == "user" || s == "User") return ChannelScope::User;
    return ChannelScope::System;
}

inline std::string_view to_string(ChannelScope sc) noexcept {
    switch (sc) {
        case ChannelScope::Runtime:   return "runtime";
        case ChannelScope::Toolchain: return "toolchain";
        case ChannelScope::User:      return "user";
        default:                      return "system";
    }
}

// ============================================================================
// Sub-Channel Canonical Specification (<Scope>/<Category>:<Slot>)
// ============================================================================

struct SubChannelSpec {
    ChannelScope scope{ChannelScope::System};
    std::string category;
    std::string slot;

    static SubChannelSpec parse(std::string_view s) {
        SubChannelSpec spec;
        s = util::trim(s);
        if (s.empty() || s == "system") return spec;

        // Check scope prefix
        if (s.starts_with("toolchain/")) {
            spec.scope = ChannelScope::Toolchain;
            s = s.substr(10);
        } else if (s.starts_with("runtime/")) {
            spec.scope = ChannelScope::Runtime;
            s = s.substr(8);
        } else if (s.starts_with("user/")) {
            spec.scope = ChannelScope::User;
            s = s.substr(5);
        } else {
            // Default heuristics: llvm, gcc, rust, java, clang are toolchains
            if (s.starts_with("llvm") || s.starts_with("gcc") || s.starts_with("rust") || 
                s.starts_with("java") || s.starts_with("clang") || s.starts_with("go")) {
                spec.scope = ChannelScope::Toolchain;
            } else if (s.starts_with("python") || s.starts_with("node") || s.starts_with("cuda")) {
                spec.scope = ChannelScope::Runtime;
            }
        }

        // Split category and slot (e.g. "llvm:22" or "gcc-15" or "openjdk-21")
        if (auto colon = s.find(':'); colon != std::string_view::npos) {
            spec.category = std::string(s.substr(0, colon));
            spec.slot = std::string(s.substr(colon + 1));
        } else if (auto dash = s.find('-'); dash != std::string_view::npos) {
            spec.category = std::string(s.substr(0, dash));
            spec.slot = std::string(s.substr(dash + 1));
        } else {
            spec.category = std::string(s);
            spec.slot = "default";
        }

        return spec;
    }

    [[nodiscard]] std::string to_string() const {
        if (scope == ChannelScope::System) return "system";
        return std::format("{}/{}:{}", channel::to_string(scope), category, slot);
    }
};

struct Channel {
    std::string name;
    std::string url;
    ChannelScope scope{ChannelScope::System};
    std::string category;
    std::string slot;
    int priority{50};
    bool enabled{true};
    bool active{false};
    std::string triplet{"x86_64-linux-gnu"};

    [[nodiscard]] std::filesystem::path resolve_target_root(
        const std::filesystem::path& sysroot = "/",
        const std::filesystem::path& user_home = {}) const 
    {
        switch (scope) {
            case ChannelScope::System:
                return sysroot;
            case ChannelScope::Runtime: {
                if (!category.empty() && !slot.empty()) {
                    return sysroot / "usr/lib/runtimes" / category / slot;
                }
                return sysroot / "usr/lib/runtimes" / name;
            }
            case ChannelScope::Toolchain: {
                if (!category.empty() && !slot.empty()) {
                    return sysroot / "opt/channels" / category / slot;
                }
                return sysroot / "opt/channels" / name;
            }
            case ChannelScope::User: {
                auto base = user_home.empty() ? (sysroot / "root/.local/channels") : (user_home / ".local/channels");
                if (!category.empty() && !slot.empty()) {
                    return base / category / slot;
                }
                return base / name;
            }
        }
        return sysroot;
    }
};

// ============================================================================
// Channel Remote Index Representation (index.toml schema_version = 1)
// ============================================================================

struct ChannelIndex {
    uint32_t schema_version{1};
    std::string channel_name;
    std::string updated_at;
    std::vector<package::PackageManifest> available_packages;

    static std::expected<ChannelIndex, std::string> parse_toml(std::string_view toml_str) {
        auto tbl_res = vendor::toml::parse_string(toml_str);
        if (!tbl_res) return std::unexpected(tbl_res.error());
        const auto& tbl = *tbl_res;

        ChannelIndex idx;
        idx.schema_version = static_cast<uint32_t>(tbl["schema_version"].value_or(1LL));

        if (auto* meta = tbl.get_as<vendor::toml::table>("channel")) {
            idx.channel_name = (*meta)["name"].value_or("");
            idx.updated_at = (*meta)["updated_at"].value_or("");
        }

        if (auto* pkgs = tbl.get_as<vendor::toml::array>("packages")) {
            for (auto&& item : *pkgs) {
                if (auto* ptab = item.as_table()) {
                    package::PackageManifest m;
                    m.name = (*ptab)["name"].value_or("");
                    std::string ver_str = std::string((*ptab)["version"].value_or(""));
                    m.version = package::Version::parse(ver_str);
                    auto release = package::parse_release_field(*ptab, m.version.rel);
                    if (!release) return std::unexpected(release.error());
                    m.version.rel = std::move(*release);
                    if (auto epoch = (*ptab)["epoch"].value<long long>()) {
                        if (*epoch < 0
                            || static_cast<unsigned long long>(*epoch)
                                > std::numeric_limits<uint32_t>::max()) {
                            return std::unexpected("Package epoch is outside the uint32 range");
                        }
                        m.version.epoch = static_cast<uint32_t>(*epoch);
                    }
                    m.description = (*ptab)["description"].value_or("");
                    m.license = (*ptab)["license"].value_or("");
                    m.channel = (*ptab)["channel"].value_or("system");
                    m.arch = (*ptab)["arch"].value_or("x86_64");
                    auto architecture = package::validate_package_architecture(m.arch);
                    if (!architecture) return std::unexpected(architecture.error());
                    m.installed_size = (*ptab)["installed_size"].value_or(0ULL);
                    m.file = (*ptab)["file"].value_or("");

                    if (auto* deps = ptab->get_as<vendor::toml::array>("dependencies")) {
                        for (auto&& d : *deps) {
                            if (auto str = d.value<std::string_view>()) {
                                m.dependencies.push_back(package::Dependency::parse(*str));
                            }
                        }
                    }
                    if (auto* provs = ptab->get_as<vendor::toml::array>("provides")) {
                        for (auto&& p : *provs) {
                            if (auto str = p.value<std::string_view>()) {
                                m.provides.emplace_back(*str);
                            }
                        }
                    }
                    if (auto* confs = ptab->get_as<vendor::toml::array>("conflicts")) {
                        for (auto&& c : *confs) {
                            if (auto str = c.value<std::string_view>()) {
                                m.conflicts.push_back(package::Dependency::parse(*str));
                            }
                        }
                    }
                    if (auto* tools = ptab->get_as<vendor::toml::array>(
                            "managed_build_tools")) {
                        std::set<std::string> roles;
                        for (auto&& item : *tools) {
                            auto* tool = item.as_table();
                            if (!tool) return std::unexpected(
                                "managed_build_tools index entries must be tables");
                            package::ManagedBuildTool observed;
                            observed.role = (*tool)["role"].value_or("");
                            observed.executable = (*tool)["executable"].value_or("");
                            observed.family = (*tool)["family"].value_or("");
                            observed.version = (*tool)["version"].value_or("");
                            if (auto* parameters = tool->get_as<vendor::toml::array>(
                                    "parameters")) {
                                for (auto&& parameter : *parameters) {
                                    auto value = parameter.value<std::string_view>();
                                    if (!value || value->empty()) return std::unexpected(
                                        "Managed build tool parameters in channel index must be non-empty strings");
                                    observed.parameters.emplace_back(*value);
                                }
                            }
                            const std::string version_argument =
                                (*tool)["version_argument"].value_or("");
                            const bool known =
                                ((observed.role == "cc" || observed.role == "cxx")
                                    && (observed.family == "gcc"
                                        || observed.family == "clang"))
                                || (observed.role == "linker"
                                    && (observed.family == "ld"
                                        || observed.family == "lld"
                                        || observed.family == "mold"))
                                || (observed.role == "rustc"
                                    && observed.family == "rustc");
                            if (!known || observed.executable.empty()
                                || observed.version.empty()
                                || version_argument != "--version") {
                                return std::unexpected(
                                    "Incomplete managed_build_tools entry in channel index");
                            }
                            if (!roles.insert(observed.role).second)
                                return std::unexpected(
                                    "Duplicate managed build tool role in channel index");
                            m.managed_build_tools.push_back(std::move(observed));
                        }
                    }
                    if (!m.name.empty()) {
                        idx.available_packages.push_back(std::move(m));
                    }
                }
            }
        }
        return idx;
    }
};

// ============================================================================
// Installed Sub-Channel Metadata
// ============================================================================

struct InstalledSubChannel {
    ChannelScope scope{ChannelScope::Toolchain};
    std::string category;
    std::string slot;
    std::filesystem::path path;
    bool is_active{false};
};

// ============================================================================
// FHS Profile Symlink Aggregator, Swapper & Environment Hook Generator
// ============================================================================

class ProfileManager {
public:
    static std::vector<InstalledSubChannel> list_installed_subchannels(const std::filesystem::path& sysroot = "/") {
        std::vector<InstalledSubChannel> list;

        auto scan_scope_dir = [&](const std::filesystem::path& base_dir, ChannelScope sc) {
            if (!std::filesystem::exists(base_dir)) return;
            for (const auto& cat_entry : std::filesystem::directory_iterator(base_dir)) {
                if (!cat_entry.is_directory()) continue;
                std::string cat_name = cat_entry.path().filename().string();
                for (const auto& slot_entry : std::filesystem::directory_iterator(cat_entry.path())) {
                    if (!slot_entry.is_directory()) continue;
                    std::string slot_name = slot_entry.path().filename().string();

                    InstalledSubChannel sc_info;
                    sc_info.scope = sc;
                    sc_info.category = cat_name;
                    sc_info.slot = slot_name;
                    sc_info.path = slot_entry.path();
                    list.push_back(std::move(sc_info));
                }
            }
        };

        scan_scope_dir(sysroot / "opt/channels", ChannelScope::Toolchain);
        scan_scope_dir(sysroot / "usr/lib/runtimes", ChannelScope::Runtime);
        return list;
    }

    static std::expected<void, std::string> switch_active_toolchain(
        const std::filesystem::path& sysroot,
        std::string_view category,
        std::string_view slot) 
    {
        std::filesystem::path toolchain_dir = sysroot / "opt/channels" / category / slot;
        if (!std::filesystem::exists(toolchain_dir)) {
            return std::unexpected(std::format("Toolchain '{}:{}' is not installed at {}", category, slot, toolchain_dir.string()));
        }

        std::filesystem::path profile_bin = sysroot / "etc/sage/profiles/default/bin";
        std::filesystem::path profile_lib = sysroot / "etc/sage/profiles/default/lib";
        std::filesystem::path profile_runtimes = sysroot / "etc/sage/profiles/default/runtimes";

        std::filesystem::create_directories(profile_bin);
        std::filesystem::create_directories(profile_lib);
        std::filesystem::create_directories(profile_runtimes);

        // Symlink targets and ld.so.conf paths must be chroot-relative:
        // when installed with `--root /mnt` the toolchain lives at
        // /mnt/opt/channels/..., but inside the chroot it is /opt/channels/...
        auto chroot_abs = [&](const std::filesystem::path& p) -> std::filesystem::path {
            return std::filesystem::path("/") / p.lexically_relative(sysroot);
        };

        auto src_bin = toolchain_dir / "bin";
        if (std::filesystem::exists(src_bin)) {
            for (const auto& entry : std::filesystem::directory_iterator(src_bin)) {
                if (entry.is_regular_file() || entry.is_symlink()) {
                    auto dest_link = profile_bin / entry.path().filename();
                    std::error_code ec;
                    std::filesystem::remove(dest_link, ec);
                    std::filesystem::create_symlink(chroot_abs(entry.path()), dest_link, ec);
                }
            }
        }

        // Aggregate shared libraries (libstdc++, libgcc_s, etc.) into profile/lib
        auto src_lib = toolchain_dir / "lib";
        if (std::filesystem::exists(src_lib)) {
            for (const auto& entry : std::filesystem::directory_iterator(src_lib)) {
                auto fname = entry.path().filename().string();
                if ((entry.is_regular_file() || entry.is_symlink()) && fname.find(".so") != std::string::npos) {
                    auto dest_link = profile_lib / fname;
                    std::error_code ec;
                    std::filesystem::remove(dest_link, ec);
                    std::filesystem::create_symlink(chroot_abs(entry.path()), dest_link, ec);
                }
            }
            // Also scan lib64/ if present
            auto src_lib64 = toolchain_dir / "lib64";
            if (std::filesystem::exists(src_lib64)) {
                for (const auto& entry : std::filesystem::directory_iterator(src_lib64)) {
                    auto fname = entry.path().filename().string();
                    if ((entry.is_regular_file() || entry.is_symlink()) && fname.find(".so") != std::string::npos) {
                        auto dest_link = profile_lib / fname;
                        std::error_code ec;
                        std::filesystem::remove(dest_link, ec);
                        std::filesystem::create_symlink(chroot_abs(entry.path()), dest_link, ec);
                    }
                }
            }
        }

        // Register toolchain lib path in /etc/ld.so.conf.d/ for dynamic linker
        {
            auto ld_conf_dir = sysroot / "etc/ld.so.conf.d";
            std::filesystem::create_directories(ld_conf_dir);
            auto conf_file = ld_conf_dir / std::format("sage-{}-{}.conf", category, slot);
            std::ofstream ofs(conf_file);
            if (ofs.is_open()) {
                ofs << chroot_abs(toolchain_dir / "lib").string() << "\n";
                auto lib64 = toolchain_dir / "lib64";
                if (std::filesystem::exists(lib64)) {
                    ofs << chroot_abs(lib64).string() << "\n";
                }
            }
        }

        // Special compiler alias mapping
        if (category == "llvm" || category == "clang") {
            std::error_code ec;
            std::filesystem::remove(profile_bin / "cc", ec);
            std::filesystem::remove(profile_bin / "c++", ec);
            if (std::filesystem::exists(src_bin / "clang")) {
                std::filesystem::create_symlink(chroot_abs(src_bin / "clang"), profile_bin / "cc", ec);
            }
            if (std::filesystem::exists(src_bin / "clang++")) {
                std::filesystem::create_symlink(chroot_abs(src_bin / "clang++"), profile_bin / "c++", ec);
            }
        } else if (category == "gcc") {
            std::error_code ec;
            std::filesystem::remove(profile_bin / "cc", ec);
            std::filesystem::remove(profile_bin / "c++", ec);
            if (std::filesystem::exists(src_bin / "gcc")) {
                std::filesystem::create_symlink(chroot_abs(src_bin / "gcc"), profile_bin / "cc", ec);
            }
            if (std::filesystem::exists(src_bin / "g++")) {
                std::filesystem::create_symlink(chroot_abs(src_bin / "g++"), profile_bin / "c++", ec);
            }
        } else if (category == "java") {
            std::error_code ec;
            auto java_home_link = profile_runtimes / "java";
            std::filesystem::remove(java_home_link, ec);
            std::filesystem::create_symlink(chroot_abs(toolchain_dir), java_home_link, ec);
        }

        util::log_success("Switched active toolchain to '{}:{}'", category, slot);
        return {};
    }

    static std::map<std::string, std::string> generate_shell_env(
        const std::filesystem::path& sysroot,
        const std::vector<SubChannelSpec>& requested_specs) 
    {
        std::map<std::string, std::string> env;
        std::vector<std::string> path_entries;
        std::vector<std::string> lib_entries;
        std::vector<std::string> include_entries;
        std::vector<std::string> pkgconfig_entries;

        for (const auto& spec : requested_specs) {
            std::filesystem::path root;
            if (spec.scope == ChannelScope::Toolchain) {
                root = sysroot / "opt/channels" / spec.category / spec.slot;
            } else if (spec.scope == ChannelScope::Runtime) {
                root = sysroot / "usr/lib/runtimes" / spec.category / spec.slot;
            } else {
                continue;
            }

            if (!std::filesystem::exists(root)) continue;

            if (std::filesystem::exists(root / "bin")) {
                path_entries.push_back((root / "bin").string());
            }
            if (std::filesystem::exists(root / "lib")) {
                lib_entries.push_back((root / "lib").string());
                if (std::filesystem::exists(root / "lib/pkgconfig")) {
                    pkgconfig_entries.push_back((root / "lib/pkgconfig").string());
                }
            }
            if (std::filesystem::exists(root / "include")) {
                include_entries.push_back((root / "include").string());
            }

            // Category specific variable synthesis
            if (spec.category == "llvm" || spec.category == "clang") {
                env["CC"] = (root / "bin/clang").string();
                env["CXX"] = (root / "bin/clang++").string();
            } else if (spec.category == "gcc") {
                env["CC"] = (root / "bin/gcc").string();
                env["CXX"] = (root / "bin/g++").string();
            } else if (spec.category == "java") {
                env["JAVA_HOME"] = root.string();
            } else if (spec.category == "cuda") {
                env["CUDA_HOME"] = root.string();
                env["CUDA_PATH"] = root.string();
            }
        }

        if (!path_entries.empty()) {
            env["PATH"] = util::join(path_entries, ":");
        }
        if (!lib_entries.empty()) {
            env["LD_LIBRARY_PATH"] = util::join(lib_entries, ":");
        }
        if (!include_entries.empty()) {
            env["CPATH"] = util::join(include_entries, ":");
        }
        if (!pkgconfig_entries.empty()) {
            env["PKG_CONFIG_PATH"] = util::join(pkgconfig_entries, ":");
        }

        return env;
    }

    static std::expected<void, std::string> regenerate_fhs_profile(
        const std::filesystem::path& sysroot = "/",
        const std::vector<Channel>& active_channels = {}) 
    {
        std::filesystem::path profile_dir = sysroot / "etc/sage/profiles/default";
        std::filesystem::path bin_dir = profile_dir / "bin";
        std::filesystem::path lib_dir = profile_dir / "lib";
        std::filesystem::path include_dir = profile_dir / "include";

        std::filesystem::create_directories(bin_dir);
        std::filesystem::create_directories(lib_dir);
        std::filesystem::create_directories(include_dir);

        // Toolchain activation writes chroot-absolute links here. Package
        // removal may delete their targets before this regeneration runs, so
        // discard only dangling generated links and preserve every live one.
        auto prune_dangling_links = [&](const std::filesystem::path& dir) {
            if (!std::filesystem::is_directory(dir)) return;
            for (const auto& entry : std::filesystem::directory_iterator(dir)) {
                std::error_code ec;
                if (!entry.is_symlink(ec) || ec) continue;
                auto target = std::filesystem::read_symlink(entry.path(), ec);
                if (ec) continue;
                auto anchored_target = target.is_absolute()
                    ? sysroot / target.relative_path()
                    : entry.path().parent_path() / target;
                if (!std::filesystem::exists(anchored_target, ec) && !ec) {
                    std::filesystem::remove(entry.path(), ec);
                }
            }
        };
        prune_dangling_links(bin_dir);
        prune_dangling_links(lib_dir);
        prune_dangling_links(include_dir);
        prune_dangling_links(profile_dir / "runtimes");

        // Aggregate symlinks from active toolchains & runtimes in priority order
        auto chroot_abs = [&](const std::filesystem::path& p) -> std::filesystem::path {
            return std::filesystem::path("/") / p.lexically_relative(sysroot);
        };
        for (const auto& ch : active_channels) {
            if (!ch.enabled || ch.scope == ChannelScope::System) continue;

            auto ch_root = ch.resolve_target_root(sysroot);
            auto ch_bin = ch_root / "bin";
            if (std::filesystem::exists(ch_bin)) {
                for (const auto& entry : std::filesystem::directory_iterator(ch_bin)) {
                    if (entry.is_regular_file() || entry.is_symlink()) {
                        auto dest_link = bin_dir / entry.path().filename();
                        std::error_code ec;
                        std::filesystem::remove(dest_link, ec);
                        std::filesystem::create_symlink(chroot_abs(entry.path()), dest_link, ec);
                    }
                }
            }

            auto ch_lib = ch_root / "lib";
            if (std::filesystem::exists(ch_lib)) {
                for (const auto& entry : std::filesystem::directory_iterator(ch_lib)) {
                    if (entry.is_regular_file() || entry.is_symlink()) {
                        auto dest_link = lib_dir / entry.path().filename();
                        std::error_code ec;
                        std::filesystem::remove(dest_link, ec);
                        std::filesystem::create_symlink(chroot_abs(entry.path()), dest_link, ec);
                    }
                }
            }
        }

        // Generate /etc/profile.d/sage-channels.sh
        std::filesystem::path profile_d = sysroot / "etc/profile.d";
        std::filesystem::create_directories(profile_d);

        std::filesystem::path sh_path = profile_d / "sage-channels.sh";
        std::ofstream sh(sh_path);
        if (!sh.is_open()) {
            return std::unexpected("Cannot write /etc/profile.d/sage-channels.sh");
        }

        sh << "#!/bin/sh\n";
        sh << "# Auto-generated by Sage Package Manager Channel Runtime\n\n";
        sh << "if [ -d \"/etc/sage/profiles/default/bin\" ]; then\n";
        sh << "    case \":$PATH:\" in\n";
        sh << "        *:/etc/sage/profiles/default/bin:*);;\n";
        sh << "        *) export PATH=\"/etc/sage/profiles/default/bin:$PATH\";;\n";
        sh << "    esac\n";
        sh << "fi\n\n";

        sh << "if [ -d \"/etc/sage/profiles/default/lib\" ]; then\n";
        sh << "    case \":$LD_LIBRARY_PATH:\" in\n";
        sh << "        *:/etc/sage/profiles/default/lib:*);;\n";
        sh << "        *) export LD_LIBRARY_PATH=\"/etc/sage/profiles/default/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}\";;\n";
        sh << "    esac\n";
        sh << "fi\n\n";

        sh << "if [ -d \"/etc/sage/profiles/default/runtimes/java\" ]; then\n";
        sh << "    export JAVA_HOME=\"/etc/sage/profiles/default/runtimes/java\"\n";
        sh << "fi\n";

        sh.close();
        std::error_code ec;
        std::filesystem::permissions(sh_path, 
            std::filesystem::perms::owner_all | 
            std::filesystem::perms::group_read | std::filesystem::perms::group_exec | 
            std::filesystem::perms::others_read | std::filesystem::perms::others_exec, 
            ec);

        return {};
    }

    static std::expected<ChannelIndex, std::string> sync_channel(
        const Channel& ch,
        const std::filesystem::path& cache_dir,
        IndexRefresh refresh = IndexRefresh::Always,
        bool persist_cache = true)
    {
        const std::filesystem::path local_idx =
            cache_dir / "channels" / (ch.name + ".toml");
        const bool remote = vendor::curl::parse_local_url(ch.url).empty();
        if (remote && refresh == IndexRefresh::IfMissing
            && std::filesystem::exists(local_idx)) {
            std::ifstream cached(local_idx);
            std::stringstream text;
            text << cached.rdbuf();
            if (auto parsed = ChannelIndex::parse_toml(text.str()); parsed) {
                return parsed;
            }
            // A corrupt/incomplete cache is never authoritative: fetch and
            // atomically replace it below.
        }

        std::string index_url = ch.url;
        if (!index_url.ends_with('/')) index_url += '/';
        index_url += "index.toml";

        auto content = vendor::curl::fetch_string(index_url);
        if (!content) {
            return std::unexpected("Failed to fetch channel index from " + index_url + ": " + content.error());
        }

        auto index_res = ChannelIndex::parse_toml(*content);
        if (!index_res) return index_res;

        if (persist_cache) {
            // Cache index locally, best effort: a read-only or missing cache
            // directory must not fail the sync itself.
            std::error_code cache_ec;
            std::filesystem::create_directories(local_idx.parent_path(), cache_ec);
            if (!cache_ec) {
                const auto temporary = local_idx.string() + ".part";
                std::ofstream f(temporary, std::ios::binary | std::ios::trunc);
                if (f.is_open()) {
                    f << *content;
                    f.close();
                    std::filesystem::rename(temporary, local_idx, cache_ec);
                    if (cache_ec) std::filesystem::remove(temporary, cache_ec);
                }
            }
        }

        return index_res;
    }
};

} // namespace sage::channel
