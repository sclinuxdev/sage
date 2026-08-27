module;
#include <cstdint>
#include <sys/stat.h>

export module sage.cli.build:elf;

import std;
import sage;
import sage.vendor.libarchive;
import :toolchain;
import :audit;

namespace sage::cli {

struct ElfScanResult {
    sage::package::PackageManifest manifest;
    std::vector<std::string> external_sonames;
    std::map<std::string, std::vector<std::string>> needed_by;
};

inline std::expected<ElfScanResult, int>
scan_and_validate_elf(const sage::package::Recipe& r,
                      const std::vector<Toolchain>& candidates,
                      const std::optional<ToolAudit>& tool_audit,
                      const std::vector<std::string>& go_command_lines,
                      std::uint64_t go_executions,
                      std::vector<std::string>& managed_cc_parameters,
                      std::vector<std::string>& managed_cxx_parameters,
                      std::vector<std::string>& managed_linker_parameters,
                      std::vector<std::string>& managed_rustc_parameters,
                      const std::filesystem::path& hermetic_root,
                      const std::filesystem::path& pkg_dir,
                      const std::filesystem::path& src_dir,
                      const std::filesystem::path& recipe_dir,
                      bool no_elf_check,
                      const sage::config::BuildConfig& bcfg,
                      const sage::config::SystemConfig& cfg,
                      const std::string& host_arch,
                      const std::string& target_arch,
                      const std::string& host_triplet,
                      const std::string& target_triplet)
{
    // 3. Automated ELF Scanner for DT_SONAME & DT_NEEDED
    sage::package::PackageManifest manifest;
    manifest.schema_version = r.schema_version;
    manifest.name = r.name;
    manifest.version = r.version;
    manifest.description = r.description;
    manifest.license = r.license;
    manifest.channel = r.channel;
    manifest.dependencies = r.host_deps;
    manifest.provides = r.provides;
    manifest.conflicts = r.conflicts;
    manifest.conffiles = r.conffiles;
    manifest.arch = r.arch;
    manifest.capability_hooks = r.capability_hooks;
    manifest.triggers = r.triggers;
    manifest.sysusers = r.sysusers;
    manifest.alternatives = r.alternatives;
    if (r.schema_version == 2) {
        const auto& tools = candidates.front();
        const auto normalize_audit_text = [&](std::string value) {
            return sage::build::replace_all(std::move(value), hermetic_root.string(),
                                            "<sage-build>");
        };
        for (auto command : tool_audit->commands())
            manifest.managed_build_commands.push_back(normalize_audit_text(std::move(command)));
        // The go toolchain has no PATH wrapper; its observed command lines
        // come from the ptrace process log recorded above.
        for (auto& command : go_command_lines)
            manifest.managed_build_commands.push_back(normalize_audit_text(command));
        const auto normalize_parameters = [&](std::vector<std::string>& parameters) {
            for (auto& parameter : parameters)
                parameter = normalize_audit_text(std::move(parameter));
        };
        normalize_parameters(managed_cc_parameters);
        normalize_parameters(managed_cxx_parameters);
        normalize_parameters(managed_linker_parameters);
        normalize_parameters(managed_rustc_parameters);
        const auto add_tool = [&](std::string role, std::string executable,
                                  std::string family, std::string version,
                                  std::vector<std::string> parameters,
                                  std::uint64_t executions) {
            if (executions == 0) return;
            manifest.managed_build_tools.push_back({
                .role = std::move(role), .executable = std::move(executable),
                .family = std::move(family), .version = std::move(version),
                .executions = executions, .parameters = std::move(parameters)});
        };
        if (r.managed_build.system == sage::package::BuildSystem::Cargo) {
            add_tool("linker-driver", tools.cc, tools.compiler_family,
                     tools.compiler_version, managed_linker_parameters,
                     tool_audit->executions("linker-driver"));
            add_tool("linker", tools.linker, tools.linker_family,
                     tools.linker_version, managed_linker_parameters,
                     tool_audit->executions("linker"));
            add_tool("rustc", tools.rustc, tools.rustc_family,
                     tools.rustc_version, managed_rustc_parameters,
                     tool_audit->executions("rustc"));
        } else if (r.managed_build.system == sage::package::BuildSystem::Go) {
            add_tool("go", "go", "go", tools.go_version, {}, go_executions);
            // cgo builds route through the managed C toolchain like any other
            // backend; record those roles when their wrappers executed.
            add_tool("cc", tools.cc, tools.compiler_family,
                     tools.compiler_version, managed_cc_parameters,
                     tool_audit->executions("cc"));
            add_tool("cxx", tools.cxx, tools.cxx_family,
                     tools.cxx_version, managed_cxx_parameters,
                     tool_audit->executions("cxx"));
            add_tool("linker", tools.linker, tools.linker_family,
                     tools.linker_version, managed_linker_parameters,
                     tool_audit->executions("linker"));
        } else {
            add_tool("cc", tools.cc, tools.compiler_family, tools.compiler_version,
                     managed_cc_parameters, tool_audit->executions("cc"));
            add_tool("cxx", tools.cxx, tools.cxx_family, tools.cxx_version,
                     managed_cxx_parameters, tool_audit->executions("cxx"));
            const auto driver = tool_audit->executions("linker-driver");
            add_tool("linker-driver",
                     tool_audit->executions("linker-driver:cc")
                         ? tools.cc : tools.cxx,
                     tool_audit->executions("linker-driver:cc")
                         ? tools.compiler_family : tools.cxx_family,
                     tool_audit->executions("linker-driver:cc")
                         ? tools.compiler_version : tools.cxx_version,
                     managed_linker_parameters, driver);
            add_tool("linker", tools.linker, tools.linker_family,
                     tools.linker_version, managed_linker_parameters,
                     tool_audit->executions("linker"));
        }

        std::string exec_audit_digest;
        if (!manifest.managed_build_commands.empty()) {
            sage::util::Sha256 hasher;
            for (const auto& cmd : manifest.managed_build_commands) {
                hasher.update(cmd.data(), cmd.size());
                hasher.update("\n", 1);
            }
            exec_audit_digest = "sha256:" + hasher.finalize();
        }

        sage::package::BuildAttestation attestation;
        attestation.schema_version = 2;
        const auto epoch_tp = std::chrono::sys_seconds(
            std::chrono::seconds(bcfg.source_date_epoch));
        attestation.built_at = std::format("{:%FT%TZ}", epoch_tp);
        attestation.builder = "sage " SAGE_VERSION;
        attestation.host_arch = host_arch;
        attestation.target_arch = target_arch == "any" ? host_arch : target_arch;
        attestation.host_triplet = host_triplet;
        attestation.target_triplet = target_triplet;
        attestation.check_dependencies = r.check_deps;
        attestation.exec_audit_digest = exec_audit_digest;
        attestation.package = {
            .name = r.name,
            .version = r.version.ver,
            .release = r.version.rel,
            .channel = r.channel,
            .arch = r.arch,
            .sha256 = ""
        };
        if (!r.source_url.empty()) {
            attestation.sources.push_back({
                .url = r.source_url,
                .sha256 = r.source_sha256
            });
        }
        for (const auto& extra : r.extra_sources) {
            attestation.sources.push_back({
                .url = extra.url,
                .sha256 = extra.sha256
            });
        }
        if (tool_audit) {
            for (auto command : tool_audit->commands()) {
                attestation.audit_commands.push_back(normalize_audit_text(std::move(command)));
            }
            for (const auto& tool_entry : manifest.managed_build_tools) {
                sage::package::BuildAttestationTool atool;
                atool.role = tool_entry.role;
                atool.executable = tool_entry.executable;
                atool.family = tool_entry.family;
                atool.version = tool_entry.version;
                atool.executions = tool_entry.executions;
                atool.parameters = tool_entry.parameters;
                if (auto resolved = ToolAudit::resolve(tool_entry.executable)) {
                    atool.path = resolved->string();
                    if (auto h = sage::util::compute_file_sha256(*resolved)) {
                        atool.sha256 = *h;
                    }
                    struct stat st{};
                    if (::stat(resolved->c_str(), &st) == 0) {
                        atool.inode = static_cast<std::uint64_t>(st.st_ino);
                    }
                }
                attestation.tools.push_back(std::move(atool));
            }
        }
        if (bcfg.sysroot != "/" || std::filesystem::exists(cfg.db_path)) {
            auto sysroot_db_path = (bcfg.sysroot != "/")
                ? bcfg.sysroot / "var/lib/sage/packages.lmdb"
                : cfg.db_path;
            if (auto sdb = sage::db::Database::open(sysroot_db_path, true)) {
                if (auto pkgs = sdb->list_installed_packages()) {
                    for (std::size_t i = 0; i < pkgs->size(); ++i) {
                        const auto& pkg = (*pkgs)[i];
                        attestation.sysroot_packages.push_back({
                            .name = pkg.name,
                            .version = pkg.version.ver,
                            .release = pkg.version.rel,
                            .sha256 = pkg.file
                        });
                    }
                }
            }
        }
        manifest.attestation_toml = attestation.serialize_toml();
    }

    // A service.toml beside the recipe declares a daemon: validated here,
    // then carried verbatim on the manifest so `sage rebuild` can regenerate
    // native init scripts for whichever init system is active.
    std::filesystem::path svc_path = recipe_dir / "service.toml";
    std::error_code svc_ec;
    if (std::filesystem::exists(svc_path, svc_ec)) {
        std::ifstream svc_in(svc_path);
        std::ostringstream svc_buf;
        svc_buf << svc_in.rdbuf();
        auto spec = sage::service::ServiceSpec::parse_toml(svc_buf.str());
        if (!spec) {
            sage::util::log_error("Invalid service.toml for '{}': {}", r.name, spec.error());
            return std::unexpected(1);
        }
        manifest.service_toml = svc_buf.str();
    }

    // Every soname this package satisfies by itself, and every soname it still
    // needs from elsewhere -- remembering which file asked, so a failure can
    // name the offender rather than just the missing library.
    std::set<std::string> self_sonames;
    std::set<std::string> needed_sonames;
    std::map<std::string, std::set<std::string>> self_verdefs;
    std::map<std::string, std::set<std::string>> needed_verneeds;
    std::map<std::string, std::vector<std::string>> needed_by;

    if (std::filesystem::exists(pkg_dir)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(pkg_dir, std::filesystem::directory_options::skip_permission_denied)) {
            auto rel = entry.path().lexically_relative(pkg_dir).generic_string();

            // A soname is normally reached through the versioned symlink the
            // package installs next to the real file (libz.so.1 ->
            // libz.so.1.3.1). Skipping symlinks meant those names never made
            // it into `provides`, which is why repository indexes ended up
            // full of so: constraints nothing satisfied.
            if (entry.is_symlink()) {
                auto base = entry.path().filename().string();
                if (base.starts_with("lib") && base.find(".so") != std::string::npos) {
                    self_sonames.insert(base);
                }
                continue;
            }
            if (!entry.is_regular_file()) continue;

            auto base = entry.path().filename().string();
            if (base.starts_with("lib") && base.find(".so") != std::string::npos) {
                self_sonames.insert(base);
            }

            auto elf_res = sage::util::scan_elf(entry.path());
            if (!elf_res) continue;
            for (const auto& rpath : elf_res->rpaths) {
                if (rpath.contains(hermetic_root.string())
                    || rpath.starts_with("/tmp/")
                    || rpath == "/tmp"
                    || rpath.contains(recipe_dir.string())
                    || rpath.contains(src_dir.string())) {
                    sage::util::log_error("ELF binary '{}' contains insecure RPATH: '{}'", rel, rpath);
                    return std::unexpected(1);
                }
            }
            for (const auto& runpath : elf_res->runpaths) {
                if (runpath.contains(hermetic_root.string())
                    || runpath.starts_with("/tmp/")
                    || runpath == "/tmp"
                    || runpath.contains(recipe_dir.string())
                    || runpath.contains(src_dir.string())) {
                    sage::util::log_error("ELF binary '{}' contains insecure RUNPATH: '{}'", rel, runpath);
                    return std::unexpected(1);
                }
            }
            if (!elf_res->soname.empty()) {
                self_sonames.insert(elf_res->soname);
                for (const auto& v : elf_res->verdef_versions) {
                    self_verdefs[elf_res->soname].insert(v);
                }
            }
            for (const auto& needed : elf_res->needed) {
                needed_sonames.insert(needed);
                needed_by[needed].push_back(rel);
            }
            for (const auto& [fname, vname] : elf_res->verneed_entries) {
                needed_verneeds[fname].insert(vname);
            }
        }
    }

    for (const auto& soname : self_sonames) {
        manifest.provides.push_back("so:" + soname);
        if (auto it = self_verdefs.find(soname); it != self_verdefs.end()) {
            for (const auto& v : it->second) {
                manifest.provides.push_back(std::format("so:{}({})", soname, v));
            }
        }
    }

    // A package does not depend on itself: a soname it installs is not an
    // external constraint, and emitting it as one makes the solver chase a
    // cycle through the package being built.
    std::vector<std::string> external_sonames;
    for (const auto& soname : needed_sonames) {
        if (!self_sonames.contains(soname)) external_sonames.push_back(soname);
    }
    for (const auto& soname : external_sonames) {
        manifest.dependencies.push_back(sage::package::Dependency::parse("so:" + soname));
        if (auto it = needed_verneeds.find(soname); it != needed_verneeds.end()) {
            for (const auto& v : it->second) {
                manifest.dependencies.push_back(
                    sage::package::Dependency::parse(std::format("so:{}({})", soname, v)));
            }
        }
    }

    // Deduplicate. The same soname is routinely reached from a dozen binaries
    // in one package, and a repeated provides entry makes the index unreadable.
    {
        std::set<std::string> seen;
        std::erase_if(manifest.provides, [&](const std::string& p) { return !seen.insert(p).second; });
    }
    {
        std::set<std::string> seen;
        std::erase_if(manifest.dependencies, [&](const sage::package::Dependency& d) {
            return !seen.insert(d.to_string()).second;
        });
    }

    // 3b. Validate every remaining DT_NEEDED against what is actually
    // installed. Without this the build happily links against a library that
    // only exists on the build host -- xfsprogs picking up the host's
    // libdevmapper -- and the failure surfaces at install time on a machine
    // that has no such file.
    if (!no_elf_check && !external_sonames.empty()) {
        const auto db_path = (bcfg.sysroot != "/")
            ? bcfg.sysroot / "var/lib/sage/data.mdb"
            : cfg.db_path;
        auto host_db = sage::db::Database::open(db_path, true);
        if (!host_db && bcfg.sysroot != "/") {
            host_db = sage::db::Database::open(cfg.db_path, true);
        }

        if (!host_db) {
            sage::util::log_warn("Cannot verify DT_NEEDED: no package database at '{}'. "
                "{} external soname(s) go unchecked -- expected while bootstrapping, a bug otherwise.",
                db_path.string(), external_sonames.size());
        } else {
            std::vector<std::string> unsatisfied;
            for (const auto& soname : external_sonames) {
                if (host_db->get_provider("so:" + soname)) continue;
                unsatisfied.push_back(soname);
            }
            if (!unsatisfied.empty()) {
                sage::util::log_error("Build linked against {} library/libraries no installed package provides:", unsatisfied.size());
                for (const auto& soname : unsatisfied) {
                    sage::util::log_error("  so:{}  needed by: {}", soname, sage::util::join(needed_by[soname], ", "));
                }
                sage::util::log_error("These came from the build host, not from the repository. Either package them, "
                    "or configure the build to not use them. Pass --no-elf-check to override.");
                return std::unexpected(1);
            }
        }
    }

    return ElfScanResult{
        .manifest = std::move(manifest),
        .external_sonames = std::move(external_sonames),
        .needed_by = std::move(needed_by),
    };
}

} // namespace sage::cli
