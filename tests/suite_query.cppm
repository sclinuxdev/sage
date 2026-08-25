module;
#include <sys/stat.h>
#include <unistd.h>

export module sage.tests.query;

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

namespace query {
export int run_local_repo_index_tests(const std::filesystem::path& temp_dir,
    const std::filesystem::path& pkg_path, const sage::package::PackageManifest& manifest,
    std::filesystem::path& local_repo) {
    // 8. Local Repository Indexing & Zero-Copy file:// Protocol Test
    const std::string escaped_channel_name = "core \"quoted\" \\ channel";
    local_repo = temp_dir / "local-repo";
    std::filesystem::create_directories(local_repo);
    std::error_code repo_copy_ec;
    std::filesystem::copy_file(
        pkg_path, local_repo / pkg_path.filename(),
        std::filesystem::copy_options::overwrite_existing, repo_copy_ec);
    auto idx_res = repo_copy_ec
        ? std::expected<void, std::string>(std::unexpected(repo_copy_ec.message()))
        : sage::archive::generate_repo_index(local_repo, escaped_channel_name);
    if (!idx_res || !std::filesystem::exists(local_repo / "index.toml")) {
        sage::util::log_error("Local repository index generation failed");
        return 1;
    }

    auto fetch_res = sage::vendor::curl::fetch_string(
        "file://" + (local_repo / "index.toml").string());
    if (!fetch_res) {
        sage::util::log_error("Local file:// protocol fetch failed");
        return 1;
    }
    auto parsed_index = sage::channel::ChannelIndex::parse_toml(*fetch_res);
    if (fetch_res->find("schema_version = 1") == std::string::npos
        || !parsed_index
        || parsed_index->channel_name != escaped_channel_name
        || parsed_index->available_packages.empty()
        || parsed_index->available_packages.front().description != manifest.description) {
        sage::util::log_error("Local file:// protocol fetch failed");
        return 1;
    }
    sage::util::log_success("8. Local Repository (file:// & /path) Indexer & Zero-Copy Fetch OK");

    return 0;
}

} // namespace query
} // namespace sage::tests
