#pragma once
// gdr2.hpp - dependency-free reader for GDReplayFormat v2 (.gdr2)
// Spec: https://github.com/maxnut/GDReplayFormat (gdr2 branch)
// Verified byte-exact against MEGA-recorded macros.

#include <cstdint>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <optional>
#include <algorithm>

namespace gdr2 {

struct Input {
    uint64_t frame = 0;
    uint8_t  button = 1;   // 1 = jump, 2 = left, 3 = right
    bool     player2 = false;
    bool     down = false;
};

// A resolved press/release pair. length == 0 means a single-frame tap.
struct Hold {
    uint64_t start = 0;
    uint64_t end = 0;
    uint8_t  button = 1;
    bool     player2 = false;
    uint64_t length() const { return end - start; }
};

struct Replay {
    int         version = 0;
    std::string inputTag;
    std::string author;
    std::string description;
    float       duration = 0.f;      // seconds, often 0 (bots skip it)
    int         gameVersion = 0;
    double      framerate = 240.0;
    int64_t     seed = 0;
    int         coins = 0;
    bool        ldm = false;
    bool        platformer = false;
    std::string botName;
    int         botVersion = 0;
    uint32_t    levelID = 0;
    std::string levelName;

    std::vector<uint64_t> deaths;
    std::vector<Input>    inputs;    // sorted by frame after parse

    uint64_t lastFrame() const { return inputs.empty() ? 0 : inputs.back().frame; }
    double   seconds()   const { return framerate > 0 ? lastFrame() / framerate : 0.0; }

    // Collapse press/release pairs. Unclosed presses run to `tail` frames past
    // the last input so a macro that ends mid-hold still draws something.
    std::vector<Hold> holds(uint64_t tail = 30) const {
        std::vector<Hold> out;
        // key: player2 * 4 + button
        std::optional<Input> open[8];
        for (auto const& in : inputs) {
            int k = (in.player2 ? 4 : 0) + (in.button & 3);
            if (in.down) {
                open[k] = in;
            } else if (open[k]) {
                out.push_back({open[k]->frame, in.frame, in.button, in.player2});
                open[k].reset();
            }
        }
        for (auto& o : open)
            if (o) out.push_back({o->frame, lastFrame() + tail, o->button, o->player2});
        std::sort(out.begin(), out.end(),
                  [](Hold const& a, Hold const& b) { return a.start < b.start; });
        return out;
    }
};

namespace detail {

struct Reader {
    const uint8_t* d; size_t n; size_t i = 0; bool bad = false;

    bool need(size_t k) { if (i + k > n) { bad = true; return false; } return true; }

    uint8_t u8() { if (!need(1)) return 0; return d[i++]; }

    uint64_t varint() {
        uint64_t v = 0; int shift = 0;
        for (int k = 0; k < 10; ++k) {
            if (!need(1)) return v;
            uint8_t b = d[i++];
            v |= uint64_t(b & 0x7f) << shift;
            if (!(b & 0x80)) return v;
            shift += 7;
        }
        bad = true; return v;
    }

    std::string str() {
        uint64_t len = varint();
        if (len > n || !need(size_t(len))) { bad = true; return {}; }
        std::string s(reinterpret_cast<const char*>(d + i), size_t(len));
        i += size_t(len);
        return s;
    }

    void skip(uint64_t k) { if (need(size_t(k))) i += size_t(k); }

    // libGDR writes IEEE floats most-significant-byte first. Fall back to
    // little-endian if the big-endian read is nonsense.
    template <class T> T real(T lo, T hi, T fallback) {
        constexpr size_t S = sizeof(T);
        if (!need(S)) return fallback;
        uint8_t be[S], le[S];
        std::memcpy(be, d + i, S);
        for (size_t k = 0; k < S; ++k) le[k] = be[S - 1 - k];
        i += S;
        T a, b;
        std::memcpy(&a, le, S);   // bytes reversed -> big-endian source
        std::memcpy(&b, be, S);   // as-is        -> little-endian source
        auto ok = [&](T v) { return std::isfinite(v) && v >= lo && v <= hi; };
        if (ok(a)) return a;
        if (ok(b)) return b;
        return fallback;
    }
};

} // namespace detail

// Returns nullopt if the buffer is not a valid GDR2 replay.
inline std::optional<Replay> parse(const uint8_t* data, size_t size) {
    if (size < 8 || std::memcmp(data, "GDR", 3) != 0) return std::nullopt;

    detail::Reader r{data, size, 3};
    Replay rep;

    rep.version = int(r.varint());
    if (rep.version != 2) return std::nullopt;   // v1 is JSON, handled elsewhere

    rep.inputTag    = r.str();
    rep.author      = r.str();
    rep.description = r.str();
    rep.duration    = r.real<float>(0.f, 1e6f, 0.f);
    rep.gameVersion = int(r.varint());
    rep.framerate   = r.real<double>(1.0, 1e5, 240.0);
    rep.seed        = int64_t(r.varint());
    rep.coins       = int(r.varint());
    rep.ldm         = r.u8() != 0;
    rep.platformer  = r.u8() != 0;
    rep.botName     = r.str();
    rep.botVersion  = int(r.varint());
    rep.levelID     = uint32_t(r.varint());
    rep.levelName   = r.str();

    r.skip(r.varint());                          // replay-level extension blob

    uint64_t deathCount = r.varint();
    if (deathCount > size) return std::nullopt;
    uint64_t f = 0;
    rep.deaths.reserve(size_t(deathCount));
    for (uint64_t k = 0; k < deathCount && !r.bad; ++k) {
        f += r.varint();
        rep.deaths.push_back(f);
    }

    uint64_t total = r.varint();
    uint64_t p1    = r.varint();
    if (r.bad || total > size * 8) return std::nullopt;

    rep.inputs.reserve(size_t(total));
    uint64_t frame = 0;
    for (uint64_t k = 0; k < total && !r.bad; ++k) {
        if (k == p1) frame = 0;                  // P2 stream restarts at 0

        uint64_t packed = r.varint();
        Input in;
        in.down = packed & 1;
        if (rep.platformer) {
            in.button = uint8_t((packed >> 1) & 3);
            frame += packed >> 3;
        } else {
            in.button = 1;
            frame += packed >> 1;
        }
        in.frame   = frame;
        in.player2 = k >= p1;
        rep.inputs.push_back(in);

        if (!rep.inputTag.empty()) r.skip(r.varint());   // per-input extension
    }
    if (r.bad) return std::nullopt;

    std::stable_sort(rep.inputs.begin(), rep.inputs.end(),
                     [](Input const& a, Input const& b) { return a.frame < b.frame; });
    return rep;
}

inline std::optional<Replay> parse(const std::vector<uint8_t>& buf) {
    return parse(buf.data(), buf.size());
}

} // namespace gdr2
