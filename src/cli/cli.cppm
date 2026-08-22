export module sage.cli;

// Shared CLI vocabulary: option state, banner/help output, argument parsing.
// Command groups live in sibling modules (sage.cli.pkg, sage.cli.query, ...)
// and the entry point in src/main.cpp dispatches between them.
import std;
import sage;

export namespace sage::cli {

using std::size_t;
using std::uint32_t;

struct CliOptions {
    std::string command;
    std::vector<std::string> args;
    std::filesystem::path target_root{"/"};
    bool dry_run{false};
    bool verbose{false};
    bool force{false};        // --force, -f, --nodeps, -d
    bool cascade{false};      // --cascade, -c
    bool no_recursive{false};  // --no-recursive
    bool no_elf_check{false};  // --no-elf-check
    bool reuse_src{false};     // --reuse-src: resume `build` on an extracted tree
    std::string channel_filter;  // --channel <NAME>
    int wait_seconds{0};      // --wait[=SECONDS]: wait for a concurrent sage
};

void print_banner() {
    std::println("{}🌿 Sage Package Manager v0.1.3 (Modern C++23){}", sage::util::color::green, sage::util::color::reset);
}

void print_help() {
    print_banner();
    std::println(R"(
Usage: sage [GLOBAL OPTIONS] <COMMAND> [ARGS...]

Commands:
  install <PKG...>         Install packages into target root via PubGrub SAT solver
  remove <PKG...>          Remove installed package files and unregister state
  rebuild                  Declarative reconcile (/etc/sage/system.toml vs LMDB)
  toolchain [list|use]     Manage multi-slot toolchains (e.g. use java:21, rust:nightly)
  shell [--with <spec...>] Launch ephemeral sandboxed shell with custom toolchains
  channel [COMMAND]        Manage multi-layer channels (list, add, remove, sync)
  repo index <DIR> [NAME]  Generate index.toml for local repository directory
  query [COMMAND]          Query packages, files, capabilities and ownership (nanosecond LMDB)
  list [-q] [PATTERN]      List installed packages (-q: bare names for scripting)
  count [PATTERN]          Print the number of installed packages
  service [COMMAND]        Inspect and generate native init scripts (OpenRC/Runit/Systemd/Dinit/s6)
  build <RECIPE_DIR>       Build package from recipe.toml (fetch source, check sha256, build, scan ELF)
  verify [PKG...]          Check installed files against the recorded files.idx hashes
  status [--full]          Show declared providers, channels, and database state

Global Options:
  --root, --sysroot <DIR>  Operate on target root directory (default: /)
  --dry-run                Simulate actions without modifying filesystem
  --verbose, -v            Enable verbose diagnostics
  --no-elf-check           Skip build-time DT_NEEDED validation (bootstrap escape hatch)
  --reuse-src              build: resume on the already-extracted src/ tree
                           instead of re-unpacking (kbuild-style increments)
  --channel <NAME>         Restrict `install` to a single channel
  --wait[=SECONDS]         Wait for a concurrent sage on the same root (default: fail fast)
  --help, -h               Show this help message
  --version, -V            Show version information
)");
}

// Parse global options. Returns nullopt when help/version was printed and the
// caller should exit successfully.
inline std::optional<CliOptions> parse_args(int argc, char* argv[]) {
    CliOptions opts;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_help();
            return std::nullopt;
        }
        if (arg == "--version" || arg == "-V") {
            print_banner();
            return std::nullopt;
        }
        if (arg == "--dry-run") {
            opts.dry_run = true;
        } else if (arg == "--verbose" || arg == "-v") {
            opts.verbose = true;
        } else if (arg == "--force" || arg == "-f" || arg == "--nodeps" || arg == "-d") {
            opts.force = true;
        } else if (arg == "--cascade" || arg == "-c") {
            opts.cascade = true;
        } else if (arg == "--no-recursive") {
            opts.no_recursive = true;
        } else if (arg == "--no-elf-check") {
            opts.no_elf_check = true;
        } else if (arg == "--reuse-src") {
            opts.reuse_src = true;
        } else if (arg == "--channel" && i + 1 < argc) {
            opts.channel_filter = argv[++i];
        } else if (arg == "--wait") {
            opts.wait_seconds = 30;
        } else if (arg.starts_with("--wait=")) {
            opts.wait_seconds = std::atoi(std::string(arg.substr(7)).c_str());
        } else if ((arg == "--root" || arg == "--sysroot") && i + 1 < argc) {
            opts.target_root = argv[++i];
        } else if (opts.command.empty()) {
            opts.command = std::string(arg);
        } else {
            opts.args.push_back(std::string(arg));
        }
    }
    return opts;
}

enum class TargetRootSnapshot { Absent, Present };
enum class DatabaseSnapshot { Unchecked, Absent, Present };

struct OperationContext {
    util::OperationLock lock;
    TargetRootSnapshot target_root_snapshot{TargetRootSnapshot::Absent};
    DatabaseSnapshot database_snapshot{DatabaseSnapshot::Absent};
};

inline std::expected<bool, std::string> classify_path_probe(
    const std::filesystem::path& path,
    std::filesystem::file_status status,
    const std::error_code& ec,
    std::filesystem::file_type expected_type,
    std::string_view description)
{
    if (ec == std::errc::no_such_file_or_directory) return false;
    if (ec) {
        return std::unexpected(std::format(
            "cannot inspect {} '{}': {}", description, path.string(), ec.message()));
    }
    if (status.type() == std::filesystem::file_type::not_found) return false;
    if (status.type() != expected_type) {
        return std::unexpected(std::format(
            "{} '{}' has the wrong file type", description, path.string()));
    }
    return true;
}

// symlink_status observes the directory entry itself. A symlink is therefore a
// wrong type instead of silently redirecting the synchronized probe elsewhere.
inline std::expected<bool, std::string> probe_path_type(
    const std::filesystem::path& path,
    std::filesystem::file_type expected_type,
    std::string_view description)
{
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    return classify_path_probe(path, status, ec, expected_type, description);
}

inline std::expected<void, std::string> validate_operation_user(
    std::uint32_t effective_uid)
{
    if (effective_uid == 0) return {};
    return std::unexpected(
        "install, remove, and rebuild operations require root privileges");
}

inline std::expected<bool, std::string> probe_package_database(
    const std::filesystem::path& configured_path)
{
    return probe_path_type(
        sage::db::resolve_lmdb_paths(configured_path).data_file,
        std::filesystem::file_type::regular,
        "package database");
}

// install/remove/rebuild call this in main before touching package state. The
// returned lock spans the complete command and the snapshots are the only DB
// existence decision those command implementations consume.
inline std::expected<OperationContext, int> acquire_operation_context(
    const CliOptions& opts,
    const std::filesystem::path& operation_lock_path = "/run/sage/operation.lock")
{
    auto user = validate_operation_user(sage::util::current_effective_uid());
    if (!user) {
        sage::util::log_error("{}", user.error());
        return std::unexpected(1);
    }

    const auto mode = opts.dry_run
        ? sage::util::LockMode::Shared
        : sage::util::LockMode::Exclusive;
    auto lock = sage::util::OperationLock::acquire(
        operation_lock_path, mode, opts.wait_seconds);
    if (!lock) {
        if (lock.error().kind == sage::util::LockFailure::Busy) {
            sage::util::log_error(
                "another sage package operation is running; retry once it finishes");
        } else {
            sage::util::log_error("cannot acquire global operation lock: {}",
                lock.error().message);
        }
        return std::unexpected(1);
    }

    const auto target_root = opts.target_root.empty()
        ? std::filesystem::path{"/"}
        : opts.target_root;
    auto root_exists = probe_path_type(
        target_root, std::filesystem::file_type::directory, "target root");
    if (!root_exists) {
        sage::util::log_error("{}", root_exists.error());
        return std::unexpected(1);
    }

    auto cfg_res = sage::config::SystemConfig::load_from_root(target_root);
    if (!cfg_res) {
        sage::util::log_error("Failed to load configuration: {}", cfg_res.error());
        return std::unexpected(1);
    }
    auto database_exists = probe_package_database(cfg_res->db_path);
    if (!database_exists) {
        sage::util::log_error("{}", database_exists.error());
        return std::unexpected(1);
    }

    return OperationContext{
        .lock = std::move(*lock),
        .target_root_snapshot = *root_exists
            ? TargetRootSnapshot::Present
            : TargetRootSnapshot::Absent,
        .database_snapshot = *database_exists
            ? DatabaseSnapshot::Present
            : DatabaseSnapshot::Absent,
    };
}

} // namespace sage::cli
