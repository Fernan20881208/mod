#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>

#include "gdr2.hpp"
#include "formats.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace geode::prelude;

namespace featurepack {

constexpr double kMissWindow = 0.30;
constexpr int kUiZ = 1200;

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

static std::string levelKey(GJGameLevel* level) {
    if (!level) return "macro-none";
    const int id = level->m_levelID.value();
    if (id > 0) return "macro-" + std::to_string(id);
    std::string s = "macro-local-" + std::string(level->m_levelName);
    for (auto& c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (!std::isalnum(u) && c != '-') c = '_';
    }
    return s;
}

static std::string pickedMacro(GJGameLevel* level) {
    return Mod::get()->getSavedValue<std::string>(levelKey(level), "");
}

static void setPickedMacro(GJGameLevel* level, std::string const& file) {
    Mod::get()->setSavedValue<std::string>(levelKey(level), file);
}

static std::string squash(std::string const& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (std::isalnum(c)) out += char(std::tolower(c));
    }
    return out;
}

static bool filenameHasID(std::string const& stem, int id) {
    if (id <= 0) return false;
    const std::string want = std::to_string(id);
    for (size_t i = 0; (i = stem.find(want, i)) != std::string::npos; ++i) {
        const bool leftOk = i == 0 || !std::isdigit(static_cast<unsigned char>(stem[i - 1]));
        const size_t after = i + want.size();
        const bool rightOk = after >= stem.size()
                          || !std::isdigit(static_cast<unsigned char>(stem[after]));
        if (leftOk && rightOk) return true;
    }
    return false;
}

struct MacroInfo {
    std::string file;
    std::optional<gdr2::Replay> replay;
};

static MacroInfo loadBest(GJGameLevel* level) {
    MacroInfo out;
    if (!level) return out;

    const auto forced = pickedMacro(level);
    if (!forced.empty()) {
        const auto path = macroDir() / forced;
        auto rep = fmts::parseAny(readFile(path));
        if (rep) {
            out.file = forced;
            out.replay = std::move(rep);
            return out;
        }
    }

    const int wantID = level->m_levelID.value();
    const std::string wantName = level->m_levelName;
    const std::string wantSquash = squash(wantName);

    enum { kNone = 0, kFileName, kFileID, kRepName, kRepID };
    int bestRank = kNone;

    std::error_code ec;
    for (auto const& e : std::filesystem::directory_iterator(macroDir(), ec)) {
        if (!e.is_regular_file()) continue;
        if (!fmts::knownExtension(e.path().extension().string())) continue;

        auto rep = fmts::parseAny(readFile(e.path()));
        if (!rep) continue;

        const auto fname = e.path().filename().string();
        const auto stem = e.path().stem().string();

        int rank = kNone;
        if (wantID > 0 && int(rep->levelID) == wantID) rank = kRepID;
        else if (!rep->levelName.empty() && squash(rep->levelName) == wantSquash) rank = kRepName;
        else if (stem == std::to_string(wantID) || filenameHasID(stem, wantID)) rank = kFileID;
        else if (!wantSquash.empty() && squash(stem) == wantSquash) rank = kFileName;

        if (rank > bestRank) {
            bestRank = rank;
            out.file = fname;
            out.replay = std::move(rep);
        }
    }
    return out;
}

static std::vector<std::string> splitLines(std::string const& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty()) out.push_back(line);
    }
    return out;
}

static std::string joinLines(std::vector<std::string> const& v) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) out += '\n';
        out += v[i];
    }
    return out;
}

static std::vector<std::string> recentMacros() {
    return splitLines(Mod::get()->getSavedValue<std::string>("feature-recent-macros", ""));
}

static void touchRecent(std::string const& file) {
    if (file.empty()) return;
    auto items = recentMacros();
    items.erase(std::remove(items.begin(), items.end(), file), items.end());
    items.insert(items.begin(), file);
    if (items.size() > 12) items.resize(12);
    Mod::get()->setSavedValue<std::string>("feature-recent-macros", joinLines(items));
}

static uint64_t fnv1a(std::string const& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= uint64_t(c);
        h *= 1099511628211ull;
    }
    return h;
}

static std::string tagKey(std::string const& file) {
    return fmt::format("feature-macro-tag-{:x}", fnv1a(file));
}

static std::string macroTag(std::string const& file) {
    return Mod::get()->getSavedValue<std::string>(tagKey(file), "Unsorted");
}

static void setMacroTag(std::string const& file, std::string const& tag) {
    Mod::get()->setSavedValue<std::string>(tagKey(file), tag);
}

static std::string nextTag(std::string const& cur) {
    if (cur == "Unsorted") return "Practice";
    if (cur == "Practice") return "Favorite";
    if (cur == "Favorite") return "Archive";
    return "Unsorted";
}

struct LibraryEntry {
    std::string file;
    std::string tag;
    bool recent = false;
};

static std::vector<LibraryEntry> listLibrary() {
    std::vector<LibraryEntry> out;
    auto recent = recentMacros();

    std::error_code ec;
    for (auto const& e : std::filesystem::directory_iterator(macroDir(), ec)) {
        if (!e.is_regular_file()) continue;
        if (!fmts::knownExtension(e.path().extension().string())) continue;
        if (!fmts::parseAny(readFile(e.path()))) continue;
        LibraryEntry x;
        x.file = e.path().filename().string();
        x.tag = macroTag(x.file);
        x.recent = std::find(recent.begin(), recent.end(), x.file) != recent.end();
        out.push_back(std::move(x));
    }

    std::sort(out.begin(), out.end(), [&](LibraryEntry const& a, LibraryEntry const& b) {
        auto ia = std::find(recent.begin(), recent.end(), a.file);
        auto ib = std::find(recent.begin(), recent.end(), b.file);
        const bool ar = ia != recent.end();
        const bool br = ib != recent.end();
        if (ar != br) return ar;
        if (ar && br) return ia < ib;
        if (a.tag != b.tag) return a.tag < b.tag;
        return a.file < b.file;
    });
    return out;
}

static double tapSeconds(double fps) {
    const double ms = double(Mod::get()->getSettingValue<int64_t>("tap-ms"));
    return std::max(1.0 / std::max(1.0, fps), ms / 1000.0);
}

static float safeX() {
#ifdef GEODE_IS_IOS
    const auto win = CCDirector::get()->getWinSize();
    return std::clamp(win.width * 0.045f, 18.f, 30.f);
#else
    return 0.f;
#endif
}

static float safeTop() {
#ifdef GEODE_IS_IOS
    const auto win = CCDirector::get()->getWinSize();
    return std::clamp(win.height * 0.025f, 6.f, 14.f);
#else
    return 0.f;
#endif
}

static ccColor4F lerp(ccColor4F a, ccColor4F b, float t) {
    t = std::clamp(t, 0.f, 1.f);
    return {
        a.r + (b.r - a.r) * t,
        a.g + (b.g - a.g) * t,
        a.b + (b.b - a.b) * t,
        a.a + (b.a - a.a) * t
    };
}

} // namespace featurepack

class $modify(FeaturePackLayer, PlayLayer) {
    struct Fields {
        std::vector<gdr2::Hold> holds;
        std::vector<uint8_t> judged;
        double fps = 240.0;
        size_t missCursor = 0;
        std::string loadedFile;
        std::string pickedSeen;

        int perfect = 0;
        int ok = 0;
        int miss = 0;
        int perfectCombo = 0;
        int bestPerfectCombo = 0;
        double points = 0.0;
        int scored = 0;

        size_t activeHold = SIZE_MAX;
        bool activeHoldP2 = false;

        CCLabelBMFont* accuracyLabel = nullptr;
        CCLabelBMFont* comboLabel = nullptr;
        CCDrawNode* holdBar = nullptr;
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        auto f = m_fields.self();

        f->accuracyLabel = CCLabelBMFont::create("", "chatFont.fnt");
        f->accuracyLabel->setScale(0.46f);
        f->accuracyLabel->setOpacity(220);
        this->addChild(f->accuracyLabel, featurepack::kUiZ);

        f->comboLabel = CCLabelBMFont::create("", "chatFont.fnt");
        f->comboLabel->setScale(0.36f);
        f->comboLabel->setOpacity(180);
        this->addChild(f->comboLabel, featurepack::kUiZ);

        f->holdBar = CCDrawNode::create();
        f->holdBar->setBlendFunc({ GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA });
        this->addChild(f->holdBar, featurepack::kUiZ);

        reloadFeatureMacro();
        return true;
    }

    void resetFeatureScore() {
        auto f = m_fields.self();
        f->missCursor = 0;
        f->perfect = f->ok = f->miss = 0;
        f->perfectCombo = f->bestPerfectCombo = 0;
        f->points = 0.0;
        f->scored = 0;
        f->activeHold = SIZE_MAX;
        f->activeHoldP2 = false;
        f->judged.assign(f->holds.size(), 0);
        if (f->accuracyLabel) f->accuracyLabel->setString("");
        if (f->comboLabel) f->comboLabel->setString("");
        if (f->holdBar) f->holdBar->clear();
    }

    void reloadFeatureMacro() {
        auto f = m_fields.self();
        f->holds.clear();
        f->loadedFile.clear();

        auto info = featurepack::loadBest(m_level);
        if (!info.replay) {
            resetFeatureScore();
            return;
        }

        f->loadedFile = info.file;
        f->fps = info.replay->framerate > 0.0 ? info.replay->framerate : 240.0;

        const int offsetFrames = int(Mod::get()->getSavedValue<int64_t>(
            "offset3-" + featurepack::levelKey(m_level), 0));

        for (auto h : info.replay->holds()) {
            if (h.button != 1) continue;
            const int64_t s = int64_t(h.start) + offsetFrames;
            const int64_t e = int64_t(h.end) + offsetFrames;
            if (e < 0) continue;
            h.start = uint64_t(std::max<int64_t>(s, 0));
            h.end = uint64_t(std::max<int64_t>(e, 0));
            f->holds.push_back(h);
        }

        std::sort(f->holds.begin(), f->holds.end(),
                  [](gdr2::Hold const& a, gdr2::Hold const& b) {
                      return a.start < b.start;
                  });
        resetFeatureScore();
        featurepack::touchRecent(f->loadedFile);
        f->pickedSeen = featurepack::pickedMacro(m_level);
        log::info("feature pack loaded {} ({} holds)", f->loadedFile, f->holds.size());
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        resetFeatureScore();
    }

    double featureNow() {
        const double settingOffset =
            double(Mod::get()->getSettingValue<int64_t>("timing-offset")) / 1000.0;
        return this->m_timePlayed - settingOffset;
    }

    void addPerfect() {
        auto f = m_fields.self();
        ++f->perfect;
        ++f->perfectCombo;
        f->bestPerfectCombo = std::max(f->bestPerfectCombo, f->perfectCombo);
        f->points += 1.0;
        ++f->scored;
    }

    void addOk(double off, double okSec) {
        auto f = m_fields.self();
        ++f->ok;
        f->perfectCombo = 0;
        const double q = okSec > 1e-6 ? std::clamp(1.0 - off / okSec, 0.25, 0.75) : 0.5;
        f->points += q;
        ++f->scored;
    }

    void addMiss() {
        auto f = m_fields.self();
        ++f->miss;
        f->perfectCombo = 0;
        ++f->scored;
    }

    void onFeatureInput(bool down, bool isP2) {
        auto f = m_fields.self();
        if (!Mod::get()->getSettingValue<bool>("enabled") || f->holds.empty()) return;

        if (!down) {
            if (f->activeHold != SIZE_MAX && f->activeHoldP2 == isP2) {
                f->activeHold = SIZE_MAX;
            }
            return;
        }

        const double now = featureNow();
        const double perfectSec =
            double(Mod::get()->getSettingValue<int64_t>("perfect-ms")) / 1000.0;
        const double okSec =
            double(Mod::get()->getSettingValue<int64_t>("ok-ms")) / 1000.0;

        size_t best = SIZE_MAX;
        double bestDelta = 1e9;

        for (size_t i = 0; i < f->holds.size(); ++i) {
            if (f->judged[i]) continue;
            auto const& h = f->holds[i];
            if (h.player2 != isP2) continue;
            if (h.player2 && !this->m_gameState.m_isDualMode) continue;

            const double t0 = double(h.start) / f->fps;
            const double d = now - t0;
            if (d < -featurepack::kMissWindow) break;
            if (std::abs(d) <= featurepack::kMissWindow
                && std::abs(d) < std::abs(bestDelta)) {
                best = i;
                bestDelta = d;
            }
        }

        if (best == SIZE_MAX) {
            addMiss();
            return;
        }

        f->judged[best] = 1;
        const double off = std::abs(bestDelta);

        if (off <= perfectSec) addPerfect();
        else if (off <= okSec) addOk(off, okSec);
        else addMiss();

        const double len = double(f->holds[best].end - f->holds[best].start) / f->fps;
        if (len > featurepack::tapSeconds(f->fps)) {
            f->activeHold = best;
            f->activeHoldP2 = isP2;
        }
    }

    void scoreMissedFeature(double now) {
        auto f = m_fields.self();
        while (f->missCursor < f->holds.size()) {
            auto const& h = f->holds[f->missCursor];
            const double t0 = double(h.start) / f->fps;
            if (t0 > now - featurepack::kMissWindow) break;

            if (!f->judged[f->missCursor]) {
                if (!(h.player2 && !this->m_gameState.m_isDualMode)) {
                    f->judged[f->missCursor] = 1;
                    addMiss();
                } else {
                    f->judged[f->missCursor] = 1;
                }
            }
            ++f->missCursor;
        }
    }

    void updateFeatureLabels() {
        auto f = m_fields.self();
        if (!f->accuracyLabel || !f->comboLabel) return;

        const bool show = Mod::get()->getSettingValue<bool>("enabled")
                       && Mod::get()->getSettingValue<bool>("accuracy")
                       && !f->holds.empty();
        f->accuracyLabel->setVisible(show);
        f->comboLabel->setVisible(show);
        if (!show) return;

        const double accuracy = f->scored > 0
            ? std::clamp(f->points / double(f->scored) * 100.0, 0.0, 100.0)
            : 100.0;

        const auto win = CCDirector::get()->getWinSize();
        const float y = win.height - 18.f - featurepack::safeTop();

        f->accuracyLabel->setPosition({ win.width * 0.5f, y });
        f->comboLabel->setPosition({ win.width * 0.5f, y - 15.f });

        f->accuracyLabel->setString(fmt::format("ACC {:.1f}%", accuracy).c_str());
        f->comboLabel->setString(
            fmt::format("PERFECT x{}  BEST x{}", f->perfectCombo, f->bestPerfectCombo).c_str());
    }

    void drawHoldGradient(double now) {
        auto f = m_fields.self();
        if (!f->holdBar) return;
        f->holdBar->clear();

        if (f->activeHold == SIZE_MAX || f->activeHold >= f->holds.size()) return;
        auto const& h = f->holds[f->activeHold];

        const double t0 = double(h.start) / f->fps;
        const double t1 = double(h.end) / f->fps;
        if (t1 <= t0 || now >= t1) {
            f->activeHold = SIZE_MAX;
            return;
        }

        const double rem = std::clamp((t1 - now) / (t1 - t0), 0.0, 1.0);
        const auto win = CCDirector::get()->getWinSize();

        const float width = std::min(win.width * 0.28f, 170.f);
        const float height = 5.f;
        const float x0 = win.width * 0.5f - width * 0.5f;
        const float y0 = win.height - 50.f - featurepack::safeTop();

        ccColor4F bg = { 0.f, 0.f, 0.f, 0.42f };
        CCPoint bgPts[4] = {
            { x0, y0 }, { x0 + width, y0 },
            { x0 + width, y0 + height }, { x0, y0 + height }
        };
        f->holdBar->drawPolygon(bgPts, 4, bg, 0.f, bg);

        const ccColor4F low = { 1.f, 0.25f, 0.20f, 0.92f };
        const ccColor4F mid = { 1.f, 0.82f, 0.20f, 0.92f };
        const ccColor4F high = h.player2
            ? ccColor4F{ 1.f, 0.35f, 0.80f, 0.92f }
            : ccColor4F{ 0.20f, 0.95f, 0.45f, 0.92f };

        constexpr int segs = 24;
        const int visible = std::max(1, int(std::ceil(rem * segs)));
        for (int i = 0; i < visible; ++i) {
            const float u0 = float(i) / segs;
            const float u1 = float(i + 1) / segs;
            const float center = (u0 + u1) * 0.5f;
            ccColor4F c = center < 0.5f
                ? featurepack::lerp(low, mid, center * 2.f)
                : featurepack::lerp(mid, high, (center - 0.5f) * 2.f);
            const float sx0 = x0 + width * u0;
            const float sx1 = x0 + width * std::min<float>(u1, float(rem));
            if (sx1 <= sx0) continue;
            CCPoint pts[4] = {
                { sx0, y0 }, { sx1, y0 }, { sx1, y0 + height }, { sx0, y0 + height }
            };
            f->holdBar->drawPolygon(pts, 4, c, 0.f, c);
        }
    }

    void applyIOSSafeArea() {
#ifdef GEODE_IS_IOS
        const auto win = CCDirector::get()->getWinSize();
        const float inset = featurepack::safeX();
        const auto side = Mod::get()->getSettingValue<std::string>("lane-side");

        if (auto children = this->getChildren()) {
            CCObject* obj = nullptr;
            CCARRAY_FOREACH(children, obj) {
                auto node = static_cast<CCNode*>(obj);
                if (!node) continue;

                if (node->getZOrder() == 1000 && side != "Middle") {
                    node->setPositionX(side == "Right" ? -inset : inset);
                } else if (node->getZOrder() == 1001) {
                    const float x = std::clamp(node->getPositionX(),
                                               inset + 8.f, win.width - inset - 8.f);
                    node->setPositionX(x);
                }
            }
        }
#endif
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        auto f = m_fields.self();

        const auto pick = featurepack::pickedMacro(m_level);
        if (pick != f->pickedSeen) {
            f->pickedSeen = pick;
            reloadFeatureMacro();
        }

        const double now = featureNow();
        scoreMissedFeature(now);
        updateFeatureLabels();
        drawHoldGradient(now);
        applyIOSSafeArea();
    }
};

class $modify(FeaturePackInput, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
        if (button != 1) return;
        auto pl = PlayLayer::get();
        if (!pl || pl != static_cast<GJBaseGameLayer*>(this)) return;
        static_cast<FeaturePackLayer*>(pl)->onFeatureInput(down, !isPlayer1);
    }
};

class FeatureLibraryPopup : public CCLayerColor, public FLAlertLayerProtocol {
    CCMenu* m_rows = nullptr;
    CCLabelBMFont* m_info = nullptr;
    CCLabelBMFont* m_filter = nullptr;
    std::vector<CCLabelBMFont*> m_labels;
    int m_page = 0;
    int m_filterMode = 0;

    static constexpr int kPerPage = 5;
    static constexpr float kW = 410.f;
    static constexpr float kH = 290.f;

    static CCMenuItemSpriteExtra* mkBtn(char const* text, float maxW,
                                        CCObject* target, SEL_MenuHandler selector) {
        auto spr = ButtonSprite::create(text);
        if (spr && spr->getContentSize().width > maxW)
            spr->setScale(maxW / spr->getContentSize().width);
        return CCMenuItemSpriteExtra::create(spr, target, selector);
    }

    std::string filterName() const {
        switch (m_filterMode) {
            case 1: return "Recent";
            case 2: return "Practice";
            case 3: return "Favorite";
            case 4: return "Archive";
            case 5: return "Unsorted";
            default: return "All";
        }
    }

    std::vector<featurepack::LibraryEntry> filtered() const {
        auto all = featurepack::listLibrary();
        if (m_filterMode == 0) return all;

        std::vector<featurepack::LibraryEntry> out;
        const auto want = filterName();
        for (auto const& e : all) {
            if ((m_filterMode == 1 && e.recent)
                || (m_filterMode >= 2 && e.tag == want)) {
                out.push_back(e);
            }
        }
        return out;
    }

public:
    static FeatureLibraryPopup* create() {
        auto ret = new FeatureLibraryPopup();
        if (ret && ret->build()) {
            ret->autorelease();
            return ret;
        }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }

    bool build() {
        if (!this->initWithColor({ 0, 0, 0, 155 })) return false;
        const auto win = CCDirector::get()->getWinSize();
        const float bx = win.width * 0.5f - kW * 0.5f;
        const float by = win.height * 0.5f - kH * 0.5f;

        auto panel = cocos2d::extension::CCScale9Sprite::create("GJ_square01.png");
        if (!panel) return false;
        panel->setContentSize({ kW, kH });
        panel->setPosition({ win.width * 0.5f, win.height * 0.5f });
        this->addChild(panel);

        auto title = CCLabelBMFont::create("Macro Library", "goldFont.fnt");
        title->setPosition({ win.width * 0.5f, by + kH - 22.f });
        title->setScale(0.75f);
        this->addChild(title);

        auto menu = CCMenu::create();
        menu->setPosition({ 0.f, 0.f });
        menu->setTouchPriority(-620);
        this->addChild(menu);

        if (auto x = CCSprite::createWithSpriteFrameName("GJ_closeBtn_001.png")) {
            x->setScale(0.7f);
            auto close = CCMenuItemSpriteExtra::create(
                x, this, menu_selector(FeatureLibraryPopup::onClose));
            close->setPosition({ bx + 16.f, by + kH - 16.f });
            menu->addChild(close);
        }

        auto filterBtn = mkBtn("Filter", 58.f, this,
                               menu_selector(FeatureLibraryPopup::onFilter));
        auto refreshBtn = mkBtn("Refresh", 62.f, this,
                                menu_selector(FeatureLibraryPopup::onRefresh));
        filterBtn->setPosition({ bx + kW * 0.35f, by + kH - 54.f });
        refreshBtn->setPosition({ bx + kW * 0.68f, by + kH - 54.f });
        menu->addChild(filterBtn);
        menu->addChild(refreshBtn);

        auto prev = mkBtn("<", 26.f, this, menu_selector(FeatureLibraryPopup::onPrev));
        auto next = mkBtn(">", 26.f, this, menu_selector(FeatureLibraryPopup::onNext));
        prev->setPosition({ bx + 26.f, by + 34.f });
        next->setPosition({ bx + kW - 26.f, by + 34.f });
        menu->addChild(prev);
        menu->addChild(next);

        m_filter = CCLabelBMFont::create("", "chatFont.fnt");
        m_filter->setScale(0.48f);
        m_filter->setPosition({ win.width * 0.5f, by + kH - 75.f });
        this->addChild(m_filter);

        m_rows = CCMenu::create();
        m_rows->setPosition({ 0.f, 0.f });
        m_rows->setTouchPriority(-630);
        this->addChild(m_rows);

        m_info = CCLabelBMFont::create("", "chatFont.fnt");
        m_info->setScale(0.43f);
        m_info->setPosition({ win.width * 0.5f, by + 15.f });
        this->addChild(m_info);

        this->setTouchEnabled(true);
        this->setKeypadEnabled(true);
        refresh();
        return true;
    }

    void keyBackClicked() { onClose(nullptr); }

    GJGameLevel* level() const {
        auto pl = PlayLayer::get();
        return pl ? pl->m_level : nullptr;
    }

    void clearLabels() {
        for (auto l : m_labels) if (l) l->removeFromParent();
        m_labels.clear();
    }

    void refresh() {
        clearLabels();
        m_rows->removeAllChildren();

        auto entries = filtered();
        const int pages = std::max(1, int((entries.size() + kPerPage - 1) / kPerPage));
        m_page = std::clamp(m_page, 0, pages - 1);

        const auto win = CCDirector::get()->getWinSize();
        const float bx = win.width * 0.5f - kW * 0.5f;
        const float by = win.height * 0.5f - kH * 0.5f;
        const float top = by + kH - 104.f;

        if (m_filter) {
            m_filter->setString(fmt::format("Folder: {}", filterName()).c_str());
        }

        for (int row = 0; row < kPerPage; ++row) {
            const size_t idx = size_t(m_page * kPerPage + row);
            if (idx >= entries.size()) break;
            auto const& e = entries[idx];
            const float y = top - 29.f * float(row);

            auto name = CCLabelBMFont::create(
                fmt::format("[{}] {}", e.tag, e.file).c_str(), "chatFont.fnt");
            name->setAnchorPoint({ 0.f, 0.5f });
            name->setScale(0.45f);
            if (name->getContentSize().width * 0.45f > kW - 115.f)
                name->setScale((kW - 115.f) / name->getContentSize().width);
            name->setPosition({ bx + 14.f, y });
            name->setColor(e.recent ? ccColor3B{ 130, 245, 255 }
                                    : ccColor3B{ 235, 235, 235 });
            this->addChild(name);
            m_labels.push_back(name);

            auto tag = mkBtn("Tag", 42.f, this, menu_selector(FeatureLibraryPopup::onTag));
            tag->setPosition({ bx + kW - 35.f, y });
            tag->setTag(int(idx));
            m_rows->addChild(tag);
        }

        if (entries.empty()) {
            m_info->setString("No macros in this folder.");
        } else {
            m_info->setString(fmt::format("{} macros - page {}/{} - cyan = recent",
                                          entries.size(), m_page + 1, pages).c_str());
        }
    }

    void onFilter(CCObject*) {
        m_filterMode = (m_filterMode + 1) % 6;
        m_page = 0;
        refresh();
    }

    void onRefresh(CCObject*) { refresh(); }
    void onPrev(CCObject*) { --m_page; refresh(); }
    void onNext(CCObject*) { ++m_page; refresh(); }

    void onTag(CCObject* sender) {
        auto entries = filtered();
        const int idx = sender->getTag();
        if (idx < 0 || idx >= int(entries.size())) return;
        auto const& e = entries[size_t(idx)];
        featurepack::setMacroTag(e.file, featurepack::nextTag(e.tag));
        refresh();
    }

    void onClose(CCObject*) {
        clearLabels();
        this->removeFromParentAndCleanup(true);
    }
};

class $modify(FeaturePackPause, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

#ifdef GEODE_IS_IOS
        if (auto children = this->getChildren()) {
            CCObject* obj = nullptr;
            CCARRAY_FOREACH(children, obj) {
                auto node = static_cast<CCNode*>(obj);
                if (node && node->getZOrder() == 100) {
                    node->setPositionX(featurepack::safeX());
                }
            }
        }
#endif

        auto face = ButtonSprite::create("Library");
        face->setScale(0.65f);
        auto btn = CCMenuItemSpriteExtra::create(
            face, this, menu_selector(FeaturePackPause::onLibrary));

        auto menu = CCMenu::create();
        menu->setPosition({ featurepack::safeX(), 0.f });
        menu->addChild(btn);
        btn->setPosition({ 92.f, 34.f });
        this->addChild(menu, 101);
    }

    void onLibrary(CCObject*) {
        if (auto popup = FeatureLibraryPopup::create()) {
            CCDirector::get()->getRunningScene()->addChild(popup, 10000);
        }
    }
};

$on_mod(Loaded) {
    log::info("feature pack: accuracy score, perfect combo, hold gradient, "
              "iOS safe area, recent macros and virtual folders/tags enabled");
}
