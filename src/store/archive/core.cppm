export module sage.archive:core;

import std;
import sage.package;

export namespace sage::archive {

using std::size_t;
using std::uint8_t;
using std::uint32_t;
using std::uint64_t;

inline constexpr std::string_view temp_file_prefix = ".sage-tmp-";
inline constexpr uint64_t canonical_build_epoch = 1700000000;

struct ExtractedPackage {
    package::PackageManifest manifest;
    std::vector<package::FileEntry> extracted_files;
    std::vector<package::FileEntry> declared_files;
};

struct InspectedPackage {
    package::PackageManifest manifest;
    std::vector<package::FileEntry> data_files;
    std::vector<package::FileEntry> declared_files;
    uint64_t source_device{0};
    uint64_t source_inode{0};
    uint64_t source_size{0};
    uint64_t source_mtime_ns{0};
    uint64_t source_ctime_ns{0};
};

} // namespace sage::archive
