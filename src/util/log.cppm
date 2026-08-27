export module sage.util:log;

import std;

export namespace sage::util {

namespace color {
    inline constexpr std::string_view reset   = "\033[0m";
    inline constexpr std::string_view bold    = "\033[1m";
    inline constexpr std::string_view dim     = "\033[2m";
    inline constexpr std::string_view red     = "\033[31m";
    inline constexpr std::string_view green   = "\033[32m";
    inline constexpr std::string_view yellow  = "\033[33m";
    inline constexpr std::string_view blue    = "\033[34m";
    inline constexpr std::string_view magenta = "\033[35m";
    inline constexpr std::string_view cyan    = "\033[36m";
    inline constexpr std::string_view white   = "\033[37m";
}

inline void write_line(std::ostream& stream, std::string_view text) {
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    stream.put('\n');
}

template <typename... Args>
inline void print_line(std::format_string<Args...> fmt, Args&&... args) {
    write_line(std::cout, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
inline void print_error_line(std::format_string<Args...> fmt, Args&&... args) {
    write_line(std::cerr, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
inline void log_info(std::format_string<Args...> fmt, Args&&... args) {
    const auto msg = std::format(fmt, std::forward<Args>(args)...);
    write_line(std::cout, std::format("{}{}::{}{}", color::cyan, color::bold, color::reset, msg));
}

template <typename... Args>
inline void log_success(std::format_string<Args...> fmt, Args&&... args) {
    const auto msg = std::format(fmt, std::forward<Args>(args)...);
    write_line(std::cout, std::format("{}{}✓{}{}", color::green, color::bold, color::reset, msg));
}

template <typename... Args>
inline void log_warn(std::format_string<Args...> fmt, Args&&... args) {
    const auto msg = std::format(fmt, std::forward<Args>(args)...);
    write_line(std::cerr, std::format("{}{}warning:{}{}", color::yellow, color::bold, color::reset, msg));
}

template <typename... Args>
inline void log_error(std::format_string<Args...> fmt, Args&&... args) {
    const auto msg = std::format(fmt, std::forward<Args>(args)...);
    write_line(std::cerr, std::format("{}{}error:{}{}", color::red, color::bold, color::reset, msg));
}

} // namespace sage::util
