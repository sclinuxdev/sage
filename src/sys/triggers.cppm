export module sage.triggers;

// Post-transaction trigger engine. A transaction reports what it touched;
// triggers decide what has to be regenerated.

import std;
import sage.package;
import sage.util;

export namespace sage::triggers {

// Two things can fire a trigger:
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

        // The only required built-in: a stale dynamic linker cache leaves the
        // root broken. Every other built-in tool is one the admin may
        // legitimately not have installed (shared-mime-info, ca-certs, ...),
        // so those stay optional and only warn when their exec is missing.
        t.push_back(package::Trigger{
            .name = "ldconfig",
            .on_paths = {"usr/lib/", "lib/"},
            .exec = "/usr/bin/ldconfig",
            .priority = 10,
            .required = true,
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
        // Standard desktop-stack caches. Every one of them is optional tooling
        // the admin may not have installed, so they warn and skip when their
        // executable is missing instead of failing the transaction.
        t.push_back(package::Trigger{
            .name = "glib-schemas",
            .on_paths = {"usr/share/glib-2.0/schemas/"},
            .exec = "/usr/bin/glib-compile-schemas",
            .args = {"/usr/share/glib-2.0/schemas"},
            .priority = 30,
        });
        t.push_back(package::Trigger{
            .name = "desktop-database",
            .on_paths = {"usr/share/applications/"},
            .exec = "/usr/bin/update-desktop-database",
            .args = {"/usr/share/applications"},
            .priority = 30,
        });
        t.push_back(package::Trigger{
            .name = "icon-cache",
            .on_paths = {"usr/share/icons/"},
            .exec = "/usr/bin/gtk-update-icon-cache",
            .args = {"-q", "-t", "-f", "/usr/share/icons/hicolor"},
            .priority = 30,
        });
        t.push_back(package::Trigger{
            .name = "font-cache",
            .on_paths = {"usr/share/fonts/"},
            .exec = "/usr/bin/fc-cache",
            .args = {"-f"},
            .priority = 30,
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

        // Declarative system users, before anything that may want to run as
        // them. The recipe declares [[sysusers]]; sage applies them through
        // the standard shadow utilities inside the target root.
        if (auto result = apply_sysusers(ctx); !result) return result;

        // Cross-package symlink arbitration ([[alternatives]]): point each
        // link at the highest-priority installed provider.
        if (auto result = apply_alternatives(ctx); !result) return result;

        // depmod must run once per kernel version, and the version is only
        // known from the touched module paths, so it cannot be a fixed
        // command line. It resolves through the installed provider of
        // virtual/depmod (kmod) and runs before the initramfs trigger so the
        // module dependency index exists when the initramfs is assembled.
        std::set<std::string> module_versions;
        for (const auto& f : ctx.touched_files) {
            constexpr std::string_view prefix = "usr/lib/modules/";
            if (!f.path.starts_with(prefix)) continue;
            const auto rest = std::string_view(f.path).substr(prefix.size());
            const auto slash = rest.find('/');
            if (slash == std::string_view::npos || slash == 0) continue;
            module_versions.insert(std::string(rest.substr(0, slash)));
        }
        if (!module_versions.empty()) {
            const package::CapabilityHook* hook = nullptr;
            for (const auto& pkg : ctx.installed_packages) {
                if (auto* h = pkg.hook_for("virtual/depmod")) { hook = h; break; }
            }
            if (hook) {
                for (const auto& version : module_versions) {
                    const auto cmd = hook->exec + " -a " + version;
                    if (!already_run.insert(cmd).second) continue;
                    auto result = execute(cmd, "depmod", false, ctx);
                    if (!result) return result;
                }
            }
        }

        for (const auto& trig : candidates) {
            if (!fires(trig, ctx, txn_capabilities)) continue;

            auto cmd = resolve_command(trig, ctx);
            if (!cmd) continue;

            if (!already_run.insert(*cmd).second) continue;
            auto result = execute(*cmd, trig.name, trig.required, ctx);
            if (!result) return result;
        }
        return {};
    }

private:
    static std::expected<void, std::string> apply_sysusers(const TriggerContext& ctx) {
        const bool any_declared = std::ranges::any_of(
            ctx.transaction_packages, [](const package::PackageManifest& pkg) {
                return !pkg.sysusers.empty();
            });
        if (!any_declared) return {};

        // Read the target root's account databases directly: the builder host
        // may share nothing with the system being assembled.
        std::set<std::string> existing_users;
        std::set<std::string> existing_groups;
        {
            std::ifstream passwd(ctx.sysroot / "etc/passwd");
            std::string line;
            while (std::getline(passwd, line)) {
                const auto colon = line.find(':');
                if (colon != std::string::npos)
                    existing_users.insert(line.substr(0, colon));
            }
            std::ifstream group(ctx.sysroot / "etc/group");
            while (std::getline(group, line)) {
                const auto colon = line.find(':');
                if (colon != std::string::npos)
                    existing_groups.insert(line.substr(0, colon));
            }
        }

        const auto ensure_group = [&](const package::SysUserEntry& entry)
            -> std::expected<bool, std::string> {
            if (existing_groups.contains(entry.name)) return false;
            if (ctx.dry_run) {
                util::log_info("Would create system group '{}'", entry.name);
                return true;
            }
            if (ctx.sysroot != "/") {
                // In non-/ sysroot environments without full chroot/shadow tools,
                // safely append the group directly to target root's etc/group.
                std::error_code ec;
                std::filesystem::create_directories(ctx.sysroot / "etc", ec);
                std::ofstream group_out(ctx.sysroot / "etc/group", std::ios::app);
                if (group_out) {
                    group_out << entry.name << ":x:" << (entry.id ? *entry.id : 999) << ":\n";
                    existing_groups.insert(entry.name);
                    util::log_info("Created system group '{}' in sysroot", entry.name);
                    return true;
                }
            }
            std::string cmd = "groupadd -r";
            if (entry.id) cmd += " -g " + std::to_string(*entry.id);
            cmd += " " + entry.name;
            auto result = execute(cmd, "sysusers-group(" + entry.name + ")", false, ctx);
            if (!result) {
                // If shadow utility fails (e.g. permission/unprivileged root), fallback to direct file injection
                std::ofstream group_out(ctx.sysroot / "etc/group", std::ios::app);
                if (group_out) {
                    group_out << entry.name << ":x:" << (entry.id ? *entry.id : 999) << ":\n";
                    existing_groups.insert(entry.name);
                    util::log_warn("sysusers: groupadd failed, direct injection applied for group '{}'", entry.name);
                    return true;
                }
                return std::unexpected(result.error());
            }
            existing_groups.insert(entry.name);
            return true;
        };

        // Groups first so user entries can reference them as primary groups.
        for (const auto& pkg : ctx.transaction_packages) {
            for (const auto& entry : pkg.sysusers) {
                if (entry.type != "group") continue;
                if (auto created = ensure_group(entry); !created)
                    return std::unexpected(created.error());
            }
        }
        for (const auto& pkg : ctx.transaction_packages) {
            for (const auto& entry : pkg.sysusers) {
                if (entry.type != "user") continue;
                if (existing_users.contains(entry.name)) continue;
                if (!entry.group.empty() && entry.group != entry.name) {
                    if (!existing_groups.contains(entry.group)) {
                        package::SysUserEntry primary;
                        primary.type = "group";
                        primary.name = entry.group;
                        primary.id = entry.id;
                        if (auto created = ensure_group(primary); !created)
                            return std::unexpected(created.error());
                    }
                }
                if (ctx.dry_run) {
                    util::log_info("Would create system user '{}'", entry.name);
                    continue;
                }
                if (ctx.sysroot != "/") {
                    std::error_code ec;
                    std::filesystem::create_directories(ctx.sysroot / "etc", ec);
                    std::ofstream passwd_out(ctx.sysroot / "etc/passwd", std::ios::app);
                    if (passwd_out) {
                        passwd_out << entry.name << ":x:" << (entry.id ? *entry.id : 999) << ":"
                                   << (entry.id ? *entry.id : 999) << ":"
                                   << entry.description << ":"
                                   << (entry.home.empty() ? "/" : entry.home) << ":"
                                   << (entry.shell.empty() ? "/usr/bin/nologin" : entry.shell) << "\n";
                        existing_users.insert(entry.name);
                        util::log_info("Created system user '{}' in sysroot", entry.name);
                        continue;
                    }
                }
                std::string cmd = "useradd -r -M";
                if (entry.id) cmd += " -u " + std::to_string(*entry.id);
                if (!entry.group.empty()) cmd += " -g " + entry.group;
                if (!entry.description.empty())
                    cmd += " -c \"" + entry.description + "\"";
                cmd += " -d " + (entry.home.empty() ? "/" : entry.home);
                cmd += " -s " + (entry.shell.empty()
                    ? std::string("/usr/bin/nologin") : entry.shell);
                cmd += " " + entry.name;
                auto result = execute(cmd, "sysusers-user(" + entry.name + ")", false, ctx);
                if (!result) {
                    std::ofstream passwd_out(ctx.sysroot / "etc/passwd", std::ios::app);
                    if (passwd_out) {
                        passwd_out << entry.name << ":x:" << (entry.id ? *entry.id : 999) << ":"
                                   << (entry.id ? *entry.id : 999) << ":"
                                   << entry.description << ":"
                                   << (entry.home.empty() ? "/" : entry.home) << ":"
                                   << (entry.shell.empty() ? "/usr/bin/nologin" : entry.shell) << "\n";
                        existing_users.insert(entry.name);
                        util::log_warn("sysusers: useradd failed, direct injection applied for user '{}'", entry.name);
                        continue;
                    }
                    return std::unexpected(result.error());
                }
                existing_users.insert(entry.name);
            }
        }
        return {};
    }

    static std::expected<void, std::string> apply_alternatives(const TriggerContext& ctx) {
        struct Offer {
            std::string package;
            const package::AlternativeEntry* entry;
        };
        std::map<std::string, std::vector<Offer>> offers;
        for (const auto& pkg : ctx.installed_packages) {
            for (const auto& alt : pkg.alternatives) {
                offers[alt.link].push_back({pkg.name, &alt});
            }
        }
        for (const auto& [link, candidates] : offers) {
            const package::AlternativeEntry* winner = nullptr;
            std::string winner_package;
            for (const auto& offer : candidates) {
                if (!winner
                    || offer.entry->priority > winner->priority
                    || (offer.entry->priority == winner->priority
                        && offer.package < winner_package)) {
                    winner = offer.entry;
                    winner_package = offer.package;
                }
            }
            if (!winner) continue;
            const auto live = ctx.sysroot / std::filesystem::path(link);
            std::error_code ec;
            const auto status = std::filesystem::symlink_status(live, ec);
            if (!ec && std::filesystem::exists(status)
                && !std::filesystem::is_symlink(status)) {
                util::log_warn(
                    "Alternative '{}': '{}' occupies the path with a real file; "
                    "leaving it alone", link, winner_package);
                continue;
            }
            std::string current;
            if (status.type() == std::filesystem::file_type::symlink) {
                auto target = std::filesystem::read_symlink(live, ec);
                if (!ec) {
                    current = target.generic_string();
                    if (current == winner->target) continue;
                }
            }
            if (ctx.dry_run) {
                util::log_info("Would point alternative '{}' at '{}' (package '{}')",
                    link, winner->target, winner_package);
                continue;
            }
            if (std::filesystem::is_symlink(live, ec)) {
                std::filesystem::remove(live, ec);
                if (ec) return std::unexpected(std::format(
                    "Cannot replace alternative symlink '{}': {}", link, ec.message()));
            }
            std::filesystem::create_symlink(winner->target, live, ec);
            if (ec) return std::unexpected(std::format(
                "Cannot create alternative symlink '{}': {}", link, ec.message()));
            util::log_info("Alternative '{}': '{}' (package '{}', priority {})",
                link, winner->target, winner_package, winner->priority);
        }
        // A link whose every provider left: drop the dangling symlink.
        for (const auto& pkg : ctx.transaction_packages) {
            for (const auto& alt : pkg.alternatives) {
                if (offers.contains(alt.link)) continue;
                const auto live = ctx.sysroot / std::filesystem::path(alt.link);
                std::error_code ec;
                const auto status = std::filesystem::symlink_status(live, ec);
                if (!ec && std::filesystem::is_symlink(status)) {
                    if (ctx.dry_run) {
                        util::log_info("Would remove dangling alternative '{}'", alt.link);
                        continue;
                    }
                    std::filesystem::remove(live, ec);
                    if (!ec)
                        util::log_info("Removed dangling alternative '{}'", alt.link);
                }
            }
        }
        return {};
    }

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
        bool required,
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
            if (!required) {
                util::log_warn(
                    "Optional trigger '{}': executable '{}' is missing -- skipping",
                    trigger_name, exec_path);
                return {};
            }
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

} // namespace sage::triggers
