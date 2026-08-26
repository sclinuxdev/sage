export module sage.solver;

import std;
import sage.package;
import sage.db;
import sage.util;

export namespace sage::solver {

using std::size_t;

// ============================================================================
// PubGrub / CDCL SAT Constraint Term & Incompatibility Models
// ============================================================================

/// Identity reference for a specific package name and version.
struct PackageVersionRef {
    std::string name;
    package::Version version;

    bool operator==(const PackageVersionRef& other) const noexcept {
        return name == other.name && version == other.version;
    }
};

/// Extracts the explicit version from a versioned provides entry (e.g. "virtual/libc = 2.44").
/// If unversioned, returns std::nullopt (fallback to package's own version).
inline std::optional<package::Version> provided_version_of(
    const package::PackageManifest& pkg, std::string_view name)
{
    const std::string prefix = std::string(name) + " ";
    for (const auto& prov : pkg.provides) {
        if (!prov.starts_with(prefix)) continue;
        auto entry = package::Dependency::parse(prov);
        if (entry.op != package::ConstraintOp::Any) return entry.version;
    }
    return std::nullopt;
}

/// A Term represents a boolean constraint on a package or virtual symbol name.
/// Positive term: symbol MUST satisfy constraint.
/// Negative term: symbol MUST NOT satisfy constraint.
struct Term {
    std::string symbol_name;
    package::Dependency constraint;
    bool is_positive{true}; // true: requires, false: forbids

    /// Checks if a concrete package manifest satisfies this term for symbol_name.
    [[nodiscard]] bool satisfies(const package::PackageManifest& pkg) const noexcept {
        if (pkg.name != symbol_name) {
            // Check if symbol is provided by this package (e.g. "virtual/libc", "so:libc.so.6")
            bool has_unversioned = false;
            bool has_matching_versioned = false;
            bool has_explicit_versioned = false;
            for (const auto& prov : pkg.provides) {
                if (prov == symbol_name) {
                    has_unversioned = true;
                    continue;
                }
                if (!prov.starts_with(symbol_name + " ")) continue;
                auto entry = package::Dependency::parse(prov);
                if (entry.op != package::ConstraintOp::Any) {
                    has_explicit_versioned = true;
                    if (constraint.satisfies(entry.version)) {
                        has_matching_versioned = true;
                    }
                } else {
                    has_unversioned = true;
                }
            }
            if (has_matching_versioned) return is_positive;
            if (has_unversioned) {
                // Unversioned provides fall back to the provider's package version.
                bool sat = constraint.satisfies(pkg.version);
                return is_positive ? sat : !sat;
            }
            if (has_explicit_versioned) return !is_positive;
            return !is_positive;
        }
        bool sat = constraint.satisfies(pkg.version);
        return is_positive ? sat : !sat;
    }

    /// Negation of the term.
    [[nodiscard]] Term negate() const {
        Term t = *this;
        t.is_positive = !t.is_positive;
        return t;
    }

    [[nodiscard]] std::string to_string() const {
        if (is_positive) {
            return constraint.to_string();
        }
        return "NOT (" + constraint.to_string() + ")";
    }
};

enum class CauseKind {
    Root,
    Dependency,
    Conflict,
    Derived
};

/// An Incompatibility represents a set of terms that cannot all be true simultaneously.
/// In CDCL terms, this corresponds to a conflict clause / nogood: (NOT T1 OR NOT T2 OR ...).
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
            res += terms[i].to_string();
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
// PubGrub CDCL Solver Engine Implementation
// ============================================================================

/// Assignment on the PubGrub trail representing a decision or a derived constraint.
struct Assignment {
    Term term;
    int decision_level{0};
    int index{0};
    std::shared_ptr<Incompatibility> cause; // nullptr for decisions
    std::optional<package::PackageManifest> decision_package; // Set if this assignment is a concrete package selection
};

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
                // e.g. "virtual/init", "so:libc.so.6" or "virtual/libc = 2.44"
                by_provides_[package::Dependency::parse(prov).name].push_back(pkg);
            }
        }
    }

    /// Solves for target root requests using a complete PubGrub / CDCL resolution algorithm.
    /// Returns either the dependency-first sorted package list, or a diagnostic cause tree error string.
    std::expected<std::vector<package::PackageManifest>, std::string> solve(
        const std::vector<package::Dependency>& root_requests)
    {
        if (root_requests.empty()) return std::vector<package::PackageManifest>{};

        trail_.clear();
        incompatibilities_.clear();
        decision_level_ = 0;

        // 1. Initialize root incompatibilities:
        // For each root request R, we add an incompatibility: { NOT R }
        // which forces unit propagation to require R at decision level 0.
        for (const auto& req : root_requests) {
            auto incomp = std::make_shared<Incompatibility>();
            incomp->cause = CauseKind::Root;
            incomp->terms.push_back(Term{
                .symbol_name = req.name,
                .constraint = req,
                .is_positive = false // "NOT req is incompatible with reality" -> req is required
            });
            incompatibilities_.push_back(incomp);
        }

        // 2. Main PubGrub CDCL Loop:
        // While there are undecided positive package requirements, propagate units and make decisions.
        while (true) {
            // Unit Propagation
            auto conflict = unit_propagation();
            if (conflict) {
                // Conflict at root level means the constraints are mathematically unsatisfiable
                if (decision_level_ == 0) {
                    std::string diag = "Dependency solver conflict:\n" + conflict->to_diagnostic_string(1);
                    return std::unexpected(diag);
                }

                // Resolve conflict, learn asserting clause, and backjump
                auto [backjump_level, learned_incomp, asserted_term] = resolve_conflict(conflict);
                backtrack(backjump_level);
                incompatibilities_.push_back(learned_incomp);

                // Add asserted fact to the trail at the backjumped level
                add_assignment(asserted_term, learned_incomp);
                continue;
            }

            // Decision Step: pick next unsatisfied symbol and candidate package
            auto next_symbol = choose_next_symbol();
            if (!next_symbol) {
                // All requirements are satisfied! We have reached a complete solution.
                break;
            }

            auto decision_res = decide(*next_symbol);
            if (!decision_res) {
                auto no_cand_incomp = std::make_shared<Incompatibility>();
                no_cand_incomp->cause = CauseKind::Conflict;
                no_cand_incomp->custom_reason = std::format(
                    "No candidate package satisfies '{}' under current constraints",
                    next_symbol->to_string());

                std::vector<Term> terms;
                auto add_unique_term = [&](const Term& t) {
                    for (const auto& existing : terms) {
                        if (existing.symbol_name == t.symbol_name &&
                            existing.is_positive == t.is_positive &&
                            existing.constraint.op == t.constraint.op &&
                            existing.constraint.version == t.constraint.version) {
                            return;
                        }
                    }
                    terms.push_back(t);
                };

                for (const auto& assign : trail_) {
                    if (assign.term.symbol_name == next_symbol->symbol_name) {
                        add_unique_term(assign.term);
                    }
                }
                add_unique_term(*next_symbol);

                no_cand_incomp->terms = std::move(terms);
                incompatibilities_.push_back(no_cand_incomp);
                continue;
            }
        }

        // 3. Extract resolved packages from trail
        std::vector<package::PackageManifest> solution;
        std::set<std::string> seen_names;
        std::map<std::string, std::string> symbol_to_pkg;

        for (const auto& assign : trail_) {
            if (assign.decision_package) {
                if (seen_names.insert(assign.decision_package->name).second) {
                    solution.push_back(*assign.decision_package);
                }
                symbol_to_pkg[assign.term.symbol_name] = assign.decision_package->name;
            }
        }

        // 4. Deterministic topological ordering (dependency-first)
        return order_dependencies(solution, symbol_to_pkg);
    }

private:
    std::vector<Assignment> trail_;
    std::vector<std::shared_ptr<Incompatibility>> incompatibilities_;
    int decision_level_{0};

    std::vector<package::PackageManifest> pool_;
    std::map<std::string, std::string> active_providers_;
    std::map<std::string, std::vector<package::PackageManifest>> by_name_;
    std::map<std::string, std::vector<package::PackageManifest>> by_provides_;

    /// Evaluates a term's satisfaction against the trail.
    /// Returns 1 if satisfied, -1 if contradicted, 0 if undecided.
    int evaluate_term(const Term& term) const {
        for (const auto& assign : trail_) {
            if (assign.term.symbol_name != term.symbol_name) continue;
            if (assign.decision_package) {
                // Concrete package assigned to this symbol
                bool matches = term.satisfies(*assign.decision_package);
                return matches ? 1 : -1;
            }
            // Constraint-level term on trail
            // 1. Exact match on constraint operator and version
            if (assign.term.constraint.op == term.constraint.op
                && assign.term.constraint.version == term.constraint.version) {
                return (assign.term.is_positive == term.is_positive) ? 1 : -1;
            }
            // 2. Trail forbids ANY version of this symbol
            if (!assign.term.is_positive && assign.term.constraint.op == package::ConstraintOp::Any) {
                return term.is_positive ? -1 : 1;
            }
            // 3. Term forbids ANY version of this symbol and trail requires it
            if (!term.is_positive && term.constraint.op == package::ConstraintOp::Any && assign.term.is_positive) {
                return -1;
            }
            // 4. Trail has concrete equality constraint
            if (assign.term.is_positive && assign.term.constraint.op == package::ConstraintOp::Equal) {
                bool sat = term.constraint.satisfies(assign.term.constraint.version);
                if (term.is_positive) {
                    return sat ? 1 : -1;
                } else {
                    return sat ? -1 : 1;
                }
            }
        }
        return 0;
    }

    /// Unit propagation: iteratively scans incompatibilities to derive terms or detect conflicts.
    std::shared_ptr<Incompatibility> unit_propagation() {
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& incomp : incompatibilities_) {
                int satisfied_count = 0;
                int contradicted_count = 0;
                int undecided_count = 0;
                const Term* undecided_term = nullptr;

                for (const auto& term : incomp->terms) {
                    int eval = evaluate_term(term);
                    if (eval == 1) ++satisfied_count;
                    else if (eval == -1) ++contradicted_count;
                    else {
                        ++undecided_count;
                        undecided_term = &term;
                    }
                }

                // If all terms in incompatibility are satisfied -> CONFLICT!
                if (satisfied_count == static_cast<int>(incomp->terms.size())) {
                    return incomp;
                }

                // If all terms except one are satisfied, and one is undecided -> derive unit (negation of undecided term)
                if (satisfied_count == static_cast<int>(incomp->terms.size()) - 1 && undecided_count == 1) {
                    Term derived = undecided_term->negate();
                    add_assignment(derived, incomp);
                    changed = true;
                    break;
                }
            }
        }
        return nullptr;
    }

    static bool terms_equal(const Term& a, const Term& b) noexcept {
        return a.symbol_name == b.symbol_name
            && a.is_positive == b.is_positive
            && a.constraint.op == b.constraint.op
            && a.constraint.version == b.constraint.version;
    }

    static bool terms_opposite(const Term& a, const Term& b) noexcept {
        return a.symbol_name == b.symbol_name
            && a.is_positive != b.is_positive
            && a.constraint.op == b.constraint.op
            && a.constraint.version == b.constraint.version;
    }

    int term_decision_level(const Term& term) const {
        for (const auto& assign : trail_) {
            if (assign.term.symbol_name != term.symbol_name) continue;
            if (assign.decision_package && term.satisfies(*assign.decision_package)) {
                return assign.decision_level;
            }
            if (terms_equal(assign.term, term)) {
                return assign.decision_level;
            }
        }
        for (const auto& assign : trail_) {
            if (assign.term.symbol_name == term.symbol_name && assign.decision_package) {
                return assign.decision_level;
            }
        }
        return 0;
    }

    /// Resolves conflict via implication graph resolution, discovers UIP, and computes backjump level.
    struct ResolutionResult {
        int backjump_level;
        std::shared_ptr<Incompatibility> learned_incompatibility;
        Term asserted_term;
    };

    ResolutionResult resolve_conflict(std::shared_ptr<Incompatibility> conflict) {
        auto current_incomp = conflict;

        // Count how many terms belong to the current decision level
        auto terms_at_level = [&](const std::shared_ptr<Incompatibility>& incomp, int level) {
            int count = 0;
            for (const auto& term : incomp->terms) {
                if (term_decision_level(term) == level) {
                    ++count;
                }
            }
            return count;
        };

        // Resolution loop until 1 UIP (Unique Implication Point) at current decision level
        while (terms_at_level(current_incomp, decision_level_) > 1) {
            // Find most recent assignment on trail that satisfies a term in current_incomp
            const Assignment* matched_assign = nullptr;
            const Term* matched_term_in_incomp = nullptr;

            for (auto it = trail_.rbegin(); it != trail_.rend(); ++it) {
                if (it->decision_level != decision_level_) continue;
                for (const auto& term : current_incomp->terms) {
                    if (term_decision_level(term) == decision_level_) {
                        if (terms_equal(it->term, term) ||
                            (it->decision_package && term.satisfies(*it->decision_package))) {
                            matched_assign = &(*it);
                            matched_term_in_incomp = &term;
                            break;
                        }
                    }
                }
                if (matched_assign) break;
            }

            if (!matched_assign || !matched_assign->cause) break;

            // Resolve current_incomp with matched_assign->cause
            auto resolved = std::make_shared<Incompatibility>();
            resolved->cause = CauseKind::Derived;
            resolved->previous_cause_a = current_incomp;
            resolved->previous_cause_b = matched_assign->cause;

            std::vector<Term> combined;
            auto add_term = [&](const Term& t) {
                for (const auto& existing : combined) {
                    if (terms_equal(existing, t)) return;
                }
                combined.push_back(t);
            };

            for (const auto& t : current_incomp->terms) {
                if (matched_term_in_incomp && !terms_equal(t, *matched_term_in_incomp)) {
                    add_term(t);
                }
            }
            for (const auto& t : matched_assign->cause->terms) {
                if (matched_term_in_incomp && !terms_opposite(t, *matched_term_in_incomp) && !terms_equal(t, *matched_term_in_incomp)) {
                    add_term(t);
                }
            }
            resolved->terms = std::move(combined);
            current_incomp = resolved;
        }

        // Determine backjump level (highest decision level among all other terms in the learned clause)
        int backjump_level = 0;
        Term asserted_term;
        for (const auto& term : current_incomp->terms) {
            int term_level = term_decision_level(term);
            if (term_level == decision_level_) {
                asserted_term = term.negate();
            } else if (term_level > backjump_level) {
                backjump_level = term_level;
            }
        }

        return ResolutionResult{
            .backjump_level = backjump_level,
            .learned_incompatibility = current_incomp,
            .asserted_term = asserted_term
        };
    }

    /// Rewinds assignments on the trail back to the specified decision level.
    void backtrack(int to_level) {
        while (!trail_.empty() && trail_.back().decision_level > to_level) {
            trail_.pop_back();
        }
        decision_level_ = to_level;
    }

    void add_assignment(const Term& term, std::shared_ptr<Incompatibility> cause,
                        std::optional<package::PackageManifest> decision_pkg = std::nullopt)
    {
        trail_.push_back(Assignment{
            .term = term,
            .decision_level = decision_level_,
            .index = static_cast<int>(trail_.size()),
            .cause = std::move(cause),
            .decision_package = std::move(decision_pkg)
        });
    }

    /// Selects the next positive symbol required by the trail that does not yet have a concrete decision.
    std::optional<Term> choose_next_symbol() const {
        for (const auto& assign : trail_) {
            if (assign.term.is_positive) {
                bool has_decision = false;
                for (const auto& other : trail_) {
                    if (other.term.symbol_name == assign.term.symbol_name && other.decision_package) {
                        has_decision = true;
                        break;
                    }
                }
                if (!has_decision) {
                    return assign.term;
                }
            }
        }
        return std::nullopt;
    }

    /// Finds and ranks matching candidate packages for a symbol requirement.
    std::vector<package::PackageManifest> find_candidates(const Term& req_term) {
        std::vector<package::PackageManifest> candidates;
        const auto& req = req_term.constraint;

        // 1. Direct name match
        if (auto it = by_name_.find(req.name); it != by_name_.end()) {
            for (const auto& pkg : it->second) {
                candidates.push_back(pkg);
            }
        }

        // 2. Sub-Channel toolchain/runtime prefix matching
        if (req.name.starts_with("toolchain/") || req.name.starts_with("runtime/")) {
            std::string prefix = req.name;
            for (const auto& pkg : pool_) {
                if (pkg.channel.starts_with(prefix) || pkg.name == prefix.substr(prefix.find('/') + 1)) {
                    candidates.push_back(pkg);
                }
            }
        }

        // 3. Virtual providers and symbols (e.g. "virtual/init", "so:libc.so.6")
        if (auto it = by_provides_.find(req.name); it != by_provides_.end()) {
            for (const auto& pkg : it->second) {
                candidates.push_back(pkg);
            }
        }

        // Filter candidates against constraints and terms on the trail
        std::vector<package::PackageManifest> filtered;
        for (const auto& cand : candidates) {
            if (!req_term.satisfies(cand)) continue;

            // Check if any terms on the trail contradict this candidate
            bool contradicted = false;
            for (const auto& assign : trail_) {
                if (assign.term.symbol_name == cand.name ||
                    assign.term.symbol_name == req_term.symbol_name) {
                    if (!assign.term.satisfies(cand)) {
                        contradicted = true;
                        break;
                    }
                }
            }
            if (!contradicted) {
                filtered.push_back(cand);
            }
        }

        // Rank candidates:
        // 1. Active provider preference from system.toml
        // 2. Exact package name match > provides
        // 3. For so: sonames, non -dev packages preferred over -dev
        // 4. Highest version wins
        const auto rank = [&](const package::PackageManifest& m) {
            int score = 0;
            if (auto p_it = active_providers_.find(req.name); p_it != active_providers_.end() && m.name == p_it->second) {
                score += 1000;
            }
            if (m.name == req.name) score += 100;
            if (req.name.starts_with("so:") && !m.name.ends_with("-dev")) score += 10;
            return score;
        };

        std::ranges::sort(filtered, [&](const package::PackageManifest& a, const package::PackageManifest& b) {
            int rank_a = rank(a);
            int rank_b = rank(b);
            if (rank_a != rank_b) return rank_a > rank_b;
            if (a.version != b.version) return a.version > b.version;
            return a.name < b.name;
        });

        return filtered;
    }

    /// Makes a decision for the specified required symbol.
    bool decide(const Term& symbol_term) {
        auto candidates = find_candidates(symbol_term);
        if (candidates.empty()) {
            return false;
        }

        // Pick best ranked candidate
        const auto& chosen = candidates.front();
        ++decision_level_;

        // Add decision assignment to the trail
        add_assignment(symbol_term, nullptr, chosen);

        // Add incompatibilities for dependencies of the chosen package:
        // chosen_pkg -> dep <=> { chosen_pkg, NOT dep } is incompatible
        for (const auto& dep : chosen.dependencies) {
            auto dep_incomp = std::make_shared<Incompatibility>();
            dep_incomp->cause = CauseKind::Dependency;
            dep_incomp->custom_reason = std::format(
                "Package '{} {}' depends on '{}'",
                chosen.name, chosen.version.to_string(), dep.to_string());
            
            // Term 1: chosen package
            dep_incomp->terms.push_back(Term{
                .symbol_name = chosen.name,
                .constraint = package::Dependency{.name = chosen.name, .op = package::ConstraintOp::Equal, .version = chosen.version},
                .is_positive = true
            });
            // Term 2: dependency NOT satisfied
            dep_incomp->terms.push_back(Term{
                .symbol_name = dep.name,
                .constraint = dep,
                .is_positive = false
            });
            incompatibilities_.push_back(dep_incomp);
        }

        // Add incompatibilities for conflicts of the chosen package:
        // chosen_pkg conflicts with conf <=> { chosen_pkg, conf } is incompatible
        for (const auto& conf : chosen.conflicts) {
            auto conf_incomp = std::make_shared<Incompatibility>();
            conf_incomp->cause = CauseKind::Conflict;
            conf_incomp->custom_reason = std::format(
                "Package '{} {}' conflicts with '{}'",
                chosen.name, chosen.version.to_string(), conf.to_string());
            // Term 1: chosen package
            conf_incomp->terms.push_back(Term{
                .symbol_name = chosen.name,
                .constraint = package::Dependency{.name = chosen.name, .op = package::ConstraintOp::Equal, .version = chosen.version},
                .is_positive = true
            });
            // Term 2: conflicting symbol
            conf_incomp->terms.push_back(Term{
                .symbol_name = conf.name,
                .constraint = conf,
                .is_positive = true
            });
            incompatibilities_.push_back(conf_incomp);
        }

        return true;
    }

    /// Topologically sort resolved packages in dependency-first install order.
    static std::vector<package::PackageManifest> order_dependencies(
        const std::vector<package::PackageManifest>& solution,
        const std::map<std::string, std::string>& symbol_to_pkg)
    {
        std::map<std::string, const package::PackageManifest*> by_name;
        for (const auto& pkg : solution) {
            by_name[pkg.name] = &pkg;
        }

        std::map<std::string, std::uint8_t> visit_state;
        std::vector<package::PackageManifest> ordered;

        auto visit = [&](this auto&& self, const package::PackageManifest& pkg) -> void {
            auto& state = visit_state[pkg.name];
            if (state == 2) return;
            if (state == 1) return; // Cycle guard: retain deterministic DFS order
            state = 1;
            for (const auto& dep : pkg.dependencies) {
                auto sym_it = symbol_to_pkg.find(dep.name);
                std::string target_pkg_name = (sym_it != symbol_to_pkg.end()) ? sym_it->second : dep.name;
                auto dep_it = by_name.find(target_pkg_name);
                if (dep_it != by_name.end()) {
                    self(*dep_it->second);
                }
            }
            state = 2;
            ordered.push_back(pkg);
        };

        for (const auto& pkg : solution) {
            visit(pkg);
        }
        return ordered;
    }
};

} // namespace sage::solver
