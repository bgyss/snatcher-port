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
