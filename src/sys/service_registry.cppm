export module sage.service_registry;

import std;
import sage.archive;
import sage.db;
import sage.service;
import sage.vendor.lmdb;

export namespace sage::service_registry {

std::expected<std::optional<std::string>, std::string> find_other_declarer(
    db::Database& db,
    vendor::lmdb::MdbTxn& txn,
    std::string_view service_name,
    std::string_view excluding_package)
{
    auto installed = db.list_installed_packages(txn);
    if (!installed) return std::unexpected(installed.error());
    for (const auto& pkg : *installed) {
        if (pkg.name == excluding_package || pkg.service_toml.empty()) continue;
        auto declaration = service::ServiceSpec::parse_toml(pkg.service_toml);
        if (declaration && declaration->name == service_name) return pkg.name;
    }
    return std::nullopt;
}

std::expected<void, std::string> validate_unique(
    db::Database& db,
    vendor::lmdb::MdbTxn& txn,
    std::string_view package_name,
    std::string_view service_toml)
{
    if (service_toml.empty()) return {};
    auto declaration = service::ServiceSpec::parse_toml(service_toml);
    if (!declaration) {
        return std::unexpected(std::format(
            "Invalid service declaration for '{}': {}", package_name, declaration.error()));
    }
    auto other = find_other_declarer(db, txn, declaration->name, package_name);
    if (!other) return std::unexpected(other.error());
    if (*other) {
        return std::unexpected(std::format(
            "Service name '{}' is already declared by package '{}'",
            declaration->name, **other));
    }
    return {};
}

// Retire only generated artifacts. Package-owned native units win, and a
// duplicate declaration already present in an older database keeps the unit
// alive until its final declarer is removed.
std::expected<std::vector<std::pair<char, std::string>>, std::string>
plan_remove_scripts(
    db::Database& db,
    vendor::lmdb::MdbTxn& txn,
    archive::FilesystemTransaction& fsx,
    std::string_view service_name,
    std::string_view declaring_package)
{
    auto other = find_other_declarer(
        db, txn, service_name, declaring_package);
    if (!other) return std::unexpected(other.error());
    if (*other) return std::vector<std::pair<char, std::string>>{};

    const std::array<std::pair<std::string, bool>, 8> paths{{
        {std::format("etc/init.d/{}", service_name), false},
        {std::format("usr/lib/systemd/system/{}.service", service_name), false},
        {std::format("etc/dinit.d/{}", service_name), false},
        {std::format("usr/lib/loom/services/{}.toml", service_name), false},
        {std::format("etc/sv/{}/run", service_name), false},
        {std::format("etc/sv/{}", service_name), true},
        {std::format("etc/s6/services/{}/run", service_name), false},
        {std::format("etc/s6/services/{}", service_name), true},
    }};
    std::vector<std::pair<char, std::string>> touched;
    for (const auto& [path, directory] : paths) {
        auto owners = db.get_path_owners(txn, path);
        if (!owners) return std::unexpected(owners.error());
        if (!owners->empty()) continue;
        if (directory) fsx.plan_remove_dir(path);
        else fsx.plan_remove_file(path);
        touched.emplace_back(directory ? 'D' : 'F', path);
    }
    return touched;
}

} // namespace sage::service_registry
