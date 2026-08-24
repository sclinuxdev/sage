export module sage.package:deps;

import std;
import sage.util;
import :version;

export namespace sage::package {

enum class ConstraintOp {
    Any,
    Equal,
    NotEqual,
    GreaterEqual,
    LessEqual,
    Greater,
    Less
};

struct Dependency {
    std::string name;
    ConstraintOp op{ConstraintOp::Any};
    Version version;

    static Dependency parse(std::string_view s) {
        s = util::trim(s);
        Dependency d;
        if (s.empty()) return d;

        size_t op_pos = std::string_view::npos;
        std::string_view op_str;

        static const std::pair<std::string_view, ConstraintOp> ops[] = {
            {">=", ConstraintOp::GreaterEqual},
            {"<=", ConstraintOp::LessEqual},
            {"!=", ConstraintOp::NotEqual},
            {"==", ConstraintOp::Equal},
            {"=",  ConstraintOp::Equal},
            {">",  ConstraintOp::Greater},
            {"<",  ConstraintOp::Less}
        };

        for (const auto& [str, op] : ops) {
            if (auto pos = s.find(str); pos != std::string_view::npos) {
                op_pos = pos;
                op_str = str;
                d.op = op;
                break;
            }
        }

        if (op_pos == std::string_view::npos) {
            d.name = std::string(s);
            d.op = ConstraintOp::Any;
        } else {
            d.name = std::string(util::trim(s.substr(0, op_pos)));
            d.version = Version::parse(util::trim(s.substr(op_pos + op_str.size())));
        }
        return d;
    }

    [[nodiscard]] bool satisfies(const Version& target_ver) const noexcept {
        if (op == ConstraintOp::Any) return true;
        auto cmp = target_ver <=> version;
        switch (op) {
            case ConstraintOp::Equal:        return cmp == 0;
            case ConstraintOp::NotEqual:     return cmp != 0;
            case ConstraintOp::GreaterEqual: return cmp >= 0;
            case ConstraintOp::LessEqual:    return cmp <= 0;
            case ConstraintOp::Greater:      return cmp > 0;
            case ConstraintOp::Less:         return cmp < 0;
            default:                         return true;
        }
    }

    [[nodiscard]] std::string to_string() const {
        if (op == ConstraintOp::Any) return name;
        std::string_view op_sym = "=";
        switch (op) {
            case ConstraintOp::GreaterEqual: op_sym = ">="; break;
            case ConstraintOp::LessEqual:    op_sym = "<="; break;
            case ConstraintOp::NotEqual:     op_sym = "!="; break;
            case ConstraintOp::Greater:      op_sym = ">"; break;
            case ConstraintOp::Less:         op_sym = "<"; break;
            default: op_sym = "="; break;
        }
        return std::format("{} {} {}", name, op_sym, version.to_string());
    }
};

} // namespace sage::package
