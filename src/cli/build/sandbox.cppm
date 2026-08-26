module;
#include <cstdint>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

export module sage.cli.build:sandbox;

import std;
import sage;

namespace sage::cli {

// Keep fakeroot's two control variables, but discard the caller's shell
// environment before entering a recipe.  This prevents PATH, locale, Cargo
// config, compiler flags and proxy variables from silently changing a build.
// Sage's deterministic variables are exported by the managed plan (or by the
// legacy phase adapter) inside the clean shell.
inline std::string hermetic_shell(std::string_view script,
                           std::string_view sandbox_prefix = {}) {
    const std::string runner = sandbox_prefix.empty()
        ? "/bin/sh" : std::string(sandbox_prefix) + " -- /bin/sh";
    return "umask 022; exec env -i FAKEROOTKEY=\"$FAKEROOTKEY\" "
        "LD_PRELOAD=\"$LD_PRELOAD\" "
        "SAGE_TEST_FAKEROOT_ACTIVE=\"$SAGE_TEST_FAKEROOT_ACTIVE\" "
        + runner + " -c "
        + sage::build::shell_quote(script);
}

// fakeroot itself cannot create a user namespace when it is the outer
// process (its LD_PRELOAD state is already active).  For the strict v2 path,
// bubblewrap is therefore outermost and launches the configured fakeroot
// inside the namespace; the recipe shell remains the fakeroot child.
inline std::string sandboxed_fakeroot_shell(std::string_view fakeroot,
                                     std::string_view script,
                                     std::string_view sandbox_prefix) {
    const char* inherited_path = std::getenv("PATH");
    const std::string launcher_path = inherited_path && *inherited_path
        ? inherited_path
        : "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
    const auto inner = "exec " + sage::build::shell_quote(fakeroot)
        + " -- /bin/sh -c " + sage::build::shell_quote(script);
    return "umask 022; exec env -i PATH="
        + sage::build::shell_quote(launcher_path) + " "
        + std::string(sandbox_prefix) + " -- /bin/sh -c "
        + sage::build::shell_quote(inner);
}

// Trace the complete process tree for every managed step.  PATH wrappers are
// still useful for role attribution, but they cannot prove that a child did
// not invoke an absolute path or a helper which clears LD_PRELOAD.  The
// ptrace/seccomp pair below observes both successful exec transitions and the
// execve/execveat syscall boundary for every descendant, including processes
// created by fakeroot and bubblewrap.
struct CgroupScope {
    std::filesystem::path cgroup_path;
    bool active{false};

    CgroupScope() = default;
    CgroupScope(const CgroupScope&) = delete;
    CgroupScope& operator=(const CgroupScope&) = delete;
    CgroupScope(CgroupScope&& other) noexcept
        : cgroup_path(std::move(other.cgroup_path)),
          active(std::exchange(other.active, false)) {}
    CgroupScope& operator=(CgroupScope&& other) noexcept {
        if (this != &other) {
            cleanup();
            cgroup_path = std::move(other.cgroup_path);
            active = std::exchange(other.active, false);
        }
        return *this;
    }

    static std::expected<std::optional<CgroupScope>, std::string> create(
        std::string_view mem_limit, std::uint64_t pids_limit, pid_t pid) {
        if (mem_limit.empty() && pids_limit == 0)
            return std::optional<CgroupScope>{};

        const std::filesystem::path base_cgroup = "/sys/fs/cgroup";
        std::ifstream controllers_file(base_cgroup / "cgroup.controllers");
        if (!controllers_file) return std::unexpected(
            "cgroups v2 is unavailable: cannot read cgroup.controllers "
            "(tip: in container/unprivileged environments, ensure cgroups v2 delegation is enabled or unset memory_limit/pids_limit)");
        std::set<std::string> controllers;
        for (std::string controller; controllers_file >> controller;)
            controllers.insert(std::move(controller));
        if (!mem_limit.empty() && !controllers.contains("memory"))
            return std::unexpected(
                "configured memory_limit requires the cgroups v2 memory controller "
                "(tip: ensure 'memory' is delegated in cgroup.subtree_control or disable memory_limit)");
        if (pids_limit > 0 && !controllers.contains("pids"))
            return std::unexpected(
                "configured pids_limit requires the cgroups v2 pids controller "
                "(tip: ensure 'pids' is delegated in cgroup.subtree_control or disable pids_limit)");

        std::filesystem::path slice =
            base_cgroup / std::format("sage-build-{}", pid);
        std::error_code ec;
        std::filesystem::create_directories(slice, ec);
        if (ec) return std::unexpected(std::format(
            "cannot create build cgroup '{}': {} "
            "(tip: in unprivileged container, check cgroupfs permissions or remove memory_limit/pids_limit)",
            slice.string(), ec.message()));

        CgroupScope scope;
        scope.cgroup_path = slice;
        scope.active = true;
        const auto write_value = [&](std::string_view name,
                                     std::string_view value)
            -> std::expected<void, std::string> {
            std::ofstream file(slice / std::string(name));
            if (!file) return std::unexpected(std::format(
                "cannot open build cgroup control '{}'", (slice / std::string(name)).string()));
            file << value << '\n';
            if (!file) return std::unexpected(std::format(
                "cannot write build cgroup control '{}'", (slice / std::string(name)).string()));
            return {};
        };
        if (!mem_limit.empty()) {
            if (auto result = write_value("memory.max", mem_limit); !result)
                return std::unexpected(result.error());
        }
        if (pids_limit > 0) {
            if (auto result = write_value("pids.max", std::to_string(pids_limit));
                !result)
                return std::unexpected(result.error());
        }
        {
            std::ofstream procs_file(slice / "cgroup.procs");
            if (!procs_file) return std::unexpected(
                "cannot open build cgroup cgroup.procs");
            procs_file << pid << '\n';
            if (!procs_file) return std::unexpected(
                "cannot move build audit supervisor into its cgroup");
        }
        return std::optional<CgroupScope>(std::move(scope));
    }

    ~CgroupScope() { cleanup(); }

private:
    void cleanup() noexcept {
        if (!active || cgroup_path.empty()) return;
        std::ifstream procs(cgroup_path / "cgroup.procs");
        pid_t p;
        while (procs >> p) {
            if (p > 1) (void)::kill(p, SIGKILL);
        }
        std::error_code ec;
        std::filesystem::remove(cgroup_path, ec);
        active = false;
    }
};

struct ProcessExecAudit {
    static bool install_seccomp() noexcept {
        const sock_filter filter[] = {
            BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                     static_cast<std::uint32_t>(offsetof(struct seccomp_data, nr))),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
                     static_cast<std::uint32_t>(__NR_execve), 0, 1),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRACE),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
                     static_cast<std::uint32_t>(__NR_execveat), 0, 1),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRACE),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        };
        const sock_fprog program{
            .len = static_cast<unsigned short>(std::size(filter)),
            .filter = const_cast<sock_filter*>(filter),
        };
        return ::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0
            && ::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0;
    }

    static std::string process_cmdline(pid_t pid) {
        std::ifstream in(std::format("/proc/{}/cmdline", pid), std::ios::binary);
        std::string value((std::istreambuf_iterator<char>(in)), {});
        for (auto& c : value) if (c == '\0') c = ' ';
        while (!value.empty() && value.back() == ' ') value.pop_back();
        return value;
    }

    static std::string process_executable(pid_t pid) {
        std::error_code ec;
        auto path = std::filesystem::read_symlink(
            std::filesystem::path("/proc") / std::to_string(pid) / "exe", ec);
        return ec ? std::string{"<unavailable>"} : path.string();
    }

    static std::expected<int, std::string> run(std::string_view command,
                                                const std::filesystem::path& log,
                                                std::string_view memory_limit = {},
                                                std::uint64_t pids_limit = 0) {
        const auto command_copy = std::string(command);
        const pid_t child = ::fork();
        if (child < 0) return std::unexpected(
            std::format("cannot fork build audit supervisor: {}", std::strerror(errno)));
        if (child == 0) {
            if (pids_limit > 0) {
                if (pids_limit > static_cast<std::uint64_t>(
                        std::numeric_limits<rlim_t>::max()))
                    _exit(125);
                struct rlimit rl{static_cast<rlim_t>(pids_limit),
                                 static_cast<rlim_t>(pids_limit)};
                if (::setrlimit(RLIMIT_NPROC, &rl) != 0) _exit(125);
            }
            if (::ptrace(PTRACE_TRACEME, 0, nullptr, nullptr) != 0
                || !install_seccomp()) _exit(125);
            ::raise(SIGSTOP);
            ::execl("/bin/sh", "/bin/sh", "-c", command_copy.c_str(), nullptr);
            _exit(127);
        }
        auto cgroup_res = CgroupScope::create(memory_limit, pids_limit, child);
        if (!cgroup_res) {
            ::kill(child, SIGKILL);
            int cleanup_status = 0;
            (void)::waitpid(child, &cleanup_status, 0);
            return std::unexpected(
                "cannot apply configured build resource limits: "
                + cgroup_res.error());
        }
        auto cgroup_scope = std::move(*cgroup_res);
        std::ofstream audit_log(log, std::ios::out | std::ios::app);
        if (!audit_log) {
            ::kill(child, SIGKILL);
            return std::unexpected("cannot open process exec audit log: " + log.string());
        }
        int status = 0;
        if (::waitpid(child, &status, 0) < 0 || !WIFSTOPPED(status)) {
            ::kill(child, SIGKILL);
            if (WIFEXITED(status) && WEXITSTATUS(status) == 125) {
                return std::unexpected(
                    "build audit child failed ptrace(PTRACE_TRACEME) or seccomp initialization; "
                    "ensure environment allows ptrace (e.g. container --cap-add=SYS_PTRACE or Yama ptrace_scope)");
            }
            return std::unexpected("build audit child did not enter tracing stop");
        }
        constexpr long trace_options = PTRACE_O_TRACECLONE | PTRACE_O_TRACEFORK
            | PTRACE_O_TRACEVFORK | PTRACE_O_TRACEEXEC | PTRACE_O_TRACESECCOMP
            | PTRACE_O_EXITKILL;
        if (::ptrace(PTRACE_SETOPTIONS, child, nullptr,
                     reinterpret_cast<void*>(trace_options)) != 0) {
            ::kill(child, SIGKILL);
            return std::unexpected(std::format(
                "cannot enable process exec tracing: {}", std::strerror(errno)));
        }
        if (::ptrace(PTRACE_CONT, child, nullptr, nullptr) != 0) {
            ::kill(child, SIGKILL);
            return std::unexpected("cannot continue build audit child");
        }

        std::set<pid_t> tracees{child};
        bool root_done = false;
        int root_status = 125 << 8;
        for (;;) {
            const pid_t pid = ::waitpid(-1, &status, __WALL);
            if (pid < 0) {
                if (errno == ECHILD) break;
                if (errno == EINTR) continue;
                return std::unexpected(std::format(
                    "process exec audit wait failed: {}", std::strerror(errno)));
            }
            if (WIFEXITED(status) || WIFSIGNALED(status)) {
                tracees.erase(pid);
                if (pid == child) {
                    root_done = true;
                    root_status = status;
                    // fakeroot starts a helper daemon which can outlive the
                    // shell in a PID namespace.  It is still a traced
                    // descendant, so wait for it forever would make a
                    // successful phase appear hung.  Terminate only the
                    // descendants of this audit root, then reap them below.
                    for (const auto descendant : tracees)
                        ::kill(descendant, SIGKILL);
                }
                continue;
            }
            if (!WIFSTOPPED(status)) continue;
            const auto event = static_cast<unsigned>(status) >> 16;
            if (event == PTRACE_EVENT_SECCOMP) {
                audit_log << "execve-boundary pid=" << pid << " syscall=execve/execveat\n";
            } else if (event == PTRACE_EVENT_EXEC) {
                audit_log << "execve pid=" << pid << " path="
                          << process_executable(pid) << " argv="
                          << process_cmdline(pid) << "\n";
            }
            // A ptrace fork/clone event stops both the event parent and the
            // newly-created child.  The child is not continued implicitly;
            // leaving it stopped deadlocks fakeroot/build-system helpers and
            // makes a complete process-tree audit unusable for real builds.
            if (event == PTRACE_EVENT_CLONE || event == PTRACE_EVENT_FORK
                || event == PTRACE_EVENT_VFORK) {
                unsigned long child_word = 0;
                if (::ptrace(PTRACE_GETEVENTMSG, pid, nullptr, &child_word) == 0
                    && child_word != 0) {
                    const auto descendant = static_cast<pid_t>(child_word);
                    tracees.insert(descendant);
                    ::ptrace(PTRACE_SETOPTIONS, descendant, nullptr,
                             reinterpret_cast<void*>(trace_options));
                    ::ptrace(PTRACE_CONT, descendant, nullptr, nullptr);
                }
            }
            // Options are inherited by traced descendants on Linux, but
            // setting them again is harmless and covers kernels that only
            // copy the tracing relationship at clone time.
            ::ptrace(PTRACE_SETOPTIONS, pid, nullptr,
                     reinterpret_cast<void*>(trace_options));
            ::ptrace(PTRACE_CONT, pid, nullptr, nullptr);
        }
        if (!root_done) return std::unexpected("build audit supervisor lost its root child");
        if (WIFEXITED(root_status)) return WEXITSTATUS(root_status);
        return 128 + WTERMSIG(root_status);
    }
};

} // namespace sage::cli
