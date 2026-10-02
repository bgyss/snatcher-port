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
    std::unique_ptr<Disc> d(new Disc());
    std::string bin_name, line;
    Track cur;
    bool have_track = false;
    while (std::getline(cue, line)) {
        std::istringstream is(line);
        std::string key;
        is >> key;
        if (key == "FILE") {
            auto a = line.find('"'), b = line.rfind('"');
            if (a == std::string::npos || b <= a) continue;
            if (!bin_name.empty()) {
                if (error) *error = "multi-file cue sheets are not supported";
                return nullptr;
            }
            bin_name = line.substr(a + 1, b - a - 1);
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
            if (idx == 1 && have_track) parse_msf(msf, &cur.start_lba);
        }
    }
    if (have_track) d->tracks_.push_back(cur);
    if (bin_name.empty() || d->tracks_.empty()) {
        if (error) *error = "cue sheet has no FILE/TRACK entries";
        return nullptr;
    }
    d->file_ = std::fopen((dir_of(cue_path) + bin_name).c_str(), "rb");
    if (!d->file_) {
        if (error) *error = "cannot open image: " + dir_of(cue_path) + bin_name;
        return nullptr;
    }
    std::fseek(d->file_, 0, SEEK_END);
    d->total_sectors_ = static_cast<uint32_t>(std::ftell(d->file_) / kRawSector);
    for (size_t i = 0; i < d->tracks_.size(); ++i)
        d->tracks_[i].end_lba = i + 1 < d->tracks_.size() ? d->tracks_[i + 1].start_lba : d->total_sectors_;
    return d;
}

Disc::~Disc() {
    if (file_) std::fclose(file_);
}

bool Disc::read_raw(uint32_t lba, uint8_t out[kRawSector]) {
    if (lba >= total_sectors_) return false;
    if (std::fseek(file_, static_cast<long>(lba) * kRawSector, SEEK_SET) != 0) return false;
    return std::fread(out, 1, kRawSector, file_) == static_cast<size_t>(kRawSector);
}

bool Disc::read_data(uint32_t lba, uint8_t out[kUserData]) {
    uint8_t raw[kRawSector];
    if (!read_raw(lba, raw)) return false;
    std::memcpy(out, raw + 16, kUserData);
    return true;
}

}  // namespace scd
