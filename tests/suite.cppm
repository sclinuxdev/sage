module;
#include <sys/stat.h>
#include <unistd.h>

export module sage.tests;

// Master architecture & subsystem integration suite (`sage-tests` binary).
// Drives the public engine surface end to end; the raw LMDB probes verify the
// on-disk key layout itself, which is exactly what a regression must pin.
import std;
import sage;

import sage.cli;
import sage.cli.build;
import sage.cli.install;
import sage.cli.rebuild;
import sage.cli.remove;

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
export int run_all() {
    sage::util::log_info("Running Sage Master Architecture & Subsystem Integration Test Suite...");

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

    // 2. Tar+Zstd Archive Packaging & Streaming Extractor Test
    auto temp_dir = std::filesystem::temp_directory_path() / "sage_archive_test";
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

    sage::package::PackageManifest manifest;
    manifest.name = "dummy-tool";
    manifest.version = sage::package::Version::parse("1.0.0-1");
    manifest.description = "Test \"mock\" tool with \\ path";
    manifest.license = "BSD-2-Clause";
    manifest.channel = "system";

    auto pkg_path = temp_dir / "dummy-tool-1.0.0-1-x86_64.pkg.tar.zst";
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

    auto extract_root = temp_dir / "sysroot";
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

    // 3. PubGrub Dependency Solver & Toolchain MSRV Resolution Test
    std::vector<sage::package::PackageManifest> repo_pool;
    sage::package::PackageManifest libfoo;
    libfoo.name = "libfoo";
    libfoo.version = sage::package::Version::parse("2.1.0-1");
    libfoo.provides = {"libfoo", "so:libfoo.so.2"};

    sage::package::PackageManifest gcc15;
    gcc15.name = "gcc";
    gcc15.version = sage::package::Version::parse("15.3.0-1");
    gcc15.channel = "toolchain/gcc:15";
    gcc15.provides = {"cc", "c++", "gcc", "toolchain/gcc"};

    sage::package::PackageManifest app;
    app.name = "demo-app";
    app.version = sage::package::Version::parse("1.0.0-1");
    app.dependencies.push_back(sage::package::Dependency::parse("libfoo >= 2.0.0"));
    app.dependencies.push_back(sage::package::Dependency::parse("toolchain/gcc >= 14.0"));

    sage::package::PackageManifest openrc_pkg;
    openrc_pkg.name = "openrc";
    openrc_pkg.version = sage::package::Version::parse("0.54.0-1");
    openrc_pkg.provides = {"openrc", "virtual/init"};

    repo_pool.push_back(libfoo);
    repo_pool.push_back(gcc15);
    repo_pool.push_back(app);
    repo_pool.push_back(openrc_pkg);

    sage::solver::DependencySolver solver(repo_pool);
    auto solve_res = solver.solve({sage::package::Dependency::parse("demo-app")});
    if (!solve_res || solve_res->size() != 3) {
        sage::util::log_error("Dependency & Toolchain MSRV solver test failed (resolved size: {})", solve_res ? solve_res->size() : 0);
        return 1;
    }
    auto dependency_before = [&](std::string_view dependency, std::string_view dependent) {
        auto dependency_it = std::ranges::find(*solve_res, dependency, &sage::package::PackageManifest::name);
        auto dependent_it = std::ranges::find(*solve_res, dependent, &sage::package::PackageManifest::name);
        return dependency_it != solve_res->end()
            && dependent_it != solve_res->end()
            && dependency_it < dependent_it;
    };
    if (!dependency_before("libfoo", "demo-app") || !dependency_before("gcc", "demo-app")) {
        sage::util::log_error("Dependency solver did not return dependency-first install order");
        return 1;
    }
    sage::util::log_success("3. Native PubGrub / CDCL SAT Dependency & MSRV Solver OK");

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

    // 5. LMDB Database & Rebuild Engine Test
    auto db_dir = temp_dir / "db";
    auto db_res = sage::db::Database::open(db_dir);
    if (!db_res) {
        sage::util::log_error("DB open failed: {}", db_res.error());
        return 1;
    }

    sage::package::PackageManifest escaped_manifest;
    escaped_manifest.name = "escaped-metadata";
    escaped_manifest.version = sage::package::Version::parse("7:1.0.0-1");
    escaped_manifest.description = "quoted \"value\" with \\ slash\nnext line";
    sage::package::FileEntry escaped_file;
    escaped_file.path = R"(usr/lib/systemd/system/system-systemd\x2dmute.slice)";
    escaped_manifest.files.push_back(std::move(escaped_file));
    auto escaped_round_trip = sage::package::PackageManifest::parse_toml(escaped_manifest.serialize_toml());
    if (!escaped_round_trip
        || escaped_round_trip->version != escaped_manifest.version
        || escaped_round_trip->description != escaped_manifest.description
        || escaped_round_trip->files.size() != 1
        || escaped_round_trip->files.front().path != escaped_manifest.files.front().path) {
        sage::util::log_error("Package metadata TOML escaping round-trip failed");
        return 1;
    }

    auto embedded_epoch_manifest = sage::package::PackageManifest::parse_toml(R"(
schema_version = 1
[package]
name = "embedded-epoch"
version = "1:2.0-3"
)");
    auto embedded_epoch_index = sage::channel::ChannelIndex::parse_toml(R"(
schema_version = 1
[channel]
name = "core"
[[packages]]
name = "embedded-epoch"
version = "1:2.0-3"
)");
    if (!embedded_epoch_manifest
        || embedded_epoch_manifest->version.epoch != 1
        || embedded_epoch_manifest->version.rel != "3"
        || !embedded_epoch_index
        || embedded_epoch_index->available_packages.size() != 1
        || embedded_epoch_index->available_packages.front().version.epoch != 1
        || embedded_epoch_index->available_packages.front().version.rel != "3") {
        sage::util::log_error("Embedded version epoch/release was not preserved");
        return 1;
    }

    auto absent_release_manifest = sage::package::PackageManifest::parse_toml(R"(
schema_version = 1
[package]
name = "absent-release"
version = "1.0"
)");
    for (const std::string_view architecture : {"amd64", "aarch64", "any", "x86_64"}) {
        auto recipe = sage::package::Recipe::parse_toml(std::format(R"(
schema_version = 1
[package]
name = "architecture-test"
version = "1.0.0"
release = "1"
arch = "{}"
)", architecture));
        if (!recipe || recipe->arch != architecture) {
            sage::util::log_error("Valid package architecture '{}' was rejected", architecture);
            return 1;
        }
    }
    auto invalid_architecture = sage::package::Recipe::parse_toml(R"(
schema_version = 1
[package]
name = "architecture-test"
version = "1.0.0"
release = "1"
arch = "mips"
)");
    if (invalid_architecture
        || !sage::package::package_architecture_matches("any", "aarch64")
        || !sage::package::package_architecture_matches("x86_64", "amd64")
        || sage::package::package_architecture_matches("aarch64", "amd64")) {
        sage::util::log_error("Package architecture validation or matching failed");
        return 1;
    }

    auto absent_release_recipe = sage::package::Recipe::parse_toml(R"(
schema_version = 1
[package]
name = "absent-release"
version = "1.0"
)");
    auto absent_release_index = sage::channel::ChannelIndex::parse_toml(R"(
schema_version = 1
[channel]
name = "core"
[[packages]]
name = "absent-release"
version = "1.0"
)");
    if (!absent_release_manifest || absent_release_manifest->version.rel != "1"
        || !absent_release_recipe || absent_release_recipe->version.rel != "1"
        || !absent_release_index || absent_release_index->available_packages.size() != 1
        || absent_release_index->available_packages.front().version.rel != "1") {
        sage::util::log_error("Absent release did not default to one");
        return 1;
    }

    for (std::string_view wrong_type : {"0", "1", "false"}) {
        auto manifest = sage::package::PackageManifest::parse_toml(std::format(R"(
schema_version = 1
[package]
name = "wrong-release-type"
version = "1.0"
release = {}
)", wrong_type));
        auto recipe = sage::package::Recipe::parse_toml(std::format(R"(
schema_version = 1
[package]
name = "wrong-release-type"
version = "1.0"
release = {}
)", wrong_type));
        auto index = sage::channel::ChannelIndex::parse_toml(std::format(R"(
schema_version = 1
[channel]
name = "core"
[[packages]]
name = "wrong-release-type"
version = "1.0"
release = {}
)", wrong_type));
        if (manifest || recipe || index) {
            sage::util::log_error(
                "Package, recipe, or channel parser accepted wrong-typed release {}", wrong_type);
            return 1;
        }
    }

    for (std::string_view invalid : {"0", "alpha", "1x", "-1"}) {
        auto manifest = sage::package::PackageManifest::parse_toml(std::format(R"(
schema_version = 1
[package]
name = "bad-release"
version = "1.0"
release = "{}"
)", invalid));
        auto recipe = sage::package::Recipe::parse_toml(std::format(R"(
schema_version = 1
[package]
name = "bad-release"
version = "1.0"
release = "{}"
)", invalid));
        auto index = sage::channel::ChannelIndex::parse_toml(std::format(R"(
schema_version = 1
[channel]
name = "core"
[[packages]]
name = "bad-release"
version = "1.0"
release = "{}"
)", invalid));
        if (manifest || recipe || index) {
            sage::util::log_error("Package, recipe, or channel parser accepted invalid release '{}'", invalid);
            return 1;
        }
    }
    auto positive_release_recipe = sage::package::Recipe::parse_toml(R"(
schema_version = 1
[package]
name = "positive-release"
version = "1.0"
release = "10"
)");
    if (!positive_release_recipe || positive_release_recipe->version.rel != "10") {
        sage::util::log_error("Recipe parser rejected a positive decimal release");
        return 1;
    }

    // The files table keeps one owner per line so shared directories can carry
    // several; sole_owner() sees through that encoding for single-owner paths.
    auto sole_owner = [](sage::db::Database& db, std::string_view path)
        -> std::optional<std::string> {
        auto owners = db.get_path_owners(path);
        if (!owners || owners->size() != 1) return std::nullopt;
        return std::move(owners->front());
    };

    sage::package::FileEntry owned_file;
    owned_file.path = "usr/bin/database-owned";
    {
        auto owner_txn = db_res->begin_write_txn();
        if (!owner_txn
            || !db_res->register_files(*owner_txn, "database-owner", "system", {owned_file})
            || !owner_txn->commit()) {
            sage::util::log_error("Failed to create database file ownership fixture");
            return 1;
        }
    }
    sage::package::FileEntry unowned_file;
    unowned_file.path = "usr/bin/database-unowned";
    bool database_conflict_rejected = false;
    {
        auto conflict_txn = db_res->begin_write_txn();
        if (!conflict_txn) {
            sage::util::log_error("Failed to open database file conflict transaction");
            return 1;
        }
        auto conflict_registration = db_res->register_files(
            *conflict_txn, "database-challenger", "system", {unowned_file, owned_file});
        database_conflict_rejected = !conflict_registration;
    }
    if (!database_conflict_rejected
        || sole_owner(*db_res, owned_file.path) != "database-owner:system"
        || sole_owner(*db_res, unowned_file.path)) {
        sage::util::log_error("Database file conflict registration was not atomic");
        return 1;
    }

    // A package identity captured before the writer lock must be revalidated
    // inside that write transaction, and same-name ownership is not sufficient.
    auto migration_db = sage::db::Database::open(temp_dir / "migration-race-db");
    sage::package::FileEntry migration_file;
    migration_file.path = "usr/bin/migration-race";
    sage::package::PackageManifest migration_old;
    migration_old.name = "migration-race";
    migration_old.version = sage::package::Version::parse("1.0.0-1");
    migration_old.channel = "system";
    migration_old.files = {migration_file};
    if (!migration_db) {
        sage::util::log_error("Failed to create migration race database");
        return 1;
    }
    {
        auto setup_txn = migration_db->begin_write_txn();
        if (!setup_txn
            || !migration_db->put_package(*setup_txn, migration_old)
            || !migration_db->register_files(
                *setup_txn, migration_old.name, migration_old.channel, migration_old.files)
            || !setup_txn->commit()) {
            sage::util::log_error("Failed to populate migration race database");
            return 1;
        }
    }
    const auto expected_migration_identity = std::optional{
        sage::package::package_identity(migration_old)};
    auto migration_new = migration_old;
    migration_new.version = sage::package::Version::parse("2.0.0-1");
    migration_new.channel = "runtime/python:3.12";
    {
        auto concurrent_txn = migration_db->begin_write_txn();
        if (!concurrent_txn
            || !migration_db->unregister_files(
                *concurrent_txn, migration_old.files, "migration-race:system")
            || !migration_db->put_package(*concurrent_txn, migration_new)
            || !migration_db->register_files(
                *concurrent_txn, migration_new.name, migration_new.channel, migration_new.files)
            || !concurrent_txn->commit()) {
            sage::util::log_error("Failed to simulate concurrent channel migration");
            return 1;
        }
    }
    {
        auto install_txn = migration_db->begin_write_txn();
        const std::string stale_owner = "migration-race:system";
        auto stale_snapshot = install_txn
            ? load_install_snapshot(
                *migration_db,
                *install_txn,
                migration_old.name,
                expected_migration_identity)
            : std::expected<std::optional<sage::package::PackageManifest>, std::string>(
                std::unexpected("transaction failed"));
        auto stale_owner_check = install_txn
            ? migration_db->check_file_conflicts(
                *install_txn,
                std::optional<std::string_view>{stale_owner},
                migration_new.files)
            : std::expected<void, std::string>(std::unexpected("transaction failed"));
        if (stale_snapshot || stale_owner_check) {
            sage::util::log_error("Concurrent same-name channel migration bypassed identity revalidation");
            return 1;
        }
    }
    auto preserved_migration = migration_db->get_package(migration_new.name);
    if (!preserved_migration || !*preserved_migration
        || sage::package::package_identity(**preserved_migration)
            != sage::package::package_identity(migration_new)
        || sole_owner(*migration_db, migration_file.path)
            != "migration-race:runtime/python:3.12") {
        sage::util::log_error("Rejected stale migration changed the concurrent package state");
        return 1;
    }
    // Split-package upgrades hand files between package identities inside a
    // single install transaction (issue #26): when foo-libs claims the
    // library, the monolithic foo's claim is still on record. The projected
    // transaction ownership tolerates exactly that handover, while every
    // owner that survives the transaction stays fatal.
    {
        auto handover_db = sage::db::Database::open(temp_dir / "split-handover-db");
        if (!handover_db) {
            sage::util::log_error("Failed to create split handover database");
            return 1;
        }
        sage::package::FileEntry handover_bin;
        handover_bin.path = "usr/bin/handover";
        sage::package::FileEntry handover_lib;
        handover_lib.path = "usr/lib/libhandover.so";
        sage::package::PackageManifest handover_monolith;
        handover_monolith.name = "handover";
        handover_monolith.version = sage::package::Version::parse("1.0.0-1");
        handover_monolith.channel = "system";
        handover_monolith.files = {handover_bin, handover_lib};
        {
            auto setup_txn = handover_db->begin_write_txn();
            if (!setup_txn
                || !handover_db->put_package(*setup_txn, handover_monolith)
                || !handover_db->register_files(
                    *setup_txn, handover_monolith.name,
                    handover_monolith.channel, handover_monolith.files)
                || !setup_txn->commit()) {
                sage::util::log_error("Failed to populate split handover fixture");
                return 1;
            }
        }
        const std::string monolith_owner = "handover:system";
        bool strict_claim_rejected = false;
        bool wrong_releasor_rejected = false;
        bool handover_accepted = false;
        bool kept_path_allowed = false;
        bool surviving_owner_fatal = false;
        {
            auto txn = handover_db->begin_write_txn();
            if (!txn) {
                sage::util::log_error("Failed to open split handover transaction");
                return 1;
            }
            // Without handover context the previous owner's claim is fatal...
            strict_claim_rejected = !handover_db->register_files(
                *txn, "handover-libs", "system", {handover_lib});
            // ...a release attributed to a bystander opens no door either...
            wrong_releasor_rejected = !handover_db->register_files(
                *txn, "handover-libs", "system", {handover_lib},
                std::nullopt,
                sage::db::ReleasedClaims{
                    {std::string{handover_lib.path}, {"bystander"}}});
            // ...and the genuine handover goes through, reowning the path.
            auto handover_registration = handover_db->register_files(
                *txn, "handover-libs", "system", {handover_lib},
                std::nullopt,
                sage::db::ReleasedClaims{
                    {std::string{handover_lib.path}, {"handover"}}});
            handover_accepted = handover_registration.has_value();
            if (!handover_accepted || !txn->commit()) {
                sage::util::log_error("Failed to commit split handover registration");
                return 1;
            }
        }
        if (!strict_claim_rejected || !wrong_releasor_rejected || !handover_accepted
            || sole_owner(*handover_db, handover_lib.path) != "handover-libs:system") {
            sage::util::log_error("In-transaction file handover conflict semantics were wrong");
            return 1;
        }
        {
            auto txn = handover_db->begin_write_txn();
            if (!txn) {
                sage::util::log_error("Failed to reopen split handover transaction");
                return 1;
            }
            // The upgrading monolith keeps its own remaining paths through the
            // allowed-owner rule, but a path the new owner holds is still fatal.
            kept_path_allowed = handover_db->check_file_conflicts(
                *txn,
                std::optional<std::string_view>{monolith_owner},
                {handover_bin}).has_value();
            surviving_owner_fatal = !handover_db->check_file_conflicts(
                *txn,
                std::optional<std::string_view>{monolith_owner},
                {handover_lib}).has_value();
        }
        if (!kept_path_allowed || !surviving_owner_fatal
            || sole_owner(*handover_db, handover_bin.path) != monolith_owner) {
            sage::util::log_error("Allowed-owner rule broke during the handover regression");
            return 1;
        }
    }

    auto corrupt_target = temp_dir / "corrupt-target";
    auto corrupt_db_dir = corrupt_target / "var/lib/sage";
    {
        auto raw_env = sage::vendor::lmdb::MdbEnv::create(corrupt_db_dir);
        if (!raw_env) {
            sage::util::log_error("Failed to create corrupt database fixture");
            return 1;
        }
        auto raw_txn = sage::vendor::lmdb::MdbTxn::begin(*raw_env);
        if (!raw_txn) {
            sage::util::log_error("Failed to begin corrupt database fixture transaction");
            return 1;
        }
        auto raw_packages = sage::vendor::lmdb::MdbDbi::open(
            *raw_txn, "packages", sage::vendor::lmdb::flag_create);
        auto raw_files = sage::vendor::lmdb::MdbDbi::open(
            *raw_txn, "files", sage::vendor::lmdb::flag_create);
        if (!raw_packages || !raw_files
            || !raw_packages->put(
                *raw_txn, "aaa-mismatch", "[package]\nname = \"different-name\"\nversion = \"1.0.0\"\n")
            || !raw_packages->put(*raw_txn, "broken", "[package]\nname = \"broken\n")
            || !raw_files->put(*raw_txn, "usr/bin/broken", "broken:system")) {
            sage::util::log_error("Failed to populate corrupt database fixture");
            return 1;
        }
        auto raw_commit = raw_txn->commit();
        if (!raw_commit) {
            sage::util::log_error("Failed to commit corrupt database fixture");
            return 1;
        }
    }
    std::filesystem::create_directories(corrupt_target / "etc/sage");
    CliOptions corrupt_install_opts;
    corrupt_install_opts.target_root = corrupt_target;
    corrupt_install_opts.args = {"dummy-tool"};
    if (cmd_install(corrupt_install_opts) == 0) {
        sage::util::log_error("Install accepted a corrupt installed package manifest");
        return 1;
    }
    auto corrupt_db = sage::db::Database::open(corrupt_db_dir, true);
    auto corrupt_package = corrupt_db
        ? corrupt_db->get_package("broken")
        : std::expected<std::optional<sage::package::PackageManifest>, std::string>(
            std::unexpected("database open failed"));
    auto mismatched_package = corrupt_db
        ? corrupt_db->get_package("aaa-mismatch")
        : std::expected<std::optional<sage::package::PackageManifest>, std::string>(
            std::unexpected("database open failed"));
    if (!corrupt_db
        || corrupt_db->list_installed_packages()
        || corrupt_package
        || mismatched_package
        || sole_owner(*corrupt_db, "usr/bin/broken") != "broken:system") {
        sage::util::log_error("Corrupt manifest failure changed existing file ownership");
        return 1;
    }

    sage::config::SystemConfig sys_cfg;
    sys_cfg.providers["virtual/init"] = "openrc";
    sys_cfg.capabilities["virtual/init"] = sage::config::CapabilityKind::Exclusive;
    sys_cfg.cache_dir = temp_dir / "cache";

    // Reconcile installs provider packages for real now, so back it with a
    // local file:// channel whose openrc archive carries an actual payload.
    auto reconcile_repo = temp_dir / "reconcile-repo";
    auto openrc_pkg_dir = temp_dir / "openrc-pkg";
    std::filesystem::create_directories(openrc_pkg_dir / "usr/bin");
    {
        std::ofstream openrc_bin(openrc_pkg_dir / "usr/bin/openrc");
        openrc_bin << "#!/bin/sh\nexit 0\n";
    }
    std::filesystem::create_directories(reconcile_repo);
    if (!sage::archive::create_package(openrc_pkg, openrc_pkg_dir,
            reconcile_repo / "openrc-0.54.0-1-x86_64.pkg.tar.zst")) {
        sage::util::log_error("Failed to pack the reconcile fixture archive");
        return 1;
    }
    {
        std::ofstream idx(reconcile_repo / "index.toml");
        idx << "schema_version = 1\n\n[channel]\nname = \"reconcile-test\"\n\n[[packages]]\n"
            << "name = \"openrc\"\nversion = \"0.54.0\"\nrelease = \"1\"\n"
            << "channel = \"system\"\narch = \"x86_64\"\n"
            << "file = \"openrc-0.54.0-1-x86_64.pkg.tar.zst\"\n";
    }
    sage::config::ChannelConfig reconcile_channel;
    reconcile_channel.name = "reconcile-test";
    reconcile_channel.url = "file://" + reconcile_repo.string();
    reconcile_channel.enabled = true;
    sys_cfg.channels.push_back(reconcile_channel);

    auto plan_res = sage::rebuild::ReconcileEngine::calculate_diff(*db_res, sys_cfg);
    if (!plan_res) {
        sage::util::log_error("Reconcile plan failed: {}", plan_res.error());
        return 1;
    }
    auto exec_res = sage::rebuild::ReconcileEngine::execute(*db_res, *plan_res, extract_root, false);
    if (!exec_res || !std::filesystem::exists(extract_root / "usr/bin/openrc")) {
        sage::util::log_error("Reconcile execute did not install the new provider payload: {}",
            exec_res.error_or("payload missing"));
        return 1;
    }

    // A reconcile plan must not remove a same-name package that changed before
    // its write transaction began. Preserve both the newer package and the old
    // provider lock so the caller can recalculate from a fresh snapshot.
    auto reconcile_race_db = sage::db::Database::open(temp_dir / "reconcile-race-db");
    sage::package::PackageManifest old_init;
    old_init.name = "old-init";
    old_init.version = sage::package::Version::parse("1.0.0-1");
    old_init.provides = {"old-init", "virtual/init"};
    if (!reconcile_race_db) {
        sage::util::log_error("Failed to create reconcile snapshot fixture");
        return 1;
    }
    {
        auto setup_txn = reconcile_race_db->begin_write_txn();
        if (!setup_txn
            || !reconcile_race_db->put_package(*setup_txn, old_init)
            || !reconcile_race_db->set_system_provider(*setup_txn, "virtual/init", old_init.name)
            || !setup_txn->commit()) {
            sage::util::log_error("Failed to populate reconcile snapshot fixture");
            return 1;
        }
    }
    auto stale_plan = sage::rebuild::ReconcileEngine::calculate_diff(
        *reconcile_race_db, sys_cfg);
    auto replacement_init = old_init;
    replacement_init.version = sage::package::Version::parse("2.0.0-1");
    {
        auto update_txn = reconcile_race_db->begin_write_txn();
        if (!stale_plan || !update_txn
            || !reconcile_race_db->put_package(*update_txn, replacement_init)
            || !update_txn->commit()) {
            sage::util::log_error("Failed to update reconcile snapshot fixture");
            return 1;
        }
    }
    auto stale_execute = sage::rebuild::ReconcileEngine::execute(
        *reconcile_race_db, *stale_plan, extract_root, false);
    auto preserved_init = reconcile_race_db->get_package(old_init.name);
    auto preserved_provider = reconcile_race_db->get_system_provider("virtual/init");
    if (stale_execute
        || !preserved_init || !*preserved_init
        || (**preserved_init).version != replacement_init.version
        || !preserved_provider || !*preserved_provider
        || **preserved_provider != old_init.name) {
        sage::util::log_error("Reconcile executed against a stale package snapshot");
        return 1;
    }

    // A provider lock can also change without changing the package record.
    // Reject the stale plan before it overwrites that newer binding.
    {
        auto reset_txn = reconcile_race_db->begin_write_txn();
        if (!reset_txn
            || !reconcile_race_db->put_package(*reset_txn, old_init)
            || !reconcile_race_db->set_system_provider(*reset_txn, "virtual/init", old_init.name)
            || !reset_txn->commit()) {
            sage::util::log_error("Failed to reset reconcile provider fixture");
            return 1;
        }
    }
    auto stale_provider_plan = sage::rebuild::ReconcileEngine::calculate_diff(
        *reconcile_race_db, sys_cfg);
    {
        auto update_txn = reconcile_race_db->begin_write_txn();
        if (!stale_provider_plan || !update_txn
            || !reconcile_race_db->set_system_provider(
                *update_txn, "virtual/init", "concurrent-init")
            || !update_txn->commit()) {
            sage::util::log_error("Failed to update reconcile provider fixture");
            return 1;
        }
    }
    auto stale_provider_execute = sage::rebuild::ReconcileEngine::execute(
        *reconcile_race_db, *stale_provider_plan, extract_root, false);
    auto concurrent_provider = reconcile_race_db->get_system_provider("virtual/init");
    auto provider_package = reconcile_race_db->get_package(old_init.name);
    if (stale_provider_execute
        || !concurrent_provider || !*concurrent_provider
        || **concurrent_provider != "concurrent-init"
        || !provider_package || !*provider_package
        || (**provider_package).version != old_init.version) {
        sage::util::log_error("Reconcile overwrote a concurrently changed provider lock");
        return 1;
    }
    sage::util::log_success("5. Declarative System Reconcile & Triggers Engine (sage rebuild) OK");

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

    // 7. Ephemeral Shell Environment Synthesis Test
    auto shell_env = sage::channel::ProfileManager::generate_shell_env(extract_root, {
        sage::channel::SubChannelSpec::parse("toolchain/llvm:22")
    });
    if (!shell_env.contains("CC") || shell_env["CC"].find("llvm/22/bin/clang") == std::string::npos) {
        sage::util::log_error("Ephemeral shell environment generation failed");
        return 1;
    }
    sage::util::log_success("7. Ephemeral Sandboxed Shell Environment Generator (sage shell) OK");

    // 8. Local Repository Indexing & Zero-Copy file:// Protocol Test
    const std::string escaped_channel_name = "core \"quoted\" \\ channel";
    auto local_repo = temp_dir / "local-repo";
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

    // 9. End-to-End `sage install` & `sage remove` into isolated Target Root Test
    auto isolated_target = temp_dir / "target_root";
    std::filesystem::create_directories(isolated_target / "etc/sage");
    std::ofstream chan_f(isolated_target / "etc/sage/channels.toml");
    chan_f << "schema_version = 1\n\n[[channels]]\nname = \"core\"\nurl = \"file://" << local_repo.string() << "\"\nscope = \"system\"\npriority = 100\nenabled = true\n";
    chan_f.close();

    CliOptions inst_opts;
    inst_opts.target_root = isolated_target;
    inst_opts.args = {"dummy-tool"};
    int inst_ret = cmd_install(inst_opts);
    if (inst_ret != 0 || !std::filesystem::exists(isolated_target / "usr/bin/dummy")) {
        sage::util::log_error("End-to-end sage install to target root failed");
        return 1;
    }

    CliOptions rem_opts;
    rem_opts.target_root = isolated_target;
    rem_opts.args = {"dummy-tool"};
    int rem_ret = cmd_remove(rem_opts);
    if (rem_ret != 0 || std::filesystem::exists(isolated_target / "usr/bin/dummy")) {
        sage::util::log_error("End-to-end sage remove from target root failed");
        return 1;
    }

    auto write_test_channel = [](const std::filesystem::path& target,
                                 const std::filesystem::path& repo) {
        std::filesystem::create_directories(target / "etc/sage");
        std::ofstream channels(target / "etc/sage/channels.toml");
        channels
            << "schema_version = 1\n\n[[channels]]\nname = \"core\"\nurl = \"file://"
            << repo.string()
            << "\"\nscope = \"system\"\npriority = 100\nenabled = true\n";
        return channels.good();
    };
    auto read_test_file = [](const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        return std::string(
            std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    };

    // Trigger timing fixtures execute on the host with sysroot "/". Their
    // commands touch only this test's temporary directory, so the suite does
    // not need a compiler or static libc merely to build a chroot-local probe.
    auto host_trigger_args = [](const std::vector<std::filesystem::path>& required,
                                const std::filesystem::path& counter) {
        std::string script;
        for (const auto& path : required) {
            script += std::format("test -e \"{}\" && ", path.string());
        }
        script += std::format("echo x >> \"{}\"", counter.string());
        return std::vector<std::string>{"-c", "'" + script + "'"};
    };

    // A solver selection, direct archive request, and extracted archive must
    // all refer to the same complete package identity.
    auto version_repo = temp_dir / "version-repo";
    auto version_1_data = temp_dir / "version-1-data";
    auto version_2_data = temp_dir / "version-2-data";
    std::filesystem::create_directories(version_repo);
    std::filesystem::create_directories(version_1_data / "usr/bin");
    std::filesystem::create_directories(version_1_data / "usr/share/version-trigger");
    std::filesystem::create_directories(version_2_data / "usr/bin");
    std::filesystem::create_directories(version_2_data / "usr/share/version-trigger");
    std::ofstream(version_1_data / "usr/bin/versioned") << "version 1\n";
    std::ofstream(version_1_data / "usr/share/version-trigger/libversioned.so")
        << "version 1 library\n";
    std::ofstream(version_2_data / "usr/bin/versioned") << "version 2\n";
    std::ofstream(version_2_data / "usr/share/version-trigger/libversioned.so")
        << "version 2 library\n";

    sage::package::PackageManifest version_1;
    version_1.name = "versioned-package";
    version_1.version = sage::package::Version::parse("1.0.0-1");
    sage::package::Trigger version_trigger;
    version_trigger.name = "version-cache";
    version_trigger.on_paths = {"usr/share/version-trigger/"};
    version_trigger.exec = "/bin/sh";
    auto version_trigger_count = temp_dir / "version-trigger-count";
    version_trigger.args = host_trigger_args({}, version_trigger_count);
    version_1.triggers = {version_trigger};
    sage::package::PackageManifest version_2 = version_1;
    version_2.version = sage::package::Version::parse("2.0.0-1");
    version_2.triggers.clear();
    auto version_1_pkg = version_repo / "versioned-package-1.0.0-1-x86_64.pkg.tar.zst";
    auto version_2_pkg = version_repo / "versioned-package-2.0.0-1-x86_64.pkg.tar.zst";
    if (!sage::archive::create_package(version_1, version_1_data, version_1_pkg)
        || !sage::archive::create_package(version_2, version_2_data, version_2_pkg)
        || !sage::archive::generate_repo_index(version_repo, "core")) {
        sage::util::log_error("Failed to create package identity fixtures");
        return 1;
    }

    auto version_target = temp_dir / "version-target";
    if (!write_test_channel(version_target, version_repo)) {
        sage::util::log_error("Failed to write package identity test channel");
        return 1;
    }
    CliOptions version_install;
    version_install.target_root = version_target;
    version_install.args = {"versioned-package"};
    if (cmd_install(version_install, "/") != 0
        || read_test_file(version_target / "usr/bin/versioned") != "version 2\n") {
        sage::util::log_error("Solver selection did not install the selected archive version");
        return 1;
    }
    auto version_db = sage::db::Database::open(version_target / "var/lib/sage/data.mdb", true);
    auto selected_version = version_db
        ? version_db->get_package("versioned-package")
        : std::expected<std::optional<sage::package::PackageManifest>, std::string>(
            std::unexpected("database open failed"));
    if (!selected_version || !*selected_version
        || (**selected_version).version != version_2.version) {
        sage::util::log_error("Installed database version differs from extracted archive version");
        return 1;
    }

    // Equal identities from multiple sources must keep solver metadata and the
    // extracted archive bound to the same highest-priority source.
    auto high_priority_repo = temp_dir / "source-high";
    auto low_priority_repo = temp_dir / "source-low";
    auto high_priority_data = temp_dir / "source-high-data";
    auto low_priority_data = temp_dir / "source-low-data";
    std::filesystem::create_directories(high_priority_repo);
    std::filesystem::create_directories(low_priority_repo);
    std::filesystem::create_directories(high_priority_data / "usr/bin");
    std::filesystem::create_directories(low_priority_data / "usr/bin");
    std::ofstream(high_priority_data / "usr/bin/source-bound") << "high priority\n";
    std::ofstream(low_priority_data / "usr/bin/source-bound") << "low priority\n";
    sage::package::PackageManifest source_bound;
    source_bound.name = "source-bound";
    source_bound.version = sage::package::Version::parse("1.0.0-1");
    auto high_priority_archive =
        high_priority_repo / "source-bound-1.0.0-1-x86_64.pkg.tar.zst";
    auto low_priority_archive =
        low_priority_repo / "source-bound-1.0.0-1-x86_64.pkg.tar.zst";
    if (!sage::archive::create_package(
            source_bound, high_priority_data, high_priority_archive)
        || !sage::archive::create_package(
            source_bound, low_priority_data, low_priority_archive)
        || !sage::archive::generate_repo_index(high_priority_repo, "high")
        || !sage::archive::generate_repo_index(low_priority_repo, "low")) {
        sage::util::log_error("Failed to create multi-source identity fixtures");
        return 1;
    }
    auto source_target = temp_dir / "source-target";
    std::filesystem::create_directories(source_target / "etc/sage");
    std::ofstream source_channels(source_target / "etc/sage/channels.toml");
    source_channels
        << "schema_version = 1\n\n"
        << "[[channels]]\nname = \"low\"\nurl = \"file://"
        << low_priority_repo.string()
        << "\"\nscope = \"system\"\npriority = 10\nenabled = true\n\n"
        << "[[channels]]\nname = \"high\"\nurl = \"file://"
        << high_priority_repo.string()
        << "\"\nscope = \"system\"\npriority = 100\nenabled = true\n";
    source_channels.close();
    CliOptions source_install;
    source_install.target_root = source_target;
    source_install.args = {source_bound.name};
    if (cmd_install(source_install) != 0
        || read_test_file(source_target / "usr/bin/source-bound") != "high priority\n") {
        sage::util::log_error("Solver metadata and archive source were not bound together");
        return 1;
    }

    auto direct_target = temp_dir / "direct-version-target";
    if (!write_test_channel(direct_target, version_repo)) {
        sage::util::log_error("Failed to write direct archive test channel");
        return 1;
    }
    CliOptions direct_install;
    direct_install.target_root = direct_target;
    direct_install.args = {version_1_pkg.string()};
    if (cmd_install(direct_install, "/") != 0
        || read_test_file(direct_target / "usr/bin/versioned") != "version 1\n"
        || read_test_file(version_trigger_count) != "x\n") {
        sage::util::log_error("Direct archive install was not locked to its exact version");
        return 1;
    }
    {
        auto direct_db = sage::db::Database::open(
            direct_target / "var/lib/sage/data.mdb", true);
        auto direct_version = direct_db
            ? direct_db->get_package("versioned-package")
            : std::expected<std::optional<sage::package::PackageManifest>, std::string>(
                std::unexpected("database open failed"));
        if (!direct_version || !*direct_version
            || (**direct_version).version != version_1.version) {
            sage::util::log_error("Direct archive manifest identity was not preserved in the database");
            return 1;
        }
    }

    // A direct archive may intentionally rebuild the same identity with a
    // different payload. Paths dropped by that rebuild must be removed from
    // both the target root and the ownership database.
    auto same_identity_data = temp_dir / "same-identity-data";
    std::filesystem::create_directories(same_identity_data / "usr/bin");
    std::ofstream(same_identity_data / "usr/bin/replacement")
        << "same identity replacement\n";
    auto same_identity_archive =
        temp_dir / "versioned-package-same-identity.pkg.tar.zst";
    if (!sage::archive::create_package(
            version_1, same_identity_data, same_identity_archive)) {
        sage::util::log_error("Failed to create same-identity reinstall fixture");
        return 1;
    }
    direct_install.args = {same_identity_archive.string()};
    if (cmd_install(direct_install, "/") != 0
        || std::filesystem::exists(direct_target / "usr/bin/versioned")
        || std::filesystem::exists(
            direct_target / "usr/share/version-trigger/libversioned.so")
        || read_test_file(direct_target / "usr/bin/replacement")
            != "same identity replacement\n"
        || read_test_file(version_trigger_count) != "x\nx\n") {
        sage::util::log_error("Same-identity reinstall left a stale payload path");
        return 1;
    }
    auto same_identity_db = sage::db::Database::open(
        direct_target / "var/lib/sage/data.mdb", true);
    if (!same_identity_db) {
        sage::util::log_error("Failed to open same-identity reinstall database");
        return 1;
    }
    if (sole_owner(*same_identity_db, "usr/bin/versioned")
        || sole_owner(*same_identity_db, "usr/share/version-trigger/libversioned.so")
        || sole_owner(*same_identity_db, "usr/bin/replacement")
            != "versioned-package:system") {
        sage::util::log_error("Same-identity reinstall left stale file ownership");
        return 1;
    }

    // A normal same-package upgrade may replace files already owned by that
    // package, while still selecting the exact newer archive.
    direct_install.args = {"versioned-package"};
    if (cmd_install(direct_install, "/") != 0
        || read_test_file(direct_target / "usr/bin/versioned") != "version 2\n") {
        sage::util::log_error("Same-package upgrade was blocked or extracted the wrong archive");
        return 1;
    }
    auto upgraded_db = sage::db::Database::open(
        direct_target / "var/lib/sage/data.mdb", true);
    auto upgraded_version = upgraded_db
        ? upgraded_db->get_package("versioned-package")
        : std::expected<std::optional<sage::package::PackageManifest>, std::string>(
            std::unexpected("database open failed"));
    if (!upgraded_version || !*upgraded_version
        || (**upgraded_version).version != version_2.version) {
        sage::util::log_error("Same-package upgrade did not record the extracted version");
        return 1;
    }

    // An explicit archive remains exact even when a newer same-name package is
    // installed. This is the local-package downgrade path used for rollback.
    direct_install.args = {version_1_pkg.string()};
    if (cmd_install(direct_install, "/") != 0
        || read_test_file(direct_target / "usr/bin/versioned") != "version 1\n") {
        sage::util::log_error("Direct archive downgrade was silently skipped");
        return 1;
    }
    auto downgraded_db = sage::db::Database::open(
        direct_target / "var/lib/sage/data.mdb", true);
    auto downgraded_version = downgraded_db
        ? downgraded_db->get_package("versioned-package")
        : std::expected<std::optional<sage::package::PackageManifest>, std::string>(
            std::unexpected("database open failed"));
    if (!downgraded_version || !*downgraded_version
        || (**downgraded_version).version != version_1.version) {
        sage::util::log_error("Direct archive downgrade did not update installed metadata");
        return 1;
    }

    // The same version from a different architecture/channel is also a distinct
    // direct archive identity, and must replace stale files owned by the old one.
    auto alternate_data = temp_dir / "alternate-identity-data";
    auto alternate_binary = alternate_data / "opt/channels/llvm/42/bin/versioned";
    std::filesystem::create_directories(alternate_binary.parent_path());
    std::ofstream(alternate_binary) << "alternate identity\n";
    sage::package::PackageManifest alternate_identity = version_1;
    alternate_identity.arch = "any";
    alternate_identity.channel = "toolchain/llvm:42";
    auto alternate_archive = temp_dir / "versioned-package-alternate.pkg.tar.zst";
    if (!sage::archive::create_package(
            alternate_identity, alternate_data, alternate_archive)) {
        sage::util::log_error("Failed to create alternate direct archive identity fixture");
        return 1;
    }
    direct_install.args = {alternate_archive.string()};
    if (cmd_install(direct_install, "/") != 0
        || std::filesystem::exists(direct_target / "usr/bin/versioned")
        || read_test_file(direct_target / "opt/channels/llvm/42/bin/versioned")
            != "alternate identity\n") {
        sage::util::log_error("Direct archive identity replacement was silently skipped");
        return 1;
    }
    auto alternate_db = sage::db::Database::open(
        direct_target / "var/lib/sage/data.mdb", true);
    auto alternate_record = alternate_db
        ? alternate_db->get_package("versioned-package")
        : std::expected<std::optional<sage::package::PackageManifest>, std::string>(
            std::unexpected("database open failed"));
    if (!alternate_record || !*alternate_record
        || sage::package::package_identity(**alternate_record)
            != sage::package::package_identity(alternate_identity)
        || sole_owner(*alternate_db, "opt/channels/llvm/42/bin/versioned")
            != "versioned-package:toolchain/llvm:42"
        || sole_owner(*alternate_db, "usr/bin/versioned")) {
        sage::util::log_error("Alternate direct archive identity was not recorded");
        return 1;
    }

    auto mismatch_repo = temp_dir / "identity-mismatch-repo";
    std::filesystem::create_directories(mismatch_repo);
    auto mismatch_v1 = mismatch_repo / version_1_pkg.filename();
    auto mismatch_v2 = mismatch_repo / version_2_pkg.filename();
    std::filesystem::copy_file(version_1_pkg, mismatch_v1);
    std::filesystem::copy_file(version_2_pkg, mismatch_v2);
    if (!sage::archive::generate_repo_index(mismatch_repo, "core")) {
        sage::util::log_error("Failed to generate archive identity mismatch index");
        return 1;
    }
    std::filesystem::copy_file(
        version_1_pkg, mismatch_v2, std::filesystem::copy_options::overwrite_existing);
    auto mismatch_target = temp_dir / "identity-mismatch-target";
    if (!write_test_channel(mismatch_target, mismatch_repo)) {
        sage::util::log_error("Failed to write archive identity mismatch channel");
        return 1;
    }
    CliOptions mismatch_install;
    mismatch_install.target_root = mismatch_target;
    mismatch_install.args = {"versioned-package"};
    if (cmd_install(mismatch_install) == 0
        || std::filesystem::exists(mismatch_target / "usr/bin/versioned")) {
        sage::util::log_error("Mismatched selected and archive identities mutated the target root");
        return 1;
    }

    // Different packages must not overwrite the same ordinary file. The first
    // package remains committed because the second fails before extraction.
    auto owner_repo = temp_dir / "owner-conflict-repo";
    auto owner_a_data = temp_dir / "owner-a-data";
    auto owner_b_data = temp_dir / "owner-b-data";
    std::filesystem::create_directories(owner_repo);
    std::filesystem::create_directories(owner_a_data / "usr/bin");
    std::filesystem::create_directories(owner_b_data / "usr/bin");
    std::ofstream(owner_a_data / "usr/bin/shared-file") << "owned by A\n";
    std::ofstream(owner_b_data / "usr/bin/shared-file") << "owned by B\n";
    sage::package::PackageManifest owner_a;
    owner_a.name = "owner-a";
    owner_a.version = sage::package::Version::parse("1.0.0-1");
    sage::package::PackageManifest owner_b;
    owner_b.name = "owner-b";
    owner_b.version = sage::package::Version::parse("1.0.0-1");
    owner_b.dependencies.push_back(sage::package::Dependency::parse("owner-a"));
    auto owner_a_pkg = owner_repo / "owner-a-1.0.0-1-x86_64.pkg.tar.zst";
    auto owner_b_pkg = owner_repo / "owner-b-1.0.0-1-x86_64.pkg.tar.zst";
    if (!sage::archive::create_package(owner_a, owner_a_data, owner_a_pkg)
        || !sage::archive::create_package(owner_b, owner_b_data, owner_b_pkg)
        || !sage::archive::generate_repo_index(owner_repo, "core")) {
        sage::util::log_error("Failed to create file ownership conflict fixtures");
        return 1;
    }
    auto owner_target = temp_dir / "owner-conflict-target";
    if (!write_test_channel(owner_target, owner_repo)) {
        sage::util::log_error("Failed to write file ownership conflict channel");
        return 1;
    }
    CliOptions owner_install;
    owner_install.target_root = owner_target;
    owner_install.args = {"owner-b"};
    if (cmd_install(owner_install) == 0) {
        sage::util::log_error("Different packages silently overwrote the same regular file");
        return 1;
    }
    auto owner_db = sage::db::Database::open(owner_target / "var/lib/sage/data.mdb", true);
    auto owner_a_record = owner_db
        ? owner_db->get_package("owner-a")
        : std::expected<std::optional<sage::package::PackageManifest>, std::string>(
            std::unexpected("database open failed"));
    auto owner_b_record = owner_db
        ? owner_db->get_package("owner-b")
        : std::expected<std::optional<sage::package::PackageManifest>, std::string>(
            std::unexpected("database open failed"));
    if (!owner_db || !owner_a_record || !*owner_a_record
        || !owner_b_record || *owner_b_record
        || sole_owner(*owner_db, "usr/bin/shared-file") != "owner-a:system"
        || read_test_file(owner_target / "usr/bin/shared-file") != "owned by A\n") {
        sage::util::log_error("File conflict changed the first package or its ownership record");
        return 1;
    }
    // Issue #26 end-to-end: splitting a monolithic package must install --
    // foo -> foo + foo-fw moves usr/lib/firmware/foo/fw.bin between
    // identities inside one transaction instead of colliding with the old
    // revision. (A .so payload would drag in the host ldconfig trigger.)
    auto handover_repo = temp_dir / "split-repo";
    auto foo_monolith_data = temp_dir / "split-monolith-data";
    auto foo_split_bin_data = temp_dir / "split-bin-data";
    auto foo_split_fw_data = temp_dir / "split-fw-data";
    std::filesystem::create_directories(handover_repo);
    std::filesystem::create_directories(foo_monolith_data / "usr/bin");
    std::filesystem::create_directories(foo_monolith_data / "usr/lib/firmware/foo");
    std::filesystem::create_directories(foo_split_bin_data / "usr/bin");
    std::filesystem::create_directories(foo_split_fw_data / "usr/lib/firmware/foo");
    std::ofstream(foo_monolith_data / "usr/bin/foo") << "foo 1\n";
    std::ofstream(foo_monolith_data / "usr/lib/firmware/foo/fw.bin") << "fw 1\n";
    std::ofstream(foo_split_bin_data / "usr/bin/foo") << "foo 2\n";
    std::ofstream(foo_split_fw_data / "usr/lib/firmware/foo/fw.bin") << "fw 2 fw-pkg\n";
    sage::package::PackageManifest foo_monolith;
    foo_monolith.name = "foo";
    foo_monolith.version = sage::package::Version::parse("1.0.0-1");
    sage::package::PackageManifest foo_split = foo_monolith;
    foo_split.version = sage::package::Version::parse("2.0.0-1");
    foo_split.dependencies.push_back(
        sage::package::Dependency::parse("foo-fw >= 2.0.0"));
    sage::package::PackageManifest foo_fw;
    foo_fw.name = "foo-fw";
    foo_fw.version = sage::package::Version::parse("2.0.0-1");
    if (!sage::archive::create_package(
            foo_monolith, foo_monolith_data, handover_repo / "foo-1.0.0-1-x86_64.pkg.tar.zst")
        || !sage::archive::generate_repo_index(handover_repo, "core")) {
        sage::util::log_error("Failed to create monolithic pre-split fixture");
        return 1;
    }
    auto handover_target = temp_dir / "split-target";
    if (!write_test_channel(handover_target, handover_repo)) {
        sage::util::log_error("Failed to write split handover channel");
        return 1;
    }
    CliOptions handover_install;
    handover_install.target_root = handover_target;
    handover_install.args = {"foo"};
    if (cmd_install(handover_install) != 0
        || read_test_file(handover_target / "usr/bin/foo") != "foo 1\n") {
        sage::util::log_error("Monolithic pre-split fixture failed to install");
        return 1;
    }
    if (!sage::archive::create_package(
            foo_split, foo_split_bin_data, handover_repo / "foo-2.0.0-1-x86_64.pkg.tar.zst")
        || !sage::archive::create_package(
            foo_fw, foo_split_fw_data,
            handover_repo / "foo-fw-2.0.0-1-x86_64.pkg.tar.zst")
        || !sage::archive::generate_repo_index(handover_repo, "core")) {
        sage::util::log_error("Failed to create split upgrade fixtures");
        return 1;
    }
    if (cmd_install(handover_install) != 0
        || read_test_file(handover_target / "usr/bin/foo") != "foo 2\n"
        || read_test_file(handover_target / "usr/lib/firmware/foo/fw.bin")
            != "fw 2 fw-pkg\n") {
        sage::util::log_error("Split-package upgrade rejected an in-transaction file handover");
        return 1;
    }
    {
        auto handover_db = sage::db::Database::open(
            handover_target / "var/lib/sage/data.mdb", true);
        if (!handover_db
            || sole_owner(*handover_db, "usr/bin/foo") != "foo:system"
            || sole_owner(*handover_db, "usr/lib/firmware/foo/fw.bin")
                != "foo-fw:system") {
            sage::util::log_error("Split upgrade recorded wrong file ownership");
            return 1;
        }
    }
    sage::util::log_success("   Split-Package Transactional Handover Upgrade OK");
    // ---- Issue #9 durable state machine: crash-boundary coverage ---------
    // Invariants under test: a committed operation is always recoverable
    // (journal fsynced before the LMDB commit, replay idempotent), orphaned
    // staging is garbage-collected at the next entry, and abandon is the only
    // destructive exit.
    {
        auto recovery_root = temp_dir / "recovery-root";
        std::filesystem::create_directories(recovery_root);
        auto recovery_db_res = sage::db::Database::open(
            recovery_root / "var/lib/sage/data.mdb");
        if (!recovery_db_res) {
            sage::util::log_error("Failed to create recovery fixture database: {}",
                recovery_db_res.error());
            return 1;
        }
        auto& recovery_db = *recovery_db_res;

        // Stage a one-file install and persist its journal WITHOUT publishing,
        // then commit the operation record. This is the exact on-disk shape
        // left by a hard kill between the LMDB commit and the publish step.
        auto make_pending = [&](std::string_view id, std::string_view leaf) -> std::expected<void, std::string> {
            auto txn = sage::archive::FilesystemTransaction::create(recovery_root);
            if (!txn) return std::unexpected(txn.error());
            const std::string stage_rel =
                std::format("staged/usr/bin/{}", leaf);
            auto fd = txn->open_staged_file(stage_rel, 0600);
            if (!fd) return std::unexpected(fd.error());
            std::string payload = std::format("{} payload\n", leaf);
            if (::write(*fd, payload.data(), payload.size())
                != static_cast<ssize_t>(payload.size())) {
                return std::unexpected(std::format("short staged write for {}", leaf));
            }
            ::close(*fd);
            txn->plan_ensure_dir("usr/bin");
            txn->plan_put_file(
                std::format("usr/bin/{}", leaf), stage_rel, 0755);
            if (auto synced = txn->sync_staging(); !synced) return synced;
            sage::archive::JournalContext ctx;
            ctx.kind = "install";
            ctx.final = true;
            ctx.sysroot = recovery_root.string();
            auto sha = txn->persist_journal(
                sage::archive::render_journal(ctx, txn->journal_entries()));
            if (!sha) return std::unexpected(sha.error());
            auto wtxn = recovery_db.begin_write_txn();
            if (!wtxn) return std::unexpected("recovery fixture write txn failed");
            auto put = recovery_db.put_operation(*wtxn,
                {std::string{id}, "install",
                 std::string{sage::db::phase_filesystem_pending},
                 txn->relative_dir(), *sha});
            if (!put) return put;
            // The instance dies here after persist_journal(): the destructor
            // must preserve the evidence directory for recovery.
            if (!wtxn->commit()) return std::unexpected("recovery fixture commit failed");
            return {};
        };
        const std::string recoverable_id(32, 'a');
        const std::string abandoned_id(32, 'b');
        if (auto made = make_pending(recoverable_id, "tool-a"); !made) {
            sage::util::log_error("Failed to stage recoverable fixture: {}", made.error());
            return 1;
        }

        // Resume publishes the journal and finalizes: live tree has the file
        // with its real mode, the record is gone, the staging dir retired.
        auto resumed = sage::rebuild::resume_pending_operations(recovery_db, recovery_root);
        if (!resumed || resumed->finalized != 1) {
            sage::util::log_error("Pending operation was not recovered: {}",
                resumed ? std::format("finalized {}", resumed->finalized) : resumed.error());
            return 1;
        }
        auto tool_path = recovery_root / "usr/bin/tool-a";
        std::error_code mode_ec;
        const bool executable = (std::filesystem::status(tool_path, mode_ec).permissions()
            & std::filesystem::perms::owner_exec) != std::filesystem::perms::none;
        if (mode_ec || read_test_file(tool_path) != "tool-a payload\n" || !executable) {
            sage::util::log_error("Recovered publish lost content or mode of usr/bin/tool-a");
            return 1;
        }
        // Read probes run inside a scoped txn: an open reader must never
        // overlap the next resume call's own transactions.
        {
            auto settled_txn = recovery_db.begin_read_txn();
            auto record_after = settled_txn
                ? recovery_db.get_operation(*settled_txn, recoverable_id)
                : std::expected<std::optional<sage::db::FilesystemOperationRecord>, std::string>(
                    std::unexpected("read txn failed"));
            if (!record_after || record_after->has_value()
                || !sage::archive::list_transaction_dirs(recovery_root).empty()) {
                sage::util::log_error("Finalized operation left a record or staging behind");
                return 1;
            }
        }

        // Replay is idempotent: resuming again finds nothing to do.
        auto again = sage::rebuild::resume_pending_operations(recovery_db, recovery_root);
        if (!again || again->finalized != 0) {
            sage::util::log_error("Second resume was not idempotent: {}",
                again ? std::format("finalized {}", again->finalized) : again.error());
            return 1;
        }

        // A transaction directory with no LMDB record is an orphan from a
        // pre-commit crash: silently collected at the next entry.
        {
            auto orphan = sage::archive::FilesystemTransaction::create(recovery_root);
            if (!orphan) {
                sage::util::log_error("Failed to create orphan fixture: {}", orphan.error());
                return 1;
            }
            sage::archive::JournalContext ctx;
            ctx.kind = "remove";
            if (auto persisted = orphan->persist_journal(
                    sage::archive::render_journal(ctx, {}));
                !persisted) {
                sage::util::log_error("Failed to persist orphan journal: {}", persisted.error());
                return 1;
            }
        }
        auto gc = sage::rebuild::resume_pending_operations(recovery_db, recovery_root);
        if (!gc || !sage::archive::list_transaction_dirs(recovery_root).empty()) {
            sage::util::log_error("Orphan transaction directory survived recovery GC");
            return 1;
        }

        // Abandon is the explicit destructive escape: the record disappears,
        // the evidence directory retires, and nothing reaches the live tree.
        if (auto made = make_pending(abandoned_id, "tool-b"); !made) {
            sage::util::log_error("Failed to stage abandon fixture: {}", made.error());
            return 1;
        }
        auto abandoned = sage::rebuild::resume_pending_operations(
            recovery_db, recovery_root, abandoned_id);
        auto abandoned_txn = recovery_db.begin_read_txn();
        auto abandoned_record = abandoned_txn
            ? recovery_db.get_operation(*abandoned_txn, abandoned_id)
            : std::expected<std::optional<sage::db::FilesystemOperationRecord>, std::string>(
                std::unexpected("read txn failed"));
        if (!abandoned || abandoned->finalized != 0
            || std::filesystem::exists(recovery_root / "usr/bin/tool-b")
            || !abandoned_record || abandoned_record->has_value()
            || !sage::archive::list_transaction_dirs(recovery_root).empty()) {
            sage::util::log_error("Abandon path did not retire the stuck operation cleanly");
            return 1;
        }
        sage::util::log_success("   Durable Operation Recovery & Orphan GC OK");
    }

    // Issue #9 repro: the old revision owns usr/lib/foo/ and the admin dropped
    // foreign state inside it; the new payload drops that subtree. The staged
    // protocol preserves the non-empty directory while the upgrade succeeds --
    // the pre-protocol code failed here with ENOTEMPTY and forked DB from disk.
    {
        auto fork_repo = temp_dir / "fork-repo";
        auto fork_v1_data = temp_dir / "fork-v1-data";
        auto fork_v2_data = temp_dir / "fork-v2-data";
        std::filesystem::create_directories(fork_repo);
        std::filesystem::create_directories(fork_v1_data / "usr/bin");
        std::filesystem::create_directories(fork_v1_data / "usr/lib/foo");
        std::filesystem::create_directories(fork_v2_data / "usr/bin");
        std::ofstream(fork_v1_data / "usr/bin/foo") << "foo v1\n";
        std::ofstream(fork_v1_data / "usr/lib/foo/plugin.dat") << "plugin\n";
        std::ofstream(fork_v2_data / "usr/bin/foo") << "foo v2\n";
        sage::package::PackageManifest fork_old;
        fork_old.name = "foo";
        fork_old.version = sage::package::Version::parse("1.0.0-1");
        sage::package::PackageManifest fork_new = fork_old;
        fork_new.version = sage::package::Version::parse("2.0.0-1");
        if (!sage::archive::create_package(
                fork_old, fork_v1_data, fork_repo / "foo-1.0.0-1-x86_64.pkg.tar.zst")
            || !sage::archive::generate_repo_index(fork_repo, "core")) {
            sage::util::log_error("Failed to create fork-scenario monolith fixture");
            return 1;
        }
        auto fork_target = temp_dir / "fork-target";
        if (!write_test_channel(fork_target, fork_repo)) {
            sage::util::log_error("Failed to write fork-scenario channel");
            return 1;
        }
        CliOptions fork_install;
        fork_install.target_root = fork_target;
        fork_install.args = {"foo"};
        if (cmd_install(fork_install) != 0
            || read_test_file(fork_target / "usr/lib/foo/plugin.dat") != "plugin\n") {
            sage::util::log_error("Fork-scenario monolith failed to install");
            return 1;
        }
        // Foreign state appears inside the package-owned directory.
        std::ofstream(fork_target / "usr/lib/foo/user.conf") << "admin state\n";
        if (!sage::archive::create_package(
                fork_new, fork_v2_data, fork_repo / "foo-2.0.0-1-x86_64.pkg.tar.zst")
            || !sage::archive::generate_repo_index(fork_repo, "core")) {
            sage::util::log_error("Failed to create fork-scenario upgrade fixture");
            return 1;
        }
        auto fork_db = sage::db::Database::open(
            fork_target / "var/lib/sage/data.mdb", true);
        if (cmd_install(fork_install) != 0
            || read_test_file(fork_target / "usr/bin/foo") != "foo v2\n"
            || read_test_file(fork_target / "usr/lib/foo/user.conf") != "admin state\n"
            || std::filesystem::exists(fork_target / "usr/lib/foo/plugin.dat")
            || !fork_db
            || sole_owner(*fork_db, "usr/lib/foo")) {
            sage::util::log_error("Non-empty stale directory forked the upgrade or disk state");
            return 1;
        }
        sage::util::log_success("   Non-Empty Stale Directory Upgrade Survival OK");
    }

    auto transaction_repo = temp_dir / "transaction-repo";
    auto transaction_a_data = temp_dir / "transaction-a-data";
    auto transaction_b_data = temp_dir / "transaction-b-data";
    auto transaction_target = temp_dir / "transaction-target";
    auto failed_install_trigger_count = temp_dir / "failed-install-trigger-count";
    std::filesystem::create_directories(transaction_repo);
    std::filesystem::create_directories(transaction_a_data / "usr/bin");
    std::filesystem::create_directories(
        transaction_a_data / "usr/share/transaction-trigger");
    std::ofstream(transaction_a_data / "usr/bin/transaction-a") << "committed package\n";
    std::ofstream(transaction_a_data / "usr/share/transaction-trigger/committed")
        << "committed trigger input\n";
    auto transaction_clang = transaction_a_data / "opt/channels/llvm/99/bin/clang";
    std::filesystem::create_directories(transaction_clang.parent_path());
    std::ofstream(transaction_clang) << "#!/bin/sh\nexit 0\n";
    std::filesystem::permissions(
        transaction_clang,
        std::filesystem::perms::owner_all
            | std::filesystem::perms::group_read
            | std::filesystem::perms::group_exec
            | std::filesystem::perms::others_read
            | std::filesystem::perms::others_exec);
    std::filesystem::create_directories(transaction_b_data / "usr/share");
    std::filesystem::create_symlink("elsewhere", transaction_b_data / "usr/share/blocked");

    sage::package::PackageManifest transaction_a;
    transaction_a.name = "transaction-a";
    transaction_a.version = sage::package::Version::parse("1.0.0-1");
    transaction_a.channel = "toolchain/llvm:99";
    sage::package::Trigger failed_install_trigger;
    failed_install_trigger.name = "failed-install-cache";
    failed_install_trigger.on_paths = {"usr/share/transaction-trigger/"};
    failed_install_trigger.exec = "/bin/sh";
    failed_install_trigger.args = host_trigger_args(
        {transaction_target / "usr/share/transaction-trigger/committed"},
        failed_install_trigger_count);
    transaction_a.triggers = {failed_install_trigger};
    sage::package::PackageManifest transaction_b;
    transaction_b.name = "transaction-b";
    transaction_b.version = sage::package::Version::parse("1.0.0-1");
    transaction_b.channel = "system";
    transaction_b.dependencies.push_back(sage::package::Dependency::parse("transaction-a"));

    auto transaction_a_pkg = transaction_repo / "transaction-a-1.0.0-1-x86_64.pkg.tar.zst";
    auto transaction_b_pkg = transaction_repo / "transaction-b-1.0.0-1-x86_64.pkg.tar.zst";
    if (!sage::archive::create_package(transaction_a, transaction_a_data, transaction_a_pkg)
        || !sage::archive::create_package(transaction_b, transaction_b_data, transaction_b_pkg)
        || !sage::archive::generate_repo_index(transaction_repo, "core")) {
        sage::util::log_error("Failed to create multi-package transaction fixture");
        return 1;
    }

    std::filesystem::create_directories(transaction_target / "etc/sage");
    std::filesystem::create_directories(transaction_target / "usr/share/blocked");
    std::ofstream(transaction_target / "usr/share/blocked/keep") << "must survive\n";
    std::ofstream transaction_channels(transaction_target / "etc/sage/channels.toml");
    transaction_channels
        << "schema_version = 1\n\n[[channels]]\nname = \"core\"\nurl = \"file://"
        << transaction_repo.string()
        << "\"\nscope = \"system\"\npriority = 100\nenabled = true\n";
    transaction_channels.close();

    CliOptions transaction_install;
    transaction_install.target_root = transaction_target;
    transaction_install.args = {"transaction-b"};
    if (cmd_install(transaction_install, "/") == 0) {
        sage::util::log_error("Multi-package install accepted a later package path conflict");
        return 1;
    }
    auto transaction_db = sage::db::Database::open(
        transaction_target / "var/lib/sage/data.mdb", true);
    if (!transaction_db) {
        sage::util::log_error("Failed to inspect multi-package transaction database");
        return 1;
    }
    auto transaction_a_record = transaction_db->get_package("transaction-a");
    auto transaction_b_record = transaction_db->get_package("transaction-b");
    auto transaction_cc_link = transaction_target / "etc/sage/profiles/default/bin/cc";
    std::error_code transaction_cc_ec;
    if (!transaction_a_record || !*transaction_a_record
        || !transaction_b_record || *transaction_b_record
        || !std::filesystem::exists(transaction_target / "usr/bin/transaction-a")
        || !std::filesystem::exists(transaction_target / "usr/share/blocked/keep")
        || !std::filesystem::is_symlink(transaction_cc_link, transaction_cc_ec)
        || std::filesystem::read_symlink(transaction_cc_link, transaction_cc_ec)
            != "/opt/channels/llvm/99/bin/clang"
        || !std::filesystem::exists(transaction_target / "etc/profile.d/sage-channels.sh")
        || read_test_file(failed_install_trigger_count) != "x\n") {
        sage::util::log_error(
            "A later package failure desynchronized an earlier committed package or skipped its post-processing");
        return 1;
    }

    // Issue #18 end-to-end: a fixed-exec trigger pointing at an executable
    // the target root does not have must not fail an install when optional
    // -- every package is committed, so only a warning is warranted -- while
    // a required trigger still aborts. The round-trip through
    // serialize_triggers_toml -> .METADATA/triggers.toml -> parse keeps the
    // required flag visible in the installed manifest.
    auto policy_repo = temp_dir / "trigger-policy-repo";
    auto optional_policy_data = temp_dir / "trigger-optional-data";
    auto required_policy_data = temp_dir / "trigger-required-data";
    auto policy_target = temp_dir / "trigger-policy-target";
    std::filesystem::create_directories(policy_repo);
    std::filesystem::create_directories(optional_policy_data / "usr/share/trigger-policy");
    std::filesystem::create_directories(required_policy_data / "usr/share/trigger-policy");
    std::ofstream(optional_policy_data / "usr/share/trigger-policy/payload") << "optional\n";
    std::ofstream(required_policy_data / "usr/share/trigger-policy/payload") << "required\n";

    sage::package::PackageManifest optional_policy_pkg;
    optional_policy_pkg.name = "optional-trigger-pkg";
    optional_policy_pkg.version = sage::package::Version::parse("1.0.0-1");
    optional_policy_pkg.triggers.push_back(sage::package::Trigger{
        .name = "optional-missing-exec",
        .on_paths = {"usr/share/trigger-policy/"},
        .on_capability = {},
        .required = false,
        .exec = "/usr/bin/sage-no-such-post-transaction-tool",
        .args = {},
        .run_capability = {},
    });
    sage::package::PackageManifest required_policy_pkg = optional_policy_pkg;
    required_policy_pkg.name = "required-trigger-pkg";
    required_policy_pkg.triggers.front().name = "required-missing-exec";
    required_policy_pkg.triggers.front().required = true;

    auto optional_policy_archive =
        policy_repo / "optional-trigger-pkg-1.0.0-1-x86_64.pkg.tar.zst";
    auto required_policy_archive =
        policy_repo / "required-trigger-pkg-1.0.0-1-x86_64.pkg.tar.zst";
    if (!sage::archive::create_package(
            optional_policy_pkg, optional_policy_data, optional_policy_archive)
        || !sage::archive::create_package(
            required_policy_pkg, required_policy_data, required_policy_archive)
        || !sage::archive::generate_repo_index(policy_repo, "core")) {
        sage::util::log_error("Failed to create trigger policy fixtures");
        return 1;
    }
    std::filesystem::create_directories(policy_target / "etc/sage");
    std::ofstream policy_channels(policy_target / "etc/sage/channels.toml");
    policy_channels
        << "schema_version = 1\n\n[[channels]]\nname = \"core\"\nurl = \"file://"
        << policy_repo.string()
        << "\"\nscope = \"system\"\npriority = 100\nenabled = true\n";
    policy_channels.close();

    CliOptions optional_policy_install;
    optional_policy_install.target_root = policy_target;
    optional_policy_install.args = {"optional-trigger-pkg"};
    if (cmd_install(optional_policy_install, "/") != 0) {
        sage::util::log_error("Optional trigger with a missing executable failed the install");
        return 1;
    }
    {
        auto policy_db = sage::db::Database::open(
            policy_target / "var/lib/sage/data.mdb", true);
        if (!policy_db) {
            sage::util::log_error("Failed to inspect trigger policy database");
            return 1;
        }
        auto optional_record = policy_db->get_package("optional-trigger-pkg");
        if (!optional_record || !*optional_record
            || (**optional_record).triggers.size() != 1
            || (**optional_record).triggers.front().required
            || (**optional_record).triggers.front().exec
                != "/usr/bin/sage-no-such-post-transaction-tool"
            || !std::filesystem::exists(policy_target / "usr/share/trigger-policy/payload")) {
            sage::util::log_error(
                "Optional-missing-exec trigger did not survive packaging or the install diverged");
            return 1;
        }
    }

    CliOptions required_policy_install;
    required_policy_install.target_root = policy_target;
    required_policy_install.args = {"required-trigger-pkg"};
    if (cmd_install(required_policy_install, "/") == 0) {
        sage::util::log_error("Required trigger with a missing executable did not fail the install");
        return 1;
    }
    sage::util::log_success("   Trigger Required/Optional Exec Policy OK");

    // Split packages in one toolchain slot must refresh activation as each
    // package commits. The dependency installs libraries first; the compiler
    // package that follows is what makes the cc alias possible. Aggregate
    // triggers run only after both packages are present, and duplicate trigger
    // declarations resolving to one command execute once.
    auto split_repo = temp_dir / "split-toolchain-repo";
    auto split_libs_data = temp_dir / "split-toolchain-libs-data";
    auto split_compiler_data = temp_dir / "split-toolchain-compiler-data";
    auto split_guard_data = temp_dir / "split-toolchain-guard-data";
    auto split_target = temp_dir / "split-toolchain-target";
    auto split_trigger_count = temp_dir / "split-trigger-count";
    auto split_library = split_libs_data / "opt/channels/llvm/77/lib/libsplit.so";
    auto split_compiler_library =
        split_compiler_data / "opt/channels/llvm/77/lib/libsplit-compiler.so";
    auto split_clang = split_compiler_data / "opt/channels/llvm/77/bin/clang";
    std::filesystem::create_directories(split_repo);
    std::filesystem::create_directories(split_library.parent_path());
    std::filesystem::create_directories(split_compiler_library.parent_path());
    std::filesystem::create_directories(split_clang.parent_path());
    std::filesystem::create_directories(split_guard_data / "usr/share");
    std::ofstream(split_library) << "split toolchain library\n";
    std::ofstream(split_compiler_library) << "split compiler library\n";
    std::ofstream(split_clang) << "#!/bin/sh\nexit 0\n";
    std::ofstream(split_guard_data / "usr/share/split-trigger-guard") << "guard\n";
    std::filesystem::permissions(
        split_clang,
        std::filesystem::perms::owner_all
            | std::filesystem::perms::group_read
            | std::filesystem::perms::group_exec
            | std::filesystem::perms::others_read
            | std::filesystem::perms::others_exec);

    sage::package::PackageManifest split_libs;
    split_libs.name = "split-toolchain-libs";
    split_libs.version = sage::package::Version::parse("1.0.0-1");
    split_libs.channel = "toolchain/llvm:77";
    sage::package::Trigger split_trigger;
    split_trigger.name = "split-cache-primary";
    split_trigger.on_paths = {"opt/channels/llvm/77/lib/"};
    split_trigger.exec = "/bin/sh";
    split_trigger.args = host_trigger_args(
        {
            split_target / "opt/channels/llvm/77/lib/libsplit.so",
            split_target / "opt/channels/llvm/77/lib/libsplit-compiler.so",
        },
        split_trigger_count);
    auto duplicate_split_trigger = split_trigger;
    duplicate_split_trigger.name = "split-cache-duplicate";
    split_libs.triggers = {split_trigger, duplicate_split_trigger};
    sage::package::PackageManifest split_compiler;
    split_compiler.name = "split-toolchain-compiler";
    split_compiler.version = sage::package::Version::parse("1.0.0-1");
    split_compiler.channel = "toolchain/llvm:77";
    split_compiler.dependencies.push_back(
        sage::package::Dependency::parse("split-toolchain-libs"));
    sage::package::PackageManifest split_guard;
    split_guard.name = "split-trigger-guard";
    split_guard.version = sage::package::Version::parse("1.0.0-1");
    sage::package::Trigger failed_remove_trigger;
    failed_remove_trigger.name = "failed-remove-trigger";
    failed_remove_trigger.on_paths = {"opt/channels/llvm/77/bin/"};
    failed_remove_trigger.required = true; // must hard-fail removal's trigger pass
    failed_remove_trigger.exec = "/usr/bin/sage-missing-remove-trigger";
    split_guard.triggers = {failed_remove_trigger};

    auto split_libs_archive =
        split_repo / "split-toolchain-libs-1.0.0-1-x86_64.pkg.tar.zst";
    auto split_compiler_archive =
        split_repo / "split-toolchain-compiler-1.0.0-1-x86_64.pkg.tar.zst";
    auto split_guard_archive =
        split_repo / "split-trigger-guard-1.0.0-1-x86_64.pkg.tar.zst";
    if (!sage::archive::create_package(split_libs, split_libs_data, split_libs_archive)
        || !sage::archive::create_package(
            split_compiler, split_compiler_data, split_compiler_archive)
        || !sage::archive::create_package(split_guard, split_guard_data, split_guard_archive)
        || !sage::archive::generate_repo_index(split_repo, "core")) {
        sage::util::log_error("Failed to create split toolchain fixtures");
        return 1;
    }

    if (!write_test_channel(split_target, split_repo)) {
        sage::util::log_error("Failed to write split toolchain test channel");
        return 1;
    }
    CliOptions split_install;
    split_install.target_root = split_target;
    split_install.args = {"split-toolchain-compiler"};
    if (cmd_install(split_install, "/") != 0) {
        sage::util::log_error("Failed to install split toolchain packages");
        return 1;
    }
    split_install.args = {"split-trigger-guard"};
    if (cmd_install(split_install, "/") != 0) {
        sage::util::log_error("Failed to install post-remove trigger guard");
        return 1;
    }
    auto split_cc_link = split_target / "etc/sage/profiles/default/bin/cc";
    std::error_code split_cc_ec;
    if (!std::filesystem::is_symlink(split_cc_link, split_cc_ec)
        || std::filesystem::read_symlink(split_cc_link, split_cc_ec)
            != "/opt/channels/llvm/77/bin/clang"
        || read_test_file(split_trigger_count) != "x\n") {
        sage::util::log_error(
            "Split toolchain activation or aggregate trigger timing is incorrect");
        return 1;
    }

    auto split_owner_db = sage::db::Database::open(
        split_target / "var/lib/sage/data.mdb", true);
    if (!split_owner_db
        || sole_owner(*split_owner_db, "opt/channels/llvm/77/bin/clang")
            != "split-toolchain-compiler:toolchain/llvm:77") {
        sage::util::log_error("Split toolchain removal fixture has no registered file owner");
        return 1;
    }

    // Replacing an owned regular file with a non-empty directory forces a real
    // filesystem removal error. The command must fail and keep both DB records.
    auto installed_split_clang = split_target / "opt/channels/llvm/77/bin/clang";
    std::filesystem::remove(installed_split_clang);
    std::filesystem::create_directories(installed_split_clang);
    std::ofstream(installed_split_clang / "keep") << "block removal\n";
    CliOptions split_remove;
    split_remove.target_root = split_target;
    split_remove.args = {"split-toolchain-compiler"};
    if (cmd_remove(split_remove) == 0) {
        sage::util::log_error("Package removal ignored a real filesystem error");
        return 1;
    }
    {
        auto split_db = sage::db::Database::open(
            split_target / "var/lib/sage/data.mdb", true);
        auto split_compiler_record = split_db
            ? split_db->get_package("split-toolchain-compiler")
            : std::expected<std::optional<sage::package::PackageManifest>, std::string>(
                std::unexpected("database open failed"));
        auto split_libs_record = split_db
            ? split_db->get_package("split-toolchain-libs")
            : std::expected<std::optional<sage::package::PackageManifest>, std::string>(
                std::unexpected("database open failed"));
        if (!split_compiler_record || !*split_compiler_record
            || !split_libs_record || !*split_libs_record) {
            sage::util::log_error("Failed split removal discarded installed database records");
            return 1;
        }
    }

    // Restore the owned file and verify removal uses the sysroot-relative paths
    // stored in the manifest instead of prepending the toolchain root twice.
    // A remaining package deliberately fails its post-remove trigger; removal
    // still commits, and profile regeneration must discard the now-dangling cc
    // link before that trigger error is returned.
    std::error_code restore_ec;
    std::filesystem::remove_all(installed_split_clang, restore_ec);
    if (restore_ec) {
        sage::util::log_error("Failed to restore split toolchain fixture: {}", restore_ec.message());
        return 1;
    }
    std::ofstream(installed_split_clang) << "#!/bin/sh\nexit 0\n";
    if (cmd_remove(split_remove) == 0
        || std::filesystem::exists(split_target / "opt/channels/llvm/77/bin/clang")
        || std::filesystem::exists(split_target / "opt/channels/llvm/77/lib/libsplit.so")
        || std::filesystem::exists(
            split_target / "opt/channels/llvm/77/lib/libsplit-compiler.so")
        || std::filesystem::is_symlink(split_cc_link)) {
        sage::util::log_error(
            "Failed post-remove trigger left package paths or a stale profile link");
        return 1;
    }
    auto removed_split_db = sage::db::Database::open(
        split_target / "var/lib/sage/data.mdb", true);
    auto removed_split_packages = removed_split_db
        ? removed_split_db->list_installed_packages()
        : std::expected<std::vector<sage::package::PackageManifest>, std::string>(
            std::unexpected("database open failed"));
    if (!removed_split_packages || removed_split_packages->size() != 1
        || removed_split_packages->front().name != "split-trigger-guard") {
        sage::util::log_error("Failed trigger changed the committed removal state");
        return 1;
    }
    // An empty directory declared by two packages is shared: both register as
    // owners, one removal only releases its claim, and the last removal takes
    // the directory away.
    auto shared_repo = temp_dir / "shared-dir-repo";
    auto shared_a_data = temp_dir / "shared-a-data";
    auto shared_b_data = temp_dir / "shared-b-data";
    std::filesystem::create_directories(shared_repo);
    std::filesystem::create_directories(shared_a_data / "usr/bin");
    std::filesystem::create_directories(shared_a_data / "usr/share/common");
    std::filesystem::create_directories(shared_b_data / "usr/bin");
    std::filesystem::create_directories(shared_b_data / "usr/share/common");
    std::ofstream(shared_a_data / "usr/bin/share-a") << "a\n";
    std::ofstream(shared_b_data / "usr/bin/share-b") << "b\n";
    sage::package::PackageManifest share_a;
    share_a.name = "share-a";
    share_a.version = sage::package::Version::parse("1.0.0-1");
    sage::package::PackageManifest share_b;
    share_b.name = "share-b";
    share_b.version = sage::package::Version::parse("1.0.0-1");
    auto share_a_pkg = shared_repo / "share-a-1.0.0-1-x86_64.pkg.tar.zst";
    auto share_b_pkg = shared_repo / "share-b-1.0.0-1-x86_64.pkg.tar.zst";
    if (!sage::archive::create_package(share_a, shared_a_data, share_a_pkg)
        || !sage::archive::create_package(share_b, shared_b_data, share_b_pkg)
        || !sage::archive::generate_repo_index(shared_repo, "core")) {
        sage::util::log_error("Failed to create shared directory fixtures");
        return 1;
    }
    auto shared_target = temp_dir / "shared-dir-target";
    if (!write_test_channel(shared_target, shared_repo)) {
        sage::util::log_error("Failed to write shared directory test channel");
        return 1;
    }
    CliOptions shared_install;
    shared_install.target_root = shared_target;
    shared_install.args = {"share-a", "share-b"};
    if (cmd_install(shared_install) != 0
        || !std::filesystem::is_directory(shared_target / "usr/share/common")) {
        sage::util::log_error("Shared empty directory install failed");
        return 1;
    }
    auto shared_db = sage::db::Database::open(
        shared_target / "var/lib/sage/data.mdb", true);
    auto common_owners = shared_db
        ? shared_db->get_path_owners("usr/share/common")
        : std::expected<std::vector<std::string>, std::string>(
            std::unexpected("database open failed"));
    if (!common_owners || common_owners->size() != 2) {
        sage::util::log_error("Shared directory was not registered with both owners");
        return 1;
    }
    CliOptions shared_remove_a;
    shared_remove_a.target_root = shared_target;
    shared_remove_a.args = {"share-a"};
    if (cmd_remove(shared_remove_a) != 0
        || !std::filesystem::is_directory(shared_target / "usr/share/common")) {
        sage::util::log_error("Removing one owner deleted the shared directory");
        return 1;
    }
    CliOptions shared_remove_b;
    shared_remove_b.target_root = shared_target;
    shared_remove_b.args = {"share-b"};
    if (cmd_remove(shared_remove_b) != 0
        || std::filesystem::exists(shared_target / "usr/share/common")) {
        sage::util::log_error("Last owner removal left the shared directory behind");
        return 1;
    }

    // A reinstall that drops directories releases their claims: a shared
    // directory survives with its remaining owner, a sole directory goes away.
    auto dropper_repo = temp_dir / "dropper-repo";
    auto dropper_v1_data = temp_dir / "dropper-v1-data";
    auto dropper_v2_data = temp_dir / "dropper-v2-data";
    auto keeper_data = temp_dir / "keeper-data";
    std::filesystem::create_directories(dropper_repo);
    std::filesystem::create_directories(dropper_v1_data / "usr/bin");
    std::filesystem::create_directories(dropper_v1_data / "usr/share/extra");
    std::filesystem::create_directories(dropper_v1_data / "usr/share/common");
    std::filesystem::create_directories(dropper_v2_data / "usr/bin");
    std::filesystem::create_directories(keeper_data / "usr/bin");
    std::filesystem::create_directories(keeper_data / "usr/share/common");
    std::ofstream(dropper_v1_data / "usr/bin/dtool") << "dropper v1\n";
    std::ofstream(dropper_v2_data / "usr/bin/dtool") << "dropper v2\n";
    std::ofstream(dropper_v2_data / "usr/bin/dtool2") << "dropper v2 extra\n";
    std::ofstream(keeper_data / "usr/bin/ktool") << "keeper\n";
    sage::package::PackageManifest dropper_v1;
    dropper_v1.name = "dropper";
    dropper_v1.version = sage::package::Version::parse("1.0.0-1");
    sage::package::PackageManifest dropper_v2 = dropper_v1;
    dropper_v2.version = sage::package::Version::parse("2.0.0-1");
    sage::package::PackageManifest keeper;
    keeper.name = "keeper";
    keeper.version = sage::package::Version::parse("1.0.0-1");
    auto dropper_v1_pkg = dropper_repo / "dropper-1.0.0-1-x86_64.pkg.tar.zst";
    auto dropper_v2_pkg = dropper_repo / "dropper-2.0.0-1-x86_64.pkg.tar.zst";
    auto keeper_pkg = dropper_repo / "keeper-1.0.0-1-x86_64.pkg.tar.zst";
    if (!sage::archive::create_package(dropper_v1, dropper_v1_data, dropper_v1_pkg)
        || !sage::archive::create_package(dropper_v2, dropper_v2_data, dropper_v2_pkg)
        || !sage::archive::create_package(keeper, keeper_data, keeper_pkg)
        || !sage::archive::generate_repo_index(dropper_repo, "core")) {
        sage::util::log_error("Failed to create dropped-directory fixtures");
        return 1;
    }
    auto dropper_target = temp_dir / "dropper-target";
    if (!write_test_channel(dropper_target, dropper_repo)) {
        sage::util::log_error("Failed to write dropped-directory test channel");
        return 1;
    }
    CliOptions dropper_install;
    dropper_install.target_root = dropper_target;
    dropper_install.args = {dropper_v1_pkg.string()};
    if (cmd_install(dropper_install) != 0) {
        sage::util::log_error("Failed to install dropper version 1");
        return 1;
    }
    dropper_install.args = {"keeper"};
    if (cmd_install(dropper_install) != 0) {
        sage::util::log_error("Failed to install the shared-directory keeper");
        return 1;
    }
    dropper_install.args = {"dropper"};
    if (cmd_install(dropper_install) != 0
        || std::filesystem::exists(dropper_target / "usr/share/extra")
        || !std::filesystem::is_directory(dropper_target / "usr/share/common")
        || read_test_file(dropper_target / "usr/bin/dtool2") != "dropper v2 extra\n") {
        sage::util::log_error("Reinstall did not release dropped directories correctly");
        return 1;
    }
    auto dropper_db = sage::db::Database::open(
        dropper_target / "var/lib/sage/data.mdb", true);
    if (sole_owner(*dropper_db, "usr/share/common") != "keeper:system") {
        sage::util::log_error("Released shared directory kept the stale owner claim");
        return 1;
    }

    // A declared directory that gained foreign content survives a reinstall
    // that no longer ships it; only genuinely empty dropped directories go.
    auto tool_repo = temp_dir / "tool-repo";
    auto tool_v1_data = temp_dir / "tool-v1-data";
    auto tool_v2_data = temp_dir / "tool-v2-data";
    std::filesystem::create_directories(tool_repo);
    std::filesystem::create_directories(tool_v1_data / "usr/bin");
    std::filesystem::create_directories(tool_v1_data / "usr/share/data");
    std::filesystem::create_directories(tool_v2_data / "usr/bin");
    std::ofstream(tool_v1_data / "usr/bin/tool") << "tool v1\n";
    std::ofstream(tool_v2_data / "usr/bin/tool2") << "tool v2\n";
    sage::package::PackageManifest tool_v1;
    tool_v1.name = "dtool";
    tool_v1.version = sage::package::Version::parse("1.0.0-1");
    sage::package::PackageManifest tool_v2 = tool_v1;
    tool_v2.version = sage::package::Version::parse("2.0.0-1");
    auto tool_v1_pkg = tool_repo / "dtool-1.0.0-1-x86_64.pkg.tar.zst";
    auto tool_v2_pkg = tool_repo / "dtool-2.0.0-1-x86_64.pkg.tar.zst";
    if (!sage::archive::create_package(tool_v1, tool_v1_data, tool_v1_pkg)
        || !sage::archive::create_package(tool_v2, tool_v2_data, tool_v2_pkg)
        || !sage::archive::generate_repo_index(tool_repo, "core")) {
        sage::util::log_error("Failed to create declared-directory upgrade fixtures");
        return 1;
    }
    auto tool_target = temp_dir / "tool-target";
    if (!write_test_channel(tool_target, tool_repo)) {
        sage::util::log_error("Failed to write declared-directory test channel");
        return 1;
    }
    CliOptions tool_install;
    tool_install.target_root = tool_target;
    tool_install.args = {tool_v1_pkg.string()};
    if (cmd_install(tool_install) != 0) {
        sage::util::log_error("Failed to install declared-directory fixture version 1");
        return 1;
    }
    std::ofstream(tool_target / "usr/share/data/foreign") << "user data\n";
    tool_install.args = {"dtool"};
    auto tool_record = std::optional<sage::package::PackageManifest>{};
    if (cmd_install(tool_install) == 0) {
        auto tool_db = sage::db::Database::open(
            tool_target / "var/lib/sage/data.mdb", true);
        auto tool_pkg = tool_db ? tool_db->get_package("dtool")
            : std::expected<std::optional<sage::package::PackageManifest>, std::string>(
                std::unexpected("database open failed"));
        tool_record = tool_pkg ? *tool_pkg : std::nullopt;
    }
    if (!tool_record
        || tool_record->version != tool_v2.version
        || std::filesystem::exists(tool_target / "usr/bin/tool")
        || read_test_file(tool_target / "usr/bin/tool2") != "tool v2\n"
        || read_test_file(tool_target / "usr/share/data/foreign") != "user data\n") {
        sage::util::log_error("Reinstall destroyed a declared directory with foreign content");
        return 1;
    }

    sage::util::log_success("9. End-to-End `sage install` & `sage remove` to Target Root OK");

    // 10. Complete Closed-Loop: `sage build` -> `sage repo index` -> `sage install` -> `sage remove` with Orphan Cleanup
    auto build_test_dir = temp_dir / "build_test";
    std::filesystem::create_directories(build_test_dir / "libsample");
    std::filesystem::create_directories(build_test_dir / "sample-app");
    std::filesystem::create_directories(build_test_dir / "repo");
    // Both recipes produce their payload from an `install` phase writing into
    // $DESTDIR. `cmd_build` clears <recipe>/pkg/ before running the phases, so
    // a payload staged there beforehand would be deleted and the package would
    // come out empty; going through the phase is also what exercises the
    // DESTDIR contract these packages are meant to demonstrate.

    // 1. Write libsample recipe
    std::ofstream lib_recipe(build_test_dir / "libsample/recipe.toml");
    lib_recipe << R"(schema_version = 1
[package]
name = "libsample"
version = "2:2.0"
release = "1"
description = "Sample dynamic library"
license = "MIT"
channel = "system"

provides = ["libsample", "so:libsample.so.1"]

install = [
    'mkdir -p "$DESTDIR/usr/share/libsample"',
    'printf "/* libsample fixture */\n" > "$DESTDIR/usr/share/libsample/payload"',
]
)";
    lib_recipe.close();

    // 2. Write sample-app recipe
    std::ofstream app_recipe(build_test_dir / "sample-app/recipe.toml");
    app_recipe << R"(schema_version = 1
[package]
name = "sample-app"
version = "2.0.0"
release = "1"
description = "Sample user application"
license = "GPL-3.0"
channel = "system"

dependencies = ["libsample >= 1.0.0"]

install = [
    'mkdir -p "$DESTDIR/usr/bin"',
    'printf "#!/bin/sh\necho running sample-app\n" > "$DESTDIR/usr/bin/sample-app"',
    'chmod 755 "$DESTDIR/usr/bin/sample-app"',
]
)";
    app_recipe.close();

    // 3. Execute `sage build` on both packages
    auto recipedia = build_test_dir / "recipedia";
    auto recipedia_root = build_test_dir / "builder-root";
    std::filesystem::create_directories(recipedia);
    std::filesystem::create_directories(recipedia_root / "etc/sage");
    auto publish_index = [&](std::string_view packages) {
        std::ofstream index(recipedia / "index.toml");
        index << "schema_version = 1\n[channel]\nname = \"core\"\n" << packages;
        return index.good();
    };
    std::ofstream(recipedia_root / "etc/sage/channels.toml")
        << "schema_version = 1\n[[channels]]\nname = \"core\"\nurl = \"file://"
        << recipedia.string() << "\"\nenabled = true\n";
    if (!publish_index(R"(
[[packages]]
name = "libsample"
version = "2.0"
release = "20"
epoch = 2
channel = "toolchain/foo"
)")) {
        sage::util::log_error("Failed to create cross-channel Recipedia fixture");
        return 1;
    }
    CliOptions build_lib_opts;
    build_lib_opts.args = {(build_test_dir / "libsample").string()};
    build_lib_opts.target_root = recipedia_root;
    if (cmd_build(build_lib_opts) != 0) {
        sage::util::log_error("Failed to build libsample");
        return 1;
    }
    // A package published in another package channel is not this identity,
    // and the local artifact is not a publication. Rebuilding must therefore
    // retain the recipe's release and may replace the local archive.
    if (cmd_build(build_lib_opts) != 0) {
        sage::util::log_error("Failed to rebuild unpublished local libsample");
        return 1;
    }
    auto local_rebuild = sage::archive::inspect_package(
        build_test_dir / "libsample/libsample-2.0-1-x86_64.pkg.tar.zst");
    if (!local_rebuild || local_rebuild->manifest.version.rel != "1") {
        sage::util::log_error("Local or cross-channel publication incorrectly advanced the system release");
        return 1;
    }
    if (!publish_index(R"(
[[packages]]
name = "libsample"
version = "2.0"
release = "1"
epoch = 2
channel = "system"
[[packages]]
name = "libsample"
version = "2.0"
release = "3"
epoch = 2
channel = "system"
[[packages]]
name = "libsample"
version = "2.0"
release = "2"
epoch = 2
channel = "system"
[[packages]]
name = "libsample"
version = "2.0"
release = "99"
epoch = 1
channel = "system"
)")) {
        sage::util::log_error("Failed to publish Recipedia release fixtures");
        return 1;
    }
    if (cmd_build(build_lib_opts) != 0) {
        sage::util::log_error("Failed to rebuild published libsample");
        return 1;
    }
    auto rebuilt_lib = sage::archive::inspect_package(
        build_test_dir / "libsample/libsample-2.0-4-x86_64.pkg.tar.zst");
    if (!rebuilt_lib || rebuilt_lib->manifest.version.rel != "4") {
        sage::util::log_error("Rebuild did not advance beyond the highest published release");
        return 1;
    }

    // A packager may intentionally skip releases. Published 2:2.0-1..3 must
    // not pull an explicitly declared 2:2.0-10 backwards to release 4.
    {
        std::ifstream recipe_in(build_test_dir / "libsample/recipe.toml");
        std::stringstream recipe_text;
        recipe_text << recipe_in.rdbuf();
        auto updated = recipe_text.str();
        auto release = updated.find("release = \"1\"");
        if (release == std::string::npos) {
            sage::util::log_error("Failed to locate declared release in libsample fixture");
            return 1;
        }
        updated.replace(release, std::string_view("release = \"1\"").size(), "release = \"10\"");
        std::ofstream(build_test_dir / "libsample/recipe.toml") << updated;
    }
    if (cmd_build(build_lib_opts) != 0) {
        sage::util::log_error("Failed to build explicitly higher libsample release");
        return 1;
    }
    auto declared_lib = sage::archive::inspect_package(
        build_test_dir / "libsample/libsample-2.0-10-x86_64.pkg.tar.zst");
    if (!declared_lib || declared_lib->manifest.version.rel != "10") {
        sage::util::log_error("Published release lookup regressed an explicitly higher recipe release");
        return 1;
    }
    if (!publish_index(R"(
[[packages]]
name = "libsample"
version = "2.0"
release = "18446744073709551615"
epoch = 2
channel = "system"
)")) {
        sage::util::log_error("Failed to publish exhausted release fixture");
        return 1;
    }
    if (cmd_build(build_lib_opts) == 0) {
        sage::util::log_error("UINT64_MAX published release wrapped or reused a package identity");
        return 1;
    }

    CliOptions build_app_opts;
    build_app_opts.args = {(build_test_dir / "sample-app").string()};
    build_app_opts.target_root = recipedia_root;
    if (cmd_build(build_app_opts) != 0) {
        sage::util::log_error("Failed to build sample-app");
        return 1;
    }

    // Move built packages to repo/. The names carry the arch suffix that
    // `cmd_build` emits; the recipes above declare no `arch`, so both land on
    // the PackageManifest default.
    auto stage_package = [&](const std::filesystem::path& built,
                             const std::filesystem::path& into) -> bool {
        std::error_code ec;
        std::filesystem::copy_file(built, into, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            sage::util::log_error("Failed to stage {} into the test repo: {}",
                built.filename().string(), ec.message());
            return false;
        }
        return true;
    };

    if (!stage_package(build_test_dir / "libsample/libsample-2.0-1-x86_64.pkg.tar.zst",
                       build_test_dir / "repo/libsample-2.0-1-x86_64.pkg.tar.zst")) {
        return 1;
    }
    if (!stage_package(build_test_dir / "sample-app/sample-app-2.0.0-1-x86_64.pkg.tar.zst",
                       build_test_dir / "repo/sample-app-2.0.0-1-x86_64.pkg.tar.zst")) {
        return 1;
    }

    // 4. Generate local repo index
    auto build_idx_res = sage::archive::generate_repo_index(build_test_dir / "repo", "core");
    if (!build_idx_res) {
        sage::util::log_error("Failed to generate index for built packages");
        return 1;
    }

    // 5. Point isolated target to the newly built repo
    auto loop_target = build_test_dir / "target_root";
    std::filesystem::create_directories(loop_target / "etc/sage");
    std::ofstream loop_chan(loop_target / "etc/sage/channels.toml");
    loop_chan << "schema_version = 1\n\n[[channels]]\nname = \"core\"\nurl = \"file://" << (build_test_dir / "repo").string() << "\"\nscope = \"system\"\npriority = 100\nenabled = true\n";
    loop_chan.close();

    // 6. Install sample-app (which requires libsample)
    CliOptions loop_inst_opts;
    loop_inst_opts.target_root = loop_target;
    loop_inst_opts.args = {"sample-app"};
    if (cmd_install(loop_inst_opts, "/") != 0) {
        sage::util::log_error("Failed to install built sample-app");
        return 1;
    }

    // Verify files on disk and packages in LMDB
    if (!std::filesystem::exists(loop_target / "usr/bin/sample-app") || 
        !std::filesystem::exists(loop_target / "usr/share/libsample/payload")) {
        sage::util::log_error("Files from sample-app and libsample were not properly installed to target root");
        return 1;
    }

    // 7. Verify Reverse Dependency Protection: Attempting to remove libsample directly while sample-app depends on it must fail!
    CliOptions direct_lib_rem;
    direct_lib_rem.target_root = loop_target;
    direct_lib_rem.args = {"libsample"};
    if (cmd_remove(direct_lib_rem) == 0) {
        sage::util::log_error("Direct removal of libsample should have been blocked by reverse dependency protection!");
        return 1;
    }

    // 8. Remove with --cascade: should remove both libsample and sample-app!
    direct_lib_rem.cascade = true;
    if (cmd_remove(direct_lib_rem) != 0) {
        sage::util::log_error("Failed to cascaded-remove libsample and sample-app");
        return 1;
    }

    // Verify all files from both sample-app and libsample are gone from disk!
    if (std::filesystem::exists(loop_target / "usr/bin/sample-app") ||
        std::filesystem::exists(loop_target / "usr/share/libsample/payload")) {
        sage::util::log_error("Files still exist on disk after cascade removal");
        return 1;
    }

    // Verify LMDB is clean
    auto loop_db = sage::db::Database::open(loop_target / "var/lib/sage/data.mdb");
    if (!loop_db) {
        sage::util::log_error("LMDB unavailable after cascaded removal");
        return 1;
    }
    auto loop_packages = loop_db->list_installed_packages();
    if (!loop_packages || !loop_packages->empty()) {
        sage::util::log_error("LMDB still contains packages after cascaded removal");
        return 1;
    }

    sage::util::log_success("10. Complete Build -> Index -> Install -> Remove (Auto Orphan Cleanup) Closed-Loop OK");
    sage::util::log_success("11. Reverse Dependency Protection & Cascade Removal Safety Locks OK");

    // 12. Host operation lock and zero-write dry-run protocol.
    {
        // Lock provisioning chowns state to root, and the install/remove
        // dry-run protocol gates on root as well. CI covers this scenario
        // fully as root; local non-root runs skip rather than fail spuriously.
        if (sage::util::current_effective_uid() != 0) {
            sage::util::log_warn(
                "12. Global Operation Lock & Zero-Write Dry-Run Protocol SKIPPED (requires root)");
            goto scenario_12_done;
        }
        const auto operation_lock_root = temp_dir / "operation-lock-host";
        const auto operation_lock_path = operation_lock_root / "sage/operation.lock";
        std::filesystem::create_directory(operation_lock_root);
        std::filesystem::permissions(
            operation_lock_root,
            std::filesystem::perms::owner_all
                | std::filesystem::perms::group_read
                | std::filesystem::perms::group_exec
                | std::filesystem::perms::others_read
                | std::filesystem::perms::others_exec,
            std::filesystem::perm_options::replace);

        auto non_root = validate_operation_user(1000);
        if (non_root || !validate_operation_user(0)) {
            sage::util::log_error("Package-state operation root requirement failed");
            return 1;
        }

        // Shared previews coexist, but their root-only file lock excludes the first
        // writer just as it excludes an established writer.
        {
            auto shared_a = sage::util::OperationLock::acquire(
                operation_lock_path, sage::util::LockMode::Shared);
            auto shared_b = sage::util::OperationLock::acquire(
                operation_lock_path, sage::util::LockMode::Shared);
            auto blocked_writer = sage::util::OperationLock::acquire(
                operation_lock_path, sage::util::LockMode::Exclusive);
            if (!shared_a || !shared_b || blocked_writer
                || blocked_writer.error().kind != sage::util::LockFailure::Busy) {
                sage::util::log_error("SH/SH coexistence or SH-to-EX exclusion failed");
                return 1;
            }
        }

        const auto public_metadata = sage::util::snapshot_file_metadata(
            operation_lock_root);
        const auto namespace_metadata = sage::util::snapshot_file_metadata(
            operation_lock_path.parent_path());
        const auto lock_metadata = sage::util::snapshot_file_metadata(operation_lock_path);
        if (!public_metadata || !namespace_metadata || !lock_metadata
            || !std::filesystem::is_directory(operation_lock_root)
            || !std::filesystem::is_directory(operation_lock_path.parent_path())
            || !std::filesystem::is_regular_file(operation_lock_path)
            || public_metadata->owner_uid != 0
            || public_metadata->owner_gid != 0
            || public_metadata->mode != 0755
            || namespace_metadata->owner_uid != 0
            || namespace_metadata->owner_gid != 0
            || namespace_metadata->mode != 0700
            || lock_metadata->owner_uid != 0
            || lock_metadata->owner_gid != 0
            || lock_metadata->mode != 0600) {
            sage::util::log_error("Operation lock namespace is not securely provisioned");
            return 1;
        }

        // An exclusive writer excludes both modes, then release of its fd makes
        // the same inode immediately available again.
        {
            auto exclusive = sage::util::OperationLock::acquire(
                operation_lock_path, sage::util::LockMode::Exclusive);
            auto blocked_reader = sage::util::OperationLock::acquire(
                operation_lock_path, sage::util::LockMode::Shared);
            auto blocked_writer = sage::util::OperationLock::acquire(
                operation_lock_path, sage::util::LockMode::Exclusive);
            if (!exclusive || blocked_reader || blocked_writer
                || blocked_reader.error().kind != sage::util::LockFailure::Busy
                || blocked_writer.error().kind != sage::util::LockFailure::Busy) {
                sage::util::log_error("EX did not exclude SH and EX contenders");
                return 1;
            }
        }
        if (!sage::util::OperationLock::acquire(
                operation_lock_path, sage::util::LockMode::Shared)) {
            sage::util::log_error("Operation lock was not reacquirable after release");
            return 1;
        }

        // The caller supplies one absolute deadline. Repeated EWOULDBLOCK
        // results consume it instead of starting a fresh wait on each retry.
        {
            auto exclusive = sage::util::OperationLock::acquire(
                operation_lock_path, sage::util::LockMode::Exclusive);
            const auto started = std::chrono::steady_clock::now();
            const auto deadline = started + std::chrono::milliseconds(150);
            auto waited = sage::util::OperationLock::acquire_until(
                operation_lock_path, sage::util::LockMode::Shared, deadline);
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started);
            if (!exclusive || waited
                || waited.error().kind != sage::util::LockFailure::Busy
                || elapsed < std::chrono::milliseconds(100)
                || elapsed > std::chrono::milliseconds(500)) {
                sage::util::log_error("Operation lock wait did not honor its absolute deadline");
                return 1;
            }
        }

        // Fatal open/flock failures keep their real classification and reason.
        auto missing_lock = sage::util::OperationLock::acquire(
            temp_dir / "no-such-run/sage/operation.lock",
            sage::util::LockMode::Shared);
        const auto wrong_lock_root = temp_dir / "wrong-operation-lock-host";
        const auto wrong_lock_file = wrong_lock_root / "operation.lock";
        std::filesystem::create_directory(wrong_lock_root);
        std::filesystem::permissions(
            wrong_lock_root, std::filesystem::perms::owner_all,
            std::filesystem::perm_options::replace);
        std::filesystem::create_directory(wrong_lock_file);
        auto wrong_lock = sage::util::OperationLock::acquire(
            wrong_lock_file, sage::util::LockMode::Shared);
        auto fatal_flock_call = +[](int, int) -> std::expected<void, int> {
            return std::unexpected(static_cast<int>(std::errc::io_error));
        };
        auto fatal_flock = sage::util::OperationLock::acquire_until(
            operation_lock_path,
            sage::util::LockMode::Shared,
            sage::util::OperationLock::deadline_after(0),
            fatal_flock_call);
        if (missing_lock || wrong_lock || fatal_flock
            || missing_lock.error().kind != sage::util::LockFailure::Unusable
            || missing_lock.error().message.find("No such file or directory")
                == std::string::npos
            || wrong_lock.error().kind != sage::util::LockFailure::Unusable
            || fatal_flock.error().kind != sage::util::LockFailure::Unusable
            || fatal_flock.error().message.find("Input/output") == std::string::npos) {
            sage::util::log_error("Fatal operation-lock errors were reported as contention");
            return 1;
        }

        const auto loose_namespace_host = temp_dir / "loose-namespace-host";
        const auto loose_namespace = loose_namespace_host / "sage";
        std::filesystem::create_directory(loose_namespace_host);
        std::filesystem::create_directory(loose_namespace);
        std::filesystem::permissions(
            loose_namespace,
            std::filesystem::perms::owner_all
                | std::filesystem::perms::group_read
                | std::filesystem::perms::group_exec
                | std::filesystem::perms::others_read
                | std::filesystem::perms::others_exec,
            std::filesystem::perm_options::replace);
        auto loose_namespace_lock = sage::util::OperationLock::acquire(
            loose_namespace / "operation.lock", sage::util::LockMode::Shared);
        if (loose_namespace_lock
            || loose_namespace_lock.error().kind != sage::util::LockFailure::Unusable
            || loose_namespace_lock.error().message.find("mode 0700 directory")
                == std::string::npos) {
            sage::util::log_error("Insecure operation lock namespace was accepted");
            return 1;
        }

        const auto loose_lock_root = temp_dir / "loose-operation-lock-host";
        const auto loose_lock_file = loose_lock_root / "operation.lock";
        std::filesystem::create_directory(loose_lock_root);
        std::filesystem::permissions(
            loose_lock_root, std::filesystem::perms::owner_all,
            std::filesystem::perm_options::replace);
        std::ofstream(loose_lock_file) << "insecure\n";
        std::filesystem::permissions(
            loose_lock_file,
            std::filesystem::perms::owner_read
                | std::filesystem::perms::owner_write
                | std::filesystem::perms::group_read
                | std::filesystem::perms::others_read,
            std::filesystem::perm_options::replace);
        if (sage::util::OperationLock::acquire(
                loose_lock_file, sage::util::LockMode::Shared)) {
            sage::util::log_error("Insecure operation lock mode was accepted");
            return 1;
        }

        // Probe classification: only ENOENT is absent; permissions and wrong
        // entry types remain errors. symlink_status also rejects redirected
        // target roots and data.mdb entries.
        auto absent_probe = classify_path_probe(
            "missing", {}, std::make_error_code(std::errc::no_such_file_or_directory),
            std::filesystem::file_type::regular, "fixture");
        auto permission_probe = classify_path_probe(
            "denied", {}, std::make_error_code(std::errc::permission_denied),
            std::filesystem::file_type::regular, "fixture");
        auto wrong_type_probe = classify_path_probe(
            "directory", std::filesystem::file_status{std::filesystem::file_type::directory}, {},
            std::filesystem::file_type::regular, "fixture");
        if (!absent_probe || *absent_probe || permission_probe || wrong_type_probe
            || permission_probe.error().find("Permission denied") == std::string::npos
            || wrong_type_probe.error().find("wrong file type") == std::string::npos) {
            sage::util::log_error("Database probe error classification failed");
            return 1;
        }

        // A configured *.mdb name denotes an LMDB environment directory; both
        // Database::open and the synchronized probe must resolve its real
        // data.mdb file through the same helper.
        {
            const auto configured_db_path = temp_dir / "custom-lmdb/state.mdb";
            const auto resolved_db_paths = sage::db::resolve_lmdb_paths(configured_db_path);
            auto custom_db = sage::db::Database::open(configured_db_path);
            auto custom_probe = probe_package_database(configured_db_path);
            if (!custom_db || !custom_probe || !*custom_probe
                || resolved_db_paths.environment != configured_db_path.parent_path()
                || resolved_db_paths.data_file != configured_db_path.parent_path() / "data.mdb"
                || resolved_db_paths.lock_file != configured_db_path.parent_path() / "lock.mdb"
                || std::filesystem::exists(configured_db_path)
                || !std::filesystem::is_regular_file(resolved_db_paths.data_file)
                || !std::filesystem::is_regular_file(resolved_db_paths.lock_file)) {
                sage::util::log_error("Custom LMDB path probe/open resolution diverged");
                return 1;
            }
        }

        // An absent target can be previewed without creating the root. The held
        // SH lock also excludes the first real writer.
        const auto absent_root = temp_dir / "dry-run-absent-root";
        {
            CliOptions absent_opts;
            absent_opts.target_root = absent_root;
            absent_opts.args = {"dummy-tool"};
            absent_opts.dry_run = true;
            auto context = acquire_operation_context(absent_opts, operation_lock_path);
            auto first_writer = sage::util::OperationLock::acquire(
                operation_lock_path, sage::util::LockMode::Exclusive);
            if (!context
                || context->target_root_snapshot != TargetRootSnapshot::Absent
                || context->database_snapshot != DatabaseSnapshot::Absent
                || first_writer
                || first_writer.error().kind != sage::util::LockFailure::Busy
                || cmd_remove(absent_opts, context->database_snapshot) != 0
                || cmd_rebuild(absent_opts, context->database_snapshot) == 0
                || std::filesystem::exists(absent_root)) {
                sage::util::log_error("Absent-root dry-run created state or admitted a writer");
                return 1;
            }
        }
        if (std::filesystem::exists(absent_root)) {
            sage::util::log_error("Absent-root preview persisted a target path");
            return 1;
        }

        // A present root with no DB uses an empty installed set. Install parses
        // the channel in memory, remove is a no-op, and rebuild is explicit.
        const auto empty_root = temp_dir / "dry-run-empty-db-root";
        if (!write_test_channel(empty_root, local_repo)) {
            sage::util::log_error("Failed to create empty-DB dry-run fixture");
            return 1;
        }
        {
            CliOptions dry_install;
            dry_install.target_root = empty_root;
            dry_install.args = {"dummy-tool"};
            dry_install.dry_run = true;
            auto context = acquire_operation_context(dry_install, operation_lock_path);
            if (!context
                || context->target_root_snapshot != TargetRootSnapshot::Present
                || context->database_snapshot != DatabaseSnapshot::Absent
                || cmd_install(dry_install, std::nullopt, context->database_snapshot) != 0
                || cmd_remove(dry_install, context->database_snapshot) != 0
                || cmd_rebuild(dry_install, context->database_snapshot) == 0) {
                sage::util::log_error("Absent-database dry-run semantics failed");
                return 1;
            }
        }
        if (std::filesystem::exists(empty_root / "var/lib/sage")
            || std::filesystem::exists(empty_root / "var/cache/sage")
            || std::filesystem::exists(empty_root / "usr/bin/dummy")) {
            sage::util::log_error("Empty-database dry-run persisted target state");
            return 1;
        }

        // The operation context is a frozen snapshot. Even if a test bypasses
        // the protocol and creates a directory-shaped data.mdb afterwards, an
        // Absent command path neither re-probes nor opens it.
        const auto frozen_root = temp_dir / "frozen-absent-snapshot-root";
        if (!write_test_channel(frozen_root, local_repo)) return 1;
        {
            CliOptions frozen;
            frozen.target_root = frozen_root;
            frozen.args = {"dummy-tool"};
            frozen.dry_run = true;
            auto context = acquire_operation_context(frozen, operation_lock_path);
            if (!context || context->database_snapshot != DatabaseSnapshot::Absent) return 1;
            std::filesystem::create_directories(frozen_root / "var/lib/sage/data.mdb");
            if (cmd_install(frozen, std::nullopt, context->database_snapshot) != 0
                || cmd_remove(frozen, context->database_snapshot) != 0) {
                sage::util::log_error("Commands re-probed a frozen Absent database snapshot");
                return 1;
            }
        }

        // Wrong target/data types and ENOTDIR are never treated as an empty DB.
        const auto wrong_db_root = temp_dir / "wrong-db-type-root";
        std::filesystem::create_directories(wrong_db_root / "var/lib/sage/data.mdb");
        CliOptions wrong_db;
        wrong_db.target_root = wrong_db_root;
        wrong_db.dry_run = true;
        if (acquire_operation_context(wrong_db, operation_lock_path)) {
            sage::util::log_error("Directory-shaped data.mdb was treated as absent");
            return 1;
        }
        const auto symlink_db_root = temp_dir / "symlink-db-root";
        std::filesystem::create_directories(symlink_db_root / "var/lib/sage");
        std::ofstream(temp_dir / "regular-db-decoy") << "not LMDB\n";
        std::filesystem::create_symlink(
            temp_dir / "regular-db-decoy", symlink_db_root / "var/lib/sage/data.mdb");
        wrong_db.target_root = symlink_db_root;
        if (acquire_operation_context(wrong_db, operation_lock_path)) {
            sage::util::log_error("Symlink data.mdb was accepted by the synchronized probe");
            return 1;
        }
        const auto enotdir_root = temp_dir / "enotdir-root";
        std::filesystem::create_directory(enotdir_root);
        std::ofstream(enotdir_root / "var") << "not a directory\n";
        wrong_db.target_root = enotdir_root;
        if (acquire_operation_context(wrong_db, operation_lock_path)) {
            sage::util::log_error("ENOTDIR database path was treated as absent");
            return 1;
        }
        const auto symlink_root = temp_dir / "symlink-root";
        std::filesystem::create_directory(temp_dir / "real-root");
        std::filesystem::create_directory_symlink(temp_dir / "real-root", symlink_root);
        wrong_db.target_root = symlink_root;
        if (acquire_operation_context(wrong_db, operation_lock_path)) {
            sage::util::log_error("Symlink target root was accepted by the synchronized probe");
            return 1;
        }

        // Seed a normal DB and package, then prove every relevant byte and
        // size/mtime/ctime remains stable across all three previews. atime is
        // intentionally excluded because reads may update it.
        if (cmd_install(inst_opts) != 0) {
            sage::util::log_error("Failed to seed present-DB dry-run fixture");
            return 1;
        }
        {
            auto fixture_cfg = sage::config::SystemConfig::load_from_root(isolated_target);
            auto fixture_db = sage::db::Database::open(
                isolated_target / "var/lib/sage/data.mdb");
            if (!fixture_cfg || !fixture_db) return 1;
            auto txn = fixture_db->begin_write_txn();
            if (!txn) return 1;
            for (const auto& [iface, provider] : fixture_cfg->exclusive_providers()) {
                auto stored = fixture_db->set_system_provider(*txn, iface, provider);
                if (!stored) return 1;
            }
            if (!txn->commit()) return 1;
        }
        const auto legacy_pid_lock = isolated_target / "var/lib/sage/lock";
        std::ofstream(legacy_pid_lock) << "legacy-pid-sentinel\n";
        const std::vector<std::filesystem::path> watched_files{
            isolated_target / "var/lib/sage/data.mdb",
            isolated_target / "var/lib/sage/lock.mdb",
            legacy_pid_lock,
            isolated_target / "var/cache/sage/channels/core.toml",
            isolated_target / "usr/bin/dummy",
        };
        std::map<std::filesystem::path,
            std::pair<std::string, sage::util::FileMetadataSnapshot>> before;
        for (const auto& path : watched_files) {
            auto metadata = sage::util::snapshot_file_metadata(path);
            if (!metadata) {
                sage::util::log_error("Missing present-DB dry-run fixture '{}': {}",
                    path.string(), metadata.error());
                return 1;
            }
            before.emplace(path, std::pair{read_test_file(path), *metadata});
        }
        {
            CliOptions dry_install = inst_opts;
            dry_install.dry_run = true;
            auto context = acquire_operation_context(dry_install, operation_lock_path);
            if (!context || context->database_snapshot != DatabaseSnapshot::Present
                || cmd_install(dry_install, std::nullopt, context->database_snapshot) != 0) {
                sage::util::log_error("Present-DB install preview failed");
                return 1;
            }
            CliOptions dry_remove = rem_opts;
            dry_remove.dry_run = true;
            if (cmd_remove(dry_remove, context->database_snapshot) != 0) {
                sage::util::log_error("Present-DB remove preview failed");
                return 1;
            }
            CliOptions dry_rebuild;
            dry_rebuild.target_root = isolated_target;
            dry_rebuild.dry_run = true;
            if (cmd_rebuild(dry_rebuild, context->database_snapshot) != 0) {
                sage::util::log_error("Present-DB rebuild preview failed");
                return 1;
            }
        }
        for (const auto& [path, expected] : before) {
            auto metadata = sage::util::snapshot_file_metadata(path);
            if (!metadata || *metadata != expected.second
                || read_test_file(path) != expected.first) {
                sage::util::log_error("Dry-run modified content or metadata for '{}'", path.string());
                return 1;
            }
        }

        // A real EX-held operation still initializes a fresh root, and another
        // EX-held operation mutates the established state normally.
        const auto fresh_root = temp_dir / "fresh-operation-root";
        CliOptions fresh_install;
        fresh_install.target_root = fresh_root;
        fresh_install.args = {"dummy-tool"};
        {
            auto context = acquire_operation_context(fresh_install, operation_lock_path);
            if (!context
                || context->target_root_snapshot != TargetRootSnapshot::Absent
                || context->database_snapshot != DatabaseSnapshot::Absent
                || !write_test_channel(fresh_root, local_repo)
                || cmd_install(fresh_install, std::nullopt, context->database_snapshot) != 0) {
                sage::util::log_error("Real operation failed to initialize a fresh target root");
                return 1;
            }
        }
        if (!std::filesystem::is_regular_file(fresh_root / "var/lib/sage/data.mdb")
            || !std::filesystem::exists(fresh_root / "usr/bin/dummy")
            || std::filesystem::exists(fresh_root / "var/lib/sage/lock")) {
            sage::util::log_error("Fresh mutation did not create the expected package state");
            return 1;
        }
        CliOptions fresh_remove;
        fresh_remove.target_root = fresh_root;
        fresh_remove.args = {"dummy-tool"};
        {
            auto context = acquire_operation_context(fresh_remove, operation_lock_path);
            if (!context || context->database_snapshot != DatabaseSnapshot::Present
                || cmd_remove(fresh_remove, context->database_snapshot) != 0) {
                sage::util::log_error("Real operation failed to mutate established state");
                return 1;
            }
        }
        if (std::filesystem::exists(fresh_root / "usr/bin/dummy")) {
            sage::util::log_error("Real remove left the installed package file behind");
            return 1;
        }
    }
scenario_12_done:
    sage::util::log_success("12. Global Operation Lock & Zero-Write Dry-Run Protocol OK");

    // 13. Build configuration: global flag injection, the per-recipe [build]
    // override (which doubles as the fourth phase-command scope), and the
    // compiler fallback -- the provenance Recipedia reads from the manifest
    // and the repository index.
    {
        // Pure-model defaults: an empty document yields the built-in baseline,
        // and the shipped /etc/sage/build.toml must keep parsing to exactly
        // that -- the two are kept in lockstep by this assertion.
        auto empty_cfg = sage::config::BuildConfig::parse_toml("");
        auto shipped_cfg = sage::config::BuildConfig::parse_toml(R"(schema_version = 1
cc = "clang"
cxx = "clang++"
fallback_cc = "gcc"
fallback_cxx = "g++"
cflags = "-O3 -march=x86-64-v3"
)");
        if (!empty_cfg || !shipped_cfg || *shipped_cfg != *empty_cfg
            || empty_cfg->cc != "clang" || empty_cfg->cflags != "-O3 -march=x86-64-v3"
            || !empty_cfg->cxxflags.empty()) {
            sage::util::log_error("BuildConfig defaults or shipped-default parse drifted");
            return 1;
        }
        // Explicit job count parses through; absence stays auto (0 = nproc).
        auto jobs_cfg = sage::config::BuildConfig::parse_toml("jobs = 4\n");
        if (!jobs_cfg || jobs_cfg->jobs != 4 || empty_cfg->jobs != 0) {
            sage::util::log_error("BuildConfig jobs parsing drifted");
            return 1;
        }

        auto write_build_toml = [&](const std::filesystem::path& root, std::string_view body) {
            std::filesystem::create_directories(root / "etc/sage");
            std::ofstream(root / "etc/sage/system.toml") << "schema_version = 1\n";
            std::ofstream f(root / "etc/sage/build.toml");
            f << body;
            return f.good();
        };
        auto write_canary_recipe = [&](const std::filesystem::path& dir, std::string_view toml_body) {
            std::filesystem::create_directories(dir);
            std::ofstream recipe(dir / "recipe.toml");
            recipe << toml_body;
            return recipe.good();
        };
        auto build_with_root = [&](const std::filesystem::path& recipe_dir,
                                   const std::filesystem::path& target_root,
                                   const std::filesystem::path& extract_dir,
                                   std::string_view pkg_filename)
            -> std::expected<sage::package::PackageManifest, std::string> {
            CliOptions build_opts;
            build_opts.args = {recipe_dir.string()};
            build_opts.target_root = target_root;
            if (cmd_build(build_opts) != 0) {
                return std::unexpected("cmd_build failed for " + recipe_dir.string());
            }
            auto extracted = sage::archive::extract_package(recipe_dir / pkg_filename, extract_dir);
            if (!extracted) return std::unexpected(extracted.error());
            return std::move(extracted->manifest);
        };
        auto read_text = [](const std::filesystem::path& p) {
            std::ifstream f(p);
            std::stringstream ss;
            ss << f.rdbuf();
            return ss.str();
        };

        // (a) Global baseline reaches the recipe shell and is stamped.
        auto canary_root = temp_dir / "bcfg-target-a";
        auto canary_dir = temp_dir / "bcfg-canary";
        if (!write_build_toml(canary_root, R"(cc = "cc"
cxx = "c++"
fallback_cc = "cc"
fallback_cxx = "c++"
cflags = "-DGLOBAL_CFLAG=1"
)")
            || !write_canary_recipe(canary_dir, R"(schema_version = 1
[package]
name = "flagcanary"
version = "1.0.0"
release = "1"
description = "build-config canary"
license = "MIT"
channel = "system"
install = [
    'mkdir -p "$DESTDIR/usr/share"',
    'printf "int sage_canary;\n" > canary.c',
    '$CC -c canary.c -o "$DESTDIR/usr/share/canary.o"',
    'printf "%s" "$CFLAGS" > "$DESTDIR/usr/share/cflags.txt"',
]
)")) {
            sage::util::log_error("Failed to create build-config canary fixtures");
            return 1;
        }
        auto canary = build_with_root(canary_dir, canary_root, temp_dir / "bcfg-canary-x",
                                      "flagcanary-1.0.0-1-x86_64.pkg.tar.zst");
        // The compiled object carries its producer's fingerprint, so the
        // stamp names the real compiler family rather than the injected CC.
        if (!canary || canary->build_compiler.empty() || canary->build_compiler_version.empty()
            || canary->build_cflags != "-DGLOBAL_CFLAG=1"
            || canary->build_cxxflags != "-DGLOBAL_CFLAG=1"  // cxxflags mirror cflags
            || read_text(temp_dir / "bcfg-canary-x/usr/share/cflags.txt") != "-DGLOBAL_CFLAG=1"
            || read_text(temp_dir / "bcfg-canary-x/usr/share/canary.o").empty()) {
            sage::util::log_error("Global build-config injection or provenance stamping failed");
            return 1;
        }

        // (b) The recipe's [build] table replaces the baseline and carries the
        // phase commands themselves.
        auto override_root = temp_dir / "bcfg-target-b";
        auto override_dir = temp_dir / "bcfg-override";
        if (!write_build_toml(override_root, R"(cc = "cc"
fallback_cc = "cc"
cflags = "-DGLOBAL_CFLAG=1"
)")
            || !write_canary_recipe(override_dir, R"(schema_version = 1
[package]
name = "flagoverride"
version = "1.0.0"
release = "1"
description = "build-config override canary"
license = "MIT"
channel = "system"

[build]
cflags = "-DLOCAL_CFLAG=2"
install = [
    'mkdir -p "$DESTDIR/usr/share"',
    'printf "int sage_canary;\n" > canary.c',
    '$CC -c canary.c -o "$DESTDIR/usr/share/canary.o"',
    'printf "%s" "$CFLAGS" > "$DESTDIR/usr/share/cflags.txt"',
]
)")) {
            sage::util::log_error("Failed to create build-config override fixtures");
            return 1;
        }
        auto overridden = build_with_root(override_dir, override_root, temp_dir / "bcfg-override-x",
                                          "flagoverride-1.0.0-1-x86_64.pkg.tar.zst");
        if (!overridden || overridden->build_compiler.empty()
            || overridden->build_cflags != "-DLOCAL_CFLAG=2"
            || overridden->build_cxxflags != "-DLOCAL_CFLAG=2"  // mirrors the recipe cflags
            || read_text(temp_dir / "bcfg-override-x/usr/share/cflags.txt") != "-DLOCAL_CFLAG=2") {
            sage::util::log_error("Per-recipe [build] override did not replace the global baseline");
            return 1;
        }

        // (c) An unusable primary compiler degrades to the fallback pair.
        auto fallback_root = temp_dir / "bcfg-target-c";
        auto fallback_dir = temp_dir / "bcfg-fallback";
        if (!write_build_toml(fallback_root, R"(cc = "/nonexistent/sage-no-such-cc"
fallback_cc = "cc"
fallback_cxx = "c++"
cflags = "-DFALLBACK_CFLAG=3"
)")
            || !write_canary_recipe(fallback_dir, R"(schema_version = 1
[package]
name = "flagfallback"
version = "1.0.0"
release = "1"
description = "build-config fallback canary"
license = "MIT"
channel = "system"
install = [
    'mkdir -p "$DESTDIR/usr/share"',
    'printf "int sage_canary;\n" > canary.c',
    '$CC -c canary.c -o "$DESTDIR/usr/share/canary.o"',
    'printf "%s" "$CFLAGS" > "$DESTDIR/usr/share/cflags.txt"',
]
)")) {
            sage::util::log_error("Failed to create build-config fallback fixtures");
            return 1;
        }
        auto fallback = build_with_root(fallback_dir, fallback_root, temp_dir / "bcfg-fallback-x",
                                        "flagfallback-1.0.0-1-x86_64.pkg.tar.zst");
        if (!fallback || fallback->build_compiler.empty()
            || fallback->build_cflags != "-DFALLBACK_CFLAG=3"
            || read_text(temp_dir / "bcfg-fallback-x/usr/share/cflags.txt") != "-DFALLBACK_CFLAG=3") {
            sage::util::log_error("Compiler fallback did not degrade to the configured fallback pair");
            return 1;
        }

        // (d) A package that never compiled anything claims no provenance:
        // os-release-style recipes stay silent about compilers and flags.
        auto plain_dir = temp_dir / "bcfg-plain";
        if (!write_canary_recipe(plain_dir, R"(schema_version = 1
[package]
name = "notcanary"
version = "1.0.0"
release = "1"
description = "build-config silence canary"
license = "MIT"
channel = "system"
install = [
    'mkdir -p "$DESTDIR/usr/share"',
    'printf "%s" "$CFLAGS" > "$DESTDIR/usr/share/cflags.txt"',
]
)")) {
            sage::util::log_error("Failed to create provenance-silence fixture");
            return 1;
        }
        auto plain = build_with_root(plain_dir, canary_root, temp_dir / "bcfg-plain-x",
                                     "notcanary-1.0.0-1-x86_64.pkg.tar.zst");
        if (!plain || !plain->build_compiler.empty() || !plain->build_compiler_version.empty()
            || !plain->build_cflags.empty() || !plain->build_cxxflags.empty()
            || !plain->build_ldflags.empty()) {
            sage::util::log_error("A compiler-free package must not carry build provenance");
            return 1;
        }

        // (e) The job count reaches the phase shells as MAKEFLAGS="-jN" and
        // CARGO_BUILD_JOBS="N"; unset in build.toml means one per hardware
        // thread.
        const unsigned expect_jobs = std::max(1u, std::thread::hardware_concurrency());
        auto job_dir = temp_dir / "bcfg-jobs";
        if (!write_canary_recipe(job_dir, R"(schema_version = 1
[package]
name = "jobcanary"
version = "1.0.0"
release = "1"
description = "build-config jobs canary"
license = "MIT"
channel = "system"
install = [
    'mkdir -p "$DESTDIR/usr/share"',
    'printf "%s" "$MAKEFLAGS" > "$DESTDIR/usr/share/makeflags.txt"',
    'printf "%s" "$CARGO_BUILD_JOBS" > "$DESTDIR/usr/share/cargojobs.txt"',
]
)")) {
            sage::util::log_error("Failed to create jobs canary fixture");
            return 1;
        }
        auto jobcanary = build_with_root(job_dir, canary_root, temp_dir / "bcfg-jobs-x",
                                         "jobcanary-1.0.0-1-x86_64.pkg.tar.zst");
        if (!jobcanary
            || read_text(temp_dir / "bcfg-jobs-x/usr/share/makeflags.txt") != std::format("-j{}", expect_jobs)
            || read_text(temp_dir / "bcfg-jobs-x/usr/share/cargojobs.txt") != std::to_string(expect_jobs)) {
            sage::util::log_error("MAKEFLAGS/CARGO_BUILD_JOBS did not carry the configured job count");
            return 1;
        }

        // (f) A pinned compiler never falls back: an unusable pin fails the
        // build even though the global fallback pair is alive and well.
        auto pin_dir = temp_dir / "bcfg-pin-bad";
        if (!write_canary_recipe(pin_dir, R"(schema_version = 1
[package]
name = "pinbad"
version = "1.0.0"
release = "1"
description = "pinned-compiler failure canary"
license = "MIT"
channel = "system"

[build]
cc = "/nonexistent/sage-no-such-cc"
install = [
    'mkdir -p "$DESTDIR/usr/share"',
    'printf ok > "$DESTDIR/usr/share/done.txt"',
]
)")) {
            sage::util::log_error("Failed to create pinned-compiler fixture");
            return 1;
        }
        {
            CliOptions build_opts;
            build_opts.args = {pin_dir.string()};
            build_opts.target_root = canary_root;  // its global pair probes fine
            if (cmd_build(build_opts) == 0) {
                sage::util::log_error("A broken pinned compiler must fail, not fall back");
                return 1;
            }
        }

        // (g) A service.toml beside the recipe rides the manifest verbatim;
        // the parse round-trip must keep the daemon definition intact for
        // `sage rebuild` to regenerate scripts on an init switch.
        auto svc_dir = temp_dir / "bcfg-svc";
        if (!write_canary_recipe(svc_dir, R"(schema_version = 1
[package]
name = "svcanary"
version = "1.0.0"
release = "1"
description = "service manifest canary"
license = "MIT"
channel = "system"
install = [
    'mkdir -p "$DESTDIR/usr/bin"',
    'printf "#!/bin/sh\n" > "$DESTDIR/usr/bin/svcanary"',
]
)")) {
            sage::util::log_error("Failed to create service canary recipe");
            return 1;
        }
        {
            std::ofstream f(svc_dir / "service.toml");
            f << R"(schema_version = 1
[service]
name = "svcanary"
description = "canary daemon"
exec_start = "/usr/bin/svcanary --foreground"
after = ["net"]
)";
        }
        auto svcpkg = build_with_root(svc_dir, canary_root, temp_dir / "bcfg-svc-x",
                                      "svcanary-1.0.0-1-x86_64.pkg.tar.zst");
        if (!svcpkg || svcpkg->service_toml.empty()) {
            sage::util::log_error("service.toml did not ride the built manifest");
            return 1;
        }
        auto spec_back = sage::service::ServiceSpec::parse_toml(svcpkg->service_toml);
        if (!spec_back || spec_back->name != "svcanary"
            || spec_back->exec_start != "/usr/bin/svcanary --foreground"
            || spec_back->after.size() != 1 || spec_back->after[0] != "net") {
            sage::util::log_error("Manifest service_toml failed to round-trip through serialization");
            return 1;
        }

        sage::util::log_success("13. Build Config Injection, Recipe Override & Compiler Fallback OK");
    }

    // 14. Conffile Protection: reinstall keeps locally modified configuration
    {
        auto temp_dir = std::filesystem::temp_directory_path() / "sage_conffile_test";
        std::filesystem::remove_all(temp_dir);
        std::filesystem::create_directories(temp_dir / "etc/sage");
        std::ofstream(temp_dir / "etc/sage/system.toml") << "schema_version = 1\n";
        auto conf_dir = temp_dir / "confpkg";
        std::filesystem::create_directories(conf_dir);
        auto read_text = [](const std::filesystem::path& p) {
            std::ifstream f(p);
            std::stringstream ss;
            ss << f.rdbuf();
            return ss.str();
        };
        {
            std::ofstream recipe(conf_dir / "recipe.toml");
            recipe << R"(schema_version = 1
[package]
name = "confpkg"
version = "3.0.0"
release = "1"
description = "conffile protection canary"
license = "MIT"
channel = "system"
conffiles = ["/etc/myapp.conf", "/etc/plain.conf"]
install = [
    'mkdir -p "$DESTDIR/etc"',
    'printf "v1\n" > "$DESTDIR/etc/myapp.conf"',
    'printf "v1\n" > "$DESTDIR/etc/plain.conf"',
]
)";
        }

        // Build once; the archive manifest must carry the declaration.
        CliOptions build_opts;
        build_opts.args = {conf_dir.string()};
        build_opts.target_root = temp_dir;
        if (cmd_build(build_opts) != 0) {
            sage::util::log_error("Failed to build confpkg");
            return 1;
        }
        auto conf_inspect = sage::archive::inspect_package(
            conf_dir / "confpkg-3.0.0-1-x86_64.pkg.tar.zst");
        const std::vector<std::string> expected_conffiles{"/etc/myapp.conf", "/etc/plain.conf"};
        if (!conf_inspect || conf_inspect->manifest.conffiles != expected_conffiles) {
            sage::util::log_error("Built archive manifest does not carry the conffiles declaration");
            return 1;
        }
        // Fresh install through a local channel: both files land normally.
        auto repo_dir = temp_dir / "repo";
        std::filesystem::create_directories(repo_dir);
        std::filesystem::copy_file(
            conf_dir / "confpkg-3.0.0-1-x86_64.pkg.tar.zst",
            repo_dir / "confpkg-3.0.0-1-x86_64.pkg.tar.zst",
            std::filesystem::copy_options::overwrite_existing);
        if (!sage::archive::generate_repo_index(repo_dir, "core")) {
            sage::util::log_error("Failed to index conffile test repo");
            return 1;
        }
        auto conf_root = temp_dir / "target";
        std::filesystem::create_directories(conf_root / "etc/sage");
        {
            std::ofstream chan(conf_root / "etc/sage/channels.toml");
            chan << "schema_version = 1\n\n[[channels]]\nname = \"core\"\nurl = \"file://"
                 << repo_dir.string() << "\"\nscope = \"system\"\npriority = 100\nenabled = true\n";
        }
        CliOptions install_opts;
        install_opts.target_root = conf_root;
        // Reinstall goes through the archive argument so it bypasses the
        // same-version no-op filter: a plain name request for an already
        // satisfied version resolves to nothing by design.
        const auto conf_archive_path = repo_dir / "confpkg-3.0.0-1-x86_64.pkg.tar.zst";
        install_opts.args = {conf_archive_path.string()};
        if (cmd_install(install_opts) != 0) {
            sage::util::log_error("Fresh confpkg install failed");
            return 1;
        }
        if (read_text(conf_root / "etc/myapp.conf") != "v1\n"
            || read_text(conf_root / "etc/plain.conf") != "v1\n") {
            sage::util::log_error("Conffiles missing after fresh install");
            return 1;
        }

        // The admin edits one of them, then the same version is reinstalled.
        { std::ofstream f(conf_root / "etc/myapp.conf"); f << "admin-edited"; }
        if (cmd_install(install_opts) != 0) {
            sage::util::log_error("Reinstall over modified config failed");
            return 1;
        }
        if (read_text(conf_root / "etc/myapp.conf") != "admin-edited"
            || read_text(conf_root / "etc/myapp.conf.new") != "v1\n") {
            sage::util::log_error("Modified conffile was not protected across reinstall");
            return 1;
        }
        // The untouched conffile is overwritten in place -- no stray .new.
        if (read_text(conf_root / "etc/plain.conf") != "v1\n"
            || std::filesystem::exists(conf_root / "etc/plain.conf.new")) {
            sage::util::log_error("Unmodified conffile was not cleanly replaced");
            return 1;
        }

        std::filesystem::remove_all(temp_dir);
        sage::util::log_success("14. Conffile Protection on Reinstall OK");
    }

    // 15. Multi-source recipes: `[[source]]` arrays fetch every entry beside
    // the primary archive and stage the extras at src/distfiles/, while the
    // scope collectors keep seeing keys written after the last block.
    {
        auto temp_dir = std::filesystem::temp_directory_path() / "sage_multisrc_test";
        std::filesystem::remove_all(temp_dir);
        std::filesystem::create_directories(temp_dir / "target/etc/sage");
        std::ofstream(temp_dir / "target/etc/sage/system.toml") << "schema_version = 1\n";

        // Model level: the first [[source]] fills the primary slot, the rest
        // become extras, and trailing keys still land in host_deps/provides --
        // they live inside the last array element's TOML table.
        auto multi = sage::package::Recipe::parse_toml(R"(schema_version = 1
[package]
name = "m"
version = "1.0.0"
release = "1"

[[source]]
url = "https://a.example/main.tar.gz"
sha256 = "aaaa"

[[source]]
url = "https://b.example/fix.patch"
sha256 = "bbbb"

dependencies = ["zlib >= 1.3"]
provides = ["virtual/m"]
)");
        if (!multi || multi->source_url != "https://a.example/main.tar.gz"
            || multi->source_sha256 != "aaaa"
            || multi->extra_sources.size() != 1
            || multi->extra_sources[0].url != "https://b.example/fix.patch"
            || multi->extra_sources[0].sha256 != "bbbb"
            || multi->host_deps.size() != 1 || multi->host_deps[0].name != "zlib"
            || multi->provides.size() != 1 || multi->provides[0] != "virtual/m") {
            sage::util::log_error("Multi-source recipe parse dropped an entry or a trailing scope key");
            return 1;
        }
        // Backward compat: the single [source] table yields no extras.
        auto single = sage::package::Recipe::parse_toml(R"(schema_version = 1
[package]
name = "s"
version = "1.0.0"
release = "1"

[source]
url = "https://a.example/main.tar.gz"
sha256 = "aaaa"
)");
        if (!single || single->source_url.empty() || !single->extra_sources.empty()) {
            sage::util::log_error("Single-source recipe parse regressed");
            return 1;
        }

        // End to end over file:// URLs: a primary tarball unpacked to src/ and
        // one plain extra file staged at src/distfiles/, both consumed by the
        // install phase relative to the work directory.
        auto dist_dir = temp_dir / "dist";
        std::filesystem::create_directories(dist_dir / "main-1.0");
        {
            std::ofstream f(dist_dir / "main-1.0/payload.txt");
            f << "primary payload\n";
        }
        {
            std::ofstream f(dist_dir / "extra.txt");
            f << "extra payload\n";
        }
        if (std::system(std::format("tar -czf \"{}\" -C \"{}\" main-1.0",
                (dist_dir / "main.tar.gz").string(), dist_dir.string()).c_str()) != 0) {
            sage::util::log_error("Failed to pack multi-source fixture tarball");
            return 1;
        }
        auto sha_of = [](const std::filesystem::path& p) -> std::string {
            auto h = sage::util::compute_file_sha256(p);
            return h ? *h : "";
        };
        const std::string main_sha = sha_of(dist_dir / "main.tar.gz");
        const std::string extra_sha = sha_of(dist_dir / "extra.txt");

        auto write_recipe = [&](const std::filesystem::path& dir, std::string_view name,
                                const std::string& extra_hash) {
            std::filesystem::create_directories(dir);
            std::ofstream recipe(dir / "recipe.toml");
            recipe << std::format(R"(schema_version = 1
[package]
name = "{}"
version = "1.0.0"
release = "1"
description = "multi-source canary"
license = "MIT"
channel = "system"

[[source]]
url = "file://{}/main.tar.gz"
sha256 = "{}"

[[source]]
url = "file://{}/extra.txt"
sha256 = "{}"

install = [
    'mkdir -p "$DESTDIR/usr/share"',
    'cp payload.txt "$DESTDIR/usr/share/primary.txt"',
    'cp distfiles/extra.txt "$DESTDIR/usr/share/extra.txt"',
]
)", name, dist_dir.string(), main_sha, dist_dir.string(), extra_hash);
            return recipe.good();
        };

        auto ms_dir = temp_dir / "multisrc";
        if (!write_recipe(ms_dir, "multisrc", extra_sha)) {
            sage::util::log_error("Failed to write multi-source fixture recipe");
            return 1;
        }
        CliOptions build_ok;
        build_ok.args = {ms_dir.string()};
        build_ok.target_root = temp_dir / "target";
        if (cmd_build(build_ok) != 0) {
            sage::util::log_error("Failed to build a multi-source recipe");
            return 1;
        }
        auto unpacked = sage::archive::extract_package(
            ms_dir / "multisrc-1.0.0-1-x86_64.pkg.tar.zst", temp_dir / "unpacked");
        auto read_text = [](const std::filesystem::path& p) {
            std::ifstream f(p);
            std::stringstream ss;
            ss << f.rdbuf();
            return ss.str();
        };
        if (!unpacked
            || read_text(temp_dir / "unpacked/usr/share/primary.txt") != "primary payload\n"
            || read_text(temp_dir / "unpacked/usr/share/extra.txt") != "extra payload\n") {
            sage::util::log_error("Extra sources did not reach src/distfiles/ during build");
            return 1;
        }
        // The staged copies are consumed, not shipped: no distfiles leak into
        // the package payload or the installed tree.
        if (std::filesystem::exists(temp_dir / "unpacked/usr/share/distfiles")) {
            sage::util::log_error("distfiles staging leaked into the package payload");
            return 1;
        }

        // A wrong hash on any entry -- including extras -- is fatal.
        auto bad_dir = temp_dir / "multisrc-bad";
        if (!write_recipe(bad_dir, "multisrcbad",
                          std::string(extra_sha.size(), 'f'))) {
            sage::util::log_error("Failed to write bad-hash fixture recipe");
            return 1;
        }
        CliOptions build_bad;
        build_bad.args = {bad_dir.string()};
        build_bad.target_root = temp_dir / "target";
        if (cmd_build(build_bad) == 0) {
            sage::util::log_error("A bad sha256 on an extra source was not fatal");
            return 1;
        }

        std::filesystem::remove_all(temp_dir);
        sage::util::log_success("15. Multi-Source Fetch, Verification & Staging OK");
    }

    std::filesystem::remove_all(temp_dir);
    sage::util::log_success("🎉 All Sage Master Architecture & Subsystem Integration Tests Passed Successfully!");
    return 0;
}
} // namespace sage::tests
