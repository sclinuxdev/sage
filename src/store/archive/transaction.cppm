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

export namespace sage::archive {

using std::size_t;
using std::uint32_t;
using std::uint64_t;

inline constexpr std::string_view journal_magic_line = "sage-journal 1";
inline constexpr std::string_view journal_file_name = "journal";

struct PlanEntry {
    enum class Kind { PutFile, PutSymlink, EnsureDir, RemoveFile, RemoveDir };
    Kind kind;
    uint32_t mode{0644};          // PutFile final permission (4-digit octal text)
    std::string staged;           // PutFile/PutSymlink: relative to the txn dir
    std::string target;           // relative to the target root
};

namespace txn {

inline constexpr size_t link_buffer_size = 8192;

std::expected<uint32_t, std::string> parse_mode(std::string_view token) {
    if (token.empty() || token.size() > 6)
        return std::unexpected("Invalid mode '" + std::string(token) + "'");
    uint32_t mode = 0;
    for (const char c : token) {
        if (c < '0' || c > '7')
            return std::unexpected(
                "Invalid mode '" + std::string(token) + "' (expected octal)");
        mode = (mode << 3) | static_cast<uint32_t>(c - '0');
    }
    return mode;
}

std::vector<std::string> split_tokens(std::string_view line) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start < line.size()) {
        while (start < line.size() && line[start] == ' ') ++start;
        if (start >= line.size()) break;
        auto end = line.find(' ', start);
        if (end == std::string_view::npos) end = line.size();
        out.emplace_back(line.substr(start, end - start));
        start = end;
    }
    return out;
}

std::string join_tokens(const std::vector<std::string>& tokens, size_t from) {
    std::string out;
    for (size_t i = from; i < tokens.size(); ++i) {
        if (i > from) out.push_back(' ');
        out += tokens[i];
    }
    return out;
}

struct TargetLeaf {
    detail::UniqueFd parent;
    std::string leaf;
};

// Resolve the parent directory of a plan target below the live root without
// creating anything: the journal's EnsureDir entries guarantee ancestors, so
// a missing parent means a corrupted or unordered plan.
std::expected<TargetLeaf, std::string> open_target(
    int root_fd, std::string_view target_rel) {
    auto normalized = normalize_data_path(target_rel);
    if (!normalized) return std::unexpected(normalized.error());
    auto components = detail::rel_components(*normalized);
    if (components.empty())
        return std::unexpected("Target path must name a leaf: " + *normalized);
    TargetLeaf out;
    out.leaf = components.back();
    components.pop_back();
    auto parent = detail::open_anchored_dir_strict(root_fd, components);
    if (!parent) return std::unexpected(parent.error());
    out.parent = std::move(*parent);
    return out;
}

// Cross-filesystem preflight: publishing through renameat(2) only works
// within one filesystem, so every entry is rejected up front (naming the
// path) instead of failing midway with an unhelpful EXDEV.
std::expected<void, std::string> check_same_device(
    int parent_fd, uint64_t transaction_device, std::string_view target) {
    struct stat status {};
    if (::fstat(parent_fd, &status) != 0)
        return std::unexpected(std::string(std::strerror(errno)));
    if (static_cast<uint64_t>(status.st_dev) != transaction_device) {
        return std::unexpected(std::format(
            "EXDEV: '{}' resides on filesystem {} but the transaction "
            "directory lives on {}; cross-filesystem publication of a "
            "journal entry is not supported",
            target, status.st_dev, transaction_device));
    }
    return {};
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
    int dir_fd, uint64_t device, const PlanEntry& entry,
    const TargetLeaf& target) {
    if (auto check = check_same_device(target.parent.get(), device, entry.target);
        !check) return std::unexpected(check.error());

    struct stat target_status {};
    const bool target_exists =
        ::fstatat(target.parent.get(), target.leaf.c_str(),
            &target_status, AT_SYMLINK_NOFOLLOW) == 0;
    struct stat staged_status {};
    const bool staged_exists =
        ::fstatat(dir_fd, entry.staged.c_str(), &staged_status, AT_SYMLINK_NOFOLLOW) == 0;

    // Idempotent completion: an earlier attempt consumed the staged payload
    // via a direct rename (staged missing, regular target present). While
    // the staged file exists it stays the source of truth: republish
    // overwrites, so same-size-different-content downgrades re-publish.
    if (!staged_exists) {
        if (target_exists && S_ISREG(target_status.st_mode)) return {};
        return std::unexpected(std::format(
            "Staged payload '{}' for target '{}' is missing",
            entry.staged, entry.target));
    }

    int in_flags = O_RDONLY | O_NOFOLLOW;
#ifdef O_CLOEXEC
    in_flags |= O_CLOEXEC;
#endif
    detail::UniqueFd source(::openat(dir_fd, entry.staged.c_str(), in_flags));
    if (source.get() < 0) {
        return std::unexpected(std::format(
            "Cannot open staged file '{}': {}",
            entry.staged, std::strerror(errno)));
    }

    auto temp_name = make_temp_name("publish");
    if (!temp_name) return std::unexpected(temp_name.error());
    int out_flags = O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW;
#ifdef O_CLOEXEC
    out_flags |= O_CLOEXEC;
#endif
    detail::UniqueFd destination(
        ::openat(target.parent.get(), temp_name->c_str(), out_flags, 0600));
    if (destination.get() < 0) {
        return std::unexpected(std::format(
            "Cannot create temporary file for '{}': {}",
            entry.target, std::strerror(errno)));
    }
    TempGuard guard{target.parent.get(), &*temp_name};

    // Crash-boundary invariant: content reaches the temporary file and is
    // fsynced BEFORE renameat makes the name visible, so the live tree can
    // never expose a name whose bytes are still page-cache-only. A power
    // loss between fsync and fchmod loses only mode bits, which replay
    // repairs from the journal's recorded mode. Bounded streaming copy
    // (copy_file_range preferred): never materialize a package file in RAM.
    if (auto copied = detail::copy_fd_exact(source.get(), destination.get(),
            static_cast<uint64_t>(staged_status.st_size));
        !copied) {
        return std::unexpected(copied.error());
    }
    if (auto synced = detail::fsync_fd(destination.get()); !synced)
        return std::unexpected(synced.error());
    if (::fchmod(destination.get(), entry.mode & 07777) != 0) {
        return std::unexpected(std::format(
            "Cannot set final mode on '{}': {}", entry.target, std::strerror(errno)));
    }
    destination = detail::UniqueFd{};

    if (::renameat(target.parent.get(), temp_name->c_str(),
            target.parent.get(), target.leaf.c_str()) != 0) {
        return std::unexpected(std::format(
            "Cannot publish '{}': {}", entry.target, std::strerror(errno)));
    }
    guard.dismiss();
    if (auto synced = detail::fsync_fd(target.parent.get()); !synced)
        return std::unexpected(synced.error());
    return {};
}

std::expected<void, std::string> publish_put_symlink(
    int dir_fd, uint64_t device, const PlanEntry& entry,
    const TargetLeaf& target) {
    if (auto check = check_same_device(target.parent.get(), device, entry.target);
        !check) return std::unexpected(check.error());

    char desired_buffer[link_buffer_size];
    const ssize_t desired_length = ::readlinkat(
        dir_fd, entry.staged.c_str(), desired_buffer, sizeof(desired_buffer));
    if (desired_length <= 0) {
        return std::unexpected(std::format(
            "Staged symlink '{}' unreadable: {}",
            entry.staged,
            desired_length < 0 ? std::strerror(errno) : "empty"));
    }
    const std::string desired(desired_buffer, static_cast<size_t>(desired_length));

    // Idempotent completion: read back the live link and compare.
    char live_buffer[link_buffer_size];
    const ssize_t live_length = ::readlinkat(
        target.parent.get(), target.leaf.c_str(), live_buffer, sizeof(live_buffer));
    if (live_length > 0
        && desired == std::string_view(live_buffer, static_cast<size_t>(live_length))) {
        return {};
    }

    // Never unlink-then-symlink in the live tree: stage a temporary symlink
    // in the target parent and flip it atomically with renameat.
    for (int attempt = 0; attempt < 4; ++attempt) {
        auto temp_name = make_temp_name("link");
        if (!temp_name) return std::unexpected(temp_name.error());
        if (::symlinkat(desired.c_str(), target.parent.get(), temp_name->c_str()) != 0) {
            if (errno == EEXIST) continue;
            return std::unexpected(std::format(
                "Cannot stage temporary symlink for '{}': {}",
                entry.target, std::strerror(errno)));
        }
        TempGuard guard{target.parent.get(), &*temp_name};
        if (::renameat(target.parent.get(), temp_name->c_str(),
                target.parent.get(), target.leaf.c_str()) != 0) {
            return std::unexpected(std::format(
                "Cannot publish symlink '{}': {}", entry.target, std::strerror(errno)));
        }
        guard.dismiss();
        if (auto synced = detail::fsync_fd(target.parent.get()); !synced)
            return std::unexpected(synced.error());
        return {};
    }
    return std::unexpected(std::format(
        "Cannot allocate temporary symlink name for '{}'", entry.target));
}

std::expected<void, std::string> publish_ensure_dir(
    int root_fd, uint64_t device, const PlanEntry& entry) {
    // The journal only names directories the package owns; implied ancestors
    // materialize through the anchored chain (each new level fsynced to its
    // parent). EEXIST on a directory is idempotent success; on anything else
    // it surfaces as an ENOTDIR anchor failure.
    auto normalized = normalize_data_path(entry.target);
    auto dir = detail::create_anchored_dir_chain(root_fd, *normalized, 0755);
    if (!dir) return std::unexpected(std::format(
        "Cannot ensure directory '{}': {}", entry.target, dir.error()));
    struct stat status {};
    if (::fstat(dir->get(), &status) != 0)
        return std::unexpected(std::string(std::strerror(errno)));
    if (static_cast<uint64_t>(status.st_dev) != device) {
        return std::unexpected(std::format(
            "EXDEV: '{}' resides on filesystem {} but the transaction "
            "directory lives on {}; cross-filesystem publication is not "
            "supported", entry.target, status.st_dev, device));
    }
    return {};
}

// Missing parent = subtree already gone: removal trivially complete.
std::expected<std::optional<TargetLeaf>, std::string> open_remove_target(
    int root_fd, const PlanEntry& entry) {
    auto normalized = normalize_data_path(entry.target);
    if (!normalized) return std::unexpected(normalized.error());
    auto components = detail::rel_components(*normalized);
    TargetLeaf out;
    out.leaf = components.back();
    components.pop_back();
    auto parent = detail::open_anchored_dir_strict(root_fd, components);
    if (!parent) {
        if (parent.error().find("No such file") != std::string::npos)
            return std::optional<TargetLeaf>{}; // nothing left to remove
        return std::unexpected(parent.error());
    }
    out.parent = std::move(*parent);
    return std::optional<TargetLeaf>(std::move(out));
}

std::expected<void, std::string> publish_remove_file(
    uint64_t device, const PlanEntry& entry, int root_fd) {
    auto target = open_remove_target(root_fd, entry);
    if (!target) return std::unexpected(target.error());
    if (!*target) return {}; // parent subtree vanished: already done
    const auto& t = **target;
    if (auto check = check_same_device(t.parent.get(), device, entry.target);
        !check) return std::unexpected(check.error());

    if (::unlinkat(t.parent.get(), t.leaf.c_str(), 0) != 0) {
        if (errno == ENOENT) return {}; // idempotent success
        return std::unexpected(std::format(
            "Cannot remove '{}': {}", entry.target, std::strerror(errno)));
    }
    if (auto synced = detail::fsync_fd(t.parent.get()); !synced)
        return std::unexpected(synced.error());
    return {};
}

std::expected<void, std::string> publish_remove_dir(
    uint64_t device, const PlanEntry& entry, int root_fd) {
    auto target = open_remove_target(root_fd, entry);
    if (!target) return std::unexpected(target.error());
    if (!*target) return {};
    const auto& t = **target;
    if (auto check = check_same_device(t.parent.get(), device, entry.target);
        !check) return std::unexpected(check.error());

    if (::unlinkat(t.parent.get(), t.leaf.c_str(), AT_REMOVEDIR) != 0) {
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
    if (auto synced = detail::fsync_fd(t.parent.get()); !synced)
        return std::unexpected(synced.error());
    return {};
}

} // namespace txn

// --- journal model -----------------------------------------------------------

struct JournalContext {
    std::string kind;             // install | remove | reconcile
    bool final{false};
    std::string sysroot;
    bool regenerate_profile{false};
    std::vector<std::string> toolchain_activations;              // "gcc:15"
    std::vector<std::pair<char, std::string>> touched;           // 'F'|'D'|'L' + rel path
    std::vector<std::string> package_manifests_toml;             // verbatim blocks
};

struct ParsedJournal {
    JournalContext ctx;
    std::vector<PlanEntry> plan;
};

inline std::string render_journal(
    const JournalContext& ctx, const std::vector<PlanEntry>& plan) {
    std::string text(journal_magic_line);
    text += '\n';
    text += std::format("kind {}\n", ctx.kind);
    text += std::format("final {}\n", ctx.final);
    text += "[context]\n";
    text += std::format("sysroot {}\n", ctx.sysroot);
    text += std::format("regenerate_profile {}\n", ctx.regenerate_profile);
    for (const auto& activation : ctx.toolchain_activations)
        text += std::format("toolchain_activate {}\n", activation);
    for (const auto& [type, path] : ctx.touched)
        text += std::format("touched {} {}\n", type, path);
    for (const auto& manifest : ctx.package_manifests_toml) {
        text += "package-begin\n";
        text += manifest;
        if (!manifest.empty() && manifest.back() != '\n') text += '\n';
        text += "package-end\n";
    }
    text += "[plan]\n";
    for (const auto& entry : plan) {
        switch (entry.kind) {
        case PlanEntry::Kind::PutFile:
            text += std::format("P F {:04o} {} {}\n",
                entry.mode, entry.staged, entry.target);
            break;
        case PlanEntry::Kind::PutSymlink:
            text += std::format("P L {} {}\n", entry.staged, entry.target);
            break;
        case PlanEntry::Kind::EnsureDir:
            text += std::format("P D {}\n", entry.target);
            break;
        case PlanEntry::Kind::RemoveFile:
            text += std::format("R F {}\n", entry.target);
            break;
        case PlanEntry::Kind::RemoveDir:
            text += std::format("R D {}\n", entry.target);
            break;
        }
    }
    return text;
}

// Line-oriented parser, tolerant of unknown lines (forward compatibility):
// anything unrecognized outside the plan grammar is ignored.
inline std::expected<ParsedJournal, std::string> parse_journal(std::string_view text) {
    ParsedJournal out;
    bool first = true;
    bool have_kind = false;
    bool in_plan = false;
    bool collecting_manifest = false;
    std::string manifest_block;

    const auto flush_manifest = [&] {
        if (collecting_manifest) {
            while (!manifest_block.empty() && manifest_block.back() == '\n')
                manifest_block.pop_back();
            out.ctx.package_manifests_toml.push_back(manifest_block);
            manifest_block.clear();
            collecting_manifest = false;
        }
    };

    size_t start = 0;
    std::optional<std::string> failure;
    const auto handle_line = [&](std::string_view line) -> void {
        if (first) {
            first = false;
            if (line != journal_magic_line) {
                failure = std::format("Journal header mismatch: expected '{}', got '{}'",
                    journal_magic_line, line);
            }
            return;
        }
        if (collecting_manifest) {
            if (line == "package-end") {
                flush_manifest();
            } else {
                manifest_block.append(line);
                manifest_block.push_back('\n');
            }
            return;
        }
        if (line == "[context]") {
            in_plan = false;
            return;
        }
        if (line == "[plan]") {
            in_plan = true;
            return;
        }
        if (line.empty()) return;
        const auto tokens = txn::split_tokens(line);
        if (tokens.empty()) return; // whitespace-only: tolerated
        const auto& key = tokens[0];
        if (key == "kind" && tokens.size() == 2) {
            out.ctx.kind = tokens[1];
            have_kind = true;
        } else if (key == "final" && tokens.size() == 2) {
            if (tokens[1] != "true" && tokens[1] != "false") {
                failure = "Invalid 'final' value: " + tokens[1];
                return;
            }
            out.ctx.final = tokens[1] == "true";
        } else if (key == "sysroot" && tokens.size() >= 2) {
            out.ctx.sysroot = txn::join_tokens(tokens, 1);
        } else if (key == "regenerate_profile" && tokens.size() == 2) {
            if (tokens[1] != "true" && tokens[1] != "false") {
                failure = "Invalid 'regenerate_profile' value: " + tokens[1];
                return;
            }
            out.ctx.regenerate_profile = tokens[1] == "true";
        } else if (key == "toolchain_activate" && tokens.size() == 2) {
            out.ctx.toolchain_activations.push_back(tokens[1]);
        } else if (key == "touched" && tokens.size() == 3
            && tokens[1].size() == 1) {
            out.ctx.touched.emplace_back(tokens[1][0], tokens[2]);
        } else if (key == "package-begin") {
            collecting_manifest = true;
        } else if (in_plan && key.size() == 1 && (key == "P" || key == "R")) {
            PlanEntry entry;
            bool recognized = false;
            if (key == "P" && tokens.size() >= 3) {
                if (tokens[1] == "F") {
                    // Anything but the exact v1 shape stays an unknown,
                    // forward-compatible line and is ignored.
                    if (tokens.size() == 5) {
                        auto mode = txn::parse_mode(tokens[2]);
                        if (!mode) {
                            failure = mode.error();
                            return;
                        }
                        entry.kind = PlanEntry::Kind::PutFile;
                        entry.mode = *mode;
                        entry.staged = tokens[3];
                        entry.target = tokens[4];
                        recognized = true;
                    }
                } else if (tokens[1] == "L") {
                    if (tokens.size() == 4) {
                        entry.kind = PlanEntry::Kind::PutSymlink;
                        entry.staged = tokens[2];
                        entry.target = tokens[3];
                        recognized = true;
                    }
                } else if (tokens[1] == "D") {
                    if (tokens.size() == 3) {
                        entry.kind = PlanEntry::Kind::EnsureDir;
                        entry.target = tokens[2];
                        recognized = true;
                    }
                }
            } else if (key == "R" && tokens.size() >= 3) {
                if (tokens[1] == "F" && tokens.size() == 3) {
                    entry.kind = PlanEntry::Kind::RemoveFile;
                    entry.target = tokens[2];
                    recognized = true;
                } else if (tokens[1] == "D" && tokens.size() == 3) {
                    entry.kind = PlanEntry::Kind::RemoveDir;
                    entry.target = tokens[2];
                    recognized = true;
                }
            }
            if (recognized) out.plan.push_back(std::move(entry));
        }
        // Everything else: tolerated and ignored (forward compatibility).
    };

    while (start <= text.size()) {
        auto newline = text.find('\n', start);
        auto line = text.substr(start,
            newline == std::string_view::npos ? std::string_view::npos : newline - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        handle_line(line);
        if (failure) return std::unexpected(*failure);
        if (newline == std::string_view::npos) break;
        start = newline + 1;
    }

    if (!have_kind)
        return std::unexpected("Journal is missing the 'kind' field");
    return out;
}

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
        std::string_view stage_rel, uint32_t staging_mode = 0600) {
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
        int fd = ::openat(stage_parent->get(), leaf.c_str(),
            flags, staging_mode & 07777);
        if (fd < 0) {
            return std::unexpected(std::format(
                "Cannot stage '{}': {}", stage_rel, std::strerror(errno)));
        }
        return fd;
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
        uint32_t mode) {
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

    // Durability barrier before the LMDB commit: fsync all staged files,
    // then directories bottom-up (a parent's entry for a child only means
    // something once the child is durable), ending with the transaction
    // directory itself. After this returns, a crash may lose the LMDB commit
    // but can never leave a journal or payload half-written on disk.
    std::expected<void, std::string> sync_staging() {
        return detail::sync_tree_anchored(dir_fd_.get());
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
        text.resize(static_cast<size_t>(status.st_size));
        size_t done = 0;
        while (done < text.size()) {
            ssize_t got = ::read(file.get(), text.data() + done, text.size() - done);
            if (got < 0 && errno == EINTR) continue;
            if (got <= 0) {
                return std::unexpected(
                    "Short read on journal: " + std::string(std::strerror(errno)));
            }
            done += static_cast<size_t>(got);
        }
        return text;
    }

    // Idempotent publication of [plan] into the live tree; every change is
    // followed by an fsync of the affected parent directory. Each entry
    // pre-checks its target parent st_dev against the transaction directory
    // and reports EXDEV-style errors naming the path before mutating.
    // On any failing entry, publication stops immediately: earlier entries
    // stay published (each was individually atomic) while the failure and
    // its evidence remain untouched, so a retry -- from the normal path or
    // startup recovery -- replays the whole plan idempotently.
    std::expected<void, std::string> publish(std::string_view journal_text) {
        auto parsed = parse_journal(journal_text);
        if (!parsed) return std::unexpected(parsed.error());

        detail::UniqueFd root(detail::open_root_fd(target_root_));
        if (root.get() < 0) {
            return std::unexpected("Cannot securely open target root: "
                + std::string(std::strerror(errno)));
        }

        for (const auto& entry : parsed->plan) {
            std::expected<void, std::string> outcome = std::unexpected(
                std::string("Unhandled plan entry"));
            if (entry.kind == PlanEntry::Kind::PutFile) {
                auto target = txn::open_target(root.get(), entry.target);
                if (!target) return std::unexpected(target.error());
                outcome = txn::publish_put_file(
                    dir_fd_.get(), device_, entry, *target);
            } else if (entry.kind == PlanEntry::Kind::PutSymlink) {
                auto target = txn::open_target(root.get(), entry.target);
                if (!target) return std::unexpected(target.error());
                outcome = txn::publish_put_symlink(
                    dir_fd_.get(), device_, entry, *target);
            } else if (entry.kind == PlanEntry::Kind::EnsureDir) {
                outcome = txn::publish_ensure_dir(root.get(), device_, entry);
            } else if (entry.kind == PlanEntry::Kind::RemoveFile) {
                outcome = txn::publish_remove_file(device_, entry, root.get());
            } else { // RemoveDir
                outcome = txn::publish_remove_dir(device_, entry, root.get());
            }
            if (!outcome) return outcome; // stop at first failure; evidence kept
        }
        return {};
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
        std::string relative_dir, detail::UniqueFd dir_fd, uint64_t device,
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
    uint64_t device_{0};
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
    if (static_cast<uint64_t>(dir_status.st_dev)
        != static_cast<uint64_t>(root_status.st_dev)) {
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
    if (static_cast<uint64_t>(dir_status.st_dev)
        != static_cast<uint64_t>(root_status.st_dev)) {
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
