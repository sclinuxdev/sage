export module sage.repo;

// Channel pool & archive resolution: the metadata pool the solver plans
// against, plus the local archive path backing each candidate. Shared by
// `sage install` and `sage rebuild`.

import std;
import sage.channel;
import sage.config;
import sage.package;
import sage.util;
import sage.vendor.curl;

export namespace sage::repo {

// Priority order breaks equal-version ties, so metadata and payload always
// come from one repository.
struct RepoSnapshot {
    std::vector<package::PackageManifest> pool;
    std::map<package::PackageIdentity, std::filesystem::path> archives;
    // Remote channel URL per archive that is not directly readable from
    // disk; stays empty for file:// channels. ensure_local_archive()
    // consults this to fetch a payload on first use.
    std::map<package::PackageIdentity, std::string> remote_urls;
};

inline std::expected<RepoSnapshot, std::string> fetch_repo_snapshot(
    const config::SystemConfig& cfg,
    std::string_view channel_filter = {},
    bool persist_cache = true)
{
    RepoSnapshot snap;
    auto channel_configs = cfg.channels;
    std::ranges::stable_sort(channel_configs, std::greater{},
        &config::ChannelConfig::priority);

    bool filter_matched = channel_filter.empty();
    for (const auto& ch_cfg : channel_configs) {
        if (!ch_cfg.enabled) continue;
        // --channel narrows the pool to one channel. Installed packages are
        // still added by the caller, so a restricted install can satisfy
        // constraints that are already met on the system.
        if (!channel_filter.empty() && ch_cfg.name != channel_filter) continue;
        filter_matched = true;

        channel::Channel ch;
        ch.name = ch_cfg.name;
        ch.url = ch_cfg.url;
        ch.scope = channel::parse_scope(ch_cfg.scope);
        ch.priority = ch_cfg.priority;

        auto idx_res = channel::ProfileManager::sync_channel(
            ch, cfg.cache_dir, persist_cache);
        if (!idx_res) continue;

        std::filesystem::path dir_base;
        // A channel that is neither file:// nor a bare filesystem path is
        // remote: its payloads must be downloaded into the per-root cache
        // before they can be inspected or unpacked.
        bool remote_channel = !ch.url.starts_with("file://") && !ch.url.starts_with("/");
        if (ch.url.starts_with("file://")) {
            dir_base = std::filesystem::path(ch.url.substr(7));
        } else if (ch.url.starts_with("/")) {
            dir_base = std::filesystem::path(ch.url);
        } else {
            dir_base = cfg.cache_dir / "pkg";
        }

        for (const auto& pkg : idx_res->available_packages) {
            if (!package::package_architecture_matches(pkg.arch, cfg.architecture)) continue;
            snap.pool.push_back(pkg);
            std::filesystem::path local_p;
            if (!pkg.file.empty()) {
                local_p = dir_base / pkg.file;
                if (remote_channel) {
                    std::string base{ch.url};
                    if (base.ends_with('/')) base.pop_back();
                    snap.remote_urls[package::package_identity(pkg)] =
                        base + "/" + pkg.file;
                }
            } else {
                // Legacy naming fallbacks for hand-rolled repositories.
                local_p = dir_base / std::format(
                    "{}-{}-{}-{}.pkg.tar.zst", pkg.name, pkg.version.ver, pkg.version.rel, pkg.arch);
                if (!std::filesystem::exists(local_p)) {
                    local_p = dir_base / std::format("{}-{}-{}.pkg.tar.zst", pkg.name, pkg.version.ver, pkg.version.rel);
                }
                if (!std::filesystem::exists(local_p)) {
                    local_p = dir_base / std::format("{}-{}.pkg.tar.zst", pkg.name, pkg.version.ver);
                }
            }
            snap.archives.try_emplace(package::package_identity(pkg), std::move(local_p));
        }
    }

    if (!filter_matched) {
        return std::unexpected(std::format(
            "No enabled channel named '{}' is configured for '{}'",
            channel_filter, cfg.root_dir.string()));
    }
    return snap;
}

// Resolve a selected package to a readable local archive, fetching it from
// its channel first when the snapshot came from a remote URL. Downloads go
// through vendor::curl::download_file, which stages to a .part file and only
// renames after a fully successful transfer, so the cache never holds a
// truncated archive that a later exists() check would trust.
inline std::expected<std::filesystem::path, std::string>
ensure_local_archive(const RepoSnapshot& snap, const package::PackageIdentity& identity) {
    auto archive_it = snap.archives.find(identity);
    if (archive_it == snap.archives.end()) {
        return std::unexpected(std::format(
            "No package archive available for '{}' {} in any configured channel",
            identity.name, identity.version.to_string()));
    }
    if (std::filesystem::exists(archive_it->second)) return archive_it->second;

    auto url_it = snap.remote_urls.find(identity);
    if (url_it == snap.remote_urls.end()) {
        return std::unexpected(std::format(
            "Package archive for '{}' not found at {}",
            identity.name, archive_it->second.string()));
    }
    util::log_info("  ↓ fetching {} from {}",
        archive_it->second.filename().string(), url_it->second);
    auto downloaded = vendor::curl::download_file(url_it->second, archive_it->second);
    if (!downloaded) {
        return std::unexpected(std::format(
            "Failed to download archive for '{}': {}", identity.name, downloaded.error()));
    }
    return archive_it->second;
}

} // namespace sage::repo
