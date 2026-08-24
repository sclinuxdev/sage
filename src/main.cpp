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
// the command word to its implementation module.
int main(int argc, char* argv[]) {
    if (argc < 2) {
        sage::cli::print_help();
        return 0;
    }

    auto parsed = sage::cli::parse_args(argc, argv);
    if (!parsed) return 0;
    const auto& opts = *parsed;

    using namespace sage::cli;

    // Package-state commands share one host operation lock. Its lifetime spans
    // command return, so command-local LMDB environments close before unlock.
    std::optional<OperationContext> operation;
    if (opts.command == "install" || opts.command == "remove" || opts.command == "rebuild") {
        auto operation_res = acquire_operation_context(opts);
        if (!operation_res) return operation_res.error();
        operation = std::move(*operation_res);
    }

    if (opts.command == "install") {
        return cmd_install(opts, std::nullopt, operation->database_snapshot);
    }
    if (opts.command == "remove") {
        return cmd_remove(opts, operation->database_snapshot);
    }
    if (opts.command == "build") {
        return cmd_build(opts);
    }
    if (opts.command == "rebuild") {
        return cmd_rebuild(opts, operation->database_snapshot);
    }
    if (opts.command == "status") {
        return cmd_status(opts);
    }
    if (opts.command == "query") {
        return cmd_query(opts);
    }
    if (opts.command == "list") {
        return cmd_list(opts);
    }
    if (opts.command == "count") {
        return cmd_count(opts);
    }
    if (opts.command == "toolchain") {
        return cmd_toolchain(opts);
    }
    if (opts.command == "shell") {
        return cmd_shell(opts);
    }
    if (opts.command == "service") {
        return cmd_service(opts);
    }
    if (opts.command == "channel") {
        return cmd_channel(opts);
    }
    if (opts.command == "repo") {
        return cmd_repo(opts);
    }
    if (opts.command == "verify") {
        return cmd_verify(opts);
    }

    std::println(std::cerr, "Unknown command: '{}'", opts.command);
    sage::cli::print_help();
    return 1;
}
