export module sage.package:version;

import std;
import sage.vendor.toml;

export namespace sage::package {

using std::uint32_t;
using std::uint64_t;
using std::size_t;

// Version model with standard epoch-ver-rel ordering.
struct Version {
    uint32_t epoch{0};
    std::string ver;
    std::string rel{"1"};

    static Version parse(std::string_view s) {
        Version v;
        if (s.empty()) return v;

        // Check for epoch (e.g. "1:2.0.0-1")
        if (auto colon = s.find(':'); colon != std::string_view::npos) {
            uint32_t ep = 0;
            for (char c : s.substr(0, colon)) {
                if (std::isdigit(static_cast<unsigned char>(c))) ep = ep * 10 + (c - '0');
            }
            v.epoch = ep;
            s = s.substr(colon + 1);
        }

        // Check for release (e.g. "2.0.0-1")
        if (auto dash = s.rfind('-'); dash != std::string_view::npos) {
            v.ver = std::string(s.substr(0, dash));
            v.rel = std::string(s.substr(dash + 1));
        } else {
            v.ver = std::string(s);
            v.rel = "1";
        }
        return v;
    }

    [[nodiscard]] std::string to_string() const {
        if (epoch > 0) {
            return std::format("{}:{}-{}", epoch, ver, rel);
        }
        return std::format("{}-{}", ver, rel);
    }

    // Alphanumeric segment comparator (vercmp)
    static int compare_segments(std::string_view a, std::string_view b) noexcept {
        size_t i = 0, j = 0;
        while (i < a.size() || j < b.size()) {
            while (i < a.size() && !std::isalnum(static_cast<unsigned char>(a[i]))) ++i;
            while (j < b.size() && !std::isalnum(static_cast<unsigned char>(b[j]))) ++j;
            if (i >= a.size() || j >= b.size()) {
                if (i >= a.size() && j >= b.size()) return 0;
                return (i >= a.size()) ? -1 : 1;
            }

            bool a_digit = std::isdigit(static_cast<unsigned char>(a[i]));
            bool b_digit = std::isdigit(static_cast<unsigned char>(b[j]));

            if (a_digit && b_digit) {
                // Numeric comparison
                size_t start_i = i, start_j = j;
                while (i < a.size() && std::isdigit(static_cast<unsigned char>(a[i]))) ++i;
                while (j < b.size() && std::isdigit(static_cast<unsigned char>(b[j]))) ++j;

                std::string_view sa = a.substr(start_i, i - start_i);
                std::string_view sb = b.substr(start_j, j - start_j);
                // Strip leading zeros
                while (sa.size() > 1 && sa.front() == '0') sa.remove_prefix(1);
                while (sb.size() > 1 && sb.front() == '0') sb.remove_prefix(1);

                if (sa.size() != sb.size()) {
                    return sa.size() < sb.size() ? -1 : 1;
                }
                if (sa != sb) {
                    return sa < sb ? -1 : 1;
                }
            } else if (!a_digit && !b_digit) {
                // Alpha segment comparison
                size_t start_i = i, start_j = j;
                while (i < a.size() && std::isalpha(static_cast<unsigned char>(a[i]))) ++i;
                while (j < b.size() && std::isalpha(static_cast<unsigned char>(b[j]))) ++j;

                std::string_view sa = a.substr(start_i, i - start_i);
                std::string_view sb = b.substr(start_j, j - start_j);
                if (sa != sb) {
                    return sa < sb ? -1 : 1;
                }
            } else {
                return a_digit ? 1 : -1;
            }
        }
        return 0;
    }

    std::strong_ordering operator<=>(const Version& other) const noexcept {
        if (epoch != other.epoch) {
            return epoch <=> other.epoch;
        }
        int vc = compare_segments(ver, other.ver);
        if (vc != 0) {
            return vc < 0 ? std::strong_ordering::less : std::strong_ordering::greater;
        }
        int rc = compare_segments(rel, other.rel);
        if (rc != 0) {
            return rc < 0 ? std::strong_ordering::less : std::strong_ordering::greater;
        }
        return std::strong_ordering::equal;
    }

    bool operator==(const Version& other) const noexcept {
        return (*this <=> other) == std::strong_ordering::equal;
    }
};

// Releases participate in monotonic publication identity selection, so their
// textual representation is intentionally narrower than upstream versions.
// Keep the parser here so recipes, manifests, and repository indexes enforce
// exactly the same positive-decimal contract.
inline std::expected<uint64_t, std::string> parse_release(std::string_view release) {
    uint64_t value = 0;
    if (release.empty()) return std::unexpected("Release must be a positive decimal integer");
    auto [end, error] = std::from_chars(release.data(), release.data() + release.size(), value);
    if (error != std::errc{} || end != release.data() + release.size() || value == 0) {
        return std::unexpected(std::format(
            "Invalid release '{}' (expected a positive decimal integer)", release));
    }
    return value;
}

// Architecture canonicalization shared by manifests, recipes and channel indexes.
inline bool package_architecture_matches(
    std::string_view package_architecture,
    std::string_view target_architecture)
{
    if (package_architecture == "any") return true;
    const auto canonical = [](std::string_view architecture) {
        return architecture == "x86_64" ? std::string_view{"amd64"} : architecture;
    };
    return canonical(package_architecture) == canonical(target_architecture);
}

inline std::expected<void, std::string> validate_package_architecture(
    std::string_view architecture)
{
    if (architecture == "amd64"
        || architecture == "x86_64"
        || architecture == "aarch64"
        || architecture == "any") {
        return {};
    }
    return std::unexpected(std::format(
        "Unsupported package architecture '{}'; expected amd64 (or legacy x86_64), aarch64, or any",
        architecture));
}

inline std::expected<std::string, std::string> parse_release_field(
    const vendor::toml::table& table,
    std::string_view fallback = "1")
{
    const auto* node = table.get("release");
    if (!node) return std::string(fallback);
    auto release = node->value<std::string_view>();
    if (!release) return std::unexpected("Release must be a TOML string when present");
    if (auto parsed = parse_release(*release); !parsed) {
        return std::unexpected(parsed.error());
    }
    return std::string(*release);
}

} // namespace sage::package
