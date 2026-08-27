export module sage.package:recipe_v2_outputs;

import std;
import sage.vendor.toml;
import :version;
import :deps;
import :recipe_types;
import :recipe_v2_transforms;

export namespace sage::package {

inline std::expected<void, std::string> parse_v2_outputs(
    const vendor::toml::table& bld,
    Recipe& r)
{
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
    const auto parse_deps_strict = [&](const vendor::toml::table& table,
                                       std::string_view key,
                                       std::string_view scope,
                                       std::vector<Dependency>& target)
        -> std::expected<void, std::string> {
        std::vector<std::string> raw;
        if (auto result = parse_string_array_strict(table, key, scope, raw); !result)
            return std::unexpected(result.error());
        for (const auto& item : raw) {
            target.push_back(Dependency::parse(item));
        }
        return {};
    };

    if (bld.contains("outputs") && !bld.get_as<vendor::toml::array>("outputs"))
        return std::unexpected(std::string{"build.outputs must be an array"});
    if (auto* arr = bld.get_as<vendor::toml::array>("outputs")) {
        for (auto&& element : *arr) {
            auto* item = element.as_table();
            if (!item) return std::unexpected(std::string{"build.outputs entries must be tables"});
            if (auto result = reject_unknown_keys(*item,
                    {"name", "description", "license", "version", "release",
                     "channel", "arch", "inherit", "dependencies", "provides",
                     "conflicts", "conffiles", "install_files",
                     "install_excludes", "optional_excludes", "install_copies",
                     "install_symlinks", "install_moves", "install_removes",
                     "install_generates", "file_permissions"},
                    "build.outputs[]"); !result)
                return std::unexpected(result.error());
            auto name = (*item)["name"].value<std::string_view>();
            if (!name || name->empty()) return std::unexpected(
                std::string{"build.outputs entries require a non-empty name"});
            InstallOutput output;
            output.name = std::string(*name);
            const auto parse_output_string =
                [&](const char* key, std::optional<std::string>& target)
                -> std::expected<void, std::string> {
                if (!item->contains(key)) return {};
                auto value = (*item)[key].value<std::string_view>();
                if (!value) return std::unexpected(std::format(
                    "build.outputs.{} must be a string", key));
                target = std::string(*value);
                return {};
            };
            if (auto result = parse_output_string("description", output.description); !result)
                return std::unexpected(result.error());
            if (auto result = parse_output_string("license", output.license); !result)
                return std::unexpected(result.error());
            if (auto result = parse_output_string("version", output.version); !result)
                return std::unexpected(result.error());
            if (auto result = parse_output_string("release", output.release); !result)
                return std::unexpected(result.error());
            if (auto result = parse_output_string("channel", output.channel); !result)
                return std::unexpected(result.error());
            if (auto result = parse_output_string("arch", output.arch); !result)
                return std::unexpected(result.error());
            if (auto result = parse_string_array_strict(*item, "inherit",
                    "build.outputs[]", output.inherit); !result)
                return std::unexpected(result.error());
            if (auto result = parse_string_array_strict(*item, "install_files",
                    "build.outputs[]", output.install_files); !result)
                return std::unexpected(result.error());
            if (auto result = parse_string_array_strict(*item, "install_excludes",
                    "build.outputs[]", output.install_excludes); !result)
                return std::unexpected(result.error());
            if (auto result = parse_string_array_strict(*item, "optional_excludes",
                    "build.outputs[]", output.optional_excludes); !result)
                return std::unexpected(result.error());
            if (auto result = parse_copy_entries(*item, "install_copies",
                    "build.outputs[]", output.install_copies); !result)
                return std::unexpected(result.error());
            if (auto result = parse_symlink_entries(*item, "install_symlinks",
                    "build.outputs[]", output.install_symlinks); !result)
                return std::unexpected(result.error());
            if (auto result = parse_move_entries(*item, "install_moves",
                    "build.outputs[]", output.install_moves); !result)
                return std::unexpected(result.error());
            if (auto result = parse_remove_entries(*item, "install_removes",
                    "build.outputs[]", output.install_removes); !result)
                return std::unexpected(result.error());
            if (auto result = parse_generate_entries(*item, "install_generates",
                    "build.outputs[]", output.install_generates); !result)
                return std::unexpected(result.error());
            if (auto result = parse_file_permission_entries(*item, "file_permissions",
                    "build.outputs[]", output.file_permissions); !result)
                return std::unexpected(result.error());
            if (item->contains("dependencies")) {
                output.dependencies.emplace();
                if (auto result = parse_deps_strict(*item, "dependencies",
                        "build.outputs[]", *output.dependencies); !result)
                    return std::unexpected(result.error());
            }
            if (item->contains("conflicts")) {
                output.conflicts.emplace();
                if (auto result = parse_deps_strict(*item, "conflicts",
                        "build.outputs[]", *output.conflicts); !result)
                    return std::unexpected(result.error());
            }
            if (item->contains("provides")) {
                output.provides.emplace();
                if (auto result = parse_string_array_strict(*item, "provides",
                        "build.outputs[]", *output.provides); !result)
                    return std::unexpected(result.error());
                for (const auto& prov : *output.provides) {
                    auto entry = Dependency::parse(prov);
                    if (entry.name.empty() || entry.name == "."
                        || entry.name == ".."
                        || entry.name.find_first_of(" \t") != std::string::npos)
                        return std::unexpected(
                            std::string{"build.outputs.provides entries must be names or "
                                "'name <op> version': "} + prov);
                }
            }
            if (item->contains("conffiles")) {
                output.conffiles.emplace();
                if (auto result = parse_string_array_strict(*item, "conffiles",
                        "build.outputs[]", *output.conffiles); !result)
                    return std::unexpected(result.error());
            }
            r.managed_build.outputs.push_back(std::move(output));
        }
    }
    return {};
}

} // namespace sage::package
