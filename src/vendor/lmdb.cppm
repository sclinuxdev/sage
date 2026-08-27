module;

#include <lmdb.h>

export module sage.vendor.lmdb;

import std;

export namespace sage::vendor::lmdb {

using std::size_t;

inline constexpr unsigned int flag_rdonly = MDB_RDONLY;
inline constexpr unsigned int flag_create = MDB_CREATE;
inline constexpr unsigned int flag_nosync = MDB_NOSYNC;
inline constexpr unsigned int flag_nolock = MDB_NOLOCK;

inline MDB_val to_val(std::string_view sv) noexcept {
    return MDB_val{
        .mv_size = sv.size(),
        .mv_data = const_cast<char*>(sv.data())
    };
}

inline std::string_view from_val(const MDB_val& val) noexcept {
    if (!val.mv_data || val.mv_size == 0) return {};
    return std::string_view{static_cast<const char*>(val.mv_data), val.mv_size};
}

class MdbTxn;
class MdbDbi;
class MdbCursor;

class MdbEnv {
public:
    MdbEnv() noexcept : env_(nullptr) {}
    ~MdbEnv() noexcept { close(); }

    MdbEnv(const MdbEnv&) = delete;
    MdbEnv& operator=(const MdbEnv&) = delete;

    MdbEnv(MdbEnv&& other) noexcept : env_(std::exchange(other.env_, nullptr)) {}
    MdbEnv& operator=(MdbEnv&& other) noexcept {
        if (this != &other) {
            close();
            env_ = std::exchange(other.env_, nullptr);
        }
        return *this;
    }

    static std::expected<MdbEnv, std::string> create(
        const std::filesystem::path& path,
        size_t map_size = 10ULL * 1024 * 1024 * 1024, // 10GB default virtual map size
        unsigned int max_dbs = 32,
        unsigned int flags = 0,
        bool create_path = true)
    {
        MDB_env* env = nullptr;
        int rc = mdb_env_create(&env);
        if (rc != 0) {
            return std::unexpected(mdb_strerror(rc));
        }

        MdbEnv res;
        res.env_ = env;

        mdb_env_set_mapsize(res.env_, map_size);
        mdb_env_set_maxdbs(res.env_, max_dbs);

        if (create_path) {
            std::error_code ec;
            std::filesystem::create_directories(path, ec);
            if (ec) {
                return std::unexpected(std::format(
                    "cannot create LMDB directory '{}': {}", path.string(), ec.message()));
            }
        }
        rc = mdb_env_open(res.env_, path.c_str(), flags, 0644);
        if (rc != 0) {
            return std::unexpected(mdb_strerror(rc));
        }

        return res;
    }

    void close() noexcept {
        if (env_) {
            mdb_env_close(env_);
            env_ = nullptr;
        }
    }

    [[nodiscard]] MDB_env* handle() const noexcept { return env_; }
    [[nodiscard]] explicit operator bool() const noexcept { return env_ != nullptr; }

private:
    MDB_env* env_{nullptr};
};

class MdbTxn {
public:
    MdbTxn() noexcept : txn_(nullptr), committed_(false) {}
    ~MdbTxn() noexcept { abort(); }

    MdbTxn(const MdbTxn&) = delete;
    MdbTxn& operator=(const MdbTxn&) = delete;

    MdbTxn(MdbTxn&& other) noexcept 
        : txn_(std::exchange(other.txn_, nullptr)),
          committed_(std::exchange(other.committed_, false)) {}

    MdbTxn& operator=(MdbTxn&& other) noexcept {
        if (this != &other) {
            abort();
            txn_ = std::exchange(other.txn_, nullptr);
            committed_ = std::exchange(other.committed_, false);
        }
        return *this;
    }

    static std::expected<MdbTxn, std::string> begin(MdbEnv& env, bool read_only = false, MdbTxn* parent = nullptr) {
        MDB_txn* txn = nullptr;
        unsigned int flags = read_only ? MDB_RDONLY : 0;
        int rc = mdb_txn_begin(env.handle(), parent ? parent->handle() : nullptr, flags, &txn);
        if (rc != 0) {
            return std::unexpected(mdb_strerror(rc));
        }
        MdbTxn res;
        res.txn_ = txn;
        res.committed_ = false;
        return res;
    }

    std::expected<void, std::string> commit() noexcept {
        if (!txn_ || committed_) return {};
        int rc = mdb_txn_commit(txn_);
        txn_ = nullptr;
        committed_ = true;
        if (rc != 0) {
            return std::unexpected(mdb_strerror(rc));
        }
        return {};
    }

    void abort() noexcept {
        if (txn_ && !committed_) {
            mdb_txn_abort(txn_);
            txn_ = nullptr;
        }
    }

    [[nodiscard]] MDB_txn* handle() const noexcept { return txn_; }
    [[nodiscard]] explicit operator bool() const noexcept { return txn_ != nullptr; }

private:
    MDB_txn* txn_{nullptr};
    bool committed_{false};
};

class MdbDbi {
public:
    MdbDbi() noexcept : dbi_(0), valid_(false) {}

    static std::expected<MdbDbi, std::string> open(
        MdbTxn& txn, 
        const char* name = nullptr, 
        unsigned int flags = MDB_CREATE) 
    {
        MDB_dbi dbi = 0;
        int rc = mdb_dbi_open(txn.handle(), name, flags, &dbi);
        if (rc != 0) {
            return std::unexpected(mdb_strerror(rc));
        }
        MdbDbi res;
        res.dbi_ = dbi;
        res.valid_ = true;
        return res;
    }

    [[nodiscard]] std::optional<std::string_view> get(MdbTxn& txn, std::string_view key) const noexcept {
        if (!valid_ || !txn.handle()) return std::nullopt;
        MDB_val k = to_val(key);
        MDB_val v{};
        int rc = mdb_get(txn.handle(), dbi_, &k, &v);
        if (rc == 0) {
            return from_val(v);
        }
        return std::nullopt;
    }

    [[nodiscard]] std::expected<std::optional<std::string_view>, std::string> get_checked(
        MdbTxn& txn,
        std::string_view key) const noexcept
    {
        if (!valid_ || !txn.handle()) {
            return std::unexpected(std::string{"Invalid DBI or transaction"});
        }
        MDB_val k = to_val(key);
        MDB_val v{};
        int rc = mdb_get(txn.handle(), dbi_, &k, &v);
        if (rc == 0) {
            return std::optional<std::string_view>{from_val(v)};
        }
        if (rc == MDB_NOTFOUND) {
            return std::optional<std::string_view>{};
        }
        return std::unexpected(mdb_strerror(rc));
    }

    std::expected<void, std::string> put(
        MdbTxn& txn, 
        std::string_view key, 
        std::string_view val, 
        unsigned int flags = 0) const noexcept 
    {
        if (!valid_ || !txn.handle()) {
            return std::unexpected(std::string{"Invalid DBI or transaction"});
        }
        MDB_val k = to_val(key);
        MDB_val v = to_val(val);
        int rc = mdb_put(txn.handle(), dbi_, &k, &v, flags);
        if (rc != 0) {
            return std::unexpected(mdb_strerror(rc));
        }
        return {};
    }

    std::expected<void, std::string> del(MdbTxn& txn, std::string_view key) const noexcept {
        if (!valid_ || !txn.handle()) {
            return std::unexpected(std::string{"Invalid DBI or transaction"});
        }
        MDB_val k = to_val(key);
        int rc = mdb_del(txn.handle(), dbi_, &k, nullptr);
        if (rc != 0 && rc != MDB_NOTFOUND) {
            return std::unexpected(mdb_strerror(rc));
        }
        return {};
    }

    [[nodiscard]] MDB_dbi handle() const noexcept { return dbi_; }
    [[nodiscard]] explicit operator bool() const noexcept { return valid_; }

private:
    MDB_dbi dbi_{0};
    bool valid_{false};
};

class MdbCursor {
public:
    MdbCursor() noexcept : cursor_(nullptr) {}
    ~MdbCursor() noexcept { close(); }

    MdbCursor(const MdbCursor&) = delete;
    MdbCursor& operator=(const MdbCursor&) = delete;

    MdbCursor(MdbCursor&& other) noexcept : cursor_(std::exchange(other.cursor_, nullptr)) {}
    MdbCursor& operator=(MdbCursor&& other) noexcept {
        if (this != &other) {
            close();
            cursor_ = std::exchange(other.cursor_, nullptr);
        }
        return *this;
    }

    static std::expected<MdbCursor, std::string> open(MdbTxn& txn, MdbDbi& dbi) {
        MDB_cursor* cursor = nullptr;
        int rc = mdb_cursor_open(txn.handle(), dbi.handle(), &cursor);
        if (rc != 0) {
            return std::unexpected(mdb_strerror(rc));
        }
        MdbCursor res;
        res.cursor_ = cursor;
        return res;
    }

    void close() noexcept {
        if (cursor_) {
            mdb_cursor_close(cursor_);
            cursor_ = nullptr;
        }
    }

    std::expected<bool, std::string> get(
        MDB_cursor_op op,
        std::string_view& key,
        std::string_view& val) noexcept
    {
        if (!cursor_) return std::unexpected(std::string{"Invalid cursor"});
        MDB_val k = to_val(key);
        MDB_val v = to_val(val);
        int rc = mdb_cursor_get(cursor_, &k, &v, op);
        if (rc == 0) {
            key = from_val(k);
            val = from_val(v);
            return true;
        }
        if (rc == MDB_NOTFOUND) return false;
        return std::unexpected(mdb_strerror(rc));
    }

    std::expected<bool, std::string> first(
        std::string_view& key,
        std::string_view& val) noexcept
    {
        return get(MDB_FIRST, key, val);
    }

    std::expected<bool, std::string> next(
        std::string_view& key,
        std::string_view& val) noexcept
    {
        return get(MDB_NEXT, key, val);
    }

    std::expected<bool, std::string> seek(
        std::string_view& key,
        std::string_view& val) noexcept
    {
        return get(MDB_SET, key, val);
    }

    std::expected<bool, std::string> seek_range(
        std::string_view& key,
        std::string_view& val) noexcept
    {
        return get(MDB_SET_RANGE, key, val);
    }

    [[nodiscard]] MDB_cursor* handle() const noexcept { return cursor_; }
    [[nodiscard]] explicit operator bool() const noexcept { return cursor_ != nullptr; }

private:
    MDB_cursor* cursor_{nullptr};
};

} // namespace sage::vendor::lmdb
