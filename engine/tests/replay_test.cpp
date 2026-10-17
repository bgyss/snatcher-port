#undef NDEBUG   // tests rely on assert even in Release builds
#include <cassert>
#include <cstdio>
#include <string>

#include "replay.h"
#include "system.h"   // Button enum

using namespace scd;

static void test_roundtrip() {
    Replay r;
    r.disc_sha1 = "abc123";
    r.engine = "test";
    r.add(0x120, kUp);
    r.add(0x127, 0);
    r.add(0x600, kStart | kC);
    Replay p;
    std::string err;
    assert(Replay::parse(r.serialize(), &p, &err));
    assert(p.disc_sha1 == "abc123" && p.bram_sha1 == "none");
    assert(p.events.size() == 3 && p.events[2].frame == 0x600 && p.events[2].buttons == (kStart | kC));
}

static void test_lookup() {
    Replay r;
    r.add(10, kUp);
    r.add(20, 0);
    assert(r.buttons_at(0) == 0);
    assert(r.buttons_at(9) == 0);
    assert(r.buttons_at(10) == kUp);
    assert(r.buttons_at(19) == kUp);
    assert(r.buttons_at(20) == 0);
    assert(r.buttons_at(1000000) == 0);
}

static void test_add_dedups_equal_consecutive() {
    Replay r;
    r.add(5, kUp);
    r.add(6, kUp);   // no change: not stored
    r.add(7, 0);
    assert(r.events.size() == 2);
}

static void test_buttons() {
    assert(buttons_from_string("-") == 0 && buttons_from_string("") == 0);
    assert(buttons_from_string("SC") == (kStart | kC));
    assert(buttons_to_string(kUp | kDown) == "UD");
    assert(buttons_to_string(0) == "-");
}

static void test_parse_errors() {
    Replay p;
    std::string err;
    assert(!Replay::parse("", &p, &err));                                   // no header
    assert(!Replay::parse("# snatcher-replay v1\n@0x10 Z\n", &p, &err));    // bad button
    assert(err.find("line 2") != std::string::npos);
    assert(!Replay::parse("# snatcher-replay v1\n@0x20 U\n@0x10 -\n", &p, &err));   // unsorted
    assert(err.find("line 3") != std::string::npos);
    assert(!Replay::parse("# snatcher-replay v9\n", &p, &err));             // unknown version
}

static void test_parse_accepts_crlf_and_rejects_negative_frame() {
    Replay p;
    std::string err;
    assert(Replay::parse("# snatcher-replay v1\r\ndisc sha1=abc\r\n@0x10 U\r\n@0x20 -\r\n", &p, &err));
    assert(p.disc_sha1 == "abc" && p.events.size() == 2 && p.events[1].buttons == 0);
    assert(!Replay::parse("# snatcher-replay v1\n@-1 S\n", &p, &err));
    assert(!Replay::parse("# snatcher-replay v1\n@0x10 \n", &p, &err));   // empty button field
}

static void test_sha1() {
    const char* path = "replay_test_sha1.tmp";
    FILE* f = std::fopen(path, "wb");
    std::fputs("abc", f);
    std::fclose(f);
    assert(sha1_file(path) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    std::remove(path);
    assert(sha1_file("does/not/exist") == "none");
}

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

int main() {
    test_roundtrip();
    test_lookup();
    test_add_dedups_equal_consecutive();
    test_buttons();
    test_parse_errors();
    test_parse_accepts_crlf_and_rejects_negative_frame();
    test_sha1();
    test_v2_gun_roundtrip();
    test_gun_at_boundaries();
    test_v1_parses_unchanged();
    test_gun_parse_errors();
    test_incremental_events_concatenate();
    std::puts("replay_test: ok");
    return 0;
}
