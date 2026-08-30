#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#if __has_include(<Geode/ui/GeodeUI.hpp>)
    #include <Geode/ui/GeodeUI.hpp>
    #define HAS_GEODE_UI 1
#endif
// cheat api ships its header under two different paths depending on version,
// so try both. if neither is there this compiles to nothing and the indicator
// silently never lights up, which is exactly what was happening.
#if __has_include(<legowiifun.cheat_api/include/cheatAPI.hpp>)
    #include <legowiifun.cheat_api/include/cheatAPI.hpp>
    #define HAS_CHEAT_API 1
#elif __has_include(<legowiifun.cheatAPI/include/cheatAPI.hpp>)
    #include <legowiifun.cheatAPI/include/cheatAPI.hpp>
    #define HAS_CHEAT_API 1
#endif

#include "gdr2.hpp"
#include "formats.hpp"

#include <climits>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <vector>

using namespace geode::prelude;

constexpr int kWorldZ = 500;
constexpr int kLaneZ  = 1000;
// a tap is short in TIME not frames, 1200tps macros exist and they broke this
constexpr double kDefaultTapMs = 45.0;

constexpr float kMaxStep = 0.1f;

// speed portals or sum
constexpr float kSpeeds[5] = { 251.16f, 311.58f, 387.42f, 468.00f, 576.00f };
constexpr int   kSpeedIDs[5] = { 200, 201, 202, 203, 1334 };

// how long stuff is
constexpr double kFlashTime = 0.28;

// between 0.36 and 0.44 i guess, what the fuck dude
constexpr float kBodyWidth = 0.44f;

// how many frames counts as a tap on THIS macro
static uint64_t tapFrames(double fps) {
    const double ms = double(Mod::get()->getSettingValue<int64_t>("tap-ms"));
    return uint64_t(std::max(1.0, fps * ms / 1000.0));
}

// press state.
enum : uint8_t { kPending = 0, kActive = 1, kDone = 2 };

// loadin up macros

static std::filesystem::path macroDir() {
    auto p = Mod::get()->getConfigDir() / "macros";
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    return p;
}

static std::vector<uint8_t> readFile(std::filesystem::path const& p) {
    std::ifstream f(p, std::ios::binary);
    return { std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>() };
}

// which file the user explicitly picked for this level, if any
static std::string levelKey(GJGameLevel* level) {
    if (!level) return "macro-none";
    const int id = level->m_levelID.value();
    if (id > 0) return "macro-" + std::to_string(id);
    std::string s = "macro-local-" + std::string(level->m_levelName);
    for (auto& c : s) if (!isalnum((unsigned char)c) && c != '-') c = '_';
    return s;
}

static std::string pickedMacro(GJGameLevel* level) {
    return Mod::get()->getSavedValue<std::string>(levelKey(level), "");
}

static void setPickedMacro(GJGameLevel* level, std::string const& file) {
    Mod::get()->setSavedValue<std::string>(levelKey(level), file);
}

// every parseable .gdr2 in the folder, for the ui list
struct MacroEntry {
    std::string file;
    size_t presses = 0;
    double fps = 240.0;
    double seconds = 0.0;
    uint32_t levelID = 0;
    std::string levelName;
    size_t inputs = 0;
};

static std::vector<MacroEntry> listMacros() {
    std::vector<MacroEntry> out;
    std::error_code ec;
    for (auto const& e : std::filesystem::directory_iterator(macroDir(), ec)) {
        if (!e.is_regular_file()) continue;
        if (!fmts::knownExtension(e.path().extension().string())) continue;
        auto rep = fmts::parseAny(readFile(e.path()));
        if (!rep) continue;
        MacroEntry m;
        m.file = e.path().filename().string();
        m.presses = rep->holds().size();
        m.inputs = rep->inputs.size();
        m.fps = rep->framerate;
        m.seconds = rep->seconds();
        m.levelID = rep->levelID;
        m.levelName = rep->levelName;
        out.push_back(std::move(m));
    }
    std::sort(out.begin(), out.end(),
              [](MacroEntry const& a, MacroEntry const& b) { return a.file < b.file; });
    return out;
}

// lowercase, letters and digits only, so "Bloodbath", "bloodbath" and
// "Bloodbath (2).slc" all reduce to the same thing
static std::string squash(std::string const& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s)
        if (std::isalnum(c)) out += char(std::tolower(c));
    return out;
}

// Does the level id appear in the filename as a number in its own right?
// "123456789.slc" and "123456789 - attempt 4.slc" yes, "1234567890.slc" no.
static bool filenameHasID(std::string const& stem, int id) {
    if (id <= 0) return false;
    const std::string want = std::to_string(id);
    for (size_t i = 0; (i = stem.find(want, i)) != std::string::npos; ++i) {
        const bool leftOk  = i == 0 || !std::isdigit((unsigned char)stem[i - 1]);
        const size_t after = i + want.size();
        const bool rightOk = after >= stem.size()
                          || !std::isdigit((unsigned char)stem[after]);
        if (leftOk && rightOk) return true;
    }
    return false;
}

static std::optional<gdr2::Replay> findMacro(GJGameLevel* level) {
    // an explicit pick from the pause menu beats any automatic match
    const auto forced = pickedMacro(level);
    if (!forced.empty()) {
        const auto p = macroDir() / forced;
        std::error_code fec;
        if (std::filesystem::exists(p, fec)) {
            auto rep = fmts::parseAny(readFile(p));
            if (rep) {
                log::info("macro (picked): {} | {} inputs | {} tps | {:.2f}s",
                          forced, rep->inputs.size(), rep->framerate, rep->seconds());
                return rep;
            }
            log::warn("picked macro {} will not parse, falling back", forced);
        } else {
            log::warn("picked macro {} is gone, falling back", forced);
        }
    }

    const int wantID = level->m_levelID.value();
    const std::string wantName = level->m_levelName;
    log::info("level {} \"{}\" | scanning {}", wantID, wantName, macroDir().string());

    // Match strength, best first. Silicate never writes the level id or name
    // into its files, and plenty of gdr recordings leave them blank too, so a
    // matcher that only trusts what is inside the file finds nothing for them.
    // The filename is the other thing the user controls, so it counts.
    enum { kNone = 0, kFileName, kFileID, kRepName, kRepID };

    const std::string wantSquash = squash(wantName);

    std::string bestFile;
    std::optional<gdr2::Replay> best;
    int bestRank = kNone;
    std::vector<std::string> seen;

    std::error_code ec;
    for (auto const& e : std::filesystem::directory_iterator(macroDir(), ec)) {
        if (!e.is_regular_file()) continue;
        if (!fmts::knownExtension(e.path().extension().string())) continue;
        const auto fname = e.path().filename().string();
        const auto stem  = e.path().stem().string();

        auto rep = fmts::parseAny(readFile(e.path()));
        if (!rep) { log::warn("  {} failed to parse", fname); continue; }
        seen.push_back(fname);

        int rank = kNone;
        if (wantID > 0 && int(rep->levelID) == wantID)                 rank = kRepID;
        else if (!rep->levelName.empty() && squash(rep->levelName) == wantSquash)
                                                                       rank = kRepName;
        else if (stem == std::to_string(wantID))                       rank = kFileID;
        else if (filenameHasID(stem, wantID))                          rank = kFileID;
        else if (!wantSquash.empty() && squash(stem) == wantSquash)    rank = kFileName;

        if (rank <= bestRank) continue;
        bestRank = rank;
        bestFile = fname;
        best = std::move(rep);
    }

    if (best) {
        static char const* why[] = { "", "filename matches the level name",
                                     "filename carries the level id",
                                     "macro stores this level name",
                                     "macro stores this level id" };
        log::info("macro: {} | {} inputs | {} tps | {:.2f}s | bot {} | {}",
                  bestFile, best->inputs.size(), best->framerate,
                  best->seconds(), best->botName, why[bestRank]);
        return best;
    }

    // Nothing matched. Say what is actually in the folder, because the usual
    // cause is a file that carries no level info and is not named after the
    // level either, and there is no way to tell that from silence.
    if (seen.empty()) {
        log::info("no macros in {}", macroDir().string());
    } else {
        log::info("no macro matched this level. {} readable file(s) present. "
                  "Rename one to \"{}\" plus its extension, or pick it from the "
                  "pause menu:",
                  seen.size(), wantID > 0 ? std::to_string(wantID) : wantName);
        for (auto const& s : seen) log::info("    {}", s);
    }
    return std::nullopt;
}

// settings

static ccColor4F tint(char const* colourKey, char const* transKey) {
    const auto c = Mod::get()->getSettingValue<ccColor3B>(colourKey);
    const auto t = Mod::get()->getSettingValue<int64_t>(transKey);   // 0 solid, 100 gone
    const float a = 1.f - float(std::clamp<int64_t>(t, 0, 100)) / 100.f;
    return { c.r / 255.f, c.g / 255.f, c.b / 255.f, a };
}

struct Look {
    ccColor4F indicator, p2, player, lane, laneP2;
    float width, ahead;
    double offset;
    bool  showPlayer, showLane, laneLeft, showAccuracy, splitLane, edgeMarker;
    bool  holdTracks, pressBadge, playerSquare, circleNotes, flipLane, laneMiddle;
    float badgeSize, laneWindow, laneScale, laneDepth, midHit;
    // real time, not frames, so a window means the same on every macro
    double perfectSec, okSec;
    int   maxNotes;
};

static Look readSettings() {
    // reads every setting fresh. call readSettingsCached() in the hot path.
    Look l;
    l.indicator     = tint("color", "transparency");
    l.p2            = tint("p2-color", "p2-transparency");
    l.player        = tint("player-color", "player-transparency");
    l.lane          = tint("color", "lane-transparency");
    l.laneP2        = tint("p2-color", "lane-transparency");
    l.width         = float(Mod::get()->getSettingValue<double>("line-width"));
    l.ahead         = float(Mod::get()->getSettingValue<double>("look-ahead"));
    l.offset        = double(Mod::get()->getSettingValue<int64_t>("timing-offset")) / 1000.0;
    l.showPlayer    = Mod::get()->getSettingValue<bool>("player-line");
    l.showLane      = Mod::get()->getSettingValue<bool>("lane");
    l.laneLeft      = Mod::get()->getSettingValue<std::string>("lane-side") != "Right";
    l.laneWindow    = float(Mod::get()->getSettingValue<double>("lane-window"));
    l.laneScale     = float(Mod::get()->getSettingValue<double>("lane-scale"));
    l.laneDepth     = float(Mod::get()->getSettingValue<double>("lane-depth"));
    l.midHit        = std::clamp(float(Mod::get()->getSettingValue<double>("mid-lane-hit")), 0.08f, 0.5f);
    l.circleNotes   = Mod::get()->getSettingValue<std::string>("note-shape") == "Circle";
    l.flipLane      = Mod::get()->getSettingValue<bool>("lane-flip");
    l.laneMiddle    = Mod::get()->getSettingValue<std::string>("lane-side") == "Middle";
    l.splitLane     = Mod::get()->getSettingValue<bool>("p2-lane");
    l.edgeMarker    = Mod::get()->getSettingValue<bool>("edge-marker");
    l.holdTracks    = Mod::get()->getSettingValue<bool>("hold-tracks");
    l.pressBadge    = Mod::get()->getSettingValue<bool>("press-badge");
    l.playerSquare  = Mod::get()->getSettingValue<bool>("player-square");
    l.badgeSize     = float(Mod::get()->getSettingValue<double>("badge-size"));
    l.showAccuracy  = Mod::get()->getSettingValue<bool>("accuracy");
    // Milliseconds, not frames. A frame means a different amount of time in
    // every macro, so a frame based window was four times wider on a 60 tps
    // recording than a 240 tps one and twenty times wider than a 1200 tps one.
    // That is what made low fps macros look spread out and high fps squished.
    l.perfectSec    = double(Mod::get()->getSettingValue<int64_t>("perfect-ms")) / 1000.0;
    l.okSec         = double(Mod::get()->getSettingValue<int64_t>("ok-ms")) / 1000.0;
    l.maxNotes      = int(Mod::get()->getSettingValue<int64_t>("max-notes"));
    return l;
}

// bits

// GD's CCDrawNode blends with GL_ONE as the source factor, so alpha only eats
// i did this with Claude Code lord forgive me
static CCDrawNode* makeNode() {
    auto n = CCDrawNode::create();
    n->setBlendFunc({ GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA });
    // Indicators are not part of the level and must not be dimmed, tinted or
    // faded by it. Refusing the cascade means a fade trigger, a dark section or
    // a colour trigger on the layer we hang off cannot reach in and change the
    // colours the settings asked for.
    n->setCascadeOpacityEnabled(false);
    n->setCascadeColorEnabled(false);
    return n;
}

static void poly(CCDrawNode* n, CCPoint a, CCPoint b, CCPoint c, CCPoint d, ccColor4F col) {
    if (col.a <= 0.002f) return;
    CCPoint v[4] = { a, b, c, d };
    n->drawPolygon(v, 4, col, 0.f, { 0.f, 0.f, 0.f, 0.f });
}

static void quad(CCDrawNode* n, float x0, float x1, float y0, float y1, ccColor4F c) {
    if (x1 <= x0 || y1 <= y0) return;
    poly(n, { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 }, c);
}

// Thick line between two points, for the chevrons.
static void stroke(CCDrawNode* n, CCPoint a, CCPoint b, float t, ccColor4F c) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 0.001f) return;
    const float nx = -dy / len * t, ny = dx / len * t;
    poly(n, { a.x + nx, a.y + ny }, { a.x - nx, a.y - ny },
            { b.x - nx, b.y - ny }, { b.x + nx, b.y + ny }, c);
}

// one convex polygon shaped like a pill. has to be a single shape or the
// caps end up on a different layer than the body and you get a white seam
// straight through the middle of every hold.
static void stadium(CCDrawNode* n, float cx, float yBot, float yTop, float w, ccColor4F c) {
    if (c.a <= 0.002f || w <= 0.f) return;
    const int SEG = 14;
    const float r = w * 0.5f;
    const float PI = 3.14159265f;
    if (yTop < yBot) std::swap(yTop, yBot);

    CCPoint v[2 * (SEG + 1)];
    int k = 0;
    for (int i = 0; i <= SEG; ++i) {           // bottom cap, left round to right
        const float a = PI + PI * float(i) / float(SEG);
        v[k++] = CCPoint(cx + r * std::cos(a), yBot + r * std::sin(a));
    }
    for (int i = 0; i <= SEG; ++i) {           // top cap, right round to left
        const float a = PI * float(i) / float(SEG);
        v[k++] = CCPoint(cx + r * std::cos(a), yTop + r * std::sin(a));
    }
    n->drawPolygon(v, unsigned(k), c, 0.f, { 0.f, 0.f, 0.f, 0.f });
}

// Hollow square, so the level still reads through the middle.
static void square(CCDrawNode* n, float cx, float cy, float half, float t, ccColor4F c) {
    quad(n, cx - half, cx + half, cy + half - t, cy + half, c);
    quad(n, cx - half, cx + half, cy - half, cy - half + t, c);
    quad(n, cx - half, cx - half + t, cy - half, cy + half, c);
    quad(n, cx + half - t, cx + half, cy - half, cy + half, c);
}

// green when perfect, then yellow sliding to orange across the ok window,
// red once you are outside it. how far off you were, as a colour.
static ccColor4F verdictColour(double off, double perfect, double ok) {
    if (off <= perfect) return { 0.28f, 1.00f, 0.42f, 1.f };
    if (off <= ok) {
        const float t = (ok > perfect)
                      ? float((off - perfect) / (ok - perfect)) : 0.f;
        return { 1.00f, 0.90f - 0.42f * t, 0.18f - 0.12f * t, 1.f };
    }
    return { 1.00f, 0.26f, 0.30f, 1.f };
}

static ccColor3B toC3B(ccColor4F c) {
    return { GLubyte(std::clamp(c.r, 0.f, 1.f) * 255),
             GLubyte(std::clamp(c.g, 0.f, 1.f) * 255),
             GLubyte(std::clamp(c.b, 0.f, 1.f) * 255) };
}

static ccColor4F lighten(ccColor4F c, float k) {
    return { std::clamp(c.r * k, 0.f, 1.f), std::clamp(c.g * k, 0.f, 1.f),
             std::clamp(c.b * k, 0.f, 1.f), c.a };
}

// The level is measured as a speed profile: how fast the player is actually
// moving in each slice of x. Speed at a position is a property of the level,
// not of the attempt, so EVERY attempt contributes, including one that starts
// at a start position deep into the level. That is the difference from a
// time-indexed curve, which only a run from the beginning can record and which
// therefore never learns the part of a level you are practising.
//
// Integrating it gives time-at-x, which is the one thing a start position
// needs to know. Slices nobody has reached yet fall back to the speed portal
// model, so this degrades to the old behaviour and improves from there.
constexpr float  kBucket     = 120.f;   // units of x per slice
constexpr size_t kMaxBuckets = 4096;    // covers x up to ~491k

static std::string packProf(std::vector<uint16_t> const& p) {
    std::string s;
    s.reserve(p.size() * 4);
    for (size_t i = 0; i < p.size(); ++i) {
        if (i) s += ',';
        s += std::to_string(p[i]);       // 0 means never measured
    }
    return s;
}

static std::vector<uint16_t> unpackProf(std::string const& s) {
    std::vector<uint16_t> p;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = s.find(',', i);
        if (j == std::string::npos) j = s.size();
        long v = 0;
        try { v = std::stol(s.substr(i, j - i)); }
        catch (...) { return {}; }
        if (v < 0 || v > 2000) return {};
        p.push_back(uint16_t(v));
        if (p.size() > kMaxBuckets) return {};
        i = j + 1;
    }
    return p;
}

// Physics rates a GD install can plausibly be running. Stock 2.2 is 240; the
// rest are what the common physics bypass options set. Measured rates get
// snapped to one of these so a noisy reading cannot put the clock on a rate
// that does not exist.
constexpr double kTpsTable[] = {
    60.0, 75.0, 120.0, 144.0, 180.0, 240.0, 360.0, 480.0,
    600.0, 720.0, 960.0, 1200.0, 2400.0
};

// Nearest by ratio, not by difference, so 240 vs 360 is judged as fairly as
// 1200 vs 2400. Returns 0 if nothing is close enough to trust.
static double snapTps(double v) {
    if (!(v > 1.0) || !std::isfinite(v)) return 0.0;
    double best = 0.0, bestErr = 1e9;
    for (double t : kTpsTable) {
        const double e = std::abs(std::log(v / t));
        if (e < bestErr) { bestErr = e; best = t; }
    }
    return bestErr < 0.08 ? best : 0.0;   // within ~8%, else refuse
}

static float snapSpeed(float v) {
    float best = kSpeeds[1], bestErr = 1e9f;
    for (float s : kSpeeds) {
        const float e = std::abs(s - v);
        if (e < bestErr) { bestErr = e; best = s; }
    }
    return best;
}

// this one too because im a bum

// android runs this every frame on a phone cpu, so only actually hit the
// settings store a few times a second. 0.2s is way below noticing it.
static Look readSettingsCached() {
    static Look cached = readSettings();
    static double nextAt = 0.0;
    const double t = double(clock()) / double(CLOCKS_PER_SEC);
    if (t >= nextAt || t < nextAt - 5.0) {
        nextAt = t + 0.2;
        cached = readSettings();
    }
    return cached;
}


// mod resources come out as "<modid>/<file>"
static const char* kDisc   = "bogdoner.click-indicators/taikohitcircle.png";
static const char* kRing   = "bogdoner.click-indicators/taikohitcircleoverlay.png";
static const char* kTarget = "bogdoner.click-indicators/sliderfollowcircle.png";

class $modify(IndicatorLayer, PlayLayer) {
    struct Portal { float x; float v; };
    struct Seg    { double t; float x; float v; };

    struct Fields {
        std::vector<gdr2::Hold> holds;
        std::vector<uint8_t>    state;    // pending / active / done
        std::vector<uint8_t>    judged;   // has the scorer looked at it
        std::vector<Portal>     portals;
        std::vector<Seg>        segs;
        float levelEndX = 0.f;   // furthest object, used to sanity check the walk
        bool  portalsScanned = false;
        int   scanTries = 0;
        double macroSeconds = 0.0;

        CCDrawNode*     world = nullptr;
        bool            worldDetached = false;   // sibling of m_objectLayer
        CCDrawNode*     lane  = nullptr;
        CCLabelBMFont*  verdict = nullptr;
        CCLabelBMFont*  tally = nullptr;

        double fps = 240.0;
        bool   active = false;
        bool   hasP2 = false;
        // Some macros carry player 2 inputs on levels that are never dual, and
        // those turn into a second column of notes nobody can ever hit. The
        // game's own dual flag settles it: on a single player level it stays
        // false all run, and on a level that goes dual it flips exactly when
        // the p2 notes start mattering. Latched, because dual can end again.
        bool   everDual = false;
        bool   p2Warned = false;

        double levelTime = 0.0;    // wall clock for fades, not for macro timing
        float  startX = 0.f;
        float  speed = kSpeeds[1];
        float  lastX = 0.f;
        double lastSpeedTime = 0.0;
        // measuring window for the opening speed, see learnStartSpeed
        float  spawnX = -1e9f;
        float  measX0 = -1.f;
        double measT0 = 0.0;

        // Position drives the macro clock. No scaling: the walk is already in
        // macro seconds if the speed profile is right.
        double scale = 1.0;

        // The clock.
        //
        // A macro recorded from the start of a level is a list of moments
        // measured in gameplay seconds, and m_timePlayed is gameplay seconds.
        // They are the same quantity, so for a run from the start the clock is
        // just m_timePlayed and there is nothing to model, nothing to
        // integrate, and nothing to drift. Speed portals, dash orbs, lag, frame
        // rate and physics bypass all cancel because both sides are counted in
        // the same time.
        //
        // Position is consulted exactly once, to answer "how far into the macro
        // does this attempt begin", and only when the answer is not zero. It is
        // never allowed to touch the clock again: it was position feedback that
        // dragged the old clock off, since a position timeline built from a
        // wrong start speed runs fast and takes the indicators with it.
        double clockBase = 0.0;    // macro seconds at the anchor
        double timeBase = 0.0;     // m_timePlayed at the anchor
        bool   anchored = false;
        // Fallback for the case where m_timePlayed does not advance: hand
        // accumulated dt, which is worse but never freezes.
        double clockAccum = 0.0;
        bool   gameTimer = true;
        double timerStall = 0.0;
        double lastDriftLog = 0.0;
        bool   platformer = false;

        // Measured speed profile, see kBucket. This is what the portal scan
        // was trying to compute and kept getting wrong: portals sitting off
        // the route, dash orbs, and anything else a static scan cannot see.
        std::vector<uint16_t> prof;
        bool   profDirty = false;
        std::string profKey;
        float  lastProfX = -1e9f;     // window the current speed reading spans
        double lastProfT = 0.0;
        float  startSpeed = kSpeeds[1];
        bool   startSpeedKnown = false;
        std::string speedKey;
        double nextCheckAt = 0.0;
        bool   posClock = false;   // false until the level scan succeeds
        double fallbackNow = 0.0;  // accumulated clock, used only if it does not

        size_t cursor = 0;
        size_t missCursor = 0;

        int nPerfect = 0, nOk = 0, nMiss = 0;
        double verdictAt = -10.0;
        double flashAt = -10.0;
        ccColor4F flashCol = { 1.f, 1.f, 1.f, 1.f };

        static constexpr int kQueue = 32;
        double  pressTimes[kQueue] = {};   // macro time the click landed on
        uint8_t pressIsP2[kQueue] = {};
        std::atomic<int> pressWrite{ 0 };
        int    pressRead = 0;

        std::atomic<int> holdMask{ 0 };
        bool cheatFlagged = false;
        bool noteAtLine = false;

        // real sprites for the osu circles. drawDot gives you a blob, these
        // give you an actual circle because they ARE one.
        int      offsetFrames = 0;      // per level nudge, saved
        // The game drops an invisible spike near the spawn in every level and
        // makes you die on it to set its internal anticheat flag. It is placed
        // by the game, not the level, so it sits at the same spot every time,
        // which makes it the one landmark every macro and every level share.
        float    spikeX = -1.f;         // world x of that spike, -1 unknown
        double   spikeTime = -1.0;      // when the player reaches it
        bool     spikeLogged = false;
        // Bounds the renderer last used, so the horizontal transform knows
        // what it is rotating. One frame stale, which is invisible because
        // the layout only moves when a setting changes.
        float    lbX0 = 0.f, lbX1 = 0.f, lbY0 = 0.f, lbY1 = 0.f;
        bool     lbSet = false;
        uint64_t firstPressFrame = 0;
        // macro-clock time of the first real press this attempt, for Align.
        // negative means the player has not clicked yet.
        double   firstClickAt = -1.0;
        static constexpr size_t kClickLog = 64;
        std::vector<double> clickLog;   // P1 press times this attempt
        bool     calibrating = false;   // next real click sets the offset
        std::string offsetKey;

        CCNode* laneRoot = nullptr;   // lane + sprites live here so one
                                      // scale flip turns the whole lane over
        CCNode* spriteHost = nullptr;
        std::vector<CCSprite*> discs, rings, targets;
        size_t discUsed = 0, ringUsed = 0;   // bit 0 p1 down, bit 1 p2 down
    };

    // ------------------------------------------------------------ setup

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        if (!Mod::get()->getSettingValue<bool>("enabled")) return true;

        auto f = m_fields.self();

        // name for local levels, id for levels on servers, fuck startpos copies they ruin everything angry emoji
        f->speedKey = "startspeed-" + (level->m_levelID.value() > 0
                        ? std::to_string(level->m_levelID.value())
                        : ("local-" + std::string(level->m_levelName)));
        // Deliberately not "offset-" or "offset2-". Both earlier key names hold
        // values that were fitted against a clock that was wrong, so carrying
        // them into a clock that is right would import the error. Zero is the
        // correct offset for a macro recorded from the start of a level, and
        // that is what everyone now gets.
        f->offsetKey = "offset3-" + levelKey(level);
        f->offsetFrames = Mod::get()->getSavedValue<int64_t>(f->offsetKey, 0);

        // The level says what speed it opens at, right there in its header.
        // Everything before this guessed: assume 1x, then watch the player and
        // measure. That is why a level opening at 0.5x or 4x laid its bars out
        // wrong for the whole of the first run on a machine that had not played
        // it before, which is exactly the "indicators drift towards me" that
        // only ever showed up on someone else's install. Read it instead.
        if (this->m_levelSettings) {
            // Speed enum order is Normal, Slow, Fast, Faster, Fastest, which is
            // 1x, 0.5x, 2x, 3x, 4x. kSpeeds is in ascending speed order.
            static const int kFromEnum[5] = { 1, 0, 2, 3, 4 };
            const int e = int(this->m_levelSettings->m_startSpeed);
            if (e >= 0 && e < 5) {
                f->startSpeed = kSpeeds[kFromEnum[e]];
                f->startSpeedKnown = true;
                log::info("start speed {:.0f} u/s from the level header",
                          f->startSpeed);
            }
        }

        if (!f->startSpeedKnown) {
            const double cached = Mod::get()->getSavedValue<double>(f->speedKey, 0.0);
            if (cached > 100.0) {
                f->startSpeed = float(cached);
                f->startSpeedKnown = true;
                log::info("start speed {:.0f} u/s from cache", f->startSpeed);
            }
        }

        f->profKey = "vprof-" + levelKey(level);
        f->prof = unpackProf(Mod::get()->getSavedValue<std::string>(f->profKey, ""));
        if (!f->prof.empty()) {
            size_t known = 0;
            for (auto v : f->prof) if (v) ++known;
            log::info("measured speed profile: {} of {} slices known, out to x {:.0f}",
                      known, f->prof.size(), double(f->prof.size()) * kBucket);
        }

        // built even when no macro matched. picking one from the pause menu
        // later flips active on, and it would explode on null nodes otherwise.
        // Draw above the level rather than inside it. As a child of
        // m_objectLayer the bars sit in the middle of the level's own z-order
        // and deco gets painted over them. Hanging them off the same PARENT
        // instead, at a z just above the object layer, puts them on top of
        // everything the level draws while staying below whatever the game put
        // higher up, so the UI is not covered. The transform is copied every
        // frame in postUpdate, so the coordinates the renderer works in do not
        // change at all.
        f->world = makeNode();
        if (auto host = m_objectLayer->getParent()) {
            host->addChild(f->world, m_objectLayer->getZOrder() + 1);
            f->worldDetached = true;
        } else {
            m_objectLayer->addChild(f->world, kWorldZ);
        }

        f->laneRoot = CCNode::create();
        this->addChild(f->laneRoot, kLaneZ);

        f->lane = makeNode();
        f->laneRoot->addChild(f->lane, 0);

        f->spriteHost = CCNode::create();
        f->laneRoot->addChild(f->spriteHost, 1);

        f->verdict = CCLabelBMFont::create("", "chatFont.fnt");
        f->verdict->setScale(0.42f);
        f->verdict->setAnchorPoint({ 0.5f, 0.5f });
        this->addChild(f->verdict, kLaneZ + 1);

        f->tally = CCLabelBMFont::create("", "chatFont.fnt");
        f->tally->setScale(0.34f);
        f->tally->setAnchorPoint({ 0.5f, 0.5f });
        f->tally->setOpacity(150);
        this->addChild(f->tally, kLaneZ + 1);

        auto rep = findMacro(level);
        if (!rep) {
            log::info("no macro for this level, nodes ready for a manual pick");
            return true;
        }
        intake(*rep);
        f->active = true;

        // Try the object scan now rather than waiting for the first frame.
        // Everything the layout depends on, the speed timeline and the check
        // on the macro's declared rate, is then already settled before a
        // single indicator is drawn.
        tryScanPortals();
        return true;
    }

    // Objects do not exist during PlayLayer::init, GD builds them afterwards,
    // so this runs from the update loop and retries until the level is there.
    void tryScanPortals() {
        auto f = m_fields.self();
        if (f->portalsScanned) return;
        if (++f->scanTries > 300) {
            f->portalsScanned = true;
            log::warn("gave up looking for level objects, assuming a flat 1x level");
            return;
        }
        if (!m_objects || m_objects->count() == 0) return;

        scanPortals();
        f->portalsScanned = true;

        const double walk = timeToReach(f->levelEndX);
        // In a platformer x is not a clock. You can stand still, walk back,
        // and be sent anywhere by a teleport, so every position derived number
        // is meaningless there and the step counter is the only clock.
        if (f->platformer) {
            f->posClock = false;
            log::info("platformer level, running on physics steps only");
        } else if (walk > 1.0) {
            f->posClock = true;
        } else {
            log::warn("walk came out at {:.2f}s, keeping the accumulated clock", walk);
        }
        // if macro shorter its fine because end screens, if longer "fuck" i guesss
        const bool bad = walk > 1.0 && f->macroSeconds > walk * 1.02;
        log::info("scanned {} objects on try {}: {} speed portals, level ends at x {:.0f}",
                  m_objects->count(), f->scanTries, f->portals.size(), f->levelEndX);
        log::info("portal walk check: whole level {:.2f}s vs macro {:.2f}s  ratio {:.3f}  "
                  "start speed {:.0f}{}",
                  walk, f->macroSeconds,
                  f->macroSeconds > 0.01 ? walk / f->macroSeconds : 0.0,
                  levelStartSpeed(),
                  bad ? "  <-- macro longer than the level, wrong macro?" : "  ok");
        log::info("clock source: {}, start speed {:.0f} u/s ({})",
                  f->posClock ? "player position" : "accumulated time",
                  levelStartSpeed(),
                  f->startSpeedKnown ? "known" : "ASSUMED, play once from the start");
        if (!f->portals.empty())
            log::info("first portals: {:.0f}@{:.0f}  {:.0f}@{:.0f}",
                      f->portals[0].v, f->portals[0].x,
                      f->portals.size() > 1 ? f->portals[1].v : 0.f,
                      f->portals.size() > 1 ? f->portals[1].x : 0.f);

        checkDeclaredFps(walk);
    }

    // Some bots write their own recording setting into the framerate field
    // instead of the rate the frame numbers are actually counted in. MEGA does
    // it: a macro saved with the game set to 1200 says "1200 tps" but its frame
    // numbers are plain physics steps at 240, so dividing by 1200 collapses the
    // whole macro into a fifth of the level and every indicator lands early and
    // bunched up. Death Corridor was exactly this.
    //
    // The level itself settles it. Reading the macro at the declared rate makes
    // it 25s long on a level that takes 127s to walk, and 127/25 is 1200/240 to
    // within half a percent. That agreement between two independent numbers is
    // what makes it safe to act on.
    //
    // Deliberately narrow. It only fires when the declared rate is ABOVE the
    // 240 GD actually steps at, which is the only case a bot can overstate. A
    // macro that says 240 is never touched, so a genuine part-of-the-level
    // recording, which is also shorter than its level, cannot be caught by
    // this and rescaled into nonsense.
    void checkDeclaredFps(double walk) {
        auto f = m_fields.self();
        if (f->platformer || walk < 1.0 || f->holds.empty()) return;
        if (f->fps <= 260.0) return;                  // not overstated, leave it
        if (f->macroSeconds > walk * 0.75) return;    // already spans the level

        double best = 0.0, bestErr = 1e9;
        for (double cand : kTpsTable) {
            if (cand >= f->fps - 1.0) continue;       // only rates below declared
            const double corrected = f->macroSeconds * (f->fps / cand);
            const double err = std::abs(corrected - walk) / walk;
            if (err < bestErr) { bestErr = err; best = cand; }
        }
        if (best <= 0.0 || bestErr > 0.12) {
            log::warn("macro says {:.0f} tps and reads as {:.2f}s, but this level "
                      "walks in {:.2f}s. Cannot tell what rate it was really "
                      "counted at, leaving it alone.", f->fps, f->macroSeconds, walk);
            return;
        }

        const double was = f->fps;
        f->fps = best;
        f->macroSeconds = f->macroSeconds * (was / best);
        log::warn("macro claims {:.0f} tps but its frames are counted at {:.0f}: "
                  "at {:.0f} it would be {:.2f}s on a {:.2f}s level, at {:.0f} it "
                  "is {:.2f}s ({:.1f}% off). Using {:.0f}.",
                  was, best, was, f->macroSeconds * best / was, walk,
                  best, f->macroSeconds, bestErr * 100.0, best);
    }

    void scanPortals() {
        auto f = m_fields.self();
        if (!m_objects) return;
        for (unsigned i = 0; i < m_objects->count(); ++i) {
            auto obj = static_cast<GameObject*>(m_objects->objectAtIndex(i));
            if (!obj) continue;
            for (int k = 0; k < 5; ++k)
                if (obj->m_objectID == kSpeedIDs[k]) {
                    f->portals.push_back({ obj->getPositionX(), kSpeeds[k] });
                    break;
                }
            if (obj->getPositionX() > f->levelEndX) f->levelEndX = obj->getPositionX();
        }
        std::sort(f->portals.begin(), f->portals.end(),
                  [](Portal const& a, Portal const& b) { return a.x < b.x; });

        // checks speed and x = 0 and y = 0 on start otherwise gay
        f->segs.clear();
        float x = 0.f, v = levelStartSpeed();
        double t = 0.0;
        f->segs.push_back({ 0.0, 0.f, v });
        for (auto const& p : f->portals) {
            if (p.x <= x) { f->segs.back().v = p.v; v = p.v; continue; }
            t += double(p.x - x) / double(v);
            x = p.x; v = p.v;
            f->segs.push_back({ t, x, v });
        }
    }

    // macro position time + position time checks macro which is a bi-curious relationship!
    double tAtX(float px) {
        auto const& S = m_fields->segs;
        if (S.empty()) return 0.0;
        size_t lo = 0, hi = S.size() - 1;
        while (lo < hi) {
            const size_t mid = (lo + hi + 1) / 2;
            if (S[mid].x <= px) lo = mid; else hi = mid - 1;
        }
        return S[lo].t + double(px - S[lo].x) / double(S[lo].v);
    }

    // tells megahack / eclipse / whatever that this is on, via cheat api.
    // silently does nothing if the api mod isnt installed
    void setCheating(bool on) {
        auto f = m_fields.self();
        if (on == f->cheatFlagged) return;
        if (!Mod::get()->getSettingValue<bool>("cheat-indicator")) return;
        f->cheatFlagged = on;
#ifdef HAS_CHEAT_API
        // these return a Result, nothing useful to do with it here
        if (on) (void)cheatAPIEvents::setCheatingAll();
        else    (void)cheatAPIEvents::endCheatingAll();
        log::debug("cheat api told: {}", on);
#endif
    }

    // drop the cheat flag on the way out so it does not stay lit in the menus
    void onQuit() {
        setCheating(false);
        saveProf();
        PlayLayer::onQuit();
    }

    // called by the pause menu after LOAD, so a pick takes effect right away
    void reloadMacro() {
        auto f = m_fields.self();
        auto rep = findMacro(m_level);
        f->holds.clear();
        f->hasP2 = false;
        f->active = false;
        if (rep) {
            intake(*rep);
            // intake resets fps to whatever the file declares, so a macro that
            // overstates its rate has to be re-checked here or picking it from
            // the pause menu would undo the correction the level load made.
            if (f->portalsScanned) checkDeclaredFps(timeToReach(f->levelEndX));
            f->active = !f->holds.empty();
        } else {
            f->state.clear();
            f->judged.clear();
            f->cursor = f->missCursor = 0;
        }
        f->nPerfect = f->nOk = f->nMiss = 0;
        if (f->world) f->world->clear();
        if (f->lane)  f->lane->clear();
        if (f->spriteHost) hideSprites();
        log::info("macro reloaded: {} presses", f->holds.size());
    }

    // Line the macro up with where you actually clicked. Takes the first real
    // press of the attempt and the macro's first press, and stores the gap as
    // this level's offset.
    //
    // This used to have a second mode that pinned macro frame 0 to the level's
    // anticheat spike. That existed to paper over a clock that did not know
    // where in the level it was; it was correcting the clock's error, not the
    // macro's. The clock counts physics steps now and macro frame 0 is level
    // start by construction, so pinning to the spike would introduce exactly
    // the offset it used to cancel. Gone, and the saved offsets it wrote are
    // abandoned along with it, see offsetKey.
    // Returns the new offset in frames, or INT_MIN if it could not align.
    // Align by matching the SHAPE of what you played against the macro.
    //
    // Matching a single click to the nearest press only works when the macro
    // is already close, because "nearest" stops meaning anything once the
    // error is bigger than the gap between presses. That is exactly the case
    // on a start position deep into a level nobody has played through, where
    // the anchor is estimated and can be a second or more out. A rhythm of
    // several clicks is unambiguous where one click is not: slide it along the
    // macro and only the true offset makes all of them land at once.
    //
    // Returns the new offset in frames, or INT_MIN if it could not align.
    int alignByPattern() {
        auto f = m_fields.self();
        if (f->holds.empty() || f->clickLog.size() < 3) return INT_MIN;

        std::vector<double> mt;
        mt.reserve(f->holds.size());
        for (auto const& h : f->holds)
            if (!h.player2) mt.push_back(double(h.start) / f->fps);
        if (mt.size() < 2) return INT_MIN;
        std::sort(mt.begin(), mt.end());

        // Cost of a candidate shift: how far each click sits from the nearest
        // macro press, capped so one stray click cannot outvote the rest.
        const double cap = 0.25;
        auto costOf = [&](double sh) {
            double score = 0.0;
            for (double c : f->clickLog) {
                const double t = c + sh;
                auto it = std::lower_bound(mt.begin(), mt.end(), t);
                double d = cap;
                if (it != mt.end())   d = std::min(d, *it - t);
                if (it != mt.begin()) d = std::min(d, t - *(it - 1));
                score += d;
            }
            return score;
        };

        double bestShift = 0.0, bestScore = 1e18;
        for (int i = -6000; i <= 6000; ++i) {          // +/- 6s, 1ms steps
            const double sh = double(i) * 0.001;
            const double score = costOf(sh);
            if (score < bestScore) { bestScore = score; bestShift = sh; }
        }

        const double perClick = bestScore / double(f->clickLog.size());
        if (perClick > 0.06) {
            log::warn("pattern align: best fit is {:.0f}ms off per click, too "
                      "loose to trust. Play the section again more cleanly.",
                      perClick * 1000.0);
            return INT_MIN;
        }

        f->offsetFrames += int(std::llround(-bestShift * f->fps));
        Mod::get()->setSavedValue<int64_t>(f->offsetKey, int64_t(f->offsetFrames));
        reloadMacro();
        log::info("pattern aligned on {} clicks: shift {:+.3f}s, {:.0f}ms per "
                  "click, offset now {}f",
                  f->clickLog.size(), -bestShift, perClick * 1000.0, f->offsetFrames);
        return f->offsetFrames;
    }

    bool canPatternAlign() { return m_fields->clickLog.size() >= 3
                                 && !m_fields->holds.empty(); }

    int alignToFirstClick() {
        auto f = m_fields.self();
        if (f->holds.empty() || f->firstClickAt < 0.0) return INT_MIN;

        const double macroAt  = double(f->firstPressFrame) / f->fps;
        const double deltaSec = f->firstClickAt - macroAt;
        if (std::abs(deltaSec) > 30.0) return INT_MIN;   // nonsense, refuse

        f->offsetFrames += int(std::llround(deltaSec * f->fps));
        Mod::get()->setSavedValue<int64_t>(f->offsetKey, int64_t(f->offsetFrames));
        reloadMacro();
        log::info("aligned: click {:.3f}s vs macro {:.3f}s, offset now {}f",
                  f->firstClickAt, macroAt, f->offsetFrames);
        return f->offsetFrames;
    }

    void setOffset(int frames) {
        auto f = m_fields.self();
        f->offsetFrames = frames;
        Mod::get()->setSavedValue<int64_t>(f->offsetKey, int64_t(frames));
        reloadMacro();
    }

    int  currentOffset() { return m_fields.self()->offsetFrames; }
    bool canAlign() { auto f = m_fields.self();
                      return !f->holds.empty() && f->firstClickAt >= 0.0; }

    void resetLevel() {
        PlayLayer::resetLevel();
        auto f = m_fields.self();
        if (!f->active) return;

        saveProf();         // keep whatever the attempt that just ended learned
        f->lastProfX = -1e9f;
        f->lastProfT = 0.0;
        f->levelTime = 0.0;
        f->firstClickAt = -1.0;
        f->clickLog.clear();
        f->spikeTime = -1.0;
        f->spikeX = -1.f;
        f->everDual = false;
        f->p2Warned = false;
        f->startX = m_player1 ? m_player1->getPositionX() : 0.f;
        f->lastX = f->startX;
        f->speed = kSpeeds[1];
        f->lastSpeedTime = 0.0;
        f->spawnX = -1e9f;
        f->measX0 = -1.f;
        f->measT0 = 0.0;
        f->cursor = 0;
        f->missCursor = 0;
        f->fallbackNow = 0.0;
        f->clockBase = 0.0;
        f->clockAccum = 0.0;
        f->timeBase = 0.0;
        f->gameTimer = true;
        f->timerStall = 0.0;
        f->lastDriftLog = 0.0;
        // Drop the anchor on every reset. A practice respawn puts the player
        // somewhere else in the level, so where in the macro the next attempt
        // begins has to be worked out again.
        f->anchored = false;
        f->nPerfect = f->nOk = f->nMiss = 0;
        f->verdictAt = -10.0;
        f->flashAt = -10.0;
        f->state.assign(f->holds.size(), kPending);
        f->judged.assign(f->holds.size(), 0);
        f->pressRead = 0;
        f->pressWrite.store(0, std::memory_order_release);
        f->holdMask.store(0, std::memory_order_release);
        if (f->verdict) f->verdict->setString("");
        if (f->tally) f->tally->setString("");
    }

    // and this code has even gotten me back to drinking
    //
    // Where we are in the macro, in macro seconds.
    double absTime() {
        auto f = m_fields.self();
        if (!f->anchored) return f->fallbackNow;
        if (f->gameTimer) return f->clockBase + (this->m_timePlayed - f->timeBase);
        return f->clockBase + f->clockAccum;
    }

    // check if macro is off by a bijilion units
    double posTime() {
        return tAtX(m_player1 ? m_player1->getPositionX() : 0.f);
    }

    // Time to reach px, integrating the measured speed profile and falling
    // back to the portal model for slices nobody has crossed yet. Sets
    // *coverage to the fraction that came from measurement, so the log can say
    // how much of the answer is real.
    double profTime(float px, double* coverage = nullptr) {
        auto f = m_fields.self();
        if (coverage) *coverage = 0.0;
        if (px <= 0.f) return 0.0;

        double t = 0.0;
        size_t measured = 0, total = 0;
        for (float x = 0.f; x < px; x += kBucket) {
            const float w = std::min(kBucket, px - x);
            const size_t b = size_t(x / kBucket);
            ++total;

            float v = 0.f;
            if (b < f->prof.size() && f->prof[b] > 0) { v = float(f->prof[b]); ++measured; }
            else v = speedAt(x + w * 0.5f);
            if (v < 1.f) v = kSpeeds[1];

            t += double(w) / double(v);
        }
        if (coverage && total) *coverage = double(measured) / double(total);
        return t;
    }

    // Learn how fast the player really moves through each slice of the level.
    // Runs on every attempt, from anywhere, because speed at a position is a
    // property of the level rather than of the run.
    void measureProfile(float px, bool alive) {
        auto f = m_fields.self();
        if (!alive || f->platformer) return;

        if (f->lastProfX < -1e8f) {
            f->lastProfX = px;
            f->lastProfT = this->m_timePlayed;
            return;
        }

        const double span = this->m_timePlayed - f->lastProfT;
        if (span < 0.05) return;          // long enough to be precise

        const float from = f->lastProfX;
        const float dx = px - from;
        f->lastProfX = px;
        f->lastProfT = this->m_timePlayed;

        if (dx <= 0.f) return;            // stopped, dead, or moving backwards
        const float v = float(dx / span);
        if (v < 1.f || v > 2000.f) return;

        size_t b0 = size_t(std::max(0.f, from) / kBucket);
        size_t b1 = size_t(std::max(0.f, px) / kBucket);
        if (b0 >= kMaxBuckets) return;
        if (b1 >= kMaxBuckets) b1 = kMaxBuckets - 1;
        if (f->prof.size() <= b1) f->prof.resize(b1 + 1, 0);

        const uint16_t nv = uint16_t(std::lround(v));
        for (size_t b = b0; b <= b1; ++b) {
            // Only rewrite on a real disagreement, so a steady reading does not
            // mark the profile dirty on every single frame.
            if (f->prof[b] == 0 || std::abs(int(f->prof[b]) - int(nv)) > 3) {
                f->prof[b] = nv;
                f->profDirty = true;
            }
        }
    }

    void saveProf() {
        auto f = m_fields.self();
        if (!f->profDirty || f->profKey.empty() || f->prof.empty()) return;
        f->profDirty = false;
        Mod::get()->setSavedValue<std::string>(f->profKey, packProf(f->prof));
    }
    // The level's opening speed lives in the level header, not in any object, (CLAUDE CODE FABLE 5 SORRY KINGS)
    // so a level that starts at 4x with its first portal thousands of units in
    // looks like a 1x level to an object scan. Measure it instead: run from the
    // start once and the real value is observable, then cache it forever.
    float levelStartSpeed() {
        auto f = m_fields.self();
        for (auto const& p : f->portals) {
            if (p.x <= 20.f) return p.v;   // an explicit portal still wins
            break;
        }
        return f->startSpeed;
    }

    void learnStartSpeed(float px) {
        auto f = m_fields.self();
        if (f->platformer) return;   // no such thing as a scroll speed here
        if (f->startSpeedKnown || !f->portalsScanned) return;

        const float firstPortalX = f->portals.empty() ? 1e9f : f->portals[0].x;
        if (px >= firstPortalX) return;    // past it, no longer observable

        // Measure between two fixed points in time rather than smoothing a per
        // frame estimate. The old version blended once per frame and snapped at
        // a fixed 0.20s, so at 240 fps it had converged and at 30 fps it had
        // barely started, and the snap could land on the wrong speed. Which
        // speed it picked then decided every bar position for the whole level.
        // Displacement over elapsed time does not care how many frames it took.
        const double t = f->gameTimer ? this->m_timePlayed : f->levelTime;

        if (f->spawnX < -1e8f) f->spawnX = px;

        if (f->measX0 < 0.f) {
            // Open the window on movement, not on the clock. Waiting a fixed
            // fraction of a second means any pause at spawn lands inside the
            // window and drags the average down, which snapped fast levels to
            // a slower speed at every frame rate. Once the player has actually
            // covered ground we know the reading is all motion.
            if (px - f->spawnX < 10.f) return;
            f->measX0 = px;
            f->measT0 = t;
            return;
        }

        const double span = t - f->measT0;
        if (span < 0.25) return;           // long enough to be precise

        const float v = float((px - f->measX0) / span);
        if (v < 1.f || v > 2000.f) return; // nonsense, wait for a better window

        const float s = snapSpeed(v);
        f->startSpeed = s;
        f->startSpeedKnown = true;
        Mod::get()->setSavedValue<double>(f->speedKey, double(s));
        // Cached for next time, NOT rebuilt now. Rebuilding the timeline while
        // you are playing respaces every bar on screen, and a bar that moves
        // while you are reading it is worse than a bar that is slightly wrong.
        // The clock does not depend on this, and the next load picks it up.
        log::info("learned start speed {:.0f} u/s from {:.0f} units over {:.2f}s, "
                  "cached as \"{}\". Takes effect next time this level loads.",
                  s, px - f->measX0, span, f->speedKey);
    }

    double timeToReach(float targetX) { return tAtX(targetX); }

    // Grab the anticheat spike's position. It is a plain GameObject hanging off
    // GJBaseGameLayer, so no object scan is needed. Cheap enough to retry until
    // the game has created it.
    void findSpike() {
        auto f = m_fields.self();
        if (f->spikeX >= 0.f) return;
        auto spike = this->m_anticheatSpike;
        if (!spike) return;

        f->spikeX = spike->getPositionX();
        f->spikeTime = tAtX(f->spikeX);
        if (!f->spikeLogged) {
            f->spikeLogged = true;
            log::info("anticheat spike at x {:.1f}, reached at {:.3f}s",
                      f->spikeX, f->spikeTime);
        }
    }

    float speedAt(float px) {
        auto const& S = m_fields->segs;
        if (S.empty()) return m_fields->startSpeed;
        float v = S[0].v;
        for (auto const& s : S) { if (s.x <= px) v = s.v; else break; }
        return v;
    }

    // Position at a macro time, the exact inverse of tAtX.
    // Where a macro time lands on screen, measured FROM THE PLAYER rather than
    // from the start of the level.
    //
    // xAt() integrates a speed timeline rebuilt from the level's portals, and
    // any error in it, a mis-snapped start speed or a portal the scan missed,
    // keeps adding up the further you get. That is why the bars looked fine
    // early and were visibly wrong by 18% of Bloodbath, and why the rhythm lane
    // stayed correct throughout: the lane places notes by time, only the world
    // bars go through this.
    //
    // The player is at px at time now, by definition. Taking the difference
    // cancels whatever the timeline has got wrong so far, so the only error
    // left is whatever happens inside the lookahead window, about two seconds,
    // instead of everything since the start.
    float xRel(double t, double now, float px) {
        return px + (xAt(t) - xAt(now));
    }

    float xAt(double t) {
        auto f = m_fields.self();
        auto const& S = f->segs;
        if (S.empty()) return f->startX + float(t * f->speed);
        const double raw = t / (f->scale > 0.01 ? f->scale : 1.0);
        size_t lo = 0, hi = S.size() - 1;
        while (lo < hi) {
            const size_t mid = (lo + hi + 1) / 2;
            if (S[mid].t <= raw) lo = mid; else hi = mid - 1;
        }
        return S[lo].x + float((raw - S[lo].t) * S[lo].v);
    }

    // FRAMES

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        auto f = m_fields.self();
        if (!m_player1) return;
        if (!f->world || !f->lane || !f->spriteHost) return;   // nodes gone, nothing to draw

        // master switch, live. off means nothing draws and nothing scores.
        // no macro loaded takes the same path, otherwise the last macro's
        // drawings sat on screen and the cheat flag stayed on after unloading.
        if (!Mod::get()->getSettingValue<bool>("enabled") || !f->active) {
            f->world->clear();
            f->lane->clear();
            hideSprites();
            if (f->verdict) f->verdict->setVisible(false);
            if (f->tally)   f->tally->setVisible(false);
            setCheating(false);
            return;
        }
        setCheating(true);

        if (this->m_gameState.m_isDualMode) {
            auto ff = m_fields.self();
            if (!ff->everDual) {
                ff->everDual = true;
                log::info("level went dual, player 2 notes are now live");
            }
        }

        const bool alive = !m_player1->m_isDead;
        if (alive) {
            f->levelTime += std::min(dt, kMaxStep);
            // What the clock reads in the few frames before the anchor is
            // taken. Level time is the right answer there, since an attempt is
            // at the start of the level until something says otherwise.
            f->fallbackNow = f->gameTimer ? this->m_timePlayed
                                          : f->fallbackNow + std::min(dt, kMaxStep);
        }

        const float px = m_player1->getPositionX();

        if (alive) {
            const double span = f->levelTime - f->lastSpeedTime;
            if (span >= 0.05) {
                const float v = float((px - f->lastX) / span);
                if (v > 1.f && v < 2000.f) f->speed = f->speed * 0.5f + v * 0.5f;
                f->lastX = px;
                f->lastSpeedTime = f->levelTime;
            }
        }

        tryScanPortals();
        learnStartSpeed(px);

        // Anchor once per attempt, then let the game's own play timer run.
        // See absTime().
        //
        // Played from the beginning the anchor is zero and nothing is modelled
        // at all. Resuming from a start position or a checkpoint is the only
        // case that needs an estimate of where in the macro we are, so the
        // object scan is waited on first to give that estimate something to
        // work with.
        // Ask the game, do not infer it from x. A start position sitting near
        // the level start, or any level whose first object is not at x 0,
        // reads as a fresh run to a position test and would put the whole
        // macro at the wrong place.
        const bool fromStart = !this->m_startPosObject && !this->m_currentCheckpoint;

        // A run from the start needs no estimate, so it does not wait for the
        // scan. Only the resume path does.
        if (alive && !f->anchored && (fromStart || f->portalsScanned || f->platformer)) {
            if (fromStart || f->platformer) {
                f->clockBase = 0.0;
            } else {
                double cov = 0.0;
                f->clockBase = profTime(px, &cov);
                log::info("resuming at x {:.0f} = {:.2f}s into the macro "
                          "({:.0f}% of that measured, the rest estimated from "
                          "speed portals)", px, f->clockBase, cov * 100.0);
                if (cov < 0.98)
                    log::info("  the estimated part is the error you will feel. "
                              "Playing through the unmeasured stretch once "
                              "replaces it, or use Align in the pause menu.");
            }

            // From the start, macro time IS the play timer, so the base is a
            // true zero and the couple of frames it took to get here are not
            // lost. Resuming, measure elapsed from the anchor instead, which
            // stays correct whether or not GD rewinds the timer on a respawn.
            f->timeBase = fromStart ? 0.0 : this->m_timePlayed;
            f->clockAccum = 0.0;
            f->anchored = true;
        }

        // m_timePlayed is the number behind the in game time display and it is
        // what the clock rides on. If it ever stops advancing while the player
        // is alive and moving, fall back to accumulated dt rather than freeze
        // every note on screen.
        if (f->anchored && alive) {
            if (f->gameTimer) {
                const double elapsed = this->m_timePlayed - f->timeBase;
                if (elapsed <= f->clockAccum + 1e-9) {
                    f->timerStall += dt;
                    if (f->timerStall > 0.5) {
                        f->gameTimer = false;
                        log::warn("game timer is not advancing, falling back to "
                                  "accumulated time");
                    }
                } else {
                    f->timerStall = 0.0;
                    f->clockAccum = elapsed;
                }
            } else {
                f->clockAccum += std::min(dt, kMaxStep);
            }
        }

        measureProfile(px, alive);

        // The position timeline is now only a diagnostic. When it disagrees
        // with the clock the clock is the one to believe: a timeline built on
        // the wrong start speed, or on portals the player never passes through,
        // runs at the wrong rate and used to take the indicators with it.
        if (f->anchored && alive && f->posClock && !f->platformer
            && f->levelTime > f->lastDriftLog + 5.0) {
            f->lastDriftLog = f->levelTime;
            const double drift = posTime() - absTime();
            if (std::abs(drift) > 0.25)
                log::debug("position timeline is {:+.3f}s off the clock at x {:.0f}, "
                           "clock wins", drift, px);
        }
        // Keep the detached world node standing exactly where the object layer
        // stands. Content size is left at zero so the anchor point cannot
        // introduce an offset, which is what lets a plain copy of position,
        // scale, rotation and skew reproduce the transform exactly.
        if (f->worldDetached && m_objectLayer) {
            f->world->setPosition(m_objectLayer->getPosition());
            f->world->setScaleX(m_objectLayer->getScaleX());
            f->world->setScaleY(m_objectLayer->getScaleY());
            f->world->setRotation(m_objectLayer->getRotation());
            f->world->setSkewX(m_objectLayer->getSkewX());
            f->world->setSkewY(m_objectLayer->getSkewY());
        }

        const Look L = readSettingsCached();

        // Draw nothing until the anchor is in AND the level has been scanned.
        // Before either, the clock reads roughly zero and the position
        // timeline is empty, so bars would be laid out against a flat 1x
        // guess and then jump when the real one arrives. A blank frame is
        // better than a frame that lies, especially on a level whose first
        // click comes early enough to be confused by the jump.
        if (!f->anchored || !f->portalsScanned) {
            f->world->clear();
            f->lane->clear();
            hideSprites();
            return;
        }

        const double macroNow = absTime();
        const double now = macroNow - L.offset;

        // no if statements aura this checksoff Bravo 3 checkmark which kinda works?
        const int w = f->pressWrite.load(std::memory_order_acquire);
        while (f->pressRead < w) {
            const int slot = f->pressRead % Fields::kQueue;
            judgePress(f->pressTimes[slot], f->pressIsP2[slot] != 0, L);
            ++f->pressRead;
        }

        // Report only. An earlier version rewrote the cached start speed from
        // this comparison and it was a mistake twice over: f->speed is a
        // smoothed estimate that begins each attempt at its initialised value,
        // so the first comparison is always against a reading that has not
        // converged, and "correcting" from it flipped the cached speed back
        // and forth every couple of seconds. Each flip rebuilt the timeline,
        // which is what made the indicators visibly shift while playing. The
        // measured profile below is where a wrong model actually gets fixed.
        if (f->posClock && alive && !f->platformer
            && f->levelTime > 3.0 && f->levelTime > f->nextCheckAt) {
            f->nextCheckAt = f->levelTime + 5.0;
            const float want = speedAt(px), got = f->speed;
            if (std::abs(want - got) > 25.f)
                log::debug("speed at x {:.0f}: timeline says {:.0f}, player is "
                           "doing {:.0f}", px, want, got);
        }

        // One transform on the container covers every layout, so both note
        // shapes get all of them for free and neither renderer knows about it.
        if (f->laneRoot) {
            const auto winSz = CCDirector::get()->getWinSize();
            auto root = f->laneRoot;

            if (L.laneMiddle && f->lbSet) {
                // Middle sits across the top of the screen. The renderer has
                // already sized it off screen width, so no shrinking here, just
                // stand it on its side and park it.
                //
                // cocos rotation is clockwise, so at scale 1:
                //   +90 maps (x,y) -> ( y, -x)
                //   -90 maps (x,y) -> (-y,  x)
                // Solve for the hit line (lbY0) and the band edge and the
                // positions drop straight out. Rotated, not mirrored, so
                // nothing ends up back to front.
                const float band = f->lbX1 - f->lbX0;          // thickness now
                // Where the hit line sits along the screen. Was pinned at
                // 0.175 of the width, hard against the edge; now it follows
                // the Middle lane hit position setting so the aim point can be
                // brought towards the centre. drawLaneFlat sizes the ladder
                // off the same number, so the run-in always fits what is left.
                const float pad  = winSz.width * L.midHit;
                const float cy   = winSz.height - winSz.height * 0.025f - band;

                root->setScaleX(1.f);
                root->setScaleY(1.f);

                if (L.flipLane) {
                    // receptor on the right, notes travel in from the left
                    root->setRotation(-90.f);
                    root->setPositionX(winSz.width - pad + f->lbY0);
                    root->setPositionY(cy - f->lbX0);
                } else {
                    // receptor on the left, notes travel in from the right
                    root->setRotation(90.f);
                    root->setPositionX(pad - f->lbY0);
                    root->setPositionY(cy + f->lbX1);
                }
            } else {
                root->setRotation(0.f);
                root->setScaleX(1.f);
                root->setScaleY(L.flipLane ? -1.f : 1.f);
                root->setPositionX(0.f);
                root->setPositionY(L.flipLane ? winSz.height : 0.f);
            }
        }

        findSpike();

        retireActive(now);
        scoreMissed(now, L);
        // The world bars place notes by converting macro time back into an x,
        // which needs a level where x and time mean the same thing. In a
        // platformer they would be drawn at invented positions, so the lane
        // is the only honest view there.
        if (f->platformer) f->world->clear();
        else               drawWorld(px, now, L);
        drawLane(now, L);
        updateLabels(L);
    }

    // hold and leave
    // p2 notes only count once the level has actually been dual
    // Live dual state, not a latch. It used to stay on for the rest of the
    // level once a dual portal had been passed, so a level with one dual
    // section carried a second column of notes all the way to the end.
    bool p2Live() {
        auto f = m_fields.self();
        return f->hasP2 && this->m_gameState.m_isDualMode;
    }

    bool skipP2(gdr2::Hold const& h) {
        if (!h.player2) return false;
        auto f = m_fields.self();
        if (this->m_gameState.m_isDualMode) return false;
        if (!f->p2Warned) {
            f->p2Warned = true;
            log::info("player 2 notes are hidden outside dual sections");
        }
        return true;
    }

    void retireActive(double now) {
        auto f = m_fields.self();
        const int held = f->holdMask.load(std::memory_order_acquire);
        for (size_t i = f->cursor; i < f->holds.size(); ++i) {
            auto const& h = f->holds[i];
            // holds are in start order, so once we are past now nothing
            // further can be active yet. without this the loop ran to the end
            // of the macro every frame.
            if (double(h.start) / f->fps > now + 1.0) break;
            if (f->state[i] != kActive) continue;
            const bool stillDown = (held & (h.player2 ? 2 : 1)) != 0;
            if (!stillDown || now > double(h.end) / f->fps)
                f->state[i] = kDone;
        }
    }

    // grabs the next free sprite from a pool, making one if we ran out
    CCSprite* take(std::vector<CCSprite*>& pool, size_t& used, const char* file) {
        auto f = m_fields.self();
        if (!f->spriteHost) return nullptr;
        if (used >= pool.size()) {
            auto s = CCSprite::create(file);
            if (!s) return nullptr;
            s->setAnchorPoint({ 0.5f, 0.5f });
            f->spriteHost->addChild(s);
            pool.push_back(s);
        }
        auto s = pool[used++];
        s->setVisible(true);
        return s;
    }

    void placeCircle(CCSprite* s, float x, float y, float diameter, ccColor4F c) {
        if (!s) return;
        const float w = s->getContentSize().width;
        s->setScale(w > 0.1f ? diameter / w : 1.f);
        s->setPosition({ x, y });
        s->setColor({ GLubyte(c.r * 255), GLubyte(c.g * 255), GLubyte(c.b * 255) });
        s->setOpacity(GLubyte(std::clamp(c.a, 0.f, 1.f) * 255));
    }

    void hideSprites() {
        auto f = m_fields.self();
        for (auto s : f->discs)   s->setVisible(false);
        for (auto s : f->rings)   s->setVisible(false);
        for (auto s : f->targets) s->setVisible(false);
        f->discUsed = f->ringUsed = 0;
    }

    // One place that turns a parsed replay into the holds we draw. Both the
    // initial load and the pause-menu reload go through here so they cannot
    // drift apart.
    //
    // Two things happen that did not before:
    //   - buttons 2 and 3 are left/right, platformer only. In a classic level
    //     they are noise, and because holds are keyed per button per player
    //     they were showing up as extra notes. That is the phantom p2 presses.
    //   - the per level offset gets folded in, so a macro whose frame 0 does
    //     not line up with the level start can be nudged into place.
    void intake(gdr2::Replay const& rep) {
        auto f = m_fields.self();

        f->fps = rep.framerate > 0 ? rep.framerate : 240.0;
        f->platformer = rep.platformer;
        auto all = rep.holds();

        // Buttons 2 and 3 are left/right. In a classic level they are noise
        // that showed up as phantom notes; in a platformer level they are real
        // movement, but there is nowhere to draw them, so jump notes are what
        // gets shown either way. The macro is no longer thrown away for having
        // them, which is what used to make every platformer macro do nothing.
        size_t dropped = 0;
        f->holds.clear();
        f->holds.reserve(all.size());
        for (auto h : all) {
            if (h.button != 1) { ++dropped; continue; }
            const int64_t s = int64_t(h.start) + f->offsetFrames;
            const int64_t e = int64_t(h.end)   + f->offsetFrames;
            if (e < 0) { ++dropped; continue; }
            h.start = uint64_t(std::max<int64_t>(s, 0));
            h.end   = uint64_t(std::max<int64_t>(e, 0));
            f->holds.push_back(h);
        }
        if (dropped)
            log::info("dropped {} {} presses", dropped,
                      rep.platformer ? "left/right movement" : "non jump / out of range");

        f->hasP2 = false;
        for (auto const& h : f->holds)
            if (h.player2) { f->hasP2 = true; break; }

        f->macroSeconds = double(rep.lastFrame() + f->offsetFrames) / f->fps;
        f->state.assign(f->holds.size(), kPending);
        f->judged.assign(f->holds.size(), 0);
        f->cursor = 0;
        f->missCursor = 0;
        f->firstPressFrame = f->holds.empty() ? 0 : f->holds.front().start;

        log::info("{} presses ({}), offset {}f, first press at {:.3f}s",
                  f->holds.size(), f->hasP2 ? "dual" : "single",
                  f->offsetFrames, double(f->firstPressFrame) / f->fps);
    }

    // click accuracy

    void onPlayerInput(bool down, bool isP2) {
        auto f = m_fields.self();
        if (!f->active) return;

        const int bit = isP2 ? 2 : 1;
        int cur = f->holdMask.load(std::memory_order_relaxed);
        f->holdMask.store(down ? (cur | bit) : (cur & ~bit), std::memory_order_release);
        if (!down) return;

        const int w = f->pressWrite.load(std::memory_order_relaxed);
        const int slot = w % Fields::kQueue;
        // Read the clock here, in the update the click actually arrived in,
        // rather than working backwards from when the draw loop got round to
        // it. What gets scored is then the real error, not an estimate of it.
        f->pressTimes[slot] = absTime();
        f->pressIsP2[slot]  = isP2 ? 1 : 0;
        f->pressWrite.store(w + 1, std::memory_order_release);
    }

    void judgePress(double pressTime, bool isP2, Look const& L) {
        auto f = m_fields.self();
        // remembered before any of the early returns, so Align still works on
        // a level where nothing has matched yet
        if (f->firstClickAt < 0.0 && !isP2) f->firstClickAt = pressTime - L.offset;
        // Kept for pattern align. One click cannot tell you which press it was
        // when the anchor is a second or two out; a handful of them can.
        if (!isP2 && f->clickLog.size() < Fields::kClickLog)
            f->clickLog.push_back(pressTime - L.offset);
        if (f->holds.empty()) return;
        if (f->state.size() != f->holds.size()) return;
        if (f->judged.size() != f->holds.size()) return;

        const double now = pressTime - L.offset;
        const double missWindow = 0.30;
        const char* who = isP2 ? "P2 " : "";

        size_t best = SIZE_MAX;
        double bestDelta = 1e9;
        for (size_t i = f->cursor; i < f->holds.size(); ++i) {
            const double t0 = double(f->holds[i].start) / f->fps;
            if (t0 - now > missWindow) break;
            if (f->state[i] != kPending || f->holds[i].player2 != isP2) continue;
            if (skipP2(f->holds[i])) continue;
            const double d = now - t0;
            if (std::abs(d) < std::abs(bestDelta)) { bestDelta = d; best = i; }
        }

        if (best == SIZE_MAX || std::abs(bestDelta) > missWindow) {
            ++f->nMiss;
            const ccColor4F miss = verdictColour(1e9, L.perfectSec, L.okSec);
            flash(miss);
            if (L.showAccuracy) setVerdict(fmt::format("{}MISS", who), toC3B(miss));
            return;
        }

        // click click clique
        const bool tap = f->holds[best].length() <= tapFrames(f->fps);
        f->state[best]  = tap ? kDone : kActive;
        f->judged[best] = 1;

        // Reported in ms. Frames were misleading across macros: 30 frames off
        // is 125 ms on a 240 tps recording but 25 ms on a 1200 tps one.
        const double off = std::abs(bestDelta);
        const int ms = int(std::lround(off * 1000.0));
        const char* sign = bestDelta > 0 ? "+" : "-";

        const ccColor4F col = verdictColour(off, L.perfectSec, L.okSec);
        flash(col);
        if (off <= L.perfectSec) {
            ++f->nPerfect;
            if (L.showAccuracy) setVerdict(fmt::format("{}PERFECT", who), toC3B(col));
        } else if (off <= L.okSec) {
            ++f->nOk;
            if (L.showAccuracy) setVerdict(fmt::format("{}OK {}{}ms", who, sign, ms), toC3B(col));
        } else {
            ++f->nMiss;
            if (L.showAccuracy) setVerdict(fmt::format("{}MISS {}{}ms", who, sign, ms), toC3B(col));
        }
    }

    // beat up children part 0 (im a child)
    void scoreMissed(double now, Look const& L) {
        auto f = m_fields.self();
        if (f->judged.size() != f->holds.size()) return;
        const double late = 0.30;
        while (f->missCursor < f->holds.size()) {
            const double t0 = double(f->holds[f->missCursor].start) / f->fps;
            if (t0 > now - late) break;
            if (!f->judged[f->missCursor] && skipP2(f->holds[f->missCursor])) {
                f->judged[f->missCursor] = 1;      // phantom p2, not your fault
            } else if (!f->judged[f->missCursor]) {
                f->judged[f->missCursor] = 1;
                ++f->nMiss;
                const ccColor4F mc = verdictColour(1e9, L.perfectSec, L.okSec);
                flash(mc);
                if (L.showAccuracy)
                    setVerdict(f->holds[f->missCursor].player2 ? "P2 MISS" : "MISS",
                               toC3B(mc));
            }
            ++f->missCursor;
        }
    }

    void flash(ccColor4F c) {
        auto f = m_fields.self();
        f->flashAt = f->levelTime;
        f->flashCol = c;
    }

    void setVerdict(std::string const& text, ccColor3B col) {
        auto f = m_fields.self();
        if (!f->verdict) return;
        f->verdict->setString(text.c_str());
        f->verdict->setColor(col);
        f->verdictAt = f->levelTime;
    }

    void updateLabels(Look const& L) {
        auto f = m_fields.self();
        if (!f->verdict || !f->tally) return;
        if (!L.showAccuracy) {
            f->verdict->setVisible(false);
            f->tally->setVisible(false);
            return;
        }
        f->verdict->setVisible(true);
        f->tally->setVisible(true);
        const double age = f->levelTime - f->verdictAt;
        const float a = age < 0.5 ? 1.f : std::max(0.f, 1.f - float((age - 0.5) / 0.6));
        f->verdict->setOpacity(GLubyte(a * 255.f));
        f->tally->setString(fmt::format("{} / {} / {}", f->nPerfect, f->nOk, f->nMiss).c_str());
    }

    // indicator which the BMWs don't use :laugh please:

    void drawWorld(float px, double now, Look const& L) {
        auto f = m_fields.self();
        auto n = f->world;
        n->clear();

        const auto win = CCDirector::get()->getWinSize();
        const CCPoint bl = m_objectLayer->convertToNodeSpace({ 0.f, 0.f });
        const CCPoint tr = m_objectLayer->convertToNodeSpace({ win.width, win.height });
        const float yLo = std::min(bl.y, tr.y) - 60.f;
        const float yHi = std::max(bl.y, tr.y) + 60.f;
        const float viewL = std::min(bl.x, tr.x);
        const float viewR = std::max(bl.x, tr.x);

        const float zoom  = m_objectLayer->getScale();
        const float half  = (L.badgeSize * 0.5f) / (zoom > 0.01f ? zoom : 1.f);
        const float thick = std::max(half * 0.34f, 0.6f);
        const float p1Y = m_player1 ? m_player1->getPositionY() : (bl.y + tr.y) * .5f;
        const float p2Y = m_player2 ? m_player2->getPositionY() : p1Y;

        const int held = f->holdMask.load(std::memory_order_acquire);

        // beat up children by draining the indicator.
        // Position only, NOT state. A press that has scrolled off the left can
        // never be hit or drawn again whether or not it was clicked, and gating
        // this on kDone meant one missed note pinned the cursor for the rest of
        // the run and every scan below kept growing.
        // Gated on posClock: before the level scan lands, xAt is guessing off a
        // measured speed and could report the whole macro as already behind us,
        // which would skip it silently. Advancing the cursor cannot be undone,
        // so it waits for the real timeline.
        while (f->posClock
               && f->cursor < f->holds.size()
               && xRel(double(f->holds[f->cursor].end) / f->fps, now, px) < viewL)
            ++f->cursor;

        int drawnCount = 0;
        for (size_t i = f->cursor; i < f->holds.size(); ++i) {
            auto const& h = f->holds[i];
            if (f->state[i] == kDone) continue;
            if (skipP2(h)) continue;
            if (drawnCount >= L.maxNotes) break;   // dense macros drown the screen

            const double t0 = double(h.start) / f->fps;
            const double t1 = double(h.end) / f->fps;
            if (t0 - now > L.ahead) break;

            const float x0 = xRel(t0, now, px);
            const float x1 = xRel(t1, now, px);
            if (std::max(x1, x0) < viewL) continue;

            const ccColor4F c = h.player2 ? L.p2 : L.indicator;
            const bool tap = h.length() <= tapFrames(f->fps);

            // drain bar
            const bool riding = L.holdTracks && !tap
                             && f->state[i] == kActive
                             && (held & (h.player2 ? 2 : 1)) != 0;

            float left, right;
            if (tap) {
                left  = x0 - L.width * .5f;
                right = x0 + L.width * .5f;
            } else {
                left  = riding ? std::max(px, x0) : x0;
                right = std::max(x1, x0 + L.width);
            }
            if (right <= left || left > viewR) continue;

            quad(n, left, right, yLo, yHi, c);
            ++drawnCount;

            if (!riding) {
                ccColor4F m = c;
                if (L.edgeMarker) {
                    m.a = std::clamp(c.a * 2.4f + 0.12f, 0.f, 1.f);
                    quad(n, left, left + std::max(1.5f, L.width * 0.4f), yLo, yHi, m);
                }
                if (L.pressBadge) {
                    m.a = std::clamp(c.a * 3.2f + 0.4f, 0.f, 1.f);
                    square(n, left, h.player2 ? p2Y : p1Y, half, thick, m);
                }
            }
        }

        if (L.showPlayer)
            quad(n, px - L.width * .5f, px + L.width * .5f, yLo, yHi, L.player);

        // if then if if if if if if i code ts like toby fox lmaoooooooooooooooooo
        if (L.playerSquare) {
            ccColor4F pc = L.player;
            pc.a = std::clamp(pc.a * 1.6f + 0.2f, 0.f, 1.f);
            square(n, px, p1Y, half * 1.35f, thick, pc);
            if (p2Live() && m_player2)
                square(n, m_player2->getPositionX(), p2Y, half * 1.35f, thick, pc);
        }
    }

    // ------------------------------------------------------------ ladder lane (I used AI here)


    // flat osu mania lane. no perspective, no rails, just columns with dots
    // and grey capsules for holds. totally separate from the ladder path.
    // straight osu mania. flat lane, solid discs, grey capsules, black
    // receptors at the bottom. no rims, no perspective, no hit bar.
    // osu mania, circle skin. black lane, big solid discs at full column
    // width, white ring receptors at the base. no rims on the notes.
    // osu mania. white outlined discs, coloured capsules for holds with a
    // brighter head, dashed empty rings for receptors.
    void drawLaneCircles(double now, Look const& L) {
        auto f = m_fields.self();
        auto n = f->lane;
        n->clear();
        hideSprites();

        const auto win = CCDirector::get()->getWinSize();
        const float k     = std::clamp(L.laneScale, 0.4f, 2.5f);
        const bool  split = p2Live() && L.splitLane;
        const int   cols  = split ? 2 : 1;
        // Laid out standing up, then the container is turned on its side for
        // Middle. So in Middle the thickness comes off screen height and the
        // length off screen width, which is what lets it span the whole screen.
        const float colW  = L.laneMiddle ? win.height * 0.132f * k
                                         : win.width  * 0.082f * k;
        const float total = cols * colW;
        const float left  = (L.laneMiddle || L.laneLeft)
                          ? 20.f : win.width - 20.f - total;
        const float dia   = colW * 0.86f;          // outer, including the white rim
        const float inner = dia * 0.84f;           // the coloured part
        const float hitY  = win.height * 0.10f + dia * 0.5f;
        // Middle is laid out standing up and then turned on its side, so its
        // LENGTH becomes horizontal. It has to fit in whatever is left between
        // the hit line and the far edge of the screen, or the far end of the
        // ladder runs off screen and the oldest notes are invisible. That was
        // already happening at the old fixed 0.88 of screen width, and moving
        // the hit line towards the centre would have made it far worse.
        const float midRoom = std::max(60.f, win.width * (1.f - L.midHit) - 24.f);
        const float topY  = hitY + (L.laneMiddle
                                    ? std::min(midRoom, win.width * 0.88f * k)
                                    : win.height * 0.80f * k);
        const float span  = topY - hitY;
        const ccColor4F WHITE = { 1.f, 1.f, 1.f, 0.95f };

        auto colX = [&](bool p2) {
            return left + colW * 0.5f + ((split && p2) ? colW : 0.f);
        };

        f->lbX0 = left; f->lbX1 = left + total;
        f->lbY0 = hitY - dia * 0.7f; f->lbY1 = topY;
        f->lbSet = true;

        auto lY = [&](float y) { return L.flipLane ? win.height - y : y; };
        // The labels are children of the play layer, not of the lane, so they
        // are placed in screen space. For the upright lane the lane's own
        // coordinates happen to be screen coordinates and this works. For
        // Middle the lane gets rotated a quarter turn underneath them, so the
        // same numbers put the score off in a corner: place it against the
        // band's real screen position instead, centred and just below it.
        if (L.laneMiddle) {
            const float band = total;
            const float bandBot = win.height - win.height * 0.025f - band;
            if (f->verdict) f->verdict->setPosition({ win.width * .5f, bandBot - 14.f });
            if (f->tally)   f->tally->setPosition({ win.width * .5f, bandBot - 32.f });
        } else {
            if (f->verdict) f->verdict->setPosition({ left + total * .5f, lY(hitY - dia * 0.9f) });
            if (f->tally)   f->tally->setPosition({ left + total * .5f, lY(hitY - dia * 1.25f) });
        }
        if (!L.showLane) return;

        // dark shaft with a bright rule down each outer edge
        quad(n, left, left + total, hitY - dia * 0.7f, topY, { 0.f, 0.f, 0.f, 0.45f });
        const ccColor4F rule = { 0.85f, 0.95f, 1.f, 0.85f };
        quad(n, left - 2.f, left, hitY - dia * 0.7f, topY, rule);
        quad(n, left + total, left + total + 2.f, hitY - dia * 0.7f, topY, rule);

        f->noteAtLine = false;

        const double perfSec = L.perfectSec;
        auto yOfT = [&](double t) { return hitY + float((t - now) / L.laneWindow) * span; };

        // one unbroken ring, chords stepped all the way round with no gaps
        auto solidRing = [&](float cx, float cy, float r, float thick, ccColor4F c) {
            const int SEG = 40;
            const float TAU = 6.2831853f;
            for (int i = 0; i < SEG; ++i) {
                const float b0 = float(i)     * TAU / SEG;
                const float b1 = float(i + 1) * TAU / SEG;
                stroke(n, { cx + r * std::cos(b0), cy + r * std::sin(b0) },
                          { cx + r * std::cos(b1), cy + r * std::sin(b1) }, thick, c);
            }
        };

        int laneCount = 0;
        for (int pass = 0; pass < 2; ++pass) {
            int seen = 0;
            for (size_t i = f->cursor; i < f->holds.size(); ++i) {
                auto const& h = f->holds[i];
                if (f->state[i] == kDone) continue;
                if (skipP2(h)) continue;
                if (seen >= L.maxNotes) break;

                const double t0 = double(h.start) / f->fps;
                const double t1 = double(h.end) / f->fps;
                if ((t0 - now) / L.laneWindow > 1.0) break;

                const bool tap    = h.length() <= tapFrames(f->fps);
                const bool active = f->state[i] == kActive;
                const double uEnd = (t1 - now) / L.laneWindow;
                if ((tap ? (t0 - now) / L.laneWindow : uEnd) < -0.25) continue;

                const float cx = colX(h.player2);
                const float y0 = std::max(yOfT(t0), hitY);
                if (y0 > topY + dia) continue;
                ++seen;

                const bool onTime = f->state[i] == kPending && std::abs(t0 - now) <= perfSec;

                ccColor4F body = h.player2 ? L.laneP2 : L.lane;
                body.a = std::clamp(body.a * 2.4f + 0.25f, 0.f, 1.f);
                if (active) body = lighten(body, 1.25f);

                if (pass == 0) {
                    // a hold is ONE pill, length = how long you hold it. no
                    // separate head disc, that is what made it look like two
                    // circles welded together.
                    if (tap) continue;
                    if (onTime || active) f->noteAtLine = true;
                    ccColor4F fill = onTime ? ccColor4F{ 1.f, 1.f, 1.f, 1.f } : body;
                    const float y1 = std::max(std::min(yOfT(t1), topY), y0);
                    stadium(n, cx, y0, y1, dia, WHITE);     // outline
                    stadium(n, cx, y0, y1, inner, fill);    // fill
                    ++laneCount;
                } else {
                    if (!tap) continue;
                    if (onTime) f->noteAtLine = true;
                    ccColor4F fill = onTime ? ccColor4F{ 1.f, 1.f, 1.f, 1.f } : body;
                    placeCircle(take(f->discs, f->discUsed, kDisc), cx, y0, dia, WHITE);
                    placeCircle(take(f->discs, f->discUsed, kDisc), cx, y0, inner, fill);
                    ++laneCount;
                }
            }
        }

        // solid outlined receptors, empty inside
        const double age = f->levelTime - f->flashAt;
        ccColor4F ring = { 0.88f, 0.90f, 0.94f, 0.85f };
        if (age >= 0 && age < kFlashTime)
            ring = f->flashCol;
        else if (f->noteAtLine) ring = { 1.f, 1.f, 1.f, 1.f };

        for (int i = 0; i < cols; ++i)
            solidRing(left + colW * 0.5f + i * colW, hitY, dia * 0.5f, dia * 0.032f, ring);
    }

    // Flat style for the Middle lane. Straight lines only: two thin rails, a
    // bar for the receptor, and rectangles for the notes with a lighter head
    // at the leading edge. Drawn standing up like the others, the container
    // turns it on its side, so "along time" ends up running across the screen.
    void drawLaneFlat(double now, Look const& L) {
        auto f = m_fields.self();
        auto n = f->lane;
        n->clear();
        hideSprites();

        const auto win = CCDirector::get()->getWinSize();
        const float k     = std::clamp(L.laneScale, 0.4f, 2.5f);
        const bool  split = p2Live() && L.splitLane;
        const int   cols  = split ? 2 : 1;
        const float colW  = win.height * 0.104f * k;
        const float total = cols * colW;
        const float left  = 20.f;
        const float hitY  = win.height * 0.10f;
        const float topY  = hitY + win.width * 0.70f * k;
        const float span  = topY - hitY;

        const float rail  = colW * 0.045f;      // rail thickness
        const float headW = colW * 0.075f;      // head bar, along time
        const float recW  = colW * 0.055f;      // receptor bar, along time
        const float ext   = colW * 0.10f;       // how far those overhang the rails

        f->lbX0 = left - ext; f->lbX1 = left + total + ext;
        f->lbY0 = hitY - recW * 2.8f; f->lbY1 = topY;
        f->lbSet = true;

        auto lY = [&](float y) { return L.flipLane ? win.height - y : y; };
        if (f->verdict) f->verdict->setPosition({ left + total * .5f, lY(hitY - colW * 0.5f) });
        if (f->tally)   f->tally->setPosition({ left + total * .5f, lY(hitY - colW * 0.85f) });
        if (!L.showLane) return;

        auto colBase = [&](bool p2) { return left + ((split && p2) ? colW : 0.f); };

        // the two rails the notes run between
        ccColor4F railCol = L.lane;
        railCol.a = std::clamp(railCol.a * 1.9f + 0.15f, 0.f, 1.f);
        for (int i = 0; i <= cols; ++i) {
            const float x = left + float(i) * colW;
            quad(n, x - rail * .5f, x + rail * .5f, hitY, topY, railCol);
        }

        f->noteAtLine = false;

        const double perfSec = L.perfectSec;
        auto yOfT = [&](double t) { return hitY + float((t - now) / L.laneWindow) * span; };

        int seen = 0;
        for (size_t i = f->cursor; i < f->holds.size(); ++i) {
            auto const& h = f->holds[i];
            if (f->state[i] == kDone) continue;
            if (skipP2(h)) continue;
            if (seen >= L.maxNotes) break;

            const double t0 = double(h.start) / f->fps;
            const double t1 = double(h.end) / f->fps;
            if ((t0 - now) / L.laneWindow > 1.0) break;

            const bool tap    = h.length() <= tapFrames(f->fps);
            const bool active = f->state[i] == kActive;
            const double uEnd = (t1 - now) / L.laneWindow;
            if ((tap ? (t0 - now) / L.laneWindow : uEnd) < -0.25) continue;

            const float x0 = colBase(h.player2);
            const float y0 = std::max(yOfT(t0), hitY);
            if (y0 > topY) continue;
            ++seen;

            const bool onTime = f->state[i] == kPending && std::abs(t0 - now) <= perfSec;
            if (onTime || active) f->noteAtLine = true;

            ccColor4F edge = h.player2 ? L.laneP2 : L.lane;
            edge.a = std::clamp(edge.a * 2.3f + 0.25f, 0.f, 1.f);
            if (active) edge = lighten(edge, 1.2f);
            ccColor4F fill = edge;
            fill.a *= 0.42f;

            const float y1 = tap ? y0 + headW : std::min(yOfT(t1), topY);
            const float bx0 = x0 + colW * 0.10f;
            const float bx1 = x0 + colW * 0.90f;

            if (y1 > y0 + 0.5f) {
                // body: outline drawn as four thin bars so the middle stays
                // see through, same as the reference
                const float b = colW * 0.028f;
                quad(n, bx0, bx1, y0, y1, fill);
                quad(n, bx0, bx0 + b, y0, y1, edge);
                quad(n, bx1 - b, bx1, y0, y1, edge);
                quad(n, bx0, bx1, y0, y0 + b, edge);
                quad(n, bx0, bx1, y1 - b, y1, edge);
            }

            // head bar at the leading edge, lighter and slightly taller
            ccColor4F head = onTime ? ccColor4F{ 1.f, 1.f, 1.f, 1.f }
                                    : ccColor4F{ 0.86f, 0.88f, 0.92f, edge.a };
            quad(n, x0 - ext * .35f, x0 + colW + ext * .35f, y0, y0 + headW, head);
            quad(n, x0 + colW * 0.16f, x0 + colW * 0.84f,
                 y0 + headW * 0.22f, y0 + headW * 0.78f, fill);
        }

        // Receptor. The circle lane draws an unbroken empty ring, so this is
        // the same idea squared off: a hollow outline the notes land inside,
        // rather than the solid slab it used to be.
        const double age = f->levelTime - f->flashAt;
        ccColor4F bar = { 1.f, 1.f, 1.f, 0.95f };
        if (age >= 0 && age < kFlashTime) bar = f->flashCol;
        else if (f->noteAtLine) bar = { 1.f, 1.f, 1.f, 1.f };

        const float rx0 = left - ext;
        const float rx1 = left + total + ext;
        const float ry0 = hitY - recW * 2.6f;
        const float ry1 = hitY + recW * 2.6f;
        const float rt  = colW * 0.034f;          // outline thickness
        quad(n, rx0, rx1, ry0, ry0 + rt, bar);    // along time, near side
        quad(n, rx0, rx1, ry1 - rt, ry1, bar);    // along time, far side
        quad(n, rx0, rx0 + rt, ry0, ry1, bar);    // across lane
        quad(n, rx1 - rt, rx1, ry0, ry1, bar);
    }

    void drawLane(double now, Look const& L) {
        // Middle gets its own flat look regardless of note shape
        if (L.laneMiddle)  { drawLaneFlat(now, L); return; }
        if (L.circleNotes) { drawLaneCircles(now, L); return; }
        hideSprites();
        auto f = m_fields.self();
        auto n = f->lane;
        n->clear();

        const auto win = CCDirector::get()->getWinSize();
        const float k    = std::clamp(L.laneScale, 0.4f, 2.5f);
        const float halfBot = (L.laneMiddle ? win.height * 0.066f
                                            : win.width  * 0.041f) * k;
        const float cx   = L.laneMiddle ? halfBot + 20.f
                         : (L.laneLeft ? win.width * 0.115f : win.width * 0.885f);
        const float hitY = win.height * 0.215f;
        const float topY = hitY + (L.laneMiddle ? win.width * 0.80f
                                                : win.height * 0.45f) * k;
        const float span = topY - hitY;
        const float railBot = (L.laneMiddle ? win.height * 0.009f
                                            : win.width  * 0.0055f) * k;
        const float rungBot = span * 0.062f;

        // Perspective. Depth z grows with time to the press, width goes as 1/z,
        // and screen height follows so that width is linear in height, which is
        // what a one point projection gives you.
        const float depth = std::clamp(L.laneDepth, 0.2f, 4.f);
        const float sTop  = 1.f / (1.f + depth);
        auto scaleAt = [&](double u) { return 1.f / (1.f + depth * float(u)); };
        auto yOf     = [&](float s) { return hitY + span * (1.f - s) / (1.f - sTop); };

        f->lbX0 = cx - halfBot; f->lbX1 = cx + halfBot;
        f->lbY0 = hitY - win.height * 0.02f; f->lbY1 = topY;
        f->lbSet = true;

        auto lY = [&](float y) { return L.flipLane ? win.height - y : y; };
        if (f->verdict) f->verdict->setPosition({ cx, lY(hitY - win.height * 0.055f) });
        if (f->tally)   f->tally->setPosition({ cx, lY(hitY - win.height * 0.095f) });

        if (!L.showLane) return;

        const int held = f->holdMask.load(std::memory_order_acquire);
        const bool anyHeld = held != 0;

        ccColor4F rail = lighten(L.lane, 1.0f);
        rail.a = std::clamp(rail.a * 1.9f + 0.10f, 0.f, 1.f);
        ccColor4F face   = rail;
        ccColor4F faceP2 = L.laneP2;
        faceP2.a = rail.a;
        ccColor4F top    = lighten(rail, 1.45f);
        ccColor4F topP2  = lighten(faceP2, 1.45f);
        ccColor4F shade  = lighten(rail, 0.42f);
        ccColor4F shadeP2 = lighten(faceP2, 0.42f);
        // Hold bodies sit behind the slab in a darkened shade of the same colour.
        ccColor4F bodyC   = lighten(rail, 0.55f);
        ccColor4F bodyP2  = lighten(faceP2, 0.55f);

        // ---- rails, tapering with the perspective
        {
            const float ht = halfBot * sTop, rt = railBot * sTop;
            poly(n, { cx - halfBot, hitY }, { cx - halfBot + railBot, hitY },
                    { cx - ht + rt, topY }, { cx - ht, topY }, rail);
            poly(n, { cx + halfBot - railBot, hitY }, { cx + halfBot, hitY },
                    { cx + ht, topY }, { cx + ht - rt, topY }, rail);
        }

        // green zone bands under the notes, real windows not vibes
        // sized off the actual perfect/ok frame settings
        const double perfSec = L.perfectSec;
        const double okSec   = L.okSec;
        auto yForSec = [&](double sec) { return yOf(scaleAt(sec / L.laneWindow)); };
        {
            ccColor4F okBand = L.lane;
            okBand.a = std::clamp(okBand.a * 0.55f, 0.f, 1.f);
            quad(n, cx - halfBot * 0.94f, cx + halfBot * 0.94f,
                    hitY, yForSec(okSec), okBand);

            // target slot is exactly one slab tall so a note fills it dead on
            ccColor4F perfBand = L.player;
            perfBand.a = std::clamp(std::max(L.player.a, 0.5f) * 0.75f, 0.f, 1.f);
            quad(n, cx - halfBot * 0.98f, cx + halfBot * 0.98f,
                    hitY, hitY + rungBot, perfBand);
        }

        f->noteAtLine = false;

        // ---- rungs and holds
        for (size_t i = f->cursor; i < f->holds.size(); ++i) {
            auto const& h = f->holds[i];
            if (f->state[i] == kDone) continue;

            const double t0 = double(h.start) / f->fps;
            const double t1 = double(h.end) / f->fps;
            double u0 = (t0 - now) / L.laneWindow;
            if (u0 > 1.0) break;

            const bool tap = h.length() <= tapFrames(f->fps);
            const bool active = f->state[i] == kActive;
            const double uEnd = (t1 - now) / L.laneWindow;

            // more osu mania stuff
            if ((tap ? u0 : uEnd) < -0.25) continue;
            u0 = std::max(u0, 0.0);

            const float s0 = scaleAt(u0);
            const float y0 = yOf(s0);
            const float hw0 = halfBot * s0;

            ccColor4F fc = h.player2 ? faceP2 : face;
            ccColor4F tc = h.player2 ? topP2 : top;
            ccColor4F sc = h.player2 ? shadeP2 : shade;
            ccColor4F bc = h.player2 ? bodyP2 : bodyC;
            if (active) { fc = lighten(fc, 1.5f); tc = lighten(tc, 1.5f); bc = lighten(bc, 1.5f); f->noteAtLine = true; }

            // perfect score like my ass (perfect)
            const bool onTime = f->state[i] == kPending && std::abs(t0 - now) <= perfSec;
            if (onTime) {
                fc = { 1.f, 1.f, 1.f, std::clamp(fc.a * 1.8f + 0.25f, 0.f, 1.f) };
                tc = fc;
            }

            // kay?
            float colC = cx, colH = hw0;
            if (p2Live() && L.splitLane) {
                colH = hw0 * 0.47f;
                colC = cx + (h.player2 ? hw0 * 0.53f : -hw0 * 0.53f);
            }

            // Amen
            const float th = rungBot * s0;
            const float capTop = y0 + th * 1.34f;
            if (onTime) f->noteAtLine = true;

            if (onTime) {
                ccColor4F halo = fc;
                halo.a *= 0.45f;
                quad(n, colC - colH * 1.22f, colC + colH * 1.22f,
                        y0 - th * 0.55f, capTop + th * 0.4f, halo);
            }
            quad(n, colC - colH, colC + colH, y0 - th * 0.16f, y0, sc);
            quad(n, colC - colH, colC + colH, y0, y0 + th, fc);
            quad(n, colC - colH, colC + colH, y0 + th, capTop, tc);

            // OSU Mania bullshit
            if (!tap) {
                const double u1 = std::clamp(uEnd, u0, 1.0);
                const float s1 = scaleAt(u1);
                const float y1 = std::max(yOf(s1), capTop + th * 0.4f);

                float colH1 = halfBot * s1;
                float colC1 = cx;
                if (p2Live() && L.splitLane) {
                    colC1 = cx + (h.player2 ? colH1 * 0.53f : -colH1 * 0.53f);
                    colH1 *= 0.47f;
                }
                const float bw0 = colH * kBodyWidth;
                const float bw1 = colH1 * kBodyWidth;

                poly(n, { colC - bw0, capTop }, { colC + bw0, capTop },
                        { colC1 + bw1, y1 }, { colC1 - bw1, y1 }, bc);
                quad(n, colC1 - bw1, colC1 + bw1, y1, y1 + rungBot * s1 * 0.30f,
                     lighten(bc, 1.45f));
            }
        }

        // hit bar maybe good maybe not
        const double age = f->levelTime - f->flashAt;
        ccColor4F bar = L.player;
        bar.a = std::max(bar.a, 0.75f);
        // lights up the moment a bar reaches all the way down
        if (f->noteAtLine) bar = { 1.f, 1.f, 1.f, 1.f };
        if (age >= 0 && age < kFlashTime) {
            const float m = 1.f - float(age / kFlashTime) * 0.35f;
            bar = { f->flashCol.r * m, f->flashCol.g * m, f->flashCol.b * m, 1.f };
        }
        const float pad = halfBot * 0.10f;
        const float barH = span * 0.022f;
        quad(n, cx - halfBot - pad, cx + halfBot + pad, hitY - barH, hitY + barH, bar);

        // how tf did this work lmao
        ccColor4F chev = anyHeld || (age >= 0 && age < kFlashTime)
                       ? bar : ccColor4F{ 0.78f, 0.80f, 0.82f, std::max(L.player.a, 0.6f) };
        const float cs = halfBot * 0.42f;
        const float ct = halfBot * 0.10f;
        const float gap = halfBot * 0.18f;
        const float lx = cx - halfBot - pad - gap;
        const float rx = cx + halfBot + pad + gap;
        stroke(n, { lx - cs, hitY + cs }, { lx, hitY }, ct, chev);
        stroke(n, { lx - cs, hitY - cs }, { lx, hitY }, ct, chev);
        stroke(n, { rx + cs, hitY + cs }, { rx, hitY }, ct, chev);
        stroke(n, { rx + cs, hitY - cs }, { rx, hitY }, ct, chev);
    }
};

// input player client side only but what if the client is fat if (player = fat) sudo apt install opsec delete sys32 (this is a joke ignore)
class $modify(IndicatorInput, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
        if (button != 1) return;
        auto pl = PlayLayer::get();
        if (!pl || pl != static_cast<GJBaseGameLayer*>(this)) return;
        if (!pl->m_player1) return;
        static_cast<IndicatorLayer*>(pl)->onPlayerInput(down, !isPlayer1);
    }
};

// ---------------------------------------------------------------- pause ui

// built on plain cocos and gd bindings only. geode's Popup and ScrollLayer
// templates move between sdk versions and kept failing to resolve, so this
// uses nothing that can wander off: a colour layer, a scale9 panel, menus.
class MacroPopup : public CCLayerColor, public FLAlertLayerProtocol {
    CCNode*        m_panel = nullptr;
    CCMenu*        m_rows  = nullptr;
    CCLabelBMFont* m_info  = nullptr;
    int            m_page  = 0;
    std::vector<CCLabelBMFont*> m_labels;

    static constexpr int   kPerPage = 5;      // rows per page
    static constexpr float kW = 380.f;
    static constexpr float kH = 290.f;

    static CCMenuItemSpriteExtra* mkBtn(char const* text, float w,
                                        CCObject* t, SEL_MenuHandler sel) {
        auto spr = ButtonSprite::create(text);
        if (spr && spr->getContentSize().width > w)
            spr->setScale(w / spr->getContentSize().width);
        return CCMenuItemSpriteExtra::create(spr, t, sel);
    }

public:
    static MacroPopup* create() {
        auto ret = new MacroPopup();
        if (ret && ret->build()) { ret->autorelease(); return ret; }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }

    bool build() {
        if (!this->initWithColor({ 0, 0, 0, 150 })) return false;
        const auto win = CCDirector::get()->getWinSize();

        auto panel = cocos2d::extension::CCScale9Sprite::create("GJ_square01.png");
        if (!panel) return false;
        panel->setContentSize({ kW, kH });
        panel->setPosition({ win.width * .5f, win.height * .5f });
        this->addChild(panel);
        m_panel = panel;

        const float bx = win.width * .5f - kW * .5f;
        const float by = win.height * .5f - kH * .5f;

        auto title = CCLabelBMFont::create("Click Guide", "goldFont.fnt");
        title->setPosition({ win.width * .5f, by + kH - 22.f });
        title->setScale(0.8f);
        this->addChild(title);

        auto menu = CCMenu::create();
        menu->setPosition({ 0.f, 0.f });
        menu->setTouchPriority(-600);
        this->addChild(menu);

        if (auto x = CCSprite::createWithSpriteFrameName("GJ_closeBtn_001.png")) {
            x->setScale(0.7f);
            auto b = CCMenuItemSpriteExtra::create(x, this, menu_selector(MacroPopup::onClose));
            b->setPosition({ bx + 16.f, by + kH - 16.f });
            menu->addChild(b);
        }

        auto bImport  = mkBtn("Import Macro", 96.f, this, menu_selector(MacroPopup::onImport));
        auto bRefresh = mkBtn("Refresh", 62.f, this, menu_selector(MacroPopup::onRefresh));
        auto bSet     = mkBtn("Settings", 70.f, this, menu_selector(MacroPopup::onSettings));
        bImport->setPosition({ bx + kW * .24f, by + kH - 54.f });
        bRefresh->setPosition({ bx + kW * .55f, by + kH - 54.f });
        bSet->setPosition({ bx + kW * .82f, by + kH - 54.f });
        menu->addChild(bImport);
        menu->addChild(bRefresh);
        menu->addChild(bSet);

        auto prev = mkBtn("<", 26.f, this, menu_selector(MacroPopup::onPrev));
        auto next = mkBtn(">", 26.f, this, menu_selector(MacroPopup::onNext));
        prev->setPosition({ bx + 26.f, by + 62.f });
        next->setPosition({ bx + kW - 26.f, by + 62.f });
        menu->addChild(prev);
        menu->addChild(next);

        // timing row. Align snaps the macro to where you actually clicked,
        // the arrows nudge it a frame at a time if you want it exact.
        auto bAlign = mkBtn("Align", 56.f, this, menu_selector(MacroPopup::onAlign));
        auto bMinus = mkBtn("-1f", 30.f, this, menu_selector(MacroPopup::onMinus));
        auto bPlus  = mkBtn("+1f", 30.f, this, menu_selector(MacroPopup::onPlus));
        auto bZero  = mkBtn("Reset", 50.f, this, menu_selector(MacroPopup::onZero));
        bAlign->setPosition({ bx + kW * 0.17f, by + 20.f });
        bMinus->setPosition({ bx + kW * 0.41f, by + 20.f });
        bPlus->setPosition({ bx + kW * 0.56f, by + 20.f });
        bZero->setPosition({ bx + kW * 0.82f, by + 20.f });
        menu->addChild(bAlign);
        menu->addChild(bMinus);
        menu->addChild(bPlus);
        menu->addChild(bZero);

        m_rows = CCMenu::create();
        m_rows->setPosition({ 0.f, 0.f });
        m_rows->setTouchPriority(-610);
        this->addChild(m_rows);

        m_info = CCLabelBMFont::create("", "chatFont.fnt");
        m_info->setScale(0.46f);
        m_info->setPosition({ win.width * .5f, by + 42.f });
        this->addChild(m_info);

        this->setTouchEnabled(true);
        this->setKeypadEnabled(true);
        refresh();
        return true;
    }

    void keyBackClicked() { onClose(nullptr); }

    GJGameLevel* level() {
        auto pl = PlayLayer::get();
        return pl ? pl->m_level : nullptr;
    }

    void refresh() {
        m_rows->removeAllChildren();
        const auto entries = listMacros();
        const auto picked = pickedMacro(level());

        const int pages = std::max(1, int((entries.size() + kPerPage - 1) / kPerPage));
        m_page = std::clamp(m_page, 0, pages - 1);

        const auto win = CCDirector::get()->getWinSize();
        const float bx = win.width * .5f - kW * .5f;
        const float by = win.height * .5f - kH * .5f;
        const float top = by + kH - 84.f;

        for (int r = 0; r < kPerPage; ++r) {
            const size_t idx = size_t(m_page * kPerPage + r);
            if (idx >= entries.size()) break;
            auto const& e = entries[idx];
            const float y = top - 27.f * float(r);
            const bool on = (e.file == picked);

            auto name = CCLabelBMFont::create(e.file.c_str(), "chatFont.fnt");
            name->setAnchorPoint({ 0.f, 0.5f });
            name->setScale(0.5f);
            if (name->getContentSize().width * 0.5f > kW - 130.f)
                name->setScale((kW - 130.f) / name->getContentSize().width);
            name->setPosition({ bx + 16.f, y });
            name->setColor(on ? ccColor3B{ 110, 255, 150 } : ccColor3B{ 235, 235, 235 });
            this->addChild(name);
            m_labels.push_back(name);

            auto btn = mkBtn(on ? "Loaded" : "Load", 54.f, this,
                             menu_selector(MacroPopup::onLoad));
            btn->setPosition({ bx + kW - 44.f, y });
            btn->setTag(int(idx));
            m_rows->addChild(btn);
        }

        if (entries.empty()) {
            m_info->setString("No macros in the folder (.gdr2 .gdr .xd .slc)");
        } else {
            auto it = std::find_if(entries.begin(), entries.end(),
                                   [&](MacroEntry const& e) { return e.file == picked; });
            if (it == entries.end())
                m_info->setString(fmt::format("{} macros, page {}/{}",
                                              entries.size(), m_page + 1, pages).c_str());
            else {
                auto l = layer();
                const int off = l ? l->currentOffset() : 0;
                m_info->setString(fmt::format("{} - {} inputs @ {:.0f} FPS - offset {:+d}f",
                                              it->file, it->inputs, it->fps, off).c_str());
            }
        }
    }

    void clearLabels() {
        for (auto l : m_labels) if (l) l->removeFromParent();
        m_labels.clear();
    }

    IndicatorLayer* layer() {
        auto pl = PlayLayer::get();
        return pl ? static_cast<IndicatorLayer*>(pl) : nullptr;
    }

    void onAlign(CCObject*) {
        auto l = layer();
        if (!l) return;

        // Several clicks beat one, so use the rhythm whenever there is one.
        if (l->canPatternAlign()) {
            const int off = l->alignByPattern();
            if (off != INT_MIN) {
                clearLabels();
                refresh();
                FLAlertLayer::create("Align",
                    "Matched the rhythm of your clicks against the macro and "
                    "locked it on.\n\nThis is the one to use after a start "
                    "position: play the section through once, then hit Align.",
                    "OK")->show();
                return;
            }
        }

        if (!l->canAlign()) {
            FLAlertLayer::create(
                "Align",
                "Align needs real clicks from you first. Play the section "
                "through once, then come back and hit Align.\n\nA macro "
                "recorded from the start of a level usually needs none of "
                "this: it lines up on its own. Align is for start positions "
                "in a part of the level you have never played through, where "
                "the mod has to estimate how far in you are.",
                "OK")->show();
            return;
        }
        const int off = l->alignToFirstClick();
        if (off == INT_MIN) {
            FLAlertLayer::create("Align",
                "Could not align. Your click and the macro's first press were "
                "too far apart to be the same one. Try playing a few seconds "
                "of the section first, so Align has a rhythm to match.",
                "OK")->show();
            return;
        }
        clearLabels();
        refresh();
    }

    void nudge(int by) {
        auto l = layer();
        if (!l) return;
        l->setOffset(l->currentOffset() + by);
        clearLabels();
        refresh();
    }
    void onMinus(CCObject*) { nudge(-1); }
    void onPlus(CCObject*)  { nudge(+1); }
    void onZero(CCObject*) {
        auto l = layer();
        if (!l) return;
        l->setOffset(0);
        clearLabels();
        refresh();
    }

    void onPrev(CCObject*) { clearLabels(); --m_page; refresh(); }
    void onNext(CCObject*) { clearLabels(); ++m_page; refresh(); }
    void onRefresh(CCObject*) { clearLabels(); refresh(); }

    void onLoad(CCObject* sender) {
        const auto entries = listMacros();
        const int idx = sender->getTag();
        if (idx < 0 || idx >= int(entries.size())) return;

        auto lvl = level();
        const bool already = pickedMacro(lvl) == entries[idx].file;
        setPickedMacro(lvl, already ? "" : entries[idx].file);

        if (auto pl = PlayLayer::get())
            static_cast<IndicatorLayer*>(pl)->reloadMacro();
        clearLabels();
        refresh();
    }

    // Copy one picked file into the macros folder without clobbering anything
    // already in there. Returns the name it was saved as, or empty on failure.
    static std::string adoptMacro(std::filesystem::path const& src) {
        auto name = src.filename();
        if (name.empty()) return {};

        // The android picker cannot filter by extension, so whatever comes
        // back may not even be a macro. Parsing it is the real test, and it
        // also means a correct file with an odd extension still gets in.
        if (!fmts::knownExtension(name.extension().string()))
            name += ".gdr2";

        std::error_code ec;
        auto dest = macroDir() / name;
        for (int n = 2; std::filesystem::exists(dest, ec) && n < 100; ++n) {
            auto stem = name.stem().string() + " (" + std::to_string(n) + ")";
            dest = macroDir() / (stem + name.extension().string());
        }

        std::filesystem::copy_file(src, dest, ec);
        if (ec) {
            log::warn("could not copy {} in: {}", src.string(), ec.message());
            return {};
        }
        return dest.filename().string();
    }

    void onImport(CCObject*) {
#ifdef GEODE_IS_WINDOWS
        // Windows can just open the folder, and that is the nicer answer there
        // anyway: you can drop a whole batch in at once.
        //
        // It does not use the picker below for a dull reason. Geode's
        // async::spawn, which is how a picker result gets back onto the main
        // thread, crashes MSVC 14.44 outright with an internal compiler error,
        // on both of its overloads and on a six line file that does nothing
        // else. Nothing to route around in this mod, and openFolder already
        // works here, so Windows keeps it.
        const bool opened = geode::utils::file::openFolder(macroDir());
        if (opened) {
            FLAlertLayer::create(
                "Import Macro",
                "Drop your <cg>.gdr2</c>, <cg>.gdr</c>, <cg>.xd</c> or "
                "<cg>.slc</c> files into the folder that just opened, then "
                "press <cy>Refresh</c>.",
                "OK")->show();
            return;
        }
        FLAlertLayer::create(this,
            "Import Macro",
            ("Could not open the folder from here. Copy your macros in with a "
             "file manager, then press <cy>Refresh</c>:\n\n<cl>"
             + macroDir().string() + "</c>"),
            "OK", "Copy Path")->show();
#else
        // Ask the system for the files rather than pointing at a folder. On
        // android the macros folder lives inside the game's private storage,
        // which a file manager cannot browse to, so handing someone the path
        // was advice they could not act on. The picker is the only way in.
        Ref<MacroPopup> self = this;

        // Built as locals rather than inline: nesting the designated
        // initialisers inside the call argument is its own compiler headache.
        file::FilePickOptions::Filter filter;
        filter.description = "Macros";
        filter.files = { "*.gdr2", "*.gdr", "*.xd", "*.slc" };

        file::FilePickOptions options;
        options.filters = { filter };

        geode::async::spawn(
            file::pickMany(options),
            [self](file::PickManyResult result) {
                if (!result) {
                    // Cancelling comes back as an empty list, not an error, so
                    // reaching here means it genuinely could not run.
                    log::warn("file picker failed: {}", result.unwrapErr());
                    FLAlertLayer::create(
                        "Import Macro",
                        ("Could not open the file picker.\n\n<cy>"
                         + result.unwrapErr() + "</c>").c_str(),
                        "OK")->show();
                    return;
                }

                const auto files = std::move(result).unwrap();
                if (files.empty()) return;      // cancelled, say nothing

                std::vector<std::string> added, rejected;
                for (auto const& src : files) {
                    const auto data = readFile(src);
                    if (data.empty() || !fmts::parseAny(data)) {
                        rejected.push_back(src.filename().string());
                        continue;
                    }
                    auto saved = adoptMacro(src);
                    if (saved.empty()) rejected.push_back(src.filename().string());
                    else               added.push_back(std::move(saved));
                }

                for (auto const& a : added)    log::info("imported {}", a);
                for (auto const& r : rejected) log::warn("not a macro, skipped: {}", r);

                // The popup may have been closed while the picker was up.
                if (self && self->getParent()) {
                    self->clearLabels();
                    self->refresh();
                }

                std::string msg;
                if (!added.empty()) {
                    msg = "Imported <cg>" + std::to_string(added.size())
                        + "</c> macro" + (added.size() == 1 ? "" : "s") + ".";
                    if (added.size() == 1) msg += "\n\n<cl>" + added[0] + "</c>";
                }
                if (!rejected.empty()) {
                    if (!msg.empty()) msg += "\n\n";
                    msg += "<cr>" + std::to_string(rejected.size()) + "</c> file"
                         + (rejected.size() == 1 ? " was" : "s were")
                         + " not a macro this mod can read and "
                         + (rejected.size() == 1 ? "was" : "were") + " skipped.";
                }
                FLAlertLayer::create("Import Macro", msg.c_str(), "OK")->show();
            });
#endif
    }

    void onSettings(CCObject*) {
#ifdef HAS_GEODE_UI
        geode::openSettingsPopup(Mod::get());
#else
        FLAlertLayer::create("Settings",
            "Open the Geode menu and find <cy>Click Indicators</c>.", "OK")->show();
#endif
    }

    void onClose(CCObject*) {
        clearLabels();
        this->removeFromParentAndCleanup(true);
    }
};

class $modify(IndicatorPause, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        CCNode* face = nullptr;
        for (auto nm : { "GJ_optionsBtn_001.png", "GJ_optionsBtn02_001.png",
                         "GJ_hammerBtn_001.png" }) {
            if (auto s = CCSprite::createWithSpriteFrameName(nm)) { face = s; break; }
        }
        if (face) face->setScale(0.75f);
        else      face = ButtonSprite::create("Guide");

        auto btn = CCMenuItemSpriteExtra::create(
            face, this, menu_selector(IndicatorPause::onGuide));

        auto menu = CCMenu::create();
        menu->setPosition({ 0.f, 0.f });
        menu->addChild(btn);
        btn->setPosition({ 34.f, 34.f });
        this->addChild(menu, 100);
    }

    void onGuide(CCObject*) {
        if (auto p = MacroPopup::create())
            CCDirector::get()->getRunningScene()->addChild(p, 9999);
    }
};

$on_mod(Loaded) {
    log::info("macros go in {}", macroDir().string());
#ifdef HAS_CHEAT_API
    log::info("cheat api linked, the indicator will work");
#else
    log::warn("cheat api header missing at build time, the cheat indicator "
              "will do nothing");
#endif
}
