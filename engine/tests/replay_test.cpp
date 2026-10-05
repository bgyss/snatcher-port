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

int main() {
    test_roundtrip();
    test_lookup();
    test_add_dedups_equal_consecutive();
    test_buttons();
    test_parse_errors();
    test_parse_accepts_crlf_and_rejects_negative_frame();
    test_sha1();
    std::puts("replay_test: ok");
    return 0;
}
