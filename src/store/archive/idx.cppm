export module sage.archive:idx;

import std;
import sage.package;

export namespace sage::archive {

using std::size_t;
using std::uint32_t;
using std::uint64_t;

inline std::string serialize_files_idx(const std::vector<package::FileEntry>& files) {
    std::ostringstream ss;
    ss << "# sage files index v2\n";
    ss << "# type\tmode\tuid\tgid\tcaps\tsize\tsha256\tpath\ttarget\n";
    for (const auto& f : files) {
        ss << package::to_string(f.type) << '\t'
           << std::format("{:o}", f.mode) << '\t'
           << f.uid << '\t'
           << f.gid << '\t'
           << (f.caps.empty() ? "-" : f.caps) << '\t'
           << f.size << '\t'
           << (f.sha256.empty() ? "-" : f.sha256) << '\t'
           << f.path << '\t'
           << (f.link_target.empty() ? "-" : f.link_target) << '\n';
    }
    return ss.str();
}

inline std::vector<package::FileEntry> parse_files_idx(std::string_view content) {
    std::vector<package::FileEntry> out;
    size_t pos = 0;
    while (pos < content.size()) {
        size_t eol = content.find('\n', pos);
        if (eol == std::string_view::npos) eol = content.size();
        std::string_view line = content.substr(pos, eol - pos);
        pos = eol + 1;
        if (line.ends_with('\r')) line.remove_suffix(1);
        if (line.empty() || line.front() == '#') continue;

        std::vector<std::string_view> fields;
        size_t fpos = 0;
        while (fpos < line.size()) {
            size_t tab = line.find('\t', fpos);
            if (tab == std::string_view::npos) {
                fields.push_back(line.substr(fpos));
                break;
            }
            fields.push_back(line.substr(fpos, tab - fpos));
            fpos = tab + 1;
        }

        if (fields.size() == 9) {
            package::FileEntry fe;
            fe.type = package::parse_file_type(fields[0]);
            fe.mode = static_cast<uint32_t>(std::stoull(std::string(fields[1]), nullptr, 8));
            for (char c : fields[2]) {
                if (c >= '0' && c <= '9') fe.uid = fe.uid * 10 + static_cast<uint32_t>(c - '0');
            }
            for (char c : fields[3]) {
                if (c >= '0' && c <= '9') fe.gid = fe.gid * 10 + static_cast<uint32_t>(c - '0');
            }
            if (fields[4] != "-") fe.caps = std::string(fields[4]);
            for (char c : fields[5]) {
                if (c >= '0' && c <= '9') fe.size = fe.size * 10 + static_cast<uint64_t>(c - '0');
            }
            if (fields[6] != "-") fe.sha256 = std::string(fields[6]);
            fe.path = std::string(fields[7]);
            if (fields[8] != "-") fe.link_target = std::string(fields[8]);
            if (!fe.path.empty()) out.push_back(std::move(fe));
        } else if (fields.size() == 6) {
            // Legacy 6-column format: type\tmode\tsize\tsha256\tpath\ttarget
            package::FileEntry fe;
            fe.type = package::parse_file_type(fields[0]);
            fe.mode = static_cast<uint32_t>(std::stoull(std::string(fields[1]), nullptr, 8));
            for (char c : fields[2]) {
                if (c >= '0' && c <= '9') fe.size = fe.size * 10 + static_cast<uint64_t>(c - '0');
            }
            if (fields[3] != "-") fe.sha256 = std::string(fields[3]);
            fe.path = std::string(fields[4]);
            if (fields[5] != "-") fe.link_target = std::string(fields[5]);
            if (!fe.path.empty()) out.push_back(std::move(fe));
        }
    }
    return out;
}
} // namespace sage::archive
