export module sage.repack.main;

import std;
import sage;
import sage.repack;

// CLI driver for the repack utility.
export int run_repack(int argc, char* argv[]) {
    if (argc < 3) {
        std::println("Usage: sage-repack <list-file> <out-dir>");
        return 2;
    }
    const std::string list_file = argv[1];
    const std::string out_root = argv[2];

    std::ifstream list(list_file);
    if (!list) {
        std::println("Cannot open list file {}", list_file);
        return 2;
    }
    int ok = 0, skipped = 0, failed = 0;
    std::string line;
    while (std::getline(list, line)) {
        if (line.empty()) continue;
        const char* base_env = std::getenv("REPACK_BASE");
        const auto dir = std::string(base_env ? base_env : "/mnt/recipes") + "/" + line;
        std::string archive;
        std::error_code dir_ec;
        for (const auto& e : std::filesystem::directory_iterator(dir, dir_ec))
            if (!dir_ec && e.path().extension() == ".zst") { archive = e.path().string(); break; }
        if (dir_ec) {
            std::println("SKIP (missing dir): {}", line);
            ++skipped;
            continue;
        }
        if (archive.empty()) {
            std::println("SKIP (no archive): {}", line);
            ++skipped;
            continue;
        }
                const auto work = std::filesystem::path(out_root) / ".work" / line;
        std::filesystem::create_directories(work);
        auto extracted = sage::archive::extract_package(archive, work);
        if (!extracted) {
            std::println("FAIL extract {}: {}", line, extracted.error());
            ++failed;
            continue;
        }
        sage::repack::Provenance prov;
        sage::repack::scan_payload(work, prov);

        auto& m = extracted->manifest;
        // Repack owns this section wholesale: stale entries from previous
        // runs must not survive alongside freshly verified ones.
        m.build_producers.clear();
        for (const auto& producer : prov.producers) {
            sage::package::PackageManifest::BuildProducer entry;
            entry.name = producer;
            if (auto vit = prov.versions.find(producer); vit != prov.versions.end())
                entry.versions.assign(vit->second.begin(), vit->second.end());
            if (auto sit = prov.switches.find(producer); sit != prov.switches.end()) {
                std::string joined;
                for (const auto& sw : sit->second)
                    joined += (joined.empty() ? "" : " ") + sw;
                entry.flags = std::move(joined);
            }
            m.build_producers.push_back(std::move(entry));
        }

        const std::filesystem::path out_archive =
            std::filesystem::path(out_root) / (line + ".pkg.tar.zst");
        std::filesystem::create_directories(
            std::filesystem::path(out_archive).parent_path());
        if (auto packed = sage::archive::create_package(
                m, work, out_archive);
            !packed) {
            std::println("FAIL pack {}: {}", line, packed.error());
            ++failed;
            continue;
        }
        std::filesystem::remove_all(work);
        std::println("OK: {} -> {}", line, out_archive.string());
        ++ok;
    }
    std::println("repacked={} skipped={} failed={}", ok, skipped, failed);
    return failed == 0 ? 0 : 1;
}

// Plain-C entry shim: module-mangled C++ names are not callable from a
// non-module TU.
extern "C" int sage_repack_main(int argc, char* argv[]) {
    return run_repack(argc, argv);
}
