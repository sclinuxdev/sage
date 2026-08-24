export module sage.cli.rebuild;

// `sage rebuild`: declarative reconcile of /etc/sage/system.toml vs LMDB.
import std;
import sage;

import sage.cli;

namespace sage::cli {

export int cmd_rebuild(
    const CliOptions& opts,
    DatabaseSnapshot database_snapshot = DatabaseSnapshot::Unchecked)
{
    if (opts.dry_run && database_snapshot == DatabaseSnapshot::Unchecked) {
        sage::util::log_error("Dry-run rebuild requires a synchronized database snapshot");
        return 1;
    }

    auto cfg_res = sage::config::SystemConfig::load_from_root(opts.target_root);
    if (!cfg_res) {
        sage::util::log_error("Failed to load configuration: {}", cfg_res.error());
        return 1;
    }

    if (opts.dry_run && database_snapshot == DatabaseSnapshot::Absent) {
        sage::util::log_error(
            "Cannot calculate rebuild preview: package database '{}' is not initialized",
            cfg_res->db_path.string());
        return 1;
    }

    auto db_res = opts.dry_run
        ? sage::db::Database::open_existing_read_only_no_lock(cfg_res->db_path)
        : sage::db::Database::open(cfg_res->db_path);
    if (!db_res) {
        sage::util::log_error("Failed to open database: {}", db_res.error());
        return 1;
    }

    auto plan_res = sage::rebuild::ReconcileEngine::calculate_diff(
        *db_res, *cfg_res, !opts.dry_run);
    if (!plan_res) {
        sage::util::log_error("Failed to calculate reconcile plan: {}", plan_res.error());
        return 1;
    }

    auto exec_res = sage::rebuild::ReconcileEngine::execute(*db_res, *plan_res, opts.target_root, opts.dry_run, cfg_res->providers);
    if (!exec_res) {
        sage::util::log_error("Reconcile execution failed: {}", exec_res.error());
        return 1;
    }
    return 0;
}

} // namespace sage::cli
