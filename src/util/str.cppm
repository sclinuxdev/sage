export module sage.util:str;

import std;

export namespace sage::util {

using std::size_t;

inline std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
        s.remove_suffix(1);
    }
    return s;
}

inline std::vector<std::string_view> split(std::string_view s, char delim) {
    std::vector<std::string_view> result;
    size_t start = 0;
    while (start < s.size()) {
        size_t end = s.find(delim, start);
        if (end == std::string_view::npos) {
            result.push_back(s.substr(start));
            break;
        }
        result.push_back(s.substr(start, end - start));
        start = end + 1;
    }
    return result;
}

inline std::string join(const std::vector<std::string>& list, std::string_view delimiter) {
    if (list.empty()) return {};
    std::string res = list[0];
    for (size_t i = 1; i < list.size(); ++i) {
        res += delimiter;
        res += list[i];
    }
    return res;
}

// Shell-style glob: '*' any run, '?' one character, '[abc]' / '[!abc]' a set.
// Iterative with a single backtrack point, so a pattern full of stars cannot
// turn a package listing into an exponential walk.
inline bool glob_match(std::string_view pattern, std::string_view text) noexcept {
    std::size_t p = 0, t = 0;
    std::size_t star = std::string_view::npos, star_t = 0;

    while (t < text.size()) {
        if (p < pattern.size() && pattern[p] == '[') {
            std::size_t close = pattern.find(']', p + 2);
            if (close == std::string_view::npos) return false;  // unterminated set: no match
            std::string_view set = pattern.substr(p + 1, close - p - 1);
            bool negate = !set.empty() && (set[0] == '!' || set[0] == '^');
            if (negate) set.remove_prefix(1);

            bool hit = false;
            for (std::size_t i = 0; i < set.size(); ++i) {
                if (i + 2 < set.size() && set[i + 1] == '-') {
                    if (text[t] >= set[i] && text[t] <= set[i + 2]) hit = true;
                    i += 2;
                } else if (set[i] == text[t]) {
                    hit = true;
                }
            }
            if (hit != negate) { p = close + 1; ++t; continue; }
        } else if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
            ++p; ++t;
            continue;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            star_t = t;
            continue;
        }

        if (star == std::string_view::npos) return false;
        p = star + 1;
        t = ++star_t;
    }

    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

// Human-readable byte count. Binary units, because that is what a filesystem
// reports back when someone goes to check.
inline std::string format_size(std::uintmax_t bytes) {
    constexpr std::string_view units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    auto value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(units)) {
        value /= 1024.0;
        ++unit;
    }
    return unit == 0 ? std::format("{} {}", bytes, units[0])
                     : std::format("{:.1f} {}", value, units[unit]);
}

} // namespace sage::util
