# Session Recording Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every play session in the SDL app (`snatcher`) leaves a folder with which a developer can replay it exactly in `scd_headless` (light gun included), prove the replay matches what the user saw, identify the build, find the moments the user flagged and examine any hang or crash.

**Architecture:**
- The replay format gains v2 `g` lines (Justifier state). `System::run_frame()` records and applies the gun at the same point where it records and applies the pad today.
- Two shared units move out of `headless.cpp` into `scdcore`: `checkpoint.{h,cpp}` (the `--ckpt-out` writer) and `debug_dump.{h,cpp}` (PPM, RAM dumps, failure bundle, marker).
- A new `scdsession` library holds `manifest.{h,cpp}` (JSON manifest, with the git commit from a CMake-generated `build_stamp.h`) and `session.{h,cpp}` (the session folder writer: incremental replay/checkpoint flush, markers, watchdog bundles).
- `scd_headless --session-out DIR` drives that writer for tests. The app uses it always on. `tools/playtest/run.py --session DIR` verifies a folder.
- The core stays deterministic. Recording only reads state. The interpreter and translated paths are unchanged.

**Tech Stack:** C++20 (CMake, `scdcore`, `scd_headless`, SDL3 front-end, `std::filesystem`), CMake script mode for the build stamp, Python 3 (`.venv/bin/python`, stdlib `unittest`), mise tasks.

**Spec:** `docs/superpowers/specs/2026-10-16-session-recording-design.md` (builds on `docs/superpowers/specs/2026-10-05-playtest-harness-design.md` and the harness plan `docs/superpowers/plans/2026-10-05-playtest-harness.md`).

## Global Constraints

- Never commit disc images, BIOS dumps, extracted data, decompiled/disassembled listings, MAME captures, or generated translated C++. Sessions are disc-derived or personal (screenshots, RAM, save data): they live under `work/` (gitignored) or the per-user data directory and are never committed. Only `session.replay` and `session.ckpt` may be copied into `tools/playtest/replays/`. "The matching `.bram` is the user's save data: it stays local next to the replay and is never committed."
- Always build with Apple clang on macOS (`CC=/usr/bin/clang CXX=/usr/bin/clang++`). A cached CMake dir configured with the wrong compiler must be replaced, not reconfigured. (`engine/build-interp` and `engine/build` are already configured with `/usr/bin/clang++`. Re-running `cmake -S engine -B engine/build-interp` to pick up new targets is fine.)
- The safety hook blocks `rm -rf`. Use a fresh build directory or `mv`. Python tests use `Path.unlink()` / `tempfile`. C++ tests may use `std::filesystem::remove_all` on their own temp directories.
- Python tools run with `.venv/bin/python`.
- Edit files with the Edit tool. Do not use `sed -i` (it is not portable on macOS).
- Stage files by explicit path. Never `git add -A` or `git add .`.
- Determinism: "input must be applied at frame start from recorded state only"; "the replay frame counter is `System::frame_count()`". Recording only reads state. The interpreter and translated paths must stay unchanged.
- "Recording never stops play: any write failure is logged once per file and that file is dropped; the session continues with the rest." "If the directory cannot be created, the app prints an error and continues without a session."
- Portability: the Windows cross build (`tools/package/windows_cross.sh`, mingw-w64), the CI builds (`.github/workflows/build.yml`: macOS universal 12+, MSYS2 MinGW-w64, Ubuntu 22.04 gcc) and the shareable Mac build must still compile. Use `std::filesystem`, `std::localtime`/`std::strftime` and stdio only. Platform calls (`sysctlbyname`, `uname`) go behind `#if`. `%z` is not used: MSVCRT prints a zone name.
- `git_commit` must work in CI/release builds and outside a git checkout (`"unknown"`).
- In headless runs, build `--press` lists in bash (zsh does not word-split an unquoted `$VAR`).
- There is no linter. Correctness is judged by the tests in this plan and by determinism (`run.py --session`).

In shell snippets below, `CUE="sega-cd-eng/Snatcher (Sega CD) (U)-redump.cue"` (quote it; the disc is present in this worktree).

## Review Focus

- **The session's always-on log and its F8 markers read CPU state mid-run.** `cpu_pcs()` and `state_text()` switch Musashi contexts through `select_cpu`, and the heartbeat runs every 300 frames once the log is open. The headless replay opens no log and writes no markers. If any of this perturbs emulation, no session with a marker will verify. Expected: a session with the log, markers and gun input replays to byte-identical checkpoints. (Task 5 `test_session_replays_identically`.)
- **The app is killed mid-session (crash, force quit, power button).** Expected: `manifest.json` has `ended: null`, `session.replay`/`session.ckpt` are valid up to the last flush (at most one second earlier), and `run.py --session` verifies up to the last checkpoint. (Task 5 `test_session_survives_kill`, Task 6 `test_killed_session_verifies_to_last_checkpoint`.)
- **Backup RAM changes during the session.** The core rewrites `bram.bin` at shutdown and the game saves during play. Expected: `session.replay.bram` holds the boot state, its SHA-1 equals the replay header and the manifest, and verification with it passes. (Task 5 `test_session_copies_starting_bram`, Task 6 `test_recorded_session_reproduces`.)
- **A hang that recovers and then stalls again, or a stall that never ends.** Expected: exactly one failure bundle per stall, play continues, and the second stall gets its own bundle. (Task 5 `HangLatch` block in `watchdog_test.cpp`, Task 5 `test_session_hang_writes_one_bundle`.)
- **An unwritable file inside the session folder, or an uncreatable folder.** Expected: that file is dropped with one log line, everything else is still recorded and play goes on. An uncreatable folder means no session and no crash. (Task 5 `test_unwritable_file_is_dropped`, Task 5 `create_session_dir` failure case in `session_test.cpp`.)

## File Structure

- Modify `engine/src/scd/replay.h`, `engine/src/scd/replay.cpp`: `GunState`, gun events, v2 format, incremental serialization.
- Modify `engine/src/scd/system.h`, `engine/src/scd/system.cpp`: gun record/apply in `run_frame`, `state_text()`, `cd_speed()`, `fb_hash()`.
- Create `engine/src/scd/checkpoint.h`, `engine/src/scd/checkpoint.cpp`: `Checkpointer`, `checkpoint_line` (shared `--ckpt-out` format).
- Create `engine/src/scd/debug_dump.h`, `engine/src/scd/debug_dump.cpp`: `write_ppm`, RAM dumps, `write_failure_bundle`, `write_marker`.
- Modify `engine/src/scd/watchdog.h`: `HangLatch` (one report per stall).
- Create `engine/cmake/build_stamp.cmake`: writes `build_stamp.h` (git commit, dirty, build type).
- Create `engine/src/session/manifest.h`, `engine/src/session/manifest.cpp`: `Manifest`, `manifest_json`, `iso8601_local`, `host_os`.
- Create `engine/src/session/session.h`, `engine/src/session/session.cpp`: `Session`, `SessionConfig`, folder helpers.
- Modify `engine/src/frontend/headless.cpp`: `--justifier`, `--gun`, v2 replays, shared units, `--manifest-out`, `--session-out`, `--marker-at`, `SCD_TEST_KILL_AT`.
- Modify `engine/src/frontend/sdl_main.cpp`: always-on sessions, `--no-session`, `--session-dir`, F8/F10 markers and flash, `--record-input` alias, disc drop.
- Modify `engine/CMakeLists.txt`: new sources, `scdsession`, `build_stamp` target, `checkpoint_test`, `session_test`.
- Create `engine/tests/checkpoint_test.cpp`, `engine/tests/session_test.cpp`. Modify `engine/tests/replay_test.cpp`, `engine/tests/watchdog_test.cpp`.
- Modify `tools/playtest/run.py` (`--session`), `tools/playtest/test_playtest.py`.
- Modify `mise.toml`, `tools/package/ci_package.sh`, `docs/PLAYTEST.md`, `docs/PLAYING.md`, `tools/package/PLAYTEST_README.txt`, `tools/package/PLAYTEST_README_PC.txt`, `CLAUDE.md`.

---

### Task 1: Replay v2 format (Justifier events)

**Files:**
- Modify: `engine/src/scd/replay.h` (whole file, 31 lines)
- Modify: `engine/src/scd/replay.cpp:107-180` (`Replay::add` through the end of `Replay::parse`)
- Test: `engine/tests/replay_test.cpp`

**Interfaces:**
- Consumes: the existing `scd::Replay`, `ReplayEvent`, `buttons_from_string`, `buttons_to_string`.
- Produces:
  - `struct scd::GunState { int x = 0, y = 0; bool inside = false; uint8_t buttons = 0; bool operator==(const GunState&) const = default; }` (buttons bit0 trigger, bit1 start, as `System::set_gun`)
  - `struct scd::GunEvent { uint64_t frame; GunState gun; }`
  - `std::string scd::gun_to_string(const GunState&)` gives `"143,87,in,T"`. Buttons are written as `T`, `S`, `TS` or `-`.
  - `bool scd::gun_from_string(const std::string&, GunState*)`
  - `Replay::version` (`int`, the parsed header version, default 2), `Replay::gun_events` (`std::vector<GunEvent>`)
  - `void Replay::add_gun(uint64_t frame, const GunState&)` (dedups like `add`), `GunState Replay::gun_at(uint64_t frame) const` (default `GunState{}` before the first event)
  - `std::string Replay::serialize_header() const`, `std::string Replay::serialize_events(size_t pad_from, size_t gun_from) const`, with `serialize() == serialize_header() + serialize_events(0, 0)`. `serialize()` always writes `# snatcher-replay v2`.
  - Line format: `g0x<hex frame> <x>,<y>,<in|out>,<T|S|TS|->`. Pad lines are unchanged. On a frame tie the pad line comes first.

- [ ] **Step 1: Write the failing tests**

Add to `engine/tests/replay_test.cpp`, before `int main()`:

```cpp
static void test_v2_gun_roundtrip() {
    Replay r;
    r.justifier = true;
    r.add(0x10, kStart);
    r.add_gun(0x10, {143, 87, true, 1});
    r.add_gun(0x20, {-3, 0, false, 0});
    r.add_gun(0x21, {-3, 0, false, 0});   // repeat of the current state: not stored
    r.add_gun(0x30, {5, 6, true, 3});
    const std::string text = r.serialize();
    assert(text.rfind("# snatcher-replay v2\n", 0) == 0);
    assert(text.find("justifier on\n") != std::string::npos);
    assert(text.find("@0x10 S\ng0x10 143,87,in,T\ng0x20 -3,0,out,-\ng0x30 5,6,in,TS\n") != std::string::npos);
    Replay p;
    std::string err;
    assert(Replay::parse(text, &p, &err));
    assert(p.version == 2 && p.justifier && p.gun_events.size() == 3);
    assert(p.gun_events[0].frame == 0x10 && p.gun_events[0].gun == (GunState{143, 87, true, 1}));
    assert(p.serialize() == text);
}

static void test_gun_at_boundaries() {
    Replay r;
    r.add_gun(10, {1, 2, true, 1});
    r.add_gun(20, {3, 4, false, 0});
    assert(r.gun_at(0) == GunState{});
    assert(r.gun_at(9) == GunState{});
    assert(r.gun_at(10) == (GunState{1, 2, true, 1}));
    assert(r.gun_at(19) == (GunState{1, 2, true, 1}));
    assert(r.gun_at(20) == (GunState{3, 4, false, 0}));
    assert(r.gun_at(1u << 30) == (GunState{3, 4, false, 0}));
    Replay empty;
    assert(empty.gun_at(5) == GunState{});
}

static void test_v1_parses_unchanged() {
    Replay p;
    std::string err;
    assert(Replay::parse("# snatcher-replay v1\ndisc sha1=abc\nbram sha1=def\nengine x y\ncdspeed 2\njustifier on\n@0x10 U\n", &p, &err));
    assert(p.version == 1 && p.disc_sha1 == "abc" && p.bram_sha1 == "def" && p.engine == "x y" && p.cd_speed == 2.0 && p.justifier);
    assert(p.events.size() == 1 && p.events[0].buttons == kUp && p.gun_events.empty());
}

static void test_gun_parse_errors() {
    Replay p;
    std::string err;
    assert(!Replay::parse("# snatcher-replay v1\ng0x10 1,2,in,T\n", &p, &err));   // gun lines need v2
    assert(err.find("line 2") != std::string::npos && err.find("v2") != std::string::npos);
    const char* bad[] = {"g0x10 1,2,in", "g0x10 1,2,inside,T", "g0x10 1,2,in,X", "g0x10 a,2,in,T", "g0x10 1,,in,T",
                         "g0x10 1,2,in,", "g0x10 1,2,in,T,5", "gx10 1,2,in,T", "g0x10"};
    for (const char* b : bad) {
        assert(!Replay::parse(std::string("# snatcher-replay v2\ndisc sha1=\n") + b + "\n", &p, &err));
        assert(err.find("line 3") != std::string::npos);
    }
    assert(!Replay::parse("# snatcher-replay v2\ng0x20 1,2,in,T\ng0x10 1,2,out,-\n", &p, &err));   // unsorted
    assert(err.find("line 3") != std::string::npos);
}

static void test_incremental_events_concatenate() {
    Replay r;
    r.add(5, kUp);
    r.add_gun(5, {1, 1, true, 0});
    r.add(9, 0);
    const std::string first = r.serialize_events(0, 0);
    const size_t np = r.events.size(), ng = r.gun_events.size();
    r.add_gun(12, {2, 2, true, 1});
    r.add(12, kC);
    r.add(15, 0);
    assert(r.serialize_header() + first + r.serialize_events(np, ng) == r.serialize());
    assert(r.serialize_events(r.events.size(), r.gun_events.size()).empty());
}
```

Then add the calls in `main()`, after `test_sha1();`:

```cpp
    test_v2_gun_roundtrip();
    test_gun_at_boundaries();
    test_v1_parses_unchanged();
    test_gun_parse_errors();
    test_incremental_events_concatenate();
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake -S engine -B engine/build-interp -DCMAKE_BUILD_TYPE=Release && cmake --build engine/build-interp -j --target replay_test`
Expected: compile error, `GunState` / `add_gun` not declared.

- [ ] **Step 3: Implement**

Replace `engine/src/scd/replay.h` with:

```cpp
// Input replay: pad buttons (and, from v2, the Konami Justifier) held from which boot frame (System::frame_count()).
// Text format, no disc data (docs/PLAYTEST.md "Replay format").
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scd {

struct ReplayEvent { uint64_t frame; uint16_t buttons; };

// Justifier on port 2: x,y in game screen pixels, inside = aimed at the screen, buttons bit0 trigger, bit1 start.
struct GunState {
    int x = 0, y = 0;
    bool inside = false;
    uint8_t buttons = 0;
    bool operator==(const GunState&) const = default;
};
struct GunEvent { uint64_t frame; GunState gun; };

uint16_t buttons_from_string(const std::string& s);
std::string buttons_to_string(uint16_t b);
// "143,87,in,T" <-> GunState. Buttons: T (trigger), S (start), TS, or - for none.
std::string gun_to_string(const GunState& g);
bool gun_from_string(const std::string& s, GunState* out);

struct Replay {
    int version = 2;   // header version parse() read; serialize() always writes v2
    std::string disc_sha1, engine, bram_sha1 = "none";
    double cd_speed = 1.0;
    bool justifier = false;
    std::vector<ReplayEvent> events;    // sorted by frame; buttons are held from `frame` until the next event
    std::vector<GunEvent> gun_events;   // v2, sorted by frame; held until the next event, GunState{} before the first

    void add(uint64_t frame, uint16_t buttons);          // ignores an event that repeats the current state
    void add_gun(uint64_t frame, const GunState& g);     // same, for the gun
    uint16_t buttons_at(uint64_t frame) const;
    GunState gun_at(uint64_t frame) const;
    std::string serialize() const;                       // serialize_header() + serialize_events(0, 0)
    std::string serialize_header() const;
    // Event lines for events[pad_from..] and gun_events[gun_from..], merged by frame (pad first on a tie). Appending
    // the output of successive calls (each from the previous end) reproduces serialize_events(0, 0).
    std::string serialize_events(size_t pad_from, size_t gun_from) const;
    static bool parse(const std::string& text, Replay* out, std::string* err);
};

// Lowercase hex SHA-1 of a file, or "none" when it cannot be opened.
std::string sha1_file(const std::string& path);
// SHA-1 of the first FILE named in a cue sheet (identity of a one-.bin disc; for split layouts, of track 1).
std::string disc_sha1_from_cue(const std::string& cue_path);

}  // namespace scd
```

In `engine/src/scd/replay.cpp`, replace everything from `void Replay::add(uint64_t frame, uint16_t buttons) {` (line 107) to the closing `}` of `Replay::parse` (line 180) with:

```cpp
std::string gun_to_string(const GunState& g) {
    std::string b;
    if (g.buttons & 1) b += 'T';
    if (g.buttons & 2) b += 'S';
    if (b.empty()) b = "-";
    return std::to_string(g.x) + "," + std::to_string(g.y) + "," + (g.inside ? "in" : "out") + "," + b;
}

bool gun_from_string(const std::string& s, GunState* out) {
    std::vector<std::string> f;
    for (size_t start = 0;;) {
        size_t c = s.find(',', start);
        f.push_back(s.substr(start, c == std::string::npos ? std::string::npos : c - start));
        if (c == std::string::npos) break;
        start = c + 1;
    }
    if (f.size() != 4) return false;
    GunState g;
    for (int i = 0; i < 2; ++i) {
        const std::string& v = f[i];
        if (v.empty() || !(std::isdigit(static_cast<unsigned char>(v[0])) || (v[0] == '-' && v.size() > 1))) return false;
        char* end;
        long n = std::strtol(v.c_str(), &end, 10);
        if (*end || n < -32768 || n > 32767) return false;
        (i ? g.y : g.x) = int(n);
    }
    if (f[2] == "in") g.inside = true;
    else if (f[2] != "out") return false;
    if (f[3] != "-") {
        if (f[3].empty()) return false;
        for (char c : f[3]) {
            if (c == 'T') g.buttons |= 1;
            else if (c == 'S') g.buttons |= 2;
            else return false;
        }
    }
    *out = g;
    return true;
}

void Replay::add(uint64_t frame, uint16_t buttons) {
    uint16_t cur = events.empty() ? 0 : events.back().buttons;
    if (buttons == cur) return;
    events.push_back({frame, buttons});
}

void Replay::add_gun(uint64_t frame, const GunState& g) {
    const GunState cur = gun_events.empty() ? GunState{} : gun_events.back().gun;
    if (g == cur) return;
    gun_events.push_back({frame, g});
}

uint16_t Replay::buttons_at(uint64_t frame) const {
    auto it = std::upper_bound(events.begin(), events.end(), frame, [](uint64_t f, const ReplayEvent& e) { return f < e.frame; });
    return it == events.begin() ? 0 : std::prev(it)->buttons;
}

GunState Replay::gun_at(uint64_t frame) const {
    auto it = std::upper_bound(gun_events.begin(), gun_events.end(), frame, [](uint64_t f, const GunEvent& e) { return f < e.frame; });
    return it == gun_events.begin() ? GunState{} : std::prev(it)->gun;
}

std::string Replay::serialize_header() const {
    std::ostringstream o;
    o << "# snatcher-replay v2\n";
    o << "disc sha1=" << disc_sha1 << "\n";
    o << "bram sha1=" << bram_sha1 << "\n";
    o << "engine " << engine << "\n";
    o << "cdspeed " << cd_speed << "\n";
    o << "justifier " << (justifier ? "on" : "off") << "\n";
    return o.str();
}

std::string Replay::serialize_events(size_t pad_from, size_t gun_from) const {
    std::string o;
    char line[96];
    size_t p = pad_from, g = gun_from;
    while (p < events.size() || g < gun_events.size()) {
        const bool pad = g >= gun_events.size() || (p < events.size() && events[p].frame <= gun_events[g].frame);
        if (pad) {
            std::snprintf(line, sizeof line, "@0x%llx %s\n", (unsigned long long)events[p].frame, buttons_to_string(events[p].buttons).c_str());
            ++p;
        } else {
            std::snprintf(line, sizeof line, "g0x%llx %s\n", (unsigned long long)gun_events[g].frame, gun_to_string(gun_events[g].gun).c_str());
            ++g;
        }
        o += line;
    }
    return o;
}

std::string Replay::serialize() const { return serialize_header() + serialize_events(0, 0); }

bool Replay::parse(const std::string& text, Replay* out, std::string* err) {
    auto fail = [&](int line, const std::string& m) { if (err) *err = "line " + std::to_string(line) + ": " + m; return false; };
    std::istringstream in(text);
    std::string line;
    int n = 0;
    Replay r;
    int version = 0;
    while (std::getline(in, line)) {
        ++n;
        if (!line.empty() && line.back() == '\r') line.pop_back();   // CRLF checkouts / Windows-written files
        if (line.empty()) continue;
        if (line[0] == '#') {
            if (line == "# snatcher-replay v1") version = 1;
            else if (line == "# snatcher-replay v2") version = 2;
            else return fail(n, "unsupported or malformed header");
            continue;
        }
        if (!version) return fail(n, "missing '# snatcher-replay v1' or 'v2' header");
        if (line[0] == '@' || line[0] == 'g') {
            const bool gun = line[0] == 'g';
            if (gun && version < 2) return fail(n, "gun events need a v2 replay");
            char* end;
            if (line.size() < 2 || !std::isdigit(static_cast<unsigned char>(line[1]))) return fail(n, "bad frame");
            unsigned long long f = std::strtoull(line.c_str() + 1, &end, 0);
            if (*end != ' ') return fail(n, "bad frame");
            std::string b = end + 1;
            if (gun) {
                GunState g;
                if (!gun_from_string(b, &g)) return fail(n, "bad gun state (expected x,y,in|out,T|S|TS|-)");
                if (!r.gun_events.empty() && f <= r.gun_events.back().frame) return fail(n, "gun frames must be strictly increasing");
                r.gun_events.push_back({f, g});
                continue;
            }
            if (b.empty()) return fail(n, "missing buttons");
            if (b != "-")
                for (char c : b) if (buttons_from_string(std::string(1, c)) == 0) return fail(n, std::string("unknown button '") + c + "'");
            if (!r.events.empty() && f <= r.events.back().frame) return fail(n, "frames must be strictly increasing");
            r.events.push_back({f, buttons_from_string(b)});
            continue;
        }
        std::istringstream ls(line);
        std::string key, val;
        ls >> key;
        std::getline(ls, val);
        if (!val.empty() && val[0] == ' ') val.erase(0, 1);
        if (key == "disc" || key == "bram") {
            if (val.rfind("sha1=", 0) != 0) return fail(n, "expected sha1=...");
            (key == "disc" ? r.disc_sha1 : r.bram_sha1) = val.substr(5);
        } else if (key == "engine") r.engine = val;
        else if (key == "cdspeed") r.cd_speed = std::atof(val.c_str());
        else if (key == "justifier") r.justifier = val == "on";
        else return fail(n, "unknown key '" + key + "'");
    }
    if (!version) return fail(1, "missing '# snatcher-replay v1' or 'v2' header");
    r.version = version;
    *out = std::move(r);
    return true;
}
```

The `"bad frame"` check comes before the gun state parse, so `g0x10` (no space) and `gx10 ...` both fail with `bad frame` on their own line.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build engine/build-interp -j --target replay_test && engine/build-interp/replay_test`
Expected: `replay_test: ok`. The existing `test_roundtrip`, `test_parse_errors` and `test_parse_accepts_crlf_and_rejects_negative_frame` still pass: v1 input is accepted, and `# snatcher-replay v9` is still rejected.

- [ ] **Step 5: Commit**

```bash
git add engine/src/scd/replay.h engine/src/scd/replay.cpp engine/tests/replay_test.cpp
git commit -m "playtest: replay v2 with Justifier gun events"
```

---

### Task 2: Gun input through the replay hooks; `scd_headless --justifier`/`--gun` and v2 replays

**Files:**
- Modify: `engine/src/scd/system.h:57-59` (replay hook comment)
- Modify: `engine/src/scd/system.cpp:677-678` (`run_frame` replay hooks)
- Modify: `engine/src/frontend/headless.cpp` (usage comment line 3, `struct Press` line 50, options lines 89-112, replay checks line 135, before `sys.init` line 139, trace block lines 147-152, frame loop line 194)
- Modify: `docs/PLAYTEST.md` (new "Replay format" section)
- Test: `tools/playtest/test_playtest.py`

**Interfaces:**
- Consumes: Task 1 `GunState`, `Replay::gun_at`, `Replay::add_gun`, `Replay::version`, `gun_from_string`.
- Produces:
  - `System::run_frame()`: with `set_replay_play(r)` and `r->justifier`, it applies `r->gun_at(frames_)` through `set_gun` at frame start. With `set_replay_record(r)` and the gun connected, it calls `r->add_gun(frames_, current gun)` per frame.
  - `scd_headless` flags `--justifier` (connect the gun before boot) and `--gun FRAME:X,Y,in|out,T|S|TS|-`. `--gun` is repeatable, in frame order, and the state lasts from FRAME on. A bad value exits 2 with `bad --gun`.
  - `scd_headless --replay`: a v2 replay with `justifier on` connects the gun. A v1 replay with `justifier on` exits 4, and the message contains `gun`.

- [ ] **Step 1: Write the failing tests**

In `tools/playtest/test_playtest.py`, add after the `needs_disc = ...` line (line 6):

```python
needs_headless = unittest.skipUnless(pathlib.Path(HEADLESS).exists(), "scd_headless not built")
```

Add this class after `class TestReplayRefusal` (before `class TestWatch`):

```python
class TestGun(unittest.TestCase):
    @needs_headless
    def test_bad_gun_option_rejected(self):
        r = run_headless("--frames", "5", "--justifier", "--gun", "10:1,2,sideways,T")
        self.assertEqual(r.returncode, 2, r.stderr)
        self.assertIn("bad --gun", r.stderr)

    @needs_disc
    def test_v2_justifier_replay_accepted(self):
        out = ROOT / "work" / "pt_gun"
        out.mkdir(parents=True, exist_ok=True)
        rp = out / "v2.replay"
        rp.write_text("# snatcher-replay v2\ndisc sha1=\nbram sha1=none\njustifier on\ng0x10 100,80,in,T\n")
        r = run_headless("--frames", "30", "--out", str(out), "--replay", str(rp))
        self.assertEqual(r.returncode, 0, r.stderr)

    @needs_disc
    def test_gun_replay_matches_live_gun(self):
        # --gun sets the gun before run_frame (as the app does); the replay applies it at frame start: same frames, same state.
        out = ROOT / "work" / "pt_gun"
        out.mkdir(parents=True, exist_ok=True)
        a, b, rp = out / "live.ckpt", out / "replayed.ckpt", out / "gun.replay"
        r = run_headless("--frames", "1200", "--out", str(out), "--ckpt-out", str(a), "--justifier",
                         "--gun", "300:100,80,in,T", "--gun", "306:100,80,in,-", "--gun", "700:20,30,out,S", "--gun", "706:20,30,out,-")
        self.assertEqual(r.returncode, 0, r.stderr)
        rp.write_text("# snatcher-replay v2\ndisc sha1=\nbram sha1=none\njustifier on\n"
                      "g0x12c 100,80,in,T\ng0x132 100,80,in,-\ng0x2bc 20,30,out,S\ng0x2c2 20,30,out,-\n")
        r = run_headless("--frames", "1200", "--out", str(out), "--ckpt-out", str(b), "--replay", str(rp))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(a.read_text(), b.read_text())

    @needs_disc
    def test_watch_trace_records_gun(self):
        out = ROOT / "work" / "pt_gun_hang"
        out.mkdir(parents=True, exist_ok=True)
        for f in out.glob("*"):
            f.unlink()
        r = run_headless("--frames", "100", "--out", str(out), "--watch", "--hang-frames", "30", "--justifier",
                         "--gun", "10:100,80,in,T", env={"SCD_TEST_STICK_E020": "20"})
        self.assertEqual(r.returncode, 3, r.stderr)
        text = (out / "repro.replay").read_text()
        self.assertTrue(text.startswith("# snatcher-replay v2\n"), text)
        self.assertIn("justifier on\n", text)
        self.assertIn("g0xa 100,80,in,T\n", text)
```

The hex frames are 300 = `0x12c`, 306 = `0x132`, 700 = `0x2bc`, 706 = `0x2c2` and 10 = `0xa`. The existing `test_justifier_replay_refused` (a v1 replay with `justifier on`, expects exit 4 and "gun") stays as it is.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build engine/build-interp -j --target scd_headless && SCD_HEADLESS=engine/build-interp/scd_headless .venv/bin/python tools/playtest/test_playtest.py TestGun TestReplayRefusal -v`
Expected:
- `test_v2_justifier_replay_accepted` fails (exit 4).
- `test_bad_gun_option_rejected` fails. `--gun` is unknown and is taken as nothing, so the run exits 0.
- `test_watch_trace_records_gun` fails: no `g0xa` line.
- `test_gun_replay_matches_live_gun` fails: the first run cannot connect the gun and the replay is refused.
- The `TestReplayRefusal` tests pass.

- [ ] **Step 3: Record and apply the gun in `System::run_frame`**

In `engine/src/scd/system.h`, replace:

```cpp
    // Replay hooks (run_frame start): play overrides port 1's pad, record logs port 1's pad per emulated frame.
```

with:

```cpp
    // Replay hooks (run_frame start): play overrides port 1's pad (and the gun, for a replay with justifier on); record
    // logs port 1's pad, and the gun state while the gun is connected, per emulated frame.
```

In `engine/src/scd/system.cpp`, replace (lines 677-678):

```cpp
    if (replay_play_) set_pad(0, replay_play_->buttons_at(frames_));
    if (replay_rec_) replay_rec_->add(frames_, pad_[0]);
```

with:

```cpp
    if (replay_play_) {
        set_pad(0, replay_play_->buttons_at(frames_));
        if (replay_play_->justifier) {
            const GunState g = replay_play_->gun_at(frames_);
            set_gun(g.x, g.y, g.inside, g.buttons);
        }
    }
    if (replay_rec_) {
        replay_rec_->add(frames_, pad_[0]);
        if (gun_connected_) replay_rec_->add_gun(frames_, {gun_x_, gun_y_, gun_inside_, gun_buttons_});
    }
```

- [ ] **Step 4: `scd_headless` options and v2 replays**

In `engine/src/frontend/headless.cpp`:

Replace the usage comment line

```cpp
//                [--press FRAME:BUTTONS:HOLD ...] [--replay FILE [--bram FILE]] [--ckpt-out FILE [--ckpt-every N]] [--watch [--hang-frames N]]
```

with

```cpp
//                [--press FRAME:BUTTONS:HOLD ...] [--replay FILE [--bram FILE]] [--ckpt-out FILE [--ckpt-every N]] [--watch [--hang-frames N]]
//                [--justifier [--gun FRAME:X,Y,in|out,T|S|TS|- ...]]   (gun state from FRAME on, entries in frame order)
```

Replace `struct Press { int frame; uint16_t buttons; int hold; };` with:

```cpp
struct Press { int frame; uint16_t buttons; int hold; };
struct GunPress { uint64_t frame; GunState gun; };
```

Replace `    bool watch = false;` with `    bool watch = false, justifier = false;`. Replace `    std::vector<Press> presses;` with:

```cpp
    std::vector<Press> presses;
    std::vector<GunPress> guns;
```

In the option loop, replace

```cpp
        else if (a == "--hang-frames" && i + 1 < argc) hang_frames = std::atoi(argv[++i]);
```

with

```cpp
        else if (a == "--hang-frames" && i + 1 < argc) hang_frames = std::atoi(argv[++i]);
        else if (a == "--justifier") justifier = true;
        else if (a == "--gun" && i + 1 < argc) {
            const std::string g = argv[++i];
            const size_t colon = g.find(':');
            GunState gs;
            if (colon == std::string::npos || colon == 0 || !gun_from_string(g.substr(colon + 1), &gs)) {
                std::fprintf(stderr, "error: bad --gun %s (expected FRAME:X,Y,in|out,T|S|TS|-)\n", g.c_str());
                return 2;
            }
            guns.push_back({std::strtoull(g.c_str(), nullptr, 10), gs});
        }
```

Replace the v1 refusal (line 135)

```cpp
        if (replay.justifier) { std::fprintf(stderr, "error: replay was recorded with the Justifier; gun input is not recorded, so it cannot be replayed\n"); return 4; }
```

with

```cpp
        if (replay.justifier && replay.version < 2) { std::fprintf(stderr, "error: replay was recorded with the Justifier in format v1, which has no gun input, so it cannot be replayed\n"); return 4; }
        if (replay.justifier) justifier = true;   // v2: System::run_frame applies the recorded gun state
```

Replace

```cpp
    if (!sys.init(std::move(disc), bram, &err)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
```

with

```cpp
    if (justifier) sys.set_gun_connected(true);   // before the first frame: the game reads the controller ID at boot
    if (!sys.init(std::move(disc), bram, &err)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
```

In the `if (watch) {` trace block, replace `        trace.bram_sha1 = bram_sha_at_start;` with:

```cpp
        trace.bram_sha1 = bram_sha_at_start;
        trace.justifier = justifier;
```

In the frame loop, replace `        sys.set_pad(0, pad);` with:

```cpp
        sys.set_pad(0, pad);
        if (justifier && !guns.empty()) {   // --gun: the last entry whose frame has been reached
            GunState g;
            for (auto& gp : guns) if (uint64_t(f) >= gp.frame) g = gp.gun;
            sys.set_gun(g.x, g.y, g.inside, g.buttons);
        }
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build engine/build-interp -j --target scd_headless replay_test && engine/build-interp/replay_test && SCD_HEADLESS=engine/build-interp/scd_headless .venv/bin/python tools/playtest/test_playtest.py -v`
Expected: all tests pass, including the existing `test_justifier_replay_refused` and `test_replay_matches_press`.

Run: `tools/test_intro_skip.sh engine/build-interp/scd_headless`
Expected: it reports the Junker HQ scene (PASS). The pad path is unchanged.

- [ ] **Step 6: Document the format**

In `docs/PLAYTEST.md`, insert before `## Checkpoints`:

````markdown
## Replay format

Header `# snatcher-replay v2` (v1 files still load), then `disc sha1=`, `bram sha1=`, `engine`, `cdspeed`, `justifier on|off`, then events. Each event holds until the next one of its kind:

```
@0x1a2b UCS                pad buttons from this boot frame (U D L R B C A S, - for none)
g0x1a2b 143,87,in,T        Justifier from this frame: x,y in screen pixels, in|out of the screen, T trigger / S start / TS / -
```

`scd_headless --replay` connects the Justifier for a v2 replay with `justifier on`. A v1 replay with `justifier on` is refused (exit 4) because it has no gun input. `scd_headless --justifier [--gun F:X,Y,in|out,BTNS ...]` drives the gun directly, with entries in frame order.

````

- [ ] **Step 7: Commit**

```bash
git add engine/src/scd/system.h engine/src/scd/system.cpp engine/src/frontend/headless.cpp tools/playtest/test_playtest.py docs/PLAYTEST.md
git commit -m "playtest: record and replay Justifier input (replay v2 in scd_headless)"
```

---

### Task 3: Shared `checkpoint` and `debug_dump` units (behaviour-preserving)

**Files:**
- Create: `engine/src/scd/checkpoint.h`, `engine/src/scd/checkpoint.cpp`
- Create: `engine/src/scd/debug_dump.h`, `engine/src/scd/debug_dump.cpp`
- Modify: `engine/src/scd/system.h` (`dump_state` declaration, lines 84-85), `engine/src/scd/system.cpp:1515-1530` (`System::dump_state`)
- Modify: `engine/src/frontend/headless.cpp` (remove local `write_ppm` lines 23-34 and `write_failure_bundle` lines 63-79; checkpoint lambda lines 153-163; uses at lines 205, 212-216, 267)
- Modify: `engine/CMakeLists.txt:39-40` (scdcore sources), after the `watchdog_test` target (lines 85-86)
- Modify: `mise.toml` (`test-playtest`), `tools/package/ci_package.sh` (disc-free tests), `docs/PLAYTEST.md` (Watchdog section)
- Test: `engine/tests/checkpoint_test.cpp`, `tools/playtest/test_playtest.py`

**Interfaces:**
- Consumes: `System::state_hash()`, `cpu_pcs()`, `frame_count()`, `e020/e022/e06c()`, `framebuffer()`, `main_ram()`, `prg_ram()`; `Replay::serialize()`; `WatchVerdict`.
- Produces:
  - `std::string scd::checkpoint_line(uint64_t frame, uint16_t e022, uint16_t e06c, const StateHash& h)` returns `"@0x<frame> scene=E022:%04x/E06C:%04x vram=%016llx ram=%016llx fb=%016llx\n"`.
  - `class scd::Checkpointer { explicit Checkpointer(int every = 300); bool due(uint64_t frame, uint16_t e022, uint16_t e06c); std::string emit(uint64_t frame, uint16_t e022, uint16_t e06c, const StateHash& h); std::string after_frame(System& sys); std::string final_line(System& sys); }`. An empty string means "nothing to write".
  - `std::string System::state_text()` is the exact text `dump_state()` logs. `dump_state()` now logs `state_text()`.
  - In `debug_dump.h`:
    - `bool scd::write_file(const std::string& path, const void* data, size_t len)`, `bool scd::write_text(const std::string& path, const std::string& text)`
    - `bool scd::write_ppm(const std::string& path, const uint32_t* fb, int w, int h)` (320-pixel stride, as before)
    - `bool scd::write_main_ram(const std::string& path, System&)` (`$FF0000`, 0x10000 bytes), `bool scd::write_sub_ram(const std::string& path, System&)` (PRG RAM `$7000`, 0x6000 bytes)
    - `const char* scd::verdict_name(WatchVerdict::Kind)` returns `"hang"`, `"halted"`, `"still"` or `"ok"`.
    - `bool scd::write_failure_bundle(System&, const std::string& dir, const Replay& trace, const WatchVerdict&)` writes `report.json`, `final.ppm`, `mainram.bin`, `subram.bin`, `repro.replay` and `state.txt`, then logs `dump_state()` and a `[watch]` line through `debug_log`.
    - `bool scd::write_marker(System&, const std::string& dir, const std::string& time_iso)` writes `marker.json` (`{"frame":N,"e020":"0x....","e022":"0x....","e06c":"0x....","main_pc":"0x......","sub_pc":"0x......","time":"..."}`), `screen.ppm`, `mainram.bin`, `subram.bin` and `state.txt`.

- [ ] **Step 1: Capture a baseline with the current `scd_headless`**

```bash
CUE="sega-cd-eng/Snatcher (Sega CD) (U)-redump.cue"
cmake --build engine/build-interp -j --target scd_headless
mkdir -p work/pt_base/hang_before work/pt_base/hang_after
engine/build-interp/scd_headless "$CUE" --frames 1500 --out work/pt_base --ckpt-out work/pt_base/before.ckpt --ckpt-every 100
SCD_TEST_STICK_E020=20 engine/build-interp/scd_headless "$CUE" --frames 100 --out work/pt_base/hang_before --watch --hang-frames 30; echo "exit $?"
SCD_DEBUG=1 engine/build-interp/scd_headless "$CUE" --frames 300 --out work/pt_base 2> work/pt_base/state_before.txt
```

Expected: `ran 1500 frames`, then `exit 3`, and `work/pt_base/hang_before/report.json` exists. `state_before.txt` ends with the `dump_state()` lines (`Main PC=...`, `Sub  PC=...`, `VDP regs: ...`, `CD: ...`, PCM channels) and `YM writes: ...`.

- [ ] **Step 2: Write the failing unit test**

Create `engine/tests/checkpoint_test.cpp`:

```cpp
#undef NDEBUG   // tests rely on assert even in Release builds
#include <cassert>
#include <cstdio>

#include "checkpoint.h"

using namespace scd;

int main() {
    assert(checkpoint_line(0x12c, 1, 0, {0x1, 0xab, 0xffffffffffffffffull}) ==
           "@0x12c scene=E022:0001/E06C:0000 vram=0000000000000001 ram=00000000000000ab fb=ffffffffffffffff\n");
    {   // scene changes and the every-N grid
        Checkpointer c(100);
        assert(c.due(1, 0, 0));      // first call: differs from the 0xFFFF sentinel, as scd_headless always did
        assert(!c.due(2, 0, 0));
        assert(c.due(3, 1, 0));      // $E022 changed
        assert(c.due(4, 1, 5));      // $E06C changed
        assert(!c.due(99, 1, 5));
        assert(c.due(100, 1, 5));    // grid
    }
    {   // every <= 0: scene changes only
        Checkpointer c(0);
        assert(c.due(1, 0, 0));
        assert(!c.due(300, 0, 0));
    }
    {   // one line per frame: the grid line and the exit line can land on the same frame
        Checkpointer c(100);
        const StateHash h{1, 2, 3};
        assert(!c.emit(100, 0, 0, h).empty());
        assert(c.emit(100, 0, 0, h).empty());
        assert(!c.emit(101, 0, 0, h).empty());
    }
    std::puts("checkpoint_test: ok");
    return 0;
}
```

In `engine/CMakeLists.txt`, after

```cmake
add_executable(watchdog_test tests/watchdog_test.cpp)
target_include_directories(watchdog_test PRIVATE src/scd)
```

add

```cmake

add_executable(checkpoint_test tests/checkpoint_test.cpp)
target_link_libraries(checkpoint_test PRIVATE scdcore)
```

- [ ] **Step 3: Run it to verify it fails**

Run: `cmake -S engine -B engine/build-interp -DCMAKE_BUILD_TYPE=Release && cmake --build engine/build-interp -j --target checkpoint_test`
Expected: compile error, `checkpoint.h` not found.

- [ ] **Step 4: Create the units**

`engine/src/scd/checkpoint.h`:

```cpp
// Playtest checkpoints: one line on every $E022/$E06C change and every N frames, in the scd_headless --ckpt-out format
// (docs/PLAYTEST.md). Shared by scd_headless and session recording so the format cannot drift.
#pragma once
#include <cstdint>
#include <string>

#include "system.h"

namespace scd {

// "@0x<frame> scene=E022:<e022>/E06C:<e06c> vram=<16 hex> ram=<16 hex> fb=<16 hex>\n"
std::string checkpoint_line(uint64_t frame, uint16_t e022, uint16_t e06c, const StateHash& h);

class Checkpointer {
public:
    explicit Checkpointer(int every = 300) : every_(every) {}
    // Once after every run_frame(): true when $E022/$E06C changed since the previous call, or frame is a multiple of every (> 0).
    bool due(uint64_t frame, uint16_t e022, uint16_t e06c);
    // The line for this frame, or "" when one was already emitted for it.
    std::string emit(uint64_t frame, uint16_t e022, uint16_t e06c, const StateHash& h);
    // Front-end helpers. after_frame: due() then emit() (hashing only when a line is written). final_line: emit() at exit
    // or before a failure bundle.
    std::string after_frame(System& sys);
    std::string final_line(System& sys);

private:
    int every_;
    uint16_t last22_ = 0xFFFF, last6c_ = 0xFFFF;
    uint64_t last_emitted_ = ~0ull;
};

}  // namespace scd
```

`engine/src/scd/checkpoint.cpp`:

```cpp
#include "checkpoint.h"

#include <cstdio>

namespace scd {

std::string checkpoint_line(uint64_t frame, uint16_t e022, uint16_t e06c, const StateHash& h) {
    char line[160];
    std::snprintf(line, sizeof line, "@0x%llx scene=E022:%04x/E06C:%04x vram=%016llx ram=%016llx fb=%016llx\n", (unsigned long long)frame,
                  e022, e06c, (unsigned long long)h.vram, (unsigned long long)h.ram, (unsigned long long)h.fb);
    return line;
}

bool Checkpointer::due(uint64_t frame, uint16_t e022, uint16_t e06c) {
    const bool changed = e022 != last22_ || e06c != last6c_;
    last22_ = e022;
    last6c_ = e06c;
    return changed || (every_ > 0 && frame % uint64_t(every_) == 0);
}

std::string Checkpointer::emit(uint64_t frame, uint16_t e022, uint16_t e06c, const StateHash& h) {
    if (frame == last_emitted_) return "";
    last_emitted_ = frame;
    return checkpoint_line(frame, e022, e06c, h);
}

std::string Checkpointer::after_frame(System& sys) {
    const uint64_t f = sys.frame_count();
    if (!due(f, sys.e022(), sys.e06c()) || f == last_emitted_) return "";
    return emit(f, sys.e022(), sys.e06c(), sys.state_hash());
}

std::string Checkpointer::final_line(System& sys) {
    const uint64_t f = sys.frame_count();
    if (f == last_emitted_) return "";
    return emit(f, sys.e022(), sys.e06c(), sys.state_hash());
}

}  // namespace scd
```

`engine/src/scd/debug_dump.h`:

```cpp
// Debug evidence on disk: screenshots, RAM dumps, watchdog failure bundles and user markers. Shared by scd_headless and
// session recording (docs/PLAYTEST.md).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "replay.h"
#include "system.h"
#include "watchdog.h"

namespace scd {

bool write_file(const std::string& path, const void* data, size_t len);
bool write_text(const std::string& path, const std::string& text);
// P6 image of the w x h visible area of a 320-pixel-stride framebuffer.
bool write_ppm(const std::string& path, const uint32_t* fb, int w, int h);
// Main RAM ($FF0000, 64 KB) and Sub PRG RAM $7000..$D000 (the windows state_hash() covers).
bool write_main_ram(const std::string& path, System& sys);
bool write_sub_ram(const std::string& path, System& sys);
const char* verdict_name(WatchVerdict::Kind k);   // "hang", "halted", "still", "ok"
// A watchdog trip, into the existing directory `dir`: report.json (kind, frame, $E020/$E022/$E06C, both PCs), final.ppm,
// mainram.bin, subram.bin, repro.replay (the input that led there) and state.txt. Logs dump_state() and a [watch] line.
// False if a file could not be written.
bool write_failure_bundle(System& sys, const std::string& dir, const Replay& trace, const WatchVerdict& v);
// A user marker (F8/F10), into the existing directory `dir`: marker.json, screen.ppm, mainram.bin, subram.bin, state.txt.
bool write_marker(System& sys, const std::string& dir, const std::string& time_iso);

}  // namespace scd
```

`engine/src/scd/debug_dump.cpp`:

```cpp
#include "debug_dump.h"

#include <cstdio>

namespace scd {

bool write_file(const std::string& path, const void* data, size_t len) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(data, 1, len, f) == len;
    return std::fclose(f) == 0 && ok;
}

bool write_text(const std::string& path, const std::string& text) { return write_file(path, text.data(), text.size()); }

bool write_ppm(const std::string& path, const uint32_t* fb, int w, int h) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint32_t p = fb[y * 320 + x];
            unsigned char rgb[3] = {uint8_t(p >> 16), uint8_t(p >> 8), uint8_t(p)};
            std::fwrite(rgb, 1, 3, f);
        }
    const bool ok = !std::ferror(f);
    return std::fclose(f) == 0 && ok;
}

bool write_main_ram(const std::string& path, System& sys) { return write_file(path, sys.main_ram(), 0x10000); }
bool write_sub_ram(const std::string& path, System& sys) { return write_file(path, sys.prg_ram() + 0x7000, 0x6000); }

const char* verdict_name(WatchVerdict::Kind k) {
    switch (k) {
        case WatchVerdict::kHang: return "hang";
        case WatchVerdict::kHalted: return "halted";
        case WatchVerdict::kStillScreen: return "still";
        default: return "ok";
    }
}

bool write_failure_bundle(System& sys, const std::string& dir, const Replay& trace, const WatchVerdict& v) {
    uint32_t mpc, spc;
    sys.cpu_pcs(&mpc, &spc);
    const char* kind = verdict_name(v.kind);
    char json[320];
    std::snprintf(json, sizeof json, "{\"kind\":\"%s\",\"frame\":%llu,\"since\":%llu,\"e020\":\"0x%04x\",\"e022\":\"0x%04x\",\"e06c\":\"0x%04x\",\"main_pc\":\"0x%06x\",\"sub_pc\":\"0x%06x\"}\n",
                  kind, (unsigned long long)sys.frame_count(), (unsigned long long)v.since, sys.e020(), sys.e022(), sys.e06c(), mpc, spc);
    bool ok = write_text(dir + "/report.json", json);
    ok &= write_ppm(dir + "/final.ppm", sys.framebuffer(), sys.width(), sys.height());
    ok &= write_main_ram(dir + "/mainram.bin", sys);
    ok &= write_sub_ram(dir + "/subram.bin", sys);
    ok &= write_text(dir + "/repro.replay", trace.serialize());
    ok &= write_text(dir + "/state.txt", sys.state_text());
    sys.dump_state();
    debug_log("[watch] %s at frame %llu (e020=%04x main_pc=%06x sub_pc=%06x); bundle in %s\n", kind, (unsigned long long)sys.frame_count(),
              sys.e020(), mpc, spc, dir.c_str());
    return ok;
}

bool write_marker(System& sys, const std::string& dir, const std::string& time_iso) {
    uint32_t mpc, spc;
    sys.cpu_pcs(&mpc, &spc);
    char json[320];
    std::snprintf(json, sizeof json, "{\"frame\":%llu,\"e020\":\"0x%04x\",\"e022\":\"0x%04x\",\"e06c\":\"0x%04x\",\"main_pc\":\"0x%06x\",\"sub_pc\":\"0x%06x\",\"time\":\"%s\"}\n",
                  (unsigned long long)sys.frame_count(), sys.e020(), sys.e022(), sys.e06c(), mpc, spc, time_iso.c_str());
    bool ok = write_text(dir + "/marker.json", json);
    ok &= write_ppm(dir + "/screen.ppm", sys.framebuffer(), sys.width(), sys.height());
    ok &= write_main_ram(dir + "/mainram.bin", sys);
    ok &= write_sub_ram(dir + "/subram.bin", sys);
    ok &= write_text(dir + "/state.txt", sys.state_text());
    return ok;
}

}  // namespace scd
```

In `engine/CMakeLists.txt`, replace

```cmake
  src/scd/disc.cpp src/scd/vdp.cpp src/scd/pcm.cpp src/scd/psg.cpp src/scd/system.cpp src/scd/replay.cpp)
```

with

```cmake
  src/scd/disc.cpp src/scd/vdp.cpp src/scd/pcm.cpp src/scd/psg.cpp src/scd/system.cpp src/scd/replay.cpp
  src/scd/checkpoint.cpp src/scd/debug_dump.cpp)
```

- [ ] **Step 5: `System::state_text()`**

In `engine/src/scd/system.h`, replace

```cpp
    // Prints PCs and key registers of both CPUs, the CD state and the PCM channels to the log.
    void dump_state();
```

with

```cpp
    // PCs and key registers of both CPUs, the CD state and the PCM channels (state.txt in markers and failure bundles).
    std::string state_text();
    // Prints state_text() to the log.
    void dump_state();
```

In `engine/src/scd/system.cpp`, replace the whole `void System::dump_state() { ... }` (lines 1515-1530) with:

```cpp
static void appendf(std::string& s, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0) s.append(buf, size_t(std::min(n, int(sizeof buf) - 1)));
}

std::string System::state_text() {
    std::string s;
    Cpu keep = cur_;
    for (int i = 0; i < 2; ++i) {
        select_cpu(Cpu(i));
        appendf(s, "%s PC=%06x SR=%04x SP=%06x D0=%08x A0=%08x A6=%08x\n", i ? "Sub " : "Main", reg_get(M68K_REG_PC) & 0xFFFFFF,
                reg_get(M68K_REG_SR), reg_get(M68K_REG_SP) & 0xFFFFFF, reg_get(M68K_REG_D0), reg_get(M68K_REG_A0), reg_get(M68K_REG_A6));
    }
    select_cpu(keep);
    appendf(s, "VDP regs:");
    for (int i = 0; i < 24; ++i) appendf(s, " %02x", vdp_.reg(i));
    appendf(s, "  ffef02=%02x%02x\n", main_ram_[0xef02], main_ram_[0xef03]);
    appendf(s, "sub_run=%d owner=%d mode1m=%d main_flag=%02x sub_flag=%02x int_mask=%04x ien2=%d prg_bank=%d\n", sub_run_, word_owner_, mode_1m_,
            main_flag_, sub_flag_, int_mask_, ien2_, prg_bank_);
    appendf(s, "CD: reading=%d lba=%u remaining=%u buf_valid=%d cdda=%d track=%d reads=%llu\n", cdc_reading_, cdc_lba_, cdc_remaining_, cdc_buf_valid_,
            cdda_playing_, cdda_track_, (unsigned long long)cd_reads_);
    s += pcm_.describe();
    return s;
}

void System::dump_state() { log("%s", state_text().c_str()); }
```

`system.cpp` already includes `<algorithm>` (for `std::min`), `<cstdarg>` and `<cstdio>`.

- [ ] **Step 6: Use the shared units in `scd_headless`**

In `engine/src/frontend/headless.cpp`:

Replace

```cpp
#include "recorder.h"
#include "replay.h"
#include "system.h"
#include "watchdog.h"
```

with

```cpp
#include "checkpoint.h"
#include "debug_dump.h"
#include "recorder.h"
#include "replay.h"
#include "system.h"
#include "watchdog.h"
```

Delete the local `void write_ppm(const std::string& path, const uint32_t* fb, int w, int h) { ... }` (lines 23-34, including the blank line after it). Then delete the comment `// Everything needed to look at a watchdog trip later: ...` and the local `void write_failure_bundle(...) { ... }` (lines 63-79). The calls `write_ppm(...)` and `write_failure_bundle(sys, out, trace, v)` now resolve to the `scd::` versions with the same arguments.

Replace

```cpp
    FILE* ckpt = nullptr;
    uint16_t last22 = 0xFFFF, last6c = 0xFFFF;
    if (!ckpt_path.empty() && !(ckpt = std::fopen(ckpt_path.c_str(), "w"))) { std::fprintf(stderr, "error: cannot open %s\n", ckpt_path.c_str()); return 1; }
    uint64_t last_emitted = ~0ull;
    auto emit_ckpt = [&]() {
        if (sys.frame_count() == last_emitted) return;   // the backstop and the exit line can land on the same frame
        last_emitted = sys.frame_count();
        StateHash h = sys.state_hash();
        std::fprintf(ckpt, "@0x%llx scene=E022:%04x/E06C:%04x vram=%016llx ram=%016llx fb=%016llx\n", (unsigned long long)sys.frame_count(),
                     sys.e022(), sys.e06c(), (unsigned long long)h.vram, (unsigned long long)h.ram, (unsigned long long)h.fb);
    };
```

with

```cpp
    FILE* ckpt = nullptr;
    if (!ckpt_path.empty() && !(ckpt = std::fopen(ckpt_path.c_str(), "w"))) { std::fprintf(stderr, "error: cannot open %s\n", ckpt_path.c_str()); return 1; }
    Checkpointer ckpts(ckpt_every);
```

Replace both occurrences (watchdog trip and normal exit) of

```cpp
if (ckpt) { emit_ckpt(); std::fclose(ckpt); }
```

with

```cpp
if (ckpt) { std::fputs(ckpts.final_line(sys).c_str(), ckpt); std::fclose(ckpt); }
```

Use the Edit tool's `replace_all`, which keeps the indentation.

Replace

```cpp
        if (ckpt) {
            bool changed = sys.e022() != last22 || sys.e06c() != last6c;
            if (changed || (ckpt_every > 0 && sys.frame_count() % ckpt_every == 0)) emit_ckpt();
            last22 = sys.e022(); last6c = sys.e06c();
        }
```

with

```cpp
        if (ckpt) std::fputs(ckpts.after_frame(sys).c_str(), ckpt);
```

- [ ] **Step 7: Add the bundle file to the end-to-end test**

In `tools/playtest/test_playtest.py`, in `test_hang_detected_writes_bundle`, replace

```python
        for name in ("report.json", "final.ppm", "mainram.bin", "subram.bin", "repro.replay"):
            self.assertTrue((out / name).exists(), name)
```

with

```python
        for name in ("report.json", "final.ppm", "mainram.bin", "subram.bin", "repro.replay", "state.txt"):
            self.assertTrue((out / name).exists(), name)
        self.assertTrue((out / "state.txt").read_text().startswith("Main PC="))
```

- [ ] **Step 8: Build, run the tests and compare with the baseline**

```bash
CUE="sega-cd-eng/Snatcher (Sega CD) (U)-redump.cue"
cmake --build engine/build-interp -j --target scd_headless replay_test watchdog_test checkpoint_test
engine/build-interp/checkpoint_test
engine/build-interp/scd_headless "$CUE" --frames 1500 --out work/pt_base --ckpt-out work/pt_base/after.ckpt --ckpt-every 100
SCD_TEST_STICK_E020=20 engine/build-interp/scd_headless "$CUE" --frames 100 --out work/pt_base/hang_after --watch --hang-frames 30; echo "exit $?"
SCD_DEBUG=1 engine/build-interp/scd_headless "$CUE" --frames 300 --out work/pt_base 2> work/pt_base/state_after.txt
cmp work/pt_base/before.ckpt work/pt_base/after.ckpt && echo ckpt-same
for f in report.json mainram.bin subram.bin repro.replay final.ppm; do cmp work/pt_base/hang_before/$f work/pt_base/hang_after/$f && echo "$f same"; done
ls work/pt_base/hang_after/state.txt
cmp work/pt_base/state_before.txt work/pt_base/state_after.txt && echo state-same
SCD_HEADLESS=engine/build-interp/scd_headless .venv/bin/python tools/playtest/test_playtest.py -v
```

Expected:
- `checkpoint_test: ok`, then `exit 3`.
- `ckpt-same` and `state-same` print, and each of the five bundle files prints `same`.
- `state.txt` exists.
- All Python tests pass.

`repro.replay` is the same because Task 2 already writes v2. Note that `state_before.txt` / `state_after.txt` include a `[watch]` line only for watch runs. The `SCD_DEBUG` run has none, so they compare byte for byte.

- [ ] **Step 9: Wire the test into mise and CI, document `state.txt`**

In `mise.toml` `[tasks.test-playtest]`, replace

```toml
  "cmake --build engine/build-interp -j --target scd_headless replay_test watchdog_test",
  "engine/build-interp/replay_test",
  "engine/build-interp/watchdog_test",
```

with

```toml
  "cmake --build engine/build-interp -j --target scd_headless replay_test watchdog_test checkpoint_test",
  "engine/build-interp/replay_test",
  "engine/build-interp/watchdog_test",
  "engine/build-interp/checkpoint_test",
```

In `tools/package/ci_package.sh`, replace

```bash
"$BUILD/replay_test"
"$BUILD/watchdog_test"
```

with

```bash
"$BUILD/replay_test"
"$BUILD/watchdog_test"
"$BUILD/checkpoint_test"
```

In `docs/PLAYTEST.md` (Watchdog section), replace `` `repro.replay` (the input that led there), and logs `dump_state()`. `` with `` `repro.replay` (the input that led there), `state.txt` (both CPUs, VDP, CD and PCM state), and logs the same state. ``

- [ ] **Step 10: Commit**

```bash
git add engine/src/scd/checkpoint.h engine/src/scd/checkpoint.cpp engine/src/scd/debug_dump.h engine/src/scd/debug_dump.cpp \
  engine/src/scd/system.h engine/src/scd/system.cpp engine/src/frontend/headless.cpp engine/CMakeLists.txt \
  engine/tests/checkpoint_test.cpp tools/playtest/test_playtest.py mise.toml tools/package/ci_package.sh docs/PLAYTEST.md
git commit -m "playtest: shared checkpoint and debug_dump units, state.txt in failure bundles"
```

---

### Task 4: Build stamp, manifest writer, `scd_headless --manifest-out`

**Files:**
- Create: `engine/cmake/build_stamp.cmake`
- Create: `engine/src/session/manifest.h`, `engine/src/session/manifest.cpp`
- Modify: `engine/CMakeLists.txt` (after `target_link_libraries(scdcore PUBLIC musashi nuked_opn2)` line 76; `scd_headless` link line 80; tests)
- Modify: `engine/src/scd/system.h:91` (add `cd_speed()` getter)
- Modify: `engine/src/frontend/headless.cpp` (option, `started` stamp, manifest at exit)
- Modify: `mise.toml`, `tools/package/ci_package.sh`
- Test: `engine/tests/session_test.cpp`, `tools/playtest/test_playtest.py`

**Interfaces:**
- Consumes: Task 3 `write_text`; `System::translated()`, `frame_count()`, `halted()`; `disc_sha1_from_cue`, `sha1_file`.
- Produces:
  - Generated `build_stamp.h` (in `<build>/stamp/`, private to `scdsession`) defines `SNATCHER_GIT_COMMIT` (a string: `git rev-parse --short HEAD`, else the first 7 characters of `$GITHUB_SHA`, else `"unknown"`), `SNATCHER_GIT_DIRTY` (0/1, from `git status --porcelain --untracked-files=no`) and `SNATCHER_BUILD_TYPE` (string).
  - `struct scd::Manifest { std::string app, build, os, sdl, renderer; std::vector<std::string> args; double cd_speed = 1.0; bool justifier = false; int ckpt_every = 300; std::string disc_sha1, bram_sha1 = "none", started, ended; bool has_frames = false; uint64_t frames = 0; std::string exit; int markers = 0, failures = 0; }`. In the output JSON, an empty `sdl`/`renderer`/`ended`/`exit` and `has_frames == false` are written as `null`.
  - `std::string scd::manifest_json(const Manifest&)`. Keys in order: `format, app, git_commit, git_dirty, build, build_type, os, sdl, renderer, args, cd_speed, justifier, ckpt_every, disc_sha1, bram_sha1, started, ended, frames, exit, markers, failures`. One key per line, written as `  "key": value`.
  - `std::string scd::json_escape(const std::string&)`, `std::string scd::iso8601_local(std::time_t)` (`2026-10-16T20:14:03-07:00`), `std::string scd::host_os()`, `const char* scd::build_git_commit()`, `bool scd::build_git_dirty()`.
  - `double System::cd_speed() const`.
  - `scd_headless --manifest-out FILE`: after a normal exit (not a watchdog trip), it writes a manifest for the run with `app` `"scd_headless"`, `exit` `"quit"` or `"halted"`, and `sdl`/`renderer` null.
  - The CMake library `scdsession` (STATIC, PUBLIC include `src/session`, links `scdcore`, depends on the `build_stamp` target).

- [ ] **Step 1: Write the failing unit test**

Create `engine/tests/session_test.cpp`:

```cpp
#undef NDEBUG   // tests rely on assert even in Release builds
#include <cassert>
#include <cstdio>
#include <ctime>
#include <string>

#include "manifest.h"

using namespace scd;

static bool has(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }

static void test_json_escape() {
    assert(json_escape("plain") == "plain");
    assert(json_escape("a\"b\\c") == "a\\\"b\\\\c");
    assert(json_escape("x\ny\t") == "x\\ny\\t");
    assert(json_escape(std::string("\x01", 1)) == "\\u0001");
}

static void test_manifest_open_session() {
    Manifest m;
    m.app = "snatcher";
    m.build = "interpreter";
    m.os = "TestOS 1";
    m.args = {"--justifier", "C:\\dir \"x\""};
    m.justifier = true;
    m.disc_sha1 = "abc";
    m.started = "2026-10-16T20:14:03-07:00";
    const std::string j = manifest_json(m);
    assert(j.front() == '{' && has(j, "\n}\n"));
    assert(has(j, "  \"format\": 1,\n"));
    assert(has(j, "\"app\": \"snatcher\","));
    assert(has(j, "\"git_commit\": \""));
    assert(has(j, "\"git_dirty\": true,") || has(j, "\"git_dirty\": false,"));
    assert(has(j, "\"build\": \"interpreter\","));
    assert(has(j, "\"build_type\": \""));
    assert(has(j, "\"sdl\": null,") && has(j, "\"renderer\": null,"));
    assert(has(j, "\"args\": [\"--justifier\", \"C:\\\\dir \\\"x\\\"\"],"));
    assert(has(j, "\"cd_speed\": 1.0,"));
    assert(has(j, "\"justifier\": true,"));
    assert(has(j, "\"ckpt_every\": 300,"));
    assert(has(j, "\"bram_sha1\": \"none\","));
    assert(has(j, "\"started\": \"2026-10-16T20:14:03-07:00\","));
    assert(has(j, "\"ended\": null,") && has(j, "\"frames\": null,") && has(j, "\"exit\": null,"));
    assert(has(j, "\"failures\": 0\n}"));
}

static void test_manifest_ended_session() {
    Manifest m;
    m.sdl = "3.4.10";
    m.cd_speed = 2.5;
    m.ended = "2026-10-16T21:00:00+00:00";
    m.has_frames = true;
    m.frames = 123456;
    m.exit = "quit";
    m.markers = 2;
    const std::string j = manifest_json(m);
    assert(has(j, "\"sdl\": \"3.4.10\","));
    assert(has(j, "\"cd_speed\": 2.5,"));
    assert(has(j, "\"ended\": \"2026-10-16T21:00:00+00:00\","));
    assert(has(j, "\"frames\": 123456,"));
    assert(has(j, "\"exit\": \"quit\","));
    assert(has(j, "\"markers\": 2,"));
}

static void test_iso8601_local() {
    const std::time_t t = 1792206843;   // 2026-10-17 03:14:03 UTC
    const std::string s = iso8601_local(t);
    assert(s.size() == 25);
    assert(s[4] == '-' && s[7] == '-' && s[10] == 'T' && s[13] == ':' && s[16] == ':');
    assert((s[19] == '+' || s[19] == '-') && s[22] == ':');
    char date[32];
    std::strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%S", std::localtime(&t));
    assert(s.compare(0, 19, date) == 0);
}

int main() {
    test_json_escape();
    test_manifest_open_session();
    test_manifest_ended_session();
    test_iso8601_local();
    assert(!host_os().empty());
    assert(std::string(build_git_commit()).size() > 0);
    std::puts("session_test: ok");
    return 0;
}
```

In `engine/CMakeLists.txt`, after the `checkpoint_test` lines from Task 3, add:

```cmake

add_executable(session_test tests/session_test.cpp)
target_link_libraries(session_test PRIVATE scdsession)
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake -S engine -B engine/build-interp -DCMAKE_BUILD_TYPE=Release && cmake --build engine/build-interp -j --target session_test`
Expected: compile error, `manifest.h` not found. `scdsession` does not exist yet, so CMake treats it as a plain `-lscdsession` and adds no include path.

- [ ] **Step 3: Build stamp script and the `scdsession` library**

Create `engine/cmake/build_stamp.cmake`:

```cmake
# Writes OUT (build_stamp.h) with the source tree's git commit for session manifests. Runs on every build (cmake -P);
# rewrites OUT only when the text changes, so nothing recompiles needlessly.
#   -DSRC=<repo root> -DOUT=<header> -DGIT=<git executable or empty> -DBUILD_TYPE=<CMAKE_BUILD_TYPE>
set(commit "unknown")
set(dirty 0)
if(GIT AND EXISTS "${GIT}")
  execute_process(COMMAND "${GIT}" -C "${SRC}" rev-parse --short HEAD
    OUTPUT_VARIABLE out RESULT_VARIABLE rc OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(rc EQUAL 0 AND NOT out STREQUAL "")
    set(commit "${out}")
    execute_process(COMMAND "${GIT}" -C "${SRC}" status --porcelain --untracked-files=no
      OUTPUT_VARIABLE st RESULT_VARIABLE rc2 ERROR_QUIET)
    if(rc2 EQUAL 0 AND NOT st STREQUAL "")
      set(dirty 1)
    endif()
  endif()
endif()
# CI checkouts where git refuses the directory (ownership checks) still know the commit.
if(commit STREQUAL "unknown" AND DEFINED ENV{GITHUB_SHA})
  string(SUBSTRING "$ENV{GITHUB_SHA}" 0 7 commit)
endif()
set(text "// Generated by engine/cmake/build_stamp.cmake. Do not edit.\n#pragma once\n#define SNATCHER_GIT_COMMIT \"${commit}\"\n#define SNATCHER_GIT_DIRTY ${dirty}\n#define SNATCHER_BUILD_TYPE \"${BUILD_TYPE}\"\n")
set(old "")
if(EXISTS "${OUT}")
  file(READ "${OUT}" old)
endif()
if(NOT old STREQUAL text)
  file(WRITE "${OUT}" "${text}")
endif()
```

`rev-parse --short` is used rather than `git describe`, which would print tag-relative names here (the repository has tag `v0.1.0-rc1`). Outside a git checkout, `rev-parse` fails and the commit stays `unknown`.

In `engine/CMakeLists.txt`, replace

```cmake
target_link_libraries(scdcore PUBLIC musashi nuked_opn2)
```

with

```cmake
target_link_libraries(scdcore PUBLIC musashi nuked_opn2)

# ---- Build stamp: the git commit in session manifests. Runs on every build; the header changes only with the commit or dirty state.
find_package(Git QUIET)
set(STAMP_DIR ${CMAKE_CURRENT_BINARY_DIR}/stamp)
add_custom_target(build_stamp
  COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/.. -DOUT=${STAMP_DIR}/build_stamp.h "-DGIT=${GIT_EXECUTABLE}"
          -DBUILD_TYPE=${CMAKE_BUILD_TYPE} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_stamp.cmake
  BYPRODUCTS ${STAMP_DIR}/build_stamp.h
  VERBATIM)

# ---- Session recording (manifest, session folders): shared by scd_headless and the app (docs/PLAYTEST.md "Sessions").
add_library(scdsession STATIC src/session/manifest.cpp)
target_include_directories(scdsession PUBLIC src/session PRIVATE ${STAMP_DIR})
target_link_libraries(scdsession PUBLIC scdcore)
add_dependencies(scdsession build_stamp)
```

Replace

```cmake
target_link_libraries(scd_headless PRIVATE scdcore)
```

with

```cmake
target_link_libraries(scd_headless PRIVATE scdcore scdsession)
```

- [ ] **Step 4: The manifest unit**

Create `engine/src/session/manifest.h`:

```cpp
// Session manifest (manifest.json): which build, settings and disc produced a session (docs/PLAYTEST.md "Sessions").
#pragma once
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace scd {

struct Manifest {
    std::string app;                  // "snatcher" or "scd_headless"
    std::string build;                // "translated" or "interpreter" (System::translated())
    std::string os, sdl, renderer;    // sdl / renderer "" are written as null (headless)
    std::vector<std::string> args;    // command line without the disc path
    double cd_speed = 1.0;
    bool justifier = false;
    int ckpt_every = 300;             // run.py --session replays with the same interval
    std::string disc_sha1, bram_sha1 = "none";
    std::string started, ended;       // iso8601_local(); ended "" is written as null (session still open, or the app died)
    bool has_frames = false;          // false: "frames": null
    uint64_t frames = 0;
    std::string exit;                 // "quit", "disc_changed", "halted"; "" is written as null
    int markers = 0, failures = 0;
};

// JSON, one key per line, with "format": 1 and git_commit / git_dirty / build_type from the build stamp.
std::string manifest_json(const Manifest& m);
std::string json_escape(const std::string& s);   // contents of a JSON string literal
std::string iso8601_local(std::time_t t);        // 2026-10-16T20:14:03-07:00
std::string host_os();                           // "macOS 27.2", "Linux 6.8.0-40-generic", "Windows"
const char* build_git_commit();                  // short hash, or "unknown" outside a git checkout
bool build_git_dirty();

}  // namespace scd
```

Create `engine/src/session/manifest.cpp`:

```cpp
#include "manifest.h"

#include <cstdio>

#include "build_stamp.h"   // generated by engine/cmake/build_stamp.cmake
#if defined(__APPLE__)
#include <sys/sysctl.h>
#elif !defined(_WIN32)
#include <sys/utsname.h>
#endif

namespace scd {

const char* build_git_commit() { return SNATCHER_GIT_COMMIT; }
bool build_git_dirty() { return SNATCHER_GIT_DIRTY != 0; }

std::string json_escape(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\u%04x", c);
                    o += b;
                } else {
                    o += char(c);
                }
        }
    }
    return o;
}

namespace {
std::string str(const std::string& s) { return "\"" + json_escape(s) + "\""; }
std::string str_or_null(const std::string& s) { return s.empty() ? "null" : str(s); }
}  // namespace

std::string manifest_json(const Manifest& m) {
    std::string args = "[";
    for (size_t i = 0; i < m.args.size(); ++i) args += (i ? ", " : "") + str(m.args[i]);
    args += "]";
    char speed[32];
    if (m.cd_speed == double(int64_t(m.cd_speed))) std::snprintf(speed, sizeof speed, "%.1f", m.cd_speed);
    else std::snprintf(speed, sizeof speed, "%g", m.cd_speed);
    std::string o = "{\n";
    auto field = [&o](const char* key, const std::string& value, bool last = false) {
        o += "  \"";
        o += key;
        o += "\": ";
        o += value;
        o += last ? "\n" : ",\n";
    };
    field("format", "1");
    field("app", str(m.app));
    field("git_commit", str(build_git_commit()));
    field("git_dirty", build_git_dirty() ? "true" : "false");
    field("build", str(m.build));
    field("build_type", str(SNATCHER_BUILD_TYPE));
    field("os", str(m.os));
    field("sdl", str_or_null(m.sdl));
    field("renderer", str_or_null(m.renderer));
    field("args", args);
    field("cd_speed", speed);
    field("justifier", m.justifier ? "true" : "false");
    field("ckpt_every", std::to_string(m.ckpt_every));
    field("disc_sha1", str(m.disc_sha1));
    field("bram_sha1", str(m.bram_sha1));
    field("started", str(m.started));
    field("ended", str_or_null(m.ended));
    field("frames", m.has_frames ? std::to_string(m.frames) : "null");
    field("exit", str_or_null(m.exit));
    field("markers", std::to_string(m.markers));
    field("failures", std::to_string(m.failures), true);
    o += "}\n";
    return o;
}

std::string iso8601_local(std::time_t t) {
    const std::tm lt = *std::localtime(&t);
    std::tm gt = *std::gmtime(&t);
    gt.tm_isdst = lt.tm_isdst;   // mktime reads the UTC wall clock as local time: the difference is the UTC offset
    const long off = long(std::difftime(t, std::mktime(&gt)));
    char date[32];
    std::strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%S", &lt);
    const long a = off < 0 ? -off : off;
    char tz[16];
    std::snprintf(tz, sizeof tz, "%c%02ld:%02ld", off < 0 ? '-' : '+', a / 3600, (a / 60) % 60);
    return std::string(date) + tz;
}

std::string host_os() {
#if defined(__APPLE__)
    char v[64] = {};
    size_t n = sizeof v - 1;
    if (sysctlbyname("kern.osproductversion", v, &n, nullptr, 0) == 0) return std::string("macOS ") + v;
    return "macOS";
#elif defined(_WIN32)
    return "Windows";
#else
    struct utsname u;
    if (uname(&u) == 0) return std::string(u.sysname) + " " + u.release;
    return "unknown";
#endif
}

}  // namespace scd
```

- [ ] **Step 5: Run the unit test**

Run: `cmake -S engine -B engine/build-interp -DCMAKE_BUILD_TYPE=Release && cmake --build engine/build-interp -j --target session_test && engine/build-interp/session_test && cat engine/build-interp/stamp/build_stamp.h`
Expected: `session_test: ok`. The header shows `SNATCHER_GIT_COMMIT` equal to `git rev-parse --short HEAD` and `SNATCHER_BUILD_TYPE "Release"`.

- [ ] **Step 6: Write the failing end-to-end test**

In `tools/playtest/test_playtest.py`, replace line 1 `import os, subprocess, sys, unittest, pathlib` with:

```python
import json, os, re, shutil, subprocess, sys, tempfile, unittest, pathlib
```

After `run_headless` (after its `return` line), add:

```python
def new_dir(prefix):
    (ROOT / "work").mkdir(exist_ok=True)
    return pathlib.Path(tempfile.mkdtemp(prefix=prefix, dir=ROOT / "work"))
```

Add this class before `A = "@0x64 ...`:

```python
class TestManifest(unittest.TestCase):
    @needs_disc
    def test_manifest_identifies_build(self):
        out = new_dir("pt_manifest_")
        mf = out / "manifest.json"
        r = run_headless("--frames", "60", "--out", str(out), "--manifest-out", str(mf), env={"SCD_NO_TRANSLATE": "1"})
        self.assertEqual(r.returncode, 0, r.stderr)
        m = json.loads(mf.read_text())
        self.assertEqual(m["format"], 1)
        self.assertEqual(m["app"], "scd_headless")
        self.assertEqual(m["build"], "interpreter")   # SCD_NO_TRANSLATE: interpreter on either build kind
        self.assertEqual(m["frames"], 60)
        self.assertEqual(m["exit"], "quit")
        self.assertIsNone(m["sdl"])
        self.assertEqual(m["bram_sha1"], "none")
        self.assertIsInstance(m["git_dirty"], bool)
        head = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--short", "HEAD"], capture_output=True, text=True)
        if head.returncode == 0:   # built from this checkout (and not committed since the build)
            self.assertEqual(m["git_commit"], head.stdout.strip())
```

- [ ] **Step 7: Run it to verify it fails**

Run: `cmake --build engine/build-interp -j --target scd_headless && SCD_HEADLESS=engine/build-interp/scd_headless .venv/bin/python tools/playtest/test_playtest.py TestManifest -v`
Expected: FAIL. `manifest.json` does not exist because `--manifest-out` is ignored.

- [ ] **Step 8: `System::cd_speed()` and `--manifest-out`**

In `engine/src/scd/system.h`, replace

```cpp
    void set_cd_speed(double x) { cd_speed_ = x < 1.0 ? 1.0 : x; }
```

with

```cpp
    void set_cd_speed(double x) { cd_speed_ = x < 1.0 ? 1.0 : x; }
    double cd_speed() const { return cd_speed_; }
```

In `engine/src/frontend/headless.cpp`:
- Add `#include <ctime>` after `#include <cstring>`.
- Add `#include "manifest.h"` after `#include "debug_dump.h"`.
- Replace `    std::string replay_path, ckpt_path, bram;` with `    std::string replay_path, ckpt_path, bram, manifest_path;`.
- Replace `        else if (a == "--bram" && i + 1 < argc) bram = argv[++i];` with:

```cpp
        else if (a == "--bram" && i + 1 < argc) bram = argv[++i];
        else if (a == "--manifest-out" && i + 1 < argc) manifest_path = argv[++i];
```

Replace

```cpp
    std::string cue = argv[1], out = ".", wav, video, codec = "h264";
```

with

```cpp
    std::string cue = argv[1], out = ".", wav, video, codec = "h264";
    const std::string started = iso8601_local(std::time(nullptr));
```

Replace the end of `main`

```cpp
    std::printf("ran %llu frames%s\n", (unsigned long long)sys.frame_count(), sys.halted() ? " (halted)" : "");
    write_census();
```

with

```cpp
    std::printf("ran %llu frames%s\n", (unsigned long long)sys.frame_count(), sys.halted() ? " (halted)" : "");
    if (!manifest_path.empty()) {   // run.py --session compares this build kind with the session's
        Manifest m;
        m.app = "scd_headless";
        m.build = sys.translated() ? "translated" : "interpreter";
        m.os = host_os();
        for (int i = 2; i < argc; ++i) m.args.push_back(argv[i]);
        m.cd_speed = sys.cd_speed();
        m.justifier = justifier;
        m.ckpt_every = ckpt_every;
        m.disc_sha1 = disc_sha1_from_cue(cue);
        m.bram_sha1 = bram_sha_at_start;
        m.started = started;
        m.ended = iso8601_local(std::time(nullptr));
        m.has_frames = true;
        m.frames = sys.frame_count();
        m.exit = sys.halted() ? "halted" : "quit";
        if (!write_text(manifest_path, manifest_json(m))) std::fprintf(stderr, "error: cannot write %s\n", manifest_path.c_str());
    }
    write_census();
```

Add `[--manifest-out FILE]` to the usage comment on line 3.

- [ ] **Step 9: Run the tests**

Run: `cmake --build engine/build-interp -j --target scd_headless session_test && engine/build-interp/session_test && SCD_HEADLESS=engine/build-interp/scd_headless .venv/bin/python tools/playtest/test_playtest.py -v`
Expected: `session_test: ok` and all Python tests pass, including `test_manifest_identifies_build`.

Check the fallbacks outside a git checkout by calling the script directly. An extracted copy under `work/` would not work: `git -C` would still find the enclosing repository.

```bash
cmake -DSRC=/nonexistent -DOUT=work/stamp_check.h -DGIT=$(command -v git) -DBUILD_TYPE=Release -P engine/cmake/build_stamp.cmake && cat work/stamp_check.h
GITHUB_SHA=0123456789abcdef cmake -DSRC=/nonexistent -DOUT=work/stamp_check.h -DGIT= -DBUILD_TYPE=Release -P engine/cmake/build_stamp.cmake && grep COMMIT work/stamp_check.h
```

Expected: first `#define SNATCHER_GIT_COMMIT "unknown"` with `#define SNATCHER_GIT_DIRTY 0`, then `#define SNATCHER_GIT_COMMIT "0123456"`.

- [ ] **Step 10: Wire the test into mise and CI**

In `mise.toml` `[tasks.test-playtest]`, replace

```toml
  "cmake --build engine/build-interp -j --target scd_headless replay_test watchdog_test checkpoint_test",
```

with

```toml
  "cmake --build engine/build-interp -j --target scd_headless replay_test watchdog_test checkpoint_test session_test",
```

and after `  "engine/build-interp/checkpoint_test",` add `  "engine/build-interp/session_test",`.

In `tools/package/ci_package.sh`, after `"$BUILD/checkpoint_test"` add `"$BUILD/session_test"`.

- [ ] **Step 11: Commit**

```bash
git add engine/cmake/build_stamp.cmake engine/src/session/manifest.h engine/src/session/manifest.cpp engine/src/scd/system.h \
  engine/src/frontend/headless.cpp engine/CMakeLists.txt engine/tests/session_test.cpp tools/playtest/test_playtest.py \
  mise.toml tools/package/ci_package.sh
git commit -m "playtest: build stamp and session manifest, scd_headless --manifest-out"
```

---

### Task 5: Session writer and `scd_headless --session-out`

**Files:**
- Create: `engine/src/session/session.h`, `engine/src/session/session.cpp`
- Modify: `engine/src/scd/watchdog.h` (add `HangLatch`)
- Modify: `engine/src/scd/system.h` (`fb_hash` after `state_hash`, line 79), `engine/src/scd/system.cpp` (`state_hash`, lines 1565-1577)
- Modify: `engine/CMakeLists.txt` (`scdsession` sources)
- Modify: `engine/src/frontend/headless.cpp` (options, `SCD_TEST_STICK_E020` refactor, `SCD_TEST_KILL_AT`, session begin/after_frame/end)
- Test: `engine/tests/watchdog_test.cpp`, `engine/tests/session_test.cpp`, `tools/playtest/test_playtest.py`

**Interfaces:**
- Consumes:
  - Task 1: `Replay`, `GunState`, `serialize_header`, `serialize_events`.
  - Task 3: `Checkpointer`, `write_failure_bundle`, `write_marker`, `write_text`, `verdict_name`.
  - Task 4: `Manifest`, `manifest_json`, `iso8601_local`, `host_os`, `System::cd_speed()`.
  - `System::open_log/close_log/set_replay_record/translated/frame_count/e020/halted`, `debug_log`.
- Produces:
  - `class scd::HangLatch { bool fire(const WatchVerdict& v, uint16_t e020); }`: true once per stall. It re-arms when the sampled `$E020` differs from the value it latched.
  - `uint64_t System::fb_hash() const`, which equals `state_hash().fb`.
  - `std::string scd::session_stamp(std::time_t)` gives `YYYYMMDD-HHMMSS` in local time.
  - `std::string scd::default_session_parent(const std::string& cwd, const std::string& pref)` gives `<cwd>/work/sessions` if `<cwd>/engine/src/scd` is a directory, else `pref + "sessions"`.
  - `std::string scd::create_session_dir(const std::string& parent, const std::string& stamp, std::string* err)` creates `<parent>/<stamp>`, else `<stamp>-1`, `-2` and so on. It returns `""` with `*err` set on failure.
  - `struct scd::SessionConfig { std::string cue, bram_path, engine; Manifest manifest; double cd_speed = 1.0; bool justifier = false; int ckpt_every = 300; int hang_frames = 300; uint64_t flush_ms = 1000; }`
  - `class scd::Session` (non-copyable):
    - `bool begin(System&, const SessionConfig&, const std::string& dir, std::string* err)`
    - `void after_frame(System&, uint16_t e020, uint64_t now_ms)`
    - `int marker(System&)` returns the marker number, or 0.
    - `void end(System&, const std::string& exit_reason)`
    - `bool export_replay(const std::string& path) const`
    - `bool active() const`, `const std::string& dir() const`, `int markers() const`, `int failures() const`, `const Replay& replay() const`
  - Folder layout: `manifest.json`, `session.replay`, `session.replay.bram` (only if there was backup RAM), `session.ckpt`, `session.log`, `markers/NNN-f<frame>/`, `failure/NNN-<hang|halted>-f<frame>/`. NNN is 3-digit and 1-based. Frames are decimal in folder names.
  - `scd_headless` additions:
    - `--session-out DIR` records into DIR itself. It cannot be combined with `--watch` or `--replay` (exit 2). Its flush clock is emulated time (`frame_count() * 1000 / 60`), so a flush happens every 60 frames.
    - `--marker-at F` (repeatable) writes a marker after frame F.
    - Env `SCD_TEST_KILL_AT=<frame>`: `std::_Exit(70)` after that frame's session update.

- [ ] **Step 1: Write the failing unit tests**

In `engine/tests/watchdog_test.cpp`, add before `    std::puts("watchdog_test: ok");`:

```cpp
    {   // HangLatch (live sessions): one report per stall, re-armed once $E020 moves again
        Watchdog w({30, 1800});
        HangLatch latch;
        int fired = 0;
        for (uint64_t f = 1; f <= 400; ++f) {
            // advancing, stuck at 49 for frames 50..199, advancing again, stuck at 249 from frame 250 on
            const uint16_t e020 = f < 50 ? uint16_t(f) : f < 200 ? uint16_t(49) : f < 250 ? uint16_t(f) : uint16_t(249);
            fired += latch.fire(w.feed(s(f, e020, f)), e020);
        }
        assert(fired == 2);
    }
```

In `engine/tests/session_test.cpp`:
- Add `#include <chrono>`, `#include <filesystem>`, `#include <fstream>` after `#include <ctime>`.
- Add `#include "session.h"` after `#include "manifest.h"`.
- Add before `int main()`:

```cpp
static void test_session_dirs() {
    namespace fs = std::filesystem;
    const fs::path base = fs::temp_directory_path() /
                          ("snatcher_session_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::string err;
    const std::string parent = (base / "p").string();
    const std::string a = create_session_dir(parent, "20261016-201403", &err);
    const std::string b = create_session_dir(parent, "20261016-201403", &err);
    const std::string c = create_session_dir(parent, "20261016-201403", &err);
    assert(fs::path(a).filename() == "20261016-201403" && fs::is_directory(a));
    assert(fs::path(b).filename() == "20261016-201403-1");
    assert(fs::path(c).filename() == "20261016-201403-2");
    std::ofstream((base / "file").string()) << "x";   // a file where the parent directory should be
    err.clear();
    assert(create_session_dir((base / "file").string(), "s", &err).empty() && !err.empty());
    fs::create_directories(base / "repo" / "engine" / "src" / "scd");
    assert(default_session_parent((base / "repo").string(), "/pref/") == (base / "repo" / "work" / "sessions").string());
    assert(default_session_parent((base / "elsewhere").string(), "/pref/") == "/pref/sessions");
    fs::remove_all(base);
    const std::string st = session_stamp(1792206843);
    assert(st.size() == 15 && st[8] == '-');
}
```

- Add `    test_session_dirs();` in `main()` before `std::puts`.

- [ ] **Step 2: Run them to verify they fail**

Run: `cmake --build engine/build-interp -j --target watchdog_test session_test`
Expected: compile errors, `HangLatch` not declared and `session.h` not found.

- [ ] **Step 3: `HangLatch` and `System::fb_hash()`**

In `engine/src/scd/watchdog.h`, replace

```cpp
}  // namespace scd
```

with

```cpp
// Live play (sessions): play continues after a hang, so report each stall once. fire() is true on the first kHang
// verdict of a stall and false until the sampled $E020 differs from the value it stalled at.
class HangLatch {
public:
    bool fire(const WatchVerdict& v, uint16_t e020) {
        if (latched_ && e020 != e020_) latched_ = false;
        if (v.kind != WatchVerdict::kHang || latched_) return false;
        latched_ = true;
        e020_ = e020;
        return true;
    }
private:
    bool latched_ = false;
    uint16_t e020_ = 0;
};

}  // namespace scd
```

Change the first comment line of `watchdog.h` to `// Hang / crash / still-screen detector. Pure logic: scd_headless and session recording feed it one sample per emulated frame.`

In `engine/src/scd/system.h`, replace

```cpp
    StateHash state_hash();
```

with

```cpp
    StateHash state_hash();
    uint64_t fb_hash() const;   // state_hash().fb alone: the watchdog's per-frame sample
```

In `engine/src/scd/system.cpp` (in `System::state_hash`), replace

```cpp
    s.fb = fnv(vdp_.framebuffer(), size_t(width()) * height() * sizeof(uint32_t));
    return s;
}
```

with

```cpp
    s.fb = fb_hash();
    return s;
}

uint64_t System::fb_hash() const { return fnv(vdp_.framebuffer(), size_t(width()) * height() * sizeof(uint32_t)); }
```

- [ ] **Step 4: The session unit**

Create `engine/src/session/session.h`:

```cpp
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
```

Create `engine/src/session/session.cpp`:

```cpp
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
```

In `engine/CMakeLists.txt`, replace `add_library(scdsession STATIC src/session/manifest.cpp)` with `add_library(scdsession STATIC src/session/manifest.cpp src/session/session.cpp)`.

- [ ] **Step 5: Run the unit tests**

Run: `cmake --build engine/build-interp -j --target watchdog_test session_test && engine/build-interp/watchdog_test && engine/build-interp/session_test`
Expected: `watchdog_test: ok`, `session_test: ok`.

- [ ] **Step 6: Write the failing end-to-end tests**

In `tools/playtest/test_playtest.py`, after `new_dir`, add:

```python
# Synthetic play for session tests: two pad presses and Justifier moves / trigger and start pulls (frames 200..656).
SYNTH = ("--press", "300:S:6", "--press", "500:C:6", "--justifier",
         "--gun", "200:100,80,in,T", "--gun", "206:100,80,in,-", "--gun", "650:20,30,out,S", "--gun", "656:20,30,out,-")


def record_session(frames, *extra, env=None, sdir=None):
    """Record a session with scd_headless --session-out. Returns (CompletedProcess, session dir)."""
    base = new_dir("pt_session_")
    sdir = sdir or base / "s"
    out = base / "out"
    out.mkdir()
    return run_headless("--frames", str(frames), "--out", str(out), "--session-out", str(sdir), *extra, env=env), sdir


def make_bram(path):
    """A formatted backup RAM file: the core formats a missing one and writes it at exit."""
    r = run_headless("--frames", "5", "--out", str(path.parent), "--bram", str(path))
    assert r.returncode == 0, r.stderr
    return path


def ckpt_frame(line):
    return int(line.split()[0][3:], 16)
```

Add this class before `A = "@0x64 ...`:

```python
class TestSession(unittest.TestCase):
    @needs_headless
    def test_session_out_rejects_watch(self):
        r = run_headless("--frames", "5", "--session-out", str(new_dir("pt_session_bad_") / "s"), "--watch")
        self.assertEqual(r.returncode, 2, r.stderr)

    @needs_disc
    def test_session_folder_layout(self):
        r, sdir = record_session(1200, *SYNTH, "--marker-at", "400", "--marker-at", "900")
        self.assertEqual(r.returncode, 0, r.stderr)
        for name in ("manifest.json", "session.replay", "session.ckpt", "session.log"):
            self.assertTrue((sdir / name).exists(), name)
        self.assertFalse((sdir / "session.replay.bram").exists())   # no --bram: bram sha1=none
        m = json.loads((sdir / "manifest.json").read_text())
        self.assertEqual((m["app"], m["frames"], m["exit"], m["markers"], m["failures"]), ("scd_headless", 1200, "quit", 2, 0))
        self.assertTrue(m["justifier"])
        self.assertEqual(m["bram_sha1"], "none")
        self.assertIsNotNone(m["ended"])
        self.assertEqual(sorted(p.name for p in (sdir / "markers").iterdir()), ["001-f400", "002-f900"])
        mk = sdir / "markers" / "001-f400"
        for name in ("marker.json", "screen.ppm", "mainram.bin", "subram.bin", "state.txt"):
            self.assertTrue((mk / name).exists(), name)
        self.assertEqual(json.loads((mk / "marker.json").read_text())["frame"], 400)
        self.assertEqual((mk / "mainram.bin").stat().st_size, 0x10000)
        self.assertEqual((mk / "subram.bin").stat().st_size, 0x6000)
        text = (sdir / "session.replay").read_text()
        self.assertTrue(text.startswith("# snatcher-replay v2\n"))
        self.assertIn("justifier on\n", text)
        for line in ("@0x12c S\n", "@0x132 -\n", "g0xc8 100,80,in,T\n", "g0x28a 20,30,out,S\n"):
            self.assertIn(line, text)
        self.assertIn("[user] marker 001 at frame 400", (sdir / "session.log").read_text())

    @needs_disc
    def test_session_replays_identically(self):
        # The session opens the log (heartbeat, cpu_pcs) and writes a marker (state_text); the replay does neither.
        r, sdir = record_session(1500, *SYNTH, "--marker-at", "700")
        self.assertEqual(r.returncode, 0, r.stderr)
        out = sdir.parent / "replay"
        out.mkdir()
        ck = out / "r.ckpt"
        rr = run_headless("--frames", "1500", "--out", str(out), "--replay", str(sdir / "session.replay"), "--ckpt-out", str(ck))
        self.assertEqual(rr.returncode, 0, rr.stderr)
        self.assertEqual((sdir / "session.ckpt").read_text(), ck.read_text())

    @needs_disc
    def test_session_copies_starting_bram(self):
        import hashlib
        base = new_dir("pt_session_bram_")
        bram = make_bram(base / "bram.bin")
        before = hashlib.sha1(bram.read_bytes()).hexdigest()
        r, sdir = record_session(600, "--bram", str(bram))
        self.assertEqual(r.returncode, 0, r.stderr)
        copy = sdir / "session.replay.bram"
        self.assertEqual(hashlib.sha1(copy.read_bytes()).hexdigest(), before)
        self.assertIn(f"bram sha1={before}\n", (sdir / "session.replay").read_text())
        self.assertEqual(json.loads((sdir / "manifest.json").read_text())["bram_sha1"], before)

    @needs_disc
    def test_session_hang_writes_one_bundle(self):
        r, sdir = record_session(200, "--hang-frames", "30", env={"SCD_TEST_STICK_E020": "20"})
        self.assertEqual(r.returncode, 0, r.stderr)   # play continues after a hang
        bundles = list((sdir / "failure").iterdir())
        self.assertEqual(len(bundles), 1, bundles)
        self.assertRegex(bundles[0].name, r"^001-hang-f\d+$")
        for name in ("report.json", "final.ppm", "mainram.bin", "subram.bin", "repro.replay", "state.txt"):
            self.assertTrue((bundles[0] / name).exists(), name)
        self.assertEqual(json.loads((sdir / "manifest.json").read_text())["failures"], 1)

    @needs_disc
    def test_unwritable_file_is_dropped(self):
        sdir = new_dir("pt_session_ro_") / "s"
        (sdir / "session.ckpt").mkdir(parents=True)   # a directory where the checkpoint file goes: fopen fails
        r, _ = record_session(120, sdir=sdir)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("session.ckpt", r.stderr)
        self.assertTrue((sdir / "session.replay").read_text().startswith("# snatcher-replay v2\n"))
        self.assertEqual(json.loads((sdir / "manifest.json").read_text())["frames"], 120)

    @needs_disc
    def test_session_survives_kill(self):
        r, sdir = record_session(2000, *SYNTH, env={"SCD_TEST_KILL_AT": "1000"})
        self.assertEqual(r.returncode, 70, r.stderr)
        m = json.loads((sdir / "manifest.json").read_text())
        self.assertIsNone(m["ended"])
        self.assertIsNone(m["frames"])
        self.assertIsNone(m["exit"])
        text = (sdir / "session.ckpt").read_text()
        last = ckpt_frame(text.splitlines()[-1])
        self.assertTrue(900 <= last <= 960, last)   # flushed every 60 emulated frames: the last flush was after frame 960
        out = sdir.parent / "replay"
        out.mkdir()
        ck = out / "r.ckpt"
        rr = run_headless("--frames", str(last), "--out", str(out), "--replay", str(sdir / "session.replay"), "--ckpt-out", str(ck))
        self.assertEqual(rr.returncode, 0, rr.stderr)
        self.assertEqual(text, ck.read_text())
```

- [ ] **Step 7: Run them to verify they fail**

Run: `cmake --build engine/build-interp -j --target scd_headless && SCD_HEADLESS=engine/build-interp/scd_headless .venv/bin/python tools/playtest/test_playtest.py TestSession -v`
Expected: the tests fail, because `--session-out` is ignored and no folder is created. For example, `test_session_folder_layout` fails on the missing `manifest.json`, and `test_session_out_rejects_watch` gets exit 0.

- [ ] **Step 8: `scd_headless --session-out`**

In `engine/src/frontend/headless.cpp`:
- Add `#include "session.h"` after `#include "replay.h"`.
- In the usage comment, after the `--justifier` line, add `//                [--session-out DIR [--marker-at FRAME ...]] [--manifest-out FILE]   (SCD_TEST_KILL_AT=<frame>: exit at once, for tests)`.
- Replace `    std::string replay_path, ckpt_path, bram, manifest_path;` with `    std::string replay_path, ckpt_path, bram, manifest_path, session_out;`.
- Replace `    std::vector<GunPress> guns;` with:

```cpp
    std::vector<GunPress> guns;
    std::vector<uint64_t> marker_at;
```

- Replace `        else if (a == "--manifest-out" && i + 1 < argc) manifest_path = argv[++i];` with:

```cpp
        else if (a == "--manifest-out" && i + 1 < argc) manifest_path = argv[++i];
        else if (a == "--session-out" && i + 1 < argc) session_out = argv[++i];
        else if (a == "--marker-at" && i + 1 < argc) marker_at.push_back(std::strtoull(argv[++i], nullptr, 10));
```

- Replace

```cpp
    if (const char* lf = std::getenv("SCD_LOG")) {   // debug log file, same as the app's --log
```

with

```cpp
    if (!session_out.empty() && (watch || !replay_path.empty())) {
        std::fprintf(stderr, "error: --session-out records a session; it cannot be combined with --watch or --replay\n");
        return 2;
    }
    if (const char* lf = std::getenv("SCD_LOG")) {   // debug log file, same as the app's --log
```

- Replace

```cpp
    uint16_t stuck_val = 0;
    bool stuck_set = false;
```

with

```cpp
    uint16_t stuck_val = 0;
    bool stuck_set = false;
    auto sampled_e020 = [&]() -> uint16_t {   // SCD_TEST_STICK_E020: test hook, pins the watchdog's sample only
        uint16_t e20 = sys.e020();
        if (stick_from && sys.frame_count() >= stick_from) { if (!stuck_set) { stuck_val = e20; stuck_set = true; } e20 = stuck_val; }
        return e20;
    };
    const uint64_t kill_at = std::getenv("SCD_TEST_KILL_AT") ? std::strtoull(std::getenv("SCD_TEST_KILL_AT"), nullptr, 10) : 0;
```

- Replace

```cpp
            uint16_t e20 = sys.e020();
            if (stick_from && sys.frame_count() >= stick_from) { if (!stuck_set) { stuck_val = e20; stuck_set = true; } e20 = stuck_val; }   // SCD_TEST_STICK_E020: test hook, samples only
            WatchVerdict v = dog.feed({sys.frame_count(), e20, sys.state_hash().fb, sys.halted()});
```

with

```cpp
            WatchVerdict v = dog.feed({sys.frame_count(), sampled_e020(), sys.state_hash().fb, sys.halted()});
```

- Replace

```cpp
        sys.set_replay_record(&trace);
    }
```

with

```cpp
        sys.set_replay_record(&trace);
    }
    Session session;   // --session-out: the app's session writer, driven by --press / --gun (tests)
    if (!session_out.empty()) {
        SessionConfig sc;
        sc.cue = cue;
        sc.bram_path = bram;
        sc.engine = "scd_headless --session-out";
        sc.cd_speed = sys.cd_speed();
        sc.justifier = justifier;
        sc.ckpt_every = ckpt_every;
        sc.hang_frames = hang_frames;
        sc.manifest.app = "scd_headless";
        sc.manifest.os = host_os();
        for (int i = 2; i < argc; ++i) sc.manifest.args.push_back(argv[i]);
        if (!session.begin(sys, sc, session_out, &err)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
    }
```

- Replace

```cpp
        rec.frame(sys.framebuffer(), sys.width(), sys.height(), sys.audio());
        if (watch) {
```

with

```cpp
        rec.frame(sys.framebuffer(), sys.width(), sys.height(), sys.audio());
        if (session.active()) {
            session.after_frame(sys, sampled_e020(), sys.frame_count() * 1000 / 60);   // emulated clock: a flush per second of play, deterministic
            for (uint64_t m : marker_at) if (m == sys.frame_count()) session.marker(sys);
            if (kill_at && sys.frame_count() == kill_at) std::_Exit(70);   // SCD_TEST_KILL_AT: a killed app (no cleanup)
        }
        if (watch) {
```

- Replace `    if (frametime && !ft.empty()) {` with:

```cpp
    if (session.active()) session.end(sys, sys.halted() ? "halted" : "quit");
    if (frametime && !ft.empty()) {
```

`sys` is declared before the `sampled_e020` lambda (`System& sys = System::instance();` comes before `Watchdog dog(...)`), so the lambda can capture it.

- [ ] **Step 9: Run all tests**

Run:

```bash
cmake --build engine/build-interp -j --target scd_headless replay_test watchdog_test checkpoint_test session_test
engine/build-interp/watchdog_test && engine/build-interp/session_test
SCD_HEADLESS=engine/build-interp/scd_headless .venv/bin/python tools/playtest/test_playtest.py -v
```

Expected: everything passes. If `test_session_replays_identically` fails, the session's log or markers perturb emulation. Do not paper over it. Find the first differing checkpoint (`.venv/bin/python tools/playtest/ckpt_diff.py A B`) and fix the read path in `System` (for example, make `cpu_pcs`/`state_text` read the parked CPU through `m68k_get_reg(ctx_[c].data(), ...)` instead of `select_cpu`).

- [ ] **Step 10: Commit**

```bash
git add engine/src/session/session.h engine/src/session/session.cpp engine/src/scd/watchdog.h engine/src/scd/system.h \
  engine/src/scd/system.cpp engine/CMakeLists.txt engine/src/frontend/headless.cpp engine/tests/watchdog_test.cpp \
  engine/tests/session_test.cpp tools/playtest/test_playtest.py
git commit -m "playtest: session writer (replay, checkpoints, markers, failure bundles), scd_headless --session-out"
```

---

### Task 6: `run.py --session`

**Files:**
- Modify: `tools/playtest/run.py` (whole file, 80 lines)
- Modify: `mise.toml` (new `playtest:session` task after `[tasks.playtest]`)
- Test: `tools/playtest/test_playtest.py`

**Interfaces:**
- Consumes: the Task 5 session folder; Task 4 `--manifest-out`; `ckpt_diff.parse`, `first_diff`, `VISIBLE_FIELDS`.
- Produces: `run.py --session DIR --headless BIN [--cue CUE] [--strict-ram] [--work DIR]`. Exit codes:
  - 0: reproduces at every checkpoint. A RAM-only difference without `--strict-ram` also gives 0, with a `RAM-only difference` line.
  - 1: differs visibly (`DIVERGED`, first frame printed), or RAM-only with `--strict-ram`.
  - 2: not a session folder.
  - 4: identity mismatch, passed through from `scd_headless`.
  - Any other code: the headless exit code.
  - On a build-kind mismatch it prints `warning: ...`. For a session without `ended` it prints `note: the session did not end cleanly ...`. On success it prints `ok: session reproduces at all N checkpoints (F frames)`.

- [ ] **Step 1: Write the failing tests**

In `tools/playtest/test_playtest.py`, add after `ckpt_frame`:

```python
def run_session_check(sdir, *extra, env=None):
    e = dict(os.environ, **(env or {}))
    return subprocess.run([sys.executable, str(pathlib.Path(__file__).parent / "run.py"), "--session", str(sdir), "--headless", HEADLESS,
                           "--work", str(sdir.parent / "verify"), *extra], capture_output=True, text=True, env=e)
```

Add this class after `class TestSession`:

```python
class TestSessionVerify(unittest.TestCase):
    def test_missing_manifest_is_an_error(self):
        d = new_dir("pt_nosession_")
        r = run_session_check(d)
        self.assertEqual(r.returncode, 2, r.stdout + r.stderr)
        self.assertIn("manifest.json", r.stdout)

    @needs_disc
    def test_recorded_session_reproduces(self):
        bram = make_bram(new_dir("pt_verify_bram_") / "bram.bin")
        r, sdir = record_session(1500, *SYNTH, "--marker-at", "700", "--bram", str(bram))
        self.assertEqual(r.returncode, 0, r.stderr)
        v = run_session_check(sdir)
        self.assertEqual(v.returncode, 0, v.stdout + v.stderr)
        self.assertIn("session reproduces", v.stdout)

    @needs_disc
    def test_visible_divergence_reports_frame(self):
        r, sdir = record_session(1500, *SYNTH)
        self.assertEqual(r.returncode, 0, r.stderr)
        ck = sdir / "session.ckpt"
        lines = ck.read_text().splitlines()
        frame = ckpt_frame(lines[3])
        lines[3] = re.sub(r"fb=[0-9a-f]+", "fb=" + "0" * 16, lines[3])
        ck.write_text("\n".join(lines) + "\n")
        v = run_session_check(sdir)
        self.assertEqual(v.returncode, 1, v.stdout + v.stderr)
        self.assertIn("DIVERGED", v.stdout)
        self.assertIn(f"'frame': {frame}", v.stdout)

    @needs_disc
    def test_ram_only_difference_needs_strict(self):
        r, sdir = record_session(900)
        self.assertEqual(r.returncode, 0, r.stderr)
        ck = sdir / "session.ckpt"
        lines = ck.read_text().splitlines()
        lines[2] = re.sub(r"ram=[0-9a-f]+", "ram=" + "0" * 16, lines[2])
        ck.write_text("\n".join(lines) + "\n")
        v = run_session_check(sdir)
        self.assertEqual(v.returncode, 0, v.stdout + v.stderr)
        self.assertIn("RAM-only", v.stdout)
        self.assertEqual(run_session_check(sdir, "--strict-ram").returncode, 1)

    @needs_disc
    def test_wrong_disc_is_identity_mismatch(self):
        r, sdir = record_session(300)
        self.assertEqual(r.returncode, 0, r.stderr)
        rp = sdir / "session.replay"
        rp.write_text(re.sub(r"^disc sha1=.*$", "disc sha1=0000", rp.read_text(), flags=re.M))
        v = run_session_check(sdir)
        self.assertEqual(v.returncode, 4, v.stdout + v.stderr)

    @needs_disc
    def test_build_kind_mismatch_warns(self):
        r, sdir = record_session(300, env={"SCD_NO_TRANSLATE": "1"})
        self.assertEqual(r.returncode, 0, r.stderr)
        mf = sdir / "manifest.json"
        m = json.loads(mf.read_text())
        m["build"] = "translated"
        mf.write_text(json.dumps(m))
        v = run_session_check(sdir, env={"SCD_NO_TRANSLATE": "1"})
        self.assertEqual(v.returncode, 0, v.stdout + v.stderr)
        self.assertIn("warning", v.stdout)

    @needs_disc
    def test_killed_session_verifies_to_last_checkpoint(self):
        r, sdir = record_session(2000, *SYNTH, env={"SCD_TEST_KILL_AT": "1000"})
        self.assertEqual(r.returncode, 70, r.stderr)
        v = run_session_check(sdir)
        self.assertEqual(v.returncode, 0, v.stdout + v.stderr)
        self.assertIn("did not end cleanly", v.stdout)
```

- [ ] **Step 2: Run them to verify they fail**

Run: `SCD_HEADLESS=engine/build-interp/scd_headless .venv/bin/python tools/playtest/test_playtest.py TestSessionVerify -v`
Expected: FAIL. argparse rejects `--session` (exit 2 with a usage message on stderr, not stdout), so `test_missing_manifest_is_an_error` fails on the `manifest.json` assertion and the others on exit codes.

- [ ] **Step 3: Implement**

Replace `tools/playtest/run.py` with:

```python
#!/usr/bin/env python3
"""Run a replay headless, optionally against a second build, and report the first divergence or watchdog trip.
With --session DIR, replay a recorded session folder (docs/PLAYTEST.md "Sessions") and compare it with its checkpoints.

Exit codes: 0 pass, 1 checkpoints differ, 2 not a session folder, 3 watchdog trip, 4 replay identity mismatch
(wrong disc or backup RAM), other = headless failure.
"""
import argparse
import json
import pathlib
import shutil
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import ckpt_diff

ROOT = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_CUE = ROOT / "sega-cd-eng" / "Snatcher (Sega CD) (U)-redump.cue"


def run_once(headless, cue, replay, frames, bram, out):
    out.mkdir(parents=True, exist_ok=True)
    ckpt = out / "run.ckpt"
    cmd = [headless, str(cue), "--frames", str(frames), "--out", str(out), "--replay", str(replay),
           "--ckpt-out", str(ckpt), "--watch"]
    if bram:
        tmp = out / "bram.bin"
        shutil.copy(bram, tmp)   # the core writes BRAM back on exit, so never pass the original
        cmd += ["--bram", str(tmp)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r.returncode, ckpt, r.stderr


def verify_session(a):
    sdir = pathlib.Path(a.session)
    try:
        m = json.loads((sdir / "manifest.json").read_text())
    except (OSError, ValueError) as e:
        print(f"error: {sdir} is not a session folder (manifest.json: {e})")
        return 2
    replay, ckpt = sdir / "session.replay", sdir / "session.ckpt"
    if not replay.exists() or not ckpt.exists():
        print(f"error: {sdir} has no session.replay or session.ckpt")
        return 2
    try:
        rec = ckpt_diff.parse(ckpt.read_text())
    except ValueError as e:
        print(f"error: {ckpt}: {e}")
        return 2
    if not rec:
        print(f"error: {ckpt} has no checkpoints")
        return 2
    if m.get("ended") is None:
        print("note: the session did not end cleanly (crash or kill); verifying up to its last checkpoint")
    frames = m.get("frames") or rec[-1].frame
    work = pathlib.Path(a.work) / "session"
    work.mkdir(parents=True, exist_ok=True)
    out_ck, out_mf = work / "replay.ckpt", work / "replay_manifest.json"
    cmd = [a.headless, str(a.cue), "--frames", str(frames), "--out", str(work), "--replay", str(replay),
           "--ckpt-out", str(out_ck), "--ckpt-every", str(m.get("ckpt_every", 300)), "--manifest-out", str(out_mf)]
    bram = sdir / "session.replay.bram"
    if bram.exists():
        tmp = work / "bram.bin"
        shutil.copy(bram, tmp)   # the core writes BRAM back on exit, so never pass the original
        cmd += ["--bram", str(tmp)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"headless replay failed (exit {r.returncode}):\n{r.stderr}")
        return r.returncode
    try:
        built = json.loads(out_mf.read_text()).get("build")
    except (OSError, ValueError):
        built = None
    if built and m.get("build") and built != m["build"]:
        print(f"warning: the session was recorded on the {m['build']} build but {a.headless} is the {built} build; "
              "those builds differ in RAM and may differ on screen (ADR-003)")
    rep = ckpt_diff.parse(out_ck.read_text())
    visible = ckpt_diff.first_diff(rec, rep, ckpt_diff.VISIBLE_FIELDS)
    anything = ckpt_diff.first_diff(rec, rep)
    if visible:
        print(f"DIVERGED (visible; A = session, B = headless replay): {visible}\n  first difference of any kind: {anything}")
        return 1
    if anything:
        print(f"RAM-only difference (screen and VRAM match everywhere; A = session, B = headless replay): {anything}")
        return 1 if a.strict_ram else 0
    print(f"ok: session reproduces at all {len(rec)} checkpoints ({frames} frames)")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("replay", nargs="?")
    ap.add_argument("--session", metavar="DIR", help="verify a recorded session folder against its checkpoints")
    ap.add_argument("--headless", required=True)
    ap.add_argument("--compare-headless")
    ap.add_argument("--cue", default=str(DEFAULT_CUE))
    ap.add_argument("--frames", type=int, default=20000)
    ap.add_argument("--bram")
    ap.add_argument("--strict-ram", action="store_true", help="fail on RAM-only differences too")
    ap.add_argument("--determinism", action="store_true", help="run the same build twice and compare")
    ap.add_argument("--work", default=str(ROOT / "work" / "playtest"))
    a = ap.parse_args()
    if a.session:
        return verify_session(a)
    if not a.replay:
        ap.error("give a replay file or --session DIR")
    work = pathlib.Path(a.work)
    other = a.compare_headless or (a.headless if a.determinism else None)

    rc, ck_a, err = run_once(a.headless, a.cue, a.replay, a.frames, a.bram, work / "A")
    if rc == 3:
        print(f"WATCHDOG TRIP in A; bundle in {work / 'A'}\n{err}")
        return 3
    if rc != 0:
        print(f"A failed (exit {rc}):\n{err}")
        return rc
    if other is None:
        print(f"ok: {len(ckpt_diff.parse(ck_a.read_text()))} checkpoints in {ck_a}")
        return 0
    rc, ck_b, err = run_once(other, a.cue, a.replay, a.frames, a.bram, work / "B")
    if rc == 3:
        print(f"WATCHDOG TRIP in B; bundle in {work / 'B'}\n{err}")
        return 3
    if rc != 0:
        print(f"B failed (exit {rc}):\n{err}")
        return rc
    pa, pb = ckpt_diff.parse(ck_a.read_text()), ckpt_diff.parse(ck_b.read_text())
    visible = ckpt_diff.first_diff(pa, pb, ckpt_diff.VISIBLE_FIELDS)
    anything = ckpt_diff.first_diff(pa, pb)
    if visible:
        print(f"DIVERGED (visible): {visible}\n  first difference of any kind: {anything}")
        return 1
    if anything:
        print(f"RAM-only difference (screen and VRAM match everywhere): {anything}")
        if a.strict_ram:
            return 1
        return 0
    print("ok: builds match at every checkpoint")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

The session replay runs without `--watch`. A session that hung is still compared checkpoint by checkpoint, and its own `failure/` bundles already hold the evidence.

In `mise.toml`, after the `[tasks.playtest]` block, add:

```toml

[tasks."playtest:session"]
description = "Verify a recorded session folder: mise run playtest:session work/sessions/<stamp> (docs/PLAYTEST.md, use the build kind it was recorded with)"
run = "python tools/playtest/run.py --session {{arg(name='dir')}} --headless ${PLAYTEST_HEADLESS:-engine/build/scd_headless} ${PLAYTEST_ARGS:-}"
```

- [ ] **Step 4: Run the tests**

Run: `SCD_HEADLESS=engine/build-interp/scd_headless .venv/bin/python tools/playtest/test_playtest.py -v`
Expected: everything passes, including `TestSessionVerify` (7 tests) and the existing `test_runner_determinism_mode`.

- [ ] **Step 5: Commit**

```bash
git add tools/playtest/run.py tools/playtest/test_playtest.py mise.toml
git commit -m "playtest: run.py --session verifies a recorded session folder"
```

---

### Task 7: SDL app: always-on sessions, F8/F10 markers, docs

**Files:**
- Modify: `engine/src/frontend/sdl_main.cpp` (header comment lines 2-6, includes lines 14-23, `boot` lines 62-78, options lines 115-129, setup lines 130-176, event loop lines 242-256, frame loop lines 294-305, render lines 329-334, exit lines 336-348)
- Modify: `engine/CMakeLists.txt:108-113` (`snatcher` link)
- Modify: `docs/PLAYTEST.md`, `docs/PLAYING.md`, `tools/package/PLAYTEST_README.txt`, `tools/package/PLAYTEST_README_PC.txt`, `CLAUDE.md`

**Interfaces:**
- Consumes:
  - Task 5: `Session`, `SessionConfig`, `create_session_dir`, `default_session_parent`, `session_stamp`, `Session::export_replay`.
  - Task 4: `host_os`.
  - `System::e020()`, `dump_state()`.
- Produces:
  - `snatcher` flags `--no-session` and `--session-dir DIR`. `--record-input FILE` is now an alias: the first session's replay goes to `FILE` and `FILE.bram` at exit, and it forces a session even with `--no-session`.
  - F8 writes a marker and flashes a white frame. F10 logs the state as before and also writes a marker.
  - Exit reasons: `quit` (Esc or window close), `disc_changed` (drop), `halted` (CPU halt).

- [ ] **Step 1: Link `scdsession`**

In `engine/CMakeLists.txt`, replace

```cmake
    target_link_libraries(snatcher PRIVATE scdcore SDL3::SDL3-static)
  else()
    target_link_libraries(snatcher PRIVATE scdcore SDL3::SDL3)
```

with

```cmake
    target_link_libraries(snatcher PRIVATE scdcore scdsession SDL3::SDL3-static)
  else()
    target_link_libraries(snatcher PRIVATE scdcore scdsession SDL3::SDL3)
```

- [ ] **Step 2: Header comment, includes, helpers**

In `engine/src/frontend/sdl_main.cpp`, replace

```cpp
//   --record-input FILE: save pad input per emulated frame (a .replay for scd_headless --replay) and the starting backup RAM (FILE.bram)
```

with

```cpp
//   Every boot records a session folder (docs/PLAYTEST.md "Sessions"): work/sessions/ when run from the repository, else
//   <per-user data dir>/sessions/. --no-session: do not record; --session-dir DIR: parent directory for session folders.
//   --record-input FILE: also copy the first session's replay to FILE and its starting backup RAM to FILE.bram at exit
//   F8: mark "this looks wrong" (markers/ in the session folder); F10: the same plus a state snapshot in the log
```

Add `#include <filesystem>` after `#include <ctime>`. Replace

```cpp
#include "recorder.h"
#include "replay.h"
#include "system.h"
```

with

```cpp
#include "manifest.h"
#include "recorder.h"
#include "replay.h"
#include "session.h"
#include "system.h"
```

Replace the whole `bool boot(const std::string& cue, bool* running_game) { ... }` (lines 62-78) with:

```cpp
std::string pref_dir() {   // per-user data directory (ends in a separator), "" when SDL cannot provide one
    char* pref = SDL_GetPrefPath("snatcher-port", "snatcher");
    std::string s = pref ? pref : "";
    SDL_free(pref);
    return s;
}

std::string bram_path() {
    const std::string p = pref_dir();
    return p.empty() ? p : p + "bram.bin";
}

bool boot(const std::string& cue, bool* running_game) {
    std::string err;
    auto disc = Disc::open(cue, &err);
    if (!disc) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return false;
    }
    if (!System::instance().init(std::move(disc), bram_path(), &err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return false;
    }
    *running_game = true;
    return true;
}
```

- [ ] **Step 3: Options**

Replace `    bool want_log = std::getenv("SNATCHER_LOG") != nullptr;` with:

```cpp
    bool want_log = std::getenv("SNATCHER_LOG") != nullptr;
    bool want_session = true;
    std::string session_parent;
```

Replace

```cpp
        else if (a == "--record-input" && i + 1 < argc) record_input = argv[++i];
```

with

```cpp
        else if (a == "--record-input" && i + 1 < argc) record_input = argv[++i];
        else if (a == "--no-session") want_session = false;
        else if (a == "--session-dir" && i + 1 < argc) session_parent = argv[++i];
```

- [ ] **Step 4: Setup, session open/close**

Replace everything from `    System::instance().set_cd_speed(cd_speed);` (line 130) through `    SDL_SetWindowTitle(window, running_game ? "Snatcher" : "Snatcher - drop a .cue file on this window");` (line 176). This removes the old debug-log block, `input_replay`, `recording_booted` and the `.bram` copy. The replacement is:

```cpp
    System::instance().set_cd_speed(cd_speed);
    if (!record_input.empty() && !want_session) {
        std::fprintf(stderr, "--record-input saves the session's replay: ignoring --no-session\n");
        want_session = true;
    }
    if (!record_input.empty()) {   // an hour-long freeze hunt must not be lost to a missing directory: fail now, not at exit
        std::ofstream probe(record_input, std::ios::binary);
        if (!probe) { std::fprintf(stderr, "error: cannot write %s (does the directory exist?)\n", record_input.c_str()); return 1; }
    }
    std::vector<std::string> session_args;   // manifest "args": the command line without the disc path
    for (int i = 1; i < argc; ++i)
        if (disc.empty() || disc != argv[i]) session_args.push_back(argv[i]);
    const int sdl_v = SDL_GetVersion();
    char sdl_version[32];
    std::snprintf(sdl_version, sizeof sdl_version, "%d.%d.%d", SDL_VERSIONNUM_MAJOR(sdl_v), SDL_VERSIONNUM_MINOR(sdl_v), SDL_VERSIONNUM_MICRO(sdl_v));
    const char* renderer_name = SDL_GetRendererName(renderer);
    auto log_header = [&](const std::string& log_path, const std::string& cue) {
        scd::debug_log("platform %s, SDL %s, renderer %s\n", SDL_GetPlatform(), sdl_version, renderer_name ? renderer_name : "?");
        const char* adev = SDL_GetAudioDeviceName(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK);
        scd::debug_log("audio: %s (%s); gamepad: %s; CD speed %.1fx; justifier %s\n", adev ? adev : "?", audio ? "stream ok" : "STREAM FAILED, no sound",
                       pad ? SDL_GetGamepadName(pad) : "none", cd_speed, gun ? "on" : "off");
        scd::debug_log("log file: %s\ndisc: %s\n", log_path.c_str(), cue.empty() ? "(none yet, drop a .cue on the window)" : cue.c_str());
    };
    auto open_debug_log = [&] {   // --log without a session: the per-user debug.log, as before sessions existed
        const std::string path = pref_dir() + "debug.log";
        if (System::instance().open_log(path)) log_header(path, disc);
        else std::fprintf(stderr, "could not open debug log %s\n", path.c_str());
    };
    if (want_log && !want_session) open_debug_log();

    // Session recording: one folder per booted disc. It never blocks play: on any failure the game runs without one.
    std::unique_ptr<Session> session;
    bool record_input_written = false;
    auto open_session = [&](const std::string& cue) {
        if (!want_session) return;
        std::error_code ec;
        const std::string parent = !session_parent.empty() ? session_parent
                                                           : default_session_parent(std::filesystem::current_path(ec).string(), pref_dir());
        std::string err;
        const std::string dir = create_session_dir(parent, session_stamp(std::time(nullptr)), &err);
        auto s = std::make_unique<Session>();
        SessionConfig c;
        c.cue = cue;
        c.bram_path = bram_path();
        c.engine = "snatcher app";
        c.cd_speed = cd_speed;
        c.justifier = gun;
        c.manifest.app = "snatcher";
        c.manifest.os = host_os();
        c.manifest.sdl = sdl_version;
        c.manifest.renderer = renderer_name ? renderer_name : "";
        c.manifest.args = session_args;
        if (dir.empty() || !s->begin(System::instance(), c, dir, &err)) {
            std::fprintf(stderr, "session: %s; playing without a session\n", err.c_str());
            if (want_log) open_debug_log();
            return;
        }
        log_header(dir + "/session.log", cue);   // begin() already printed and logged the folder
        session = std::move(s);
    };
    auto close_session = [&](const char* why) {
        if (!session) return;
        if (!record_input.empty() && !record_input_written) {   // --record-input: the first session's replay
            if (session->export_replay(record_input)) std::fprintf(stderr, "saved %s\n", record_input.c_str());
            else std::fprintf(stderr, "error: could not write %s\n", record_input.c_str());
            record_input_written = true;
        }
        session->end(System::instance(), why);
        session.reset();
    };

    if (!disc.empty() && boot(disc, &running_game)) open_session(disc);
    System::instance().set_gun_connected(gun);   // the controller ID is read at boot: start with --justifier to get the Gun Adjust option
    SDL_SetWindowTitle(window, running_game ? "Snatcher" : "Snatcher - drop a .cue file on this window");
```

The session begins after `init()` and before the first `run_frame()`. `bram.bin` is only written by `System::shutdown()`, so the copy is the boot state. `set_gun_connected` still comes before the first frame, as before.

- [ ] **Step 5: Events, frame loop, render, exit**

Replace the drop handler body

```cpp
                stop_recording();
                if (recording_booted) {   // frames restart at 0 and the disc/backup RAM change: keep what was recorded so far, stop recording
                    System::instance().set_replay_record(nullptr);
                    std::fprintf(stderr, "--record-input: a new disc was dropped, input recording stopped (saved at exit)\n");
                }
                if (running_game) System::instance().shutdown();
                running_game = false;
                if (boot(e.drop.data, &running_game)) SDL_SetWindowTitle(window, "Snatcher");
```

with

```cpp
                stop_recording();
                close_session("disc_changed");   // frames restart at 0 and the disc/backup RAM change: one session per disc
                if (running_game) System::instance().shutdown();
                running_game = false;
                if (boot(e.drop.data, &running_game)) {
                    SDL_SetWindowTitle(window, "Snatcher");
                    open_session(e.drop.data);
                }
```

Replace

```cpp
            } else if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_F10 && !e.key.repeat && running_game) {
                scd::debug_log("[user] F10 state snapshot at frame %llu\n", (unsigned long long)System::instance().frame_count());
                System::instance().dump_state();
```

with

```cpp
            } else if (e.type == SDL_EVENT_KEY_DOWN && (e.key.key == SDLK_F8 || e.key.key == SDLK_F10) && !e.key.repeat && running_game) {
                if (e.key.key == SDLK_F10) {
                    scd::debug_log("[user] F10 state snapshot at frame %llu\n", (unsigned long long)System::instance().frame_count());
                    System::instance().dump_state();
                }
                if (session && session->marker(System::instance())) flash_until_ns = SDL_GetTicksNS() + 300000000ull;   // 0.3 s
```

Replace

```cpp
    bool quit = false;
    while (!quit) {
```

with

```cpp
    uint64_t flash_until_ns = 0;   // F8/F10: a marker was written, frame the picture in white briefly
    bool quit = false;
    while (!quit) {
```

Replace

```cpp
                sys.run_frame();
                if (rec) rec->frame(sys.framebuffer(), sys.width(), sys.height(), sys.audio());
```

with

```cpp
                sys.run_frame();
                if (session) session->after_frame(sys, sys.e020(), SDL_GetTicks());
                if (rec) rec->frame(sys.framebuffer(), sys.width(), sys.height(), sys.audio());
```

Replace `                if (sys.halted()) running_game = false;` with:

```cpp
                if (sys.halted()) {   // the session's watchdog wrote the failure bundle in after_frame
                    running_game = false;
                    close_session("halted");
                }
```

Replace

```cpp
            SDL_RenderTexture(renderer, tex, &src, &dst);
        }
        SDL_RenderPresent(renderer);
```

with

```cpp
            SDL_RenderTexture(renderer, tex, &src, &dst);
            if (SDL_GetTicksNS() < flash_until_ns) {
                SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
                for (int i = 0; i < 3; ++i) {
                    SDL_FRect r = {float(i), float(i), 320.0f - 2 * i, 240.0f - 2 * i};
                    SDL_RenderRect(renderer, &r);
                }
            }
        }
        SDL_RenderPresent(renderer);
```

Replace

```cpp
    if (!record_input.empty() && recording_booted) {   // also after a CPU halt: a crash session is the one worth replaying
        System::instance().set_replay_record(nullptr);
        std::ofstream out(record_input, std::ios::binary);
        out << input_replay.serialize();
        out.close();
        if (out) std::fprintf(stderr, "saved %s\n", record_input.c_str());
        else std::fprintf(stderr, "error: could not write %s\n", record_input.c_str());
    }
    if (running_game) System::instance().shutdown();
```

with

```cpp
    close_session("quit");   // after a CPU halt the session was already closed as "halted"
    if (running_game) System::instance().shutdown();
```

Check that nothing still refers to the removed names:

Run: `grep -n "input_replay\|recording_booted" engine/src/frontend/sdl_main.cpp`
Expected: no output.

- [ ] **Step 6: Build both kinds**

Run:

```bash
cmake --build engine/build-interp -j
cmake --build engine/build -j
```

Expected: both build without errors, including `snatcher`. (`engine/build` is the translated build from `mise run build`. If `extracted/code` is missing it builds interpreter-only, which is also fine here.)

If `nix develop` is available, also check the Windows cross build: `tools/package/windows_cross.sh`. Expected: `built dist/windows/snatcher.exe` (do not share it if it is translated). Without the Nix shell, note in the commit message that the cross build was not run. CI (`build.yml`) covers MSYS2 and Linux on push.

- [ ] **Step 7: Docs**

In `docs/PLAYTEST.md`, replace the `## Record a session` section (from `## Record a session` up to, not including, `## Replay it`) with:

````markdown
## Record a session

Play in the app: every launch records a session folder (see [Sessions](#sessions)). For a plain replay file, `engine/build/snatcher "<disc>.cue" --record-input work/session.replay` also writes the first session's replay to `work/session.replay` and its starting backup RAM to `work/session.replay.bram` at exit. The live app loads `bram.bin` while headless starts empty, so a replay is only valid against the backup RAM it was recorded with.

## Sessions

Every launch of `snatcher` that boots a disc records a session: everything needed to replay it headless and check that the replay matches what was on screen. `--no-session` turns it off, and `--session-dir DIR` picks the parent directory.

**Where:**
- `work/sessions/<YYYYMMDD-HHMMSS>/` when started from the repository.
- Otherwise `sessions/` in the per-user data directory: `~/Library/Application Support/snatcher-port/snatcher/sessions/` on macOS, `%APPDATA%\snatcher-port\snatcher\sessions\` on Windows.

The path is printed at start and written to the log. Dropping a different disc on the window closes the session and starts a new one.

| File | Contents |
|---|---|
| `manifest.json` | build (`git_commit`, `git_dirty`, `build` translated/interpreter, `build_type`), OS, SDL, renderer, arguments, CD speed, Justifier, checkpoint interval, disc and backup-RAM SHA-1, start and end time, frames, exit (`quit`, `disc_changed`, `halted`; `null` if the app was killed), marker and failure counts |
| `session.replay` | replay v2 (pad and Justifier), appended about once a second |
| `session.replay.bram` | the backup RAM at boot (absent when there was none) |
| `session.ckpt` | checkpoints on scene changes, every 300 frames and at exit |
| `session.log` | the debug log (`--log` output) |
| `markers/NNN-f<frame>/` | F8 or F10: `marker.json` (frame, `$E020/$E022/$E06C`, both PCs, time), `screen.ppm`, `mainram.bin`, `subram.bin`, `state.txt` |
| `failure/NNN-<kind>-f<frame>/` | watchdog hang or halted CPU: the failure bundle (below) |

**Markers.** F8 means "this looks wrong": the app writes a marker and frames the picture in white for a moment. F10 also writes the state into the log. Note what you saw with the marker number afterwards.

**Watchdog.** A hang writes one bundle and play continues. The game may recover, and the next stall gets its own bundle. A halted CPU writes one bundle and closes the session.

**Verifying.** Verify a session against the build kind it was recorded with (the app from `mise run build` is the translated build):

```sh
.venv/bin/python tools/playtest/run.py --session work/sessions/20261016-201403 --headless engine/build/scd_headless
# or: mise run playtest:session work/sessions/20261016-201403
```

Exit codes:
- 0: reproduces at every checkpoint.
- 1: differs visibly. The first frame is printed. With `--strict-ram`, RAM-only differences also fail.
- 2: not a session folder.
- 4: identity mismatch (wrong disc or backup RAM).

A killed session (`ended: null`) is verified up to its last checkpoint. A warning is printed when the headless binary is a different build kind from the manifest's.

**Keeping a session.** To keep it as a reference replay, copy `session.replay` and `session.ckpt` to `tools/playtest/replays/<name>.replay` / `.ckpt`. Everything else stays local: the `.bram`, markers, failure bundles, log and manifest hold your save data, screenshots and RAM. Delete old session folders yourself. The log's last line gives the size.

**Headless test path.**
- `scd_headless <cue> --session-out DIR [--marker-at F ...] [--justifier --gun F:X,Y,in|out,BTNS ...]` records a session with the same writer into `DIR`. It flushes once per emulated second, so tests are deterministic.
- `SCD_TEST_KILL_AT=<frame>` exits at once after that frame, without cleanup (a killed app).
- `--manifest-out FILE` writes a manifest for an ordinary run.

````

In `docs/PLAYING.md`, replace the `--log` row

```markdown
| `--log` (or `SNATCHER_LOG=1`) | write `~/Library/Application Support/snatcher-port/snatcher/debug.log` (replaced each launch): setup, CD reads, CD-DA, key presses, game-state changes, a heartbeat every 5 s and a "nothing changed for 30 s" snapshot. `F10` adds a state snapshot on demand |
```

with

```markdown
| (always) | each launch records a session folder: `~/Library/Application Support/snatcher-port/snatcher/sessions/<date-time>/` (`work/sessions/` when run from the repository) with the input replay, checkpoints, the debug log, `F8`/`F10` markers and hang reports. See `docs/PLAYTEST.md` "Sessions" |
| `--no-session` / `--session-dir DIR` | do not record a session / put session folders in `DIR` |
| `--log` (or `SNATCHER_LOG=1`) | with `--no-session`: write `~/Library/Application Support/snatcher-port/snatcher/debug.log` (replaced each launch): setup, CD reads, CD-DA, key presses, game-state changes, a heartbeat every 5 s and a "nothing changed for 30 s" snapshot. In a session the same log is `session.log` |
```

In the controls table of `docs/PLAYING.md`, replace

```markdown
| F10 | | write a state snapshot to the debug log (use with `--log`) |
```

with

```markdown
| F8 | | mark "this looks wrong" (screenshot and RAM into the session's `markers/`; the picture flashes a white frame) |
| F10 | | the same, plus a state snapshot in the log |
```

In `tools/package/PLAYTEST_README.txt`, replace

```
If the game freezes, goes black, or won't advance, please send the debug log. Start it with --log:
  open Snatcher.app --args "/path/to/Snatcher.cue" --log
The log is written to ~/Library/Application Support/snatcher-port/snatcher/debug.log (it is replaced each launch,
so copy it before relaunching). It records the machine/SDL/audio setup, every CD read, music start/stop, your key
presses, game-state changes and a status line every 5 seconds. Pressing F10 at the moment it hangs adds a full state
snapshot. It contains no game data.
```

with

```
Every launch records a session folder in
  ~/Library/Application Support/snatcher-port/snatcher/sessions/<date-time>/
with your input, a debug log and status checkpoints, so the developer can replay exactly what you played.
Press F8 the moment something looks wrong (the picture flashes a white frame) and write down what you saw
with the marker number; a freeze is detected and recorded by itself. Please zip and send the whole folder of
that session privately: it contains screenshots and memory snapshots of the game and your save, so do not post
it publicly. (--no-session turns recording off.)
```

In `tools/package/PLAYTEST_README_PC.txt`, replace

```
If the game freezes, goes black, or won't advance, please send the debug log.
Start it with --log:
  snatcher "/path/to/Snatcher.cue" --log
The log is written next to the save (see above) as debug.log; it is replaced
each launch, so copy it before relaunching. It records the machine/SDL/audio
setup, every CD read, music start/stop, your key presses, game-state changes
and a status line every 5 seconds. Pressing F10 at the moment it hangs adds a
full state snapshot. It contains no game data.
```

with

```
Every launch records a session folder next to the save (see above), in
sessions/<date-time>/, with your input, a debug log and status checkpoints,
so the developer can replay exactly what you played. Press F8 the moment
something looks wrong (the picture flashes a white frame) and write down what
you saw with the marker number; a freeze is detected and recorded by itself.
Please zip and send the whole folder of that session privately: it contains
screenshots and memory snapshots of the game and your save, so do not post it
publicly. (--no-session turns recording off.)
```

In `CLAUDE.md`, replace

```
Debug aids: `--log` / `SNATCHER_LOG=1` (app) or `SCD_LOG=file` (headless) write a persistent log, `F10` snapshots state,
```

with

```
Debug aids: every app launch records a session folder (`work/sessions/<stamp>/` from the repo: replay v2 with gun input, checkpoints, log, `F8`/`F10` markers, watchdog bundles; verify with `run.py --session DIR`, test path `scd_headless --session-out DIR`; `docs/PLAYTEST.md` "Sessions"), `--log` / `SNATCHER_LOG=1` (app, with `--no-session`) or `SCD_LOG=file` (headless) write a persistent log, `F10` snapshots state,
```

- [ ] **Step 8: Manual check in the app**

```bash
CUE="sega-cd-eng/Snatcher (Sega CD) (U)-redump.cue"
engine/build/snatcher "$CUE"
```

1. Play for about a minute (past the title screen). Press F8 twice, a few seconds apart. Each press flashes a white frame. Quit with Esc.
2. Check the folder:

```bash
S=$(ls -d work/sessions/*/ | tail -1); echo "$S"; ls "$S" "$S/markers"; .venv/bin/python -m json.tool "$S/manifest.json"
```

Expected:
- `manifest.json`, `session.replay`, `session.ckpt`, `session.log`, and `session.replay.bram` if you have a save.
- `markers/001-f<frame>` and `markers/002-f<frame>`.
- The manifest has `"exit": "quit"`, a non-null `ended`, `"markers": 2`, `"app": "snatcher"`, and `git_commit` equal to `git rev-parse --short HEAD` (rebuild first if you committed since the build).
- `"build"` is `"translated"` for `engine/build`.

3. `.venv/bin/python tools/playtest/run.py --session "$S" --headless engine/build/scd_headless`. Expected: `ok: session reproduces at all N checkpoints`.
4. Repeat with `--justifier`. On the title screen open Options, then Gun Adjust, and shoot with the mouse (left click) several times. Then quit. Expected: `session.replay` has `g0x...` lines, and `run.py --session` reports `ok`.
5. `engine/build/snatcher "$CUE" --no-session`, then quit. Expected: no new folder under `work/sessions/`.
6. Start a session, then drop the same `.cue` onto the window. Expected: the first folder's manifest says `"exit": "disc_changed"`, and a second folder appears.
7. `engine/build/snatcher "$CUE" --record-input work/rec.replay`, play briefly, quit. Expected: `saved work/rec.replay` on stderr, a session folder as usual, and `work/rec.replay` equal to that session's `session.replay` (`cmp`).

If step 3 or 4 reports `DIVERGED`, the live app depends on something besides recorded input. Report the first frame and `scene=`; do not commit a workaround. Record the finding in `docs/JOURNAL.md`.

- [ ] **Step 9: Final checks and commit**

```bash
mise run test-playtest
tools/test_intro_skip.sh engine/build/scd_headless
git status --short
```

Expected:
- All unit and end-to-end tests pass, and the intro-skip regression passes.
- `git status` lists only the files below.
- Nothing under `work/`, `extracted/`, `engine/build*` or `dist/` appears, and no `.bram`, session or generated file. Those paths are gitignored. If any appear, stop.

```bash
git add engine/src/frontend/sdl_main.cpp engine/CMakeLists.txt docs/PLAYTEST.md docs/PLAYING.md \
  tools/package/PLAYTEST_README.txt tools/package/PLAYTEST_README_PC.txt CLAUDE.md
git commit -m "app: always-on session recording, F8/F10 markers (docs/PLAYTEST.md Sessions)"
```

---

## Self-review

**Spec coverage:**

| Spec requirement | Task |
|---|---|
| Always on, `--no-session`, `--session-dir`, `--record-input` alias | 7 (folder helpers in 5) |
| Default parent dir (repo `work/sessions/` vs per-user `sessions/`), path printed and logged | 5 (`default_session_parent`, `begin` logs), 7 |
| `<YYYYMMDD-HHMMSS>[-N]` folder names | 5 (`create_session_dir`, unit test) |
| Disc drop closes the session and opens a new one; no disc means no session | 7 |
| A folder that cannot be created means no session, play continues | 5 (unit test), 7 (`open_session` error path) |
| `manifest.json`: start, rewrite at exit, `null`s on a crash, git stamp, `build` from `translated()` | 4, 5 (`test_session_survives_kill`), 7 |
| Replay v2, gun events on change, `parse` v1+v2, v1 justifier refused, v2 accepted | 1, 2 |
| Gun applied at the same `run_frame` point as the pad | 2 (`test_gun_replay_matches_live_gun`) |
| Incremental flush (at most 1 s, and at exit) | 5 (`flush_ms`, kill test) |
| Live checkpoints through a shared unit (scene change, every 300 frames, exit) | 3, 5 |
| F8/F10 markers via a shared `debug_dump` unit, log line, flash; F10 keeps the state-in-log behaviour | 3, 5, 7 |
| Live watchdog: hang means one bundle and play continues with re-arm; halted means a bundle and `exit: "halted"`; still screen is logged only | 5, 7 |
| `dump_state` text variant (`state.txt`) | 3 |
| `run.py --session`: 0/1/4, first frame, `--strict-ram`, build-kind warning | 6 |
| `docs/PLAYTEST.md` Sessions section (where, contents, verify, promote) | 7 |
| Testing: replay unit tests, `--session-out` e2e + `run.py --session`, `SCD_TEST_STICK_E020` gives exactly one hang bundle, manifest `git_commit`/`build`, manual app check | 1, 4, 5, 6, 7 |
| Error handling: a write failure drops only that file | 5 (`OutFile`, `test_unwritable_file_is_dropped`) |

**Decisions where the spec was open:**
- The pref path keeps the app's existing `SDL_GetPrefPath("snatcher-port", "snatcher")`, next to `bram.bin` and `debug.log`, rather than `("snatcher", "snatcher")`.
- `git_commit` uses `git rev-parse --short HEAD` plus `git status --porcelain --untracked-files=no` for dirty, not `git describe --always --dirty`. The repository has tags, so `describe` would print `v0.1.0-rc1-N-g<hash>`, and the spec's test compares with `rev-parse --short`. The fallbacks are `$GITHUB_SHA` (CI), then `unknown`.
- `--session-out DIR` is the session folder itself, and it cannot be combined with `--watch` or `--replay`. Headless flushes on emulated time so tests are deterministic.
- `run.py` learns the headless build kind through a new `--manifest-out`.
- The manifest gains `ckpt_every`. It is also rewritten on each marker or failure, so the counts survive a crash.
- `--record-input` forces a session even with `--no-session`, and exports the first session's replay.
- The kill test uses `SCD_TEST_KILL_AT` (`std::_Exit(70)`) instead of a signal, to avoid crash reporters.
- Headless `--watch` bundles also gain `state.txt`.

**Placeholder scan:** no TBD/TODO. Every code step has the code, and every run step has its expected result. The JOURNAL entry in Task 7 Step 8 applies only if verification fails, and its content is the observed frame.

**Type consistency:**
- `GunState{x, y, inside, buttons}` (Task 1) is used in `System::run_frame` (Task 2) and `headless.cpp` (Task 2).
- `Checkpointer::after_frame/final_line` (Task 3) are used in headless (Task 3) and `Session` (Task 5).
- `write_failure_bundle(System&, dir, Replay, WatchVerdict)` and `write_marker(System&, dir, time)` (Task 3) are used in `Session` (Task 5).
- `Manifest` fields and `manifest_json` (Task 4) are used in headless (Task 4) and `Session` (Task 5).
- `SessionConfig`/`Session` (Task 5) are used in headless (Task 5) and `sdl_main` (Task 7).
- `HangLatch::fire(const WatchVerdict&, uint16_t)` (Task 5) and `System::fb_hash()`/`cd_speed()`/`state_text()` match their declarations.
