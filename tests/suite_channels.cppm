module;
#include <sys/stat.h>
#include <unistd.h>

export module sage.tests.channels;

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

namespace channels {
export int run_channel_swap_tests(const std::filesystem::path& extract_root) {
    // 6. Sub-Channel Toolchain & Profile Swapper Test
    auto tc_dir = extract_root / "opt/channels/llvm/22/bin";
    std::filesystem::create_directories(tc_dir);
    std::ofstream clang_bin(tc_dir / "clang");
    clang_bin << "#!/bin/sh\necho 'clang version 22.1.0'\n";
    clang_bin.close();
    std::filesystem::permissions(tc_dir / "clang", std::filesystem::perms::owner_all | std::filesystem::perms::group_read | std::filesystem::perms::group_exec);

    auto sw_res = sage::channel::ProfileManager::switch_active_toolchain(extract_root, "llvm", "22");
    // Profile links are deliberately chroot-relative: they point at
    // /opt/channels/..., which is where the toolchain lives once the sysroot
    // becomes the root. On the build host that target does not exist, so
    // std::filesystem::exists() -- which follows the link -- is the wrong
    // probe. Check the link itself, and that it points where it should.
    auto cc_link = extract_root / "etc/sage/profiles/default/bin/cc";
    std::error_code cc_ec;
    if (!sw_res
        || !std::filesystem::is_symlink(cc_link, cc_ec)
        || std::filesystem::read_symlink(cc_link, cc_ec) != "/opt/channels/llvm/22/bin/clang") {
        sage::util::log_error("Toolchain profile switch verification failed");
        return 1;
    }
    sage::util::log_success("6. Sub-Channel Toolchain Slot Swapper & Profile Aggregator OK");

    // Normal install/rebuild resolution must be a local-cache operation once
    // a remote channel index has been synchronized. The deliberately invalid
    // URL proves IndexRefresh::IfMissing does not touch the network.
    const auto cache_root = extract_root / "var/cache/sage/channel-test";
    const auto cached_index = cache_root / "channels/cached.toml";
    std::filesystem::create_directories(cached_index.parent_path());
    {
        std::ofstream output(cached_index);
        output << R"(schema_version = 1
[channel]
name = "cached"

[[packages]]
name = "cache-proof"
version = "1.0"
release = "1"
arch = "any"
)";
    }
    sage::channel::Channel cached_channel;
    cached_channel.name = "cached";
    cached_channel.url = "https://invalid.example/sage";
    auto cached = sage::channel::ProfileManager::sync_channel(
        cached_channel, cache_root, sage::channel::IndexRefresh::IfMissing);
    if (!cached || cached->available_packages.size() != 1
        || cached->available_packages.front().name != "cache-proof") {
        sage::util::log_error("Cached remote channel index was not reused");
        return 1;
    }

    return 0;
}

} // namespace channels
} // namespace sage::tests
