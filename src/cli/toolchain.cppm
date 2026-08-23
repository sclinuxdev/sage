export module sage.cli.toolchain;

// Channel management, multi-slot toolchains and the ephemeral shell.
import std;
import sage;

import sage.cli;

namespace sage::cli {

export int cmd_toolchain(const CliOptions& opts) {
    if (opts.args.empty() || opts.args[0] == "list") {
        auto list = sage::channel::ProfileManager::list_installed_subchannels(opts.target_root);
        std::println("Installed Toolchains & Runtimes in '{}':", opts.target_root.string());
        if (list.empty()) {
            std::println("  (No sub-channels currently installed in /opt/channels or /usr/lib/runtimes)");
            return 0;
        }
        for (const auto& sc : list) {
            std::println("  • {:<12} slot: {:<10} scope: {:<10} path: {}",
                sc.category, sc.slot, sage::channel::to_string(sc.scope), sc.path.string());
        }
        return 0;
    }

    if (opts.args[0] == "use" && opts.args.size() >= 2) {
        auto spec = sage::channel::SubChannelSpec::parse(opts.args[1]);
        auto res = sage::channel::ProfileManager::switch_active_toolchain(opts.target_root, spec.category, spec.slot);
        if (!res) {
            sage::util::log_error("{}", res.error());
            return 1;
        }
        return 0;
    }

    std::println("Usage: sage toolchain [list|use <category:slot>]");
    return 1;
}

export int cmd_shell(const CliOptions& opts) {
    std::vector<sage::channel::SubChannelSpec> specs;
    for (size_t i = 0; i < opts.args.size(); ++i) {
        if (opts.args[i] == "--with" && i + 1 < opts.args.size()) {
            specs.push_back(sage::channel::SubChannelSpec::parse(opts.args[++i]));
        }
    }

    if (specs.empty()) {
        std::println("Usage: sage shell --with <sub-channel...> (e.g. sage shell --with toolchain/llvm:22 --with runtime/python:3.12)");
        return 1;
    }

    auto env = sage::channel::ProfileManager::generate_shell_env(opts.target_root, specs);
    sage::util::log_info("Entering Ephemeral Sandboxed Shell with {} sub-channels...", specs.size());

    // Export generated environment variables
    for (const auto& [k, v] : env) {
        if (const char* old_val = sage::util::get_env(k)) {
            (void)sage::util::set_env(k, v + ":" + old_val);
        } else {
            (void)sage::util::set_env(k, v);
        }
    }
    (void)sage::util::set_env("PS1", "(sage-env) \\u@\\h:\\w\\$ ");

    const char* user_shell = sage::util::get_env("SHELL");
    if (!user_shell) user_shell = "/bin/sh";
    int ret = std::system(user_shell);
    (void)ret;
    return 0;
}

export int cmd_service(const CliOptions& opts) {
    if (opts.args.empty()) {
        std::println("Usage: sage service [list|generate <name>]");
        return 1;
    }
    std::string sub = opts.args[0];
    if (sub == "list") {
        sage::util::log_info("Available native init targets: Loom, OpenRC, Runit, Systemd, Dinit, s6");
    } else if (sub == "generate" && opts.args.size() >= 2) {
        std::string name = opts.args[1];
        sage::service::ServiceSpec spec;
        spec.name = name;
        spec.exec_start = "/usr/bin/" + name;
        auto res = sage::service::generate_service(spec, sage::service::InitType::OpenRC, opts.target_root);
        if (res) {
            sage::util::log_success("Generated OpenRC service script at {}", res->string());
        }
    }
    return 0;
}
// Persist the channel list to the *target root's* channels.toml.
//
// This must always write the target's own file. SystemConfig falls back to the
// host's /etc/sage/channels.toml when the target root has none, so a target
// that is merely borrowing the host's channels would otherwise have an edit
// silently applied to the host instead.
int write_channels(const sage::config::SystemConfig& cfg) {
    std::error_code ec;
    std::filesystem::create_directories(cfg.channels_config_path.parent_path(), ec);
    std::ofstream out(cfg.channels_config_path);
    if (!out.is_open()) {
        sage::util::log_error("Cannot write {}", cfg.channels_config_path.string());
        return 1;
    }
    out << cfg.serialize_channels_toml();
    return 0;
}

export int cmd_channel(const CliOptions& opts) {
    auto cfg_res = sage::config::SystemConfig::load_from_root(opts.target_root);
    if (!cfg_res) {
        sage::util::log_error("Failed to load configuration: {}", cfg_res.error());
        return 1;
    }
    auto& cfg = *cfg_res;

    std::string sub = opts.args.empty() ? "list" : opts.args[0];

    if (sub == "list") {
        std::println("Configured Channels for '{}':", opts.target_root.string());
        if (cfg.channels.empty()) {
            std::println("  (none configured)");
        }
        for (const auto& ch : cfg.channels) {
            std::println("  • {:<15} {:<45} scope: {:<10} priority: {:<5} {}",
                ch.name, ch.url, ch.scope, ch.priority, ch.enabled ? "enabled" : "disabled");
        }
        return 0;
    }

    if (sub == "add") {
        if (opts.args.size() < 3) {
            std::println("Usage: sage channel add <NAME> <URL> [SCOPE] [PRIORITY]");
            std::println("  SCOPE: system (default) | runtime | toolchain | user");
            return 1;
        }
        sage::config::ChannelConfig ch;
        ch.name = opts.args[1];
        ch.url = opts.args[2];
        ch.scope = opts.args.size() >= 4 ? opts.args[3] : "system";
        ch.priority = opts.args.size() >= 5 ? std::atoi(opts.args[4].c_str()) : 50;
        ch.enabled = true;

        auto existing = std::ranges::find(cfg.channels, ch.name, &sage::config::ChannelConfig::name);
        if (existing != cfg.channels.end()) {
            sage::util::log_info("Updating existing channel '{}'", ch.name);
            *existing = std::move(ch);
        } else {
            cfg.channels.push_back(std::move(ch));
        }

        if (int rc = write_channels(cfg); rc != 0) return rc;
        sage::util::log_success("Channel '{}' written to {}", opts.args[1], cfg.channels_config_path.string());
        return 0;
    }

    if (sub == "remove" || sub == "rm") {
        if (opts.args.size() < 2) {
            std::println("Usage: sage channel remove <NAME>");
            return 1;
        }
        const std::string& name = opts.args[1];
        auto removed = std::erase_if(cfg.channels, [&](const sage::config::ChannelConfig& c) { return c.name == name; });
        if (removed == 0) {
            sage::util::log_error("No channel named '{}' is configured", name);
            return 1;
        }
        if (int rc = write_channels(cfg); rc != 0) return rc;
        sage::util::log_success("Channel '{}' removed from {}", name, cfg.channels_config_path.string());
        return 0;
    }

    if (sub == "sync") {
        // Optional second argument narrows the sync to a single channel;
        // --channel does the same, so accept whichever the user reached for.
        std::string only = opts.args.size() >= 2 ? opts.args[1] : opts.channel_filter;

        size_t synced = 0;
        size_t failed = 0;
        for (const auto& ch_cfg : cfg.channels) {
            if (!only.empty() && ch_cfg.name != only) continue;
            if (!ch_cfg.enabled) {
                sage::util::log_info("Skipping disabled channel '{}'", ch_cfg.name);
                continue;
            }
            sage::channel::Channel ch;
            ch.name = ch_cfg.name;
            ch.url = ch_cfg.url;
            ch.scope = sage::channel::parse_scope(ch_cfg.scope);

            auto idx = sage::channel::ProfileManager::sync_channel(ch, cfg.cache_dir);
            if (!idx) {
                sage::util::log_error("Channel '{}': {}", ch_cfg.name, idx.error());
                failed++;
                continue;
            }
            sage::util::log_success("Channel '{}': {} packages", ch_cfg.name, idx->available_packages.size());
            synced++;
        }

        if (synced == 0 && failed == 0) {
            sage::util::log_warn("Nothing to sync{}", only.empty() ? "" : std::format(": no enabled channel named '{}'", only));
            return only.empty() ? 0 : 1;
        }
        // A partial sync is still a failure: the pool is now inconsistent with
        // what the caller asked for, and installing from it would pick stale
        // versions without saying so.
        return failed == 0 ? 0 : 1;
    }

    std::println("Usage: sage channel [list|add <NAME> <URL> [SCOPE] [PRIORITY]|remove <NAME>|sync [NAME]]");
    return 1;
}
} // namespace sage::cli
