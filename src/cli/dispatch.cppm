export module sage.cli.dispatch;

import std;
import sage;
import sage.cli;
import sage.cli.build;
import sage.cli.install;
import sage.cli.query;
import sage.cli.rebuild;
import sage.cli.remove;
import sage.cli.toolchain;

export namespace sage::cli {

struct CommandSpec {
    std::string_view name;
    bool state_lock;
    int (*run)(const CliOptions&, const OperationContext*);
};

inline constexpr auto commands = std::array{
    CommandSpec{"install",   true,  [](const CliOptions& o, const OperationContext* op) { return cmd_install(o, std::nullopt, op->database_snapshot); }},
    CommandSpec{"remove",    true,  [](const CliOptions& o, const OperationContext* op) { return cmd_remove(o, op->database_snapshot); }},
    CommandSpec{"rebuild",   true,  [](const CliOptions& o, const OperationContext* op) { return cmd_rebuild(o, op->database_snapshot); }},
    CommandSpec{"build",     false, [](const CliOptions& o, const OperationContext*) { return cmd_build(o); }},
    CommandSpec{"status",    false, [](const CliOptions& o, const OperationContext*) { return cmd_status(o); }},
    CommandSpec{"query",     false, [](const CliOptions& o, const OperationContext*) { return cmd_query(o); }},
    CommandSpec{"list",      false, [](const CliOptions& o, const OperationContext*) { return cmd_list(o); }},
    CommandSpec{"count",     false, [](const CliOptions& o, const OperationContext*) { return cmd_count(o); }},
    CommandSpec{"toolchain", false, [](const CliOptions& o, const OperationContext*) { return cmd_toolchain(o); }},
    CommandSpec{"shell",     false, [](const CliOptions& o, const OperationContext*) { return cmd_shell(o); }},
    CommandSpec{"service",   false, [](const CliOptions& o, const OperationContext*) { return cmd_service(o); }},
    CommandSpec{"channel",   false, [](const CliOptions& o, const OperationContext*) { return cmd_channel(o); }},
    CommandSpec{"repo",      false, [](const CliOptions& o, const OperationContext*) { return cmd_repo(o); }},
    CommandSpec{"verify",    false, [](const CliOptions& o, const OperationContext*) { return cmd_verify(o); }},
    CommandSpec{"recover",   true,  [](const CliOptions& o, const OperationContext*) { return cmd_recover(o); }},
};

int run_cli(int argc, char* argv[]) {
    if (argc < 2) {
        print_help();
        return 0;
    }

    auto parsed = parse_args(argc, argv);
    if (!parsed) return 0;
    const auto& opts = *parsed;

    const auto spec = std::ranges::find(commands, opts.command, &CommandSpec::name);
    if (spec == commands.end()) {
        std::println(std::cerr, "Unknown command: '{}'", opts.command);
        print_help();
        return 1;
    }

    std::optional<OperationContext> operation;
    if (spec->state_lock) {
        auto operation_res = acquire_operation_context(opts);
        if (!operation_res) return operation_res.error();
        operation = std::move(*operation_res);
    }
    return spec->run(opts, operation ? &*operation : nullptr);
}

} // namespace sage::cli

extern "C" int sage_main(int argc, char* argv[]) {
    return sage::cli::run_cli(argc, argv);
}
