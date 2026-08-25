export module sage.package:trigger;

import std;
import sage.vendor.toml;

export namespace sage::package {

using std::size_t;

// TOML basic-string escaping, shared by every serializer in this module.
inline std::string escape_toml_basic_string(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (unsigned char ch : value) {
        switch (ch) {
            case '"': escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\b': escaped += "\\b"; break;
            case '\t': escaped += "\\t"; break;
            case '\n': escaped += "\\n"; break;
            case '\f': escaped += "\\f"; break;
            case '\r': escaped += "\\r"; break;
            default:
                if (ch < 0x20 || ch == 0x7f) {
                    escaped += std::format("\\u{:04X}", ch);
                } else {
                    escaped.push_back(static_cast<char>(ch));
                }
        }
    }
    return escaped;
}

// ============================================================================
// Capability Hooks
// ============================================================================
//
// A package that provides a capability may also declare *how* that capability
// is invoked. Without this, a trigger wanting "regenerate the initramfs" would
// have to hardcode a tool path and silently do nothing the moment the admin
// switched from mkinitcpio to dracut.

struct CapabilityHook {
    std::string capability;          // e.g. "virtual/initramfs-generator"
    std::string exec;                // absolute path inside the target root
    std::vector<std::string> args;

    [[nodiscard]] std::string command_line() const {
        std::string cmd = exec;
        for (const auto& a : args) {
            cmd += " ";
            cmd += a;
        }
        return cmd;
    }
};

// ============================================================================
// Package Triggers
// ============================================================================
//
// Declared by the package, evaluated once per transaction. A trigger fires
// when either condition matches:
//
//   on_paths      -- some file touched by the transaction sits under one of
//                    these relative prefixes ("usr/lib/modules/").
//   on_capability -- some package taking part in the transaction provides one
//                    of these capabilities ("virtual/kernel").
//
// and runs either a fixed `exec`, or -- preferably -- `run_capability`, which
// is resolved at fire time through the active provider's CapabilityHook.

struct Trigger {
    std::string name;
    std::vector<std::string> on_paths;
    std::vector<std::string> on_capability;
    std::string exec;                 // absolute path inside the target root
    std::vector<std::string> args;
    std::string run_capability;       // resolved via the provider's hook
    // Lower runs first. Ordering is not cosmetic: the initramfs must exist
    // before the bootloader is asked to reference it, and ldconfig must have
    // run before anything that dlopen()s.
    int priority{50};
    // Missing exec: hard error vs. warn+skip. Kept last so designated
    // initializers can append it after the classic fields (GCC rejects
    // out-of-order designators that clang tolerates).
    bool required{false};

    [[nodiscard]] bool matches_path(std::string_view rel_path) const {
        for (const auto& prefix : on_paths) {
            if (rel_path.starts_with(prefix)) return true;
        }
        return false;
    }

    [[nodiscard]] bool matches_capability(std::string_view cap) const {
        return std::ranges::find(on_capability, cap) != on_capability.end();
    }

    [[nodiscard]] bool is_valid() const {
        if (name.empty()) return false;
        if (on_paths.empty() && on_capability.empty()) return false;
        return !exec.empty() || !run_capability.empty();
    }
};

// Shared TOML readers for the two array-of-tables sections. Both are accepted
// at the document root and inside [package], matching how sage merges the
// other recipe arrays across scopes.

inline void parse_capability_hooks(const vendor::toml::table& tbl, std::vector<CapabilityHook>& out) {
    auto read = [&](const vendor::toml::table& scope) {
        auto* arr = scope.get_as<vendor::toml::array>("capability_hooks");
        if (!arr) return;
        for (auto&& item : *arr) {
            auto* t = item.as_table();
            if (!t) continue;
            CapabilityHook h;
            h.capability = (*t)["capability"].value_or("");
            h.exec = (*t)["exec"].value_or("");
            if (h.capability.empty() || h.exec.empty()) continue;
            if (auto* args = t->get_as<vendor::toml::array>("args")) {
                for (auto&& a : *args) {
                    if (auto str = a.value<std::string_view>()) h.args.emplace_back(*str);
                }
            }
            out.push_back(std::move(h));
        }
    };
    read(tbl);
    if (auto* pkg = tbl.get_as<vendor::toml::table>("package")) read(*pkg);
}

inline std::expected<void, std::string> parse_triggers(const vendor::toml::table& tbl, std::vector<Trigger>& out) {
    auto read_strings = [](const vendor::toml::table& t, const char* key, std::vector<std::string>& dest) {
        if (auto* arr = t.get_as<vendor::toml::array>(key)) {
            for (auto&& v : *arr) {
                if (auto str = v.value<std::string_view>()) dest.emplace_back(*str);
            }
        }
    };

    auto read = [&](const vendor::toml::table& scope) -> std::expected<void, std::string> {
        auto* arr = scope.get_as<vendor::toml::array>("triggers");
        if (!arr) return {};
        for (auto&& item : *arr) {
            auto* t = item.as_table();
            if (!t) continue;
            Trigger tr;
            tr.name = (*t)["name"].value_or("");
            tr.exec = (*t)["exec"].value_or("");
            tr.run_capability = (*t)["run_capability"].value_or("");
            tr.required = (*t)["required"].value_or(false);
            tr.priority = static_cast<int>((*t)["priority"].value_or(50LL));
            read_strings(*t, "on_paths", tr.on_paths);
            read_strings(*t, "on_capability", tr.on_capability);
            read_strings(*t, "args", tr.args);
            if (!tr.is_valid()) {
                return std::unexpected(std::format(
                    "Invalid trigger '{}': needs a name, at least one of on_paths/on_capability, "
                    "and one of exec/run_capability",
                    tr.name.empty() ? "<unnamed>" : tr.name));
            }
            out.push_back(std::move(tr));
        }
        return {};
    };

    if (auto r = read(tbl); !r) return r;
    if (auto* pkg = tbl.get_as<vendor::toml::table>("package")) {
        if (auto r = read(*pkg); !r) return r;
    }
    return {};
}

// Parse a standalone `.METADATA/triggers.toml` document.
inline std::expected<std::vector<Trigger>, std::string> parse_triggers_toml(std::string_view toml_content) {
    auto tbl_res = vendor::toml::parse_string(toml_content);
    if (!tbl_res) return std::unexpected(tbl_res.error());
    std::vector<Trigger> out;
    if (auto r = parse_triggers(*tbl_res, out); !r) return std::unexpected(r.error());
    return out;
}

inline std::string serialize_triggers_toml(const std::vector<Trigger>& triggers) {
    const auto quote = [](std::string_view value) {
        return escape_toml_basic_string(value);
    };
    std::ostringstream ss;
    ss << "schema_version = 1\n";
    for (const auto& t : triggers) {
        ss << "\n[[triggers]]\n";
        ss << "name = \"" << quote(t.name) << "\"\n";
        if (!t.on_paths.empty()) {
            ss << "on_paths = [";
            for (size_t i = 0; i < t.on_paths.size(); ++i) {
                ss << (i ? ", " : "") << "\"" << quote(t.on_paths[i]) << "\"";
            }
            ss << "]\n";
        }
        if (!t.on_capability.empty()) {
            ss << "on_capability = [";
            for (size_t i = 0; i < t.on_capability.size(); ++i) {
                ss << (i ? ", " : "") << "\"" << quote(t.on_capability[i]) << "\"";
            }
            ss << "]\n";
        }
        if (!t.run_capability.empty()) {
            ss << "run_capability = \"" << quote(t.run_capability) << "\"\n";
        }
        if (!t.exec.empty()) {
            ss << "exec = \"" << quote(t.exec) << "\"\n";
        }
        if (t.required) ss << "required = true\n";
        ss << "priority = " << t.priority << "\n";
        if (!t.args.empty()) {
            ss << "args = [";
            for (size_t i = 0; i < t.args.size(); ++i) {
                ss << (i ? ", " : "") << "\"" << quote(t.args[i]) << "\"";
            }
            ss << "]\n";
        }
    }
    return ss.str();
}

} // namespace sage::package
