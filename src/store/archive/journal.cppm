export module sage.archive:journal;

import std;

export namespace sage::archive {

inline constexpr std::string_view journal_magic_line = "sage-journal 1";
inline constexpr std::string_view journal_file_name = "journal";

struct PlanEntry {
    enum class Kind { PutFile, PutSymlink, EnsureDir, RemoveFile, RemoveDir };
    Kind kind;
    std::uint32_t mode{0644};
    std::string staged;
    std::string target;
};

namespace txn {

std::expected<std::uint32_t, std::string> parse_mode(std::string_view token) {
    if (token.empty() || token.size() > 6)
        return std::unexpected("Invalid mode '" + std::string(token) + "'");
    std::uint32_t mode = 0;
    for (const char c : token) {
        if (c < '0' || c > '7')
            return std::unexpected(
                "Invalid mode '" + std::string(token) + "' (expected octal)");
        mode = (mode << 3) | static_cast<std::uint32_t>(c - '0');
    }
    return mode;
}

std::vector<std::string> split_tokens(std::string_view line) {
    std::vector<std::string> out;
    std::size_t start = 0;
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

std::string join_tokens(const std::vector<std::string>& tokens, std::size_t from) {
    std::string out;
    for (std::size_t i = from; i < tokens.size(); ++i) {
        if (i > from) out.push_back(' ');
        out += tokens[i];
    }
    return out;
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
    auto touched = ctx.touched;
    std::unordered_set<std::string> touched_paths;
    for (const auto& [type, path] : touched) touched_paths.insert(path);
    for (const auto& entry : plan) {
        std::optional<char> type;
        switch (entry.kind) {
        case PlanEntry::Kind::PutFile:
        case PlanEntry::Kind::RemoveFile: type = 'F'; break;
        case PlanEntry::Kind::PutSymlink: type = 'L'; break;
        case PlanEntry::Kind::RemoveDir: type = 'D'; break;
        case PlanEntry::Kind::EnsureDir: break;
        }
        if (type && touched_paths.insert(entry.target).second)
            touched.emplace_back(*type, entry.target);
    }

    std::string text(journal_magic_line);
    text += '\n';
    text += std::format("kind {}\n", ctx.kind);
    text += std::format("final {}\n", ctx.final);
    text += "[context]\n";
    text += std::format("sysroot {}\n", ctx.sysroot);
    text += std::format("regenerate_profile {}\n", ctx.regenerate_profile);
    for (const auto& activation : ctx.toolchain_activations)
        text += std::format("toolchain_activate {}\n", activation);
    for (const auto& [type, path] : touched)
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

    std::size_t start = 0;
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


} // namespace sage::archive
