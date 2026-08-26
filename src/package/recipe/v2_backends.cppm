export module sage.package:recipe_v2_backends;

import std;
import sage.vendor.toml;
import :version;
import :deps;
import :recipe_types;

export namespace sage::package {

inline std::expected<void, std::string> parse_backend_specs(
    const vendor::toml::table& bld,
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
    const auto parse_string_map_strict = [&](const vendor::toml::table& table,
                                             std::string_view key,
                                             std::string_view scope,
                                             std::map<std::string, std::string>& target)
        -> std::expected<void, std::string> {
        const auto* node = table.get(key);
        if (!node) return {};
        const auto* map = node->as_table();
        if (!map) return std::unexpected(std::format(
            "'{}.{}' must be a table", scope, key));
        for (const auto& [k, v] : *map) {
            if (auto s = v.value<std::string_view>()) {
                target.emplace(k.str(), std::string(*s));
            } else if (auto b = v.value<bool>()) {
                target.emplace(k.str(), *b ? "ON" : "OFF");
            } else if (auto i = v.value<std::int64_t>()) {
                target.emplace(k.str(), std::to_string(*i));
            } else {
                return std::unexpected(std::format(
                    "'{}.{}.{}' must be a string, boolean or integer", scope, key, k.str()));
            }
        }
        return {};
    };

            if (bld.contains("cmake") && !bld.get_as<vendor::toml::table>("cmake"))
                return std::unexpected("build.cmake must be a table");
            if (auto* spec = bld.get_as<vendor::toml::table>("cmake")) {
                if (auto result = reject_unknown(*spec,
                        {"definitions", "features", "build_type", "raw_options"},
                        "build.cmake"); !result)
                    return std::unexpected(result.error());
                CMakeBackendSpec cmake_spec;
                if (auto result = parse_string_map_strict(*spec, "definitions",
                        "build.cmake", cmake_spec.definitions); !result)
                    return std::unexpected(result.error());
                if (auto result = parse_string_array_strict(*spec, "features",
                        "build.cmake", cmake_spec.features); !result)
                    return std::unexpected(result.error());
                if (spec->contains("build_type")) {
                    auto bt = (*spec)["build_type"].value<std::string_view>();
                    if (!bt || bt->empty()) return std::unexpected("build.cmake.build_type must be a non-empty string");
                    cmake_spec.build_type = std::string(*bt);
                }
                if (auto result = parse_string_array_strict(*spec, "raw_options",
                        "build.cmake", cmake_spec.raw_options); !result)
                    return std::unexpected(result.error());
                r.managed_build.cmake = std::move(cmake_spec);
            }

            if (bld.contains("meson") && !bld.get_as<vendor::toml::table>("meson"))
                return std::unexpected("build.meson must be a table");
            if (auto* spec = bld.get_as<vendor::toml::table>("meson")) {
                if (auto result = reject_unknown(*spec,
                        {"options", "build_type", "raw_options"},
                        "build.meson"); !result)
                    return std::unexpected(result.error());
                MesonBackendSpec meson_spec;
                if (auto result = parse_string_map_strict(*spec, "options",
                        "build.meson", meson_spec.options); !result)
                    return std::unexpected(result.error());
                if (spec->contains("build_type")) {
                    auto bt = (*spec)["build_type"].value<std::string_view>();
                    if (!bt || bt->empty()) return std::unexpected("build.meson.build_type must be a non-empty string");
                    meson_spec.build_type = std::string(*bt);
                }
                if (auto result = parse_string_array_strict(*spec, "raw_options",
                        "build.meson", meson_spec.raw_options); !result)
                    return std::unexpected(result.error());
                r.managed_build.meson = std::move(meson_spec);
            }

            if (bld.contains("cargo") && !bld.get_as<vendor::toml::table>("cargo"))
                return std::unexpected("build.cargo must be a table");
            if (auto* spec = bld.get_as<vendor::toml::table>("cargo")) {
                if (auto result = reject_unknown(*spec,
                        {"features", "default_features", "locked", "raw_options"},
                        "build.cargo"); !result)
                    return std::unexpected(result.error());
                CargoBackendSpec cargo_spec;
                if (auto result = parse_string_array_strict(*spec, "features",
                        "build.cargo", cargo_spec.features); !result)
                    return std::unexpected(result.error());
                if (spec->contains("default_features")) {
                    auto df = (*spec)["default_features"].value<bool>();
                    if (!df) return std::unexpected("build.cargo.default_features must be a boolean");
                    cargo_spec.default_features = *df;
                }
                if (spec->contains("locked")) {
                    auto lk = (*spec)["locked"].value<bool>();
                    if (!lk) return std::unexpected("build.cargo.locked must be a boolean");
                    cargo_spec.locked = *lk;
                }
                if (auto result = parse_string_array_strict(*spec, "raw_options",
                        "build.cargo", cargo_spec.raw_options); !result)
                    return std::unexpected(result.error());
                r.managed_build.cargo = std::move(cargo_spec);
            }

            if (bld.contains("autotools") && !bld.get_as<vendor::toml::table>("autotools"))
                return std::unexpected("build.autotools must be a table");
            if (auto* spec = bld.get_as<vendor::toml::table>("autotools")) {
                if (auto result = reject_unknown(*spec,
                        {"enable", "disable", "with", "without", "raw_options"},
                        "build.autotools"); !result)
                    return std::unexpected(result.error());
                AutotoolsBackendSpec autotools_spec;
                if (auto result = parse_string_array_strict(*spec, "enable",
                        "build.autotools", autotools_spec.enable); !result)
                    return std::unexpected(result.error());
                if (auto result = parse_string_array_strict(*spec, "disable",
                        "build.autotools", autotools_spec.disable); !result)
                    return std::unexpected(result.error());
                if (auto result = parse_string_array_strict(*spec, "with",
                        "build.autotools", autotools_spec.with); !result)
                    return std::unexpected(result.error());
                if (auto result = parse_string_array_strict(*spec, "without",
                        "build.autotools", autotools_spec.without); !result)
                    return std::unexpected(result.error());
                if (auto result = parse_string_array_strict(*spec, "raw_options",
                        "build.autotools", autotools_spec.raw_options); !result)
                    return std::unexpected(result.error());
                r.managed_build.autotools = std::move(autotools_spec);
            }

            if (bld.contains("make") && !bld.get_as<vendor::toml::table>("make"))
                return std::unexpected("build.make must be a table");
            if (auto* spec = bld.get_as<vendor::toml::table>("make")) {
                if (auto result = reject_unknown(*spec,
                        {"targets", "install_targets", "variables", "raw_options"},
                        "build.make"); !result)
                    return std::unexpected(result.error());
                MakeBackendSpec make_spec;
                if (auto result = parse_string_array_strict(*spec, "targets",
                        "build.make", make_spec.targets); !result)
                    return std::unexpected(result.error());
                if (auto result = parse_string_array_strict(*spec, "install_targets",
                        "build.make", make_spec.install_targets); !result)
                    return std::unexpected(result.error());
                if (auto result = parse_string_map_strict(*spec, "variables",
                        "build.make", make_spec.variables); !result)
                    return std::unexpected(result.error());
                if (auto result = parse_string_array_strict(*spec, "raw_options",
                        "build.make", make_spec.raw_options); !result)
                    return std::unexpected(result.error());
                r.managed_build.make = std::move(make_spec);
            }

            if (bld.contains("xmake") && !bld.get_as<vendor::toml::table>("xmake"))
                return std::unexpected("build.xmake must be a table");
            if (auto* spec = bld.get_as<vendor::toml::table>("xmake")) {
                if (auto result = reject_unknown(*spec,
                        {"configs", "mode", "raw_options"},
                        "build.xmake"); !result)
                    return std::unexpected(result.error());
                XmakeBackendSpec xmake_spec;
                if (auto result = parse_string_map_strict(*spec, "configs",
                        "build.xmake", xmake_spec.configs); !result)
                    return std::unexpected(result.error());
                if (spec->contains("mode")) {
                    auto md = (*spec)["mode"].value<std::string_view>();
                    if (!md || md->empty()) return std::unexpected("build.xmake.mode must be a non-empty string");
                    xmake_spec.mode = std::string(*md);
                }
                if (auto result = parse_string_array_strict(*spec, "raw_options",
                        "build.xmake", xmake_spec.raw_options); !result)
                    return std::unexpected(result.error());
                r.managed_build.xmake = std::move(xmake_spec);
            }

    return {};
}

} // namespace sage::package
