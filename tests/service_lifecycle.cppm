module;
#include <sys/stat.h>
#include <unistd.h>

export module sage.tests.service_lifecycle;

import std;
import sage;
import sage.cli;
import sage.cli.build;
import sage.cli.install;
import sage.cli.remove;

export namespace sage::tests {

using namespace sage::cli;

int run_service_lifecycle_tests(
    const std::filesystem::path& temp_dir,
    const std::filesystem::path& canary_root)
{
    // Post-processing consumes context.touched, so every mutating plan entry
    // must be represented even when its caller did not duplicate the path in
    // JournalContext. Native systemd removal depends on this for daemon-reload.
    const auto touch_root = temp_dir / "service-plan-touch-root";
    std::filesystem::create_directories(touch_root);
    auto touch_txn = sage::archive::FilesystemTransaction::create(touch_root);
    sage::archive::JournalContext touch_ctx;
    touch_ctx.kind = "reconcile";
    touch_ctx.sysroot = touch_root.string();
    const std::string removed_unit = "usr/lib/systemd/system/native.service";
    if (!touch_txn) {
        sage::util::log_error(
            "Failed to create service journal fixture: {}", touch_txn.error());
        return 1;
    }
    touch_txn->plan_remove_file(removed_unit);
    auto touched_journal = sage::archive::parse_journal(sage::archive::render_journal(
        touch_ctx, touch_txn->journal_entries()));
    if (!touched_journal || std::ranges::find(
            touched_journal->ctx.touched, std::pair{'F', removed_unit})
            == touched_journal->ctx.touched.end()) {
        sage::util::log_error("Filesystem plan removal was absent from journal context");
        return 1;
    }

    auto write_canary_recipe = [](const std::filesystem::path& dir,
                                   std::string_view toml_body) {
        std::filesystem::create_directories(dir);
        std::ofstream recipe(dir / "recipe.toml");
        recipe << toml_body;
        return recipe.good();
    };
    auto build_with_root = [](const std::filesystem::path& recipe_dir,
                              const std::filesystem::path& target_root,
                              const std::filesystem::path& extract_dir,
                              std::string_view pkg_filename)
        -> std::expected<sage::package::PackageManifest, std::string> {
        CliOptions build_opts;
        build_opts.args = {recipe_dir.string()};
        build_opts.target_root = target_root;
        if (cmd_build(build_opts) != 0) {
            return std::unexpected("cmd_build failed for " + recipe_dir.string());
        }
        auto extracted = sage::archive::extract_package(
            recipe_dir / pkg_filename, extract_dir);
        if (!extracted) return std::unexpected(extracted.error());
        return std::move(extracted->manifest);
    };

    // (g) A service.toml beside the recipe rides the manifest verbatim;
    // the parse round-trip must keep the daemon definition intact for
    // `sage rebuild` to regenerate scripts on an init switch.
    auto svc_dir = temp_dir / "bcfg-svc";
    if (!write_canary_recipe(svc_dir, R"(schema_version = 1
[package]
name = "svcanary"
version = "1.0.0"
release = "1"
arch = "any"
description = "service manifest canary"
license = "MIT"
channel = "system"
install = [
'mkdir -p "$DESTDIR/usr/bin"',
'printf "#!/bin/sh\n" > "$DESTDIR/usr/bin/svcanary"',
]
)")) {
        sage::util::log_error("Failed to create service canary recipe");
        return 1;
    }
    {
        std::ofstream f(svc_dir / "service.toml");
        f << R"(schema_version = 1
[service]
name = "svcanary"
description = "canary daemon"
exec_start = "/usr/bin/svcanary --foreground"
after = ["net"]
)";
    }
    auto svcpkg = build_with_root(svc_dir, canary_root, temp_dir / "bcfg-svc-x",
                                  "svcanary-1.0.0-1-any.pkg.tar.zst");
    if (!svcpkg || svcpkg->service_toml.empty()) {
        sage::util::log_error("service.toml did not ride the built manifest");
        return 1;
    }
    auto spec_back = sage::service::ServiceSpec::parse_toml(svcpkg->service_toml);
    if (!spec_back || spec_back->name != "svcanary"
        || spec_back->exec_start != "/usr/bin/svcanary --foreground"
        || spec_back->after.size() != 1 || spec_back->after[0] != "net") {
        sage::util::log_error("Manifest service_toml failed to round-trip through serialization");
        return 1;
    }

    // Riding the manifest is only half the journey. The reconcile pass
    // renders init scripts from what LMDB holds, so the definition has to
    // survive installation, and a reconcile that finds no provider change
    // still has to generate what is missing -- otherwise a daemon is only
    // ever wired up by an unrelated swap happening to come along.
    auto svc_root = temp_dir / "bcfg-svc-root";
    std::filesystem::create_directories(svc_root / "etc/sage");
    {
        // No [providers] table on purpose. Declaring one on a root whose
        // LMDB has no provider bindings yet would make the planner see
        // every exclusive capability as needing a swap, and the plan under
        // test here is specifically the one with nothing to swap. Absent
        // providers leave the planner with no exclusive interfaces to
        // compare and target_init falling back to systemd.
        std::ofstream f(svc_root / "etc/sage/system.toml");
        f << "schema_version = 1\n";
    }
    CliOptions svc_install;
    svc_install.target_root = svc_root;
    svc_install.args = {
        (svc_dir / "svcanary-1.0.0-1-any.pkg.tar.zst").string()
    };
    if (cmd_install(svc_install, "/") != 0) {
        sage::util::log_error("Failed to install the service manifest canary");
        return 1;
    }
    auto svc_db = sage::db::Database::open(svc_root / "var/lib/sage/data.mdb");
    if (!svc_db) {
        sage::util::log_error("Failed to open the service canary database");
        return 1;
    }
    auto close_service_db = [&]() {
        svc_db = std::unexpected(std::string("service canary database closed"));
    };
    auto reopen_service_db = [&]() {
        svc_db = sage::db::Database::open(svc_root / "var/lib/sage/data.mdb");
        return static_cast<bool>(svc_db);
    };
    auto installed_svc = svc_db->get_package("svcanary");
    if (!installed_svc || !*installed_svc
        || (**installed_svc).service_toml != svcpkg->service_toml) {
        sage::util::log_error("Installed package database lost service.toml");
        return 1;
    }
    const auto svc_unit = svc_root / "usr/lib/systemd/system/svcanary.service";
    if (std::filesystem::exists(svc_unit)) {
        sage::util::log_error("Install rendered a service unit it was not supposed to");
        return 1;
    }
    auto svc_cfg = sage::config::SystemConfig::load_from_root(svc_root);
    if (!svc_cfg) {
        sage::util::log_error("Failed to load the service canary system config");
        return 1;
    }
    // Planned, not hand-built: this must exercise the real no-change plan,
    // which is exactly the path that used to return before generating.
    auto svc_plan = sage::rebuild::ReconcileEngine::calculate_diff(*svc_db, *svc_cfg, false);
    if (!svc_plan || svc_plan->has_changes
        || svc_plan->target_init != sage::service::InitType::Systemd) {
        sage::util::log_error(
            "Service canary reconcile should have planned no change against systemd");
        return 1;
    }
    auto svc_exec = sage::rebuild::ReconcileEngine::execute(
        *svc_db, *svc_plan, svc_root, false, svc_cfg->providers);
    if (!svc_exec || !std::filesystem::exists(svc_unit)) {
        sage::util::log_error(
            "Reconcile without provider changes did not generate the service definition: {}",
            svc_exec.error_or("unit missing"));
        return 1;
    }
    // ... and having generated it, the next reconcile has nothing left to
    // do: the cheap pending check must not drag a staging transaction into
    // every subsequent rebuild.
    auto svc_plan_again = sage::rebuild::ReconcileEngine::calculate_diff(*svc_db, *svc_cfg, false);
    auto svc_exec_again = svc_plan_again
        ? sage::rebuild::ReconcileEngine::execute(
              *svc_db, *svc_plan_again, svc_root, false, svc_cfg->providers)
        : std::expected<void, std::string>(std::unexpected("plan failed"));
    if (!svc_exec_again) {
        sage::util::log_error("Second reconcile over a settled root failed: {}",
            svc_exec_again.error());
        return 1;
    }

    // Executable backends are settled only when both content and mode
    // match. Losing an execute bit must make the read-only preflight enter
    // reconcile and restore the generated script to 0755.
    auto openrc_plan = *svc_plan_again;
    openrc_plan.target_init = sage::service::InitType::OpenRC;
    const auto openrc_script = svc_root / "etc/init.d/svcanary";
    const auto previous_umask = ::umask(0077);
    auto openrc_first = sage::rebuild::ReconcileEngine::execute(
        *svc_db, openrc_plan, svc_root, false, svc_cfg->providers);
    ::umask(previous_umask);
    std::error_code openrc_initial_mode_ec;
    const auto openrc_initial_mode = std::filesystem::status(
        openrc_script, openrc_initial_mode_ec).permissions();
    if (!openrc_first || openrc_initial_mode_ec
        || (openrc_initial_mode & std::filesystem::perms::mask)
            != static_cast<std::filesystem::perms>(0755)
        || ::chmod(openrc_script.c_str(), 0644) != 0) {
        sage::util::log_error("Failed to prepare OpenRC mode-drift fixture");
        return 1;
    }
    const auto repair_umask = ::umask(0077);
    auto openrc_repaired = sage::rebuild::ReconcileEngine::execute(
        *svc_db, openrc_plan, svc_root, false, svc_cfg->providers);
    ::umask(repair_umask);
    std::error_code openrc_mode_ec;
    const auto openrc_mode = std::filesystem::status(
        openrc_script, openrc_mode_ec).permissions();
    if (!openrc_repaired || openrc_mode_ec
        || (openrc_mode & std::filesystem::perms::mask)
            != static_cast<std::filesystem::perms>(0755)) {
        sage::util::log_error("Reconcile did not repair generated OpenRC mode drift");
        return 1;
    }

    auto build_service_revision = [&](int release, std::string_view description,
                                       std::string_view exec_start,
                                       bool with_service) -> decltype(svcpkg) {
        auto recipe = std::format(R"(schema_version = 1
[package]
name = "svcanary"
version = "1.0.0"
release = "{}"
arch = "any"
description = "service manifest canary"
license = "MIT"
channel = "system"
install = [
'mkdir -p "$DESTDIR/usr/bin"',
'printf "#!/bin/sh\n" > "$DESTDIR/usr/bin/svcanary"',
]
)", release);
        if (!write_canary_recipe(svc_dir, recipe)) return {};
        if (with_service) {
            std::ofstream f(svc_dir / "service.toml");
            f << std::format(R"(schema_version = 1
[service]
name = "svcanary"
description = "{}"
exec_start = "{}"
after = ["net"]
)", description, exec_start);
        } else {
            std::filesystem::remove(svc_dir / "service.toml");
        }
        return build_with_root(
            svc_dir, canary_root, temp_dir / std::format("bcfg-svc-x-{}", release),
            std::format("svcanary-1.0.0-{}-any.pkg.tar.zst", release));
    };
    auto reconcile_service = [&]() -> std::expected<void, std::string> {
        auto plan = sage::rebuild::ReconcileEngine::calculate_diff(
            *svc_db, *svc_cfg, false);
        if (!plan) return std::unexpected(plan.error());
        if (plan->has_changes) {
            return std::unexpected("service reconcile unexpectedly planned a provider change");
        }
        return sage::rebuild::ReconcileEngine::execute(
            *svc_db, *plan, svc_root, false, svc_cfg->providers);
    };
    auto read_service_unit = [&]() {
        std::ifstream f(svc_unit);
        std::stringstream text;
        text << f.rdbuf();
        return text.str();
    };

    // Service names are global identities, just like their generated
    // destination paths. A second package may not register the same name.
    auto duplicate_dir = temp_dir / "bcfg-svc-duplicate";
    if (!write_canary_recipe(duplicate_dir, R"(schema_version = 1
[package]
name = "svcduplicate"
version = "1.0.0"
release = "1"
arch = "any"
description = "duplicate service identity canary"
license = "MIT"
channel = "system"
install = [
'mkdir -p "$DESTDIR/usr/bin"',
'printf "#!/bin/sh\n" > "$DESTDIR/usr/bin/svcduplicate"',
]
)")) {
        sage::util::log_error("Failed to create duplicate service recipe");
        return 1;
    }
    {
        std::ofstream f(duplicate_dir / "service.toml");
        f << R"(schema_version = 1
[service]
name = "svcanary"
exec_start = "/usr/bin/svcduplicate"
)";
    }
    auto duplicate_pkg = build_with_root(
        duplicate_dir, canary_root, temp_dir / "bcfg-svc-duplicate-x",
        "svcduplicate-1.0.0-1-any.pkg.tar.zst");
    svc_install.args = {
        (duplicate_dir / "svcduplicate-1.0.0-1-any.pkg.tar.zst").string()
    };
    close_service_db();
    if (!duplicate_pkg || cmd_install(svc_install, "/") == 0
        || !reopen_service_db()) {
        sage::util::log_error("Duplicate service name was accepted");
        return 1;
    }
    auto duplicate_record = svc_db->get_package("svcduplicate");
    if (!duplicate_record || *duplicate_record
        || read_service_unit().find("ExecStart=/usr/bin/svcanary --foreground")
            == std::string::npos) {
        sage::util::log_error("Rejected duplicate service changed installed state");
        return 1;
    }

    // Updating the universal definition withdraws the old generated unit
    // during install. The next no-provider-change reconcile must render
    // the new content rather than accepting the stale file by existence.
    auto svcpkg_v2 = build_service_revision(
        2, "updated canary daemon", "/usr/bin/svcanary --updated", true);
    if (!svcpkg_v2) {
        sage::util::log_error("Failed to build the updated service canary");
        return 1;
    }
    svc_install.args = {
        (svc_dir / "svcanary-1.0.0-2-any.pkg.tar.zst").string()
    };
    close_service_db();
    if (cmd_install(svc_install, "/") != 0 || std::filesystem::exists(svc_unit)) {
        sage::util::log_error("Service upgrade did not retire the old generated unit");
        return 1;
    }
    if (!reopen_service_db()) {
        sage::util::log_error("Failed to reopen the updated service database");
        return 1;
    }
    auto svc_update = reconcile_service();
    if (!svc_update
        || read_service_unit().find("ExecStart=/usr/bin/svcanary --updated")
            == std::string::npos) {
        sage::util::log_error(
            "Service upgrade did not render the updated definition: {}",
            svc_update.error_or("updated content missing"));
        return 1;
    }

    // Dropping service.toml on a later revision must retire the generated
    // unit without waiting for a provider transition.
    auto svcpkg_v3 = build_service_revision(3, "", "", false);
    if (!svcpkg_v3) {
        sage::util::log_error("Failed to build the service-less canary");
        return 1;
    }
    svc_install.args = {
        (svc_dir / "svcanary-1.0.0-3-any.pkg.tar.zst").string()
    };
    close_service_db();
    if (cmd_install(svc_install, "/") != 0 || std::filesystem::exists(svc_unit)) {
        sage::util::log_error("Dropping service.toml left a generated unit behind");
        return 1;
    }
    if (!reopen_service_db()) {
        sage::util::log_error("Failed to reopen the service-less database");
        return 1;
    }
    auto installed_without_service = svc_db->get_package("svcanary");
    if (!installed_without_service || !*installed_without_service
        || !(**installed_without_service).service_toml.empty()) {
        sage::util::log_error("Service-less upgrade kept stale service metadata");
        return 1;
    }

    // Ordinary package removal must retire generated units too, not only
    // provider-removal transactions in ReconcileEngine.
    auto svcpkg_v4 = build_service_revision(
        4, "removal canary daemon", "/usr/bin/svcanary --remove", true);
    if (!svcpkg_v4) {
        sage::util::log_error("Failed to build the removal service canary");
        return 1;
    }
    svc_install.args = {
        (svc_dir / "svcanary-1.0.0-4-any.pkg.tar.zst").string()
    };
    close_service_db();
    if (cmd_install(svc_install, "/") != 0 || !reopen_service_db()
        || !reconcile_service() || !std::filesystem::exists(svc_unit)) {
        sage::util::log_error("Failed to prepare the service removal canary");
        return 1;
    }
    // Defensive compatibility for a database created before service names
    // became unique: removing one declarer must keep the generated unit
    // until the final declarer goes away.
    {
        auto txn = svc_db->begin_write_txn();
        sage::package::PackageManifest legacy_alias;
        legacy_alias.name = "svcanary-legacy-alias";
        legacy_alias.version = sage::package::Version::parse("1.0.0-1");
        legacy_alias.channel = "system";
        legacy_alias.service_toml = svcpkg_v4->service_toml;
        if (!txn || !svc_db->put_package(*txn, legacy_alias) || !txn->commit()) {
            sage::util::log_error("Failed to register legacy duplicate service fixture");
            return 1;
        }
    }
    CliOptions svc_remove;
    svc_remove.target_root = svc_root;
    svc_remove.args = {"svcanary"};
    close_service_db();
    if (cmd_remove(svc_remove) != 0 || !std::filesystem::exists(svc_unit)) {
        sage::util::log_error("Removing one legacy declarer retired a shared unit");
        return 1;
    }
    svc_remove.args = {"svcanary-legacy-alias"};
    if (cmd_remove(svc_remove) != 0 || std::filesystem::exists(svc_unit)) {
        sage::util::log_error("Removing the final service declarer left its unit behind");
        return 1;
    }

    // A separately owned native unit must survive a daemon metadata
    // update. This models split packages such as systemd + systemd-units:
    // the daemon carries service.toml while another package owns the unit.
    svc_install.args = {
        (svc_dir / "svcanary-1.0.0-4-any.pkg.tar.zst").string()
    };
    if (cmd_install(svc_install, "/") != 0 || !reopen_service_db()
        || !reconcile_service() || !std::filesystem::exists(svc_unit)) {
        sage::util::log_error("Failed to restore the native-owner service fixture");
        return 1;
    }
    {
        auto txn = svc_db->begin_write_txn();
        sage::package::PackageManifest native_unit;
        native_unit.name = "svcanary-native-unit";
        native_unit.version = sage::package::Version::parse("1.0.0-1");
        native_unit.channel = "system";
        sage::package::FileEntry unit_file;
        unit_file.path = "usr/lib/systemd/system/svcanary.service";
        unit_file.type = sage::package::FileType::Regular;
        unit_file.mode = 0644;
        native_unit.files = {unit_file};
        if (!txn
            || !svc_db->put_package(*txn, native_unit)
            || !svc_db->register_files(
                *txn, native_unit.name, native_unit.channel, native_unit.files)
            || !txn->commit()) {
            sage::util::log_error("Failed to register the native unit owner fixture");
            return 1;
        }
        std::ofstream(svc_unit) << "native split-package unit\n";
    }
    auto svcpkg_v5 = build_service_revision(
        5, "native owner update", "/usr/bin/svcanary --v5", true);
    if (!svcpkg_v5) {
        sage::util::log_error("Failed to build the native-owner update canary");
        return 1;
    }
    svc_install.args = {
        (svc_dir / "svcanary-1.0.0-5-any.pkg.tar.zst").string()
    };
    close_service_db();
    if (cmd_install(svc_install, "/") != 0 || !reopen_service_db()
        || !reconcile_service()
        || read_service_unit() != "native split-package unit\n") {
        sage::util::log_error("Service update removed a separately owned native unit");
        return 1;
    }
    auto native_owners = svc_db->get_path_owners(
        "usr/lib/systemd/system/svcanary.service");
    if (!native_owners
        || std::ranges::find(
            *native_owners, "svcanary-native-unit:system")
            == native_owners->end()) {
        sage::util::log_error("Service update lost the native unit ownership claim");
        return 1;
    }



    return 0;
}

} // namespace sage::tests
