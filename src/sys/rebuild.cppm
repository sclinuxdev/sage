export module sage.rebuild;

import std;
import sage.archive;
import sage.channel;
import sage.config;
import sage.package;
import sage.service;
import sage.db;
import sage.solver;
import sage.util;
import sage.vendor.curl;

export namespace sage::rebuild {

using std::size_t;

// ============================================================================
// Channel Pool & Archive Resolution
// ============================================================================
//
// `sage install` and `sage rebuild` both need the same two things from the
// configured channels: the metadata pool the solver plans against, and the
// local archive path backing each candidate. Priority order breaks
// equal-version ties, so metadata and payload always come from one repository.

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

struct ProviderSwap {
    std::string iface;
    std::string current_provider;
    std::string target_provider;
};

struct PlannedPackageRemoval {
    std::string name;
    std::optional<package::PackageIdentity> expected_identity;
};

struct ReconcilePlan {
    std::vector<ProviderSwap> swaps;
    std::vector<package::PackageManifest> packages_to_install;
    std::vector<PlannedPackageRemoval> packages_to_remove;
    // Channel snapshot fetched during planning; execute unpacks installs from
    // plan.repos.archives, so planning and payload can never disagree.
    RepoSnapshot repos;
    service::InitType target_init{service::InitType::OpenRC};
    bool has_changes{false};
};

// ============================================================================
// Post-transaction Triggers
// ============================================================================
//
// A transaction reports what it touched; triggers decide what has to be
// regenerated. Two things can fire one:
//
//   on_paths      a file under the prefix was written or removed
//   on_capability a package taking part in the transaction provides it
//
// The capability form is what makes the kernel case work. Installing a kernel
// must rebuild the initramfs -- but *which* tool does that is the admin's
// choice, so the trigger names the capability virtual/initramfs-generator and
// sage resolves it at fire time through the active provider's
// [[capability_hooks]] entry. A recipe never writes "mkinitcpio -P".

struct TriggerContext {
    std::filesystem::path sysroot{"/"};
    // Files added, replaced or deleted by this transaction.
    std::vector<package::FileEntry> touched_files;
    // Packages installed or upgraded by this transaction.
    std::vector<package::PackageManifest> transaction_packages;
    // Every package present after the transaction. Both the source of declared
    // triggers and the pool capability hooks are resolved against.
    std::vector<package::PackageManifest> installed_packages;
    // Capability -> provider bindings from system.toml.
    std::map<std::string, std::string> providers;
    bool dry_run{false};
};

class TriggerEngine {
public:
    // Built-in triggers. These are the ones that cannot be package-declared
    // without a bootstrap problem -- ldconfig has to run before the package
    // that would have declared it is usable -- plus the two capability-driven
    // ones, which are kept here so a kernel package that forgot to declare
    // them still gets a working boot.
    static std::vector<package::Trigger> builtin_triggers() {
        std::vector<package::Trigger> t;

        t.push_back(package::Trigger{
            .name = "ldconfig",
            .on_paths = {"usr/lib/", "lib/"},
            .exec = "/usr/bin/ldconfig",
            .priority = 10,
        });
        t.push_back(package::Trigger{
            .name = "ca-certificates",
            .on_paths = {"etc/ssl/certs/", "usr/share/ca-certificates/"},
            .exec = "/usr/bin/update-ca-certificates",
            .priority = 20,
        });
        t.push_back(package::Trigger{
            .name = "mime-database",
            .on_paths = {"usr/share/mime/"},
            .exec = "/usr/bin/update-mime-database",
            .args = {"/usr/share/mime"},
            .priority = 20,
        });
        // Kernel installed -> rebuild the initramfs, then point the bootloader
        // at it. Both resolve through whichever package currently provides the
        // capability; if nothing does, they silently do not fire.
        t.push_back(package::Trigger{
            .name = "initramfs",
            .on_paths = {"usr/lib/modules/", "boot/vmlinuz"},
            .on_capability = {"virtual/kernel"},
            .run_capability = "virtual/initramfs-generator",
            .priority = 60,
        });
        t.push_back(package::Trigger{
            .name = "bootloader",
            .on_paths = {"boot/vmlinuz"},
            .on_capability = {"virtual/kernel"},
            .run_capability = "virtual/bootloader",
            .priority = 70,
        });

        return t;
    }

    static std::expected<void, std::string> run(const TriggerContext& ctx) {
        // Capabilities brought in by this transaction, for on_capability.
        std::set<std::string> txn_capabilities;
        for (const auto& pkg : ctx.transaction_packages) {
            for (const auto& prov : pkg.provides) {
                txn_capabilities.insert(prov);
            }
        }

        std::vector<package::Trigger> candidates = builtin_triggers();
        for (const auto& pkg : ctx.installed_packages) {
            candidates.insert(candidates.end(), pkg.triggers.begin(), pkg.triggers.end());
        }

        std::ranges::stable_sort(candidates, {}, &package::Trigger::priority);

        // One resolved command runs at most once per transaction, however many
        // triggers and however many touched files ask for it.
        std::set<std::string> already_run;

        for (const auto& trig : candidates) {
            if (!fires(trig, ctx, txn_capabilities)) continue;

            auto cmd = resolve_command(trig, ctx);
            if (!cmd) continue;

            if (!already_run.insert(*cmd).second) continue;
            auto result = execute(*cmd, trig.name, ctx);
            if (!result) return result;
        }
        return {};
    }

private:
    static bool fires(const package::Trigger& trig,
                      const TriggerContext& ctx,
                      const std::set<std::string>& txn_capabilities)
    {
        for (const auto& cap : trig.on_capability) {
            if (txn_capabilities.contains(cap)) return true;
        }
        for (const auto& f : ctx.touched_files) {
            if (!trig.matches_path(f.path)) continue;
            // The historical ldconfig rule only cared about shared objects,
            // not about every file that happens to live under usr/lib.
            if (trig.name == "ldconfig" && f.path.find(".so") == std::string::npos) continue;
            return true;
        }
        return false;
    }

    // Resolve a trigger to a concrete command line inside the target root, or
    // nothing when the capability has no provider / no hook.
    static std::optional<std::string> resolve_command(const package::Trigger& trig,
                                                      const TriggerContext& ctx)
    {
        if (trig.run_capability.empty()) {
            if (trig.exec.empty()) return std::nullopt;
            std::string cmd = trig.exec;
            for (const auto& a : trig.args) cmd += " " + a;
            return cmd;
        }

        const package::CapabilityHook* hook = nullptr;

        // The admin's binding wins, whether the capability is exclusive or
        // just has a declared default.
        if (auto it = ctx.providers.find(trig.run_capability); it != ctx.providers.end()) {
            for (const auto& pkg : ctx.installed_packages) {
                if (pkg.name != it->second) continue;
                hook = pkg.hook_for(trig.run_capability);
                break;
            }
        }

        // Otherwise: any installed provider that publishes a hook.
        if (!hook) {
            for (const auto& pkg : ctx.installed_packages) {
                if (!pkg.provides_capability(trig.run_capability)) continue;
                if (auto* h = pkg.hook_for(trig.run_capability)) {
                    hook = h;
                    break;
                }
            }
        }

        if (!hook) {
            // A capability nobody provides is the ordinary case -- most roots
            // never install a bootloader -- and warning about it on every
            // transaction trains people to ignore trigger warnings. Only a
            // provider that is installed but ships no hook is worth reporting:
            // that one is a packaging mistake.
            bool provided = std::ranges::any_of(ctx.installed_packages,
                [&](const auto& pkg) { return pkg.provides_capability(trig.run_capability); });
            if (provided) {
                util::log_warn("Trigger '{}' wants capability '{}': it is provided, but no provider declares a capability hook -- skipping",
                    trig.name, trig.run_capability);
            }
            return std::nullopt;
        }

        std::string cmd = hook->command_line();
        for (const auto& a : trig.args) cmd += " " + a;
        return cmd;
    }

    // Post-transaction tools are provided BY the target and resolve their own
    // paths against "/". For a sysroot other than "/" they must run inside the
    // chroot: host-side they would rebuild the HOST's caches and leave the
    // sysroot's untouched, and the host loader may not even match the target's
    // glibc. Every path here is therefore relative to the target root.
    static std::expected<void, std::string> execute(
        const std::string& cmd,
        std::string_view trigger_name,
        const TriggerContext& ctx)
    {
        std::string exec_path = cmd.substr(0, cmd.find(' '));
        auto within_root = [&](std::filesystem::path path) {
            path = (std::filesystem::path("/") / path.relative_path())
                       .lexically_normal().relative_path();
            for (unsigned links = 0; links < 40; ++links) {
                std::filesystem::path resolved;
                bool followed = false;
                for (auto component = path.begin(); component != path.end(); ++component) {
                    resolved /= *component;
                    std::error_code ec;
                    auto status = std::filesystem::symlink_status(ctx.sysroot / resolved, ec);
                    if (ec || status.type() == std::filesystem::file_type::not_found) return false;
                    if (status.type() != std::filesystem::file_type::symlink) continue;
                    auto target = std::filesystem::read_symlink(ctx.sysroot / resolved, ec);
                    if (ec) return false;
                    std::filesystem::path remainder;
                    for (auto rest = std::next(component); rest != path.end(); ++rest) {
                        remainder /= *rest;
                    }
                    path = target.is_absolute() ? target.relative_path()
                                                : resolved.parent_path() / target;
                    if (!remainder.empty()) path /= remainder;
                    path = (std::filesystem::path("/") / path)
                               .lexically_normal().relative_path();
                    followed = true;
                    break;
                }
                if (!followed) return true;
            }
            return false;
        };
        const bool executable_exists = ctx.sysroot == "/"
            ? std::filesystem::exists(exec_path)
            : within_root(exec_path);
        if (!executable_exists) {
            return std::unexpected(std::format(
                "Required executable '{}' for trigger '{}' is missing",
                exec_path, trigger_name));
        }

        std::string full = (ctx.sysroot == "/")
            ? cmd
            : std::format("chroot {} {}", ctx.sysroot.string(), cmd);

        if (ctx.dry_run) {
            util::log_info("Would run trigger '{}': {}", trigger_name, full);
            return {};
        }

        util::log_info("Running trigger '{}': {}", trigger_name, full);
        int ret = std::system(full.c_str());
        if (ret != 0) {
            return std::unexpected(std::format(
                "Trigger '{}' failed (status {}): {}", trigger_name, ret, full));
        }
        return {};
    }
};

class ReconcileEngine {
public:
    static std::expected<ReconcilePlan, std::string> calculate_diff(
        db::Database& db,
        const config::SystemConfig& desired_config,
        bool persist_cache = true)
    {
        ReconcilePlan plan;
        auto current_providers = db.get_all_system_providers();
        if (!current_providers) {
            return std::unexpected(
                "Failed to read current system providers: " + current_providers.error());
        }

        // 1. Calculate provider diffs.
        //
        // Only *exclusive* capabilities take part: they are the ones where at
        // most one provider may exist, so a changed binding means packages
        // must actually be swapped. Retargeting a shared default such as
        // virtual/initramfs-generator changes which tool later transactions
        // call, not what is installed -- reconciling on it would uninstall a
        // perfectly valid coexisting provider.
        for (const auto& [iface, target_prov] : desired_config.exclusive_providers()) {
            std::string cur = current_providers->contains(iface) ? current_providers->at(iface) : "";
            if (cur != target_prov) {
                plan.swaps.push_back(ProviderSwap{
                    .iface = iface,
                    .current_provider = cur,
                    .target_provider = target_prov
                });
                plan.has_changes = true;
            }
        }

        // Determine active init system
        std::string init_prov = desired_config.providers.contains("virtual/init") ? 
                                desired_config.providers.at("virtual/init") : "openrc";
        plan.target_init = service::parse_init_type(init_prov);
        if (plan.target_init == service::InitType::Unknown) {
            return std::unexpected(std::format(
                "Unsupported virtual/init provider '{}'", init_prov));
        }

        if (!plan.has_changes) {
            return plan;
        }

        // 2. Fetch channels: the solve pool and the archives behind it. Only
        // reached when a swap is pending, so the common no-op reconcile never
        // touches the network.
        auto snapshot_res = fetch_repo_snapshot(
            desired_config, {}, persist_cache);
        if (!snapshot_res) return std::unexpected(snapshot_res.error());
        plan.repos = std::move(*snapshot_res);

        // 3. Solve dependencies for new providers
        std::vector<package::Dependency> root_reqs;
        for (const auto& swap : plan.swaps) {
            root_reqs.push_back(package::Dependency{
                .name = swap.target_provider,
                .op = package::ConstraintOp::Any,
                .version = {}
            });
            if (!swap.current_provider.empty() && swap.current_provider != swap.target_provider) {
                auto current_package = db.get_package(swap.current_provider);
                if (!current_package) {
                    return std::unexpected(std::format(
                        "Failed to read current provider package '{}': {}",
                        swap.current_provider, current_package.error()));
                }
                plan.packages_to_remove.push_back(PlannedPackageRemoval{
                    .name = swap.current_provider,
                    .expected_identity = *current_package
                        ? std::optional{package::package_identity(**current_package)}
                        : std::nullopt,
                });
            }
        }

        solver::DependencySolver solver(plan.repos.pool, desired_config.providers);
        auto solve_res = solver.solve(root_reqs);
        if (!solve_res) {
            return std::unexpected("Reconcile dependency resolution failed: " + solve_res.error());
        }

        plan.packages_to_install = *solve_res;
        return plan;
    }

    static std::expected<void, std::string> execute(
        db::Database& db,
        const ReconcilePlan& plan,
        const std::filesystem::path& sysroot = "/",
        bool dry_run = false,
        const std::map<std::string, std::string>& providers = {})
    {
        if (!plan.has_changes) {
            util::log_info(
                "System providers already match; regenerating service definitions.");
        }

        util::log_info("Executing Declarative System Reconcile (Target Init: {})...", service::to_string(plan.target_init));

        for (const auto& swap : plan.swaps) {
            util::log_info("  • Swapping interface '{}': [{}] -> [{}]", 
                swap.iface, 
                swap.current_provider.empty() ? "none" : swap.current_provider, 
                swap.target_provider);
        }

        if (dry_run) {
            util::log_info("Dry-run preview completed successfully (no changes applied).");
            return {};
        }

        auto wtxn = db.begin_write_txn();
        if (!wtxn) return std::unexpected(std::string("Failed to open database write transaction"));

        // The plan was computed before taking the writer lock. Validate every
        // provider binding before changing any of them so a stale reconcile
        // cannot overwrite a concurrently committed provider choice.
        for (const auto& swap : plan.swaps) {
            auto current = db.get_system_provider(*wtxn, swap.iface);
            if (!current) {
                return std::unexpected(std::format(
                    "Failed to revalidate provider '{}': {}", swap.iface, current.error()));
            }
            const std::string current_name = *current ? **current : std::string{};
            if (current_name != swap.current_provider) {
                return std::unexpected(std::format(
                    "System provider '{}' changed after the reconcile plan was created",
                    swap.iface));
            }
        }

        // 1. Update system provider locks in LMDB
        for (const auto& swap : plan.swaps) {
            auto set_res = db.set_system_provider(*wtxn, swap.iface, swap.target_provider);
            if (!set_res) return std::unexpected(set_res.error());
        }

        // 2. Remove obsolete providers -- physically first: an exclusive
        // capability allows exactly one provider on disk, so the outgoing
        // package's files must go before the incoming ones land.
        for (const auto& removal : plan.packages_to_remove) {
            auto old_pkg = db.get_package(*wtxn, removal.name);
            if (!old_pkg) {
                return std::unexpected(std::format(
                    "Failed to read package '{}' in reconcile transaction: {}",
                    removal.name, old_pkg.error()));
            }
            auto current_identity = *old_pkg
                ? std::optional{package::package_identity(**old_pkg)}
                : std::nullopt;
            if (current_identity != removal.expected_identity) {
                return std::unexpected(std::format(
                    "Installed package '{}' changed after the reconcile plan was created",
                    removal.name));
            }
            if (*old_pkg) {
                const auto& old_manifest = **old_pkg;
                auto my_owner = std::format("{}:{}", removal.name, old_manifest.channel);

                // Manifest files plus anything still registered to this
                // package; children sort before parents so directories empty
                // out and can be pruned.
                std::set<std::string> paths;
                for (const auto& f : old_manifest.files) paths.insert(util::clean_rel_path(f.path));
                auto registered = db.get_package_files(*wtxn, removal.name);
                if (!registered) return std::unexpected(registered.error());
                paths.insert(registered->begin(), registered->end());
                std::vector<std::string> ordered(paths.begin(), paths.end());
                std::ranges::stable_sort(ordered, [&](const std::string& a, const std::string& b) {
                    return util::path_depth(a) > util::path_depth(b);
                });

                std::unordered_map<std::string, package::FileType> declared_types;
                for (const auto& f : old_manifest.files) {
                    declared_types.emplace(util::clean_rel_path(f.path), f.type);
                }
                for (const auto& path : ordered) {
                    auto owners = db.get_path_owners(*wtxn, path);
                    if (!owners) return std::unexpected(owners.error());
                    const bool mine =
                        std::ranges::find(*owners, my_owner) != owners->end();
                    if (!mine && !owners->empty()) continue;
                    if (mine && owners->size() > 1) continue;
                    const bool declared_directory =
                        declared_types.contains(path)
                        && declared_types.at(path) == package::FileType::Directory;
                    auto rm_res = archive::remove_path_anchored(
                        sysroot, path, !mine || declared_directory);
                    if (!rm_res) {
                        return std::unexpected(std::format(
                            "Failed to remove '{}' of outgoing provider '{}': {}",
                            path, removal.name, rm_res.error()));
                    }
                }

                auto file_res = db.unregister_files(*wtxn, old_manifest.files, my_owner);
                if (!file_res) return std::unexpected(file_res.error());
                auto provide_res = db.unregister_provides(*wtxn, old_manifest.provides);
                if (!provide_res) return std::unexpected(provide_res.error());
                auto delete_res = db.del_package(*wtxn, removal.name);
                if (!delete_res) return std::unexpected(delete_res.error());
                // Remove legacy service scripts
                service::remove_service(removal.name, service::InitType::OpenRC, sysroot);
                service::remove_service(removal.name, service::InitType::Systemd, sysroot);
                service::remove_service(removal.name, service::InitType::Runit, sysroot);
                service::remove_service(removal.name, service::InitType::Dinit, sysroot);
                service::remove_service(removal.name, service::InitType::S6, sysroot);
                service::remove_service(removal.name, service::InitType::Loom, sysroot);
            }
        }

        // 3. Install newly selected packages: unpack the archive into the
        // target root, then register the real result in LMDB. Channel index
        // manifests carry names and versions but not files or capability
        // hooks, so the archive manifest's hooks/triggers are adopted here --
        // otherwise e.g. an initramfs generator installed by a swap would be
        // invisible to the very trigger that has to run it.
        std::vector<package::PackageManifest> installed_now;
        for (const auto& selected : plan.packages_to_install) {
            const auto identity = package::package_identity(selected);

            // Exact version already installed and registered: keep its record,
            // file list and hooks; a swap only needs the new provider itself.
            auto existing = db.get_package(*wtxn, selected.name);
            if (!existing) return std::unexpected(existing.error());
            if (*existing && package::package_identity(**existing) == identity) continue;

            auto archive_res = ensure_local_archive(plan.repos, identity);
            if (!archive_res) {
                return std::unexpected(archive_res.error());
            }
            auto inspect_res = archive::inspect_package(*archive_res);
            if (!inspect_res) {
                return std::unexpected(std::format(
                    "Invalid package archive for '{}': {}", selected.name, inspect_res.error()));
            }
            if (package::package_identity(inspect_res->manifest) != identity) {
                return std::unexpected(std::format(
                    "Archive identity does not match selected package '{}'", selected.name));
            }

            util::log_info("Unpacking {} -> {}...", selected.name, sysroot.string());
            const package::PackageManifest* previous =
                *existing ? &**existing : nullptr;
            auto ext_res = archive::extract_package(
                *archive_res, sysroot, &selected, &*inspect_res, previous);
            if (!ext_res) {
                return std::unexpected(std::format(
                    "Failed to extract package '{}': {}", selected.name, ext_res.error()));
            }

            auto installed_pkg = selected;
            installed_pkg.files = ext_res->extracted_files;
            installed_pkg.capability_hooks = ext_res->manifest.capability_hooks;
            installed_pkg.triggers = ext_res->manifest.triggers;
            installed_pkg.conffiles = ext_res->manifest.conffiles;
            installed_pkg.service_toml = ext_res->manifest.service_toml;

            auto p_res = db.put_package(*wtxn, installed_pkg);
            if (!p_res) return std::unexpected(p_res.error());
            auto f_res = db.register_files(*wtxn, installed_pkg.name, installed_pkg.channel, installed_pkg.files);
            if (!f_res) return std::unexpected(f_res.error());
            auto prov_res = db.register_provides(*wtxn, installed_pkg.name, installed_pkg.provides);
            if (!prov_res) return std::unexpected(prov_res.error());
            installed_now.push_back(std::move(installed_pkg));
        }

        auto commit_res = wtxn->commit();
        if (!commit_res) return std::unexpected("Database commit failed: " + commit_res.error());

        // 4. Automatically re-generate native service configurations for all
        // installed daemons. A daemon is a package whose manifest carries a
        // universal service.toml; everything else owns no init scripts.
        auto installed = db.list_installed_packages();
        if (!installed) {
            return std::unexpected("Installed package database is inconsistent after reconcile: " + installed.error());
        }
        size_t gen_count = 0;
        for (const auto& pkg : *installed) {
            if (pkg.service_toml.empty()) continue;
            auto spec = service::ServiceSpec::parse_toml(pkg.service_toml);
            if (!spec) {
                if (plan.target_init == service::InitType::Loom) {
                    return std::unexpected(std::format(
                        "Invalid service for '{}': {}", pkg.name, spec.error()));
                }
                util::log_warn("Skipping service for '{}': {}", pkg.name, spec.error());
                continue;
            }
            // A package may ship its own native script for this init (the
            // systemd split packages keep their upstream units): package data
            // wins over the generated form.
            auto dest = service::service_destination(spec->name, plan.target_init, sysroot);
            if (!dest) {
                if (plan.target_init == service::InitType::Loom) {
                    return std::unexpected(std::format(
                        "Invalid Loom destination for '{}': {}", pkg.name, dest.error()));
                }
                util::log_warn("Skipping service for '{}': {}", pkg.name, dest.error());
                continue;
            }
            auto owners = db.get_path_owners(dest->string());
            if (owners && !owners->empty()) {
                util::log_info("  · {:<20} ships its own {} script", spec->name,
                    service::to_string(plan.target_init));
                continue;
            }
            auto gen_res = plan.target_init == service::InitType::Loom
                ? service::generate_loom_service(
                    pkg.service_toml, spec->name, sysroot)
                : service::generate_service(*spec, plan.target_init, sysroot);
            if (gen_res) {
                gen_count++;
            } else if (plan.target_init == service::InitType::Loom) {
                return std::unexpected(std::format(
                    "Cannot generate Loom service for '{}': {}", pkg.name, gen_res.error()));
            } else {
                util::log_warn("Cannot generate {} script for '{}': {}",
                    service::to_string(plan.target_init), pkg.name, gen_res.error());
            }
        }
        if (plan.target_init == service::InitType::Loom) {
            auto valid = service::validate_loom_services(sysroot);
            if (!valid) return std::unexpected(valid.error());
        }

        // 5. Execute post-transaction triggers
        TriggerContext trig_ctx;
        trig_ctx.sysroot = sysroot;
        trig_ctx.transaction_packages = std::move(installed_now);
        trig_ctx.installed_packages = std::move(*installed);
        trig_ctx.providers = providers;
        for (const auto& pkg : trig_ctx.transaction_packages) {
            trig_ctx.touched_files.insert(trig_ctx.touched_files.end(), pkg.files.begin(), pkg.files.end());
        }
        auto trigger_result = TriggerEngine::run(trig_ctx);
        if (!trigger_result) {
            return std::unexpected(
                "Post-transaction trigger failed: " + trigger_result.error());
        }

        util::log_success("Reconcile completed! Regenerated {} native service scripts for {}", 
            gen_count, service::to_string(plan.target_init));
        return {};
    }
};

} // namespace sage::rebuild
