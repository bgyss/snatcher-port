// Demo-video recorder: emulated frames + audio -> MP4 (H.264 or H.265, 1440x1080 4:3, ~60 fps, AAC), through ffmpeg.
// Pass 1 (live) pipes frames to ffmpeg as lossless H.264, cheap enough to run beside the emulator, and writes the audio
// to a WAV. Frames go on a 1280-wide canvas, a whole multiple of both H40 (320 x4) and H32 (256 x5), so either mode
// fills the width as it does on a TV. finish() scales nearest-neighbour, area-filters down to 1440x1080 (4:3, even
// pixel widths), encodes H.264 (plays everywhere) or H.265 (about half the size, tagged hvc1 for Apple players) and
// muxes AAC audio. Frames and samples both come from the emulator, so audio and video stay in sync however the host paced them. ffmpeg: $SNATCHER_FFMPEG, else Homebrew,
// nix profile or PATH.
#pragma once

#include <algorithm>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "system.h"

#ifdef _WIN32
#define SCD_POPEN _popen
#define SCD_PCLOSE _pclose
#else
#define SCD_POPEN popen
#define SCD_PCLOSE pclose
#endif

namespace scd {

class Recorder {
public:
    ~Recorder() { if (video_) finish(nullptr); }

    bool active() const { return video_ != nullptr; }
    const std::string& path() const { return out_; }

    // Canvas height is the active height at start (224 on NTSC); a taller frame is cropped, a shorter one padded.
    // codec: "h264" or "h265".
    bool start(const std::string& out, int height, const std::string& codec, std::string* err) {
        if (codec != "h264" && codec != "h265") { if (err) *err = "unknown video codec " + codec + " (h264 or h265)"; return false; }
        hevc_ = codec == "h265";
        out_ = out;
        h_ = height;
        tmp_video_ = out + ".video.mkv";
        tmp_audio_ = out + ".audio.wav";
        audio_ = std::fopen(tmp_audio_.c_str(), "wb");
        if (!audio_) { if (err) *err = "cannot write " + tmp_audio_; return false; }
        write_wav_header(0);
        std::string cmd = ffmpeg() + " -hide_banner -loglevel error -y -f rawvideo -pixel_format bgr0 -video_size " + std::to_string(kCanvasW) + "x" +
                          std::to_string(h_) + " -framerate 59.9227 -i - -c:v libx264rgb -preset ultrafast -qp 0 " + quote(tmp_video_);
#ifdef _WIN32
        video_ = SCD_POPEN(cmd.c_str(), "wb");
#else
        std::signal(SIGPIPE, SIG_IGN);   // ffmpeg exiting early must not kill the emulator
        video_ = SCD_POPEN(cmd.c_str(), "w");
#endif
        if (!video_) { if (err) *err = "cannot run ffmpeg"; std::fclose(audio_); audio_ = nullptr; return false; }
        row_.assign(size_t(kCanvasW) * h_, 0);
        frames_ = 0;
        samples_ = 0;
        return true;
    }

    // fb: 320-pixel stride, w active pixels per line, 0x00RRGGBB (bgr0 bytes on little-endian hosts). pcm: interleaved stereo int16.
    void frame(const uint32_t* fb, int w, int h, const std::vector<int16_t>& pcm) {
        if (!video_) return;
        std::fill(row_.begin(), row_.end(), 0);
        const int sx = w > 0 && kCanvasW % w == 0 ? kCanvasW / w : kCanvasW / 320;
        for (int y = 0; y < std::min(h, h_); ++y)
            for (int x = 0; x < std::min(w, kCanvasW / sx); ++x)
                std::fill_n(row_.begin() + size_t(y) * kCanvasW + size_t(x) * sx, sx, fb[y * 320 + x]);
        std::fwrite(row_.data(), 4, row_.size(), video_);
        std::fwrite(pcm.data(), 2, pcm.size(), audio_);
        samples_ += pcm.size();
        ++frames_;
    }

    // Closes pass 1 and runs the final encode (blocking: seconds per minute of footage). Returns true on success.
    bool finish(std::string* err) {
        if (!video_) return false;
        int rc = SCD_PCLOSE(video_);
        video_ = nullptr;
        write_wav_header(uint32_t(samples_ * 2));
        std::fclose(audio_);
        audio_ = nullptr;
        if (rc != 0 || frames_ == 0) { if (err) *err = "ffmpeg failed (is it installed? set SNATCHER_FFMPEG)"; return false; }
        // Frame rate from the samples the core actually produced per frame (it makes kSampleRate/60 per frame), so the
        // picture can't drift from the sound.
        char fps[32];
        std::snprintf(fps, sizeof fps, "%.6f", double(frames_) * kSampleRate / (double(samples_) / 2));
        std::string cmd = ffmpeg() + " -hide_banner -loglevel error -y -r " + fps + " -i " + quote(tmp_video_) + " -i " + quote(tmp_audio_) +
                          " -vf \"scale=iw*2:ih*6:flags=neighbor,scale=1440:1080:flags=area,setsar=1\""
                          + std::string(hevc_ ? " -c:v libx265 -preset slow -crf 18 -tag:v hvc1 -x265-params log-level=error"
                                              : " -c:v libx264 -preset slow -crf 16") +
                          " -pix_fmt yuv420p -c:a aac -b:a 192k -movflags +faststart " + quote(out_);
        bool ok = std::system(cmd.c_str()) == 0;
        if (ok) { std::remove(tmp_video_.c_str()); std::remove(tmp_audio_.c_str()); }
        else if (err) *err = "final encode failed; pass-1 files kept: " + tmp_video_ + ", " + tmp_audio_;
        return ok;
    }

    uint64_t frames() const { return frames_; }

private:
    static std::string ffmpeg() {
        if (const char* e = std::getenv("SNATCHER_FFMPEG")) return quote(e);
#ifndef _WIN32
        // An app launched from Finder has a minimal PATH.
        std::vector<std::string> c = {"/opt/homebrew/bin/ffmpeg", "/usr/local/bin/ffmpeg"};
        if (const char* home = std::getenv("HOME")) c.push_back(std::string(home) + "/.nix-profile/bin/ffmpeg");
        for (auto& p : c) if (FILE* f = std::fopen(p.c_str(), "rb")) { std::fclose(f); return quote(p); }
#endif
        return "ffmpeg";
    }

    static std::string quote(const std::string& s) {
#ifdef _WIN32
        return "\"" + s + "\"";
#else
        std::string q = "'";
        for (char c : s) q += c == '\'' ? std::string("'\\''") : std::string(1, c);
        return q + "'";
#endif
    }

    void write_wav_header(uint32_t data) {
        std::fseek(audio_, 0, SEEK_SET);
        uint32_t rate = kSampleRate, byte_rate = rate * 4, riff = 36 + data, fmt_len = 16;
        uint16_t fmt = 1, ch = 2, align = 4, bits = 16;
        std::fwrite("RIFF", 1, 4, audio_); std::fwrite(&riff, 4, 1, audio_); std::fwrite("WAVEfmt ", 1, 8, audio_);
        std::fwrite(&fmt_len, 4, 1, audio_); std::fwrite(&fmt, 2, 1, audio_); std::fwrite(&ch, 2, 1, audio_);
        std::fwrite(&rate, 4, 1, audio_); std::fwrite(&byte_rate, 4, 1, audio_); std::fwrite(&align, 2, 1, audio_);
        std::fwrite(&bits, 2, 1, audio_); std::fwrite("data", 1, 4, audio_); std::fwrite(&data, 4, 1, audio_);
        std::fseek(audio_, 0, SEEK_END);
    }

    static constexpr int kCanvasW = 1280;
    std::string out_, tmp_video_, tmp_audio_;
    FILE* video_ = nullptr;
    FILE* audio_ = nullptr;
    int h_ = 224;
    bool hevc_ = false;
    std::vector<uint32_t> row_;
    uint64_t frames_ = 0, samples_ = 0;
};

}  // namespace scd
