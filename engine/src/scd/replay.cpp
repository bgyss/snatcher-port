#include "replay.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "system.h"   // Button enum

namespace scd {
namespace {
const struct { uint16_t bit; char c; } kBtn[] = {{kUp, 'U'}, {kDown, 'D'}, {kLeft, 'L'}, {kRight, 'R'}, {kB, 'B'}, {kC, 'C'}, {kA, 'A'}, {kStart, 'S'}};

struct Sha1 {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    uint8_t buf[64];
    size_t fill = 0;
    uint64_t total = 0;
    static uint32_t rol(uint32_t v, int n) { return v << n | v >> (32 - n); }
    void block(const uint8_t* p) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) w[i] = uint32_t(p[4 * i]) << 24 | p[4 * i + 1] << 16 | p[4 * i + 2] << 8 | p[4 * i + 3];
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    void update(const uint8_t* p, size_t n) {
        total += n;
        while (n) {
            size_t take = std::min(n, sizeof buf - fill);
            std::copy(p, p + take, buf + fill);
            fill += take; p += take; n -= take;
            if (fill == 64) { block(buf); fill = 0; }
        }
    }
    std::string finish() {
        uint64_t bits = total * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t z = 0;
        while (fill != 56) update(&z, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; ++i) len[i] = uint8_t(bits >> (56 - 8 * i));
        update(len, 8);
        char out[41];
        for (int i = 0; i < 5; ++i) std::snprintf(out + 8 * i, 9, "%08x", h[i]);
        return out;
    }
};
}  // namespace

uint16_t buttons_from_string(const std::string& s) {
    uint16_t b = 0;
    for (char c : s)
        for (auto& k : kBtn) if (k.c == c) b |= k.bit;
    return b;
}

std::string buttons_to_string(uint16_t b) {
    std::string s;
    for (auto& k : kBtn) if (b & k.bit) s += k.c;
    return s.empty() ? "-" : s;
}

void Replay::add(uint64_t frame, uint16_t buttons) {
    uint16_t cur = events.empty() ? 0 : events.back().buttons;
    if (buttons == cur) return;
    events.push_back({frame, buttons});
}

uint16_t Replay::buttons_at(uint64_t frame) const {
    auto it = std::upper_bound(events.begin(), events.end(), frame, [](uint64_t f, const ReplayEvent& e) { return f < e.frame; });
    return it == events.begin() ? 0 : std::prev(it)->buttons;
}

std::string Replay::serialize() const {
    std::ostringstream o;
    o << "# snatcher-replay v1\n";
    o << "disc sha1=" << disc_sha1 << "\n";
    o << "bram sha1=" << bram_sha1 << "\n";
    o << "engine " << engine << "\n";
    o << "cdspeed " << cd_speed << "\n";
    o << "justifier " << (justifier ? "on" : "off") << "\n";
    for (auto& e : events) {
        char line[64];
        std::snprintf(line, sizeof line, "@0x%llx %s\n", (unsigned long long)e.frame, buttons_to_string(e.buttons).c_str());
        o << line;
    }
    return o.str();
}

bool Replay::parse(const std::string& text, Replay* out, std::string* err) {
    auto fail = [&](int line, const std::string& m) { if (err) *err = "line " + std::to_string(line) + ": " + m; return false; };
    std::istringstream in(text);
    std::string line;
    int n = 0;
    Replay r;
    bool header = false;
    while (std::getline(in, line)) {
        ++n;
        if (!line.empty() && line.back() == '\r') line.pop_back();   // CRLF checkouts / Windows-written files
        if (line.empty()) continue;
        if (line[0] == '#') {
            if (line != "# snatcher-replay v1") return fail(n, "unsupported or malformed header");
            header = true;
            continue;
        }
        if (!header) return fail(n, "missing '# snatcher-replay v1' header");
        if (line[0] == '@') {
            char* end;
            if (line.size() < 2 || !std::isdigit(static_cast<unsigned char>(line[1]))) return fail(n, "bad frame");
            unsigned long long f = std::strtoull(line.c_str() + 1, &end, 0);
            if (*end != ' ') return fail(n, "bad frame");
            std::string b = end + 1;
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
    if (!header) return fail(1, "missing '# snatcher-replay v1' header");
    *out = std::move(r);
    return true;
}

std::string sha1_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "none";
    Sha1 s;
    char buf[65536];
    while (f.read(buf, sizeof buf) || f.gcount() > 0) s.update(reinterpret_cast<uint8_t*>(buf), size_t(f.gcount()));
    return s.finish();
}

std::string disc_sha1_from_cue(const std::string& cue_path) {
    std::ifstream f(cue_path);
    std::string line;
    while (std::getline(f, line)) {
        size_t p = line.find("FILE");
        size_t q1 = line.find('"');
        size_t q2 = line.find('"', q1 + 1);
        if (p == std::string::npos || q1 == std::string::npos || q2 == std::string::npos) continue;
        std::string name = line.substr(q1 + 1, q2 - q1 - 1);
        size_t slash = cue_path.find_last_of("/\\");
        return sha1_file((slash == std::string::npos ? std::string() : cue_path.substr(0, slash + 1)) + name);
    }
    return "none";
}

}  // namespace scd
