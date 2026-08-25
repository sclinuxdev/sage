import std;
import sage;
import sage.cli;
import sage.cli.build;
import sage.cli.install;
import sage.cli.query;
import sage.cli.rebuild;
import sage.cli.remove;
import sage.cli.toolchain;

// Entry point: global option parsing lives in sage.cli; this file only maps
// the command word to its implementation module. The mapping is a data table,
// not control flow: adding a command means adding one row.
namespace {

struct CommandSpec {
    std::string_view name;
    bool state_lock; // shares the host package-state operation lock
    int (*run)(const sage::cli::CliOptions&, const sage::cli::OperationContext*);
};

inline constexpr auto commands = std::array{
    CommandSpec{"install",   true,  [](const sage::cli::CliOptions& o, const sage::cli::OperationContext* op) { return cmd_install(o, std::nullopt, op->database_snapshot); }},
    CommandSpec{"remove",    true,  [](const sage::cli::CliOptions& o, const sage::cli::OperationContext* op) { return cmd_remove(o, op->database_snapshot); }},
    CommandSpec{"rebuild",   true,  [](const sage::cli::CliOptions& o, const sage::cli::OperationContext* op) { return cmd_rebuild(o, op->database_snapshot); }},
    CommandSpec{"build",     false, [](const sage::cli::CliOptions& o, const sage::cli::OperationContext*) { return cmd_build(o); }},
    CommandSpec{"status",    false, [](const sage::cli::CliOptions& o, const sage::cli::OperationContext*) { return cmd_status(o); }},
    CommandSpec{"query",     false, [](const sage::cli::CliOptions& o, const sage::cli::OperationContext*) { return cmd_query(o); }},
    CommandSpec{"list",      false, [](const sage::cli::CliOptions& o, const sage::cli::OperationContext*) { return cmd_list(o); }},
    CommandSpec{"count",     false, [](const sage::cli::CliOptions& o, const sage::cli::OperationContext*) { return cmd_count(o); }},
    CommandSpec{"toolchain", false, [](const sage::cli::CliOptions& o, const sage::cli::OperationContext*) { return cmd_toolchain(o); }},
    CommandSpec{"shell",     false, [](const sage::cli::CliOptions& o, const sage::cli::OperationContext*) { return cmd_shell(o); }},
    CommandSpec{"service",   false, [](const sage::cli::CliOptions& o, const sage::cli::OperationContext*) { return cmd_service(o); }},
    CommandSpec{"channel",   false, [](const sage::cli::CliOptions& o, const sage::cli::OperationContext*) { return cmd_channel(o); }},
    CommandSpec{"repo",      false, [](const sage::cli::CliOptions& o, const sage::cli::OperationContext*) { return cmd_repo(o); }},
    CommandSpec{"verify",    false, [](const sage::cli::CliOptions& o, const sage::cli::OperationContext*) { return cmd_verify(o); }},
    CommandSpec{"recover",   true,  [](const sage::cli::CliOptions& o, const sage::cli::OperationContext*) { return cmd_recover(o); }},
};

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        sage::cli::print_help();
        return 0;
    }

    auto parsed = sage::cli::parse_args(argc, argv);
    if (!parsed) return 0;
    const auto& opts = *parsed;

    using namespace sage::cli;
    const auto spec = std::ranges::find(commands, opts.command, &CommandSpec::name);
    if (spec == commands.end()) {
        std::println(std::cerr, "Unknown command: '{}'", opts.command);
        print_help();
        return 1;
    }

    // Package-state commands share one host operation lock. Its lifetime spans
    // command return, so command-local LMDB environments close before unlock.
    std::optional<OperationContext> operation;
    if (spec->state_lock) {
        auto operation_res = acquire_operation_context(opts);
        if (!operation_res) return operation_res.error();
        operation = std::move(*operation_res);
    }
    return spec->run(opts, operation ? &*operation : nullptr);
}
