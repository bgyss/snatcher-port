// CD image access: .cue plus raw MODE1/2352 + audio .bin files (redump layout: one joined .bin or one per track).
#pragma once
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace scd {

constexpr int kRawSector = 2352;
constexpr int kUserData = 2048;

struct Track {
    int number = 0;
    bool audio = false;
    uint32_t start_lba = 0;   // INDEX 01, in sectors from the start of the first bin (no 150 pregap)
    uint32_t end_lba = 0;     // exclusive
};

class Disc {
public:
    // Returns nullptr and fills *error on failure.
    static std::unique_ptr<Disc> open(const std::string& cue_path, std::string* error);
    ~Disc();

    const std::vector<Track>& tracks() const { return tracks_; }
    uint32_t total_sectors() const { return total_sectors_; }

    // 2048 bytes of user data from a MODE1 sector. Returns false when out of range.
    bool read_data(uint32_t lba, uint8_t out[kUserData]);
    // 2352 raw bytes (used for CD-DA).
    bool read_raw(uint32_t lba, uint8_t out[kRawSector]);

private:
    Disc() = default;
    struct File {
        std::FILE* fp;
        uint32_t start, sectors;   // position of this .bin in the joined image
    };
    std::vector<File> files_;
    std::vector<Track> tracks_;
    uint32_t total_sectors_ = 0;
};

}  // namespace scd
