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
    std::string go;
    std::string target_triplet;
    // The first four fields are the canonical executables Sage probed.  The
    // *_for_build fields are deliberately separate: during a build Sage may
    // substitute an audit wrapper or a compiler-cache wrapper, while manifests
    // continue to name the executable whose --version was actually inspected.
    std::string cc_for_build;
    std::string cxx_for_build;
    std::string linker_for_build;
    std::string rustc_for_build;
    std::string cc_cache_for_build;
    std::string cxx_cache_for_build;
    std::string cache_for_build;
    std::string compiler_cache_mode;
    std::string path_for_build;
    std::string compiler_version;
    std::string cxx_version;
    std::string linker_version;
    std::string rustc_version;
    std::string go_version;
    std::string compiler_family;
    std::string cxx_family;
    std::string linker_family;
    std::string rustc_family;
    std::string go_family;
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

// Apply the recipe's declared flag downgrade to one flag class. Only removal
// is expressible: a recipe may state that it cannot take LTO, the global
// -march baseline or --as-needed, and Sage drops those tokens from every
// managed channel. Nothing can be added.
inline std::string apply_flag_policy(std::string flags,
                                     const package::FlagPolicy& policy) {
    if (policy.empty() || flags.empty()) return flags;
    std::string out;
    std::vector<std::string_view> tokens;
    for (std::size_t at = 0; at < flags.size();) {
        while (at < flags.size() && (flags[at] == ' ' || flags[at] == '\t')) ++at;
        if (at >= flags.size()) break;
        const auto end = flags.find_first_of(" \t", at);
        const auto token = std::string_view(flags).substr(
            at, end == std::string::npos ? std::string::npos : end - at);
        if (!token.empty()) tokens.push_back(token);
        if (end == std::string::npos) break;
        at = end + 1;
    }
    const auto drop_c = [&](std::string_view token) {
        if (policy.no_lto && (token.starts_with("-flto")
                              || token.starts_with("-ffat-lto")
                              || token == "-fno-fat-lto-objects")) return true;
        if (policy.no_march && (token.starts_with("-march=")
                                || token.starts_with("-mtune=")
                                || token.starts_with("-mcpu="))) return true;
        if (policy.no_as_needed && (token == "-Wl,--as-needed"
                                    || token == "-Wl,--no-as-needed"
                                    || token == "--as-needed"
                                    || token == "--no-as-needed")) return true;
        return false;
    };
    const auto drop_rust = [&](std::string_view token) {
        if (token == "-C") return false;
        if (policy.no_lto && (token.starts_with("lto")
                              || token.starts_with("-Clto")
                              || token.starts_with("-C lto")
                              || token.starts_with("embed-bitcode="))) return true;
        if (policy.no_march && (token.starts_with("target-cpu=")
                                || token.starts_with("-Ctarget-cpu")
                                || token.starts_with("-C target-cpu"))) return true;
        if (policy.no_as_needed && token.contains("as-needed")) return true;
        return false;
    };
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        bool drop = drop_c(tokens[i]) || drop_rust(tokens[i]);
        // RUSTFLAGS spellings put "-C" and its argument in separate tokens.
        if (!drop && tokens[i] == "-C" && i + 1 < tokens.size()
            && drop_rust(tokens[i + 1])) {
            ++i;
            continue;
        }
        if (drop) continue;
        out += out.empty() ? std::string(tokens[i]) : " " + std::string(tokens[i]);
    }
    return out;
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
    if (auto result = check(spec.rust,
                 tools.rustc_family.empty() ? tool_family(tools.rustc)
                                            : tools.rustc_family,
                 tools.rustc_version, "Rust compiler"); !result)
        return result;
    if (auto result = check(spec.go, tools.go_family.empty()
            ? std::string{"go"} : tools.go_family, tools.go_version,
            "Go toolchain"); !result)
        return result;
    return {};
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
    if (spec.system == package::BuildSystem::Go) {
        if (!spec.configure_options.empty()) return std::unexpected(
            "Go has no configure step; use build_targets/install_targets");
        if (!spec.build_dir.empty()) return std::unexpected(
            "Go builds run from the module root; build.build_dir must stay empty");
        if (tools.go_version.empty() || tools.go_version == "unknown")
            return std::unexpected(
                "Go recipes require a go toolchain with a parseable 'go version' result");
    }
    auto safe_relative = [](const std::filesystem::path& path) {
        return !path.is_absolute() && std::ranges::none_of(path, [](const auto& part) {
            return part == "..";
        });
    };
    if (!safe_relative(spec.source_subdir) || !safe_relative(spec.build_dir))
        return std::unexpected(
            "build.source_subdir and build.build_dir must stay inside the source tree");
    const auto valid_sha256 = [](std::string_view value) {
        return value.size() == 64 && std::ranges::all_of(value, [](char c) {
            return std::isxdigit(static_cast<unsigned char>(c));
        });
    };
    if (!spec.patches.empty() && spec.patches_spec.size() != spec.patches.size())
        return std::unexpected(
            "Recipe v2 patches must be normalized into PatchSpec entries");
    std::set<std::string> patch_names;
    for (const auto& patch : spec.patches_spec) {
        if (patch.file.empty() || std::filesystem::path(patch.file).filename() != patch.file
            || !patch_names.insert(patch.file).second)
            return std::unexpected(
                "build.patches entries must be unique distfiles basenames");
        if (patch.strip < 0 || patch.strip > 9 || !valid_sha256(patch.sha256))
            return std::unexpected(
                "Every build.patches entry requires a strip value from 0 to 9 and a 64-hex SHA-256");
    }
    if (!recipe.check_deps.empty()
        && !std::ranges::any_of(spec.steps, [](const package::ManagedBuildStep& step) {
               return step.phase == "check";
           }))
        return std::unexpected(
            "package.check_dependencies require at least one build.steps phase='check'");
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
    const auto cc_driver_exec = tools.cc_for_build.empty() ? tools.cc : tools.cc_for_build;
    const auto cxx_driver_exec = tools.cxx_for_build.empty() ? tools.cxx : tools.cxx_for_build;
    const auto cc_exec = tools.cc_cache_for_build.empty()
        ? cc_driver_exec : tools.cc_cache_for_build;
    const auto cxx_exec = tools.cxx_cache_for_build.empty()
        ? cxx_driver_exec : tools.cxx_cache_for_build;
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
    const std::string cxxflags = apply_flag_policy(
        cfg.cxxflags.empty() ? cfg.cflags : cfg.cxxflags, spec.flag_policy);
    const std::string cflags = apply_flag_policy(cfg.cflags, spec.flag_policy);
    const std::string cppflags = apply_flag_policy(cfg.cppflags, spec.flag_policy);
    const std::string ldflags_base = apply_flag_policy(cfg.ldflags, spec.flag_policy);
    const std::string ldflags = ldflags_base
        + (fuse.empty() ? "" : (ldflags_base.empty() ? "" : " ") + fuse)
        + audit_prefix;
    const std::string rustflags = apply_flag_policy(cfg.rustflags, spec.flag_policy)
        + (cfg.rustflags.empty() ? "" : " ")
        + "-C linker=" + cc_driver_exec
        + (fuse.empty() ? "" : " -C link-arg=" + fuse)
        + (tools.path_for_build.empty() ? ""
            : " -C link-arg=-B" + audit_root);

    plan.environment = {
        {"CC", cc_exec}, {"CXX", cxx_exec}, {"LD", linker_exec},
        {"CPPFLAGS", cppflags}, {"CFLAGS", cflags},
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
    if (spec.system == package::BuildSystem::Go) {
        // A Go recipe uses its own toolchain. For cgo support, retain CC/CXX/LD
        // pointing to the Sage toolchain audit wrappers so cgo builds are properly
        // audited, while stripping Rust/C-specific build flag shims.
        for (const auto* name : {"RUSTFLAGS", "ARFLAGS", "MAKEFLAGS",
                                 "CARGO_BUILD_JOBS", "CARGO_INCREMENTAL", "CARGO_TERM_COLOR"})
            plan.environment.erase(name);
    }
    if (spec.kernel) {
        // Kbuild has its own flag channels. Keep the source of truth in
        // build.toml: compiler flags become KCFLAGS (and the corresponding
        // preprocessor/linker/Rust channels), while LLVM=1 is derived only
        // from the compiler Sage actually selected and probed.
        plan.environment["KCFLAGS"] = cflags;
        plan.environment["KCPPFLAGS"] = cppflags;
        plan.environment["KBUILD_LDFLAGS"] = ldflags_base;
        plan.environment["KRUSTFLAGS"] = apply_flag_policy(
            cfg.rustflags, spec.flag_policy);
        if (compiler == "clang") plan.environment["LLVM"] = "1";
        if (!tools.target_triplet.empty()) {
            plan.environment["ARCH"] = config::triplet_to_kbuild_arch(tools.target_triplet);
            plan.environment["CROSS_COMPILE"] = tools.target_triplet + "-";
        }
    }
    if (!tools.target_triplet.empty()) {
        plan.environment["CHOST"] = tools.target_triplet;
        plan.environment["PKG_CONFIG_LIBDIR"] = std::format(
            "/usr/{}/lib/pkgconfig:/usr/{}/share/pkgconfig",
            tools.target_triplet, tools.target_triplet);
    }
    const std::string_view cache_mode = tools.compiler_cache_mode.empty()
        ? cfg.compiler_cache_mode()
        : std::string_view(tools.compiler_cache_mode);
    if (cache_mode == "ccache") {
        plan.environment["CCACHE_DIR"] = cfg.ccache_dir.string();
        plan.environment["CCACHE_COMPILERCHECK"] = "content";
        plan.environment["CCACHE_BASEDIR"] = paths.source.string();
        plan.environment["CCACHE_SLOPPINESS"] = "time_macros,include_file_mtime";
    } else if (cache_mode == "sccache") {
        plan.environment["SCCACHE_DIR"] = (cfg.ccache_dir / "sccache").string();
        if (spec.system == package::BuildSystem::Cargo
            && !tools.cache_for_build.empty())
            plan.environment["RUSTC_WRAPPER"] = tools.cache_for_build;
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
    if (spec.system == package::BuildSystem::Go) {
        // By default Sage builds Go hermetically: the sandbox has no network,
        // so module dependencies must be inputs -- a committed vendor/ tree, or
        // a module cache staged from [[source]] entries by a prepare step.
        // GOPROXY=off fails fast instead of hanging and GOTOOLCHAIN=local stops
        // the go command from fetching a different toolchain. A recipe that
        // declares build.network = true keeps the network and lets `go` fetch
        // from its regular proxy (go.sum still pins every module version).
        std::error_code vendor_ec;
        if (std::filesystem::is_directory(source / "vendor", vendor_ec))
            plan.environment["GOFLAGS"] = "-mod=vendor";
        if (!spec.network) {
            plan.environment["GOPROXY"] = "off";
        }
        plan.environment["GOTOOLCHAIN"] = "local";
        plan.environment["GOBIN"] = (paths.package / "usr/bin").string();
        if (!paths.home.empty()) {
            plan.environment["GOPATH"] = (paths.home / "go").string();
            plan.environment["GOCACHE"] = (paths.home / "go-cache").string();
            plan.environment["GOMODCACHE"] =
                (paths.home / "go" / "pkg" / "mod").string();
        }
    }
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
    aliases(spec.cflags_env, cflags);
    aliases(spec.cxxflags_env, cxxflags);
    aliases(spec.cppflags_env, cppflags);
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
            "CARGO_HOME", "RUSTUP_HOME", "XDG_CONFIG_HOME", "CHOST",
            "PKG_CONFIG_LIBDIR", "CCACHE_DIR", "CCACHE_COMPILERCHECK",
            "CCACHE_BASEDIR", "CCACHE_SLOPPINESS", "SCCACHE_DIR",
            "RUSTC_WRAPPER",
        };
        if (spec.system == package::BuildSystem::Cargo) names.insert("RUSTC");
        if (spec.system == package::BuildSystem::Go) {
            for (const auto& name : {std::string_view{"GOFLAGS"},
                                      std::string_view{"GOPROXY"},
                                      std::string_view{"GOTOOLCHAIN"},
                                      std::string_view{"GOBIN"},
                                      std::string_view{"GOPATH"},
                                      std::string_view{"GOCACHE"},
                                      std::string_view{"GOMODCACHE"}})
                names.insert(std::string(name));
        }
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
    std::map<std::string, std::string> all_variables = spec.variables;
    if (spec.make) {
        for (const auto& [k, v] : spec.make->variables) {
            all_variables[k] = v;
        }
    }
    for (const auto& [name, _] : all_variables) {
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
    if (!spec.patches_spec.empty()) {
        for (const auto& patch : spec.patches_spec) {
            step("patch-" + patch.file, source, std::format("patch -p{} --forward --input {}",
                patch.strip, shell_quote((paths.source / "distfiles" / patch.file).string())));
        }
    } else {
        for (const auto& patch : spec.patches) {
            step("patch", source, std::format("patch -p{} --forward --input {}",
                spec.patch_strip, shell_quote((paths.source / "distfiles" / patch).string())));
        }
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

    std::vector<std::string> all_build_targets = spec.build_targets;
    std::vector<std::string> all_install_targets = spec.install_targets;
    if (spec.make) {
        all_build_targets.insert(all_build_targets.end(),
            spec.make->targets.begin(), spec.make->targets.end());
        all_install_targets.insert(all_install_targets.end(),
            spec.make->install_targets.begin(), spec.make->install_targets.end());
    }
    const auto build_targets = join_args(all_build_targets);
    const auto install_targets = join_args(all_install_targets);
    std::string make_vars;
    for (const auto& [key, raw] : all_variables) {
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
            std::vector<std::string> autotools_opts = spec.configure_options;
            if (spec.autotools) {
                for (const auto& item : spec.autotools->enable) {
                    autotools_opts.push_back("--enable-" + item);
                }
                for (const auto& item : spec.autotools->disable) {
                    autotools_opts.push_back("--disable-" + item);
                }
                for (const auto& item : spec.autotools->with) {
                    autotools_opts.push_back("--with-" + item);
                }
                for (const auto& item : spec.autotools->without) {
                    autotools_opts.push_back("--without-" + item);
                }
                autotools_opts.insert(autotools_opts.end(),
                    spec.autotools->raw_options.begin(), spec.autotools->raw_options.end());
            }
            if (!tools.target_triplet.empty()) {
                autotools_opts.push_back("--host=" + tools.target_triplet);
            }
            for (const auto& option : autotools_opts) {
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
            const auto options = join_args(autotools_opts);
            const auto configure_vars = make_vars.empty()
                ? std::string{}
                : make_vars.substr(1) + " ";
            if (spec.build_dir.empty()) {
                step("configure", source, configure_vars
                    + "./configure --prefix=/usr --libdir=/usr/lib" +
                    (options.empty() ? "" : " " + options));
            } else {
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
            custom_steps("check");
            custom_steps("pre-install");
            step("install", autotools_cwd, "make" + make_managed_vars + make_vars + " DESTDIR=" +
                shell_quote(paths.package.string()) + " " +
                (install_targets.empty() ? "install" : install_targets));
            custom_steps("install");
            custom_steps("post-install");
            break;
        }
        case package::BuildSystem::CMake: {
            std::vector<std::string> cmake_opts = spec.configure_options;
            std::string cmake_build_type = "Release";
            if (spec.cmake) {
                if (!spec.cmake->build_type.empty()) {
                    cmake_build_type = spec.cmake->build_type;
                }
                for (const auto& [k, v] : spec.cmake->definitions) {
                    cmake_opts.push_back("-D" + k + "=" + v);
                }
                for (const auto& feat : spec.cmake->features) {
                    if (feat.starts_with("-D")) {
                        cmake_opts.push_back(feat);
                    } else if (feat.find('=') != std::string::npos) {
                        cmake_opts.push_back("-D" + feat);
                    } else {
                        cmake_opts.push_back("-D" + feat + "=ON");
                    }
                }
                cmake_opts.insert(cmake_opts.end(),
                    spec.cmake->raw_options.begin(), spec.cmake->raw_options.end());
            }
            for (const auto& option : cmake_opts) {
                auto upper = option;
                std::ranges::transform(upper, upper.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
                static constexpr std::string_view banned_cmake[] = {
                    "-DCMAKE_C_COMPILER", "-DCMAKE_CXX_COMPILER", "-DCMAKE_LINKER",
                    "-DCMAKE_TOOLCHAIN_FILE", "-DCMAKE_C_FLAGS", "-DCMAKE_CXX_FLAGS",
                    "-DCMAKE_EXE_LINKER_FLAGS", "-DCMAKE_SHARED_LINKER_FLAGS"
                };
                static constexpr std::string_view banned_cmake_upper[] = {
                    "-DCMAKE_INSTALL_", "-DCMAKE_FIND_ROOT_", "-DCMAKE_PREFIX_PATH"
                };
                bool banned = (option == "-D" || option == "-G" || option.starts_with("-G"))
                    || std::ranges::any_of(banned_cmake, [&](auto p) { return option.starts_with(p); })
                    || std::ranges::any_of(banned_cmake_upper, [&](auto p) { return upper.starts_with(p); });
                if (banned) return std::unexpected("CMake compiler, linker, flags and generator are Sage-managed: " + option);
            }
            const auto cmake_arg = [&](std::string name, const std::string& value) {
                return " " + shell_quote("-D" + std::move(name) + "=" + value);
            };
            std::string cross_cmake;
            if (!tools.target_triplet.empty()) {
                cross_cmake = cmake_arg("CMAKE_SYSTEM_NAME", "Linux")
                    + cmake_arg("CMAKE_SYSTEM_PROCESSOR", config::triplet_to_kbuild_arch(tools.target_triplet));
            }
            const auto options = join_args(cmake_opts);
            step("configure", source, "cmake -S . -B " + shell_quote(build.string()) +
                (options.empty() ? "" : " " + options) + cross_cmake +
                " -G Ninja -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=" + cmake_build_type +
                " -DCMAKE_INSTALL_LIBDIR=lib" +
                cmake_arg("CMAKE_C_COMPILER", tools.cc) +
                cmake_arg("CMAKE_CXX_COMPILER", tools.cxx) +
                cmake_arg("CMAKE_LINKER", tools.linker) +
                (tools.cache_for_build.empty() ? std::string{} :
                    cmake_arg("CMAKE_C_COMPILER_LAUNCHER", tools.cache_for_build) +
                    cmake_arg("CMAKE_CXX_COMPILER_LAUNCHER", tools.cache_for_build)) +
                cmake_arg("CMAKE_C_FLAGS", cflags) +
                cmake_arg("CMAKE_CXX_FLAGS", cxxflags) +
                cmake_arg("CMAKE_EXE_LINKER_FLAGS", ldflags) +
                cmake_arg("CMAKE_SHARED_LINKER_FLAGS", ldflags));
            custom_steps("pre-build");
            step("build", source, "ninja -C " + shell_quote(build.string()) +
                " -j " + std::to_string(jobs) +
                (build_targets.empty() ? "" : " " + build_targets));
            custom_steps("post-build");
            custom_steps("check");
            custom_steps("pre-install");
            step("install", source, "DESTDIR=" + shell_quote(paths.package.string()) +
                " ninja -C " + shell_quote(build.string()) +
                (install_targets.empty() ? " install" : " " + install_targets));
            custom_steps("install");
            custom_steps("post-install");
            break;
        }
        case package::BuildSystem::Meson: {
            std::filesystem::path build = paths.source / "build";
            std::vector<std::string> meson_opts = spec.configure_options;
            std::string meson_build_type = "release";
            if (spec.meson) {
                if (!spec.meson->build_type.empty()) {
                    meson_build_type = spec.meson->build_type;
                }
                for (const auto& [k, v] : spec.meson->options) {
                    meson_opts.push_back("-D" + k + "=" + v);
                }
                meson_opts.insert(meson_opts.end(),
                    spec.meson->raw_options.begin(), spec.meson->raw_options.end());
            }
            for (const auto& option : meson_opts) {
                auto lower = option;
                std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                static constexpr std::string_view banned_prefixes[] = {
                    "--native-file", "--cross-file", "--backend", "--prefix", "--bindir",
                    "--sbindir", "--libexecdir", "--libdir", "--datadir", "--includedir",
                    "--infodir", "--localedir", "--mandir", "--sysconfdir", "--localstatedir",
                    "--sharedstatedir", "--buildtype", "-Dc_args=", "-Dcpp_args=",
                    "-Dc_link_args=", "-Dcpp_link_args="
                };
                static constexpr std::string_view banned_d_prefixes[] = {
                    "-dprefix", "-dlibdir", "-dbindir", "-dsbindir", "-dincludedir",
                    "-ddatadir", "-dsysconfdir", "-dlocalstatedir", "-dlocaledir",
                    "-dmandir", "-drunstatedir", "-dlibexecdir"
                };
                bool banned = (lower == "-d") || std::ranges::any_of(banned_prefixes, [&](auto p) {
                    return option == p || option.starts_with(std::string(p) + "=") || option.starts_with(p);
                }) || std::ranges::any_of(banned_d_prefixes, [&](auto p) {
                    return lower.starts_with(p);
                });
                if (banned) return std::unexpected("Meson toolchain and compiler/linker arguments are Sage-managed: " + option);
            }
            if (!spec.install_targets.empty()) return std::unexpected(
                "Meson install does not accept build.install_targets");
            const auto options = join_args(meson_opts);
            step("configure", source, "meson setup " + shell_quote(build.string()) +
                " --prefix=/usr --libdir=lib --buildtype=" + meson_build_type +
                (options.empty() ? "" : " " + options));
            custom_steps("pre-build");
            step("build", source, "meson compile -C " + shell_quote(build.string()) +
                " -j " + std::to_string(jobs) +
                (build_targets.empty() ? "" : " " + build_targets));
            custom_steps("post-build");
            custom_steps("check");
            custom_steps("pre-install");
            step("install", source, "DESTDIR=" + shell_quote(paths.package.string()) +
                " meson install -C " + shell_quote(build.string()) + " --no-rebuild");
            custom_steps("install");
            custom_steps("post-install");
            break;
        }
        case package::BuildSystem::Xmake: {
            std::vector<std::string> xmake_opts = spec.configure_options;
            std::string xmake_mode = "release";
            if (spec.xmake) {
                if (!spec.xmake->mode.empty()) {
                    xmake_mode = spec.xmake->mode;
                }
                for (const auto& [k, v] : spec.xmake->configs) {
                    if (v.empty()) {
                        xmake_opts.push_back("--" + k);
                    } else {
                        xmake_opts.push_back("--" + k + "=" + v);
                    }
                }
                xmake_opts.insert(xmake_opts.end(),
                    spec.xmake->raw_options.begin(), spec.xmake->raw_options.end());
            }
            for (const auto& option : xmake_opts) {
                static constexpr std::string_view banned_xmake[] = {
                    "--cc", "--cxx", "--ld", "--toolchain", "--cflags", "--cxflags", "--cxxflags", "--ldflags"
                };
                bool banned = std::ranges::any_of(banned_xmake, [&](auto p) {
                    return option == p || option.starts_with(std::string(p) + "=");
                });
                if (banned) return std::unexpected("Xmake compiler, linker and toolchain are Sage-managed: " + option);
            }
            if (!spec.install_targets.empty()) return std::unexpected(
                "Xmake install does not accept build.install_targets");
            const auto options = join_args(xmake_opts);
            step("configure", source, "xmake f --root -m " + xmake_mode +
                (options.empty() ? "" : " " + options) + " --cc=" + shell_quote(cc_exec) +
                " --cxx=" + shell_quote(cxx_exec) + " --ld=" +
                shell_quote(cxx_driver_exec) + " --cflags=" + shell_quote(cflags) +
                " --cxflags=" + shell_quote(cppflags) +
                " --cxxflags=" + shell_quote(cxxflags) +
                " --ldflags=" + shell_quote(ldflags));
            custom_steps("pre-build");
            step("build", source, "xmake -j " + std::to_string(jobs) + " --root" +
                (build_targets.empty() ? "" : " " + build_targets));
            custom_steps("post-build");
            custom_steps("check");
            custom_steps("pre-install");
            step("install", source, "xmake install --root -o " +
                shell_quote((paths.package / "usr").string()));
            custom_steps("install");
            custom_steps("post-install");
            break;
        }
        case package::BuildSystem::Cargo: {
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
            std::string cargo_build_flags;
            bool locked = true;
            if (spec.cargo) {
                locked = spec.cargo->locked;
                if (spec.cargo->default_features.has_value() && !*spec.cargo->default_features) {
                    cargo_build_flags += " --no-default-features";
                }
                if (!spec.cargo->features.empty()) {
                    std::string feat_str;
                    for (const auto& f : spec.cargo->features) {
                        feat_str += (feat_str.empty() ? "" : ",") + f;
                    }
                    cargo_build_flags += " --features " + shell_quote(feat_str);
                }
                if (!spec.cargo->raw_options.empty()) {
                    cargo_build_flags += " " + join_args(spec.cargo->raw_options);
                }
            }
            if (!tools.target_triplet.empty()) {
                cargo_build_flags += " --target " + shell_quote(tools.target_triplet);
            }
            std::string locked_flag = locked ? " --locked" : "";
            custom_steps("pre-build");
            step("build", source, "cargo build --release" + locked_flag + cargo_build_flags +
                (build_targets.empty() ? "" : " " + build_targets));
            custom_steps("post-build");
            custom_steps("check");
            custom_steps("pre-install");
            step("install", source, "cargo install --path . --root " +
                shell_quote((paths.package / "usr").string()) + locked_flag + " --no-track" +
                cargo_build_flags + (install_targets.empty() ? "" : " " + install_targets));
            custom_steps("install");
            custom_steps("post-install");
            break;
        }
        case package::BuildSystem::Go: {
            for (const auto* values : {&spec.build_targets, &spec.install_targets})
                for (const auto& value : *values)
                    if (value == "-C" || value.starts_with("-C=")
                        || value == "-mod" || value.starts_with("-mod=")
                        || value == "-overlay" || value.starts_with("-overlay=")
                        || value == "-toolexec" || value.starts_with("-toolexec="))
                        return std::unexpected(
                            "Go -C/-mod/-overlay/-toolexec are Sage-managed and "
                            "cannot be overridden");
            const auto go_build_args = join_args(spec.build_targets);
            const auto go_install_args = join_args(spec.install_targets);
            custom_steps("pre-build");
            if (!spec.build_targets.empty())
                step("build", source, "go build" + (go_build_args.empty() ? "" : " " + go_build_args));
            for (const auto* phase : {"post-build", "check", "pre-install"}) custom_steps(phase);
            step("install", source, "go install" + (go_install_args.empty() ? " ./..." : " " + go_install_args));
            for (const auto* phase : {"install", "post-install"}) custom_steps(phase);
            break;
        }
        case package::BuildSystem::Make: {
            if (!spec.configure_options.empty()) return std::unexpected(
                "Make has no configure step; use build.variables and targets");
            std::string make_raw;
            if (spec.make && !spec.make->raw_options.empty()) {
                make_raw = " " + join_args(spec.make->raw_options);
            }
            custom_steps("pre-build");
            step("build", source, "make" + make_managed_vars + make_vars + make_raw +
                (build_targets.empty() ? "" : " " + build_targets));
            for (const auto* phase : {"post-build", "check", "pre-install"}) custom_steps(phase);
            step("install", source, "make" + make_managed_vars + make_vars + make_raw +
                (install_targets.empty() ? " install" : " " + install_targets));
            for (const auto* phase : {"install", "post-install"}) custom_steps(phase);
            break;
        }
        case package::BuildSystem::Script:
            if (!spec.configure_options.empty() || !spec.build_targets.empty()
                || !spec.install_targets.empty())
                return std::unexpected(
                    "Script recipes use build.steps instead of backend targets");
            for (const auto* phase : {"pre-build", "post-build", "check", "pre-install", "install", "post-install"})
                custom_steps(phase);
            break;
        case package::BuildSystem::Legacy:
            return std::unexpected("Recipe v2 cannot use the legacy build system");
    }
    return plan;
}

} // namespace sage::build
