module;
#include <sys/stat.h>
#include <unistd.h>

export module sage.tests.provenance;

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


// Local minimal tar fixture toolkit: lets the suite mutate raw archive
// members to craft hostile fixtures without depending on engine internals.
struct TarFixture {
    static constexpr size_t kBlockSize = 512;

    struct TarHeader {
        char name[100];
        char mode[8];
        char uid[8];
        char gid[8];
        char size[12];
        char mtime[12];
        char chksum[8];
        char typeflag;
        char linkname[100];
        char magic[6];
        char version[2];
        char uname[32];
        char gname[32];
        char devmajor[8];
        char devminor[8];
        char prefix[155];
        char padding[12];
    };
    static_assert(sizeof(TarHeader) == kBlockSize);

    static uint64_t parse_octal(const char* str, size_t len) {
        uint64_t value = 0;
        size_t i = 0;
        while (i < len && (str[i] == ' ' || str[i] == '\0')) ++i;
        while (i < len && str[i] >= '0' && str[i] <= '7') {
            value = (value << 3) | static_cast<uint64_t>(str[i] - '0');
            ++i;
        }
        return value;
    }

    static uint32_t compute_tar_checksum(const TarHeader& header) {
        const auto* bytes = reinterpret_cast<const uint8_t*>(&header);
        uint32_t sum = 0;
        for (size_t i = 0; i < sizeof(header); ++i) {
            sum += (i >= 148 && i < 156) ? uint32_t(' ') : uint32_t(bytes[i]);
        }
        return sum;
    }

    static void write_octal(char* dest, size_t len, uint64_t value) {
        if (len == 0) return;
        dest[len - 1] = '\0';
        size_t i = len - 1;
        while (i > 0 && value > 0) {
            dest[--i] = static_cast<char>('0' + (value & 7));
            value >>= 3;
        }
        while (i > 0) dest[--i] = '0';
    }

    static void write_tar_checksum(char* dest, uint32_t checksum) {
        for (size_t i = 6; i > 0; --i) {
            dest[i - 1] = static_cast<char>('0' + (checksum & 7));
            checksum >>= 3;
        }
        dest[6] = '\0';
        dest[7] = ' ';
    }
};

namespace provenance {
export int run_archive_integrity_tests(std::filesystem::path& temp_dir,
    std::filesystem::path& pkg_path, sage::package::PackageManifest& manifest,
    std::filesystem::path& extract_root) {
// 2. Tar+Zstd Archive Packaging & Streaming Extractor Test
    temp_dir = std::filesystem::temp_directory_path() / "sage_archive_test";
    std::filesystem::remove_all(temp_dir);
    auto unpublished_build_root = temp_dir / "unpublished-build-root";
    std::filesystem::create_directories(unpublished_build_root / "etc/sage");
    std::ofstream(unpublished_build_root / "etc/sage/system.toml") << "schema_version = 1\n";
    const auto long_rel = std::filesystem::path(std::string(110, 'a')) / std::string(80, 'b');
    const auto boundary_rel = std::filesystem::path(std::string(100, 'e')) / std::string(49, 'f') / std::string(100, 'g');
    const auto boundary_empty_dir = std::filesystem::path(std::string(100, 'i'));
    const std::string boundary_link_target(100, 'h');
    auto populate_payload = [&](const std::filesystem::path& root, bool long_path_first) {
        auto write_long_path = [&] {
            std::filesystem::create_directories((root / long_rel).parent_path());
            std::ofstream(root / long_rel) << "long path payload\n";
            std::filesystem::create_directories((root / boundary_rel).parent_path());
            std::ofstream(root / boundary_rel) << "USTAR boundary payload\n";
        };
        auto write_dummy = [&] {
            std::filesystem::create_directories(root / "usr/bin");
            std::ofstream(root / "usr/bin/dummy") << "#!/bin/sh\necho 'hello sage'\n";
            std::filesystem::permissions(root / "usr/bin/dummy", std::filesystem::perms::owner_all | std::filesystem::perms::group_read | std::filesystem::perms::group_exec);
            std::filesystem::create_symlink(boundary_link_target, root / "usr/bin/link");
            std::filesystem::create_directories(root / boundary_empty_dir);
        };

        if (long_path_first) {
            write_long_path();
            write_dummy();
        } else {
            write_dummy();
            write_long_path();
        }
    };
    populate_payload(temp_dir / "data", true);
    populate_payload(temp_dir / "data_reordered", false);

    manifest = sage::package::PackageManifest{};
    manifest.name = "dummy-tool";
    manifest.version = sage::package::Version::parse("1.0.0-1");
    manifest.description = "Test \"mock\" tool with \\ path";
    manifest.license = "BSD-2-Clause";
    manifest.channel = "system";

    pkg_path = temp_dir / "dummy-tool-1.0.0-1-x86_64.pkg.tar.zst";
    auto pack_res = sage::archive::create_package(manifest, temp_dir / "data", pkg_path);
    if (!pack_res) {
        sage::util::log_error("Archive pack failed: {}", pack_res.error());
        return 1;
    }

    auto reordered_pkg_path = temp_dir / "dummy-tool-reordered.pkg.tar.zst";
    auto reordered_pack_res = sage::archive::create_package(manifest, temp_dir / "data_reordered", reordered_pkg_path);
    auto pkg_hash = sage::util::compute_file_sha256(pkg_path);
    auto reordered_pkg_hash = sage::util::compute_file_sha256(reordered_pkg_path);
    if (!reordered_pack_res || !pkg_hash || !reordered_pkg_hash || *pkg_hash != *reordered_pkg_hash) {
        sage::util::log_error("Archive reproducibility verification failed");
        return 1;
    }

    // The ownership preflight and extraction must refer to the same payload,
    // even when a replacement archive keeps the same package identity.
    auto preflight_package = sage::archive::inspect_package(pkg_path);
    auto replaced_payload = temp_dir / "replaced-payload";
    std::filesystem::create_directories(replaced_payload / "usr/bin");
    std::ofstream(replaced_payload / "usr/bin/injected") << "must not extract\n";
    auto replacement_archive = temp_dir / "replacement-same-identity.pkg.tar.zst";
    auto replacement_pack = sage::archive::create_package(
        manifest, replaced_payload, replacement_archive);
    auto binding_root = temp_dir / "binding-root";
    auto binding_result = preflight_package && replacement_pack
        ? sage::archive::extract_package(
            replacement_archive, binding_root, &manifest, &*preflight_package)
        : std::expected<sage::archive::ExtractedPackage, std::string>(
            std::unexpected("fixture failed"));
    if (binding_result || std::filesystem::exists(binding_root / "usr/bin/injected")) {
        sage::util::log_error("Archive replacement bypassed ownership preflight");
        return 1;
    }

    // Cleanup follows the same anchored traversal as extraction. An
    // intermediate symlink must never redirect unlink outside the sysroot.
    auto anchored_remove_root = temp_dir / "anchored-remove-root";
    auto anchored_remove_outside = temp_dir / "anchored-remove-outside";
    std::filesystem::create_directories(anchored_remove_root / "opt");
    std::filesystem::create_directories(anchored_remove_outside);
    std::ofstream(anchored_remove_outside / "keep") << "must survive\n";
    std::filesystem::create_directory_symlink(
        anchored_remove_outside, anchored_remove_root / "opt/app");
    auto anchored_remove = sage::archive::remove_path_anchored(
        anchored_remove_root, "opt/app/keep");
    if (anchored_remove || !std::filesystem::exists(anchored_remove_outside / "keep")) {
        sage::util::log_error("Anchored cleanup followed an intermediate symlink");
        return 1;
    }

    auto raw_tar_path = temp_dir / "dummy-tool.tar";
    auto decompress_cmd = std::format("zstd -dc \"{}\" > \"{}\"", pkg_path.string(), raw_tar_path.string());
    if (std::system(decompress_cmd.c_str()) != 0) {
        sage::util::log_error("Failed to decompress generated USTAR archive for inspection");
        return 1;
    }

    auto malformed_archive_dir = temp_dir / "malformed-archives";
    std::filesystem::create_directories(malformed_archive_dir);
    // libarchive's pax-restricted format represents long names/targets via
    // extended headers, so both former USTAR-limit rejections are now valid
    // packages: require successful packing AND a faithful extraction.
    auto long_name_root = temp_dir / "long-name-roundtrip";
    std::filesystem::create_directories(long_name_root);
    const auto very_long_rel = std::filesystem::path(std::string(101, 'c'));
    std::ofstream(long_name_root / very_long_rel) << "long name payload\n";
    std::error_code link_ec;
    std::filesystem::create_symlink(std::string(101, 'd'), long_name_root / "link", link_ec);
    if (link_ec) {
        sage::util::log_error("Failed to create long-link fixture: {}", link_ec.message());
        return 1;
    }
    auto long_name_pkg = malformed_archive_dir / "long-name-roundtrip.pkg.tar.zst";
    auto long_name_pack = sage::archive::create_package(manifest, long_name_root, long_name_pkg);
    auto long_name_extract_root = temp_dir / "long-name-extract-root";
    auto long_name_extract = long_name_pack
        ? sage::archive::extract_package(long_name_pkg, long_name_extract_root)
        : std::expected<sage::archive::ExtractedPackage, std::string>(
            std::unexpected(long_name_pack.error()));
    if (!long_name_extract
        || !std::filesystem::exists(long_name_extract_root / very_long_rel)
        || !std::filesystem::is_symlink(long_name_extract_root / "link")
        || std::filesystem::read_symlink(long_name_extract_root / "link").generic_string() != std::string(101, 'd')) {
        sage::util::log_error("Long names or link targets did not survive a pax round-trip");
        return 1;
    }

    extract_root = temp_dir / "sysroot";
    auto ext_res = sage::archive::extract_package(pkg_path, extract_root);
    const bool canonical_archive_paths = ext_res
        && std::ranges::none_of(ext_res->manifest.files, [](const auto& file) {
            return file.path.ends_with('/');
        });
    bool normalized_mtime = false;
    {
        struct ::stat st {};
        if (::stat((extract_root / "usr/bin/dummy").c_str(), &st) == 0) {
            normalized_mtime = static_cast<uint64_t>(st.st_mtim.tv_sec) == 1700000000;
        }
    }
    if (!ext_res || !std::filesystem::exists(extract_root / "usr/bin/dummy") ||
        !std::filesystem::exists(extract_root / long_rel) || !std::filesystem::exists(extract_root / boundary_rel) ||
        !std::filesystem::is_directory(extract_root / boundary_empty_dir) ||
        !std::filesystem::is_symlink(extract_root / "usr/bin/link") ||
        std::filesystem::read_symlink(extract_root / "usr/bin/link").generic_string() != boundary_link_target ||
        !canonical_archive_paths || !normalized_mtime) {
        sage::util::log_error("Archive extraction verification failed");
        return 1;
    }

    auto conflict_root = temp_dir / "conflict-sysroot";
    std::filesystem::create_directories(conflict_root / "usr/bin/link");
    std::ofstream(conflict_root / "usr/bin/link/keep") << "must survive\n";
    auto conflict_res = sage::archive::extract_package(pkg_path, conflict_root);
    // Streaming extraction aborts at the first conflicting member; members
    // written before it remain on disk (pacman behaves the same way). What
    // matters is the hard failure plus the untouched foreign payload.
    if (conflict_res || !std::filesystem::exists(conflict_root / "usr/bin/link/keep")) {
        sage::util::log_error("Archive path-type conflict was not reported safely");
        return 1;
    }

    std::ifstream raw_tar_bytes_in(raw_tar_path, std::ios::binary);
    std::vector<std::uint8_t> raw_tar_bytes(
        std::istreambuf_iterator<char>(raw_tar_bytes_in), {});
    auto find_tar_header = [&](const std::vector<std::uint8_t>& bytes, std::string_view wanted)
        -> std::optional<std::size_t> {
        std::size_t offset = 0;
        while (offset + sizeof(TarFixture::TarHeader) <= bytes.size()) {
            TarFixture::TarHeader header{};
            std::memcpy(&header, bytes.data() + offset, sizeof(header));
            bool all_zero = std::ranges::all_of(
                std::span(bytes.data() + offset, sizeof(header)),
                [](std::uint8_t byte) { return byte == 0; });
            if (all_zero) break;

            const auto bounded_string = [](const char* data, std::size_t size) {
                return std::string(data, std::find(data, data + size, '\0'));
            };
            std::string name;
            if (header.prefix[0] != '\0') {
                name = bounded_string(header.prefix, sizeof(header.prefix)) + "/";
            }
            name += bounded_string(header.name, sizeof(header.name));
            if (name == wanted) return offset;

            auto size = TarFixture::parse_octal(header.size, sizeof(header.size));
            offset += sizeof(header) + static_cast<std::size_t>(((size + 511) / 512) * 512);
        }
        return std::nullopt;
    };
    auto write_mutated_package = [&](const std::vector<std::uint8_t>& bytes, std::string_view stem)
        -> std::optional<std::filesystem::path> {
        auto tar_path = malformed_archive_dir / (std::string(stem) + ".tar");
        auto archive_path = malformed_archive_dir / (std::string(stem) + ".pkg.tar.zst");
        std::ofstream tar_out(tar_path, std::ios::binary);
        tar_out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        tar_out.close();
        auto command = std::format("zstd -q -f \"{}\" -o \"{}\"", tar_path.string(), archive_path.string());
        if (std::system(command.c_str()) != 0) return std::nullopt;
        return archive_path;
    };
    auto rename_tar_entry = [&](std::vector<std::uint8_t>& bytes,
                                std::string_view old_name,
                                std::string_view new_name) -> bool {
        auto offset = find_tar_header(bytes, old_name);
        if (!offset || new_name.size() > 100) return false;
        TarFixture::TarHeader header{};
        std::memcpy(&header, bytes.data() + *offset, sizeof(header));
        std::memset(header.name, 0, sizeof(header.name));
        std::memset(header.prefix, 0, sizeof(header.prefix));
        std::memcpy(header.name, new_name.data(), new_name.size());
        TarFixture::write_tar_checksum(
            header.chksum, TarFixture::compute_tar_checksum(header));
        std::memcpy(bytes.data() + *offset, &header, sizeof(header));
        return true;
    };

    auto invalid_manifest_bytes = raw_tar_bytes;
    auto manifest_header = find_tar_header(invalid_manifest_bytes, ".METADATA/manifest.toml");
    if (!manifest_header) {
        sage::util::log_error("Failed to locate manifest test fixture");
        return 1;
    }
    invalid_manifest_bytes[*manifest_header + sizeof(TarFixture::TarHeader)] = '@';
    auto invalid_manifest_pkg = write_mutated_package(invalid_manifest_bytes, "invalid-manifest");
    auto invalid_manifest_root = temp_dir / "invalid-manifest-root";
    auto invalid_manifest_result = invalid_manifest_pkg
        ? sage::archive::extract_package(*invalid_manifest_pkg, invalid_manifest_root)
        : std::expected<sage::archive::ExtractedPackage, std::string>(std::unexpected("fixture failed"));
    if (invalid_manifest_result
        || std::filesystem::exists(invalid_manifest_root / "usr/bin/dummy")
        || std::filesystem::exists(invalid_manifest_root / long_rel)) {
        sage::util::log_error("Invalid manifest was rejected only after writing package files");
        return 1;
    }

    auto traversal_bytes = raw_tar_bytes;
    if (!rename_tar_entry(
            traversal_bytes, "data/usr/bin/dummy", "data/../../escaped-by-package")) {
        sage::util::log_error("Failed to create path traversal test fixture");
        return 1;
    }
    auto traversal_pkg = write_mutated_package(traversal_bytes, "path-traversal");
    auto traversal_root = temp_dir / "path-traversal-root/inner";
    auto escaped_path = temp_dir / "escaped-by-package";
    auto traversal_result = traversal_pkg
        ? sage::archive::extract_package(*traversal_pkg, traversal_root)
        : std::expected<sage::archive::ExtractedPackage, std::string>(std::unexpected("fixture failed"));
    if (traversal_result || std::filesystem::exists(escaped_path)) {
        sage::util::log_error("Package data path escaped the target root");
        return 1;
    }

    auto symlink_pivot_bytes = raw_tar_bytes;
    if (!rename_tar_entry(
            symlink_pivot_bytes, "data/usr/bin/dummy", "data/usr/bin/link/escape")) {
        sage::util::log_error("Failed to create archive symlink traversal fixture");
        return 1;
    }
    auto symlink_pivot_pkg = write_mutated_package(symlink_pivot_bytes, "symlink-pivot");
    auto symlink_pivot_root = temp_dir / "symlink-pivot-root";
    auto symlink_pivot_result = symlink_pivot_pkg
        ? sage::archive::extract_package(*symlink_pivot_pkg, symlink_pivot_root)
        : std::expected<sage::archive::ExtractedPackage, std::string>(std::unexpected("fixture failed"));
    if (symlink_pivot_result) {
        sage::util::log_error("Archive symlink parent traversal was not rejected");
        return 1;
    }

    auto existing_symlink_root = temp_dir / "existing-symlink-root";
    auto outside_symlink_target = temp_dir / "outside-symlink-target";
    std::filesystem::create_directories(existing_symlink_root);
    std::filesystem::create_directories(outside_symlink_target);
    std::filesystem::create_directory_symlink(outside_symlink_target, existing_symlink_root / "usr");
    auto outside_dummy = outside_symlink_target / "bin/dummy";
    auto existing_symlink_result = sage::archive::extract_package(pkg_path, existing_symlink_root);
    if (existing_symlink_result || std::filesystem::exists(outside_dummy)) {
        sage::util::log_error("Existing target symlink escaped the target root");
        return 1;
    }

    auto read_archive_test_file = [](const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), {});
    };

    // A pre-created legacy fixed-name temporary symlink must be ignored: the
    // extractor uses its own unique O_EXCL/O_NOFOLLOW temporary leaf.
    auto temp_symlink_root = temp_dir / "temp-symlink-root";
    auto temp_symlink_outside = temp_dir / "temp-symlink-outside";
    std::filesystem::create_directories(temp_symlink_root / "usr/bin");
    std::ofstream(temp_symlink_outside) << "must survive\n";
    std::filesystem::create_symlink(
        temp_symlink_outside, temp_symlink_root / "usr/bin/dummy.sage_tmp");
    auto temp_symlink_result = sage::archive::extract_package(pkg_path, temp_symlink_root);
    if (!temp_symlink_result
        || read_archive_test_file(temp_symlink_outside) != "must survive\n"
        || read_archive_test_file(temp_symlink_root / "usr/bin/dummy")
            != "#!/bin/sh\necho 'hello sage'\n"
        || !std::filesystem::is_symlink(temp_symlink_root / "usr/bin/dummy.sage_tmp")) {
        sage::util::log_error("Archive extraction followed a fixed temporary-file symlink");
        return 1;
    }

    auto reserved_temp_bytes = raw_tar_bytes;
    if (!rename_tar_entry(
            reserved_temp_bytes,
            "data/usr/bin/link",
            "data/usr/bin/.sage-tmp-controlled")) {
        sage::util::log_error("Failed to create reserved temporary-name fixture");
        return 1;
    }
    auto reserved_temp_pkg = write_mutated_package(
        reserved_temp_bytes, "reserved-temp-name");
    auto reserved_temp_root = temp_dir / "reserved-temp-root";
    auto reserved_temp_result = reserved_temp_pkg
        ? sage::archive::extract_package(*reserved_temp_pkg, reserved_temp_root)
        : std::expected<sage::archive::ExtractedPackage, std::string>(
            std::unexpected("fixture failed"));
    // Streaming rejection happens at the offending member; earlier members
    // are already on disk (pacman behaves the same way).
    if (reserved_temp_result
        || reserved_temp_result.error().find("reserved temporary-file namespace")
            == std::string::npos) {
        sage::util::log_error("Archive accepted the internal temporary-file namespace");
        return 1;
    }

    // Replacing a verified parent directory with an external symlink while the
    // second archive pass is running must fail closed at the fd-relative sink.
    auto parent_race_data = temp_dir / "parent-race-data";
    auto parent_race_target = temp_dir / "parent-race-target";
    auto parent_race_outside = temp_dir / "parent-race-outside";
    std::filesystem::create_directories(parent_race_data / "z-parent");
    std::filesystem::create_directories(parent_race_target / "z-parent");
    std::filesystem::create_directories(parent_race_outside);
    std::ofstream(parent_race_data / "a-marker") << "ready\n";
    {
        std::ofstream filler(parent_race_data / "b-filler", std::ios::binary);
        std::array<char, 1024 * 1024> zeros{};
        for (int i = 0; i < 16; ++i) filler.write(zeros.data(), zeros.size());
    }
    std::ofstream(parent_race_data / "z-parent/escaped") << "must stay inside\n";
    sage::package::PackageManifest parent_race_manifest;
    parent_race_manifest.name = "parent-race";
    parent_race_manifest.version = sage::package::Version::parse("1.0.0-1");
    auto parent_race_pkg = temp_dir / "parent-race-1.0.0-1-x86_64.pkg.tar.zst";
    if (!sage::archive::create_package(
            parent_race_manifest, parent_race_data, parent_race_pkg)) {
        sage::util::log_error("Failed to create parent replacement race fixture");
        return 1;
    }
    std::atomic<bool> parent_replaced{false};
    std::jthread parent_attacker([&](std::stop_token stop) {
        while (!stop.stop_requested()
            && !std::filesystem::exists(parent_race_target / "a-marker")) {
            std::this_thread::yield();
        }
        if (stop.stop_requested()) return;
        std::error_code race_ec;
        std::filesystem::remove_all(parent_race_target / "z-parent", race_ec);
        if (!race_ec) {
            std::filesystem::create_directory_symlink(
                parent_race_outside, parent_race_target / "z-parent", race_ec);
        }
        parent_replaced.store(!race_ec, std::memory_order_release);
    });
    auto parent_race_result = sage::archive::extract_package(
        parent_race_pkg, parent_race_target);
    parent_attacker.request_stop();
    parent_attacker.join();
    if (!parent_replaced.load(std::memory_order_acquire)
        || parent_race_result
        || std::filesystem::exists(parent_race_outside / "escaped")) {
        sage::util::log_error("Archive extraction escaped through a replaced parent directory");
        return 1;
    }

    auto regular_parent_bytes = raw_tar_bytes;
    if (!rename_tar_entry(
            regular_parent_bytes, "data/usr/bin/link", "data/usr/bin/dummy/child")) {
        sage::util::log_error("Failed to create non-directory archive ancestor fixture");
        return 1;
    }
    auto regular_parent_pkg = write_mutated_package(
        regular_parent_bytes, "regular-file-parent");
    auto regular_parent_root = temp_dir / "regular-file-parent-root";
    auto regular_parent_result = regular_parent_pkg
        ? sage::archive::extract_package(*regular_parent_pkg, regular_parent_root)
        : std::expected<sage::archive::ExtractedPackage, std::string>(
            std::unexpected("fixture failed"));
    // The parent path was already seen as a non-directory member: structural
    // rejection must fire before any write under it is attempted.
    if (regular_parent_result
        || std::filesystem::exists(regular_parent_root / "usr/bin/dummy/child")) {
        sage::util::log_error("Non-directory archive ancestor was not rejected");
        return 1;
    }

    // Raw archives cannot bypass usr-merge ownership by omitting the data/bin
    // directory entry: non-base packages must use canonical usr/bin paths.
    auto usr_merge_bytes = raw_tar_bytes;
    if (!rename_tar_entry(
            usr_merge_bytes, "data/usr/bin/dummy", "data/bin/dummy")) {
        sage::util::log_error("Failed to create usr-merge alias fixture");
        return 1;
    }
    auto usr_merge_pkg = write_mutated_package(usr_merge_bytes, "usr-merge-alias");
    auto usr_merge_root = temp_dir / "usr-merge-root";
    std::filesystem::create_directories(usr_merge_root / "usr/bin");
    std::filesystem::create_symlink("usr/bin", usr_merge_root / "bin");
    auto usr_merge_result = usr_merge_pkg
        ? sage::archive::extract_package(*usr_merge_pkg, usr_merge_root)
        : std::expected<sage::archive::ExtractedPackage, std::string>(
            std::unexpected("fixture failed"));
    if (usr_merge_result
        || usr_merge_result.error().find("must use canonical usr/ paths")
            == std::string::npos
        || std::filesystem::exists(usr_merge_root / "usr/bin/dummy")) {
        sage::util::log_error("Usr-merge alias payload bypassed canonical package paths");
        return 1;
    }

    auto base_files_data = temp_dir / "base-files-data";
    std::filesystem::create_directories(base_files_data / "usr");
    constexpr std::array base_files_aliases{
        std::pair{"bin", "usr/bin"},
        std::pair{"sbin", "usr/bin"},
        std::pair{"lib", "usr/lib"},
        std::pair{"lib64", "usr/lib"},
        std::pair{"usr/sbin", "bin"},
        std::pair{"usr/lib64", "lib"},
    };
    for (const auto& [path, target] : base_files_aliases) {
        std::filesystem::create_symlink(target, base_files_data / path);
    }
    sage::package::PackageManifest base_files_manifest;
    base_files_manifest.name = "base-files";
    base_files_manifest.version = sage::package::Version::parse("1.0.0-1");
    auto base_files_pkg = temp_dir / "base-files-1.0.0-1-x86_64.pkg.tar.zst";
    auto base_files_root = temp_dir / "base-files-root";
    auto base_files_pack = sage::archive::create_package(
        base_files_manifest, base_files_data, base_files_pkg);
    auto base_files_extract = base_files_pack
        ? sage::archive::extract_package(base_files_pkg, base_files_root)
        : std::expected<sage::archive::ExtractedPackage, std::string>(
            std::unexpected(base_files_pack.error()));
    bool base_files_aliases_ok = base_files_extract.has_value();
    for (const auto& [path, target] : base_files_aliases) {
        base_files_aliases_ok = base_files_aliases_ok
            && std::filesystem::is_symlink(base_files_root / path)
            && std::filesystem::read_symlink(base_files_root / path) == target;
    }
    if (!base_files_aliases_ok) {
        sage::util::log_error("Base-files could not create the canonical usr-merge alias");
        return 1;
    }

    auto usr_sbin_bytes = raw_tar_bytes;
    if (!rename_tar_entry(
            usr_sbin_bytes, "data/usr/bin/dummy", "data/usr/sbin/dummy")) {
        sage::util::log_error("Failed to create usr/sbin package fixture");
        return 1;
    }
    auto usr_sbin_pkg = write_mutated_package(usr_sbin_bytes, "usr-sbin-path");
    auto usr_sbin_root = temp_dir / "usr-sbin-root";
    // Compatibility-path rejection happens per-member during extraction:
    // inspect only reads the .METADATA section by design.
    auto usr_sbin_result = usr_sbin_pkg
        ? sage::archive::extract_package(*usr_sbin_pkg, usr_sbin_root)
        : std::expected<sage::archive::ExtractedPackage, std::string>(
            std::unexpected("fixture failed"));
    if (usr_sbin_result
        || usr_sbin_result.error().find("compatibility path") == std::string::npos) {
        sage::util::log_error("Package accepted an usr/sbin payload path");
        return 1;
    }

    auto usr_lib64_bytes = raw_tar_bytes;
    if (!rename_tar_entry(
            usr_lib64_bytes, "data/usr/bin/dummy", "data/usr/lib64/dummy")) {
        sage::util::log_error("Failed to create usr/lib64 package fixture");
        return 1;
    }
    auto usr_lib64_pkg = write_mutated_package(usr_lib64_bytes, "usr-lib64-path");
    auto usr_lib64_root = temp_dir / "usr-lib64-root";
    auto usr_lib64_result = usr_lib64_pkg
        ? sage::archive::extract_package(*usr_lib64_pkg, usr_lib64_root)
        : std::expected<sage::archive::ExtractedPackage, std::string>(
            std::unexpected("fixture failed"));
    if (usr_lib64_result
        || usr_lib64_result.error().find("compatibility path") == std::string::npos) {
        sage::util::log_error("Package accepted an usr/lib64 payload path");
        return 1;
    }

    auto invalid_base_files_data = temp_dir / "invalid-base-files-data";
    std::filesystem::create_directories(invalid_base_files_data);
    std::filesystem::create_symlink("opt/bin", invalid_base_files_data / "bin");
    auto invalid_base_files_pkg = malformed_archive_dir / "base-files-invalid.pkg.tar.zst";
    if (!sage::archive::create_package(
            base_files_manifest, invalid_base_files_data, invalid_base_files_pkg)) {
        sage::util::log_error("Failed to create invalid base-files fixture");
        return 1;
    }
    auto invalid_base_files_root = temp_dir / "invalid-base-files-root";
    auto invalid_base_files_extract = sage::archive::extract_package(
        invalid_base_files_pkg, invalid_base_files_root);
    if (invalid_base_files_extract
        || invalid_base_files_extract.error().find("must use canonical usr/ paths")
            == std::string::npos
        || std::filesystem::exists(invalid_base_files_root / "bin")) {
        sage::util::log_error("Base-files accepted a non-canonical usr-merge target");
        return 1;
    }

    auto verify_tar_reader = [&](std::string_view reader, const std::filesystem::path& root) {
        std::filesystem::create_directories(root);
        auto command = std::format("{} -xf \"{}\" -C \"{}\"", reader, pkg_path.string(), root.string());
        return std::system(command.c_str()) == 0 &&
            std::filesystem::exists(root / "data" / long_rel) &&
            std::filesystem::exists(root / "data" / boundary_rel) &&
            std::filesystem::is_directory(root / "data" / boundary_empty_dir) &&
            std::filesystem::is_symlink(root / "data/usr/bin/link") &&
            std::filesystem::read_symlink(root / "data/usr/bin/link").generic_string() == boundary_link_target;
    };
    if (!verify_tar_reader("tar", temp_dir / "tar-sysroot") ||
        !verify_tar_reader("bsdtar", temp_dir / "bsdtar-sysroot")) {
        sage::util::log_error("POSIX USTAR compatibility verification failed");
        return 1;
    }
    sage::util::log_success("2. Deterministic POSIX USTAR + Zstandard Engine OK");

    // 2b. `cmd_build` ELF scan order must be reflected deterministically in the final manifest.
    auto write_test_elf = [](const std::filesystem::path& path,
                             std::string_view soname,
                             std::string_view needed) {
        constexpr std::size_t phoff = 64;
        constexpr std::size_t dynoff = 176;
        constexpr std::size_t stroff = 256;
        constexpr std::uint64_t base = 0x400000;

        std::string strings(1, '\0');
        const std::uint64_t soname_offset = strings.size();
        strings.append(soname);
        strings.push_back('\0');
        const std::uint64_t needed_offset = strings.size();
        strings.append(needed);
        strings.push_back('\0');

        std::vector<std::uint8_t> elf(stroff + strings.size());
        auto put = [&](std::size_t offset, std::uint64_t value, std::size_t width) {
            for (std::size_t i = 0; i < width; ++i) {
                elf[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
            }
        };

        elf[0] = 0x7f;
        elf[1] = 'E';
        elf[2] = 'L';
        elf[3] = 'F';
        elf[4] = 2;
        elf[5] = 1;
        elf[6] = 1;
        put(16, 3, 2);
        put(18, 62, 2);
        put(20, 1, 4);
        put(32, phoff, 8);
        put(52, 64, 2);
        put(54, 56, 2);
        put(56, 2, 2);

        put(64, 1, 4);
        put(68, 4, 4);
        put(80, base, 8);
        put(88, base, 8);
        put(96, elf.size(), 8);
        put(104, elf.size(), 8);
        put(112, 0x1000, 8);

        put(120, 2, 4);
        put(124, 4, 4);
        put(128, dynoff, 8);
        put(136, base + dynoff, 8);
        put(144, base + dynoff, 8);
        put(152, 80, 8);
        put(160, 80, 8);
        put(168, 8, 8);

        auto put_dynamic = [&](std::size_t index, std::uint64_t tag, std::uint64_t value) {
            put(dynoff + index * 16, tag, 8);
            put(dynoff + index * 16 + 8, value, 8);
        };
        put_dynamic(0, 5, base + stroff);
        put_dynamic(1, 10, strings.size());
        put_dynamic(2, 1, needed_offset);
        put_dynamic(3, 14, soname_offset);
        put_dynamic(4, 0, 0);

        std::memcpy(elf.data() + stroff, strings.data(), strings.size());
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(elf.data()), static_cast<std::streamsize>(elf.size()));
        return out.good();
    };

    auto write_elf_recipe = [&](const std::filesystem::path& recipe_dir, bool z_first) {
        std::filesystem::create_directories(recipe_dir);
        if (!write_test_elf(recipe_dir / "a.elf", "liba-provider.so.1", "liba-needed.so.1") ||
            !write_test_elf(recipe_dir / "z.elf", "libz-provider.so.1", "libz-needed.so.1")) {
            return false;
        }

        std::ofstream recipe(recipe_dir / "recipe.toml");
        recipe << R"(schema_version = 1
[package]
name = "elf-order-test"
version = "1.0.0"
release = "1"
description = "ELF manifest ordering fixture"
license = "BSD-2-Clause"
channel = "system"
dependencies = ["declared-runtime"]
provides = ["elf-order"]
install = [
    'mkdir -p "$DESTDIR/usr/lib"',
)";
        if (z_first) {
            recipe << "    'cp \"$RECIPE_DIR/z.elf\" \"$DESTDIR/usr/lib/z-provider.so\"',\n";
            recipe << "    'cp \"$RECIPE_DIR/a.elf\" \"$DESTDIR/usr/lib/a-provider.so\"',\n";
        } else {
            recipe << "    'cp \"$RECIPE_DIR/a.elf\" \"$DESTDIR/usr/lib/a-provider.so\"',\n";
            recipe << "    'cp \"$RECIPE_DIR/z.elf\" \"$DESTDIR/usr/lib/z-provider.so\"',\n";
        }
        recipe << "]\n";
        return recipe.good();
    };

    auto elf_test_root = temp_dir / "elf-order";
    auto z_first_dir = elf_test_root / "z-first";
    auto a_first_dir = elf_test_root / "a-first";
    if (!write_elf_recipe(z_first_dir, true) || !write_elf_recipe(a_first_dir, false)) {
        sage::util::log_error("Failed to create ELF manifest ordering fixtures");
        return 1;
    }

    auto build_elf_fixture = [&](const std::filesystem::path& recipe_dir,
                                 const std::filesystem::path& extract_dir)
        -> std::expected<sage::package::PackageManifest, std::string> {
        CliOptions build_opts;
        build_opts.args = {recipe_dir.string()};
        build_opts.target_root = unpublished_build_root;
        build_opts.no_elf_check = true;
        if (cmd_build(build_opts) != 0) {
            return std::unexpected("cmd_build failed for " + recipe_dir.string());
        }
        auto package_path = recipe_dir / "elf-order-test-1.0.0-1-x86_64.pkg.tar.zst";
        auto extracted = sage::archive::extract_package(package_path, extract_dir);
        if (!extracted) return std::unexpected(extracted.error());
        return std::move(extracted->manifest);
    };

    auto z_first_manifest = build_elf_fixture(z_first_dir, elf_test_root / "z-first-extracted");
    auto a_first_manifest = build_elf_fixture(a_first_dir, elf_test_root / "a-first-extracted");
    auto has_expected_elf_order = [](const sage::package::PackageManifest& built) {
        return built.dependencies.size() == 3 && built.provides.size() == 3 &&
            built.dependencies[0].to_string() == "declared-runtime" &&
            built.dependencies[1].to_string() == "so:liba-needed.so.1" &&
            built.dependencies[2].to_string() == "so:libz-needed.so.1" &&
            built.provides[0] == "elf-order" &&
            built.provides[1] == "so:liba-provider.so.1" &&
            built.provides[2] == "so:libz-provider.so.1";
    };
    auto z_first_hash = sage::util::compute_file_sha256(
        z_first_dir / "elf-order-test-1.0.0-1-x86_64.pkg.tar.zst");
    auto a_first_hash = sage::util::compute_file_sha256(
        a_first_dir / "elf-order-test-1.0.0-1-x86_64.pkg.tar.zst");
    if (!z_first_manifest || !a_first_manifest ||
        !has_expected_elf_order(*z_first_manifest) || !has_expected_elf_order(*a_first_manifest) ||
        !z_first_hash || !a_first_hash || *z_first_hash != *a_first_hash) {
        sage::util::log_error("cmd_build ELF manifest ordering is not deterministic");
        return 1;
    }
    sage::util::log_success("   Deterministic cmd_build ELF Manifest Ordering OK");

    return 0;
}

} // namespace provenance
} // namespace sage::tests
