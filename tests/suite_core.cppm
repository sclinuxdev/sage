module;
#include <sys/stat.h>
#include <unistd.h>

export module sage.tests.core;

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

namespace core {
export int run_versioning_tests() {
    // 1. Semantic Versioning Test
    auto v1 = sage::package::Version::parse("1.2.3-1");
    auto v2 = sage::package::Version::parse("1.2.4-1");
    if (!(v1 < v2)) {
        sage::util::log_error("Version comparator test failed");
        return 1;
    }
    auto legacy_config = sage::config::SystemConfig::parse_system_toml(
        "[providers]\ninit = \"openrc\"\n");
    auto shared_override = sage::config::SystemConfig::parse_system_toml(
        "[providers]\ninit = \"openrc\"\n\n[capabilities]\ninit = \"shared\"\n");
    auto aarch64_config = sage::config::SystemConfig::parse_system_toml(
        "[system]\narchitecture = \"aarch64\"\n");
    auto invalid_architecture_config = sage::config::SystemConfig::parse_system_toml(
        "[system]\narchitecture = \"mips\"\n");
    if (!legacy_config
        || !legacy_config->is_exclusive_capability("virtual/init")
        || !legacy_config->is_exclusive_capability("virtual/udev")
        || !legacy_config->is_exclusive_capability("virtual/libc")
        || !shared_override
        || shared_override->is_exclusive_capability("virtual/init")
        || !aarch64_config
        || aarch64_config->architecture != "aarch64"
        || invalid_architecture_config) {
        sage::util::log_error("Legacy capability defaults or explicit override failed");
        return 1;
    }

    const auto builtin_triggers = sage::triggers::TriggerEngine::builtin_triggers();
    auto ldconfig_trigger = std::ranges::find_if(
        builtin_triggers, [](const auto& trigger) { return trigger.name == "ldconfig"; });
    auto certificates_trigger = std::ranges::find_if(
        builtin_triggers, [](const auto& trigger) { return trigger.name == "ca-certificates"; });
    auto mime_trigger = std::ranges::find_if(
        builtin_triggers, [](const auto& trigger) { return trigger.name == "mime-database"; });
    if (ldconfig_trigger == builtin_triggers.end()
        || ldconfig_trigger->exec != "/usr/bin/ldconfig"
        || !ldconfig_trigger->required
        || certificates_trigger == builtin_triggers.end()
        || certificates_trigger->exec != "/usr/bin/update-ca-certificates"
        || certificates_trigger->required
        || mime_trigger == builtin_triggers.end()
        || mime_trigger->exec != "/usr/bin/update-mime-database"
        || mime_trigger->required) {
        sage::util::log_error(
            "Built-in triggers do not use canonical usr-merge paths or the required/optional policy");
        return 1;
    }

    sage::package::PackageManifest failing_trigger_package;
    failing_trigger_package.name = "failing-trigger";
    failing_trigger_package.version = sage::package::Version::parse("1.0.0-1");
    failing_trigger_package.triggers.push_back(sage::package::Trigger{
        .name = "must-fail",
        .on_paths = {"usr/share/must-fail/"},
        .on_capability = {},
        .exec = "/bin/false",
        .args = {},
        .run_capability = {},
    });
    sage::package::FileEntry failing_trigger_file;
    failing_trigger_file.path = "usr/share/must-fail/input";
    sage::triggers::TriggerContext failing_trigger_context;
    failing_trigger_context.touched_files = {failing_trigger_file};
    failing_trigger_context.installed_packages = {failing_trigger_package};
    auto failing_trigger_result =
        sage::triggers::TriggerEngine::run(failing_trigger_context);
    if (failing_trigger_result
        || failing_trigger_result.error().find("must-fail") == std::string::npos) {
        sage::util::log_error("Trigger execution failure was not propagated");
        return 1;
    }
    failing_trigger_package.triggers.front().name = "missing-trigger";
    failing_trigger_package.triggers.front().required = true;
    failing_trigger_package.triggers.front().exec = "/usr/bin/sage-missing-trigger";
    failing_trigger_context.installed_packages = {failing_trigger_package};
    auto missing_trigger_result = sage::triggers::TriggerEngine::run(failing_trigger_context);
    if (missing_trigger_result
        || missing_trigger_result.error().find("Required executable") == std::string::npos) {
        sage::util::log_error("Missing required trigger executable was treated as success");
        return 1;
    }
    // An optional trigger whose exec is absent only warns -- the transaction
    // must still succeed (issue #18: every package committed, yet exit 1).
    failing_trigger_package.triggers.front().name = "optional-missing-trigger";
    failing_trigger_package.triggers.front().required = false;
    failing_trigger_context.installed_packages = {failing_trigger_package};
    if (auto optional_missing_result =
            sage::triggers::TriggerEngine::run(failing_trigger_context);
        !optional_missing_result) {
        sage::util::log_error(
            "Optional trigger with a missing executable failed the transaction: {}",
            optional_missing_result.error());
        return 1;
    }

    auto trigger_sysroot = std::filesystem::temp_directory_path() / "sage_trigger_symlink_test";
    std::filesystem::remove_all(trigger_sysroot);
    std::filesystem::create_directories(trigger_sysroot / "usr/bin");
    std::filesystem::create_directories(trigger_sysroot / "usr/libexec");
    std::ofstream(trigger_sysroot / "usr/libexec/real-trigger") << "fixture\n";
    std::filesystem::create_symlink(
        "/usr/libexec/real-trigger", trigger_sysroot / "usr/bin/absolute-trigger");
    std::filesystem::create_symlink(
        "../libexec/real-trigger", trigger_sysroot / "usr/bin/relative-trigger");
    std::filesystem::create_symlink(
        "../../../usr/libexec/real-trigger", trigger_sysroot / "usr/bin/clamped-trigger");
    std::filesystem::create_symlink(
        "/usr/libexec/missing-trigger", trigger_sysroot / "usr/bin/dangling-trigger");
    failing_trigger_context.sysroot = trigger_sysroot;
    failing_trigger_context.dry_run = true;
    for (std::string_view executable : {
             "/usr/bin/absolute-trigger", "/usr/bin/relative-trigger",
             "/usr/bin/clamped-trigger"}) {
        failing_trigger_package.triggers.front().name = "sysroot-symlink-trigger";
        failing_trigger_package.triggers.front().exec = executable;
        failing_trigger_context.installed_packages = {failing_trigger_package};
        if (auto result = sage::triggers::TriggerEngine::run(failing_trigger_context); !result) {
            sage::util::log_error(
                "Target-root trigger symlink '{}' was not resolved: {}", executable, result.error());
            return 1;
        }
    }
    failing_trigger_package.triggers.front().name = "dangling-sysroot-trigger";
    failing_trigger_package.triggers.front().required = true;
    failing_trigger_package.triggers.front().exec = "/usr/bin/dangling-trigger";
    failing_trigger_context.installed_packages = {failing_trigger_package};
    auto dangling_trigger_result = sage::triggers::TriggerEngine::run(failing_trigger_context);
    if (dangling_trigger_result
        || dangling_trigger_result.error().find("Required executable") == std::string::npos) {
        sage::util::log_error("Dangling target-root trigger symlink was treated as executable");
        return 1;
    }
    std::filesystem::remove_all(trigger_sysroot);
    sage::util::log_success("1. Semantic & Alphanum Version Comparator OK");

    return 0;
}

} // namespace core
} // namespace sage::tests
