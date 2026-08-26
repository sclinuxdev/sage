export module sage.package:recipe;

import std;
import sage.vendor.toml;
import :version;
import :deps;
import :trigger;
export import :recipe_types;
export import :recipe_v1;
export import :recipe_v2_transforms;
export import :recipe_v2_backends;
export import :recipe_v2_outputs;
export import :recipe_v2;

export namespace sage::package {

inline std::expected<Recipe, std::string> Recipe::parse_toml(std::string_view toml_content) {
    auto tbl_res = vendor::toml::parse_string(toml_content);
    if (!tbl_res) return std::unexpected(tbl_res.error());
    const auto& tbl = *tbl_res;

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

    Recipe r;
    if (tbl.contains("schema_version")
        && !tbl["schema_version"].value<std::int64_t>())
        return std::unexpected("schema_version must be an integer");
    r.schema_version = static_cast<uint32_t>(tbl["schema_version"].value_or(1LL));
    if (r.schema_version != 1 && r.schema_version != 2) {
        return std::unexpected(std::format(
            "Unsupported recipe schema_version {}; expected 1 or 2",
            r.schema_version));
    }

    if (r.schema_version == 2) {
        if (auto result = reject_unknown(tbl,
                {"schema_version", "package", "upstream", "source",
                 "build", "capability_hooks", "triggers", "sysusers",
                 "alternatives"}, "recipe"); !result)
            return std::unexpected(result.error());
    }

    if (auto* pkg = tbl.get_as<vendor::toml::table>("package")) {
        if (r.schema_version == 2) {
            if (auto result = reject_unknown(*pkg,
                    {"name", "version", "release", "description", "license",
                     "channel", "arch", "dependencies", "build_dependencies",
                     "check_dependencies", "provides", "conflicts", "conffiles",
                     "upstream", "upstream_regex"}, "package"); !result)
                return std::unexpected(result.error());
        }
        r.name = (*pkg)["name"].value_or("");
        std::string ver_str = std::string((*pkg)["version"].value_or(""));
        r.version = Version::parse(ver_str);
        auto release = parse_release_field(*pkg);
        if (!release) return std::unexpected(release.error());
        r.version.rel = std::move(*release);
        r.description = (*pkg)["description"].value_or("");
        r.license = (*pkg)["license"].value_or("");
        r.channel = (*pkg)["channel"].value_or("system");
        r.arch = (*pkg)["arch"].value_or("x86_64");
        r.upstream.url = (*pkg)["upstream"].value_or("");
        r.upstream.version_regex = (*pkg)["upstream_regex"].value_or("");
        auto architecture = validate_package_architecture(r.arch);
        if (!architecture) return std::unexpected(architecture.error());
    } else {
        return std::unexpected("Missing [package] section in recipe");
    }

    if (r.schema_version == 2 && tbl.contains("upstream")
        && !tbl.get_as<vendor::toml::table>("upstream"))
        return std::unexpected("recipe.upstream must be a table");
    if (auto* upstream = tbl.get_as<vendor::toml::table>("upstream")) {
        if (r.schema_version == 2) {
            if (auto result = reject_unknown(*upstream,
                    {"url", "version_regex"}, "upstream"); !result)
                return std::unexpected(result.error());
        }
        if (upstream->contains("url")) {
            auto u = (*upstream)["url"].value<std::string_view>();
            if (!u || u->empty()) return std::unexpected("upstream.url must be a non-empty string");
            r.upstream.url = std::string(*u);
        }
        if (upstream->contains("version_regex")) {
            auto re = (*upstream)["version_regex"].value<std::string_view>();
            if (!re || re->empty()) return std::unexpected("upstream.version_regex must be a non-empty string");
            r.upstream.version_regex = std::string(*re);
        }
    }

    std::vector<const vendor::toml::table*> src_scopes;
    if (auto* s = tbl.get_as<vendor::toml::table>("source")) {
        src_scopes.push_back(s);
        if (r.schema_version == 2) {
            if (auto result = reject_unknown(*s, {"url", "sha256"}, "source"); !result)
                return std::unexpected(result.error());
        }
        r.source_url = (*s)["url"].value_or("");
        r.source_sha256 = (*s)["sha256"].value_or("");
    }
    if (auto* arr = tbl.get_as<vendor::toml::array>("source")) {
        for (auto&& el : *arr) {
            if (auto* s = el.as_table()) {
                src_scopes.push_back(s);
                if (r.schema_version == 2) {
                    if (auto result = reject_unknown(*s, {"url", "sha256"}, "source[]"); !result)
                        return std::unexpected(result.error());
                }
                std::string u = std::string((*s)["url"].value_or(""));
                std::string h = std::string((*s)["sha256"].value_or(""));
                if (r.source_url.empty()) {
                    r.source_url = std::move(u);
                    r.source_sha256 = std::move(h);
                } else {
                    r.extra_sources.push_back(ExtraSource{
                        .url = std::move(u),
                        .sha256 = std::move(h),
                    });
                }
            }
        }
    }

    const auto parse_strings = [](const vendor::toml::table& table,
                                  std::string_view key,
                                  std::vector<std::string>& target) {
        if (auto* arr = table.get_as<vendor::toml::array>(key)) {
            for (auto&& item : *arr) {
                if (auto s = item.value<std::string_view>()) {
                    target.emplace_back(*s);
                }
            }
        }
    };
    const auto parse_deps = [](const vendor::toml::table& table,
                               std::string_view key,
                               std::vector<Dependency>& target) {
        if (auto* arr = table.get_as<vendor::toml::array>(key)) {
            for (auto&& item : *arr) {
                if (auto s = item.value<std::string_view>()) {
                    target.push_back(Dependency::parse(*s));
                }
            }
        }
    };
    const auto parse_deps_strict = [](const vendor::toml::table& table,
                                      std::string_view key,
                                      std::string_view scope,
                                      std::vector<Dependency>& target)
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
            target.push_back(Dependency::parse(std::string(*s)));
        }
        return {};
    };
    const auto parse_string_array_strict = [](const vendor::toml::table& table,
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

    if (r.schema_version == 1) {
        if (auto* pkg = tbl.get_as<vendor::toml::table>("package")) {
            parse_deps(*pkg, "dependencies", r.host_deps);
            parse_deps(*pkg, "conflicts", r.conflicts);
            parse_strings(*pkg, "build_dependencies", r.build_deps);
            parse_strings(*pkg, "provides", r.provides);
            parse_strings(*pkg, "conffiles", r.conffiles);
        }
        parse_deps(tbl, "dependencies", r.host_deps);
        parse_deps(tbl, "conflicts", r.conflicts);
        parse_strings(tbl, "build_dependencies", r.build_deps);
        parse_strings(tbl, "provides", r.provides);
        parse_strings(tbl, "conffiles", r.conffiles);
        for (const auto* src : src_scopes) {
            parse_deps(*src, "dependencies", r.host_deps);
            parse_deps(*src, "conflicts", r.conflicts);
            parse_strings(*src, "build_dependencies", r.build_deps);
            parse_strings(*src, "provides", r.provides);
            parse_strings(*src, "conffiles", r.conffiles);
        }

        parse_v1_legacy(tbl, src_scopes, r);
    } else {
        auto pkg = tbl.get_as<vendor::toml::table>("package");
        r.host_deps.clear();
        r.conflicts.clear();
        r.build_deps.clear();
        r.check_deps.clear();
        r.provides.clear();
        r.conffiles.clear();
        if (auto result = parse_deps_strict(*pkg, "dependencies", "package",
                                            r.host_deps); !result)
            return std::unexpected(result.error());
        if (auto result = parse_deps_strict(*pkg, "conflicts", "package",
                                            r.conflicts); !result)
            return std::unexpected(result.error());
        if (auto result = parse_string_array_strict(*pkg, "build_dependencies",
                "package", r.build_deps); !result)
            return std::unexpected(result.error());
        if (auto result = parse_string_array_strict(*pkg, "check_dependencies",
                "package", r.check_deps); !result)
            return std::unexpected(result.error());
        if (auto result = parse_string_array_strict(*pkg, "provides",
                "package", r.provides); !result)
            return std::unexpected(result.error());
        if (auto result = parse_string_array_strict(*pkg, "conffiles",
                "package", r.conffiles); !result)
            return std::unexpected(result.error());
        for (const auto& prov : r.provides) {
            auto entry = Dependency::parse(prov);
            const auto well_formed = !entry.name.empty()
                && entry.name != "." && entry.name != ".."
                && entry.name.find_first_of(" \t") == std::string::npos;
            if (!well_formed)
                return std::unexpected(
                    "package.provides entries must be names or 'name <op> version': "
                    + prov);
        }

        if (tbl.contains("sysusers")
            && !tbl.get_as<vendor::toml::array>("sysusers"))
            return std::unexpected("sysusers must be an array of tables");
        if (auto* arr = tbl.get_as<vendor::toml::array>("sysusers")) {
            for (auto&& element : *arr) {
                auto* item = element.as_table();
                if (!item) return std::unexpected("sysusers entries must be inline tables");
                if (auto result = reject_unknown(*item,
                        {"type", "name", "id", "description", "home", "shell", "group"},
                        "sysusers[]"); !result)
                    return std::unexpected(result.error());
                auto type = (*item)["type"].value<std::string_view>();
                auto name = (*item)["name"].value<std::string_view>();
                if (!type || (*type != "user" && *type != "group"))
                    return std::unexpected("sysusers entries require type = \"user\" or \"group\"");
                if (!name || name->empty() || name->find('/') != std::string::npos
                    || *name == "." || *name == "..")
                    return std::unexpected("sysusers entries require a simple non-empty name");
                SysUserEntry entry;
                entry.type = std::string(*type);
                entry.name = std::string(*name);
                if (item->contains("id")) {
                    auto id = (*item)["id"].value<std::int64_t>();
                    if (!id || *id <= 0 || *id > 0x7FFFFFFF)
                        return std::unexpected(std::format(
                            "sysusers id for '{}' must be a positive integer system uid/gid", entry.name));
                    entry.id = static_cast<uint32_t>(*id);
                }
                entry.description = (*item)["description"].value_or("");
                entry.home = (*item)["home"].value_or("");
                entry.shell = (*item)["shell"].value_or("");
                entry.group = (*item)["group"].value_or("");
                if (entry.type == "group"
                    && (!entry.home.empty() || !entry.shell.empty() || !entry.group.empty()))
                    return std::unexpected(std::format(
                        "sysusers group entry '{}' cannot declare home, shell or group", entry.name));
                r.sysusers.push_back(std::move(entry));
            }
            std::set<std::string> sysuser_names;
            for (const auto& entry : r.sysusers) {
                if (!sysuser_names.insert(entry.name).second)
                    return std::unexpected("sysusers declares the same name more than once: " + entry.name);
            }
        }

        if (tbl.contains("alternatives")
            && !tbl.get_as<vendor::toml::array>("alternatives"))
            return std::unexpected("alternatives must be an array of tables");
        if (auto* arr = tbl.get_as<vendor::toml::array>("alternatives")) {
            for (auto&& element : *arr) {
                auto* item = element.as_table();
                if (!item) return std::unexpected("alternatives entries must be inline tables");
                if (auto result = reject_unknown(*item,
                        {"link", "target", "priority"}, "alternatives[]"); !result)
                    return std::unexpected(result.error());
                auto link = (*item)["link"].value<std::string_view>();
                auto target = (*item)["target"].value<std::string_view>();
                if (!link || link->empty() || link->starts_with('/')
                    || std::ranges::any_of(std::filesystem::path(std::string(*link)),
                        [](const auto& part) { return part == ".."; }))
                    return std::unexpected("alternatives entries require a relative link path");
                if (!target || target->empty() || target->starts_with('/'))
                    return std::unexpected("alternatives entries require a relative target");
                int priority = 50;
                if (item->contains("priority")) {
                    auto value = (*item)["priority"].value<std::int64_t>();
                    if (!value || *value < 0 || *value > 1000)
                        return std::unexpected("alternatives priority must be between 0 and 1000");
                    priority = static_cast<int>(*value);
                }
                r.alternatives.push_back(AlternativeEntry{
                    .link = std::string(*link),
                    .target = std::string(*target),
                    .priority = priority});
            }
            std::set<std::string> alternative_links;
            for (const auto& alt : r.alternatives) {
                if (!alternative_links.insert(alt.link).second)
                    return std::unexpected("alternatives declares the same link more than once: " + alt.link);
            }
        }

        const auto valid_sha256 = [](std::string_view value) {
            return value.size() == 64 && std::ranges::all_of(value, [](char c) {
                return std::isxdigit(static_cast<unsigned char>(c));
            });
        };
        if (!r.source_url.empty() && !valid_sha256(r.source_sha256)) {
            return std::unexpected("Recipe v2 requires a 64-hex source.sha256 when source.url is present");
        }
        for (const auto& source : r.extra_sources) {
            if (source.url.empty() || !valid_sha256(source.sha256)) {
                return std::unexpected("Recipe v2 [[source]] entries require a URL and a 64-hex sha256");
            }
        }

        if (auto result = parse_managed_build(tbl, src_scopes, r); !result)
            return std::unexpected(result.error());
    }

    parse_capability_hooks(tbl, r.capability_hooks);
    if (auto trig_res = parse_triggers(tbl, r.triggers); !trig_res) {
        return std::unexpected(trig_res.error());
    }

    return r;
}

} // namespace sage::package
