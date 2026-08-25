export module sage.build;

import std;
import sage.package;
import sage.config;

export namespace sage::build {

struct Toolchain {
    std::string cc;
    std::string cxx;
    std::string linker;
    std::string rustc;
    std::string compiler_version;
    std::string cxx_version;
    std::string linker_version;
    std::string rustc_version;
    std::string compiler_family;
    std::string cxx_family;
    std::string linker_family;
    std::string rustc_family;
};

struct BuildPaths {
    std::filesystem::path source;
    std::filesystem::path package;
};

struct BuildStep {
    std::string name;
    std::filesystem::path work_dir;
    std::string command;
};

struct BuildPlan {
    std::map<std::string, std::string> environment;
    std::vector<BuildStep> steps;
};

inline std::string shell_quote(std::string_view value) {
    std::string out{"'"};
    for (char c : value) out += c == '\'' ? "'\\''" : std::string(1, c);
    out += '\'';
    return out;
}

// The execution boundary shared by legacy recipe phases and Sage-managed v2
// steps. Keep the inner command a single shell program so its exports, cwd and
// child build processes all remain in the same fakeroot environment.
inline std::string fakeroot_command(std::string_view executable,
                                    std::string_view command) {
    return shell_quote(executable) + " -- /bin/sh -c " + shell_quote(command);
}

inline std::string tool_family(std::string_view value, bool linker = false) {
    auto base = std::filesystem::path(value).filename().string();
    if (linker) {
        if (base.find("lld") != std::string::npos) return "lld";
        if (base.find("mold") != std::string::npos) return "mold";
        if (base == "ld" || base.find("bfd") != std::string::npos) return "ld";
        return base;
    }
    if (base.find("clang") != std::string::npos) return "clang";
    if (base == "gcc" || base == "g++" || base.starts_with("gcc-")) return "gcc";
    return base;
}

inline std::string join_args(const std::vector<std::string>& values) {
    std::string out;
    for (const auto& value : values)
        out += (out.empty() ? "" : " ") + shell_quote(value);
    return out;
}

inline std::string replace_all(std::string value, std::string_view from,
                               std::string_view to) {
    for (std::size_t at = value.find(from); at != std::string::npos;
         at = value.find(from, at + to.size())) {
        value.replace(at, from.size(), to);
    }
    return value;
}

inline std::expected<void, std::string> validate_toolchain(
    const package::Recipe& recipe, const Toolchain& tools)
{
    const auto& spec = recipe.managed_build;
    const auto check = [&](const package::ToolRequirement& requirement,
                           std::string_view actual_family,
                           std::string_view actual_version,
                           std::string_view kind)
        -> std::expected<void, std::string> {
        if (requirement.family.empty()) return {};
        if (actual_family != requirement.family) return std::unexpected(std::format(
            "recipe defaults to {} family '{}' from package '{}', selected '{}'",
            kind, requirement.family, requirement.package, actual_family));
        if (actual_version.empty() || actual_version == "unknown"
            || package::Version::parse(actual_version)
                < package::Version::parse(requirement.minimum_version)) {
            return std::unexpected(std::format(
                "recipe requires {} package '{}' family '{}' >= {}, selected executable reports {}",
                kind, requirement.package, requirement.family,
                requirement.minimum_version,
                actual_version.empty() ? "an unknown version" : actual_version));
        }
        return {};
    };
    if (auto result = check(spec.compiler,
            tools.compiler_family.empty() ? tool_family(tools.cc)
                                          : tools.compiler_family,
            tools.compiler_version, "compiler"); !result)
        return result;
    if (auto result = check(spec.compiler,
            tools.cxx_family.empty() ? tool_family(tools.cxx)
                                     : tools.cxx_family,
            tools.cxx_version, "C++ compiler"); !result)
        return result;
    if (auto result = check(spec.linker,
                 tools.linker_family.empty() ? tool_family(tools.linker, true)
                                             : tools.linker_family,
                 tools.linker_version, "linker"); !result)
        return result;
    return check(spec.rust,
                 tools.rustc_family.empty() ? tool_family(tools.rustc)
                                            : tools.rustc_family,
                 tools.rustc_version, "Rust compiler");
}

inline std::expected<BuildPlan, std::string> plan_v2(
    const package::Recipe& recipe, const config::BuildConfig& cfg,
    const BuildPaths& paths, const Toolchain& tools, unsigned jobs)
{
    if (recipe.schema_version != 2)
        return std::unexpected("Managed build planning requires recipe schema_version 2");

    const auto& spec = recipe.managed_build;
    if (auto requirement = validate_toolchain(recipe, tools); !requirement)
        return std::unexpected(requirement.error());
    if (spec.system == package::BuildSystem::Cargo
        && (tools.rustc.empty() || tools.rustc_family != "rustc"
            || tools.rustc_version.empty() || tools.rustc_version == "unknown")) {
        return std::unexpected(
            "Cargo recipes require a Sage-configured rustc with a parseable --version result");
    }
    auto safe_relative = [](const std::filesystem::path& path) {
        return !path.is_absolute() && std::ranges::none_of(path, [](const auto& part) {
            return part == "..";
        });
    };
    if (!safe_relative(spec.source_subdir) || !safe_relative(spec.build_dir))
        return std::unexpected(
            "build.source_subdir and build.build_dir must stay inside the source tree");
    for (const auto& patch : spec.patches) {
        if (patch.empty() || std::filesystem::path(patch).filename() != patch)
            return std::unexpected("build.patches entries must be distfiles basenames");
    }
    auto allowed = [](std::string_view selected,
                      const std::vector<std::string>& choices) {
        return choices.empty() || std::ranges::contains(choices, selected);
    };
    const auto compiler = tools.compiler_family.empty()
        ? tool_family(tools.cc) : tools.compiler_family;
    const auto linker = tools.linker_family.empty()
        ? tool_family(tools.linker, true) : tools.linker_family;
    if (!allowed(compiler, spec.allowed_compilers))
        return std::unexpected(std::format(
            "Sage selected compiler '{}' but recipe does not allow it", compiler));
    if (!allowed(linker, spec.allowed_linkers))
        return std::unexpected(std::format(
            "Sage selected linker '{}' but recipe does not allow it", linker));

    BuildPlan plan;
    const auto source = spec.source_subdir.empty()
        ? paths.source : paths.source / spec.source_subdir;
    const auto build = source / spec.build_dir;
    const auto fuse_name = linker == "ld" ? "bfd" : linker;
    const std::string fuse = fuse_name.empty() ? "" : "-fuse-ld=" + fuse_name;
    const std::string cxxflags = cfg.cxxflags.empty() ? cfg.cflags : cfg.cxxflags;
    const std::string ldflags = cfg.ldflags +
        (fuse.empty() ? "" : (cfg.ldflags.empty() ? "" : " ") + fuse);
    const std::string rustflags = cfg.rustflags
        + (cfg.rustflags.empty() ? "" : " ")
        + "-C linker=" + tools.cc
        + (fuse.empty() ? "" : " -C link-arg=" + fuse);

    plan.environment = {
        {"CC", tools.cc}, {"CXX", tools.cxx}, {"LD", tools.linker},
        {"CPPFLAGS", cfg.cppflags}, {"CFLAGS", cfg.cflags},
        {"CXXFLAGS", cxxflags}, {"LDFLAGS", ldflags},
        {"RUSTFLAGS", rustflags}, {"DESTDIR", paths.package.string()},
        {"PREFIX", "/usr"}, {"MAKEFLAGS", std::format("-j{}", jobs)},
        {"CARGO_BUILD_JOBS", std::to_string(jobs)},
    };
    if (spec.system == package::BuildSystem::Cargo)
        plan.environment["RUSTC"] = tools.rustc;
    auto valid_env_name = [](std::string_view name) {
        if (name.empty() || !(std::isalpha(static_cast<unsigned char>(name[0]))
                              || name[0] == '_')) return false;
        return std::ranges::all_of(name.substr(1), [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
        });
    };
    for (const auto* names : {&spec.cflags_env, &spec.cxxflags_env,
                              &spec.cppflags_env, &spec.ldflags_env,
                              &spec.rustflags_env, &spec.cc_env,
                              &spec.cxx_env, &spec.linker_env}) {
        for (const auto& name : *names)
            if (!valid_env_name(name)) return std::unexpected(
                "Invalid build.flag_env variable name: " + name);
    }
    auto aliases = [&](const std::vector<std::string>& names,
                       const std::string& value) {
        for (const auto& name : names) plan.environment[name] = value;
    };
    aliases(spec.cflags_env, cfg.cflags);
    aliases(spec.cxxflags_env, cxxflags);
    aliases(spec.cppflags_env, cfg.cppflags);
    aliases(spec.ldflags_env, ldflags);
    aliases(spec.rustflags_env, rustflags);
    aliases(spec.cc_env, tools.cc);
    aliases(spec.cxx_env, tools.cxx);
    aliases(spec.linker_env, tools.linker);

    const std::set<std::string> managed_names = [&] {
        std::set<std::string> names{
            "CC", "CXX", "LD", "CPPFLAGS", "CFLAGS", "CXXFLAGS",
            "LDFLAGS", "RUSTFLAGS", "DESTDIR", "PREFIX", "MAKEFLAGS",
            "CARGO_BUILD_JOBS",
        };
        if (spec.system == package::BuildSystem::Cargo) names.insert("RUSTC");
        for (const auto* aliases : {&spec.cflags_env, &spec.cxxflags_env,
                                    &spec.cppflags_env, &spec.ldflags_env,
                                    &spec.rustflags_env, &spec.cc_env,
                                    &spec.cxx_env, &spec.linker_env})
            names.insert(aliases->begin(), aliases->end());
        return names;
    }();
    for (const auto& [name, _] : spec.variables) {
        if (!valid_env_name(name))
            return std::unexpected("Invalid build.variables name: " + name);
        if (managed_names.contains(name))
            return std::unexpected(
                "build.variables cannot override Sage-managed channel: " + name);
    }

    auto managed_assignment = [&](std::string_view value) {
        const auto equal = value.find('=');
        return equal != std::string_view::npos
            && managed_names.contains(std::string(value.substr(0, equal)));
    };
    for (const auto* values : {&spec.configure_options, &spec.build_targets,
                               &spec.install_targets}) {
        for (const auto& value : *values) {
            if (managed_assignment(value)) return std::unexpected(
                "Recipe v2 cannot assign a Sage-managed channel: " + value);
        }
    }

    auto step = [&](std::string name, std::filesystem::path cwd,
                    std::string command) {
        plan.steps.push_back({std::move(name), std::move(cwd), std::move(command)});
    };
    for (const auto& patch : spec.patches) {
        step("patch", source, std::format("patch -p{} --forward --input {}",
            spec.patch_strip, shell_quote((paths.source / "distfiles" / patch).string())));
    }

    const auto options = join_args(spec.configure_options);
    const auto build_targets = join_args(spec.build_targets);
    const auto install_targets = join_args(spec.install_targets);
    std::string make_vars;
    for (const auto& [key, raw] : spec.variables) {
        auto value = replace_all(raw, "{destdir}", paths.package.string());
        value = replace_all(std::move(value), "{prefix}", "/usr");
        value = replace_all(std::move(value), "{srcdir}", source.string());
        value = replace_all(std::move(value), "{builddir}", build.string());
        make_vars += std::format(" {}={}", key, shell_quote(value));
    }
    std::string make_managed_vars;
    const auto make_channel = [&](std::string_view name) {
        make_managed_vars += std::format(" {}={}", name,
            shell_quote(plan.environment.at(std::string(name))));
    };
    for (const auto name : {"CC", "CXX", "LD", "CPPFLAGS", "CFLAGS",
                            "CXXFLAGS", "LDFLAGS", "RUSTFLAGS", "DESTDIR",
                            "PREFIX"})
        make_channel(name);
    for (const auto* names : {&spec.cflags_env, &spec.cxxflags_env,
                              &spec.cppflags_env, &spec.ldflags_env,
                              &spec.rustflags_env, &spec.cc_env,
                              &spec.cxx_env, &spec.linker_env})
        for (const auto& name : *names) make_channel(name);

    switch (spec.system) {
        case package::BuildSystem::Autotools:
            for (const auto& option : spec.configure_options) {
                if (option == "--prefix" || option.starts_with("--prefix="))
                    return std::unexpected(
                        "Autotools installation prefix is Sage-managed: " + option);
            }
            step("configure", source, "./configure --prefix=/usr" +
                (options.empty() ? "" : " " + options));
            step("build", source, "make" + make_managed_vars + make_vars +
                (build_targets.empty() ? "" : " " + build_targets));
            step("install", source, "make" + make_managed_vars + make_vars + " DESTDIR=" +
                shell_quote(paths.package.string()) + " " +
                (install_targets.empty() ? "install" : install_targets));
            break;
        case package::BuildSystem::CMake: {
            for (const auto& option : spec.configure_options) {
                if (option.starts_with("-DCMAKE_C_COMPILER")
                    || option.starts_with("-DCMAKE_CXX_COMPILER")
                    || option.starts_with("-DCMAKE_LINKER")
                    || option.starts_with("-DCMAKE_TOOLCHAIN_FILE")
                    || option.starts_with("-DCMAKE_C_FLAGS")
                    || option.starts_with("-DCMAKE_CXX_FLAGS")
                    || option.starts_with("-DCMAKE_EXE_LINKER_FLAGS")
                    || option.starts_with("-DCMAKE_SHARED_LINKER_FLAGS")
                    || option == "-G" || option.starts_with("-G"))
                    return std::unexpected(
                        "CMake compiler, linker, flags and generator are Sage-managed: "
                        + option);
            }
            const auto cmake_arg = [&](std::string name, const std::string& value) {
                return " " + shell_quote("-D" + std::move(name) + "=" + value);
            };
            step("configure", source, "cmake -S . -B " + shell_quote(build.string()) +
                (options.empty() ? "" : " " + options) +
                " -G Ninja -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Release" +
                cmake_arg("CMAKE_C_COMPILER", tools.cc) +
                cmake_arg("CMAKE_CXX_COMPILER", tools.cxx) +
                cmake_arg("CMAKE_LINKER", tools.linker) +
                cmake_arg("CMAKE_C_FLAGS", cfg.cflags) +
                cmake_arg("CMAKE_CXX_FLAGS", cxxflags) +
                cmake_arg("CMAKE_EXE_LINKER_FLAGS", ldflags) +
                cmake_arg("CMAKE_SHARED_LINKER_FLAGS", ldflags));
            step("build", source, "cmake --build " + shell_quote(build.string()) +
                " --parallel " + std::to_string(jobs) +
                (build_targets.empty() ? "" : " --target " + build_targets));
            step("install", source, "DESTDIR=" + shell_quote(paths.package.string()) +
                (install_targets.empty()
                    ? " cmake --install " + shell_quote(build.string())
                    : " cmake --build " + shell_quote(build.string())
                        + " --target " + install_targets));
            break;
        }
        case package::BuildSystem::Meson:
            for (const auto& option : spec.configure_options) {
                if (option == "--native-file" || option.starts_with("--native-file=")
                    || option == "--cross-file" || option.starts_with("--cross-file=")
                    || option == "--backend" || option.starts_with("--backend=")
                    || option == "--prefix" || option.starts_with("--prefix=")
                    || option == "--buildtype" || option.starts_with("--buildtype=")
                    || option.starts_with("-Dc_args=")
                    || option.starts_with("-Dcpp_args=")
                    || option.starts_with("-Dc_link_args=")
                    || option.starts_with("-Dcpp_link_args="))
                    return std::unexpected(
                        "Meson toolchain and compiler/linker arguments are Sage-managed: "
                        + option);
            }
            if (!spec.install_targets.empty()) return std::unexpected(
                "Meson install does not accept build.install_targets");
            step("configure", source, "meson setup " + shell_quote(build.string()) +
                " --prefix=/usr --buildtype=release" +
                (options.empty() ? "" : " " + options));
            step("build", source, "meson compile -C " + shell_quote(build.string()) +
                " -j " + std::to_string(jobs) +
                (build_targets.empty() ? "" : " " + build_targets));
            step("install", source, "DESTDIR=" + shell_quote(paths.package.string()) +
                " meson install -C " + shell_quote(build.string()) + " --no-rebuild");
            break;
        case package::BuildSystem::Xmake:
            for (const auto& option : spec.configure_options) {
                if (option == "--cc" || option.starts_with("--cc=")
                    || option == "--cxx" || option.starts_with("--cxx=")
                    || option == "--ld" || option.starts_with("--ld=")
                    || option == "--toolchain" || option.starts_with("--toolchain=")
                    || option == "--cflags" || option.starts_with("--cflags=")
                    || option == "--cxflags" || option.starts_with("--cxflags=")
                    || option == "--cxxflags" || option.starts_with("--cxxflags=")
                    || option == "--ldflags" || option.starts_with("--ldflags="))
                    return std::unexpected(
                        "Xmake compiler, linker and toolchain are Sage-managed: " + option);
            }
            if (!spec.install_targets.empty()) return std::unexpected(
                "Xmake install does not accept build.install_targets");
            // fakeroot deliberately reports uid 0. Xmake refuses every
            // invocation in that environment unless --root is explicit, so
            // Sage owns this backend requirement on all three steps.
            step("configure", source, "xmake f --root -m release" +
                (options.empty() ? "" : " " + options) + " --cc=" + shell_quote(tools.cc) +
                " --cxx=" + shell_quote(tools.cxx) + " --ld=" +
                // Xmake's ld slot is a compiler driver: a raw GNU ld cannot
                // consume driver options such as -m64 or linked libraries.
                // The probed linker is still selected truthfully by the
                // Sage-owned -fuse-ld flag carried in ldflags.
                shell_quote(tools.cxx) + " --cflags=" + shell_quote(cfg.cflags) +
                " --cxflags=" + shell_quote(cfg.cppflags) +
                " --cxxflags=" + shell_quote(cxxflags) +
                " --ldflags=" + shell_quote(ldflags));
            step("build", source, "xmake -j " + std::to_string(jobs) + " --root" +
                (build_targets.empty() ? "" : " " + build_targets));
            step("install", source, "xmake install --root -o " +
                shell_quote((paths.package / "usr").string()));
            break;
        case package::BuildSystem::Cargo:
            if (!spec.configure_options.empty()) return std::unexpected(
                "Cargo has no configure step; use build_targets/install_targets");
            for (const auto* values : {&spec.build_targets, &spec.install_targets})
                if (std::ranges::contains(*values, "--config"))
                    return std::unexpected(
                        "Cargo --config can override the Sage-managed toolchain");
            step("build", source, "cargo build --release --locked" +
                (build_targets.empty() ? "" : " " + build_targets));
            step("install", source, "cargo install --path . --root " +
                shell_quote((paths.package / "usr").string()) + " --locked --no-track" +
                (install_targets.empty() ? "" : " " + install_targets));
            break;
        case package::BuildSystem::Make:
            if (!spec.configure_options.empty()) return std::unexpected(
                "Make has no configure step; use build.variables and targets");
            step("build", source, "make" + make_managed_vars + make_vars +
                (build_targets.empty() ? "" : " " + build_targets));
            step("install", source, "make" + make_managed_vars + make_vars +
                (install_targets.empty() ? " install" : " " + install_targets));
            break;
        case package::BuildSystem::Legacy:
            return std::unexpected("Recipe v2 cannot use the legacy build system");
    }
    return plan;
}

} // namespace sage::build
