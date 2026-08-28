module;
#include <cstdint>
#include <cctype>
#include <cstdlib>

export module sage.cli.build:probe;

import std;
import sage;

namespace sage::cli {

// -- Sage-managed build toolchain probing -----------------------------------
//
// Probe the executable itself instead of treating its configured filename as
// provenance: `cc` may be GCC or Clang, and a cross linker is commonly named
// `<triple>-ld`. The name is only a conservative fallback when --version does
// not identify a known family.
struct ToolProbe {
    std::string family;
    std::string version;
};

struct ProbeRemover {
    const std::filesystem::path& p;
    ~ProbeRemover() { std::error_code ec; std::filesystem::remove(p, ec); }
};

inline std::optional<ToolProbe> probe_tool(std::string_view tool, bool linker = false,
                                           std::string_view version_arg = "--version") {
    if (tool.empty()) return std::nullopt;
    std::filesystem::path out = std::filesystem::temp_directory_path()
        / std::format("sage-cc-probe-{}.txt", sage::util::current_pid());
    ProbeRemover remover{out};

    // Version probing is part of the reproducibility boundary.  A target
    // sysroot may not ship the caller's locale (for example C.UTF-8), and
    // shells can print a locale warning before the tool's real first line.
    // Force the portable C locale so that the version parser observes the
    // selected executable rather than an environment diagnostic.
    // Use shell-quoting even for administrator-provided executable names.
    // Double quotes would still expand `$()`/backticks from build.toml and
    // make version probing an unintended command-execution surface.
    const auto probe_command = "LC_ALL=C LANG=C "
        + sage::build::shell_quote(tool) + " " + std::string(version_arg)
        + " > " + sage::build::shell_quote(out.string()) + " 2>&1";
    int rc = std::system(probe_command.c_str());
    if (rc != 0) return std::nullopt;
    std::ifstream f(out);
    std::stringstream captured;
    captured << f.rdbuf();
    const std::string output = captured.str();
    if (output.empty()) return std::nullopt;
    const std::string line = output.substr(0, output.find('\n'));
    std::string lower = output;
    std::ranges::transform(lower, lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    std::string family;
    if (linker) {
        if (lower.contains("mold")) family = "mold";
        else if (lower.contains("lld")) family = "lld";
        else if (lower.contains("gnu ld") || lower.contains("gnu binutils"))
            family = "ld";
    } else {
        if (lower.contains("clang")) family = "clang";
        else if (lower.contains("rustc")) family = "rustc";
        else if (lower.contains("gcc")
                 || lower.contains("free software foundation")) family = "gcc";
    }
    if (family.empty()) {
        // `go version` reports "go version go1.24.1 linux/amd64": neither the
        // clang/gcc heuristics nor a leading digit apply, so recognize the
        // Go toolchain explicitly.
        if (output.starts_with("go version")) family = "go";
    }
    if (family.empty()) family = sage::build::tool_family(tool, linker);

    size_t i = 0;
    while (i < line.size()) {
        size_t j = line.find(' ', i);
        std::string_view tok(line.data() + i, (j == std::string::npos ? line.size() : j) - i);
        if (family == "go" && tok.starts_with("go")
            && tok.size() > 2
            && std::isdigit(static_cast<unsigned char>(tok[2]))) {
            return ToolProbe{std::move(family), std::string(tok.substr(2))};
        }
        if (!tok.empty() && std::isdigit(static_cast<unsigned char>(tok.front()))) {
            return ToolProbe{std::move(family), std::string(tok)};
        }
        i = (j == std::string::npos) ? line.size() : j + 1;
    }
    return ToolProbe{std::move(family), "unknown"};
}

inline bool probe_fakeroot(std::string_view executable) {
    if (executable.empty()) return false;
    const auto command = sage::build::shell_quote(executable)
        + " --version >/dev/null 2>&1";
    return std::system(command.c_str()) == 0;
}

inline bool compiler_like_executable(std::string_view path) {
    const auto base = std::filesystem::path(path).filename().string();
    if (base == "cc1" || base == "cc1plus" || base == "collect2"
        || base == "as" || base == "ar" || base == "ranlib"
        || base == "gcc-ar" || base == "gcc-nm" || base == "gcc-ranlib"
        || base == "llvm-ar" || base == "llvm-nm" || base == "llvm-ranlib"
        || base.ends_with("-tblgen")
        || (base.starts_with("clang-") && base.size() > 6 && !std::isdigit(static_cast<unsigned char>(base[6])))
        // clang-scan-deps is an analysis helper launched by the selected
        // clang driver, not an alternative compiler/linker.  It must be
        // visible to module dependency generation without being mistaken for
        // an unmanaged compiler implementation.
        || base == "clang-scan-deps"
        // Every dynamically linked executable enters through the system ELF
        // loader; it is not a linker invocation and is outside Sage's tool
        // selection boundary.
        || base.starts_with("ld-linux")) return false;
    for (const auto prefix : {std::string_view{"cc"}, std::string_view{"c++"},
                              std::string_view{"gcc"}, std::string_view{"g++"},
                              std::string_view{"clang"}, std::string_view{"clang++"},
                              std::string_view{"ld"}, std::string_view{"lld"},
                              std::string_view{"mold"}, std::string_view{"rustc"}}) {
        if (base.starts_with(prefix)
            && (base.size() == prefix.size()
                || base[prefix.size()] == '.'
                || (base[prefix.size()] == '-' && base.size() > prefix.size() + 1
                    && (std::isdigit(static_cast<unsigned char>(base[prefix.size() + 1]))
                        || base.ends_with("-ld") || base.ends_with("-gcc")
                        || base.ends_with("-g++") || base.ends_with("-clang")
                        || base.ends_with("-clang++")))
                || std::isdigit(static_cast<unsigned char>(base[prefix.size()]))))
            return true;
    }
    return false;
}


} // namespace sage::cli
