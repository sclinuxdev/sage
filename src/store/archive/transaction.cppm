module;
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <cerrno>
#include <cstring>
export module sage.archive:transaction;

// Durable filesystem transaction directories (issue #9).
//
// Every committed package operation owns one on-disk directory under
// `<target_root>/var/lib/sage/transactions/txn-<id>/` plus one LMDB record
// (owned by sage.db). Payloads are staged here first -- never touching the
// live tree -- and an immutable journal text describes the ordered, idempotent
// publication plan. All operations use anchored openat/mkdirat/O_NOFOLLOW
// syscalls: a symlink inside the target root can never redirect a write
// outside it.
//
// Crash-boundary contract with the LMDB side (sage.db / sage.rebuild):
//   * no record        -> nothing durable points here; the RAII destructor
//                         discards the whole directory.
//   * filesystem_pending -> journal AND staged payloads were fsynced BEFORE
//                         the commit, so recovery can replay publish().
//   * postprocess_pending -> publication done; only triggers/toolchains
//                         remain. The directory survives as the only forensic
//                         evidence until the resume driver retires it.
//
// Recovery = re-running the same publish() (idempotent per entry), so a crash
// at ANY boundary leaves a state the next write command can finish.

import std;
import sage.package;
import sage.util;
import :core;
import :detail;
import :journal;

export namespace sage::archive {


namespace txn {

inline constexpr std::size_t link_buffer_size = 8192;

struct TargetLeaf {
    int parent_fd;   // owned by the publish ParentSyncBatch cache
    std::string leaf;
};

// Publication mutates each unique target parent directory many times but
// must fsync it only once. This cache keeps every touched parent descriptor
// open for the whole plan, pre-flights its st_dev against the transaction
// directory exactly once (EXDEV names the offending path up front instead of
// failing midway), and flushes one fsync per directory after all entries.
class ParentSyncBatch {
public:
    ParentSyncBatch(int root_fd, std::uint64_t device) noexcept
        : root_fd_(root_fd), device_(device) {}

    // Anchored, cached parent-directory descriptor. A missing parent is a
    // corrupted or unordered plan for publications; for removals it merely
    // means the subtree is already gone (`tolerant`, reported as nullopt).
    std::expected<std::optional<int>, std::string> open(
        std::string_view display_path,
        const std::vector<std::string>& components,
        bool tolerant) {
        std::string key;
        for (const auto& component : components) {
            if (!key.empty()) key.push_back('/');
            key += component;
        }
        if (const auto hit = cache_.find(key); hit != cache_.end())
            return std::optional<int>(hit->second.get());
        ++misses_;

        auto dir = detail::open_anchored_dir_strict(root_fd_, components);
        if (!dir) {
            if (tolerant && dir.error().find("No such file") != std::string::npos)
                return std::optional<int>{}; // subtree vanished: nothing to do
            return std::unexpected(dir.error());
        }
        struct stat status {};
        if (::fstat(dir->get(), &status) != 0)
            return std::unexpected(std::string(std::strerror(errno)));
        if (static_cast<std::uint64_t>(status.st_dev) != device_) {
            return std::unexpected(std::format(
                "EXDEV: '{}' resides on filesystem {} but the transaction "
                "directory lives on {}; cross-filesystem publication of a "
                "journal entry is not supported",
                display_path, status.st_dev, device_));
        }
        int fd = dir->get();
        cache_.emplace(std::move(key), std::move(*dir));
        return std::optional<int>(fd);
    }
    // One durability barrier per unique mutated parent. Called after the
    // whole plan applied -- and on the error path too, so an interrupted run
    // still leaves its committed prefix durable.
    std::expected<void, std::string> flush() {
        std::expected<void, std::string> first_failure;
        for (auto& [key, fd] : cache_) {
            if (auto synced = detail::fsync_fd(fd.get()); !synced && !first_failure)
                first_failure = synced;
        }
        return first_failure;
    }

    [[nodiscard]] std::size_t size() const noexcept { return cache_.size(); }
    [[nodiscard]] std::size_t misses() const noexcept { return misses_; }

private:
    int root_fd_;
    std::uint64_t device_;
    std::unordered_map<std::string, detail::UniqueFd> cache_;
    std::size_t misses_{0};
};

// Resolve the parent directory of a plan target below the live root without
// creating anything: the journal's EnsureDir entries guarantee ancestors.
std::expected<TargetLeaf, std::string> resolve_target(
    ParentSyncBatch& parents, std::string_view target_rel) {
    auto normalized = normalize_data_path(target_rel);
    if (!normalized) return std::unexpected(normalized.error());
    auto components = detail::rel_components(*normalized);
    if (components.empty())
        return std::unexpected("Target path must name a leaf: " + *normalized);
    TargetLeaf out;
    out.leaf = components.back();
    components.pop_back();
    auto parent = parents.open(target_rel, components, false);
    if (!parent) return std::unexpected(parent.error());
    if (!*parent)
        return std::unexpected("Parent directory of '" + *normalized
            + "' is missing from the plan");
    out.parent_fd = **parent;
    return out;
}

// Removals tolerate a vanished parent subtree: the entry is then complete.
std::expected<std::optional<TargetLeaf>, std::string> resolve_remove_target(
    ParentSyncBatch& parents, std::string_view target_rel) {
    auto normalized = normalize_data_path(target_rel);
    if (!normalized) return std::unexpected(normalized.error());
    auto components = detail::rel_components(*normalized);
    TargetLeaf out;
    out.leaf = components.back();
    components.pop_back();
    auto parent = parents.open(target_rel, components, true);
    if (!parent) return std::unexpected(parent.error());
    if (!*parent) return std::optional<TargetLeaf>{};
    out.parent_fd = **parent;
    return std::optional<TargetLeaf>(std::move(out));
}

// Unique temporary leaf name inside the target parent directory.
std::expected<std::string, std::string> make_temp_name(std::string_view purpose) {
    auto suffix = detail::random_hex(8);
    if (!suffix) return std::unexpected(suffix.error());
    return std::string(temp_file_prefix) + std::string(purpose) + "-" + *suffix;
}

// Removes the temporary name unless dismissed after a successful rename.
struct TempGuard {
    int parent_fd;
    const std::string* name;
    void dismiss() noexcept { parent_fd = -1; }
    ~TempGuard() noexcept {
        if (parent_fd >= 0) (void)::unlinkat(parent_fd, name->c_str(), 0);
    }
};

std::expected<void, std::string> publish_put_file(
    int dir_fd, const PlanEntry& entry, const TargetLeaf& target) {
    // Hot path: one atomic flip, zero probes. The staging protocol
    // guarantees the payload already carries its final mode and was fsynced
    // to disk before the LMDB commit (the sync_staging barrier), so
    // publishing is a zero-copy renameat within one filesystem: no
    // temporary file, no second data copy, no per-file fsync. Durability of
    // the new name is restored by one batched parent-directory flush after
    // the whole plan.
    if (::renameat(dir_fd, entry.staged.c_str(),
            target.parent_fd, target.leaf.c_str()) == 0) {
        return {};
    }
    if (errno != ENOENT) { // parent is guaranteed resolved; anything else is real
        return std::unexpected(std::format(
            "Cannot publish '{}': {}", entry.target, std::strerror(errno)));
    }

    // Idempotent completion: an earlier attempt consumed the staged payload
    // via its own rename -- the entry is complete exactly when a regular
    // file now occupies the target.
    struct stat target_status {};
    if (::fstatat(target.parent_fd, target.leaf.c_str(),
            &target_status, AT_SYMLINK_NOFOLLOW) == 0
        && S_ISREG(target_status.st_mode)) {
        return {};
    }
    return std::unexpected(std::format(
        "Staged payload '{}' for target '{}' is missing",
        entry.staged, entry.target));
}

std::expected<void, std::string> publish_put_symlink(
    int dir_fd, const PlanEntry& entry, const TargetLeaf& target) {
    char desired_buffer[link_buffer_size];
    const ssize_t desired_length = ::readlinkat(
        dir_fd, entry.staged.c_str(), desired_buffer, sizeof(desired_buffer));
    if (desired_length <= 0) {
        return std::unexpected(std::format(
            "Staged symlink '{}' unreadable: {}",
            entry.staged,
            desired_length < 0 ? std::strerror(errno) : "empty"));
    }
    const std::string desired(desired_buffer, static_cast<std::size_t>(desired_length));

    // Idempotent completion: read back the live link and compare.
    char live_buffer[link_buffer_size];
    const ssize_t live_length = ::readlinkat(
        target.parent_fd, target.leaf.c_str(), live_buffer, sizeof(live_buffer));
    if (live_length > 0
        && desired == std::string_view(live_buffer, static_cast<std::size_t>(live_length))) {
        return {};
    }

    // Never unlink-then-symlink in the live tree: stage a temporary symlink
    // in the target parent and flip it atomically with renameat.
    for (int attempt = 0; attempt < 4; ++attempt) {
        auto temp_name = make_temp_name("link");
        if (!temp_name) return std::unexpected(temp_name.error());
        if (::symlinkat(desired.c_str(), target.parent_fd, temp_name->c_str()) != 0) {
            if (errno == EEXIST) continue;
            return std::unexpected(std::format(
                "Cannot stage temporary symlink for '{}': {}",
                entry.target, std::strerror(errno)));
        }
        TempGuard guard{target.parent_fd, &*temp_name};
        if (::renameat(target.parent_fd, temp_name->c_str(),
                target.parent_fd, target.leaf.c_str()) != 0) {
            return std::unexpected(std::format(
                "Cannot publish symlink '{}': {}", entry.target, std::strerror(errno)));
        }
        guard.dismiss();
        return {};
    }
    return std::unexpected(std::format(
        "Cannot allocate temporary symlink name for '{}'", entry.target));
}

std::expected<void, std::string> publish_ensure_dir(
    ParentSyncBatch& parents, int root_fd, const PlanEntry& entry) {
    // A directory has no content: the only thing to make durable is its
    // entry inside its parent, so creation is a plain mkdirat walk with NO
    // per-level fsync and durability is delegated to the single batched
    // parent flush (this parent is registered below; ancestors are covered
    // by their own EnsureDir entries). EEXIST on a directory is idempotent
    // success; on anything else it surfaces as an ENOTDIR open failure.
    auto normalized = normalize_data_path(entry.target);
    if (!normalized) return std::unexpected(normalized.error());
    auto components = detail::rel_components(*normalized);
    if (components.empty())
        return std::unexpected("Target path must name a leaf: " + *normalized);

    int walk_flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW;
#ifdef O_CLOEXEC
    walk_flags |= O_CLOEXEC;
#endif
    detail::UniqueFd current(::fcntl(root_fd, F_DUPFD_CLOEXEC, 0));
    if (current.get() < 0)
        return std::unexpected("Cannot duplicate target root descriptor");
    for (const auto& component : components) {
        int next = ::openat(current.get(), component.c_str(), walk_flags);
        if (next < 0 && errno == ENOENT) {
            if (::mkdirat(current.get(), component.c_str(), 0755) != 0
                && errno != EEXIST) {
                return std::unexpected(std::format(
                    "Cannot ensure directory '{}': {}", entry.target,
                    std::strerror(errno)));
            }
            next = ::openat(current.get(), component.c_str(), walk_flags);
        }
        if (next < 0) {
            return std::unexpected(std::format(
                "Cannot ensure directory '{}': {}", entry.target,
                std::strerror(errno)));
        }
        current = detail::UniqueFd(next);
    }

    // Cross-filesystem preflight + one flush registration per directory.
    auto parent_components = components;
    parent_components.pop_back();
    auto parent = parents.open(entry.target, parent_components, false);
    if (!parent) return std::unexpected(parent.error());
    if (!*parent)
        return std::unexpected("Parent directory of '" + *normalized
            + "' vanished during publication");
    return {};
}

// Removals: a vanished parent subtree means the entry is already complete.
std::expected<void, std::string> publish_remove_file(
    ParentSyncBatch& parents, const PlanEntry& entry) {
    auto target = resolve_remove_target(parents, entry.target);
    if (!target) return std::unexpected(target.error());
    if (!*target) return {}; // parent subtree vanished: already done
    const auto& t = **target;

    if (::unlinkat(t.parent_fd, t.leaf.c_str(), 0) != 0) {
        if (errno == ENOENT) return {}; // idempotent success
        return std::unexpected(std::format(
            "Cannot remove '{}': {}", entry.target, std::strerror(errno)));
    }
    return {};
}

std::expected<void, std::string> publish_remove_dir(
    ParentSyncBatch& parents, const PlanEntry& entry) {
    auto target = resolve_remove_target(parents, entry.target);
    if (!target) return std::unexpected(target.error());
    if (!*target) return {};
    const auto& t = **target;

    if (::unlinkat(t.parent_fd, t.leaf.c_str(), AT_REMOVEDIR) != 0) {
        if (errno == ENOENT) return {};                 // already done
        if (errno == ENOTEMPTY || errno == EEXIST) {
            // Foreign content lives here: keep the directory (and its
            // contents); ownership withdrawal is the caller's bookkeeping.
            util::log_info("  ~ keeping non-empty directory '{}'", entry.target);
            return {};
        }
        return std::unexpected(std::format(
            "Cannot remove directory '{}': {}", entry.target, std::strerror(errno)));
    }
    return {};
}
} // namespace txn

// --- FilesystemTransaction ---------------------------------------------------

class FilesystemTransaction {
public:
    static std::expected<FilesystemTransaction, std::string> create(
        const std::filesystem::path& target_root);

    // Recovery path: adopt an existing transaction directory; validates that
    // it exists. Attached instances preserve evidence when destroyed --
    // retirement is the resume driver's explicit decision.
    static std::expected<FilesystemTransaction, std::string> attach(
        const std::filesystem::path& target_root, std::string_view relative_dir);

    FilesystemTransaction(const FilesystemTransaction&) = delete;
    FilesystemTransaction& operator=(const FilesystemTransaction&) = delete;

    FilesystemTransaction(FilesystemTransaction&& other) noexcept
        : target_root_(std::move(other.target_root_)),
          id_(std::move(other.id_)),
          relative_dir_(std::move(other.relative_dir_)),
          dir_fd_(std::move(other.dir_fd_)),
          device_(other.device_),
          plan_(std::move(other.plan_)),
          journaled_(std::exchange(other.journaled_, true)),
          discarded_(std::exchange(other.discarded_, true)) {}

    // Pre-commit RAII: before persist_journal() nothing durable references
    // this directory, so destruction discards it. Afterwards the LMDB record
    // points at the evidence and destruction MUST NOT remove it; explicit
    // discard()/retire stays with the caller.
    ~FilesystemTransaction() noexcept {
        if (!journaled_ && !discarded_) (void)discard();
    }

    [[nodiscard]] const std::string& id() const noexcept { return id_; }
    [[nodiscard]] const std::string& relative_dir() const noexcept {
        return relative_dir_;
    }
    [[nodiscard]] const std::vector<PlanEntry>& journal_entries() const noexcept {
        return plan_;
    }

    // --- prepare: staging only, the live tree is never touched --------------

    // Writable fd (O_CREAT|O_EXCL, fixed private staging mode; the final mode
    // lives in the journal). The caller owns and closes the descriptor.
    std::expected<int, std::string> open_staged_file(
        std::string_view stage_rel, std::uint32_t staging_mode = 0600) {
        auto normalized = normalize_data_path(stage_rel);
        if (!normalized) return std::unexpected(normalized.error());
        auto components = detail::rel_components(*normalized);
        const std::string leaf = components.back();
        components.pop_back();
        auto stage_parent = detail::create_anchored_dir_chain(dir_fd_.get(),
            parent_path_text(*normalized), 0700);
        if (!stage_parent) return std::unexpected(stage_parent.error());

        int flags = O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW;
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
        detail::UniqueFd file(::openat(stage_parent->get(), leaf.c_str(),
            flags, staging_mode & 07777));
        if (file.get() < 0) {
            return std::unexpected(std::format(
                "Cannot stage '{}': {}", stage_rel, std::strerror(errno)));
        }
        // open(2) always applies the process umask. The staging inode is the
        // inode publication later renames into place, so force the promised
        // final mode before the durability barrier rather than trusting umask.
        if (::fchmod(file.get(), staging_mode & 07777) != 0) {
            const std::string reason = std::strerror(errno);
            (void)::unlinkat(stage_parent->get(), leaf.c_str(), 0);
            return std::unexpected(std::format(
                "Cannot set staged mode for '{}': {}", stage_rel, reason));
        }
        return file.release();
    }

    std::expected<void, std::string> stage_symlink(
        std::string_view stage_rel, std::string_view link_target) {
        auto normalized = normalize_data_path(stage_rel);
        if (!normalized) return std::unexpected(normalized.error());
        auto components = detail::rel_components(*normalized);
        const std::string leaf = components.back();
        components.pop_back();
        auto parent = detail::create_anchored_dir_chain(dir_fd_.get(),
            parent_path_text(*normalized), 0700);
        if (!parent) return std::unexpected(parent.error());
        if (::symlinkat(std::string(link_target).c_str(),
                parent->get(), leaf.c_str()) != 0) {
            return std::unexpected(std::format(
                "Cannot stage symlink '{}': {}", stage_rel, std::strerror(errno)));
        }
        return detail::fsync_fd(parent->get());
    }

    std::expected<void, std::string> ensure_staged_dir(std::string_view stage_rel) {
        auto normalized = normalize_data_path(stage_rel);
        if (!normalized) return std::unexpected(normalized.error());
        auto dir = detail::create_anchored_dir_chain(
            dir_fd_.get(), *normalized, 0700);
        if (!dir) return std::unexpected(dir.error());
        return {};
    }

    // --- plan accumulation (rendered into the journal text by the caller) --

    void plan_put_file(std::string_view target_rel, std::string_view stage_rel,
        std::uint32_t mode) {
        plan_.push_back({PlanEntry::Kind::PutFile, mode,
            std::string(stage_rel), std::string(target_rel)});
    }
    void plan_put_symlink(std::string_view target_rel, std::string_view stage_rel) {
        plan_.push_back({PlanEntry::Kind::PutSymlink, 0777,
            std::string(stage_rel), std::string(target_rel)});
    }
    void plan_ensure_dir(std::string_view target_rel) {
        plan_.push_back({PlanEntry::Kind::EnsureDir, 0755, {}, std::string(target_rel)});
    }
    void plan_remove_file(std::string_view target_rel) {
        plan_.push_back({PlanEntry::Kind::RemoveFile, 0644, {}, std::string(target_rel)});
    }
    void plan_remove_dir(std::string_view target_rel) {
        plan_.push_back({PlanEntry::Kind::RemoveDir, 0755, {}, std::string(target_rel)});
    }

    // Durability barrier before the LMDB commit: one syncfs() over the
    // transaction filesystem makes every staged byte and directory entry
    // durable at once -- data persistence is still guaranteed to precede
    // any name a later publish() creates. After this returns, a crash may
    // lose the LMDB commit but can never leave a journal or payload
    // half-written on disk. (53k per-file fsyncs cost minutes; one barrier
    // costs the same writeback either way.)
    std::expected<void, std::string> sync_staging() {
        const auto t0 = std::chrono::steady_clock::now();
        if (::syncfs(dir_fd_.get()) != 0) {
            return std::unexpected(
                "Cannot flush staging to storage: " + std::string(std::strerror(errno)));
        }
        util::log_info("[timing:txn] syncfs barrier: {:.3f}s",
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        return {};
    }

    // Write the journal into the transaction directory, fsync file and
    // directory, and flip the instance into evidence-preserving mode. Returns
    // the sha256 hex digest for the LMDB record.
    //
    // Ordering matters: file content -> journal inode -> directory entry.
    // Only after all three are durable is it safe for the caller to commit
    // an LMDB record that references this digest; a crash before that point
    // leaves "no record" semantics, where RAII discard cleans up freely.
    std::expected<std::string, std::string> persist_journal(std::string_view text) {
        int flags = O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW;
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
        detail::UniqueFd file(::openat(dir_fd_.get(),
            journal_file_name.data(), flags, 0600));
        if (file.get() < 0) {
            return std::unexpected(std::format(
                "Cannot open journal: {}", std::strerror(errno)));
        }
        if (auto written = detail::write_fd_all(file.get(), text); !written)
            return std::unexpected(written.error());
        if (auto synced = detail::fsync_fd(file.get()); !synced)
            return std::unexpected(synced.error());
        file = detail::UniqueFd{};
        if (auto synced = detail::fsync_fd(dir_fd_.get()); !synced)
            return std::unexpected(synced.error());

        util::Sha256 hasher;
        hasher.update(text.data(), text.size());
        journaled_ = true;
        return hasher.finalize();
    }

    // Read the journal back (recovery); static, no instance required.
    static std::expected<std::string, std::string> read_journal(
        const std::filesystem::path& target_root, std::string_view relative_dir) {
        auto normalized = normalize_data_path(relative_dir);
        if (!normalized)
            return std::unexpected(normalized.error());
        detail::UniqueFd root(detail::open_root_fd(target_root));
        if (root.get() < 0) {
            return std::unexpected("Cannot securely open target root: "
                + std::string(std::strerror(errno)));
        }
        auto dir = detail::open_anchored_dir_strict(root.get(),
            detail::rel_components(*normalized));
        if (!dir) return std::unexpected(dir.error());

        int flags = O_RDONLY | O_NOFOLLOW;
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
        detail::UniqueFd file(::openat(dir->get(), journal_file_name.data(), flags));
        if (file.get() < 0) {
            return std::unexpected(std::format(
                "Cannot open journal in '{}': {}", relative_dir, std::strerror(errno)));
        }
        struct stat status {};
        if (::fstat(file.get(), &status) != 0 || status.st_size < 0)
            return std::unexpected("Cannot stat journal");
        std::string text;
        text.resize(static_cast<std::size_t>(status.st_size));
        std::size_t done = 0;
        while (done < text.size()) {
            ssize_t got = ::read(file.get(), text.data() + done, text.size() - done);
            if (got < 0 && errno == EINTR) continue;
            if (got <= 0) {
                return std::unexpected(
                    "Short read on journal: " + std::string(std::strerror(errno)));
            }
            done += static_cast<std::size_t>(got);
        }
        return text;
    }

    // Idempotent publication of [plan] into the live tree. Every PutFile is
    // a pure renameat flip; each unique target parent descriptor stays open
    // in a ParentSyncBatch that pre-flights its st_dev against the
    // transaction directory (EXDEV names the path up front) and receives
    // exactly ONE fsync after the whole plan applied. On any failing entry,
    // publication stops immediately: earlier entries stay published (each
    // was individually atomic), their parents are still flushed on the way
    // out, and the failure plus evidence remain untouched, so a retry --
    // from the normal path or startup recovery -- replays idempotently.
    std::expected<void, std::string> publish(std::string_view journal_text) {
        const auto t_total = std::chrono::steady_clock::now();
        auto parsed = parse_journal(journal_text);
        if (!parsed) return std::unexpected(parsed.error());
        const auto t_parsed = std::chrono::steady_clock::now();

        detail::UniqueFd root(detail::open_root_fd(target_root_));
        if (root.get() < 0) {
            return std::unexpected("Cannot securely open target root: "
                + std::string(std::strerror(errno)));
        }
        txn::ParentSyncBatch parents(root.get(), device_);
        std::expected<void, std::string> failure;
        for (const auto& entry : parsed->plan) {
            std::expected<void, std::string> outcome = std::unexpected(
                std::string("Unhandled plan entry"));
            if (entry.kind == PlanEntry::Kind::PutFile) {
                auto target = txn::resolve_target(parents, entry.target);
                if (!target) {
                    outcome = std::unexpected(target.error());
                } else {
                    outcome = txn::publish_put_file(dir_fd_.get(), entry, *target);
                }
            } else if (entry.kind == PlanEntry::Kind::PutSymlink) {
                auto target = txn::resolve_target(parents, entry.target);
                if (!target) {
                    outcome = std::unexpected(target.error());
                } else {
                    outcome = txn::publish_put_symlink(dir_fd_.get(), entry, *target);
                }
            } else if (entry.kind == PlanEntry::Kind::EnsureDir) {
                outcome = txn::publish_ensure_dir(parents, root.get(), entry);
            } else if (entry.kind == PlanEntry::Kind::RemoveFile) {
                outcome = txn::publish_remove_file(parents, entry);
            } else { // RemoveDir
                outcome = txn::publish_remove_dir(parents, entry);
            }
            if (!outcome) {
                failure = outcome;
                break; // stop at first failure; evidence kept
            }
        }
        const auto t_loop = std::chrono::steady_clock::now();
        // Barrier for everything published so far -- also on the error path,
        // so an interrupted run never loses its committed prefix.
        auto flushed = parents.flush();
        const auto t_done = std::chrono::steady_clock::now();
        util::log_info(
            "[timing:txn] publish: {:.3f}s (parse {:.3f}s, loop {:.3f}s, {} entries, "
            "{} parents, {} cache misses, flush {:.3f}s)",
            std::chrono::duration<double>(t_done - t_parsed).count(),
            std::chrono::duration<double>(t_parsed - t_total).count(),
            std::chrono::duration<double>(t_loop - t_parsed).count(),
            parsed->plan.size(),
            parents.size(),
            parents.misses(),
            std::chrono::duration<double>(t_done - t_loop).count());
        if (failure) return failure;
        return flushed;
    }

    // Anchored rm -rf of the transaction directory (pre-commit failures,
    // orphan GC, abandon). Idempotent.
    std::expected<void, std::string> discard() {
        if (discarded_) return {};
        auto normalized = normalize_data_path(relative_dir_);
        if (!normalized) return std::unexpected(normalized.error());
        auto components = detail::rel_components(*normalized);
        const std::string leaf = components.back();
        components.pop_back();

        detail::UniqueFd root(detail::open_root_fd(target_root_));
        if (root.get() < 0) {
            return std::unexpected("Cannot securely open target root: "
                + std::string(std::strerror(errno)));
        }
        auto parent = detail::open_anchored_dir_strict(root.get(), components);
        if (!parent) {
            if (parent.error().find("No such file") != std::string::npos) {
                discarded_ = true;
                return {};
            }
            return std::unexpected(parent.error());
        }
        if (auto removed = detail::remove_tree_anchored(parent->get(), leaf);
            !removed) {
            return removed;
        }
        if (auto synced = detail::fsync_fd(parent->get()); !synced)
            return std::unexpected(synced.error());
        discarded_ = true;
        return {};
    }

private:
    FilesystemTransaction(std::filesystem::path target_root, std::string id,
        std::string relative_dir, detail::UniqueFd dir_fd, std::uint64_t device,
        bool journaled)
        : target_root_(std::move(target_root)),
          id_(std::move(id)),
          relative_dir_(std::move(relative_dir)),
          dir_fd_(std::move(dir_fd)),
          device_(device),
          journaled_(journaled) {}

    static std::string parent_path_text(const std::string& rel) {
        auto slash = rel.rfind('/');
        if (slash == std::string::npos) return {};
        return rel.substr(0, slash);
    }

    std::filesystem::path target_root_;
    std::string id_;
    std::string relative_dir_;   // "var/lib/sage/transactions/txn-<id>"
    detail::UniqueFd dir_fd_;
    std::uint64_t device_{0};
    std::vector<PlanEntry> plan_;
    bool journaled_{false};
    bool discarded_{false};
};

inline std::expected<FilesystemTransaction, std::string>
FilesystemTransaction::create(const std::filesystem::path& target_root) {
    detail::UniqueFd root(detail::open_root_fd(target_root));
    if (root.get() < 0) {
        return std::unexpected("Cannot securely open target root: "
            + std::string(std::strerror(errno)));
    }

    auto txns = detail::create_anchored_dir_chain(
        root.get(), "var/lib/sage/transactions", 0755);
    if (!txns) return std::unexpected(txns.error());

    auto id_hex = detail::random_hex(16);
    if (!id_hex) return std::unexpected(id_hex.error());
    const std::string name = "txn-" + *id_hex;
    if (::mkdirat(txns->get(), name.c_str(), 0700) != 0) {
        return std::unexpected(std::format(
            "Cannot create transaction directory '{}': {}",
            name, std::strerror(errno)));
    }
    if (auto synced = detail::fsync_fd(txns->get()); !synced)
        return std::unexpected(synced.error());

    int dir_flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW;
#ifdef O_CLOEXEC
    dir_flags |= O_CLOEXEC;
#endif
    detail::UniqueFd dir(::openat(txns->get(), name.c_str(), dir_flags));
    if (dir.get() < 0) {
        return std::unexpected("Cannot reopen transaction directory: "
            + std::string(std::strerror(errno)));
    }

    // Owns the directory from here on: any failure below destroys the
    // instance, whose RAII destructor removes the fresh directory again.
    FilesystemTransaction txn(target_root, *id_hex,
        "var/lib/sage/transactions/" + name, std::move(dir), 0, false);

    struct stat dir_status {}, root_status {};
    if (::fstat(txn.dir_fd_.get(), &dir_status) != 0
        || ::fstat(root.get(), &root_status) != 0) {
        return std::unexpected("Cannot stat transaction directories: "
            + std::string(std::strerror(errno)));
    }
    if (static_cast<std::uint64_t>(dir_status.st_dev)
        != static_cast<std::uint64_t>(root_status.st_dev)) {
        return std::unexpected(std::format(
            "Transaction directory spans filesystems (device {} != {}); "
            "staging must share the target root filesystem for atomic flips",
            dir_status.st_dev, root_status.st_dev));
    }
    txn.device_ = dir_status.st_dev;
    return txn;
}

inline std::expected<FilesystemTransaction, std::string>
FilesystemTransaction::attach(
    const std::filesystem::path& target_root, std::string_view relative_dir) {
    auto normalized = normalize_data_path(relative_dir);
    if (!normalized) return std::unexpected(normalized.error());
    auto components = detail::rel_components(*normalized);
    const std::string leaf = components.back();

    detail::UniqueFd root(detail::open_root_fd(target_root));
    if (root.get() < 0) {
        return std::unexpected("Cannot securely open target root: "
            + std::string(std::strerror(errno)));
    }
    auto dir = detail::open_anchored_dir_strict(root.get(), components);
    if (!dir) {
        return std::unexpected(
            "No such transaction directory '" + *normalized + "': " + dir.error());
    }

    struct stat dir_status {}, root_status {};
    if (::fstat(dir->get(), &dir_status) != 0
        || ::fstat(root.get(), &root_status) != 0) {
        return std::unexpected("Cannot stat transaction directory: "
            + std::string(std::strerror(errno)));
    }
    if (static_cast<std::uint64_t>(dir_status.st_dev)
        != static_cast<std::uint64_t>(root_status.st_dev)) {
        return std::unexpected(std::format(
            "Transaction directory '{}' spans filesystems (device {} != {})",
            *normalized, dir_status.st_dev, root_status.st_dev));
    }

    std::string id = leaf.starts_with("txn-") ? leaf.substr(4) : leaf;
    // Attached transactions carry recovery evidence referenced by LMDB: the
    // destructor preserves them; retirement is explicit.
    return FilesystemTransaction(target_root, std::move(id), *normalized,
        std::move(*dir), dir_status.st_dev, true);
}

// List transaction directory names under var/lib/sage/transactions (for
// orphan GC; whether one is orphaned is decided by the LMDB side). Errors
// yield an empty list.
inline std::vector<std::string> list_transaction_dirs(
    const std::filesystem::path& target_root) {
    std::vector<std::string> out;
    detail::UniqueFd root(detail::open_root_fd(target_root));
    if (root.get() < 0) return out;
    auto dir = detail::open_anchored_dir_strict(root.get(),
        detail::rel_components("var/lib/sage/transactions"));
    if (!dir) return out;
    auto names = detail::list_dir_names(dir->get());
    if (!names) return out;
    for (const auto& name : *names) {
        struct stat status {};
        if (::fstatat(dir->get(), name.c_str(), &status, AT_SYMLINK_NOFOLLOW) == 0
            && S_ISDIR(status.st_mode)) {
            out.push_back(name);
        }
    }
    std::ranges::sort(out);
    return out;
}

} // namespace sage::archive
