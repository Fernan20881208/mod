#pragma once

// extra macro formats, all byte-level implementations read straight off the
// source of each bot's own reader/writer, no libraries needed
//
//   .gdr  -> GDR v1, what xdBot saves. msgpack (nlohmann layout) or json
//   .xd   -> old xdBot files, same GDR v1 container
//   .slc  -> Silicate. "SILL" magic = slc v2, "SLC3RPLY" = slc v3
//
// every parser funnels into the same gdr2::Replay the rest of the mod uses.
// frames are re-based through any mid-macro TPS changes so timing stays exact
// even when the tickrate moves.

#include "gdr2.hpp"

#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace fmts {

// ---------------------------------------------------------------- tiny value

// just enough dynamic value to hold what gdr v1 files contain
struct Val {
    enum Kind { Nil, Bool, Int, Dbl, Str, Arr, Map } kind = Nil;
    bool b = false;
    int64_t i = 0;
    double d = 0;
    std::string s;
    std::vector<Val> arr;
    std::map<std::string, Val> map;

    double num() const { return kind == Int ? double(i) : (kind == Dbl ? d : 0.0); }
    bool truthy() const { return kind == Bool ? b : num() != 0.0; }
    Val const* get(char const* k) const {
        if (kind != Map) return nullptr;
        auto it = map.find(k);
        return it == map.end() ? nullptr : &it->second;
    }
};

// ---------------------------------------------------------------- msgpack

// minimal msgpack reader covering everything nlohmann's to_msgpack emits for
// gdr v1 data: maps, arrays, strings, ints, floats, bools, nil, bin
class MsgUnpack {
    const uint8_t* p;
    const uint8_t* end;
    bool bad = false;

    bool need(size_t n) { if (size_t(end - p) < n) { bad = true; return false; } return true; }

    uint64_t beUint(int n) {
        if (!need(n)) return 0;
        uint64_t v = 0;
        for (int i = 0; i < n; ++i) v = (v << 8) | *p++;
        return v;
    }

public:
    MsgUnpack(const uint8_t* data, size_t len) : p(data), end(data + len) {}
    bool failed() const { return bad; }

    Val value() {
        Val v;
        if (!need(1)) return v;
        const uint8_t t = *p++;

        if (t <= 0x7f) { v.kind = Val::Int; v.i = t; return v; }                 // pos fixint
        if (t >= 0xe0) { v.kind = Val::Int; v.i = int8_t(t); return v; }         // neg fixint
        if ((t & 0xf0) == 0x80) return mapv(t & 0x0f);                            // fixmap
        if ((t & 0xf0) == 0x90) return arrv(t & 0x0f);                            // fixarray
        if ((t & 0xe0) == 0xa0) return strv(t & 0x1f);                            // fixstr

        switch (t) {
            case 0xc0: return v;                                                  // nil
            case 0xc2: v.kind = Val::Bool; v.b = false; return v;
            case 0xc3: v.kind = Val::Bool; v.b = true;  return v;
            case 0xc4: return strv(beUint(1));                                    // bin8, treat as str
            case 0xc5: return strv(beUint(2));
            case 0xc6: return strv(beUint(4));
            case 0xca: {                                                          // float32
                uint32_t u = uint32_t(beUint(4));
                float f; std::memcpy(&f, &u, 4);
                v.kind = Val::Dbl; v.d = f; return v;
            }
            case 0xcb: {                                                          // float64
                uint64_t u = beUint(8);
                double d; std::memcpy(&d, &u, 8);
                v.kind = Val::Dbl; v.d = d; return v;
            }
            case 0xcc: v.kind = Val::Int; v.i = int64_t(beUint(1)); return v;     // uint8..64
            case 0xcd: v.kind = Val::Int; v.i = int64_t(beUint(2)); return v;
            case 0xce: v.kind = Val::Int; v.i = int64_t(beUint(4)); return v;
            case 0xcf: v.kind = Val::Int; v.i = int64_t(beUint(8)); return v;
            case 0xd0: v.kind = Val::Int; v.i = int8_t(beUint(1));  return v;     // int8..64
            case 0xd1: v.kind = Val::Int; v.i = int16_t(beUint(2)); return v;
            case 0xd2: v.kind = Val::Int; v.i = int32_t(beUint(4)); return v;
            case 0xd3: v.kind = Val::Int; v.i = int64_t(beUint(8)); return v;
            case 0xd9: return strv(beUint(1));                                    // str8..32
            case 0xda: return strv(beUint(2));
            case 0xdb: return strv(beUint(4));
            case 0xdc: return arrv(beUint(2));                                    // array16/32
            case 0xdd: return arrv(beUint(4));
            case 0xde: return mapv(beUint(2));                                    // map16/32
            case 0xdf: return mapv(beUint(4));
            default: bad = true; return v;
        }
    }

private:
    Val strv(uint64_t n) {
        Val v; v.kind = Val::Str;
        if (!need(n)) return v;
        v.s.assign(reinterpret_cast<const char*>(p), size_t(n));
        p += n;
        return v;
    }
    Val arrv(uint64_t n) {
        Val v; v.kind = Val::Arr;
        v.arr.reserve(size_t(n));
        for (uint64_t i = 0; i < n && !bad; ++i) v.arr.push_back(value());
        return v;
    }
    Val mapv(uint64_t n) {
        Val v; v.kind = Val::Map;
        for (uint64_t i = 0; i < n && !bad; ++i) {
            Val k = value();
            Val val = value();
            if (k.kind == Val::Str) v.map.emplace(std::move(k.s), std::move(val));
        }
        return v;
    }
};

// ---------------------------------------------------------------- mini json

// small strict-enough json reader for the same shape of data, used when a
// gdr file was exported as json instead of msgpack
class JsonParse {
    const char* p;
    const char* end;
    bool bad = false;

    void ws() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p; }
    bool lit(char const* s) {
        const size_t n = std::strlen(s);
        if (size_t(end - p) < n || std::memcmp(p, s, n) != 0) { bad = true; return false; }
        p += n; return true;
    }

public:
    JsonParse(const char* data, size_t len) : p(data), end(data + len) {}
    bool failed() const { return bad; }

    Val value() {
        ws();
        Val v;
        if (p >= end) { bad = true; return v; }
        switch (*p) {
            case '{': return obj();
            case '[': return arr();
            case '"': v.kind = Val::Str; v.s = str(); return v;
            case 't': lit("true");  v.kind = Val::Bool; v.b = true;  return v;
            case 'f': lit("false"); v.kind = Val::Bool; v.b = false; return v;
            case 'n': lit("null"); return v;
            default:  return num();
        }
    }

private:
    std::string str() {
        std::string out;
        if (*p != '"') { bad = true; return out; }
        ++p;
        while (p < end && *p != '"') {
            char c = *p++;
            if (c == '\\' && p < end) {
                char e = *p++;
                switch (e) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u': {
                        // decode the codepoint, keep it simple: ascii lands as
                        // itself, anything above gets a ? which is fine for a
                        // filename listing
                        if (size_t(end - p) < 4) { bad = true; return out; }
                        unsigned cp = 0;
                        for (int i = 0; i < 4; ++i) {
                            char h = *p++;
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= h - '0';
                            else if (h >= 'a' && h <= 'f') cp |= h - 'a' + 10;
                            else if (h >= 'A' && h <= 'F') cp |= h - 'A' + 10;
                            else { bad = true; return out; }
                        }
                        out += cp < 128 ? char(cp) : '?';
                        break;
                    }
                    default: out += e;
                }
            } else out += c;
        }
        if (p < end) ++p; else bad = true;
        return out;
    }

    Val num() {
        Val v;
        char* np = nullptr;
        v.d = std::strtod(p, &np);
        if (np == p) { bad = true; return v; }
        // integers stay integers so ids do not round
        bool isInt = true;
        for (const char* q = p; q < np; ++q)
            if (*q == '.' || *q == 'e' || *q == 'E') { isInt = false; break; }
        if (isInt) { v.kind = Val::Int; v.i = int64_t(v.d); }
        else v.kind = Val::Dbl;
        p = np;
        return v;
    }

    Val arr() {
        Val v; v.kind = Val::Arr;
        ++p; ws();
        if (p < end && *p == ']') { ++p; return v; }
        while (p < end && !bad) {
            v.arr.push_back(value());
            ws();
            if (p < end && *p == ',') { ++p; continue; }
            if (p < end && *p == ']') { ++p; return v; }
            bad = true;
        }
        return v;
    }

    Val obj() {
        Val v; v.kind = Val::Map;
        ++p; ws();
        if (p < end && *p == '}') { ++p; return v; }
        while (p < end && !bad) {
            ws();
            std::string k = str();
            ws();
            if (p >= end || *p != ':') { bad = true; return v; }
            ++p;
            v.map.emplace(std::move(k), value());
            ws();
            if (p < end && *p == ',') { ++p; continue; }
            if (p < end && *p == '}') { ++p; return v; }
            bad = true;
        }
        return v;
    }
};

// ------------------------------------------------- tps-aware frame rebasing

// inputs arrive as (frame, tps changes along the way). everything downstream
// wants frames on ONE clock, so convert through seconds and re-emit against
// the starting tps. with no tps changes this is the identity.
struct TickInput {
    uint64_t frame;
    bool down;
    bool p2;
    int button;
};

struct TpsChange { uint64_t frame; double tps; };

inline void rebase(std::vector<TickInput> const& raw,
                   std::vector<TpsChange> const& changes,
                   double startTps, gdr2::Replay& out) {
    const double outFps = startTps > 1.0 ? startTps : 240.0;
    out.framerate = outFps;

    double lastSec = 0.0;
    uint64_t lastFrame = 0;
    double tps = outFps;
    size_t ci = 0;

    auto secondsAt = [&](uint64_t f) {
        while (ci < changes.size() && changes[ci].frame <= f) {
            lastSec += double(changes[ci].frame - lastFrame) / tps;
            lastFrame = changes[ci].frame;
            if (changes[ci].tps > 1.0) tps = changes[ci].tps;
            ++ci;
        }
        return lastSec + double(f - lastFrame) / tps;
    };

    out.inputs.reserve(raw.size());
    for (auto const& t : raw) {
        gdr2::Input in;
        in.frame = uint64_t(std::llround(secondsAt(t.frame) * outFps));
        in.down = t.down;
        in.player2 = t.p2;
        in.button = t.button;
        out.inputs.push_back(in);
    }
}

// ---------------------------------------------------------------- gdr v1

// what xdBot writes. exact field set read off maxnut/GDReplayFormat gdr.hpp:
// gameVersion, description, version, duration, bot{name,version},
// level{id,name}, author, seed, coins, ldm, framerate,
// inputs[]{frame, btn, 2p, down}
inline std::optional<gdr2::Replay> parseGDR1(std::vector<uint8_t> const& data) {
    if (data.empty()) return std::nullopt;

    Val root;
    // sniff: json starts with '{' (allow leading whitespace), else msgpack
    size_t w = 0;
    while (w < data.size() && (data[w] == ' ' || data[w] == '\n'
           || data[w] == '\r' || data[w] == '\t')) ++w;

    if (w < data.size() && data[w] == '{') {
        JsonParse j(reinterpret_cast<const char*>(data.data()), data.size());
        root = j.value();
        if (j.failed()) return std::nullopt;
    } else {
        MsgUnpack m(data.data(), data.size());
        root = m.value();
        if (m.failed()) return std::nullopt;
    }
    if (root.kind != Val::Map) return std::nullopt;

    auto inputsV = root.get("inputs");
    if (!inputsV || inputsV->kind != Val::Arr) return std::nullopt;

    gdr2::Replay rep;
    rep.version = 1;
    if (auto v = root.get("author")) rep.author = v->s;
    if (auto v = root.get("description")) rep.description = v->s;
    if (auto v = root.get("duration")) rep.duration = float(v->num());
    if (auto v = root.get("framerate")) rep.framerate = v->num();
    if (rep.framerate <= 1.0) rep.framerate = 240.0;
    if (auto v = root.get("seed")) rep.seed = uint64_t(v->num());
    if (auto v = root.get("coins")) rep.coins = int(v->num());
    if (auto v = root.get("ldm")) rep.ldm = v->truthy();
    if (auto bot = root.get("bot")) {
        if (auto v = bot->get("name")) rep.botName = v->s;
        if (auto v = bot->get("version")) rep.botVersion = int(v->num());
    }
    if (auto lvl = root.get("level")) {
        if (auto v = lvl->get("id")) rep.levelID = uint32_t(v->num());
        if (auto v = lvl->get("name")) rep.levelName = v->s;
    }

    for (auto const& iv : inputsV->arr) {
        if (iv.kind != Val::Map) continue;
        gdr2::Input in;
        if (auto v = iv.get("frame")) in.frame = uint64_t(v->num());
        if (auto v = iv.get("btn"))   in.button = int(v->num());
        if (auto v = iv.get("2p"))    in.player2 = v->truthy();
        if (auto v = iv.get("down"))  in.down = v->truthy();
        // gdr v1 button 1 = jump, that is the one indicators care about, but
        // platformer moves are kept too so holds() can pair them
        rep.inputs.push_back(in);
    }
    if (rep.inputs.empty()) return std::nullopt;
    std::sort(rep.inputs.begin(), rep.inputs.end(),
              [](gdr2::Input const& a, gdr2::Input const& b) { return a.frame < b.frame; });
    return rep;
}

// ---------------------------------------------------------------- slc v2

// "SILL" | tps f64 | metaSize u64 | meta bytes | count u64 | blobCount u64 |
// blobs (byteSize,start,length as u64 each) | blob states | "EOM"
// state = delta<<5 | type<<2 | p2<<1 | hold, tps inputs carry a f64 after
inline std::optional<gdr2::Replay> parseSLC2(std::vector<uint8_t> const& d) {
    size_t o = 0;
    auto rd = [&](void* dst, size_t n) -> bool {
        if (o + n > d.size()) return false;
        std::memcpy(dst, d.data() + o, n);
        o += n;
        return true;
    };
    auto rd64 = [&](uint64_t& v) { return rd(&v, 8); };

    char magic[4];
    if (!rd(magic, 4) || std::memcmp(magic, "SILL", 4) != 0) return std::nullopt;

    double tps = 240.0;
    if (!rd(&tps, 8)) return std::nullopt;

    uint64_t metaSize = 0;
    if (!rd64(metaSize) || o + metaSize > d.size()) return std::nullopt;
    o += metaSize;   // bot specific, skip

    uint64_t count = 0, blobCount = 0;
    if (!rd64(count) || count > 50'000'000) return std::nullopt;
    if (!rd64(blobCount) || blobCount > 50'000'000) return std::nullopt;

    struct Blob { uint64_t bytes, start, len; };
    std::vector<Blob> blobs;
    blobs.resize(size_t(blobCount));
    for (auto& b : blobs) {
        if (!rd64(b.bytes) || !rd64(b.start) || !rd64(b.len)) return std::nullopt;
        if (b.bytes == 0 || b.bytes > 8) return std::nullopt;
    }

    std::vector<TickInput> ticks;
    std::vector<TpsChange> changes;
    ticks.reserve(size_t(count));

    uint64_t frame = 0;
    for (auto const& b : blobs) {
        for (uint64_t i = 0; i < b.len; ++i) {
            uint64_t state = 0;
            if (!rd(&state, size_t(b.bytes))) return std::nullopt;

            const uint64_t delta = state >> 5;
            const int type = int((state & 0b11100) >> 2);
            const bool p2 = (state & 2) != 0;
            const bool hold = (state & 1) != 0;
            frame += delta;

            if (type == 7) {                      // tps change, payload follows
                double newTps = 0;
                if (!rd(&newTps, 8)) return std::nullopt;
                changes.push_back({ frame, newTps });
            } else if (type >= 1 && type <= 3) {  // jump / left / right
                ticks.push_back({ frame, hold, p2, type });
            }
            // skip / death / restart carry nothing we can display
        }
    }

    if (ticks.empty()) return std::nullopt;

    gdr2::Replay rep;
    rep.version = 2;
    rep.botName = "Silicate";
    rebase(ticks, changes, tps, rep);
    return rep;
}

// ---------------------------------------------------------------- slc v3

// "SLC3RPLY" | metaSize u16 | meta (f64 tps first) | atoms | 0xCC
// atom: id u32, size u64 (top byte = flags), payload
// ActionAtom (id 1): count u64, then sections until count actions decoded
inline std::optional<gdr2::Replay> parseSLC3(std::vector<uint8_t> const& d) {
    size_t o = 0;
    auto rd = [&](void* dst, size_t n) -> bool {
        if (o + n > d.size()) return false;
        std::memcpy(dst, d.data() + o, n);
        o += n;
        return true;
    };

    char magic[8];
    if (!rd(magic, 8) || std::memcmp(magic, "SLC3RPLY", 8) != 0) return std::nullopt;

    uint16_t metaSize = 0;
    if (!rd(&metaSize, 2) || metaSize < 8 || o + metaSize > d.size()) return std::nullopt;

    double tps = 240.0;
    std::memcpy(&tps, d.data() + o, 8);   // first meta field is m_tps
    o += metaSize;

    std::vector<TickInput> ticks;
    std::vector<TpsChange> changes;

    auto readSections = [&](size_t payloadEnd, uint64_t wantActions) -> bool {
        uint64_t made = 0;
        uint64_t frame = 0;

        auto emit = [&](uint64_t delta, int btn, bool hold, bool p2) {
            frame += delta;
            if (btn == 0) {                       // swift: press + instant lift
                ticks.push_back({ frame, true,  p2, 1 });
                ticks.push_back({ frame, false, p2, 1 });
                made += 2;
            } else {
                ticks.push_back({ frame, hold, p2, btn });
                made += 1;
            }
        };

        while (made < wantActions && o + 2 <= payloadEnd) {
            uint16_t head = 0;
            if (!rd(&head, 2)) return false;
            const int id = head >> 14;

            if (id == 0 || id == 1) {             // input / repeat
                const uint32_t deltaSize = (head >> 12) & 0b11;
                const uint32_t countExp  = (head >> 8) & 0b1111;
                const uint64_t bytes = 1ull << deltaSize;
                const uint64_t n = 1ull << countExp;
                const uint64_t repeats = id == 1 ? (1ull << ((head >> 3) & 0b11111)) : 1;

                struct PIn { uint64_t delta; int btn; bool hold, p2; };
                std::vector<PIn> block;
                block.reserve(size_t(n));
                for (uint64_t i = 0; i < n; ++i) {
                    uint64_t state = 0;
                    if (!rd(&state, size_t(bytes))) return false;
                    block.push_back({ state >> 4, int((state >> 2) & 0b11),
                                      (state & 1) != 0, (state & 2) != 0 });
                }
                for (uint64_t r = 0; r < repeats && made < wantActions; ++r)
                    for (auto const& p : block) {
                        if (made >= wantActions) break;
                        emit(p.delta, p.btn, p.hold, p.p2);
                    }
            } else if (id == 2) {                 // special
                const uint32_t deltaSize = (head >> 8) & 0b11;
                const int special = (head >> 10) & 0b1111;
                uint64_t delta = 0;
                if (!rd(&delta, size_t(1ull << deltaSize))) return false;
                frame += delta;
                ++made;
                if (special == 3) {               // tps
                    double newTps = 0;
                    if (!rd(&newTps, 8)) return false;
                    changes.push_back({ frame, newTps });
                } else if (special <= 2) {        // restart / restartfull / death
                    uint64_t seed = 0;
                    if (!rd(&seed, 8)) return false;
                }
                // bugpoint carries nothing
            } else {
                return false;
            }
        }
        o = payloadEnd;                            // land exactly on the next atom
        return true;
    };

    while (o + 12 <= d.size()) {
        if (d[o] == 0xCC && o + 1 == d.size()) break;   // footer

        uint32_t id = 0;
        uint64_t size = 0;
        if (!rd(&id, 4) || !rd(&size, 8)) return std::nullopt;
        size &= ~(0xFFull << 56);                       // top byte is flags
        if (o + size > d.size()) return std::nullopt;
        const size_t payloadEnd = o + size_t(size);

        if (id == 1) {                                   // ActionAtom
            uint64_t count = 0;
            if (!rd(&count, 8) || count > 50'000'000) return std::nullopt;
            if (!readSections(payloadEnd, count)) return std::nullopt;
        } else {
            o = payloadEnd;                              // unknown atom, skip
        }
    }

    if (ticks.empty()) return std::nullopt;

    gdr2::Replay rep;
    rep.version = 3;
    rep.botName = "Silicate";
    rebase(ticks, changes, tps, rep);
    return rep;
}

// ---------------------------------------------------------------- dispatch

// magic first, extension only as a tiebreak. gdr2 keeps its own parser.
inline std::optional<gdr2::Replay> parseAny(std::vector<uint8_t> const& data) {
    if (data.size() >= 4 && std::memcmp(data.data(), "GDR", 3) == 0)
        return gdr2::parse(data);
    if (data.size() >= 8 && std::memcmp(data.data(), "SLC3RPLY", 8) == 0)
        return parseSLC3(data);
    if (data.size() >= 4 && std::memcmp(data.data(), "SILL", 4) == 0)
        return parseSLC2(data);
    return parseGDR1(data);    // msgpack or json, sniffed inside
}

inline bool knownExtension(std::string const& ext) {
    return ext == ".gdr2" || ext == ".gdr" || ext == ".xd" || ext == ".slc";
}

} // namespace fmts
