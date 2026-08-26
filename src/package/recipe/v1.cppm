export module sage.package:recipe_v1;

import std;
import sage.vendor.toml;
import :deps;
import :recipe_types;

export namespace sage::package {

inline void parse_v1_legacy(const vendor::toml::table& tbl,
                            const std::vector<const vendor::toml::table*>& src_scopes,
                            Recipe& r)
{
    auto extract_cmds = [&](const char* key, std::vector<std::string>& dest) {
        if (auto* arr = tbl.get_as<vendor::toml::array>(key)) {
            for (auto&& c : *arr) {
                if (auto str = c.value<std::string_view>()) {
                    dest.emplace_back(*str);
                }
            }
        }
        if (auto* pkg = tbl.get_as<vendor::toml::table>("package")) {
            if (auto* arr = pkg->get_as<vendor::toml::array>(key)) {
                for (auto&& c : *arr) {
                    if (auto str = c.value<std::string_view>()) {
                        dest.emplace_back(*str);
                    }
                }
            }
        }
        for (const auto* src : src_scopes) {
            if (auto* arr = src->get_as<vendor::toml::array>(key)) {
                for (auto&& c : *arr) {
                    if (auto str = c.value<std::string_view>()) {
                        dest.emplace_back(*str);
                    }
                }
            }
        }
        if (auto* bld = tbl.get_as<vendor::toml::table>("build")) {
            if (auto* arr = bld->get_as<vendor::toml::array>(key)) {
                for (auto&& c : *arr) {
                    if (auto str = c.value<std::string_view>()) {
                        dest.emplace_back(*str);
                    }
                }
            }
        }
    };

    extract_cmds("prepare", r.prepare_cmds);
    extract_cmds("build", r.build_cmds);
    extract_cmds("install", r.install_cmds);

    if (auto* bld = tbl.get_as<vendor::toml::table>("build")) {
        if (auto v = (*bld)["cflags"].value<std::string_view>()) r.cflags = std::string(*v);
        if (auto v = (*bld)["cxxflags"].value<std::string_view>()) r.cxxflags = std::string(*v);
        if (auto v = (*bld)["cc"].value<std::string_view>()) r.cc = std::string(*v);
        if (auto v = (*bld)["cxx"].value<std::string_view>()) r.cxx = std::string(*v);
        if (auto v = (*bld)["ldflags"].value<std::string_view>()) r.ldflags = std::string(*v);
    }
}

} // namespace sage::package
