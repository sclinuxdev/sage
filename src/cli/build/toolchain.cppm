module;
#include <cstdint>

export module sage.cli.build:toolchain;

import std;
import sage;
import sage.build;
import :probe;

namespace sage::cli {

struct Toolchain {
    std::string cc, cxx, linker, rustc, go;
    std::string cc_for_build, cxx_for_build, linker_for_build, rustc_for_build,
        cc_cache_for_build, cxx_cache_for_build, cache_for_build,
        path_for_build;
    std::string compiler_version, cxx_version, linker_version, rustc_version,
        go_version;
    std::string compiler_family, cxx_family, linker_family, rustc_family,
        go_family;
};

inline std::expected<std::vector<Toolchain>, int>
discover_candidate_toolchains(const sage::package::Recipe& r,
                              const std::filesystem::path& recipe_dir,
                              const sage::config::BuildConfig& bcfg,
                              const std::string& target_triplet,
                              bool verbose)
{
    const bool script_recipe =
        r.schema_version == 2
        && r.managed_build.system == sage::package::BuildSystem::Script
        && !r.managed_build.script_managed_tools;
    const bool needs_managed_toolchain =
        r.schema_version == 2
        && (r.managed_build.system != sage::package::BuildSystem::Script
            || r.managed_build.script_managed_tools);

    std::vector<Toolchain> candidates;
    auto try_candidate = [&](const std::string& cc_name, const std::string& cxx_name,
                             const std::string& linker_name) {
        if (r.schema_version == 2
            && r.managed_build.system == sage::package::BuildSystem::Go) {
            if (std::ranges::any_of(candidates,
                    [](const Toolchain& t) { return !t.go.empty(); }))
                return;
            auto probed_go = probe_tool("go", false, "version");
            if (!probed_go || probed_go->family != "go"
                || probed_go->version == "unknown") {
                sage::util::log_warn(
                    "Go toolchain 'go' is not usable or has no parseable 'go version' output");
                return;
            }
            candidates.push_back(Toolchain{
                .cc = "", .cxx = "", .linker = "", .rustc = "", .go = "go",
                .go_version = std::move(probed_go->version),
                .go_family = std::move(probed_go->family)});
            return;
        }
        if (cc_name.empty() || std::ranges::any_of(candidates, [&](const Toolchain& t) { return t.cc == cc_name; })) {
            return;
        }
        auto compiler = probe_tool(cc_name);
        if (!compiler) {
            sage::util::log_warn("Compiler '{}' not usable, skipping", cc_name);
            return;
        }
        if (r.schema_version == 2) {
            const auto& allowed = r.managed_build.allowed_compilers;
            if (!allowed.empty() && !std::ranges::contains(allowed, compiler->family)) {
                sage::util::log_warn(
                    "Compiler '{}' has family '{}' which is not in recipe allowed_compilers, skipping",
                    cc_name, compiler->family);
                return;
            }
            const auto& req = r.managed_build.compiler;
            if (!req.family.empty() && req.family != compiler->family) {
                sage::util::log_warn(
                    "Compiler '{}' has family '{}' which does not match required family '{}', skipping",
                    cc_name, compiler->family, req.family);
                return;
            }
            if (!req.minimum_version.empty()) {
                const auto min_ver = sage::package::Version::parse(req.minimum_version);
                const auto actual_ver = sage::package::Version::parse(compiler->version);
                if (actual_ver < min_ver) {
                    sage::util::log_warn(
                        "Compiler '{}' version '{}' is below required minimum '{}', skipping",
                        cc_name, compiler->version, req.minimum_version);
                    return;
                }
            }
        }
        ToolProbe cxx{.family = "", .version = ""};
        if (!cxx_name.empty()) {
            if (auto probed = probe_tool(cxx_name)) cxx = std::move(*probed);
            else if (verbose) sage::util::log_info(
                "CXX '{}' probed unsuccessfully; falling back to conservative family inference",
                cxx_name);
        }
        ToolProbe rustc{.family = "", .version = ""};
        if (r.schema_version == 2
            && r.managed_build.system == sage::package::BuildSystem::Cargo) {
            auto probed_rustc = probe_tool(bcfg.rustc);
            if (!probed_rustc || probed_rustc->family != "rustc") {
                sage::util::log_warn(
                    "Cargo build requested, but rustc '{}' is not usable",
                    bcfg.rustc);
                return;
            }
            rustc = std::move(*probed_rustc);
        }
        ToolProbe go{.family = "", .version = ""};
        if (r.schema_version == 2
            && r.managed_build.system == sage::package::BuildSystem::Go) {
            auto probed_go = probe_tool("go", false, "version");
            if (probed_go && probed_go->family == "go")
                go = std::move(*probed_go);
        }
        ToolProbe linker{.family = "", .version = ""};
        if (!linker_name.empty()) {
            if (auto probed = probe_tool(linker_name, true)) linker = std::move(*probed);
            else if (verbose) sage::util::log_info(
                "Linker '{}' probed unsuccessfully; falling back to conservative family inference",
                linker_name);
        }
        if (r.schema_version == 2) {
            const auto& allowed = r.managed_build.allowed_linkers;
            if (!allowed.empty() && !std::ranges::contains(allowed, linker.family)) {
                sage::util::log_warn(
                    "Linker '{}' has family '{}' which is not in recipe allowed_linkers, skipping",
                    linker_name, linker.family);
                return;
            }
            const auto& req = r.managed_build.linker;
            if (!req.family.empty() && req.family != linker.family) {
                sage::util::log_warn(
                    "Linker '{}' has family '{}' which does not match required family '{}', skipping",
                    linker_name, linker.family, req.family);
                return;
            }
            if (!req.minimum_version.empty()) {
                const auto min_ver = sage::package::Version::parse(req.minimum_version);
                const auto actual_ver = sage::package::Version::parse(linker.version);
                if (actual_ver < min_ver) {
                    sage::util::log_warn(
                        "Linker '{}' version '{}' is below required minimum '{}', skipping",
                        linker_name, linker.version, req.minimum_version);
                    return;
                }
            }
        }
        candidates.push_back(Toolchain{
            .cc = cc_name,
            .cxx = cxx_name,
            .linker = linker_name,
            .rustc = r.managed_build.system == sage::package::BuildSystem::Cargo
                ? bcfg.rustc : "",
            .compiler_version = std::move(compiler->version),
            .cxx_version = std::move(cxx.version),
            .linker_version = std::move(linker.version),
            .rustc_version = std::move(rustc.version),
            .go_version = std::move(go.version),
            .compiler_family = std::move(compiler->family),
            .cxx_family = std::move(cxx.family),
            .linker_family = std::move(linker.family),
            .rustc_family = std::move(rustc.family),
            .go_family = std::move(go.family)});
    };

    if (script_recipe) {
        candidates.push_back(Toolchain{});
    } else if (!r.cc.empty() && r.schema_version == 1) {
        const std::string pin_cxx = r.cxx.empty() ? bcfg.cxx : r.cxx;
        auto compiler = probe_tool(r.cc);
        if (!compiler) {
            sage::util::log_error("Recipe pins compiler '{}' but it is not usable", r.cc);
            return std::unexpected(1);
        }
        candidates.push_back(Toolchain{
            .cc = r.cc, .cxx = pin_cxx, .linker = "", .rustc = "",
            .compiler_version = std::move(compiler->version),
            .cxx_version = "", .linker_version = "", .rustc_version = "",
            .compiler_family = std::move(compiler->family),
            .cxx_family = "", .linker_family = "", .rustc_family = ""});
    } else {
        try_candidate(bcfg.cc, bcfg.cxx, bcfg.linker);
        try_candidate(bcfg.fallback_cc, bcfg.fallback_cxx, bcfg.fallback_linker);
    }
    if (candidates.empty()) {
        if (r.schema_version == 2) {
            sage::util::log_warn(
                "No usable C compiler found; managed build cannot continue");
            const auto& compiler = r.managed_build.compiler;
            const auto& linker = r.managed_build.linker;
            if (!compiler.family.empty()) sage::util::log_error(
                "Recipe defaults to compiler package '{}' family '{}' >= {} and linker package '{}' family '{}' >= {}, but no configured Sage toolchain satisfies it",
                compiler.package, compiler.family, compiler.minimum_version,
                linker.package, linker.family, linker.minimum_version);
            sage::util::log_error("No Sage-managed compiler/linker pair satisfies recipe v2");
            return std::unexpected(1);
        }
        candidates.push_back(Toolchain{
            .cc = bcfg.cc, .cxx = bcfg.cxx, .linker = "", .rustc = "",
            .compiler_version = "", .cxx_version = "", .linker_version = "",
            .rustc_version = "", .compiler_family = "", .cxx_family = "",
            .linker_family = "", .rustc_family = ""});
    } else {
        if (needs_managed_toolchain) {
            const auto& selected = candidates.front();
            if (r.managed_build.system == sage::package::BuildSystem::Cargo) {
                sage::util::log_info(
                    "Using managed Rust toolchain: RUSTC='{}' ({}) linker-driver='{}' ({}) LD='{}' ({})",
                    selected.rustc, selected.rustc_version,
                    selected.cc, selected.compiler_version,
                    selected.linker, selected.linker_version);
            } else {
                sage::util::log_info(
                    "Using managed toolchain: CC='{}' ({}) CXX='{}' ({}) LD='{}' ({})",
                    selected.cc, selected.compiler_version,
                    selected.cxx, selected.cxx_version,
                    selected.linker, selected.linker_version);
            }
        }
    }
    return candidates;
}

} // namespace sage::cli
