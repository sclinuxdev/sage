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
    // The first four fields are the canonical executables Sage probed.  The
    // *_for_build fields are deliberately separate: during a build Sage may
    // substitute an audit wrapper, while manifests continue to name the
    // executable whose --version was actually inspected.
    std::string cc_for_build;
    std::string cxx_for_build;
    std::string linker_for_build;
    std::string rustc_for_build;
    std::string path_for_build;
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
    std::filesystem::path home;
    std::filesystem::path temp;
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

// A recipe may refine a canonical FHS subdirectory (for example a package's
// documentation directory), but it may not redirect installation into an
// arbitrary prefix. Sage appends its own canonical prefix/libdir after these
// options, and this predicate keeps every accepted refinement inside the
// corresponding system root.
inline bool canonical_autotools_install_option(std::string_view option) {
    static constexpr std::array<std::string_view, 22> names{
        "--prefix", "--exec-prefix", "--bindir", "--sbindir", "--libexecdir",
        "--libdir", "--includedir", "--oldincludedir", "--datarootdir", "--datadir",
        "--infodir", "--localedir", "--mandir", "--docdir", "--htmldir", "--dvidir",
        "--pdfdir", "--psdir", "--sysconfdir", "--sharedstatedir", "--localstatedir",
        "--runstatedir"};
    const auto equal = option.find('=');
    if (equal == std::string_view::npos) return false;
    const auto key = option.substr(0, equal);
    const auto value = option.substr(equal + 1);
    if (!std::ranges::contains(names, key) || value.empty()) return false;
    const std::filesystem::path path(value);
    if (!path.is_absolute() || path.has_root_name()
        || std::ranges::any_of(path, [](const auto& part) { return part == ".."; }))
        return false;
    const auto text = path.lexically_normal().generic_string();
    const auto exact = [&](std::string_view root) { return text == root; };
    const auto under = [&](std::string_view root) {
        return text == root || text.starts_with(std::string(root) + "/");
    };
    if (key == "--prefix" || key == "--exec-prefix") return exact("/usr");
    if (key == "--bindir") return exact("/usr/bin");
    if (key == "--sbindir") return exact("/usr/sbin") || exact("/usr/bin");
    if (key == "--libexecdir") return exact("/usr/libexec") || exact("/usr/lib");
    if (key == "--libdir") return exact("/usr/lib");
    if (key == "--includedir" || key == "--oldincludedir") return exact("/usr/include");
    if (key == "--datarootdir") return exact("/usr/share");
    if (key == "--datadir") return under("/usr/share");
    if (key == "--infodir") return under("/usr/share/info");
    if (key == "--localedir") return under("/usr/share/locale");
    if (key == "--mandir") return under("/usr/share/man");
    if (key == "--docdir" || key == "--htmldir" || key == "--dvidir"
        || key == "--pdfdir" || key == "--psdir") return under("/usr/share/doc");
    if (key == "--sysconfdir") return exact("/etc");
    if (key == "--sharedstatedir") return under("/var/lib");
    if (key == "--localstatedir") return under("/var");
    if (key == "--runstatedir") return under("/run");
    return false;
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
    if (spec.kernel && spec.system != package::BuildSystem::Make)
        return std::unexpected(
            "build.kernel=true requires the Make/Kbuild backend");
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
    const auto cc_exec = tools.cc_for_build.empty() ? tools.cc : tools.cc_for_build;
    const auto cxx_exec = tools.cxx_for_build.empty() ? tools.cxx : tools.cxx_for_build;
    const auto linker_exec = tools.linker_for_build.empty() ? tools.linker : tools.linker_for_build;
    const auto rustc_exec = tools.rustc_for_build.empty() ? tools.rustc : tools.rustc_for_build;
    const auto source = spec.source_subdir.empty()
        ? paths.source : paths.source / spec.source_subdir;
    const auto build = source / spec.build_dir;
    const auto fuse_name = linker == "ld" ? "bfd" : linker;
    const std::string fuse = fuse_name.empty() ? "" : "-fuse-ld=" + fuse_name;
    // GCC/Clang normally resolve the backend linker through an absolute
    // collect2 path. `-B` places Sage's logging shim ahead of that lookup,
    // turning the selected backend into an observed child execution.
    const auto audit_root = tools.path_for_build.substr(
        0, tools.path_for_build.find(':'));
    const std::string audit_prefix = tools.path_for_build.empty()
        ? "" : " -B" + audit_root;
    const std::string cxxflags = cfg.cxxflags.empty() ? cfg.cflags : cfg.cxxflags;
    const std::string ldflags = cfg.ldflags +
        (fuse.empty() ? "" : (cfg.ldflags.empty() ? "" : " ") + fuse)
        + audit_prefix;
    const std::string rustflags = cfg.rustflags
        + (cfg.rustflags.empty() ? "" : " ")
        + "-C linker=" + cc_exec
        + (fuse.empty() ? "" : " -C link-arg=" + fuse)
        + (tools.path_for_build.empty() ? ""
            : " -C link-arg=-B" + audit_root);

    plan.environment = {
        {"CC", cc_exec}, {"CXX", cxx_exec}, {"LD", linker_exec},
        {"CPPFLAGS", cfg.cppflags}, {"CFLAGS", cfg.cflags},
        {"CXXFLAGS", cxxflags}, {"LDFLAGS", ldflags},
        {"RUSTFLAGS", rustflags}, {"DESTDIR", paths.package.string()},
        {"PREFIX", "/usr"}, {"MAKEFLAGS", std::format("-j{}", jobs)},
        {"CARGO_BUILD_JOBS", std::to_string(jobs)},
        {"LC_ALL", "C"}, {"LANG", "C"}, {"TZ", "UTC"},
        {"SOURCE_DATE_EPOCH", std::to_string(cfg.source_date_epoch)},
        {"FORCE_SOURCE_DATE", "1"}, {"PYTHONHASHSEED", "0"},
        {"ARFLAGS", "crD"}, {"ZERO_AR_DATE", "1"},
        {"CARGO_INCREMENTAL", "0"}, {"CARGO_TERM_COLOR", "never"},
        {"DEBUGINFOD_URLS", ""},
        {"GIT_CONFIG_NOSYSTEM", "1"}, {"GIT_CONFIG_GLOBAL", "/dev/null"},
        {"TERM", "dumb"}, {"SHELL", "/bin/sh"},
        {"USER", "builder"}, {"LOGNAME", "builder"}, {"PAGER", "cat"},
    };
    if (spec.kernel) {
        // Kbuild has its own flag channels. Keep the source of truth in
        // build.toml: compiler flags become KCFLAGS (and the corresponding
        // preprocessor/linker/Rust channels), while LLVM=1 is derived only
        // from the compiler Sage actually selected and probed.
        plan.environment["KCFLAGS"] = cfg.cflags;
        plan.environment["KCPPFLAGS"] = cfg.cppflags;
        plan.environment["KBUILD_LDFLAGS"] = cfg.ldflags;
        plan.environment["KRUSTFLAGS"] = cfg.rustflags;
        if (compiler == "clang") plan.environment["LLVM"] = "1";
    }
    if (!paths.home.empty()) plan.environment["HOME"] = paths.home.string();
    if (!paths.temp.empty()) plan.environment["TMPDIR"] = paths.temp.string();
    if (!paths.home.empty()) {
        plan.environment["CARGO_HOME"] = (paths.home / ".cargo").string();
        plan.environment["RUSTUP_HOME"] = (paths.home / ".rustup").string();
        plan.environment["XDG_CONFIG_HOME"] = (paths.home / ".config").string();
    }
    if (!tools.path_for_build.empty()) plan.environment["PATH"] = tools.path_for_build;
    else plan.environment["PATH"] = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
    if (spec.system == package::BuildSystem::Cargo)
        plan.environment["RUSTC"] = rustc_exec;
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
    aliases(spec.cc_env, cc_exec);
    aliases(spec.cxx_env, cxx_exec);
    aliases(spec.linker_env, linker_exec);

    const std::set<std::string> managed_names = [&] {
        std::set<std::string> names{
            "CC", "CXX", "LD", "CPPFLAGS", "CFLAGS", "CXXFLAGS",
            "LDFLAGS", "RUSTFLAGS", "DESTDIR", "PREFIX", "MAKEFLAGS",
            "CARGO_BUILD_JOBS", "LC_ALL", "LANG", "TZ", "SOURCE_DATE_EPOCH",
            "FORCE_SOURCE_DATE", "PYTHONHASHSEED", "ARFLAGS", "ZERO_AR_DATE",
            "CARGO_INCREMENTAL", "CARGO_TERM_COLOR", "DEBUGINFOD_URLS",
            "GIT_CONFIG_NOSYSTEM", "GIT_CONFIG_GLOBAL", "HOME", "TMPDIR",
            "CARGO_HOME", "RUSTUP_HOME", "XDG_CONFIG_HOME",
        };
        if (spec.system == package::BuildSystem::Cargo) names.insert("RUSTC");
        if (spec.kernel) {
            names.insert("LLVM");
            names.insert("KCFLAGS");
            names.insert("KCPPFLAGS");
            names.insert("KBUILD_LDFLAGS");
            names.insert("KRUSTFLAGS");
        }
        names.insert("PATH");
        for (const auto* aliases : {&spec.cflags_env, &spec.cxxflags_env,
                                    &spec.cppflags_env, &spec.ldflags_env,
                                    &spec.rustflags_env, &spec.cc_env,
                                    &spec.cxx_env, &spec.linker_env})
            names.insert(aliases->begin(), aliases->end());
        return names;
    }();
    static constexpr std::array<std::string_view, 23> install_variable_names{
        "prefix", "exec_prefix", "bindir", "sbindir", "libexecdir", "libdir",
        "includedir", "oldincludedir", "datarootdir", "datadir", "infodir",
        "localedir", "mandir", "docdir", "htmldir", "dvidir", "pdfdir", "psdir",
        "sysconfdir", "sharedstatedir", "localstatedir", "runstatedir", "DESTDIR"};
    for (const auto& [name, _] : spec.variables) {
        if (!valid_env_name(name))
            return std::unexpected("Invalid build.variables name: " + name);
        if (managed_names.contains(name))
            return std::unexpected(
                "build.variables cannot override Sage-managed channel: " + name);
        if (std::ranges::contains(install_variable_names, name))
            return std::unexpected(
                "build.variables cannot override Sage-managed install directory: " + name);
    }

    auto managed_assignment = [&](std::string_view value) {
        const auto equal = value.find('=');
        if (equal == std::string_view::npos) return false;
        const auto name = value.substr(0, equal);
        return managed_names.contains(std::string(name))
            || std::ranges::contains(install_variable_names, name);
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
    const auto custom_cwd = [&](std::string_view cwd) -> std::filesystem::path {
        if (cwd == "package") return paths.package;
        if (cwd == "build") return build;
        return source;
    };
    const auto custom_steps = [&](std::string_view phase) {
        for (const auto& custom : spec.steps) {
            if (custom.phase != phase) continue;
            step("custom-" + custom.name, custom_cwd(custom.cwd), custom.command);
        }
    };
    for (const auto& patch : spec.patches) {
        step("patch", source, std::format("patch -p{} --forward --input {}",
            spec.patch_strip, shell_quote((paths.source / "distfiles" / patch).string())));
    }
    // Source archives carry upstream mtimes and patches/create steps otherwise
    // inherit the wall clock. Normalize the entire input tree before any
    // backend sees it; package archives already use the same epoch for their
    // tar members.
    step("normalize-source", source,
        "find " + shell_quote(paths.source.string())
        + " -depth -exec touch -h -d "
        + shell_quote("@" + std::to_string(cfg.source_date_epoch)) + " {} +");
    custom_steps("prepare");

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
    std::set<std::string> emitted_make_channels;
    const auto make_channel = [&](std::string_view name) {
        if (!emitted_make_channels.insert(std::string(name)).second) return;
        const auto value = plan.environment.find(std::string(name));
        if (value == plan.environment.end()) return;
        make_managed_vars += std::format(" {}={}", name,
            shell_quote(value->second));
    };
    for (const auto name : {"CC", "CXX", "LD", "CPPFLAGS", "CFLAGS",
                            "CXXFLAGS", "LDFLAGS", "RUSTFLAGS", "DESTDIR",
                            "PREFIX"})
        make_channel(name);
    if (spec.kernel) {
        make_channel("LLVM");
        make_channel("KCFLAGS");
        make_channel("KCPPFLAGS");
        make_channel("KBUILD_LDFLAGS");
        make_channel("KRUSTFLAGS");
    }
    for (const auto* names : {&spec.cflags_env, &spec.cxxflags_env,
                              &spec.cppflags_env, &spec.ldflags_env,
                              &spec.rustflags_env, &spec.cc_env,
                              &spec.cxx_env, &spec.linker_env})
        for (const auto& name : *names) make_channel(name);

    switch (spec.system) {
        case package::BuildSystem::Autotools: {
            for (const auto& option : spec.configure_options) {
                static constexpr std::array<std::string_view, 22> install_options{
                    "--prefix", "--exec-prefix", "--bindir", "--sbindir",
                    "--libexecdir", "--libdir", "--includedir", "--oldincludedir",
                    "--datarootdir", "--datadir", "--infodir", "--localedir",
                    "--mandir", "--docdir", "--htmldir", "--dvidir", "--pdfdir",
                    "--psdir", "--sysconfdir", "--sharedstatedir", "--localstatedir",
                    "--runstatedir"};
                if (std::ranges::any_of(install_options, [&](std::string_view name) {
                        return option == name || option.starts_with(std::string(name) + "=");
                    }) && !canonical_autotools_install_option(option))
                    return std::unexpected(
                        "Autotools installation directory is Sage-managed: " + option);
            }
            const auto configure_vars = make_vars.empty()
                ? std::string{}
                : make_vars.substr(1) + " ";
            if (spec.build_dir.empty()) {
                step("configure", source, configure_vars
                    + "./configure --prefix=/usr --libdir=/usr/lib" +
                    (options.empty() ? "" : " " + options));
            } else {
                // Out-of-tree Autotools projects (glibc is the important
                // system example) keep generated files out of the source
                // archive.  The directory is created inside Sage's source
                // root, and only the relative `../configure` entry point is
                // reachable from it.
                step("configure", source, "mkdir -p " + shell_quote(build.string())
                    + " && cd " + shell_quote(build.string())
                    + " && " + configure_vars
                    + "../configure --prefix=/usr --libdir=/usr/lib"
                    + (options.empty() ? "" : " " + options));
            }
            const auto autotools_cwd = spec.build_dir.empty() ? source : build;
            custom_steps("pre-build");
            step("build", autotools_cwd, "make" + make_managed_vars + make_vars +
                (build_targets.empty() ? "" : " " + build_targets));
            custom_steps("post-build");
            custom_steps("pre-install");
            step("install", autotools_cwd, "make" + make_managed_vars + make_vars + " DESTDIR=" +
                shell_quote(paths.package.string()) + " " +
                (install_targets.empty() ? "install" : install_targets));
            custom_steps("install");
            custom_steps("post-install");
            break;
        }
        case package::BuildSystem::CMake: {
            for (const auto& option : spec.configure_options) {
                auto upper = option;
                std::ranges::transform(upper, upper.begin(), [](unsigned char c) {
                    return static_cast<char>(std::toupper(c));
                });
                if (option == "-D" || option.starts_with("-DCMAKE_C_COMPILER")
                    || option.starts_with("-DCMAKE_CXX_COMPILER")
                    || option.starts_with("-DCMAKE_LINKER")
                    || option.starts_with("-DCMAKE_TOOLCHAIN_FILE")
                    || upper.starts_with("-DCMAKE_INSTALL_")
                    || upper.starts_with("-DCMAKE_FIND_ROOT_")
                    || upper.starts_with("-DCMAKE_PREFIX_PATH")
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
                " -DCMAKE_INSTALL_LIBDIR=lib" +
                cmake_arg("CMAKE_C_COMPILER", tools.cc) +
                cmake_arg("CMAKE_CXX_COMPILER", tools.cxx) +
                cmake_arg("CMAKE_LINKER", tools.linker) +
                cmake_arg("CMAKE_C_FLAGS", cfg.cflags) +
                cmake_arg("CMAKE_CXX_FLAGS", cxxflags) +
                cmake_arg("CMAKE_EXE_LINKER_FLAGS", ldflags) +
                cmake_arg("CMAKE_SHARED_LINKER_FLAGS", ldflags));
            custom_steps("pre-build");
            step("build", source, "cmake --build " + shell_quote(build.string()) +
                " --parallel " + std::to_string(jobs) +
                (build_targets.empty() ? "" : " --target " + build_targets));
            custom_steps("post-build");
            custom_steps("pre-install");
            step("install", source, "DESTDIR=" + shell_quote(paths.package.string()) +
                (install_targets.empty()
                    ? " cmake --install " + shell_quote(build.string())
                    : " cmake --build " + shell_quote(build.string())
                        + " --target " + install_targets));
            custom_steps("install");
            custom_steps("post-install");
            break;
        }
        case package::BuildSystem::Meson:
            for (const auto& option : spec.configure_options) {
                auto lower = option;
                std::ranges::transform(lower, lower.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
                if (option == "--native-file" || option.starts_with("--native-file=")
                    || option == "--cross-file" || option.starts_with("--cross-file=")
                    || option == "--backend" || option.starts_with("--backend=")
                    || option == "--prefix" || option.starts_with("--prefix=")
                    || option == "--bindir" || option.starts_with("--bindir=")
                    || option == "--sbindir" || option.starts_with("--sbindir=")
                    || option == "--libexecdir" || option.starts_with("--libexecdir=")
                    || option == "--libdir" || option.starts_with("--libdir=")
                    || option == "--datadir" || option.starts_with("--datadir=")
                    || option == "--includedir" || option.starts_with("--includedir=")
                    || option == "--infodir" || option.starts_with("--infodir=")
                    || option == "--localedir" || option.starts_with("--localedir=")
                    || option == "--mandir" || option.starts_with("--mandir=")
                    || option == "--sysconfdir" || option.starts_with("--sysconfdir=")
                    || option == "--localstatedir" || option.starts_with("--localstatedir=")
                    || option == "--sharedstatedir" || option.starts_with("--sharedstatedir=")
                    || lower == "-d"
                    || lower.starts_with("-dprefix") || lower.starts_with("-dlibdir")
                    || lower.starts_with("-dbindir") || lower.starts_with("-dsbindir")
                    || lower.starts_with("-dincludedir") || lower.starts_with("-ddatadir")
                    || lower.starts_with("-dsysconfdir") || lower.starts_with("-dlocalstatedir")
                    || lower.starts_with("-dlocaledir") || lower.starts_with("-dmandir")
                    || lower.starts_with("-drunstatedir") || lower.starts_with("-dlibexecdir")
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
                " --prefix=/usr --libdir=lib --buildtype=release" +
                (options.empty() ? "" : " " + options));
            custom_steps("pre-build");
            step("build", source, "meson compile -C " + shell_quote(build.string()) +
                " -j " + std::to_string(jobs) +
                (build_targets.empty() ? "" : " " + build_targets));
            custom_steps("post-build");
            custom_steps("pre-install");
            step("install", source, "DESTDIR=" + shell_quote(paths.package.string()) +
                " meson install -C " + shell_quote(build.string()) + " --no-rebuild");
            custom_steps("install");
            custom_steps("post-install");
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
                (options.empty() ? "" : " " + options) + " --cc=" + shell_quote(cc_exec) +
                " --cxx=" + shell_quote(cxx_exec) + " --ld=" +
                // Xmake's ld slot is a compiler driver: a raw GNU ld cannot
                // consume driver options such as -m64 or linked libraries.
                // The probed linker is still selected truthfully by the
                // Sage-owned -fuse-ld flag carried in ldflags.
                shell_quote(cxx_exec) + " --cflags=" + shell_quote(cfg.cflags) +
                " --cxflags=" + shell_quote(cfg.cppflags) +
                " --cxxflags=" + shell_quote(cxxflags) +
                " --ldflags=" + shell_quote(ldflags));
            custom_steps("pre-build");
            step("build", source, "xmake -j " + std::to_string(jobs) + " --root" +
                (build_targets.empty() ? "" : " " + build_targets));
            custom_steps("post-build");
            custom_steps("pre-install");
            step("install", source, "xmake install --root -o " +
                shell_quote((paths.package / "usr").string()));
            custom_steps("install");
            custom_steps("post-install");
            break;
        case package::BuildSystem::Cargo:
            if (!spec.configure_options.empty()) return std::unexpected(
                "Cargo has no configure step; use build_targets/install_targets");
            for (const auto* values : {&spec.build_targets, &spec.install_targets})
                if (std::ranges::any_of(*values, [](const std::string& value) {
                        return value == "--config" || value.starts_with("--config=")
                            || value == "--root" || value.starts_with("--root=")
                            || value == "--target-dir" || value.starts_with("--target-dir=");
                    }))
                    return std::unexpected(
                        "Cargo config/root/target-dir are Sage-managed and cannot be overridden");
            custom_steps("pre-build");
            step("build", source, "cargo build --release --locked" +
                (build_targets.empty() ? "" : " " + build_targets));
            custom_steps("post-build");
            custom_steps("pre-install");
            step("install", source, "cargo install --path . --root " +
                shell_quote((paths.package / "usr").string()) + " --locked --no-track" +
                (install_targets.empty() ? "" : " " + install_targets));
            custom_steps("install");
            custom_steps("post-install");
            break;
        case package::BuildSystem::Make:
            if (!spec.configure_options.empty()) return std::unexpected(
                "Make has no configure step; use build.variables and targets");
            custom_steps("pre-build");
            step("build", source, "make" + make_managed_vars + make_vars +
                (build_targets.empty() ? "" : " " + build_targets));
            custom_steps("post-build");
            custom_steps("pre-install");
            step("install", source, "make" + make_managed_vars + make_vars +
                (install_targets.empty() ? " install" : " " + install_targets));
            custom_steps("install");
            custom_steps("post-install");
            break;
        case package::BuildSystem::Script:
            if (!spec.configure_options.empty() || !spec.build_targets.empty()
                || !spec.install_targets.empty())
                return std::unexpected(
                    "Script recipes use build.steps instead of backend targets");
            custom_steps("pre-build");
            custom_steps("post-build");
            custom_steps("pre-install");
            custom_steps("install");
            custom_steps("post-install");
            break;
        case package::BuildSystem::Legacy:
            return std::unexpected("Recipe v2 cannot use the legacy build system");
    }
    return plan;
}

} // namespace sage::build
