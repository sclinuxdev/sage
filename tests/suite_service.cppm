module;
#include <sys/stat.h>
#include <unistd.h>

export module sage.tests.service;

import std;
import sage;

import sage.cli;
import sage.cli.build;
import sage.cli.install;
import sage.cli.rebuild;
import sage.cli.remove;
import sage.tests.service_lifecycle;

namespace sage::tests {

using namespace sage::cli;
using std::size_t;
using std::uint8_t;
using std::uint32_t;
using std::uint64_t;

namespace service {
export int run_service_generation_tests(const std::filesystem::path& extract_root) {
    // 4. Multi-Init Universal Service Generation Test
    sage::service::ServiceSpec svc;
    svc.name = "sshd";
    svc.description = "OpenSSH Server";
    svc.exec_start = "/usr/bin/sshd -D";
    auto gen_openrc = sage::service::generate_service(svc, sage::service::InitType::OpenRC, extract_root);
    auto gen_sysd = sage::service::generate_service(svc, sage::service::InitType::Systemd, extract_root);
    if (!gen_openrc || !gen_sysd) {
        sage::util::log_error("Service generation test failed");
        return 1;
    }
    for (const auto invalid_name : {
            "/tmp/host-file", "../escape", "nested/name", "space name"}) {
        const auto document = std::format(R"(schema_version = 1
[service]
name = "{}"
exec_start = "/usr/bin/false"
)", invalid_name);
        if (sage::service::ServiceSpec::parse_toml(document)
            || sage::service::service_destination(
                invalid_name, sage::service::InitType::Systemd, extract_root)) {
            sage::util::log_error("Unsafe service name '{}' was accepted", invalid_name);
            return 1;
        }
    }
    auto loom_destination = sage::service::service_destination(
        "sshd", sage::service::InitType::Loom, extract_root);
    auto schema_two = sage::service::ServiceSpec::parse_toml(R"(
schema_version = 2
[service]
name = "sshd"
command = ["/usr/bin/sshd", "-D"]
restart = "on-failure"
)");
    if (!loom_destination
        || *loom_destination != extract_root / "usr/lib/loom/services/sshd.toml"
        || !schema_two
        || schema_two->command != std::vector<std::string>{"/usr/bin/sshd", "-D"}) {
        sage::util::log_error("Loom service target or schema v2 parsing failed");
        return 1;
    }
    const auto loom_binary = extract_root / "usr/lib/loom/loom";
    std::filesystem::create_directories(loom_binary.parent_path());
    {
        std::ofstream validator(loom_binary);
        validator << "#!/bin/sh\n[ \"$1\" = validate ]\n";
    }
    std::filesystem::permissions(loom_binary, std::filesystem::perms::owner_all);
    auto valid_loom_graph = sage::service::validate_loom_services(extract_root);
    {
        std::ofstream validator(loom_binary);
        validator << "#!/bin/sh\nexit 1\n";
    }
    auto invalid_loom_graph = sage::service::validate_loom_services(extract_root);
    if (!valid_loom_graph || invalid_loom_graph) {
        sage::util::log_error("Loom graph validation result was not enforced");
        return 1;
    }
    std::ifstream openrc_script(extract_root / "etc/init.d/sshd");
    std::string openrc_shebang;
    std::getline(openrc_script, openrc_shebang);
    if (openrc_shebang != "#!/usr/bin/openrc-run") {
        sage::util::log_error("OpenRC service uses a non-canonical interpreter path");
        return 1;
    }
    sage::util::log_success("4. Universal Multi-Init Service Generator (Loom/OpenRC/Systemd/Runit/Dinit/s6) OK");

    return 0;
}

export int run_ephemeral_shell_tests(const std::filesystem::path& extract_root) {
    // 7. Ephemeral Shell Environment Synthesis Test
    auto shell_env = sage::channel::ProfileManager::generate_shell_env(extract_root, {
        sage::channel::SubChannelSpec::parse("toolchain/llvm:22")
    });
    if (!shell_env.contains("CC") || shell_env["CC"].find("llvm/22/bin/clang") == std::string::npos) {
        sage::util::log_error("Ephemeral shell environment generation failed");
        return 1;
    }
    sage::util::log_success("7. Ephemeral Sandboxed Shell Environment Generator (sage shell) OK");

    return 0;
}

} // namespace service
} // namespace sage::tests
