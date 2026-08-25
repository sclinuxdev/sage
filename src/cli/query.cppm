export module sage.cli.query;

// Read-mostly inspection commands backed by the LMDB registry.
import std;
import sage;

import sage.cli;

namespace sage::cli {

namespace {

// Substring match, or glob when the pattern carries a wildcard. A bare name is
// far more often meant as "anything containing this" than as an exact match,
// but `sage count 'python-*'` should still mean what it looks like.
bool inventory_matches(std::string_view name, std::string_view pattern) {
    if (pattern.empty()) return true;
    if (pattern.find_first_of("*?[") == std::string_view::npos) {
        return name.find(pattern) != std::string_view::npos;
    }
    return sage::util::glob_match(pattern, name);
}

std::expected<std::vector<sage::package::PackageManifest>, std::string>
collect_installed(const CliOptions& opts, std::string_view pattern) {
    auto cfg_res = sage::config::SystemConfig::load_from_root(opts.target_root);
    auto db_res = sage::db::Database::open(
        cfg_res ? cfg_res->db_path : std::filesystem::path("/var/lib/sage/data.mdb"), true);
    if (!db_res) return std::unexpected(db_res.error());

    auto list = db_res->list_installed_packages();
    if (!list) return std::unexpected(list.error());
    std::erase_if(*list, [&](const auto& pkg) { return !inventory_matches(pkg.name, pattern); });
    std::ranges::sort(*list, {}, &sage::package::PackageManifest::name);
    return std::move(*list);
}

} // namespace

export int cmd_list(const CliOptions& opts) {
    bool quiet = false;
    std::string pattern;
    for (const auto& a : opts.args) {
        if (a == "--quiet" || a == "-q") quiet = true;
        else if (a == "--help" || a == "-h") {
            std::println("Usage: sage list [-q|--quiet] [PATTERN]");
            return 0;
        } else if (pattern.empty()) pattern = a;
    }

    auto list_res = collect_installed(opts, pattern);
    if (!list_res) {
        // Nothing is installed on a root that has no database yet. That is a
        // legitimate answer to "what is installed", not a failure, so -q stays
        // silent and scripts reading it see an empty list rather than an error.
        if (!quiet) sage::util::log_warn("Database not yet initialized or inaccessible: {}", list_res.error());
        return quiet ? 0 : 0;
    }
    const auto& list = *list_res;

    // -q prints bare names, one per line, so the output can be piped straight
    // into xargs without anything having to strip decoration off it.
    if (quiet) {
        for (const auto& pkg : list) std::println("{}", pkg.name);
        return 0;
    }

    if (list.empty()) {
        std::println("No installed packages in '{}'{}", opts.target_root.string(),
            pattern.empty() ? "" : std::format(" matching '{}'", pattern));
        return 0;
    }

    std::uintmax_t total_size = 0;
    for (const auto& pkg : list) total_size += pkg.installed_size;

    std::println("Installed packages in '{}'{}:", opts.target_root.string(),
        pattern.empty() ? "" : std::format(" matching '{}'", pattern));
    for (const auto& pkg : list) {
        std::println("  {:<28} {:<16} [{}]", pkg.name, pkg.version.to_string(), pkg.channel);
    }
    std::println("");
    std::println("{} package(s), {} installed", list.size(), sage::util::format_size(total_size));
    return 0;
}

export int cmd_count(const CliOptions& opts) {
    std::string pattern;
    for (const auto& a : opts.args) {
        if (a == "--help" || a == "-h") {
            std::println("Usage: sage count [PATTERN]");
            return 0;
        }
        if (pattern.empty()) pattern = a;
    }

    auto list_res = collect_installed(opts, pattern);
    // A bare number and nothing else: this exists to be captured in a shell
    // substitution, so it must not print a word even when the root is empty.
    std::println("{}", list_res ? list_res->size() : 0u);
    return 0;
}

export int cmd_query(const CliOptions& opts) {
    if (opts.args.empty()) {
        std::println("Usage: sage query [installed|count|info <pkg>|owner <path>|files <pkg>|capabilities]");
        return 1;
    }

    std::string sub = opts.args[0];
    auto cfg_res = sage::config::SystemConfig::load_from_root(opts.target_root);
    auto db_res = sage::db::Database::open(cfg_res ? cfg_res->db_path : std::filesystem::path("/var/lib/sage/data.mdb"), true);
    if (!db_res) {
        sage::util::log_warn("Database not yet initialized or inaccessible: {}", db_res.error());
        return 0;
    }
    auto& db = *db_res;

    if (sub == "files" && opts.args.size() >= 2) {
        std::string pkg_name = opts.args[1];
        auto pkg_res = db.get_package(pkg_name);
        if (!pkg_res) {
            sage::util::log_error(
                "Failed to read package '{}': {}", pkg_name, pkg_res.error());
            return 1;
        }
        if (!*pkg_res) {
            sage::util::log_error("Package '{}' is not installed", pkg_name);
            return 1;
        }
        const auto& pkg = **pkg_res;
        if (pkg.files.empty()) {
            // Packages registered before files.idx existed have paths in the
            // files table but no per-file metadata; fall back to those rather
            // than claiming the package owns nothing.
            auto paths = db.get_package_files(pkg_name);
            if (!paths) {
                sage::util::log_error(
                    "Failed to read files for '{}': {}", pkg_name, paths.error());
                return 1;
            }
            if (paths->empty()) {
                std::println("{} owns no files", pkg_name);
                return 0;
            }
            sage::util::log_warn("'{}' predates files.idx: listing paths without hashes", pkg_name);
            for (const auto& path : *paths) std::println("/{}", path);
            return 0;
        }
        for (const auto& f : pkg.files) {
            std::println("{:<4} {:>6o} {:>10}  {:<64}  /{}",
                sage::package::to_string(f.type), f.mode, f.size,
                f.sha256.empty() ? "-" : f.sha256, f.path);
        }
        return 0;
    }

    if (sub == "capabilities") {
        auto list = db.list_installed_packages();
        if (!list) {
            sage::util::log_error("Failed to list installed packages: {}", list.error());
            return 1;
        }
        std::map<std::string, std::vector<std::string>> by_cap;
        for (const auto& pkg : *list) {
            for (const auto& prov : pkg.provides) {
                if (!prov.starts_with("virtual/")) continue;
                by_cap[prov].push_back(pkg.name);
            }
        }
        auto cfg = cfg_res ? *cfg_res : sage::config::SystemConfig::default_config();
        std::println("Virtual capabilities in '{}':", opts.target_root.string());
        for (const auto& [cap, providers] : by_cap) {
            auto bound = cfg.provider_for(cap);
            std::println("  • {:<34} {:<10} providers: {}{}",
                cap,
                sage::config::to_string(cfg.capability_kind(cap)),
                sage::util::join(providers, ", "),
                bound ? std::format("  (bound: {})", *bound) : std::string{});
        }
        if (by_cap.empty()) std::println("  (none)");
        return 0;
    }

    if (sub == "count") {
        auto list = db.list_installed_packages();
        if (!list) {
            sage::util::log_error("Failed to count installed packages: {}", list.error());
            return 1;
        }
        std::println("{}", list->size());
        return 0;
    }

    if (sub == "installed") {
        auto list = db.list_installed_packages();
        if (!list) {
            sage::util::log_error("Installed package database is inconsistent: {}", list.error());
            return 1;
        }
        // LMDB hands these back in key order already, but sorting explicitly
        // keeps the listing stable if the key layout ever changes.
        std::ranges::sort(*list, {}, &sage::package::PackageManifest::name);
        std::println("Installed packages in '{}' ({} total):", opts.target_root.string(), list->size());
        for (const auto& pkg : *list) {
            std::println("  • {:<20} {:<15} [{}]", pkg.name, pkg.version.to_string(), pkg.channel);
        }
    } else if (sub == "info" && opts.args.size() >= 2) {
        std::string pkg_name = opts.args[1];
        auto pkg_res = db.get_package(pkg_name);
        if (!pkg_res) {
            sage::util::log_error("Installed package database is inconsistent: {}", pkg_res.error());
            return 1;
        }
        if (*pkg_res) {
            const auto& pkg = **pkg_res;
            std::println("Package:     {}", pkg.name);
            std::println("Version:     {}", pkg.version.to_string());
            std::println("Channel:     {}", pkg.channel);
            std::println("License:     {}", pkg.license);
            std::println("Description: {}", pkg.description);
            std::println("Provides:    {}", sage::util::join(pkg.provides, ", "));
            if (!pkg.build_producers.empty()) {
                std::println("Build Producers:");
                for (const auto& p : pkg.build_producers) {
                    auto versions = sage::util::join(p.versions, "+");
                    std::println("  • {:<10} {}", p.name,
                        versions.empty() ? "(version unknown)" : versions);
                    if (!p.flags.empty()) {
                        std::println("      flags (artifact-verified): {}", p.flags);
                    }
                }
                if (!pkg.build_flag_passthrough.empty()) {
                    std::println("  Flag passthrough channels: {}",
                        sage::util::join(pkg.build_flag_passthrough, ", "));
                }
            }
        } else {
            sage::util::log_error("Package '{}' is not installed", pkg_name);
            return 1;
        }
    } else if (sub == "owner" && opts.args.size() >= 2) {
        std::string path = opts.args[1];
        auto owners = db.get_path_owners(path);
        if (!owners) {
            sage::util::log_error("Failed to read file ownership: {}", owners.error());
            return 1;
        }
        if (owners->empty()) {
            std::println("No installed package owns {}", path);
        } else {
            std::println("{} is owned by {}", path, sage::util::join(*owners, ", "));
        }
    }
    return 0;
}

// Check installed files against the hashes recorded at install time.
export int cmd_verify(const CliOptions& opts) {
    auto cfg_res = sage::config::SystemConfig::load_from_root(opts.target_root);
    if (!cfg_res) {
        sage::util::log_error("Failed to load configuration: {}", cfg_res.error());
        return 1;
    }
    auto db_res = sage::db::Database::open(cfg_res->db_path, true);
    if (!db_res) {
        sage::util::log_error("Failed to open database at {}: {}", cfg_res->db_path.string(), db_res.error());
        return 1;
    }

    std::vector<sage::package::PackageManifest> targets;
    if (opts.args.empty()) {
        auto installed = db_res->list_installed_packages();
        if (!installed) {
            sage::util::log_error("Failed to list installed packages: {}", installed.error());
            return 1;
        }
        targets = std::move(*installed);
    } else {
        for (const auto& name : opts.args) {
            auto pkg_res = db_res->get_package(name);
            if (!pkg_res) {
                sage::util::log_error("Failed to read package '{}': {}", name, pkg_res.error());
                return 1;
            }
            if (!*pkg_res) {
                sage::util::log_error("Package '{}' is not installed", name);
                return 1;
            }
            targets.push_back(std::move(**pkg_res));
        }
    }

    size_t checked = 0;
    size_t modified = 0;
    size_t missing = 0;
    size_t unhashed = 0;

    for (const auto& pkg : targets) {
        for (const auto& f : pkg.files) {
            if (f.type != sage::package::FileType::Regular) continue;
            // The shared install-info index is appended to by every info
            // page installation; the ownership conflict check already
            // exempts it, so integrity verification must not flag it either.
            if (f.path == "usr/share/info/dir" || f.path.ends_with("/info/dir")) continue;
            // The loader cache is regenerated in place by the ldconfig
            // trigger on every library-touching transaction.
            if (f.path == "etc/ld.so.cache") continue;
            if (f.sha256.empty()) { unhashed++; continue; }

            std::filesystem::path on_disk = opts.target_root / f.path;
            if (!std::filesystem::exists(on_disk)) {
                std::println("missing   {:<20} /{}", pkg.name, f.path);
                missing++;
                continue;
            }
            auto hash = sage::util::compute_file_sha256(on_disk);
            checked++;
            if (!hash) {
                std::println("unreadable {:<20} /{}", pkg.name, f.path);
                missing++;
            } else if (*hash != f.sha256) {
                std::println("modified  {:<20} /{}", pkg.name, f.path);
                modified++;
            }
        }
    }

    if (unhashed > 0) {
        sage::util::log_warn("{} file(s) carry no recorded hash (installed before files.idx existed)", unhashed);
    }
    if (modified == 0 && missing == 0) {
        sage::util::log_success("Verified {} file(s) across {} package(s): all match", checked, targets.size());
        return 0;
    }
    sage::util::log_error("{} modified, {} missing out of {} file(s) checked", modified, missing, checked);
    return 1;
}
export int cmd_status(const CliOptions& opts) {
    auto cfg_res = sage::config::SystemConfig::load_from_root(opts.target_root);
    if (!cfg_res) {
        sage::util::log_error("Failed to load configuration: {}", cfg_res.error());
        return 1;
    }
    const auto& cfg = *cfg_res;
    const bool full = std::ranges::find(opts.args, "--full") != opts.args.end();

    print_banner();
    std::println("");
    std::println("Target Root:   {}", opts.target_root.string());
    std::println("Config Dir:    {}", cfg.config_dir.string());
    std::println("Database:      {}{}", cfg.db_path.string(),
        std::filesystem::exists(cfg.db_path) ? "" : "  (not initialized)");
    std::println("Schema:        v{}", cfg.schema_version);

    std::println("");
    std::println("Active Providers:");
    if (cfg.providers.empty()) {
        std::println("  (none declared)");
    } else {
        for (const auto& [interface_name, provider] : cfg.providers) {
            std::println("  • {:<16} {}", interface_name, provider);
        }
    }

    std::println("");
    std::println("Channels ({}):", cfg.channels.size());
    for (const auto& ch : cfg.channels) {
        std::println("  • {:<15} scope: {:<10} priority: {:<5} {}",
            ch.name, ch.scope, ch.priority, ch.enabled ? "enabled" : "disabled");
    }

    std::println("");
    auto db_res = sage::db::Database::open(cfg.db_path, true);
    if (!db_res) {
        std::println("Installed Packages: (database unavailable)");
        return 0;
    }
    auto installed = db_res->list_installed_packages();
    if (!installed) {
        sage::util::log_error("Installed package database is inconsistent: {}", installed.error());
        return 1;
    }
    std::println("Installed Packages: {}", installed->size());
    if (full) {
        for (const auto& pkg : *installed) {
            std::println("  • {:<20} {:<15} [{}]", pkg.name, pkg.version.to_string(), pkg.channel);
        }
    }

    // Read-only peek at the transaction protocol state. Recovery is never
    // attempted here -- only a mutating command may drive pending
    // operations to their terminal state.
    std::println("");
    auto pending = sage::rebuild::pending_operations(*db_res);
    if (!pending) {
        std::println("Pending Operations: (unavailable)");
    } else if (pending->empty()) {
        std::println("Pending Operations: 0");
    } else {
        std::println("Pending Operations: {}", pending->size());
        for (const auto& op : *pending) {
            std::println("  • {:<34} {:<12} {}", op.id, op.kind, op.phase);
        }
        std::println("  (run 'sage rebuild' to resume; use its --abandon escape if stuck)");
    }
    return 0;
}
// Explicit escape hatch for the staged-transaction state machine: drives every
// pending operation to its terminal state (publish -> postprocess -> retire),
// or -- with --abandon <ID> -- retires one stuck operation after a loud
// warning. This is the only command whose job is recovery itself; install,
// remove and rebuild run the same driver implicitly at entry.
export int cmd_recover(const CliOptions& opts) {
    std::optional<std::string> abandon;
    for (size_t i = 0; i < opts.args.size(); ++i) {
        if (opts.args[i] == "--abandon") {
            if (i + 1 >= opts.args.size()) {
                sage::util::log_error("--abandon requires an operation id (see 'sage status')");
                return 1;
            }
            abandon = opts.args[++i];
        } else {
            sage::util::log_error("Unknown argument for recover: '{}'", opts.args[i]);
            return 1;
        }
    }

    auto cfg_res = sage::config::SystemConfig::load_from_root(opts.target_root);
    if (!cfg_res) {
        sage::util::log_error("Failed to load configuration: {}", cfg_res.error());
        return 1;
    }
    auto db_res = sage::db::Database::open(cfg_res->db_path);
    if (!db_res) {
        sage::util::log_error(
            "Failed to open database at {}: {}", cfg_res->db_path.string(), db_res.error());
        return 1;
    }

    auto resumed = sage::rebuild::resume_pending_operations(
        *db_res, opts.target_root,
        abandon ? std::optional<std::string_view>{*abandon} : std::nullopt);
    if (!resumed) {
        sage::util::log_error("Recovery failed: {}", resumed.error());
        sage::util::log_error(
            "State is left untouched for retry; 'sage recover --abandon <ID>' "
            "is the last resort.");
        return 1;
    }
    if (abandon) {
        sage::util::log_warn(
            "Operation '{}' was abandoned. Database and filesystem may be "
            "inconsistent; verify with 'sage verify'.",
            *abandon);
    }
    sage::util::log_success(
        "Recovery finalized {} operation(s).", resumed->finalized);
    return 0;
}
} // namespace sage::cli
