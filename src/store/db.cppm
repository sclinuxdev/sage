export module sage.db;

import std;
import sage.vendor.lmdb;
import sage.vendor.toml;
import sage.package;
import sage.util;

export namespace sage::db {

using std::size_t;

struct LmdbPaths {
    std::filesystem::path environment;
    std::filesystem::path data_file;
    std::filesystem::path lock_file;
};

inline LmdbPaths resolve_lmdb_paths(const std::filesystem::path& configured_path) {
    auto environment = configured_path.extension() == ".mdb"
        ? configured_path.parent_path()
        : configured_path;
    return LmdbPaths{
        .environment = environment,
        .data_file = environment / "data.mdb",
        .lock_file = environment / "lock.mdb",
    };
}

// A files-table value is a newline-separated set of "pkg:channel" owners.
// Regular files and symlinks always carry exactly one; explicitly declared
// directories accumulate one claim per installing package, so removing one
// claimant releases only its own claim and the last claimant deletes the
// directory. Single-owner values are byte-identical to the historical format.
std::vector<std::string_view> split_owners(std::string_view value) {
    std::vector<std::string_view> owners;
    for (const auto owner : util::split(value, '\n')) {
        if (!owner.empty()) owners.push_back(owner);
    }
    return owners;
}

std::string join_owners(const std::vector<std::string_view>& owners) {
    std::string joined;
    for (size_t i = 0; i < owners.size(); ++i) {
        if (i > 0) joined.push_back('\n');
        joined.append(owners[i]);
    }
    return joined;
}

std::string_view owner_package(std::string_view owner) {
    const auto colon = owner.find(':');
    return colon == std::string_view::npos ? owner : owner.substr(0, colon);
}
// A committed package operation lives in two places at once: an LMDB record
// here and a transaction directory on disk (journal + staged payload). The
// phase field is the crash-recovery state machine; the record's absence is
// itself a state:
//
//   no record              -> nothing committed (staging is RAII/孤儿 GC territory)
//   filesystem_pending     -> package/provider/file metadata AND this record
//                             were committed atomically in one write txn;
//                             journal+payload were fsynced *before* that
//                             commit, so the record can never point at
//                             half-persisted evidence. Recovery: verify
//                             journal hash, publish to live tree.
//   postprocess_pending    -> filesystem publish done, post-processing
//                             (triggers/profile/toolchains) not. Recovery:
//                             rerun post-processing from journal context.
//   no record again        -> everything finished; only then may the record
//                             be deleted and the txn directory retired.
//
// Every failure keeps the record and evidence untouched so the next run
// retries from the same phase.
inline constexpr std::string_view phase_filesystem_pending = "filesystem_pending";
inline constexpr std::string_view phase_postprocess_pending = "postprocess_pending";

inline constexpr auto operation_schema_version = 1;

struct FilesystemOperationRecord {
    std::string id, kind, phase, transaction_dir, journal_sha256;
};

// The value is TOML per contract §3, but written by hand: toml++'s stream
// serializer cannot cross the sage.vendor.toml module boundary (only its
// parser is re-exported), and manifest.cppm already established this exact
// manual-writer pattern for DB values.
std::string serialize_operation(const FilesystemOperationRecord& record) {
    const auto quote = [](std::string_view value) {
        return '"' + package::escape_toml_basic_string(value) + '"';
    };
    std::ostringstream ss;
    ss << "schema_version = " << operation_schema_version << "\n\n";
    ss << "id = " << quote(record.id) << "\n";
    ss << "kind = " << quote(record.kind) << "\n";
    ss << "phase = " << quote(record.phase) << "\n";
    ss << "transaction_dir = " << quote(record.transaction_dir) << "\n";
    ss << "journal_sha256 = " << quote(record.journal_sha256) << "\n";
    return ss.str();
}

std::expected<FilesystemOperationRecord, std::string> parse_operation(
    std::string_view id,
    std::string_view value)
{
    auto parsed = vendor::toml::parse_string(value);
    if (!parsed) {
        return std::unexpected(std::format(
            "Failed to parse operation '{}' record: {}", id, parsed.error()));
    }
    const auto& tbl = *parsed;

    // Unknown future schema versions must fail loudly rather than be
    // half-understood by a recovery driver that would then delete or
    // republish evidence on wrong assumptions.
    const auto* schema_version = tbl["schema_version"].as_integer();
    if (!schema_version) {
        return std::unexpected(std::format(
            "Operation '{}' record has no integer schema_version", id));
    }
    if (schema_version->get() != operation_schema_version) {
        return std::unexpected(std::format(
            "Operation '{}' record uses unsupported schema version {}", id, schema_version->get()));
    }

    FilesystemOperationRecord record;
    for (const auto& [key, target] : {
        std::pair{std::string_view{"id"}, &record.id},
        std::pair{std::string_view{"kind"}, &record.kind},
        std::pair{std::string_view{"phase"}, &record.phase},
        std::pair{std::string_view{"transaction_dir"}, &record.transaction_dir},
        std::pair{std::string_view{"journal_sha256"}, &record.journal_sha256},
    }) {
        if (auto v = tbl[key].value<std::string_view>()) *target = std::string(*v);
        else return std::unexpected(std::format(
            "Operation '{}' record is missing string field '{}'", id, key));
    }
    return record;
}


// Ownership claims retired by the running transaction itself: cleaned path
// -> names of the packages whose previous revisions give that path up during
// this very transaction. A split-package upgrade moves files between package
// identities inside one install run -- when the new owner is checked, the
// old owner's claim is still on record -- so conflict detection must
// tolerate exactly these in-flight handovers instead of rejecting every
// foreign claim against the pre-transaction database state.
using ReleasedClaims =
    std::unordered_map<std::string, std::unordered_set<std::string>>;

class Database {
public:
    Database() = default;
    ~Database() = default;

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    Database(Database&&) noexcept = default;
    Database& operator=(Database&&) noexcept = default;

    static std::expected<Database, std::string> open(
        const std::filesystem::path& db_path, 
        bool read_only = false,
        size_t map_size = 10ULL * 1024 * 1024 * 1024) 
    {
        return open_impl(db_path, read_only, true, true, map_size);
    }

    static std::expected<Database, std::string> open_existing_read_only_no_lock(
        const std::filesystem::path& db_path,
        size_t map_size = 10ULL * 1024 * 1024 * 1024)
    {
        return open_impl(db_path, true, false, false, map_size);
    }

private:
    static std::expected<Database, std::string> open_impl(
        const std::filesystem::path& db_path,
        bool read_only,
        bool use_lmdb_lock,
        bool create_path,
        size_t map_size)
    {
        const auto paths = resolve_lmdb_paths(db_path);
        unsigned int env_flags = read_only ? vendor::lmdb::flag_rdonly : 0;
        if (!use_lmdb_lock) env_flags |= vendor::lmdb::flag_nolock;
        auto env_res = vendor::lmdb::MdbEnv::create(
            paths.environment, map_size, 32, env_flags, create_path);
        if (!env_res) return std::unexpected(env_res.error());

        Database db;
        db.env_ = std::move(*env_res);

        // Open/Create tables in initial transaction
        auto txn_res = vendor::lmdb::MdbTxn::begin(db.env_, read_only);
        if (!txn_res) return std::unexpected(txn_res.error());
        auto& txn = *txn_res;

        unsigned int dbi_flags = read_only ? 0 : vendor::lmdb::flag_create;

        auto dbi_pkg = vendor::lmdb::MdbDbi::open(txn, "packages", dbi_flags);
        if (!dbi_pkg) return std::unexpected("Failed to open packages table: " + dbi_pkg.error());
        db.dbi_packages_ = *dbi_pkg;

        auto dbi_files = vendor::lmdb::MdbDbi::open(txn, "files", dbi_flags);
        if (!dbi_files) return std::unexpected("Failed to open files table: " + dbi_files.error());
        db.dbi_files_ = *dbi_files;

        auto dbi_prov = vendor::lmdb::MdbDbi::open(txn, "provides", dbi_flags);
        if (!dbi_prov) return std::unexpected("Failed to open provides table: " + dbi_prov.error());
        db.dbi_provides_ = *dbi_prov;

        auto dbi_chan = vendor::lmdb::MdbDbi::open(txn, "channels", dbi_flags);
        if (!dbi_chan) return std::unexpected("Failed to open channels table: " + dbi_chan.error());
        db.dbi_channels_ = *dbi_chan;

        auto dbi_sys = vendor::lmdb::MdbDbi::open(txn, "system", dbi_flags);
        if (!dbi_sys) return std::unexpected("Failed to open system table: " + dbi_sys.error());
        db.dbi_system_ = *dbi_sys;

        auto dbi_ops = vendor::lmdb::MdbDbi::open(txn, "operations", dbi_flags);
        if (!dbi_ops) return std::unexpected("Failed to open operations table: " + dbi_ops.error());
        db.dbi_operations_ = *dbi_ops;

        auto commit_res = txn.commit();
        if (!commit_res) return std::unexpected(commit_res.error());

        return db;
    }

public:

    std::expected<vendor::lmdb::MdbTxn, std::string> begin_read_txn() {
        return vendor::lmdb::MdbTxn::begin(env_, true);
    }

    std::expected<vendor::lmdb::MdbTxn, std::string> begin_write_txn() {
        return vendor::lmdb::MdbTxn::begin(env_, false);
    }

    // ========================================================================
    // Packages Table
    // ========================================================================

    std::expected<std::optional<package::PackageManifest>, std::string> get_package(
        std::string_view name)
    {
        auto txn = begin_read_txn();
        if (!txn) {
            return std::unexpected("Failed to open installed package read transaction: " + txn.error());
        }
        return get_package(*txn, name);
    }

    std::expected<std::optional<package::PackageManifest>, std::string> get_package(
        vendor::lmdb::MdbTxn& txn,
        std::string_view name)
    {
        auto val = dbi_packages_.get_checked(txn, name);
        if (!val) {
            return std::unexpected("Failed to read installed package metadata: " + val.error());
        }
        if (!*val) return std::optional<package::PackageManifest>{};
        auto parsed = package::PackageManifest::parse_toml(**val);
        if (!parsed) {
            return std::unexpected(std::format(
                "Failed to parse installed package '{}' metadata: {}", name, parsed.error()));
        }
        if (parsed->name != name) {
            return std::unexpected(std::format(
                "Installed package key '{}' contains metadata for '{}'", name, parsed->name));
        }
        return std::optional<package::PackageManifest>{std::move(*parsed)};
    }

    std::expected<void, std::string> put_package(
        vendor::lmdb::MdbTxn& txn, 
        const package::PackageManifest& manifest) 
    {
        std::string serialized = manifest.serialize_toml();
        return dbi_packages_.put(txn, manifest.name, serialized);
    }

    std::expected<void, std::string> del_package(
        vendor::lmdb::MdbTxn& txn, 
        std::string_view name) 
    {
        return dbi_packages_.del(txn, name);
    }

    std::expected<std::vector<package::PackageManifest>, std::string> list_installed_packages() {
        auto txn = begin_read_txn();
        if (!txn) return std::unexpected("Failed to open installed package read transaction: " + txn.error());
        return list_installed_packages(*txn);
    }

    std::expected<std::vector<package::PackageManifest>, std::string> list_installed_packages(
        vendor::lmdb::MdbTxn& txn)
    {
        std::vector<package::PackageManifest> list;
        auto cur_res = vendor::lmdb::MdbCursor::open(txn, dbi_packages_);
        if (!cur_res) return std::unexpected("Failed to open installed package cursor: " + cur_res.error());
        auto& cursor = *cur_res;

        std::string_view k, v;
        auto entry = cursor.first(k, v);
        if (!entry) {
            return std::unexpected("Failed to read installed package cursor: " + entry.error());
        }
        while (*entry) {
            auto pkg = package::PackageManifest::parse_toml(v);
            if (!pkg) {
                return std::unexpected(std::format(
                    "Failed to parse installed package '{}' metadata: {}", k, pkg.error()));
            }
            if (pkg->name != k) {
                return std::unexpected(std::format(
                    "Installed package key '{}' contains metadata for '{}'", k, pkg->name));
            }
            list.push_back(std::move(*pkg));

            entry = cursor.next(k, v);
            if (!entry) {
                return std::unexpected("Failed to advance installed package cursor: " + entry.error());
            }
        }
        return list;
    }

    // ========================================================================
    // Files Table & Conflict Detection
    // ========================================================================

    std::expected<std::vector<std::string>, std::string> get_path_owners(
        std::string_view rel_path)
    {
        auto txn = begin_read_txn();
        if (!txn) {
            return std::unexpected("Failed to open file ownership read transaction: " + txn.error());
        }
        return get_path_owners(*txn, rel_path);
    }

    // Every owner registered for a path, empty when the path is unregistered.
    std::expected<std::vector<std::string>, std::string> get_path_owners(
        vendor::lmdb::MdbTxn& txn,
        std::string_view rel_path)
    {
        auto cleaned = util::clean_rel_path(rel_path);
        auto val = dbi_files_.get_checked(txn, cleaned);
        if (!val) {
            return std::unexpected("Failed to read file ownership: " + val.error());
        }
        std::vector<std::string> owners;
        if (*val) {
            for (const auto owner : split_owners(**val)) owners.emplace_back(owner);
        }
        return owners;
    }

    std::expected<std::vector<std::string>, std::string> get_package_files(
        std::string_view package_name)
    {
        auto txn = begin_read_txn();
        if (!txn) return std::unexpected("Failed to open file ownership read transaction: " + txn.error());
        return get_package_files(*txn, package_name);
    }

    std::expected<std::vector<std::string>, std::string> get_package_files(
        vendor::lmdb::MdbTxn& txn,
        std::string_view package_name)
    {
        std::vector<std::string> files;
        auto cur_res = vendor::lmdb::MdbCursor::open(txn, dbi_files_);
        if (!cur_res) return std::unexpected("Failed to open file ownership cursor: " + cur_res.error());
        auto& cursor = *cur_res;
        std::string_view k, v;
        auto entry = cursor.first(k, v);
        if (!entry) return std::unexpected("Failed to read file ownership cursor: " + entry.error());
        while (*entry) {
            for (const auto owner : split_owners(v)) {
                if (owner_package(owner) == package_name) {
                    files.emplace_back(k);
                    break;
                }
            }
            entry = cursor.next(k, v);
            if (!entry) return std::unexpected("Failed to advance file ownership cursor: " + entry.error());
        }
        return files;
    }

    // Register package files into LMDB with atomic conflict checking
    // allowed_owner repeats the caller's own previous identity;
    // released_claims lists the foreign owners giving their paths up inside
    // the same transaction. A path conflicts only when an owner survives it.
    std::expected<void, std::string> check_file_conflicts(
        vendor::lmdb::MdbTxn& txn,
        std::optional<std::string_view> allowed_owner,
        const std::vector<package::FileEntry>& files,
        const ReleasedClaims& released_claims = {})
    {
        for (const auto& f : files) {
            if (f.type == package::FileType::Directory) continue;
            auto cleaned = util::clean_rel_path(f.path);
            if (cleaned == "usr/share/info/dir" || cleaned.ends_with("/info/dir")) continue;

            auto existing = dbi_files_.get_checked(txn, cleaned);
            if (!existing) {
                return std::unexpected(std::format(
                    "Failed to check ownership for '{}': {}", cleaned, existing.error()));
            }
            if (!*existing) continue;
            for (const auto owner : split_owners(**existing)) {
                if (allowed_owner && owner == *allowed_owner) continue;
                const auto released = released_claims.find(cleaned);
                if (released != released_claims.end()
                    && released->second.contains(std::string{owner_package(owner)})) {
                    continue;
                }
                return std::unexpected(std::format(
                    "File conflict: '{}' is already owned by '{}'",
                    cleaned, **existing));
            }
        }
        return {};
    }

    std::expected<void, std::string> register_files(
        vendor::lmdb::MdbTxn& txn,
        std::string_view pkg_name,
        std::string_view channel,
        const std::vector<package::FileEntry>& files,
        std::optional<std::string_view> allowed_owner = std::nullopt,
        const ReleasedClaims& released_claims = {})
    {
        std::string owner_val = std::format("{}:{}", pkg_name, channel);

        auto conflict_res = check_file_conflicts(
            txn, allowed_owner, files, released_claims);
        if (!conflict_res) return conflict_res;

        // Insert file records (new owner claims file)
        for (const auto& f : files) {
            auto cleaned = util::clean_rel_path(f.path);
            if (cleaned == "usr/share/info/dir" || cleaned.ends_with("/info/dir")) continue;
            if (f.type != package::FileType::Directory) {
                auto res = dbi_files_.put(txn, cleaned, owner_val);
                if (!res) return res;
                continue;
            }
            // Directories accumulate one claim per package; a re-registering
            // package replaces its previous claim so a channel migration does
            // not stack stale entries onto the shared set.
            auto existing = dbi_files_.get_checked(txn, cleaned);
            if (!existing) {
                return std::unexpected(std::format(
                    "Failed to read ownership for '{}': {}", cleaned, existing.error()));
            }
            std::vector<std::string_view> owners;
            if (*existing) {
                for (const auto owner : split_owners(**existing)) {
                    if (owner_package(owner) != pkg_name) owners.push_back(owner);
                }
            }
            owners.push_back(owner_val);
            auto res = dbi_files_.put(txn, cleaned, join_owners(owners));
            if (!res) return res;
        }

        return {};
    }

    std::expected<void, std::string> unregister_files(
        vendor::lmdb::MdbTxn& txn,
        const std::vector<package::FileEntry>& files,
        std::string_view expected_owner = "")
    {
        for (const auto& f : files) {
            auto cleaned = util::clean_rel_path(f.path);
            if (cleaned == "usr/share/info/dir" || cleaned.ends_with("/info/dir")) continue;

            if (f.type == package::FileType::Directory) {
                // Release only this package's claim; the entry survives while
                // other claimants remain and dies with the last one.
                auto existing = dbi_files_.get_checked(txn, cleaned);
                if (!existing) {
                    return std::unexpected(std::format(
                        "Failed to read ownership for '{}': {}", cleaned, existing.error()));
                }
                if (!*existing) continue;
                const auto owners = split_owners(**existing);
                if (expected_owner.empty()) {
                    auto res = dbi_files_.del(txn, cleaned);
                    if (!res) return res;
                    continue;
                }
                std::vector<std::string_view> remaining;
                for (const auto owner : owners) {
                    if (owner != expected_owner) remaining.push_back(owner);
                }
                if (remaining.size() == owners.size()) continue;
                auto res = remaining.empty()
                    ? dbi_files_.del(txn, cleaned)
                    : dbi_files_.put(txn, cleaned, join_owners(remaining));
                if (!res) return res;
                continue;
            }

            if (!expected_owner.empty()) {
                auto existing = dbi_files_.get_checked(txn, cleaned);
                if (!existing) {
                    return std::unexpected(std::format(
                        "Failed to read ownership for '{}': {}", cleaned, existing.error()));
                }
                if (*existing && **existing != expected_owner) {
                    continue;
                }
            }
            auto res = dbi_files_.del(txn, cleaned);
            if (!res) return res;
        }
        return {};
    }

    // Prune file registrations whose owning package is no longer installed.
    // The files table is the authoritative ownership registry: entries left
    // behind by upgrades/removes (e.g. when a previous version's manifest file
    // list was incomplete) would otherwise block other packages from claiming
    // the same paths. Returns the number of pruned entries.
    std::expected<std::size_t, std::string> prune_orphaned_files(
        vendor::lmdb::MdbTxn& txn)
    {
        std::vector<std::string> orphaned;
        std::unordered_map<std::string, bool> package_exists;
        auto cur_res = vendor::lmdb::MdbCursor::open(txn, dbi_files_);
        if (!cur_res) return std::unexpected("Failed to open orphaned-file cursor: " + cur_res.error());
        auto& cursor = *cur_res;
        std::string_view k, v;
        auto entry = cursor.first(k, v);
        if (!entry) return std::unexpected("Failed to read orphaned-file cursor: " + entry.error());
        while (*entry) {
            bool any_owner_installed = false;
            for (const auto owner : split_owners(v)) {
                const auto package_name = owner_package(owner);
                auto known = package_exists.find(std::string(package_name));
                if (known == package_exists.end()) {
                    auto package = dbi_packages_.get_checked(txn, package_name);
                    if (!package) {
                        return std::unexpected(
                            "Failed to check package while pruning ownership: " + package.error());
                    }
                    known = package_exists.emplace(package_name, package->has_value()).first;
                }
                if (known->second) {
                    any_owner_installed = true;
                    break;
                }
            }
            if (!any_owner_installed) {
                orphaned.emplace_back(k);
            }
            entry = cursor.next(k, v);
            if (!entry) return std::unexpected("Failed to advance orphaned-file cursor: " + entry.error());
        }
        for (const auto& key : orphaned) {
            auto deleted = dbi_files_.del(txn, key);
            if (!deleted) return std::unexpected("Failed to prune orphaned file ownership: " + deleted.error());
        }
        return orphaned.size();
    }

    // ========================================================================
    // Provides Table
    // ========================================================================

    std::optional<std::string> get_provider(std::string_view symbol) {
        auto txn = begin_read_txn();
        if (!txn) return std::nullopt;
        auto val = dbi_provides_.get(*txn, symbol);
        if (val) return std::string(*val);
        return std::nullopt;
    }

    std::expected<void, std::string> register_provides(
        vendor::lmdb::MdbTxn& txn,
        std::string_view pkg_name,
        const std::vector<std::string>& provides) 
    {
        for (const auto& p : provides) {
            auto res = dbi_provides_.put(txn, p, pkg_name);
            if (!res) return res;
        }
        return {};
    }

    std::expected<void, std::string> unregister_provides(
        vendor::lmdb::MdbTxn& txn,
        const std::vector<std::string>& provides) 
    {
        for (const auto& p : provides) {
            auto res = dbi_provides_.del(txn, p);
            if (!res) return res;
        }
        return {};
    }

    // ========================================================================
    // System Providers Table (virtual/init, virtual/udev, virtual/libc)
    // ========================================================================

    std::expected<std::optional<std::string>, std::string> get_system_provider(
        std::string_view iface)
    {
        auto txn = begin_read_txn();
        if (!txn) {
            return std::unexpected("Failed to open system provider read transaction: " + txn.error());
        }
        return get_system_provider(*txn, iface);
    }

    std::expected<std::optional<std::string>, std::string> get_system_provider(
        vendor::lmdb::MdbTxn& txn,
        std::string_view iface)
    {
        auto val = dbi_system_.get_checked(txn, iface);
        if (!val) {
            return std::unexpected("Failed to read system provider: " + val.error());
        }
        if (!*val) return std::optional<std::string>{};
        return std::optional<std::string>{std::string(**val)};
    }

    std::expected<void, std::string> set_system_provider(
        vendor::lmdb::MdbTxn& txn,
        std::string_view iface,
        std::string_view provider) 
    {
        return dbi_system_.put(txn, iface, provider);
    }

    std::expected<std::map<std::string, std::string>, std::string> get_all_system_providers() {
        std::map<std::string, std::string> res;
        auto txn = begin_read_txn();
        if (!txn) return std::unexpected("Failed to open system provider read transaction: " + txn.error());

        auto cur_res = vendor::lmdb::MdbCursor::open(*txn, dbi_system_);
        if (!cur_res) return std::unexpected("Failed to open system provider cursor: " + cur_res.error());
        auto& cursor = *cur_res;

        std::string_view k, v;
        auto entry = cursor.first(k, v);
        if (!entry) return std::unexpected("Failed to read system provider cursor: " + entry.error());
        while (*entry) {
            res[std::string(k)] = std::string(v);
            entry = cursor.next(k, v);
            if (!entry) return std::unexpected("Failed to advance system provider cursor: " + entry.error());
        }
        return res;
    }

    // ========================================================================
    // Operations Table (filesystem transaction recovery records)
    // ========================================================================

    // The caller must put/update the record inside the SAME write transaction
    // as the package/provider/file metadata: that single LMDB commit is what
    // makes "metadata visible" and "operation resumable" indivisible. A record
    // committed without its metadata would send recovery after an install
    // whose packages do not exist; the reverse would strand published files.
    std::expected<void, std::string> put_operation(
        vendor::lmdb::MdbTxn& txn,
        const FilesystemOperationRecord& record)
    {
        return dbi_operations_.put(txn, record.id, serialize_operation(record));
    }

    std::expected<std::optional<FilesystemOperationRecord>, std::string> get_operation(
        vendor::lmdb::MdbTxn& txn,
        std::string_view id)
    {
        auto val = dbi_operations_.get_checked(txn, id);
        if (!val) {
            return std::unexpected(std::format("Failed to read operation '{}' record: {}", id, val.error()));
        }
        if (!*val) return std::optional<FilesystemOperationRecord>{};
        auto parsed = parse_operation(id, **val);
        if (!parsed) return std::unexpected(parsed.error());
        return std::optional<FilesystemOperationRecord>{std::move(*parsed)};
    }

    std::expected<std::vector<FilesystemOperationRecord>, std::string> list_operations() {
        auto txn = begin_read_txn();
        if (!txn) return std::unexpected("Failed to open operation read transaction: " + txn.error());
        return list_operations(*txn);
    }

    std::expected<std::vector<FilesystemOperationRecord>, std::string> list_operations(
        vendor::lmdb::MdbTxn& txn)
    {
        std::vector<FilesystemOperationRecord> list;
        auto cur_res = vendor::lmdb::MdbCursor::open(txn, dbi_operations_);
        if (!cur_res) return std::unexpected("Failed to open operation cursor: " + cur_res.error());
        auto& cursor = *cur_res;

        std::string_view k, v;
        auto entry = cursor.first(k, v);
        if (!entry) return std::unexpected("Failed to read operation cursor: " + entry.error());
        while (*entry) {
            auto parsed = parse_operation(k, v);
            if (!parsed) return std::unexpected(parsed.error());
            list.push_back(std::move(*parsed));

            entry = cursor.next(k, v);
            if (!entry) return std::unexpected("Failed to advance operation cursor: " + entry.error());
        }
        return list;
    }

    // Rewrites the stored value with only the phase changed; the journal
    // hash and transaction dir must survive so recovery evidence stays intact.
    std::expected<void, std::string> update_operation_phase(
        vendor::lmdb::MdbTxn& txn,
        std::string_view id,
        std::string_view phase)
    {
        auto existing = get_operation(txn, id);
        if (!existing) return std::unexpected(existing.error());
        if (!*existing) {
            return std::unexpected(std::format(
                "Cannot update phase of unknown operation '{}'", id));
        }
        (*existing)->phase = std::string(phase);
        return put_operation(txn, **existing);
    }

    // Only reached once publish AND post-processing have fully succeeded --
    // deleting earlier would turn a crash into silent partial state, since a
    // missing record reads as "nothing pending". abandon_id is the only path
    // that may delete a non-finished record.
    std::expected<void, std::string> delete_operation(
        vendor::lmdb::MdbTxn& txn,
        std::string_view id)
    {
        return dbi_operations_.del(txn, id);
    }


private:
    vendor::lmdb::MdbEnv env_;
    vendor::lmdb::MdbDbi dbi_packages_;
    vendor::lmdb::MdbDbi dbi_files_;
    vendor::lmdb::MdbDbi dbi_provides_;
    vendor::lmdb::MdbDbi dbi_channels_;
    vendor::lmdb::MdbDbi dbi_system_;
    vendor::lmdb::MdbDbi dbi_operations_;
};

} // namespace sage::db
