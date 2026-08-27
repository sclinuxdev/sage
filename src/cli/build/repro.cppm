module;
#include <cstdint>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

export module sage.cli.build:repro;

import std;
import sage;
import sage.vendor.libarchive;
import :toolchain;
import :source;
import :transforms;
import :audit;
import :sandbox;

namespace sage::cli {

struct ReproCleanup {
    std::filesystem::path root;
    ~ReproCleanup() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};
inline std::expected<void, int>
run_reproducibility_check(const sage::package::Recipe& r,
                          const sage::package::PackageManifest& manifest,
                          const std::vector<Toolchain>& candidates,
                          const std::filesystem::path& recipe_dir,
                          const std::filesystem::path& archive_path,
                          const std::vector<std::filesystem::path>& extra_paths,
                          const std::vector<std::pair<std::string, std::filesystem::path>>& created_packages,
                          const std::filesystem::path& target_root,
                          const sage::config::BuildConfig& bcfg,
                          const std::string& target_triplet,
                          const std::string& active_cache_mode,
                          const std::filesystem::path& cache_dir,
                          const std::vector<std::pair<std::filesystem::path, std::string>>& vendor_paths = {})
{
    (void)target_root;
    const int requested_compile_jobs = bcfg.configured_compile_jobs();
    const unsigned compile_jobs = requested_compile_jobs > 0
        ? static_cast<unsigned>(requested_compile_jobs)
        : std::max(1u, std::thread::hardware_concurrency());
            sage::util::log_info("Running second build pass to verify reproducibility (--check-reproducible)...");
        const auto repro_root = std::filesystem::temp_directory_path()
            / std::format("sage-repro-{}", sage::util::current_pid());
        std::error_code repro_ec;
        std::filesystem::create_directories(repro_root, repro_ec);
        ReproCleanup repro_cleanup{repro_root};

        std::filesystem::path repro_home = repro_root / "home";
        std::filesystem::path repro_temp = repro_root / "tmp";
        std::filesystem::path repro_src = repro_root / "src";
        std::filesystem::path repro_pkg = repro_root / "pkg";
        std::filesystem::create_directories(repro_home, repro_ec);
        std::filesystem::create_directories(repro_temp, repro_ec);
        std::filesystem::create_directories(repro_pkg, repro_ec);
        std::filesystem::create_directories(repro_src / "distfiles", repro_ec);

        if (!archive_path.empty()) {
            auto extracted = extract_source_archive(archive_path, repro_src);
            if (!extracted) {
                sage::util::log_error("Reproducibility pass failed to unpack source: {}", extracted.error());
                return std::unexpected(1);
            }
        }
        for (const auto& [vpath, target_rel] : vendor_paths) {
            const auto target_dir = repro_src / target_rel;
            std::filesystem::create_directories(target_dir, repro_ec);
            auto vext = extract_source_archive(vpath, target_dir);
            if (!vext) {
                sage::util::log_error("Reproducibility pass failed to unpack vendor {}: {}", vpath.filename().string(), vext.error());
                return std::unexpected(1);
            }
        }
        for (const auto& path : extra_paths) {
            std::filesystem::copy_file(path, repro_src / "distfiles" / path.filename(),
                std::filesystem::copy_options::overwrite_existing, repro_ec);
        }
        if (r.schema_version == 2 && r.source_url.empty()) {
            for (const auto& entry : std::filesystem::directory_iterator(recipe_dir)) {
                const auto name = entry.path().filename().string();
                if (name == "recipe.toml" || name == "distfiles"
                    || name == "pkg" || name == ".sage-source"
                    || name.ends_with(".pkg.tar.zst")) continue;
                std::filesystem::copy(entry.path(), repro_src / entry.path().filename(),
                    std::filesystem::copy_options::recursive
                    | std::filesystem::copy_options::overwrite_existing, repro_ec);
            }
        }
        if (r.schema_version == 2) {
            for (const auto& patch : r.managed_build.patches_spec) {
                const auto beside_recipe = recipe_dir / patch.file;
                const auto attached = std::filesystem::is_regular_file(beside_recipe, repro_ec)
                    ? beside_recipe : recipe_dir / "distfiles" / patch.file;
                if (!std::filesystem::is_regular_file(attached, repro_ec)) continue;
                std::filesystem::copy_file(attached, repro_src / "distfiles" / patch.file,
                    std::filesystem::copy_options::overwrite_existing, repro_ec);
            }
        }
        std::filesystem::path repro_work =
            (r.schema_version == 2 || !r.source_url.empty()) ? repro_src : recipe_dir;

        std::optional<ToolAudit> repro_tool_audit;
        if (r.schema_version == 2 && !candidates.empty()) {
            auto& selected = candidates.front();
            auto canonical = sage::build::Toolchain{
                .cc = selected.cc, .cxx = selected.cxx, .linker = selected.linker,
                .rustc = selected.rustc, .go = selected.go,
                .target_triplet = target_triplet,
                .compiler_cache_mode = active_cache_mode,
                .compiler_version = selected.compiler_version,
                .cxx_version = selected.cxx_version,
                .linker_version = selected.linker_version,
                .rustc_version = selected.rustc_version,
                .go_version = selected.go_version,
                .compiler_family = selected.compiler_family,
                .cxx_family = selected.cxx_family,
                .linker_family = selected.linker_family,
                .rustc_family = selected.rustc_family,
                .go_family = selected.go_family};
            repro_tool_audit = ToolAudit::create(
                canonical, repro_root, bcfg.sysroot, cache_dir, active_cache_mode);
            if (repro_tool_audit && r.managed_build.network) {
                repro_tool_audit->sandbox = sage::build::replace_all(
                    std::move(repro_tool_audit->sandbox), " --unshare-net", "");
            }
            if (repro_tool_audit && !repro_tool_audit->sandbox.empty()) {
                repro_tool_audit->sandbox += " --ro-bind "
                    + sage::build::shell_quote(recipe_dir.string()) + " "
                    + sage::build::shell_quote(recipe_dir.string())
                    + " --bind " + sage::build::shell_quote(repro_src.string()) + " "
                    + sage::build::shell_quote(repro_src.string())
                    + " --bind " + sage::build::shell_quote(repro_pkg.string()) + " "
                    + sage::build::shell_quote(repro_pkg.string())
                    + " --bind " + sage::build::shell_quote(repro_home.string()) + " "
                    + sage::build::shell_quote(repro_home.string())
                    + " --bind " + sage::build::shell_quote(repro_temp.string()) + " "
                    + sage::build::shell_quote(repro_temp.string());
                if (auto fakeroot_path = ToolAudit::resolve(bcfg.fakeroot)) {
                    std::error_code sysroot_ec;
                    const auto sysroot_root = std::filesystem::weakly_canonical(
                        bcfg.sysroot, sysroot_ec);
                    const auto relative_root = sysroot_ec ? bcfg.sysroot : sysroot_root;
                    const auto namespace_fakeroot = (std::filesystem::path("/")
                        / fakeroot_path->lexically_relative(relative_root)).lexically_normal();
                    const auto namespace_parent = namespace_fakeroot.parent_path();
                    const bool hidden_tmp = namespace_parent.string().starts_with("/tmp/");
                    repro_tool_audit->sandbox += (hidden_tmp
                        ? " --dir " + sage::build::shell_quote(namespace_parent.string()) : "")
                        + " --ro-bind " + sage::build::shell_quote(fakeroot_path->string())
                        + " " + sage::build::shell_quote(namespace_fakeroot.string());
                }
            }
        }

        const Toolchain& cand = candidates.front();
        if (r.schema_version == 2) {
            auto repro_plan = sage::build::plan_v2(
                r, bcfg, {.source = repro_work, .package = repro_pkg,
                           .home = repro_home, .temp = repro_temp},
                {.cc = cand.cc, .cxx = cand.cxx, .linker = cand.linker,
                 .rustc = cand.rustc, .go = cand.go,
                 .target_triplet = target_triplet,
                 .cc_for_build = repro_tool_audit ? repro_tool_audit->cc : cand.cc,
                 .cxx_for_build = repro_tool_audit ? repro_tool_audit->cxx : cand.cxx,
                 .linker_for_build = repro_tool_audit ? repro_tool_audit->linker : cand.linker,
                 .rustc_for_build = repro_tool_audit ? repro_tool_audit->rustc : cand.rustc,
                 .cc_cache_for_build = repro_tool_audit
                     ? repro_tool_audit->cc_cache : cand.cc_cache_for_build,
                 .cxx_cache_for_build = repro_tool_audit
                     ? repro_tool_audit->cxx_cache : cand.cxx_cache_for_build,
                 .cache_for_build = repro_tool_audit
                     ? repro_tool_audit->cache_for_build : cand.cache_for_build,
                 .compiler_cache_mode = active_cache_mode,
                 .path_for_build = repro_tool_audit ? repro_tool_audit->path : cand.path_for_build,
                 .compiler_version = cand.compiler_version,
                 .cxx_version = cand.cxx_version,
                 .linker_version = cand.linker_version,
                 .rustc_version = cand.rustc_version,
                 .go_version = cand.go_version,
                 .compiler_family = cand.compiler_family,
                 .cxx_family = cand.cxx_family,
                 .linker_family = cand.linker_family,
                 .rustc_family = cand.rustc_family,
                 .go_family = cand.go_family}, compile_jobs);
            if (!repro_plan) {
                sage::util::log_error("Reproducibility pass failed to plan: {}", repro_plan.error());
                return std::unexpected(1);
            }
            repro_plan->environment["RECIPE_DIR"] = recipe_dir.string();
            repro_plan->environment["SRCDIR"] = repro_work.string();
            repro_plan->environment["PKGDIR"] = repro_pkg.string();
            std::string exports;
            for (const auto& [name, value] : repro_plan->environment) {
                exports += std::format("export {}={}; ", name,
                    sage::build::shell_quote(value));
            }
            for (const auto& step : repro_plan->steps) {
                const auto full_cmd = exports + "cd "
                    + sage::build::shell_quote(step.work_dir.string())
                    + " && " + step.command;
                const auto configured_fakeroot = ToolAudit::resolve(bcfg.fakeroot)
                    .value_or(std::filesystem::path(bcfg.fakeroot));
                std::error_code sysroot_ec;
                const auto sysroot_root = std::filesystem::weakly_canonical(
                    bcfg.sysroot, sysroot_ec);
                const auto relative_root = sysroot_ec ? bcfg.sysroot : sysroot_root;
                const auto sandbox_fakeroot = (std::filesystem::path("/")
                    / configured_fakeroot.lexically_relative(relative_root)).lexically_normal();
                const auto fakeroot_cmd = repro_tool_audit && !repro_tool_audit->sandbox.empty()
                    ? sandboxed_fakeroot_shell(
                        sandbox_fakeroot.string(),
                        full_cmd,
                        repro_tool_audit->sandbox)
                    : sage::build::fakeroot_command(bcfg.fakeroot,
                        hermetic_shell(full_cmd));
                const auto command_result = ProcessExecAudit::run(
                    fakeroot_cmd, repro_tool_audit->process_exec_log,
                    bcfg.memory_limit, bcfg.pids_limit);
                if (!command_result || *command_result != 0) {
                    sage::util::log_error("Reproducibility pass step failed: {}", step.name);
                    return std::unexpected(1);
                }
            }
        }

        const auto repro_transform_source = repro_src / r.managed_build.source_subdir;
        apply_install_transforms(
            repro_transform_source, repro_pkg, r.managed_build.install_copies,
            r.managed_build.install_symlinks,
            r.managed_build.install_moves,
            r.managed_build.install_removes,
            r.managed_build.install_generates);
        apply_file_permissions(repro_pkg, r.managed_build.file_permissions);
        if (auto repro_content = apply_content_policy(
                repro_pkg, r.managed_build.content); !repro_content) {
            sage::util::log_error("Reproducibility pass content policy failed: {}",
                                  repro_content.error());
            return std::unexpected(1);
        }

        for (const auto& [pkg_name, pass1_file] : created_packages) {
            std::filesystem::path repro_pkg_data = repro_pkg;
            if (!r.managed_build.outputs.empty()) {
                const auto output_spec = std::ranges::find(r.managed_build.outputs,
                    pkg_name, &sage::package::InstallOutput::name);
                if (output_spec != r.managed_build.outputs.end()) {
                    repro_pkg_data = repro_root / ("output-" + pkg_name);
                    std::filesystem::create_directories(repro_pkg_data, repro_ec);
                    std::filesystem::copy(repro_pkg, repro_pkg_data,
                        std::filesystem::copy_options::recursive
                            | std::filesystem::copy_options::copy_symlinks, repro_ec);
                    apply_payload_policy(repro_pkg_data, output_spec->install_files,
                                         output_spec->install_excludes,
                                         output_spec->optional_excludes);
                    apply_install_transforms(
                        repro_transform_source, repro_pkg_data,
                        output_spec->install_copies,
                        output_spec->install_symlinks,
                        output_spec->install_moves,
                        output_spec->install_removes,
                        output_spec->install_generates);
                    apply_file_permissions(repro_pkg_data, output_spec->file_permissions);
                    if (auto repro_sysusers = apply_sysusers_fragment(
                            repro_pkg_data, pkg_name, r.sysusers); !repro_sysusers) {
                        sage::util::log_error(
                            "Reproducibility pass sysusers staging failed: {}",
                            repro_sysusers.error());
                        return std::unexpected(1);
                    }
                }
            } else {
                apply_payload_policy(repro_pkg_data, r.managed_build.install_files,
                                     r.managed_build.install_excludes,
                                     r.managed_build.optional_excludes);
                if (auto repro_sysusers = apply_sysusers_fragment(
                        repro_pkg_data, pkg_name, r.sysusers); !repro_sysusers) {
                    sage::util::log_error(
                        "Reproducibility pass sysusers staging failed: {}",
                        repro_sysusers.error());
                    return std::unexpected(1);
                }
            }
            std::filesystem::path repro_pkg_file = repro_root / pass1_file.filename();
            auto pack_res = sage::archive::create_package(manifest, repro_pkg_data, repro_pkg_file);
            if (!pack_res) {
                sage::util::log_error("Reproducibility check packaging failed: {}", pack_res.error());
                return std::unexpected(1);
            }
            auto hash1 = sage::util::compute_file_sha256(pass1_file);
            auto hash2 = sage::util::compute_file_sha256(repro_pkg_file);
            if (!hash1 || !hash2 || *hash1 != *hash2) {
                sage::util::log_error("Reproducibility check failed for package '{}':\n  Pass 1 SHA256: {}\n  Pass 2 SHA256: {}",
                    pkg_name,
                    hash1 ? *hash1 : "error",
                    hash2 ? *hash2 : "error");
                return std::unexpected(1);
            }
            sage::util::log_success("Reproducibility check passed for package '{}' (SHA256: {})",
                pkg_name, *hash1);
        }
        return {};
    }
} // namespace sage::cli
