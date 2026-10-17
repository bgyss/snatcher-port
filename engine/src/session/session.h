// Session recording (docs/PLAYTEST.md "Sessions"): one folder per booted disc with the replay, checkpoints, log, manifest,
// user markers and watchdog failure bundles, enough to replay and verify the session headless. Shared by the SDL app
// (always on) and scd_headless --session-out (tests). Recording only reads emulator state and never stops play: a file
// that cannot be written is logged once and dropped.
#pragma once
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>

#include "checkpoint.h"
#include "manifest.h"
#include "replay.h"
#include "system.h"
#include "watchdog.h"

namespace scd {

std::string session_stamp(std::time_t t);   // local time, YYYYMMDD-HHMMSS
// <cwd>/work/sessions when cwd is the repository (has engine/src/scd), else <pref>sessions (pref ends in a separator).
std::string default_session_parent(const std::string& cwd, const std::string& pref);
// Creates <parent>/<stamp>, or <stamp>-1, -2, ... when it exists (parents as needed). "" with *err on failure.
std::string create_session_dir(const std::string& parent, const std::string& stamp, std::string* err);

struct SessionConfig {
    std::string cue;         // the disc (its SHA-1 goes into the replay header and the manifest)
    std::string bram_path;   // the backup RAM file System::init loaded ("" = none)
    std::string engine;      // replay header "engine"
    Manifest manifest;       // app, os, sdl, renderer, args from the front end; Session fills the rest
    double cd_speed = 1.0;
    bool justifier = false;
    int ckpt_every = 300;
    int hang_frames = 300;
    uint64_t flush_ms = 1000;   // replay/checkpoint lines reach the disk at least this often (and on markers, failures, end)
};

class Session {
public:
    Session() = default;
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    ~Session();

    // Right after System::init() succeeded, before the first run_frame(): creates `dir`, copies the backup RAM to
    // session.replay.bram, writes manifest.json and the replay header, opens session.log and records input through
    // System::set_replay_record. False with *err when `dir` cannot be created; nothing is recorded then.
    bool begin(System& sys, const SessionConfig& cfg, const std::string& dir, std::string* err);
    // After every run_frame(): checkpoint, watchdog (e020: the sampled $E020, sys.e020() except under the
    // SCD_TEST_STICK_E020 test hook) and a flush once flush_ms passed since the last (now_ms: a monotonic clock).
    void after_frame(System& sys, uint16_t e020, uint64_t now_ms);
    // F8 / F10: markers/NNN-f<frame>/. Returns the marker number, or 0 when it could not be written.
    int marker(System& sys);
    // Final checkpoint, flush, stop recording, manifest with ended/frames/exit, log the session size, close session.log.
    void end(System& sys, const std::string& exit_reason);
    // --record-input: the replay so far to `path`, the starting backup RAM to `path`.bram (when there was one).
    bool export_replay(const std::string& path) const;

    bool active() const { return active_; }
    const std::string& dir() const { return dir_; }
    int markers() const { return manifest_.markers; }
    int failures() const { return manifest_.failures; }
    const Replay& replay() const { return replay_; }

private:
    struct OutFile {
        FILE* f = nullptr;
        std::string path;
        bool open(const std::string& p);         // logs and stays closed on failure
        void write(const std::string& text);     // on a failed write: logs once, closes, writes nothing more
        void close();
    };
    void flush();
    void write_manifest();
    void failure(System& sys, const WatchVerdict& v);

    bool active_ = false;
    std::string dir_;
    SessionConfig cfg_;
    Manifest manifest_;
    Replay replay_;   // System records into it: its address must stay fixed while active (do not move a Session)
    Checkpointer ckpt_{300};
    Watchdog dog_;
    HangLatch hangs_;
    bool halted_written_ = false;
    OutFile replay_f_, ckpt_f_;
    std::string pending_ckpt_;
    size_t pad_written_ = 0, gun_written_ = 0;
    uint64_t last_flush_ms_ = 0;
};

}  // namespace scd
