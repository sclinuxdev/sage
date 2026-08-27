export module sage.package:recipe_v2;

import std;
import sage.vendor.toml;
import :version;
import :deps;
import :trigger;
import :recipe_types;
import :recipe_v2_transforms;
import :recipe_v2_backends;
import :recipe_v2_outputs;

export namespace sage::package {

inline std::expected<void, std::string> parse_managed_build(
    const vendor::toml::table& tbl,
    const std::vector<const vendor::toml::table*>& src_scopes,
    Recipe& r)
{
    const auto reject_unknown = [](const vendor::toml::table& table,
                                   std::initializer_list<std::string_view> allowed,
                                   std::string_view scope)
        -> std::expected<void, std::string> {
        for (const auto& [key, _] : table) {
            if (!std::ranges::contains(allowed, key.str())) {
                return std::unexpected(std::format(
                    "Unknown key '{}' in {}", key.str(), scope));
            }
        }
        return {};
    };
    const auto require_string = [](const vendor::toml::table& table,
                                   std::string_view key,
                                   std::string_view scope,
                                   bool required)
        -> std::expected<std::string, std::string> {
        const auto* node = table.get(key);
        if (!node) {
            if (required) return std::unexpected(std::format(
                "Missing required string '{}.{}'", scope, key));
            return std::string{};
        }
        auto value = node->value<std::string_view>();
        if (!value) return std::unexpected(std::format(
            "'{}.{}' must be a string", scope, key));
        if (required && value->empty()) return std::unexpected(std::format(
            "'{}.{}' must not be empty", scope, key));
        return std::string(*value);
    };
    const auto parse_string_array_strict = [&](const vendor::toml::table& table,
                                               std::string_view key,
                                               std::string_view scope,
                                               std::vector<std::string>& target)
        -> std::expected<void, std::string> {
        const auto* node = table.get(key);
        if (!node) return {};
        const auto* arr = node->as_array();
        if (!arr) return std::unexpected(std::format(
            "'{}.{}' must be an array of strings", scope, key));
        for (const auto& item : *arr) {
            auto s = item.value<std::string_view>();
            if (!s) return std::unexpected(std::format(
                "'{}.{}' entries must be strings", scope, key));
            target.emplace_back(*s);
        }
        return {};
    };

    auto* bld = tbl.get_as<vendor::toml::table>("build");
    if (!bld) return std::unexpected("Recipe v2 requires a [build] table");
    if (auto result = reject_unknown(*bld,
            {"system", "payload", "kernel", "source_subdir", "build_dir",
             "configure_options", "build_targets", "install_targets",
             "install_files", "install_excludes", "optional_excludes",
             "install_copies", "install_symlinks", "install_moves",
             "install_removes", "install_generates", "file_permissions",
             "outputs", "steps", "patches", "patch_checksums", "patch_strip",
             "allowed_compilers", "allowed_linkers", "variables", "flag_env",
             "tool_env", "toolchain", "tools", "flag_policy", "content",
             "network", "header_only",
             "cmake", "meson", "cargo", "autotools", "make", "xmake"},
            "build"); !result)
        return std::unexpected(result.error());
    auto has_commands = [&](const vendor::toml::table& scope) {
        return scope.get_as<vendor::toml::array>("prepare")
            || scope.get_as<vendor::toml::array>("build")
            || scope.get_as<vendor::toml::array>("install");
    };
    if (has_commands(tbl) || has_commands(*bld)) {
        return std::unexpected(
            "Recipe v2 forbids prepare/build/install shell command arrays");
    }
    if (auto* pkg = tbl.get_as<vendor::toml::table>("package");
        pkg && has_commands(*pkg)) {
        return std::unexpected(
            "Recipe v2 forbids prepare/build/install shell command arrays");
    }
    for (const auto* src : src_scopes) {
        if (has_commands(*src)) return std::unexpected(
            "Recipe v2 forbids prepare/build/install shell command arrays");
    }
    auto system = parse_build_system((*bld)["system"].value_or(""));
    if (!system) return std::unexpected(system.error());
    r.managed_build.system = *system;
    const auto payload_value = (*bld)["payload"].value<std::string_view>();
    if (!payload_value) return std::unexpected(
        "Recipe v2 requires build.payload = \"all\", \"allowlist\", or \"outputs\"");
    if (*payload_value == "all") r.managed_build.payload = PayloadMode::All;
    else if (*payload_value == "allowlist")
        r.managed_build.payload = PayloadMode::Allowlist;
    else if (*payload_value == "outputs")
        r.managed_build.payload = PayloadMode::Outputs;
    else return std::unexpected(
        "build.payload must be \"all\", \"allowlist\", or \"outputs\"");
    if (auto value = (*bld)["kernel"].value<bool>())
        r.managed_build.kernel = *value;
    else if (bld->contains("kernel"))
        return std::unexpected("build.kernel must be a boolean");
    if (r.managed_build.kernel
        && r.managed_build.system != BuildSystem::Make)
        return std::unexpected(
            "build.kernel=true requires system = \"make\"");
    if (bld->contains("tools")) {
        auto tools = (*bld)["tools"].value<bool>();
        if (!tools) return std::unexpected(
            "build.tools must be a boolean");
        if (*tools) {
            if (r.managed_build.system != BuildSystem::Script)
                return std::unexpected(
                    "build.tools=true is valid only for script recipes");
            r.managed_build.script_managed_tools = true;
        }
    }
    if (bld->contains("header_only")) {
        auto ho = (*bld)["header_only"].value<bool>();
        if (!ho) return std::unexpected("build.header_only must be a boolean");
        r.managed_build.header_only = *ho;
    }
    if (bld->contains("network")) {
        auto network = (*bld)["network"].value<bool>();
        if (!network) return std::unexpected(
            "build.network must be a boolean");
        r.managed_build.network = *network;
    }
    if (bld->contains("flag_policy")
        && !bld->get_as<vendor::toml::table>("flag_policy"))
        return std::unexpected("build.flag_policy must be a table");
    if (auto* policy = bld->get_as<vendor::toml::table>("flag_policy")) {
        if (auto result = reject_unknown(*policy,
                {"lto", "march", "as-needed"}, "build.flag_policy"); !result)
            return std::unexpected(result.error());
        const auto parse_downgrade =
            [&](const char* key, bool& target)
            -> std::expected<void, std::string> {
            if (!policy->contains(key)) return {};
            auto value = (*policy)[key].value<bool>();
            if (!value) return std::unexpected(std::format(
                "build.flag_policy.{} must be a boolean", key));
            if (!*value) target = true;
            else return std::unexpected(std::format(
                "build.flag_policy.{} = true is the default and cannot "
                "weaken a recipe; flag_policy declares downgrades only", key));
            return {};
        };
        if (auto result = parse_downgrade("lto",
                r.managed_build.flag_policy.no_lto); !result)
            return std::unexpected(result.error());
        if (auto result = parse_downgrade("march",
                r.managed_build.flag_policy.no_march); !result)
            return std::unexpected(result.error());
        if (auto result = parse_downgrade("as-needed",
                r.managed_build.flag_policy.no_as_needed); !result)
            return std::unexpected(result.error());
    }
    if (bld->contains("content")
        && !bld->get_as<vendor::toml::table>("content"))
        return std::unexpected("build.content must be a table");
    if (auto* content = bld->get_as<vendor::toml::table>("content")) {
        if (auto result = reject_unknown(*content,
                {"strip", "man_compress", "shebangs", "locales"},
                "build.content"); !result)
            return std::unexpected(result.error());
        if (content->contains("strip")) {
            auto value = (*content)["strip"].value<std::string_view>();
            if (!value || (*value != "none" && *value != "unneeded"
                           && *value != "debug"))
                return std::unexpected(
                    "build.content.strip must be none, unneeded, or debug");
            r.managed_build.content.strip = std::string(*value);
        }
        if (content->contains("man_compress")) {
            auto value = (*content)["man_compress"].value<std::string_view>();
            if (!value || (*value != "none" && *value != "gzip"))
                return std::unexpected(
                    "build.content.man_compress must be none or gzip");
            r.managed_build.content.man_compress = std::string(*value);
        }
        if (content->contains("shebangs")) {
            auto value = (*content)["shebangs"].value<std::string_view>();
            if (!value || *value != "absolute")
                return std::unexpected(
                    "build.content.shebangs must be \"absolute\"");
            r.managed_build.content.shebangs = std::string(*value);
        }
        if (auto result = parse_string_array_strict(*content, "locales",
                "build.content", r.managed_build.content.locales); !result)
            return std::unexpected(result.error());
        for (const auto& locale : r.managed_build.content.locales) {
            if (locale.empty() || locale.find('/') != std::string::npos
                || locale == "." || locale == "..")
                return std::unexpected(
                    "build.content.locales entries must be plain locale names");
        }
    }
    auto source_subdir = require_string(*bld, "source_subdir", "build", false);
    auto build_dir = require_string(*bld, "build_dir", "build", false);
    if (!source_subdir || !build_dir)
        return std::unexpected(!source_subdir ? source_subdir.error()
                                                : build_dir.error());
    r.managed_build.source_subdir = std::move(*source_subdir);
    r.managed_build.build_dir = build_dir->empty()
        ? (*system == BuildSystem::CMake || *system == BuildSystem::Meson ? "build" : "")
        : std::move(*build_dir);
    if (bld->contains("patch_strip")
        && !(*bld)["patch_strip"].value<std::int64_t>())
        return std::unexpected("build.patch_strip must be an integer");
    r.managed_build.patch_strip = static_cast<int>((*bld)["patch_strip"].value_or(1LL));
    if (r.managed_build.patch_strip < 0 || r.managed_build.patch_strip > 9)
        return std::unexpected("build.patch_strip must be between 0 and 9");
    if (auto result = parse_string_array_strict(*bld, "configure_options",
            "build", r.managed_build.configure_options); !result)
        return std::unexpected(result.error());
    if (auto result = parse_string_array_strict(*bld, "build_targets",
            "build", r.managed_build.build_targets); !result)
        return std::unexpected(result.error());
    if (auto result = parse_string_array_strict(*bld, "install_targets",
            "build", r.managed_build.install_targets); !result)
        return std::unexpected(result.error());
    if (auto result = parse_string_array_strict(*bld, "install_files",
            "build", r.managed_build.install_files); !result)
        return std::unexpected(result.error());
    if (auto result = parse_string_array_strict(*bld, "install_excludes",
            "build", r.managed_build.install_excludes); !result)
        return std::unexpected(result.error());
    for (const auto key : {"install_copies", "install_symlinks",
                           "install_moves", "install_removes",
                           "install_generates", "file_permissions",
                           "optional_excludes"}) {
        if (bld->contains(key) && !bld->get_as<vendor::toml::array>(key))
            return std::unexpected(std::format(
                "build.{} must be an array", key));
    }

    if (auto result = parse_copy_entries(*bld, "install_copies", "build", r.managed_build.install_copies); !result)
        return std::unexpected(result.error());
    if (auto result = parse_symlink_entries(*bld, "install_symlinks", "build", r.managed_build.install_symlinks); !result)
        return std::unexpected(result.error());
    if (auto result = parse_move_entries(*bld, "install_moves", "build", r.managed_build.install_moves); !result)
        return std::unexpected(result.error());
    if (auto result = parse_remove_entries(*bld, "install_removes", "build", r.managed_build.install_removes); !result)
        return std::unexpected(result.error());
    if (auto result = parse_generate_entries(*bld, "install_generates", "build", r.managed_build.install_generates); !result)
        return std::unexpected(result.error());
    if (auto result = parse_file_permission_entries(*bld, "file_permissions", "build", r.managed_build.file_permissions); !result)
        return std::unexpected(result.error());
    if (auto result = parse_string_array_strict(*bld, "optional_excludes", "build", r.managed_build.optional_excludes); !result)
        return std::unexpected(result.error());

    if (auto result = parse_backend_specs(*bld, r); !result)
        return std::unexpected(result.error());

    if (auto result = parse_v2_outputs(*bld, r); !result)
        return std::unexpected(result.error());

    std::map<std::string, std::string> source_hashes;
    const auto valid_sha256 = [](std::string_view value) {
        return value.size() == 64 && std::ranges::all_of(value, [](char c) {
            return std::isxdigit(static_cast<unsigned char>(c));
        });
    };
    const auto normalize_sha256 = [](std::string value) {
        std::ranges::transform(value, value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
    };
    auto add_source_hash = [&](std::string_view url, std::string_view sha)
        -> std::expected<void, std::string> {
        if (url.empty()) return {};
        const auto name = std::filesystem::path(std::string(url)).filename().string();
        if (name.empty()) return {};
        if (auto it = source_hashes.find(name); it != source_hashes.end()) {
            if (it->second != normalize_sha256(std::string(sha)))
                return std::unexpected("Conflicting SHA-256 for source file: " + name);
            return {};
        }
        source_hashes.emplace(name, normalize_sha256(std::string(sha)));
        return {};
    };
    if (!r.source_url.empty()) {
        if (auto result = add_source_hash(r.source_url, r.source_sha256); !result)
            return std::unexpected(result.error());
    }
    for (const auto& extra : r.extra_sources) {
        if (auto result = add_source_hash(extra.url, extra.sha256); !result)
            return std::unexpected(result.error());
    }

    if (bld->contains("steps") && !bld->get_as<vendor::toml::array>("steps"))
        return std::unexpected("build.steps must be an array");
    if (auto* arr = bld->get_as<vendor::toml::array>("steps")) {
        static constexpr std::array<std::string_view, 7> phases{
            "prepare", "pre-build", "post-build", "check",
            "pre-install", "install", "post-install"};
        static constexpr std::array<std::string_view, 3> directories{
            "source", "build", "package"};
        for (auto&& element : *arr) {
            auto* item = element.as_table();
            if (!item) return std::unexpected(
                "build.steps entries must be inline tables");
            if (auto result = reject_unknown(*item,
                    {"name", "phase", "cwd", "command", "unsafe_shell"},
                    "build.steps[]"); !result)
                return std::unexpected(result.error());
            auto name = (*item)["name"].value<std::string_view>();
            auto phase = (*item)["phase"].value<std::string_view>();
            auto cwd = (*item)["cwd"].value<std::string_view>().value_or("source");
            auto command = (*item)["command"].value<std::string_view>();
            if (!name || !phase || !command || name->empty()
                || phase->empty() || command->empty()) {
                return std::unexpected(
                    "build.steps entries require name, phase and command");
            }
            if (!std::ranges::contains(phases, *phase)) {
                return std::unexpected(std::format(
                    "Unsupported build.steps phase '{}'", *phase));
            }
            if (!std::ranges::contains(directories, cwd)) {
                return std::unexpected(std::format(
                    "Unsupported build.steps cwd '{}'", cwd));
            }
            bool unsafe_shell = false;
            if (item->contains("unsafe_shell")) {
                auto val = (*item)["unsafe_shell"].value<bool>();
                if (!val) return std::unexpected("build.steps.unsafe_shell must be a boolean");
                unsafe_shell = *val;
            }
            r.managed_build.steps.push_back(ManagedBuildStep{
                .name = std::string(*name),
                .phase = std::string(*phase),
                .cwd = std::string(cwd),
                .command = std::string(*command),
                .unsafe_shell = unsafe_shell});
        }
    }

    const bool has_check_step = std::ranges::any_of(
        r.managed_build.steps,
        [](const ManagedBuildStep& step) { return step.phase == "check"; });
    if (std::ranges::any_of(r.check_deps, [](const std::string& dep) {
            return dep.empty();
        }))
        return std::unexpected(
            "package.check_dependencies entries must be non-empty strings");
    if (!r.check_deps.empty() && !has_check_step)
        return std::unexpected(
            "package.check_dependencies require at least one build.steps phase='check'");
    if (r.managed_build.system == BuildSystem::Script
        && r.managed_build.steps.empty())
        return std::unexpected(
            "Script recipes require at least one build.steps entry");

    std::set<std::string> patch_names;
    auto add_patch = [&](std::string file, int strip, std::string sha)
        -> std::expected<void, std::string> {
        if (file.empty() || std::filesystem::path(file).filename() != file)
            return std::unexpected(
                "build.patches entries require a basename 'file' string");
        if (!patch_names.insert(file).second)
            return std::unexpected(
                "build.patches cannot declare the same file more than once: "
                + file);
        r.managed_build.patches.push_back(file);
        r.managed_build.patches_spec.push_back(PatchSpec{
            .file = std::move(file),
            .strip = strip,
            .sha256 = std::move(sha)});
        return {};
    };
    if (bld->contains("patches")) {
        auto* arr = bld->get_as<vendor::toml::array>("patches");
        if (!arr) return std::unexpected("build.patches must be an array");
        for (const auto& elem : *arr) {
            if (auto str = elem.value<std::string_view>()) {
                if (auto result = add_patch(std::string(*str),
                        r.managed_build.patch_strip, {}); !result)
                    return std::unexpected(result.error());
            } else if (auto* ptbl = elem.as_table()) {
                if (auto result = reject_unknown(
                        *ptbl, {"file", "strip", "sha256"},
                        "build.patches[]"); !result)
                    return std::unexpected(result.error());
                auto f = (*ptbl)["file"].value<std::string_view>();
                if (!f) return std::unexpected(
                    "build.patches entries require a basename 'file' string");
                int strip = r.managed_build.patch_strip;
                if (ptbl->contains("strip")) {
                    auto strip_value = (*ptbl)["strip"].value<std::int64_t>();
                    if (!strip_value || *strip_value < 0 || *strip_value > 9)
                        return std::unexpected(
                            "build.patches entry 'strip' must be between 0 and 9");
                    strip = static_cast<int>(*strip_value);
                }
                if (!ptbl->contains("sha256"))
                    return std::unexpected(
                        "structured build.patches entries require sha256");
                auto sha_value = (*ptbl)["sha256"].value<std::string_view>();
                if (!sha_value || !valid_sha256(*sha_value))
                    return std::unexpected(
                        "build.patches entry 'sha256' must be a 64-hex SHA-256");
                if (auto result = add_patch(std::string(*f), strip,
                        normalize_sha256(std::string(*sha_value))); !result)
                    return std::unexpected(result.error());
            } else {
                return std::unexpected(
                    "build.patches must be an array of strings or tables");
            }
        }
    }
    if (bld->contains("patch_checksums")
        && !bld->get_as<vendor::toml::table>("patch_checksums"))
        return std::unexpected("build.patch_checksums must be a table");
    if (auto* checksums = bld->get_as<vendor::toml::table>("patch_checksums")) {
        for (const auto& [name, value] : *checksums) {
            auto sha = value.value<std::string_view>();
            if (name.str().empty()
                || std::filesystem::path(name.str()).filename() != name.str()
                || !sha || !valid_sha256(*sha))
                return std::unexpected(
                    "build.patch_checksums entries require a basename and 64-hex SHA-256");
            r.managed_build.patch_checksums.emplace(
                name.str(), normalize_sha256(std::string(*sha)));
        }
    }
    for (const auto& [name, _] : r.managed_build.patch_checksums) {
        if (!patch_names.contains(name))
            return std::unexpected(
                "build.patch_checksums names an undeclared patch: " + name);
    }
    for (auto& patch : r.managed_build.patches_spec) {
        const auto source = source_hashes.find(patch.file);
        const auto checksum = r.managed_build.patch_checksums.find(patch.file);
        if (source != source_hashes.end() && checksum != r.managed_build.patch_checksums.end()
            && source->second != checksum->second)
            return std::unexpected(std::format(
                "Patch '{}' has conflicting source and patch_checksums SHA-256 declarations",
                patch.file));
        if (source != source_hashes.end() && !patch.sha256.empty()
            && source->second != patch.sha256)
            return std::unexpected(std::format(
                "Patch '{}' SHA-256 does not match its [[source]] declaration",
                patch.file));
        if (checksum != r.managed_build.patch_checksums.end()
            && !patch.sha256.empty() && checksum->second != patch.sha256)
            return std::unexpected(std::format(
                "Patch '{}' SHA-256 conflicts with build.patch_checksums",
                patch.file));
        if (patch.sha256.empty()) {
            if (checksum != r.managed_build.patch_checksums.end())
                patch.sha256 = checksum->second;
            else if (source != source_hashes.end())
                patch.sha256 = source->second;
        }
        if (patch.sha256.empty())
            return std::unexpected(
                "Every build.patches entry requires a SHA-256 declaration");
    }

    if (auto result = parse_string_array_strict(*bld, "allowed_compilers",
            "build", r.managed_build.allowed_compilers); !result)
        return std::unexpected(result.error());
    if (auto result = parse_string_array_strict(*bld, "allowed_linkers",
            "build", r.managed_build.allowed_linkers); !result)
        return std::unexpected(result.error());
    if (bld->contains("variables") && !bld->get_as<vendor::toml::table>("variables"))
        return std::unexpected("build.variables must be a table");
    if (auto* vars = bld->get_as<vendor::toml::table>("variables")) {
        for (auto&& [key, value] : *vars) {
            if (auto s = value.value<std::string_view>())
                r.managed_build.variables.emplace(std::string(key.str()), *s);
            else return std::unexpected("build.variables values must be strings");
        }
    }
    if (bld->contains("flag_env") && !bld->get_as<vendor::toml::table>("flag_env"))
        return std::unexpected("build.flag_env must be a table");
    if (auto* flags = bld->get_as<vendor::toml::table>("flag_env")) {
        if (auto result = reject_unknown(*flags,
                {"cflags", "cxxflags", "cppflags", "ldflags", "rustflags"},
                "build.flag_env"); !result)
            return std::unexpected(result.error());
        if (auto result = parse_string_array_strict(*flags, "cflags",
                "build.flag_env", r.managed_build.cflags_env); !result)
            return std::unexpected(result.error());
        if (auto result = parse_string_array_strict(*flags, "cxxflags",
                "build.flag_env", r.managed_build.cxxflags_env); !result)
            return std::unexpected(result.error());
        if (auto result = parse_string_array_strict(*flags, "cppflags",
                "build.flag_env", r.managed_build.cppflags_env); !result)
            return std::unexpected(result.error());
        if (auto result = parse_string_array_strict(*flags, "ldflags",
                "build.flag_env", r.managed_build.ldflags_env); !result)
            return std::unexpected(result.error());
        if (auto result = parse_string_array_strict(*flags, "rustflags",
                "build.flag_env", r.managed_build.rustflags_env); !result)
            return std::unexpected(result.error());
    }
    if (bld->contains("tool_env") && !bld->get_as<vendor::toml::table>("tool_env"))
        return std::unexpected("build.tool_env must be a table");
    if (auto* tools = bld->get_as<vendor::toml::table>("tool_env")) {
        if (auto result = reject_unknown(*tools, {"cc", "cxx", "linker"},
                "build.tool_env"); !result)
            return std::unexpected(result.error());
        if (auto result = parse_string_array_strict(*tools, "cc",
                "build.tool_env", r.managed_build.cc_env); !result)
            return std::unexpected(result.error());
        if (auto result = parse_string_array_strict(*tools, "cxx",
                "build.tool_env", r.managed_build.cxx_env); !result)
            return std::unexpected(result.error());
        if (auto result = parse_string_array_strict(*tools, "linker",
                "build.tool_env", r.managed_build.linker_env); !result)
            return std::unexpected(result.error());
    }

    const auto validate_payload_patterns = [](const std::vector<std::string>& patterns,
                                               std::string_view field)
        -> std::expected<void, std::string> {
        for (const auto& pattern : patterns) {
            if (pattern.empty() || pattern.starts_with('/')
                || std::filesystem::path(pattern).has_root_path()
                || std::ranges::any_of(std::filesystem::path(pattern),
                    [](const auto& part) { return part == ".."; })) {
                return std::unexpected(std::format(
                    "build.{} entries must be non-empty relative paths without '..': {}",
                    field, pattern));
            }
            if (pattern.starts_with("data/") || pattern == "data") {
                return std::unexpected(std::format(
                    "build.{} addresses the archive metadata prefix 'data/': {}",
                    field, pattern));
            }
        }
        return {};
    };
    if (auto result = validate_payload_patterns(r.managed_build.install_files, "install_files"); !result)
        return std::unexpected(result.error());
    if (auto result = validate_payload_patterns(r.managed_build.install_excludes, "install_excludes"); !result)
        return std::unexpected(result.error());
    if (auto result = validate_payload_patterns(r.managed_build.optional_excludes, "optional_excludes"); !result)
        return std::unexpected(result.error());
    const auto validate_payload_path = [](std::string_view path,
                                          std::string_view field,
                                          bool allow_absolute) -> std::expected<void, std::string> {
        const std::filesystem::path candidate(path);
        if (path.empty() || (!allow_absolute && candidate.is_absolute())
            || std::ranges::any_of(candidate, [](const auto& part) { return part == ".."; })) {
            return std::unexpected(std::format(
                "build.{} path must stay within the source/staging root: {}",
                field, path));
        }
        return {};
    };
    for (const auto& perm : r.managed_build.file_permissions) {
        if (auto result = validate_payload_path(perm.path, "file_permissions.path", false); !result)
            return std::unexpected(result.error());
    }
    for (const auto& copy : r.managed_build.install_copies) {
        if (auto result = validate_payload_path(copy.source, "install_copies.from", false); !result)
            return std::unexpected(result.error());
        if (auto result = validate_payload_path(copy.destination, "install_copies.to", false); !result)
            return std::unexpected(result.error());
    }
    for (const auto& move : r.managed_build.install_moves) {
        if (auto result = validate_payload_path(move.source, "install_moves.from", false); !result)
            return std::unexpected(result.error());
        if (auto result = validate_payload_path(move.destination, "install_moves.to", false); !result)
            return std::unexpected(result.error());
    }
    for (const auto& remove : r.managed_build.install_removes) {
        if (auto result = validate_payload_patterns({remove.path}, "install_removes"); !result)
            return std::unexpected(result.error());
    }
    for (const auto& generate : r.managed_build.install_generates) {
        if (auto result = validate_payload_path(generate.path, "install_generates.path", false); !result)
            return std::unexpected(result.error());
    }

    std::set<std::string> output_names;
    for (const auto& output : r.managed_build.outputs) {
        if (!output_names.insert(output.name).second)
            return std::unexpected("build.outputs contains duplicate name: " + output.name);
        if (output.name.find('/') != std::string::npos || output.name == "." || output.name == "..")
            return std::unexpected("build.outputs names must be simple package names");
        if (auto result = validate_payload_patterns(output.install_files, "outputs.install_files"); !result)
            return std::unexpected(result.error());
        if (auto result = validate_payload_patterns(output.install_excludes, "outputs.install_excludes"); !result)
            return std::unexpected(result.error());
        if (auto result = validate_payload_patterns(output.optional_excludes, "outputs.optional_excludes"); !result)
            return std::unexpected(result.error());
        if (output.install_files.empty())
            return std::unexpected("build.outputs entries require install_files");
        for (const auto& copy : output.install_copies) {
            if (auto result = validate_payload_path(copy.source, "outputs.install_copies.from", false); !result)
                return std::unexpected(result.error());
            if (auto result = validate_payload_path(copy.destination, "outputs.install_copies.to", false); !result)
                return std::unexpected(result.error());
        }
        for (const auto& move : output.install_moves) {
            if (auto result = validate_payload_path(move.source, "outputs.install_moves.from", false); !result)
                return std::unexpected(result.error());
            if (auto result = validate_payload_path(move.destination, "outputs.install_moves.to", false); !result)
                return std::unexpected(result.error());
        }
        for (const auto& remove : output.install_removes) {
            if (auto result = validate_payload_patterns({remove.path}, "outputs.install_removes"); !result)
                return std::unexpected(result.error());
        }
        for (const auto& generate : output.install_generates) {
            if (auto result = validate_payload_path(generate.path, "outputs.install_generates.path", false); !result)
                return std::unexpected(result.error());
        }
        for (const auto& perm : output.file_permissions) {
            if (auto result = validate_payload_path(perm.path, "outputs.file_permissions.path", false); !result)
                return std::unexpected(result.error());
        }
        for (const auto& link : output.install_symlinks) {
            if (auto result = validate_payload_path(link.path, "outputs.install_symlinks.path", false); !result)
                return std::unexpected(result.error());
            const std::filesystem::path target(link.target);
            const auto parent = std::filesystem::path(link.path).parent_path();
            const auto resolved = (parent / target).lexically_normal();
            if (link.target.empty() || target.is_absolute() || target.has_root_path()
                || resolved.is_absolute()
                || std::ranges::any_of(resolved, [](const auto& part) { return part == ".."; }))
                return std::unexpected(std::format(
                    "build.outputs.install_symlinks.target must remain inside the staging root: {}",
                    link.target));
        }
    }

    std::set<std::string> step_names;
    for (const auto& step : r.managed_build.steps) {
        if (!step_names.insert(step.name).second)
            return std::unexpected("build.steps contains duplicate name: " + step.name);
        if (step.command.find('\0') != std::string::npos)
            return std::unexpected("build.steps command contains NUL: " + step.name);
    }
    if (!r.managed_build.outputs.empty()
        && (!r.managed_build.install_files.empty()
            || !r.managed_build.install_excludes.empty()))
        return std::unexpected(
            "build.outputs and top-level install_files/install_excludes are mutually exclusive");
    if (r.managed_build.payload == PayloadMode::All
        && (!r.managed_build.install_files.empty()
            || !r.managed_build.install_excludes.empty()
            || !r.managed_build.outputs.empty()))
        return std::unexpected(
            "build.payload=all cannot be combined with an allowlist or outputs");
    if (r.managed_build.payload == PayloadMode::Allowlist
        && r.managed_build.install_files.empty())
        return std::unexpected(
            "build.payload=allowlist requires non-empty build.install_files");
    if (r.managed_build.payload == PayloadMode::Outputs
        && r.managed_build.outputs.empty())
        return std::unexpected(
            "build.payload=outputs requires at least one build.outputs entry");
    if (!r.managed_build.outputs.empty()
        && r.managed_build.payload != PayloadMode::Outputs)
        return std::unexpected(
            "build.outputs requires build.payload=outputs");
    for (const auto& link : r.managed_build.install_symlinks) {
        if (auto result = validate_payload_path(link.path, "install_symlinks.path", false); !result)
            return std::unexpected(result.error());
        const std::filesystem::path target(link.target);
        const auto parent = std::filesystem::path(link.path).parent_path();
        const auto resolved = (parent / target).lexically_normal();
        if (link.target.empty() || target.is_absolute() || target.has_root_path()
            || resolved.is_absolute()
            || std::ranges::any_of(resolved, [](const auto& part) { return part == ".."; }))
            return std::unexpected(std::format(
                "build.install_symlinks.target must remain inside the staging root: {}",
                link.target));
    }
    if ((r.name.ends_with("-libs") || r.name.ends_with("-dev"))
        && r.managed_build.payload != PayloadMode::Outputs
        && r.managed_build.install_files.empty()) {
        return std::unexpected(std::format(
            "Recipe v2 split package '{}' must declare build.install_files; "
            "the backend install tree is not an implicit package boundary",
            r.name));
    }

    if (bld->contains("toolchain") && !bld->get_as<vendor::toml::table>("toolchain"))
        return std::unexpected("build.toolchain must be a table");
    if (auto* suite = bld->get_as<vendor::toml::table>("toolchain")) {
        if (auto result = reject_unknown(*suite,
                {"compiler", "linker", "rust", "go"}, "build.toolchain"); !result)
            return std::unexpected(result.error());
        auto parse_tool = [&](std::string_view kind,
                              ToolRequirement& requirement,
                              std::initializer_list<std::string_view> families)
            -> std::expected<void, std::string> {
            if (suite->contains(kind) && !suite->get_as<vendor::toml::table>(kind))
                return std::unexpected(std::format("build.toolchain.{} must be a table", kind));
            auto* tool = suite->get_as<vendor::toml::table>(kind);
            if (!tool) return {};
            if (auto result = reject_unknown(*tool,
                    {"family", "package", "minimum_version"},
                    std::string("build.toolchain.") + std::string(kind)); !result)
                return std::unexpected(result.error());
            requirement.family = (*tool)["family"].value_or("");
            requirement.package = (*tool)["package"].value_or("");
            requirement.minimum_version = (*tool)["minimum_version"].value_or("");
            if (requirement.family.empty() || requirement.package.empty()
                || requirement.minimum_version.empty()) {
                return std::unexpected(std::format(
                    "build.toolchain.{} requires family, package and minimum_version", kind));
            }
            if (!std::ranges::contains(families, requirement.family)) {
                return std::unexpected(std::format(
                    "Unsupported build.toolchain.{} family '{}'", kind, requirement.family));
            }
            return {};
        };
        if (auto result = parse_tool("compiler", r.managed_build.compiler, {"clang", "gcc"}); !result)
            return std::unexpected(result.error());
        if (auto result = parse_tool("linker", r.managed_build.linker, {"lld", "mold", "ld"}); !result)
            return std::unexpected(result.error());
        if (suite->contains("rust")) {
            if (r.managed_build.system != BuildSystem::Cargo)
                return std::unexpected("build.toolchain.rust is valid only for Cargo recipes");
            if (auto result = parse_tool("rust", r.managed_build.rust, {"rustc"}); !result)
                return std::unexpected(result.error());
        }
        if (suite->contains("go")) {
            if (r.managed_build.system != BuildSystem::Go)
                return std::unexpected("build.toolchain.go is valid only for Go recipes");
            if (auto result = parse_tool("go", r.managed_build.go, {"go"}); !result)
                return std::unexpected(result.error());
        }

        if (!r.managed_build.compiler.family.empty()
            && !r.managed_build.allowed_compilers.empty()
            && !std::ranges::contains(r.managed_build.allowed_compilers, r.managed_build.compiler.family))
            return std::unexpected("Default compiler family is absent from allowed_compilers");
        if (!r.managed_build.linker.family.empty()
            && !r.managed_build.allowed_linkers.empty()
            && !std::ranges::contains(r.managed_build.allowed_linkers, r.managed_build.linker.family))
            return std::unexpected("Default linker family is absent from allowed_linkers");

        auto require_package = [&](const ToolRequirement& requirement) {
            const auto minimum = Version::parse(requirement.minimum_version);
            bool found = false;
            for (const auto& raw : r.build_deps) {
                const auto dependency = Dependency::parse(raw);
                if (dependency.name != requirement.package) continue;
                found = true;
                const bool strong_enough =
                    (dependency.op == ConstraintOp::Equal
                        || dependency.op == ConstraintOp::GreaterEqual)
                    && dependency.version >= minimum;
                if (!strong_enough) return std::expected<void, std::string>(
                    std::unexpected(std::format(
                        "build dependency '{}' does not guarantee toolchain minimum {} >= {}",
                        raw, requirement.package, requirement.minimum_version)));
            }
            if (!found) r.build_deps.push_back(std::format(
                "{} >= {}", requirement.package, requirement.minimum_version));
            return std::expected<void, std::string>{};
        };
        if (!r.managed_build.compiler.family.empty()) {
            if (auto result = require_package(r.managed_build.compiler); !result)
                return std::unexpected(result.error());
        }
        if (!r.managed_build.linker.family.empty()) {
            if (auto result = require_package(r.managed_build.linker); !result)
                return std::unexpected(result.error());
        }
        if (!r.managed_build.rust.family.empty()) {
            if (auto result = require_package(r.managed_build.rust); !result)
                return std::unexpected(result.error());
        }
        if (!r.managed_build.go.family.empty()) {
            if (auto result = require_package(r.managed_build.go); !result)
                return std::unexpected(result.error());
        }
    }

    return {};
}

} // namespace sage::package
