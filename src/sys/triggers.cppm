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
            .required = true,
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
            auto result = execute(*cmd, trig.name, trig.required, ctx);
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
