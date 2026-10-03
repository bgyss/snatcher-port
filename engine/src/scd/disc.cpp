#include "disc.h"

#include <cstring>
#include <fstream>
#include <sstream>

namespace scd {

namespace {

bool parse_msf(const std::string& s, uint32_t* frames) {
    unsigned m, sec, f;
    if (std::sscanf(s.c_str(), "%u:%u:%u", &m, &sec, &f) != 3) return false;
    *frames = (m * 60 + sec) * 75 + f;
    return true;
}

std::string dir_of(const std::string& path) {
    auto p = path.find_last_of("/\\");
    return p == std::string::npos ? std::string() : path.substr(0, p + 1);
}

}  // namespace

std::unique_ptr<Disc> Disc::open(const std::string& cue_path, std::string* error) {
    std::ifstream cue(cue_path);
    if (!cue) {
        if (error) *error = "cannot open cue file: " + cue_path;
        return nullptr;
    }
    // Single-file cues (one .bin) and split ones (one .bin per track, the current redump layout) both work: each
    // FILE starts where the previous one ends, so a split image maps onto the same disc positions as a joined one.
    std::unique_ptr<Disc> d(new Disc());
    std::string line;
    Track cur;
    bool have_track = false;
    while (std::getline(cue, line)) {
        std::istringstream is(line);
        std::string key;
        is >> key;
        if (key == "FILE") {
            auto a = line.find('"'), b = line.rfind('"');
            if (a == std::string::npos || b <= a) continue;
            std::string path = dir_of(cue_path) + line.substr(a + 1, b - a - 1);
            std::FILE* f = std::fopen(path.c_str(), "rb");
            if (!f) {
                if (error) *error = "cannot open image: " + path;
                return nullptr;
            }
            std::fseek(f, 0, SEEK_END);
            uint32_t sectors = static_cast<uint32_t>(std::ftell(f) / kRawSector);
            d->files_.push_back({f, d->total_sectors_, sectors});
            d->total_sectors_ += sectors;
        } else if (key == "TRACK") {
            if (have_track) d->tracks_.push_back(cur);
            cur = Track();
            std::string type;
            is >> cur.number >> type;
            cur.audio = (type == "AUDIO");
            if (!cur.audio && type != "MODE1/2352") {
                if (error) *error = "unsupported track type " + type + " (need MODE1/2352 + AUDIO)";
                return nullptr;
            }
            have_track = true;
        } else if (key == "INDEX") {
            int idx;
            std::string msf;
            is >> idx >> msf;
            if (idx == 1 && have_track && !d->files_.empty() && parse_msf(msf, &cur.start_lba))
                cur.start_lba += d->files_.back().start;   // INDEX times are relative to the current FILE
        }
    }
    if (have_track) d->tracks_.push_back(cur);
    if (d->files_.empty() || d->tracks_.empty()) {
        if (error) *error = "cue sheet has no FILE/TRACK entries";
        return nullptr;
    }
    for (size_t i = 0; i < d->tracks_.size(); ++i)
        d->tracks_[i].end_lba = i + 1 < d->tracks_.size() ? d->tracks_[i + 1].start_lba : d->total_sectors_;
    return d;
}

Disc::~Disc() {
    for (auto& f : files_) std::fclose(f.fp);
}

bool Disc::read_raw(uint32_t lba, uint8_t out[kRawSector]) {
    if (lba >= total_sectors_) return false;
    for (auto& f : files_) {
        if (lba < f.start || lba >= f.start + f.sectors) continue;
        if (std::fseek(f.fp, static_cast<long>(lba - f.start) * kRawSector, SEEK_SET) != 0) return false;
        return std::fread(out, 1, kRawSector, f.fp) == static_cast<size_t>(kRawSector);
    }
    return false;
}

bool Disc::read_data(uint32_t lba, uint8_t out[kUserData]) {
    uint8_t raw[kRawSector];
    if (!read_raw(lba, raw)) return false;
    std::memcpy(out, raw + 16, kUserData);
    return true;
}

}  // namespace scd
