module;
#include <cstdint>

export module sage.cli.build:pack;

import std;
import sage;
import sage.vendor.libarchive;
import :toolchain;
import :transforms;
import :elf;

namespace sage::cli {

inline std::expected<std::vector<std::pair<std::string, std::filesystem::path>>, int>
pack_build_outputs(const sage::package::Recipe& r,
                   const ElfScanResult& elf_result,
                   const std::filesystem::path& hermetic_root,
                   const std::filesystem::path& pkg_dir,
                   const std::filesystem::path& src_dir,
                   const std::filesystem::path& recipe_dir,
                   const std::vector<std::string>& managed_cc_parameters,
                   const std::vector<std::string>& managed_cxx_parameters,
                   const std::vector<std::string>& managed_linker_parameters,
                   const std::vector<std::string>& managed_rustc_parameters,
                   const std::filesystem::path& target_root)
{
    const auto& manifest = elf_result.manifest;
    const auto& external_sonames = elf_result.external_sonames;
    const auto& needed_by = elf_result.needed_by;
    // 4. Archive creation.  A v2 recipe may name several output views of the
    // same install tree.  Each view is copied before filtering, so one output
    // cannot delete a file another output owns; every archive receives its own
    // ELF-provided sonames and dependencies.
    struct OutputPath {
        std::string name;
        std::filesystem::path data;
    };
    std::vector<OutputPath> output_paths;
    if (r.schema_version == 2 && !r.managed_build.outputs.empty()) {
        for (const auto& output : r.managed_build.outputs) {
            const auto data = hermetic_root / ("output-" + output.name);
            std::error_code output_ec;
            std::filesystem::remove_all(data, output_ec);
            std::filesystem::copy(pkg_dir, data,
                std::filesystem::copy_options::recursive
                    | std::filesystem::copy_options::copy_symlinks, output_ec);
            if (output_ec) {
                sage::util::log_error("Cannot clone payload for output '{}': {}",
                                      output.name, output_ec.message());
                return std::unexpected(1);
            }
            auto payload = apply_payload_policy(data, output.install_files,
                                                 output.install_excludes,
                                                 output.optional_excludes);
            if (!payload) {
                sage::util::log_error("Invalid payload for output '{}': {}",
                                      output.name, payload.error());
                return std::unexpected(1);
            }
            const auto transform_source = src_dir / r.managed_build.source_subdir;
            auto output_transforms = apply_install_transforms(
                transform_source, data,
                output.install_copies,
                output.install_symlinks,
                output.install_moves,
                output.install_removes,
                output.install_generates);
            if (!output_transforms) {
                sage::util::log_error("Invalid install transform for output '{}': {}",
                                      output.name, output_transforms.error());
                return std::unexpected(1);
            }
            auto output_perms = apply_file_permissions(data, output.file_permissions);
            if (!output_perms) {
                sage::util::log_error("Invalid file permissions for output '{}': {}",
                                      output.name, output_perms.error());
                return std::unexpected(1);
            }
            auto output_sysusers = apply_sysusers_fragment(
                data, output.name, r.sysusers);
            if (!output_sysusers) {
                sage::util::log_error("Cannot stage sysusers for output '{}': {}",
                                      output.name, output_sysusers.error());
                return std::unexpected(1);
            }
            output_paths.push_back({output.name, data});
        }
    } else {
        output_paths.push_back({r.name, pkg_dir});
    }

    // Exhaustiveness check in outputs mode
    if (r.schema_version == 2 && (r.managed_build.payload == sage::package::PayloadMode::Outputs || !r.managed_build.outputs.empty())) {
        std::error_code pkg_ec;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(
                 pkg_dir, std::filesystem::directory_options::skip_permission_denied, pkg_ec)) {
            if (pkg_ec) {
                sage::util::log_error("Cannot inspect staged payload: {}", pkg_ec.message());
                return std::unexpected(1);
            }
            if (entry.is_directory(pkg_ec) && !entry.is_symlink(pkg_ec)) continue;
            const auto rel = entry.path().lexically_relative(pkg_dir).generic_string();
            bool claimed = false;
            for (std::size_t out_idx = 0; out_idx < output_paths.size(); ++out_idx) {
                const auto& out = output_paths[out_idx];
                const auto out_file = out.data / rel;
                std::error_code check_ec;
                if (std::filesystem::exists(out_file, check_ec) || std::filesystem::is_symlink(out_file, check_ec)) {
                    claimed = true;
                    break;
                }
                if (out_idx < r.managed_build.outputs.size()) {
                    const auto& out_spec = r.managed_build.outputs[out_idx];
                    for (const auto& move : out_spec.install_moves) {
                        if (move.source == rel || std::filesystem::path(move.source) == std::filesystem::path(rel)) {
                            claimed = true;
                            break;
                        }
                    }
                    if (claimed) break;
                    for (const auto& rm_pat : out_spec.install_removes) {
                        if (sage::util::glob_match(rm_pat.path, rel)) {
                            claimed = true;
                            break;
                        }
                    }
                    if (claimed) break;
                }
            }
            if (!claimed) {
                sage::util::log_error("Unassigned payload path '{}' was not claimed by any output", rel);
                return std::unexpected(1);
            }
        }
    }

    if (output_paths.size() > 1) {
        std::map<std::string, std::string> owners;
        for (const auto& output : output_paths) {
            std::error_code output_ec;
            for (const auto& entry : std::filesystem::recursive_directory_iterator(
                     output.data, std::filesystem::directory_options::skip_permission_denied,
                     output_ec)) {
                if (output_ec) {
                    sage::util::log_error("Cannot inspect output '{}': {}",
                                          output.name, output_ec.message());
                    return std::unexpected(1);
                }
                if (entry.is_directory(output_ec) && !entry.is_symlink(output_ec)) continue;
                const auto rel = entry.path().lexically_relative(output.data).generic_string();
                const auto [it, inserted] = owners.emplace(rel, output.name);
                if (!inserted) {
                    sage::util::log_error(
                        "Outputs '{}' and '{}' both claim payload path '{}'",
                        it->second, output.name, rel);
                    return std::unexpected(1);
                }
            }
        }
    }

    std::vector<std::pair<std::string, std::filesystem::path>> created_packages;

    for (const auto& output : output_paths) {
        auto output_manifest = manifest;
        output_manifest.name = output.name;
        if (!r.managed_build.outputs.empty()) {
            const auto output_spec = std::ranges::find(r.managed_build.outputs,
                output.name, &sage::package::InstallOutput::name);
            if (output_spec == r.managed_build.outputs.end()) {
                sage::util::log_error("Output '{}' has no recipe metadata", output.name);
                return std::unexpected(1);
            }
            output_manifest.description = output_spec->description.value_or(r.description);
            output_manifest.license = output_spec->license.value_or(r.license);
            if (output_spec->version) {
                output_manifest.version = sage::package::Version::parse(*output_spec->version);
            } else {
                output_manifest.version = r.version;
            }
            if (output_spec->release) {
                output_manifest.version.rel = *output_spec->release;
            }
            if (output_spec->channel) {
                output_manifest.channel = *output_spec->channel;
            } else {
                output_manifest.channel = r.channel;
            }
            if (output_spec->arch) {
                output_manifest.arch = *output_spec->arch;
            } else {
                output_manifest.arch = r.arch;
            }
            const auto inherits = [&](std::string_view key) {
                return std::ranges::contains(output_spec->inherit, key);
            };
            if (output_spec->dependencies) {
                output_manifest.dependencies = *output_spec->dependencies;
            } else if (inherits("dependencies")) {
                output_manifest.dependencies = r.host_deps;
            } else {
                output_manifest.dependencies.clear();
            }

            if (output_spec->provides) {
                output_manifest.provides = *output_spec->provides;
            } else if (inherits("provides")) {
                output_manifest.provides = r.provides;
            } else {
                output_manifest.provides.clear();
            }

            if (output_spec->conflicts) {
                output_manifest.conflicts = *output_spec->conflicts;
            } else if (inherits("conflicts")) {
                output_manifest.conflicts = r.conflicts;
            } else {
                output_manifest.conflicts.clear();
            }

            if (output_spec->conffiles) {
                output_manifest.conffiles = *output_spec->conffiles;
            } else if (inherits("conffiles")) {
                output_manifest.conffiles = r.conffiles;
            } else {
                output_manifest.conffiles.clear();
            }
            output_manifest.files.clear();
            const auto& perms_list = !output_spec->file_permissions.empty()
                ? output_spec->file_permissions
                : r.managed_build.file_permissions;
            for (const auto& perm : perms_list) {
                sage::package::FileEntry fe;
                fe.path = perm.path;
                fe.mode = perm.mode;
                fe.uid = perm.uid;
                fe.gid = perm.gid;
                fe.caps = perm.caps;
                output_manifest.files.push_back(std::move(fe));
            }

            if (!manifest.attestation_toml.empty()) {
                auto parsed_att = sage::package::BuildAttestation::parse_toml(manifest.attestation_toml);
                if (parsed_att) {
                    parsed_att->package.name = output_manifest.name;
                    parsed_att->package.version = output_manifest.version.ver;
                    parsed_att->package.release = output_manifest.version.rel;
                    parsed_att->package.channel = output_manifest.channel;
                    parsed_att->package.arch = output_manifest.arch;
                    output_manifest.attestation_toml = parsed_att->serialize_toml();
                }
            }

            std::set<std::string> output_self_sonames;
            std::set<std::string> output_needed_sonames;
            std::map<std::string, std::set<std::string>> out_self_verdefs;
            std::map<std::string, std::set<std::string>> out_needed_verneeds;
            for (const auto& entry : std::filesystem::recursive_directory_iterator(output.data)) {
                if (entry.is_symlink()) {
                    const auto base = entry.path().filename().string();
                    if (base.starts_with("lib") && base.find(".so") != std::string::npos)
                        output_self_sonames.insert(base);
                    continue;
                }
                if (!entry.is_regular_file()) continue;
                const auto base = entry.path().filename().string();
                if (base.starts_with("lib") && base.find(".so") != std::string::npos)
                    output_self_sonames.insert(base);
                auto elf = sage::util::scan_elf(entry.path());
                if (!elf) continue;
                for (const auto& rpath : elf->rpaths) {
                    if (rpath.contains(hermetic_root.string())
                        || rpath.starts_with("/tmp/")
                        || rpath == "/tmp"
                        || rpath.contains(recipe_dir.string())
                        || rpath.contains(src_dir.string())) {
                        sage::util::log_error("ELF binary in output '{}' contains insecure RPATH: '{}'",
                                              output.name, rpath);
                        return std::unexpected(1);
                    }
                }
                for (const auto& runpath : elf->runpaths) {
                    if (runpath.contains(hermetic_root.string())
                        || runpath.starts_with("/tmp/")
                        || runpath == "/tmp"
                        || runpath.contains(recipe_dir.string())
                        || runpath.contains(src_dir.string())) {
                        sage::util::log_error("ELF binary in output '{}' contains insecure RUNPATH: '{}'",
                                              output.name, runpath);
                        return std::unexpected(1);
                    }
                }
                if (!elf->soname.empty()) {
                    output_self_sonames.insert(elf->soname);
                    for (const auto& v : elf->verdef_versions) {
                        out_self_verdefs[elf->soname].insert(v);
                    }
                }
                output_needed_sonames.insert(elf->needed.begin(), elf->needed.end());
                for (const auto& [fname, vname] : elf->verneed_entries) {
                    out_needed_verneeds[fname].insert(vname);
                }
            }
            for (const auto& soname : output_self_sonames) {
                output_manifest.provides.push_back("so:" + soname);
                if (auto it = out_self_verdefs.find(soname); it != out_self_verdefs.end()) {
                    for (const auto& v : it->second) {
                        output_manifest.provides.push_back(std::format("so:{}({})", soname, v));
                    }
                }
            }
            for (const auto& soname : output_needed_sonames) {
                if (!output_self_sonames.contains(soname)) {
                    output_manifest.dependencies.push_back(
                        sage::package::Dependency::parse("so:" + soname));
                    if (auto it = out_needed_verneeds.find(soname); it != out_needed_verneeds.end()) {
                        for (const auto& v : it->second) {
                            output_manifest.dependencies.push_back(
                                sage::package::Dependency::parse(std::format("so:{}({})", soname, v)));
                        }
                    }
                }
            }
            std::set<std::string> seen_provides;
            std::erase_if(output_manifest.provides, [&](const std::string& value) {
                return !seen_provides.insert(value).second;
            });
            std::set<std::string> seen_dependencies;
            std::erase_if(output_manifest.dependencies,
                [&](const sage::package::Dependency& value) {
                    return !seen_dependencies.insert(value.to_string()).second;
                });
        } else {
            output_manifest.files.clear();
            for (const auto& perm : r.managed_build.file_permissions) {
                sage::package::FileEntry fe;
                fe.path = perm.path;
                fe.mode = perm.mode;
                fe.uid = perm.uid;
                fe.gid = perm.gid;
                fe.caps = perm.caps;
                output_manifest.files.push_back(std::move(fe));
            }
        }
        std::string out_name = std::format("{}-{}-{}-{}.pkg.tar.zst",
            output_manifest.name, output_manifest.version.ver, output_manifest.version.rel, output_manifest.arch);
        std::filesystem::path out_path = recipe_dir / out_name;
        auto pack_res = sage::archive::create_package(output_manifest, output.data, out_path);
        if (!pack_res) {
            sage::util::log_error("Package '{}' packaging failed: {}",
                                  output.name, pack_res.error());
            return std::unexpected(1);
        }
        sage::util::log_success("Package built successfully: {}", out_path.string());
        created_packages.push_back({output.name, out_path});
    }


    return created_packages;
}

} // namespace sage::cli
