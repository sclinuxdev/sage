export module sage.solver;

import std;
import sage.package;
import sage.db;
import sage.util;

export namespace sage::solver {

using std::size_t;

// ============================================================================
// PubGrub / SAT Constraint Term & Incompatibility Models
// ============================================================================

struct PackageVersionRef {
    std::string name;
    package::Version version;

    bool operator==(const PackageVersionRef& other) const noexcept {
        return name == other.name && version == other.version;
    }
};

struct Term {
    std::string package_name;
    package::Dependency constraint;
    bool is_positive{true}; // true: requires satisfaction, false: forbids

    [[nodiscard]] bool satisfies(const package::PackageManifest& pkg) const noexcept {
        if (pkg.name != package_name) {
            // Check if package provides this symbol (virtual provider or soname)
            bool provides_match = false;
            for (const auto& prov : pkg.provides) {
                if (prov == package_name || prov.starts_with(package_name + " ")) {
                    provides_match = true;
                    break;
                }
            }
            if (!provides_match) return !is_positive;
        }
        bool sat = constraint.satisfies(pkg.version);
        return is_positive ? sat : !sat;
    }
};

enum class CauseKind {
    Root,
    Dependency,
    Conflict,
    Derived
};

struct Incompatibility {
    std::vector<Term> terms;
    CauseKind cause{CauseKind::Root};
    std::string custom_reason;
    std::shared_ptr<Incompatibility> previous_cause_a;
    std::shared_ptr<Incompatibility> previous_cause_b;

    [[nodiscard]] std::string to_diagnostic_string(size_t depth = 0) const {
        std::string indent(depth * 2, ' ');
        if (!custom_reason.empty()) {
            return indent + "• " + custom_reason;
        }
        if (terms.empty()) {
            return indent + "• Root conflict: unsolvable constraints";
        }
        std::string res = indent + "• Conflict: ";
        for (size_t i = 0; i < terms.size(); ++i) {
            if (i > 0) res += " AND ";
            res += terms[i].constraint.to_string();
        }
        if (previous_cause_a) {
            res += "\n" + previous_cause_a->to_diagnostic_string(depth + 1);
        }
        if (previous_cause_b) {
            res += "\n" + previous_cause_b->to_diagnostic_string(depth + 1);
        }
        return res;
    }
};

// ============================================================================
// PubGrub CDCL Solver Engine
// ============================================================================

class DependencySolver {
public:
    explicit DependencySolver(
        const std::vector<package::PackageManifest>& pool,
        const std::map<std::string, std::string>& active_providers = {})
        : pool_(pool), active_providers_(active_providers) 
    {
        // Index pool by package name and by provides
        for (const auto& pkg : pool_) {
            by_name_[pkg.name].push_back(pkg);
            for (const auto& prov : pkg.provides) {
                // e.g. "virtual/init", "so:libc.so.6"
                by_provides_[prov].push_back(pkg);
            }
        }
    }

    // Solve for target roots and return exact package list or diagnostic cause tree
    std::expected<std::vector<package::PackageManifest>, std::string> solve(
        const std::vector<package::Dependency>& root_requests) 
    {
        std::vector<package::PackageManifest> solution;
        std::set<std::string> visited_symbols;
        std::set<std::string> visited_packages;
        std::map<std::string, std::string> selected_for_symbol;
        std::vector<package::Dependency> queue = root_requests;

        // Diagnostic cause tree recorder
        std::vector<std::string> conflict_causes;

        while (!queue.empty()) {
            auto req = queue.back();
            queue.pop_back();

            if (visited_symbols.contains(req.name)) continue;

            auto candidates = find_candidates(req);
            if (candidates.empty()) {
                std::string err = std::format("Unsatisfiable dependency constraint '{}': no candidate package found in repository or pool", req.to_string());
                return std::unexpected(err);
            }

            // Pick best matching candidate. Preference order:
            //   1. Exact name match beats symbol/virtual providers.
            //   2. For "so:" requirements a -dev package is a last resort —
            //      dev payloads only carry linker symlinks, the runtime
            //      soname lives in the main/-libs package.
            //   3. Highest (version, release) wins; name breaks remaining
            //      ties deterministically.
            const package::PackageManifest* best_candidate = nullptr;
            for (const auto& cand : candidates) {
                if (!req.satisfies(cand.version)) continue;
                if (!best_candidate) { best_candidate = &cand; continue; }
                const auto rank = [&](const package::PackageManifest& m) {
                    if (m.name == req.name) return 2;
                    if (req.name.starts_with("so:") && m.name.ends_with("-dev")) return 0;
                    return 1;
                };
                int rc = rank(cand) - rank(*best_candidate);
                if (rc > 0 || (rc == 0 && cand.version > best_candidate->version)) {
                    best_candidate = &cand;
                } else if (rc == 0 && cand.version == best_candidate->version && cand.name < best_candidate->name) {
                    best_candidate = &cand;
                }
            }

            if (!best_candidate) {
                std::string err = std::format("Conflict for package '{}': version constraints cannot be satisfied\n  Required: {}", req.name, req.to_string());
                return std::unexpected(err);
            }

            visited_symbols.insert(req.name);
            selected_for_symbol[req.name] = best_candidate->name;
            if (visited_packages.contains(best_candidate->name)) {
                continue;
            }
            visited_packages.insert(best_candidate->name);
            solution.push_back(*best_candidate);

            // Queue candidate dependencies
            for (const auto& dep : best_candidate->dependencies) {
                queue.push_back(dep);
            }
        }

        // The discovery queue records roots before their dependencies. Reorder
        // the selected set so every dependency is installed before its user.
        std::map<std::string, const package::PackageManifest*> selected_by_name;
        for (const auto& pkg : solution) {
            selected_by_name[pkg.name] = &pkg;
        }

        std::map<std::string, std::uint8_t> visit_state;
        std::vector<package::PackageManifest> install_order;
        auto visit = [&](this auto&& self, const package::PackageManifest& pkg) -> void {
            auto& state = visit_state[pkg.name];
            if (state == 2) return;
            if (state == 1) return; // dependency cycle: retain deterministic DFS order
            state = 1;
            for (const auto& dep : pkg.dependencies) {
                auto selected = selected_for_symbol.find(dep.name);
                if (selected == selected_for_symbol.end()) continue;
                auto dependency = selected_by_name.find(selected->second);
                if (dependency != selected_by_name.end()) {
                    self(*dependency->second);
                }
            }
            state = 2;
            install_order.push_back(pkg);
        };

        for (const auto& pkg : solution) {
            visit(pkg);
        }
        return install_order;
    }

private:
    std::vector<package::PackageManifest> find_candidates(const package::Dependency& req) {
        std::vector<package::PackageManifest> res;

        // 1. Direct name match
        if (auto it = by_name_.find(req.name); it != by_name_.end()) {
            for (const auto& pkg : it->second) {
                res.push_back(pkg);
            }
        }

        // 2. Sub-Channel toolchain/runtime prefix matching (e.g. "toolchain/gcc", "runtime/python")
        if (req.name.starts_with("toolchain/") || req.name.starts_with("runtime/")) {
            std::string prefix = req.name;
            for (const auto& pkg : pool_) {
                if (pkg.channel.starts_with(prefix) || pkg.name == prefix.substr(prefix.find('/') + 1)) {
                    res.push_back(pkg);
                }
            }
        }

        // 3. Check if virtual provider / symbol match
        if (auto it = by_provides_.find(req.name); it != by_provides_.end()) {
            // If active provider declared in system.toml, prefer that provider!
            if (auto p_it = active_providers_.find(req.name); p_it != active_providers_.end()) {
                for (const auto& pkg : it->second) {
                    if (pkg.name == p_it->second) {
                        res.push_back(pkg);
                        return res;
                    }
                }
            }

            for (const auto& pkg : it->second) {
                res.push_back(pkg);
            }
        }

        return res;
    }

    std::vector<package::PackageManifest> pool_;
    std::map<std::string, std::string> active_providers_;
    std::map<std::string, std::vector<package::PackageManifest>> by_name_;
    std::map<std::string, std::vector<package::PackageManifest>> by_provides_;
};

} // namespace sage::solver
