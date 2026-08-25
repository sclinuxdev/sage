module;

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

export module sage.service;

import std;
import sage.vendor.toml;
import sage.util;

export namespace sage::service {

using std::uint32_t;
using std::size_t;

class OwnedFd {
public:
    explicit OwnedFd(int fd = -1) noexcept : fd_(fd) {}
    ~OwnedFd() { if (fd_ >= 0) ::close(fd_); }
    OwnedFd(const OwnedFd&) = delete;
    OwnedFd& operator=(const OwnedFd&) = delete;
    OwnedFd(OwnedFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    OwnedFd& operator=(OwnedFd&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) ::close(fd_);
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    [[nodiscard]] int get() const noexcept { return fd_; }
private:
    int fd_;
};

class TemporaryPath {
public:
    explicit TemporaryPath(std::filesystem::path path) : path_(std::move(path)) {}
    ~TemporaryPath() {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }
    [[nodiscard]] const std::filesystem::path& get() const noexcept { return path_; }
private:
    std::filesystem::path path_;
};

enum class InitType {
    OpenRC,
    Runit,
    Systemd,
    Dinit,
    S6,
    Loom,
    Unknown
};

inline InitType parse_init_type(std::string_view s) noexcept {
    if (s == "openrc" || s == "OpenRC") return InitType::OpenRC;
    if (s == "runit" || s == "Runit") return InitType::Runit;
    if (s == "systemd" || s == "Systemd") return InitType::Systemd;
    if (s == "dinit" || s == "Dinit") return InitType::Dinit;
    if (s == "s6" || s == "S6") return InitType::S6;
    if (s == "loom" || s == "Loom") return InitType::Loom;
    return InitType::Unknown;
}

inline std::string_view to_string(InitType t) noexcept {
    switch (t) {
        case InitType::OpenRC:  return "openrc";
        case InitType::Runit:   return "runit";
        case InitType::Systemd: return "systemd";
        case InitType::Dinit:   return "dinit";
        case InitType::S6:      return "s6";
        case InitType::Loom:    return "loom";
        default:                return "unknown";
    }
}

struct ServiceSpec {
    uint32_t schema_version{1};
    std::string name;
    std::string description;
    std::string exec_start;
    std::string exec_stop;
    std::string exec_reload;
    std::vector<std::string> command;
    std::vector<std::string> stop_command;
    std::vector<std::string> reload_command;
    std::string user{"root"};
    std::string group{"root"};
    std::string working_dir{"/"};
    std::string pid_file;
    std::string restart{"always"}; // "always", "on-failure", "no"
    std::string type{"simple"};     // "simple", "forking"
    std::vector<std::string> after;
    std::vector<std::string> before;

    static std::expected<ServiceSpec, std::string> parse_toml(std::string_view toml_str) {
        auto tbl_res = vendor::toml::parse_string(toml_str);
        if (!tbl_res) return std::unexpected(tbl_res.error());
        const auto& tbl = *tbl_res;

        ServiceSpec s;
        s.schema_version = static_cast<uint32_t>(tbl["schema_version"].value_or(1LL));
        if (s.schema_version != 1 && s.schema_version != 2) {
            return std::unexpected(std::format(
                "Unsupported service.toml schema {}", s.schema_version));
        }
        if (auto* svc = tbl.get_as<vendor::toml::table>("service")) {
            s.name = (*svc)["name"].value_or("");
            s.description = (*svc)["description"].value_or("");
            s.exec_start = (*svc)["exec_start"].value_or("");
            s.exec_stop = (*svc)["exec_stop"].value_or("");
            s.exec_reload = (*svc)["exec_reload"].value_or("");
            const auto parse_argv = [&](std::string_view key, std::vector<std::string>& target) {
                if (auto* argv = svc->get_as<vendor::toml::array>(key)) {
                    for (auto&& argument : *argv) {
                        if (auto value = argument.value<std::string_view>()) {
                            target.emplace_back(*value);
                        }
                    }
                }
            };
            parse_argv("command", s.command);
            parse_argv("stop_command", s.stop_command);
            parse_argv("reload_command", s.reload_command);
            if (s.schema_version == 2) {
                if (!s.exec_start.empty() || !s.exec_stop.empty() || !s.exec_reload.empty()) {
                    return std::unexpected(
                        "service.toml schema 2 uses command arrays, not exec_* strings");
                }
                s.exec_start = util::join(s.command, " ");
                s.exec_stop = util::join(s.stop_command, " ");
                s.exec_reload = util::join(s.reload_command, " ");
            }
            s.user = (*svc)["user"].value_or("root");
            s.group = (*svc)["group"].value_or("root");
            s.working_dir = (*svc)["working_dir"].value_or("/");
            s.pid_file = (*svc)["pid_file"].value_or("");
            s.restart = (*svc)["restart"].value_or("always");
            s.type = (*svc)["type"].value_or("simple");

            if (auto* aft = svc->get_as<vendor::toml::array>("after")) {
                for (auto&& a : *aft) {
                    if (auto str = a.value<std::string_view>()) {
                        s.after.emplace_back(*str);
                    }
                }
            }
            if (auto* bef = svc->get_as<vendor::toml::array>("before")) {
                for (auto&& b : *bef) {
                    if (auto str = b.value<std::string_view>()) {
                        s.before.emplace_back(*str);
                    }
                }
            }
        } else {
            return std::unexpected("Missing [service] section in service.toml");
        }

        if (s.name.empty() || s.exec_start.empty()) {
            return std::unexpected(s.schema_version == 2
                ? "Service 'name' and non-empty 'command' are required"
                : "Service 'name' and 'exec_start' are required");
        }
        return s;
    }

    [[nodiscard]] std::string render_openrc() const {
        std::ostringstream ss;
        ss << "#!/usr/bin/openrc-run\n";
        ss << "# Generated automatically by Sage Package Manager\n";
        ss << "description=\"" << (description.empty() ? name : description) << "\"\n\n";

        ss << "depend() {\n";
        if (!after.empty()) {
            ss << "    need " << util::join(after, " ") << "\n";
        } else {
            ss << "    need net\n";
        }
        if (!before.empty()) {
            ss << "    before " << util::join(before, " ") << "\n";
        }
        ss << "}\n\n";

        ss << "command=\"" << exec_start << "\"\n";
        if (!pid_file.empty()) {
            ss << "pidfile=\"" << pid_file << "\"\n";
        }
        if (user != "root") {
            ss << "command_user=\"" << user << ":" << group << "\"\n";
        }
        if (type == "simple") {
            ss << "command_background=\"yes\"\n";
            if (pid_file.empty()) {
                ss << "pidfile=\"/run/" << name << ".pid\"\n";
            }
        }
        return ss.str();
    }

    [[nodiscard]] std::string render_systemd() const {
        std::ostringstream ss;
        ss << "# Generated automatically by Sage Package Manager\n";
        ss << "[Unit]\n";
        ss << "Description=" << (description.empty() ? name : description) << "\n";
        if (!after.empty()) {
            ss << "After=" << util::join(after, " ") << "\n";
        }
        if (!before.empty()) {
            ss << "Before=" << util::join(before, " ") << "\n";
        }
        ss << "\n[Service]\n";
        ss << "Type=" << (type.empty() ? "simple" : type) << "\n";
        ss << "ExecStart=" << exec_start << "\n";
        if (!exec_stop.empty()) ss << "ExecStop=" << exec_stop << "\n";
        if (!exec_reload.empty()) ss << "ExecReload=" << exec_reload << "\n";
        if (user != "root") ss << "User=" << user << "\n";
        if (group != "root") ss << "Group=" << group << "\n";
        if (working_dir != "/") ss << "WorkingDirectory=" << working_dir << "\n";
        if (!pid_file.empty()) ss << "PIDFile=" << pid_file << "\n";
        if (restart != "no") ss << "Restart=" << (restart == "always" ? "always" : "on-failure") << "\n";

        ss << "\n[Install]\n";
        ss << "WantedBy=multi-user.target\n";
        return ss.str();
    }

    [[nodiscard]] std::string render_runit() const {
        std::ostringstream ss;
        ss << "#!/bin/sh\n";
        ss << "# Generated automatically by Sage Package Manager\n";
        ss << "exec 2>&1\n";
        if (working_dir != "/") {
            ss << "cd " << working_dir << " || exit 1\n";
        }
        if (user != "root") {
            ss << "exec chpst -u " << user << ":" << group << " " << exec_start << "\n";
        } else {
            ss << "exec " << exec_start << "\n";
        }
        return ss.str();
    }

    [[nodiscard]] std::string render_dinit() const {
        std::ostringstream ss;
        ss << "# Generated automatically by Sage Package Manager\n";
        ss << "type = process\n";
        ss << "command = " << exec_start << "\n";
        if (working_dir != "/") ss << "working-dir = " << working_dir << "\n";
        if (!after.empty()) {
            for (const auto& a : after) {
                ss << "depends-on = " << a << "\n";
            }
        }
        if (restart == "always") {
            ss << "restart = true\n";
        }
        return ss.str();
    }

    [[nodiscard]] std::string render_s6() const {
        std::ostringstream ss;
        ss << "#!/bin/sh\n";
        ss << "# Generated automatically by Sage Package Manager\n";
        if (user != "root") {
            ss << "exec s6-setuidgid " << user << " " << exec_start << "\n";
        } else {
            ss << "exec " << exec_start << "\n";
        }
        return ss.str();
    }
};

// Where this init system's script for `name` lives under the sysroot.
// Callers use it to decide whether the path is already owned by a package
// (a shipped native unit wins over the generated form).
inline std::expected<std::filesystem::path, std::string> service_destination(
    std::string_view name,
    InitType init_type,
    const std::filesystem::path& sysroot = "/")
{
    switch (init_type) {
        case InitType::OpenRC:  return sysroot / "etc/init.d" / name;
        case InitType::Systemd: return sysroot / "usr/lib/systemd/system" / (std::string(name) + ".service");
        case InitType::Runit:   return sysroot / "etc/sv" / name / "run";
        case InitType::Dinit:   return sysroot / "etc/dinit.d" / name;
        case InitType::S6:      return sysroot / "etc/s6/services" / name / "run";
        case InitType::Loom:    return sysroot / "usr/lib/loom/services" / (std::string(name) + ".toml");
        default:                return std::unexpected("Unsupported init system");
    }
}

inline std::expected<std::string, std::string> render_service(
    const ServiceSpec& spec,
    InitType init_type)
{
    switch (init_type) {
        case InitType::OpenRC:  return spec.render_openrc();
        case InitType::Systemd: return spec.render_systemd();
        case InitType::Runit:   return spec.render_runit();
        case InitType::Dinit:   return spec.render_dinit();
        case InitType::S6:      return spec.render_s6();
        case InitType::Loom:
            return std::unexpected(
                "Loom generation requires the original Sage service document");
        default:
            return std::unexpected("Unsupported init system");
    }
}

inline bool script_is_executable(InitType init_type) noexcept {
    switch (init_type) {
        case InitType::OpenRC:
        case InitType::Runit:
        case InitType::S6:
            return true;
        default:
            return false;
    }
}

inline std::expected<std::filesystem::path, std::string> generate_service(
    const ServiceSpec& spec,
    InitType init_type,
    const std::filesystem::path& sysroot = "/")
{
    auto dest_res = service_destination(spec.name, init_type, sysroot);
    if (!dest_res) return std::unexpected(dest_res.error());
    auto content_res = render_service(spec, init_type);
    if (!content_res) return std::unexpected(content_res.error());

    const std::filesystem::path dest = std::move(*dest_res);
    if (auto parent = dest.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent);
    }

    std::ofstream out(dest);
    if (!out.is_open()) {
        return std::unexpected("Failed to write service file: " + dest.string());
    }
    out << *content_res;
    out.close();

    ::chmod(dest.c_str(), script_is_executable(init_type) ? 0755 : 0644);

    return dest;
}

inline std::expected<std::filesystem::path, std::string> generate_loom_service(
    std::string_view sage_toml,
    std::string_view name,
    const std::filesystem::path& sysroot = "/")
{
    auto destination = service_destination(name, InitType::Loom, sysroot);
    if (!destination) return std::unexpected(destination.error());
    std::filesystem::create_directories(destination->parent_path());

    static std::atomic_uint64_t next{0};
    TemporaryPath input(std::filesystem::temp_directory_path() / std::format(
        "sage-loom-{}-{}.toml", ::getpid(), next.fetch_add(1)));
    OwnedFd input_fd(::open(
        input.get().c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
    if (input_fd.get() < 0) {
        return std::unexpected(std::format(
            "Cannot create temporary Loom service input: {}", std::strerror(errno)));
    }
    size_t written = 0;
    while (written < sage_toml.size()) {
        const auto result = ::write(
            input_fd.get(), sage_toml.data() + written, sage_toml.size() - written);
        if (result < 0) {
            if (errno == EINTR) continue;
            return std::unexpected(std::format(
                "Cannot write temporary Loom service input: {}", std::strerror(errno)));
        }
        if (result == 0) {
            return std::unexpected("Short write to temporary Loom service input");
        }
        written += static_cast<size_t>(result);
    }
    if (::fsync(input_fd.get()) != 0) {
        return std::unexpected(std::format(
            "Cannot sync temporary Loom service input: {}", std::strerror(errno)));
    }

    const auto executable = std::filesystem::absolute(
        sysroot / "usr/lib/loom/loom").string();
    const auto input_path = input.get().string();
    const auto output_path = std::filesystem::absolute(*destination).string();
    const pid_t child = ::fork();
    if (child < 0) {
        return std::unexpected(std::format(
            "Cannot fork Loom service compiler: {}", std::strerror(errno)));
    }
    if (child == 0) {
        ::execl(
            executable.c_str(), executable.c_str(), "compile-service",
            "--from-sage", input_path.c_str(), "--output", output_path.c_str(),
            static_cast<char*>(nullptr));
        ::_exit(127);
    }

    int status = 0;
    pid_t waited;
    do {
        waited = ::waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0) {
        return std::unexpected(std::format(
            "Cannot wait for Loom service compiler: {}", std::strerror(errno)));
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        std::error_code error;
        std::filesystem::remove(*destination, error);
        return std::unexpected(std::format(
            "Loom service compiler failed for '{}'", name));
    }
    return *destination;
}

inline std::expected<void, std::string> validate_loom_services(
    const std::filesystem::path& sysroot = "/")
{
    const auto executable = std::filesystem::absolute(
        sysroot / "usr/lib/loom/loom").string();
    const auto root = std::filesystem::absolute(sysroot).string();
    const pid_t child = ::fork();
    if (child < 0) {
        return std::unexpected(std::format(
            "Cannot fork Loom validator: {}", std::strerror(errno)));
    }
    if (child == 0) {
        ::execl(
            executable.c_str(), executable.c_str(), "validate", "--root",
            root.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }
    int status = 0;
    pid_t waited;
    do {
        waited = ::waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0) {
        return std::unexpected(std::format(
            "Cannot wait for Loom validator: {}", std::strerror(errno)));
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return std::unexpected(std::string("Loom rejected the generated service graph"));
    }
    return {};
}

inline bool remove_service(
    std::string_view name,
    InitType init_type,
    const std::filesystem::path& sysroot = "/") 
{
    std::filesystem::path dest;
    switch (init_type) {
        case InitType::OpenRC:
            dest = sysroot / "etc/init.d" / name;
            break;
        case InitType::Systemd:
            dest = sysroot / "usr/lib/systemd/system" / (std::string(name) + ".service");
            break;
        case InitType::Runit: {
            std::filesystem::path sv_dir = sysroot / "etc/sv" / name;
            std::error_code ec;
            return std::filesystem::remove_all(sv_dir, ec) > 0;
        }
        case InitType::Dinit:
            dest = sysroot / "etc/dinit.d" / name;
            break;
        case InitType::S6: {
            std::filesystem::path s6_dir = sysroot / "etc/s6/services" / name;
            std::error_code ec;
            return std::filesystem::remove_all(s6_dir, ec) > 0;
        }
        case InitType::Loom:
            dest = sysroot / "usr/lib/loom/services" / (std::string(name) + ".toml");
            break;
        default:
            return false;
    }

    std::error_code ec;
    return std::filesystem::remove(dest, ec);
}

} // namespace sage::service
