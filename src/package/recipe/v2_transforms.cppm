export module sage.package:recipe_v2_transforms;

import std;
import sage.vendor.toml;
import :version;
import :deps;
import :recipe_types;

export namespace sage::package {

inline std::expected<void, std::string> reject_unknown_keys(
    const vendor::toml::table& table,
    std::initializer_list<std::string_view> allowed,
    std::string_view scope)
{
    for (const auto& [key, _] : table) {
        if (!std::ranges::contains(allowed, key.str())) {
            return std::unexpected(std::format(
                "Unknown key '{}' in {}", key.str(), scope));
        }
    }
    return {};
}

inline std::expected<void, std::string> parse_copy_entries(
    const vendor::toml::table& scope_tbl,
    const char* key,
    std::string_view scope_name,
    std::vector<InstallCopy>& target)
{
    if (auto* arr = scope_tbl.get_as<vendor::toml::array>(key)) {
        for (auto&& element : *arr) {
            auto* item = element.as_table();
            if (!item) return std::unexpected(std::format(
                "{}.{} entries must be inline tables", scope_name, key));
            if (auto result = reject_unknown_keys(*item, {"from", "to"},
                    std::format("{}.{}[]", scope_name, key)); !result)
                return std::unexpected(result.error());
            auto source = (*item)["from"].value<std::string_view>();
            auto destination = (*item)["to"].value<std::string_view>();
            if (!source || !destination || source->empty()
                || destination->empty()) return std::unexpected(std::format(
                    "{}.{} entries require non-empty from and to", scope_name, key));
            target.push_back({std::string(*source), std::string(*destination)});
        }
    }
    return {};
}

inline std::expected<void, std::string> parse_symlink_entries(
    const vendor::toml::table& scope_tbl,
    const char* key,
    std::string_view scope_name,
    std::vector<InstallSymlink>& target)
{
    if (auto* arr = scope_tbl.get_as<vendor::toml::array>(key)) {
        for (auto&& element : *arr) {
            auto* item = element.as_table();
            if (!item) return std::unexpected(std::format(
                "{}.{} entries must be inline tables", scope_name, key));
            if (auto result = reject_unknown_keys(*item, {"path", "target"},
                    std::format("{}.{}[]", scope_name, key)); !result)
                return std::unexpected(result.error());
            auto path = (*item)["path"].value<std::string_view>();
            auto target_path = (*item)["target"].value<std::string_view>();
            if (!path || !target_path || path->empty()
                || target_path->empty()) return std::unexpected(std::format(
                    "{}.{} entries require non-empty path and target", scope_name, key));
            target.push_back({std::string(*path), std::string(*target_path)});
        }
    }
    return {};
}

inline std::expected<void, std::string> parse_move_entries(
    const vendor::toml::table& scope_tbl,
    const char* key,
    std::string_view scope_name,
    std::vector<InstallMove>& target)
{
    if (auto* arr = scope_tbl.get_as<vendor::toml::array>(key)) {
        for (auto&& element : *arr) {
            auto* item = element.as_table();
            if (!item) return std::unexpected(std::format(
                "{}.{} entries must be inline tables", scope_name, key));
            if (auto result = reject_unknown_keys(*item, {"from", "to"},
                    std::format("{}.{}[]", scope_name, key)); !result)
                return std::unexpected(result.error());
            auto source = (*item)["from"].value<std::string_view>();
            auto destination = (*item)["to"].value<std::string_view>();
            if (!source || !destination || source->empty()
                || destination->empty()) return std::unexpected(std::format(
                    "{}.{} entries require non-empty from and to", scope_name, key));
            target.push_back({std::string(*source), std::string(*destination)});
        }
    }
    return {};
}

inline std::expected<void, std::string> parse_remove_entries(
    const vendor::toml::table& scope_tbl,
    const char* key,
    std::string_view scope_name,
    std::vector<InstallRemove>& target)
{
    if (auto* arr = scope_tbl.get_as<vendor::toml::array>(key)) {
        for (auto&& element : *arr) {
            if (auto s = element.value<std::string_view>()) {
                if (s->empty()) return std::unexpected(std::format(
                    "{}.{} entries require a non-empty path", scope_name, key));
                target.push_back({std::string(*s)});
            } else if (auto* item = element.as_table()) {
                if (auto result = reject_unknown_keys(*item, {"path"},
                        std::format("{}.{}[]", scope_name, key)); !result)
                    return std::unexpected(result.error());
                auto path = (*item)["path"].value<std::string_view>();
                if (!path || path->empty()) return std::unexpected(std::format(
                    "{}.{} entries require a non-empty path", scope_name, key));
                target.push_back({std::string(*path)});
            } else {
                return std::unexpected(std::format(
                    "{}.{} entries must be strings or inline tables", scope_name, key));
            }
        }
    }
    return {};
}

inline std::expected<void, std::string> parse_generate_entries(
    const vendor::toml::table& scope_tbl,
    const char* key,
    std::string_view scope_name,
    std::vector<InstallGenerate>& target)
{
    if (auto* arr = scope_tbl.get_as<vendor::toml::array>(key)) {
        for (auto&& element : *arr) {
            auto* item = element.as_table();
            if (!item) return std::unexpected(std::format(
                "{}.{} entries must be inline tables", scope_name, key));
            if (auto result = reject_unknown_keys(*item,
                    {"path", "content", "mode"}, std::format("{}.{}[]", scope_name, key)); !result)
                return std::unexpected(result.error());
            auto path = (*item)["path"].value<std::string_view>();
            auto content = (*item)["content"].value<std::string_view>();
            if (!path || !content || path->empty()) return std::unexpected(std::format(
                "{}.{} entries require non-empty path and content", scope_name, key));
            auto mode = (*item)["mode"].value<std::int64_t>().value_or(0644);
            if (mode < 0 || mode > 07777) return std::unexpected(std::format(
                "{}.{} entries require valid octal mode", scope_name, key));
            target.push_back({std::string(*path), std::string(*content), static_cast<uint32_t>(mode)});
        }
    }
    return {};
}

inline std::expected<void, std::string> parse_file_permission_entries(
    const vendor::toml::table& scope_tbl,
    const char* key,
    std::string_view scope_name,
    std::vector<FilePermission>& target)
{
    if (auto* arr = scope_tbl.get_as<vendor::toml::array>(key)) {
        for (auto&& element : *arr) {
            auto* item = element.as_table();
            if (!item) return std::unexpected(std::format(
                "{}.{} entries must be inline tables", scope_name, key));
            if (auto result = reject_unknown_keys(*item,
                    {"path", "mode", "uid", "gid", "caps", "user", "group"},
                    std::format("{}.{}[]", scope_name, key)); !result)
                return std::unexpected(result.error());
            auto path = (*item)["path"].value<std::string_view>();
            if (!path || path->empty()) return std::unexpected(std::format(
                "{}.{} entries require a non-empty path", scope_name, key));
            if (item->contains("mode")
                && !(*item)["mode"].value<std::int64_t>())
                return std::unexpected(std::format(
                    "{}.{}.mode must be an integer", scope_name, key));
            if (item->contains("uid")
                && !(*item)["uid"].value<std::int64_t>())
                return std::unexpected(std::format(
                    "{}.{}.uid must be an integer", scope_name, key));
            if (item->contains("gid")
                && !(*item)["gid"].value<std::int64_t>())
                return std::unexpected(std::format(
                    "{}.{}.gid must be an integer", scope_name, key));
            if (item->contains("user")
                && !(*item)["user"].value<std::string_view>())
                return std::unexpected(std::format(
                    "{}.{}.user must be a string", scope_name, key));
            if (item->contains("group")
                && !(*item)["group"].value<std::string_view>())
                return std::unexpected(std::format(
                    "{}.{}.group must be a string", scope_name, key));
            if (item->contains("uid") && item->contains("user"))
                return std::unexpected(std::format(
                    "{}.{} entries cannot set both uid and user",
                    scope_name, key));
            if (item->contains("gid") && item->contains("group"))
                return std::unexpected(std::format(
                    "{}.{} entries cannot set both gid and group",
                    scope_name, key));
            auto mode = (*item)["mode"].value<std::int64_t>().value_or(0644);
            auto uid = (*item)["uid"].value<std::int64_t>().value_or(0);
            auto gid = (*item)["gid"].value<std::int64_t>().value_or(0);
            auto caps = (*item)["caps"].value<std::string_view>().value_or("");
            auto user = (*item)["user"].value<std::string_view>().value_or("");
            auto group = (*item)["group"].value<std::string_view>().value_or("");
            if (mode < 0 || mode > 07777 || uid < 0 || gid < 0
                || user.find('/') != std::string::npos
                || group.find('/') != std::string::npos)
                return std::unexpected(std::format(
                    "{}.{} entries require valid mode, uid and gid", scope_name, key));
            target.push_back(FilePermission{
                .path = std::string(*path),
                .mode = static_cast<uint32_t>(mode),
                .uid = static_cast<uint32_t>(uid),
                .gid = static_cast<uint32_t>(gid),
                .caps = std::string(caps),
                .user = std::string(user),
                .group = std::string(group),
            });
        }
    }
    return {};
}

} // namespace sage::package
