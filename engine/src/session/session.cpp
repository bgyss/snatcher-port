#include "session.h"

#include <filesystem>
#include <system_error>

#include "debug_dump.h"

namespace scd {
namespace fs = std::filesystem;

std::string session_stamp(std::time_t t) {
    char s[32];
    std::strftime(s, sizeof s, "%Y%m%d-%H%M%S", std::localtime(&t));
    return s;
}

std::string default_session_parent(const std::string& cwd, const std::string& pref) {
    std::error_code ec;
    if (!cwd.empty() && fs::is_directory(fs::path(cwd) / "engine" / "src" / "scd", ec)) return (fs::path(cwd) / "work" / "sessions").string();
    return pref + "sessions";
}

std::string create_session_dir(const std::string& parent, const std::string& stamp, std::string* err) {
    std::error_code ec;
    fs::create_directories(parent, ec);
    for (int n = 0; n < 1000; ++n) {
        const fs::path p = fs::path(parent) / (n ? stamp + "-" + std::to_string(n) : stamp);
        if (fs::create_directory(p, ec)) return p.string();
        if (ec) break;
    }
    if (err) *err = "cannot create a session folder in " + parent + (ec ? ": " + ec.message() : std::string());
    return "";
}

bool Session::OutFile::open(const std::string& p) {
    close();
    path = p;
    f = std::fopen(p.c_str(), "wb");
    if (!f) debug_log("session: cannot open %s; recording continues without it\n", p.c_str());
    return f != nullptr;
}

void Session::OutFile::write(const std::string& text) {
    if (!f || text.empty()) return;
    if (std::fwrite(text.data(), 1, text.size(), f) != text.size() || std::fflush(f) != 0) {
        debug_log("session: writing %s failed (disk full?); recording continues without it\n", path.c_str());
        std::fclose(f);
        f = nullptr;
    }
}

void Session::OutFile::close() {
    if (f) std::fclose(f);
    f = nullptr;
}

Session::~Session() {
    replay_f_.close();
    ckpt_f_.close();
}

bool Session::begin(System& sys, const SessionConfig& cfg, const std::string& dir, std::string* err) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (!fs::is_directory(dir, ec)) {
        if (err) *err = "cannot create session folder " + dir;
        return false;
    }
    dir_ = dir;
    cfg_ = cfg;
    if (!sys.open_log(dir_ + "/session.log")) std::fprintf(stderr, "session: cannot open %s/session.log\n", dir_.c_str());
    debug_log("session: recording to %s\n", dir_.c_str());

    replay_ = Replay{};
    replay_.disc_sha1 = disc_sha1_from_cue(cfg.cue);
    replay_.bram_sha1 = cfg.bram_path.empty() ? "none" : sha1_file(cfg.bram_path);
    replay_.engine = cfg.engine;
    replay_.cd_speed = cfg.cd_speed;
    replay_.justifier = cfg.justifier;
    if (replay_.bram_sha1 != "none") {   // the core writes the live file back at shutdown: keep the boot state
        fs::copy_file(cfg.bram_path, dir_ + "/session.replay.bram", fs::copy_options::overwrite_existing, ec);
        if (ec) debug_log("session: cannot copy the backup RAM (%s); the replay will not verify\n", ec.message().c_str());
    }

    manifest_ = cfg.manifest;
    manifest_.build = sys.translated() ? "translated" : "interpreter";
    manifest_.cd_speed = cfg.cd_speed;
    manifest_.justifier = cfg.justifier;
    manifest_.ckpt_every = cfg.ckpt_every;
    manifest_.disc_sha1 = replay_.disc_sha1;
    manifest_.bram_sha1 = replay_.bram_sha1;
    manifest_.started = iso8601_local(std::time(nullptr));
    manifest_.ended.clear();
    manifest_.has_frames = false;
    manifest_.exit.clear();
    manifest_.markers = manifest_.failures = 0;
    write_manifest();

    if (replay_f_.open(dir_ + "/session.replay")) replay_f_.write(replay_.serialize_header());
    ckpt_f_.open(dir_ + "/session.ckpt");
    ckpt_ = Checkpointer(cfg.ckpt_every);
    dog_ = Watchdog({cfg.hang_frames, 1800});
    hangs_ = HangLatch{};
    halted_written_ = false;
    pending_ckpt_.clear();
    pad_written_ = gun_written_ = 0;
    last_flush_ms_ = 0;
    sys.set_replay_record(&replay_);
    active_ = true;
    return true;
}

void Session::after_frame(System& sys, uint16_t e020, uint64_t now_ms) {
    if (!active_) return;
    pending_ckpt_ += ckpt_.after_frame(sys);
    const WatchVerdict v = dog_.feed({sys.frame_count(), e020, sys.fb_hash(), sys.halted()});
    if (hangs_.fire(v, e020)) failure(sys, v);
    else if (v.kind == WatchVerdict::kHalted && !halted_written_) { halted_written_ = true; failure(sys, v); }
    else if (v.kind == WatchVerdict::kStillScreen) debug_log("[watch] screen unchanged since frame %llu\n", (unsigned long long)v.since);
    if (now_ms - last_flush_ms_ >= cfg_.flush_ms) {
        flush();
        last_flush_ms_ = now_ms;
    }
}

void Session::flush() {
    replay_f_.write(replay_.serialize_events(pad_written_, gun_written_));
    pad_written_ = replay_.events.size();
    gun_written_ = replay_.gun_events.size();
    ckpt_f_.write(pending_ckpt_);
    pending_ckpt_.clear();
}

void Session::write_manifest() {
    const std::string tmp = dir_ + "/manifest.json.tmp", path = dir_ + "/manifest.json";
    if (!write_text(tmp, manifest_json(manifest_))) {
        debug_log("session: cannot write %s\n", tmp.c_str());
        return;
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);   // atomic replace: a crash leaves the previous manifest, never half of one
    if (ec) debug_log("session: cannot write %s: %s\n", path.c_str(), ec.message().c_str());
}

void Session::failure(System& sys, const WatchVerdict& v) {
    char name[64];
    std::snprintf(name, sizeof name, "failure/%03d-%s-f%llu", manifest_.failures + 1, verdict_name(v.kind), (unsigned long long)sys.frame_count());
    const std::string d = dir_ + "/" + name;
    std::error_code ec;
    fs::create_directories(d, ec);
    if (ec) {
        debug_log("session: cannot create %s\n", d.c_str());
        return;
    }
    if (!write_failure_bundle(sys, d, replay_, v)) debug_log("session: %s is incomplete\n", d.c_str());
    ++manifest_.failures;
    flush();
    write_manifest();
}

int Session::marker(System& sys) {
    if (!active_) return 0;
    const int n = manifest_.markers + 1;
    char name[64];
    std::snprintf(name, sizeof name, "markers/%03d-f%llu", n, (unsigned long long)sys.frame_count());
    const std::string d = dir_ + "/" + name;
    std::error_code ec;
    fs::create_directories(d, ec);
    if (ec || !write_marker(sys, d, iso8601_local(std::time(nullptr)))) {
        debug_log("session: could not write marker %s\n", d.c_str());
        return 0;
    }
    manifest_.markers = n;
    debug_log("[user] marker %03d at frame %llu\n", n, (unsigned long long)sys.frame_count());
    flush();
    write_manifest();
    return n;
}

void Session::end(System& sys, const std::string& exit_reason) {
    if (!active_) return;
    pending_ckpt_ += ckpt_.final_line(sys);
    flush();
    replay_f_.close();
    ckpt_f_.close();
    sys.set_replay_record(nullptr);
    manifest_.ended = iso8601_local(std::time(nullptr));
    manifest_.has_frames = true;
    manifest_.frames = sys.frame_count();
    manifest_.exit = exit_reason;
    write_manifest();
    uintmax_t bytes = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir_, ec), last; !ec && it != last; it.increment(ec)) {
        std::error_code fe;
        if (it->is_regular_file(fe)) {
            const uintmax_t sz = it->file_size(fe);
            if (!fe) bytes += sz;
        }
    }
    debug_log("session: %s ended (%s) at frame %llu, %llu bytes\n", dir_.c_str(), exit_reason.c_str(), (unsigned long long)sys.frame_count(),
              (unsigned long long)bytes);
    sys.close_log();
    active_ = false;
}

bool Session::export_replay(const std::string& path) const {
    bool ok = write_text(path, replay_.serialize());
    if (replay_.bram_sha1 != "none") {
        std::error_code ec;
        fs::copy_file(dir_ + "/session.replay.bram", path + ".bram", fs::copy_options::overwrite_existing, ec);
        ok = ok && !ec;
    }
    return ok;
}

}  // namespace scd
