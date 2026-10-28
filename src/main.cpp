#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/ui/Popup.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <sstream>
#include <functional>
#include <map>

using namespace geode::prelude;

static const char* FONT = "bigFont.fnt";
static const char* FONT2 = "chatFont.fnt";
constexpr float BAR_W = 240.f;
static const char* TIERS[7] = {"Unrated", "Easy", "Normal", "Hard", "Harder", "Insane", "Demon"};
static const char* MODES[8] = {"Cube", "Ship", "Ball", "UFO", "Wave", "Robot", "Spider", "Swing"};

static std::string levelKey(GJGameLevel* lvl) {
    if (lvl->m_levelID.value() != 0) return fmt::format("lvl{}", lvl->m_levelID.value());
    return fmt::format("loc{}", std::hash<std::string>{}(std::string(lvl->m_levelName)));
}

template <size_t N>
static std::array<int, N> loadArr(const std::string& key) {
    std::array<int, N> a{};
    auto s = Mod::get()->getSavedValue<std::string>(key, "");
    std::istringstream ss(s);
    for (auto& v : a) {
        if (!(ss >> v)) break;
    }
    return a;
}

template <size_t N>
static void saveArr(const std::string& key, const std::array<int, N>& a) {
    std::string s;
    for (int v : a) s += std::to_string(v) + " ";
    Mod::get()->setSavedValue(key, s);
}

static int modeIndex(PlayerObject* p) {
    if (p->m_isShip) return 1;
    if (p->m_isBall) return 2;
    if (p->m_isBird) return 3;
    if (p->m_isDart) return 4;
    if (p->m_isRobot) return 5;
    if (p->m_isSpider) return 6;
    if (p->m_isSwing) return 7;
    return 0;
}

static std::string tierName(GJGameLevel* l) {
    int d = static_cast<int>(l->m_difficulty);
    if (d <= 0) return "Unrated";
    if (d >= 6) return "Demon";
    return TIERS[d];
}

static void registerLevel(GJGameLevel* lvl, const std::string& key) {
    auto idx = Mod::get()->getSavedValue<std::string>("index", ",");
    if (idx.find("," + key + ",") == std::string::npos) {
        idx += key + ",";
        Mod::get()->setSavedValue("index", idx);
    }
    Mod::get()->setSavedValue(key + "_t", tierName(lvl));
}

static ccColor4F heatColor(int n, int mx) {
    if (n <= 0 || mx <= 0) return {0.12f, 0.16f, 0.2f, 0.55f};
    float t = (float)n / (float)mx;
    if (t < 0.5f) {
        float k = t / 0.5f;
        return {0.1f + 0.9f * k, 0.9f, 0.2f, 0.95f};
    }
    float k = (t - 0.5f) / 0.5f;
    return {1.f, 0.9f - 0.75f * k, 0.15f, 0.95f};
}

static ccColor4F chanceColor(float p) {
    if (p >= 0.75f) return {0.2f, 1.f, 0.4f, 1.f};
    if (p >= 0.5f) return {1.f, 0.9f, 0.2f, 1.f};
    if (p >= 0.3f) return {1.f, 0.55f, 0.1f, 1.f};
    return {1.f, 0.2f, 0.2f, 1.f};
}

static ccColor3B col3(ccColor4F c) {
    return {(GLubyte)(c.r * 255), (GLubyte)(c.g * 255), (GLubyte)(c.b * 255)};
}

static void fillRect(CCDrawNode* n, float x0, float y0, float x1, float y1,
                     ccColor4F fill, float bw, ccColor4F bc) {
    CCPoint v[4] = {CCPoint(x0, y0), CCPoint(x1, y0), CCPoint(x1, y1), CCPoint(x0, y1)};
    n->drawPolygon(v, 4, fill, bw, bc);
}

struct Stuck { int lo, hi, deaths; float pass, tries; bool lowData; };

static std::vector<Stuck> computeStuck(const std::array<int, 101>& d, int wins) {
    std::array<float, 101> reach{};
    float run = (float)wins;
    for (int p = 99; p >= 0; p--) { run += d[p]; reach[p] = run; }
    auto surv = [&](int p) {
        if (p < 0 || p > 99 || reach[p] < 3.f) return 1.f;
        return 1.f - d[p] / reach[p];
    };
    std::vector<int> idx(100);
    std::iota(idx.begin(), idx.end(), 0);
    std::sort(idx.begin(), idx.end(), [&](int a, int b) { return d[a] > d[b]; });
    std::vector<Stuck> out;
    std::vector<int> picked;
    for (int p : idx) {
        if (d[p] <= 0 || out.size() >= 4) break;
        bool tooClose = false;
        for (int q : picked) if (std::abs(q - p) <= 2) tooClose = true;
        if (tooClose) continue;
        picked.push_back(p);
        float pass = 1.f;
        int zone = 0;
        for (int q = p - 2; q <= p + 2; q++) {
            pass *= surv(q);
            if (q >= 0 && q <= 99) zone += d[q];
        }
        pass = std::clamp(pass, 0.01f, 1.f);
        out.push_back({std::max(0, p - 2), std::min(99, p + 2), zone, pass, std::min(99.f, 1.f / pass), zone < 3});
    }
    return out;
}

class DeathLab : public Popup {
protected:
    GJGameLevel* m_dlLvl = nullptr;
    std::array<int, 101> m_dlD{};
    int m_dlWins = 0;
    std::array<int, 8> m_dlModes{};

public:
    static DeathLab* create(GJGameLevel* lvl, const std::array<int, 101>& d, int wins,
                            const std::array<int, 8>& modes, bool fast = false) {
        auto ret = new DeathLab();
        if (ret->init(lvl, d, wins, modes, fast)) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    void onSkip(CCObject*) {
        auto lvl = m_dlLvl;
        auto d = m_dlD;
        int wins = m_dlWins;
        auto modes = m_dlModes;
        this->onClose(nullptr);
        if (auto p = DeathLab::create(lvl, d, wins, modes, true)) p->show();
    }

protected:
    bool init(GJGameLevel* lvl, const std::array<int, 101>& d, int wins,
              const std::array<int, 8>& modes, bool fast) {
        if (!Popup::init(440.f, 270.f)) return false;
        m_dlLvl = lvl;
        m_dlD = d;
        m_dlWins = wins;
        m_dlModes = modes;
        if (m_bgSprite) m_bgSprite->setVisible(false);
        auto sz = m_mainLayer->getContentSize();
        float w = sz.width > 0.f ? sz.width : 440.f;
        float h = sz.height > 0.f ? sz.height : 270.f;
        const ccColor3B CYAN = {120, 255, 255};
        const ccColor3B GREEN = {90, 255, 140};
        const ccColor3B SOFT = {200, 220, 230};
        const float BOOT = fast ? 0.f : 10.f;
        const float BUILD = fast ? 0.8f : 5.f;
        auto at = [&](float f) { return BOOT + f * BUILD; };
        auto soft = [](ccColor4F c, float a) { c.a = a; return c; };

        // background panel, visible from the start
        auto panel = CCDrawNode::create();
        fillRect(panel, 0.f, 0.f, w, h, {0.02f, 0.05f, 0.1f, 0.94f}, 2.f, {0.2f, 0.9f, 1.f, 0.8f});
        for (float y = 10.f; y < h; y += 20.f)
            panel->drawSegment({10.f, y}, {w - 10.f, y}, 0.5f, {0.2f, 0.8f, 1.f, 0.035f});
        ccColor4F neon = {0.4f, 1.f, 1.f, 0.7f};
        panel->drawSegment({4.f, 4.f}, {24.f, 4.f}, 2.f, neon);
        panel->drawSegment({4.f, 4.f}, {4.f, 24.f}, 2.f, neon);
        panel->drawSegment({w - 4.f, 4.f}, {w - 24.f, 4.f}, 2.f, neon);
        panel->drawSegment({w - 4.f, 4.f}, {w - 4.f, 24.f}, 2.f, neon);
        panel->drawSegment({4.f, h - 4.f}, {24.f, h - 4.f}, 2.f, neon);
        panel->drawSegment({4.f, h - 4.f}, {4.f, h - 24.f}, 2.f, neon);
        panel->drawSegment({w - 4.f, h - 4.f}, {w - 24.f, h - 4.f}, 2.f, neon);
        panel->drawSegment({w - 4.f, h - 4.f}, {w - 4.f, h - 24.f}, 2.f, neon);
        m_mainLayer->addChild(panel, -1);

        // decorations that appear when the boot ends
        auto deco = CCDrawNode::create();
        fillRect(deco, 2.f, h - 24.f, w - 2.f, h - 2.f, {0.06f, 0.14f, 0.2f, 0.85f}, 0.f, {0.f, 0.f, 0.f, 0.f});
        deco->drawDot({14.f, h - 13.f}, 3.f, {1.f, 0.35f, 0.35f, 0.8f});
        deco->drawDot({26.f, h - 13.f}, 3.f, {1.f, 0.85f, 0.3f, 0.8f});
        deco->drawDot({38.f, h - 13.f}, 3.f, {0.35f, 1.f, 0.5f, 0.8f});
        deco->drawSegment({10.f, 96.f}, {w - 10.f, 96.f}, 1.f, {0.3f, 0.9f, 1.f, 0.25f});
        deco->drawSegment({255.f, 100.f}, {255.f, h - 30.f}, 1.f, {0.3f, 0.9f, 1.f, 0.18f});
        deco->setVisible(false);
        deco->runAction(CCSequence::create(CCDelayTime::create(BOOT), CCShow::create(), nullptr));
        m_mainLayer->addChild(deco, 0);

        auto label = [&](const std::string& t, const char* font, float sc, ccColor3B col,
                         CCPoint pos, float ax, float delay) -> CCLabelBMFont* {
            auto l = CCLabelBMFont::create(t.c_str(), font);
            l->setScale(sc);
            l->setColor(col);
            l->setAnchorPoint({ax, 0.5f});
            l->setPosition(pos);
            l->setOpacity(0);
            l->runAction(CCSequence::create(CCDelayTime::create(delay), CCFadeIn::create(0.25f), nullptr));
            m_mainLayer->addChild(l, 5);
            return l;
        };
        auto hbar = [&](float x, float y, float len, float bh, ccColor4F c, float delay) {
            auto n = CCDrawNode::create();
            fillRect(n, 0.f, 0.f, std::max(len, 1.f), bh, c, 0.f, {0.f, 0.f, 0.f, 0.f});
            n->setPosition({x, y});
            n->setScaleX(0.f);
            n->runAction(CCSequence::create(CCDelayTime::create(delay),
                CCEaseOut::create(CCScaleTo::create(0.5f, 1.f, 1.f), 2.f), nullptr));
            m_mainLayer->addChild(n, 4);
        };
        auto vbar = [&](float x, float y, float bw, float len, ccColor4F c, float delay) {
            auto n = CCDrawNode::create();
            fillRect(n, 0.f, 0.f, bw, std::max(len, 2.f), c, 0.f, {0.f, 0.f, 0.f, 0.f});
            n->setPosition({x, y});
            n->setScaleY(0.f);
            n->runAction(CCSequence::create(CCDelayTime::create(delay),
                CCEaseOut::create(CCScaleTo::create(0.5f, 1.f, 1.f), 2.f), nullptr));
            m_mainLayer->addChild(n, 4);
        };

        // boot sequence
        if (!fast) {
            auto bootLayer = CCNode::create();
            m_mainLayer->addChild(bootLayer, 20);
            static const char* LINES[16] = {
                "DEATHLAB BIOS v0.4  (c) cybernode",
                "checking memory ............. OK",
                "mounting /saves/death.db .... OK",
                "reading level index ......... OK",
                "loading stat engine ......... OK",
                "calibrating hazard model .... OK",
                "decrypting attempt logs ..... OK",
                "linking gamemode sensors .... OK",
                "scanning difficulty tiers ... OK",
                "building survival curves .... OK",
                "estimating pass chances ..... OK",
                "spinning up neon core ....... OK",
                "syncing scanline emitter .... OK",
                "compiling heat map .......... OK",
                "all systems nominal",
                "launching analyzer_"};
            for (int i = 0; i < 16; i++) {
                auto l = CCLabelBMFont::create(LINES[i], FONT2);
                l->setScale(0.75f);
                l->setColor(i >= 14 ? ccColor3B{255, 255, 255} : ccColor3B{90, 255, 140});
                l->setAnchorPoint({0.f, 0.5f});
                l->setPosition({18.f, h - 18.f - 12.f * (float)i});
                l->setOpacity(0);
                l->runAction(CCSequence::create(CCDelayTime::create(0.3f + 0.58f * (float)i),
                    CCFadeIn::create(0.1f), nullptr));
                bootLayer->addChild(l);
            }
            auto back = CCDrawNode::create();
            fillRect(back, 18.f, 12.f, w - 18.f, 18.f, {1.f, 1.f, 1.f, 0.1f}, 1.f, {0.4f, 1.f, 1.f, 0.5f});
            bootLayer->addChild(back);
            auto fill = CCDrawNode::create();
            fillRect(fill, 0.f, 0.f, w - 36.f, 6.f, {0.4f, 1.f, 1.f, 0.7f}, 0.f, {0.f, 0.f, 0.f, 0.f});
            fill->setPosition({18.f, 12.f});
            fill->setScaleX(0.f);
            fill->runAction(CCScaleTo::create(BOOT, 1.f, 1.f));
            bootLayer->addChild(fill);
            bootLayer->runAction(CCSequence::create(CCDelayTime::create(BOOT), CCRemoveSelf::create(), nullptr));

            auto skipLbl = CCLabelBMFont::create("SKIP >>", FONT);
            skipLbl->setScale(0.32f);
            skipLbl->setColor({255, 255, 255});
            auto skipItem = CCMenuItemSpriteExtra::create(skipLbl, this, menu_selector(DeathLab::onSkip));
            skipItem->setPosition({w - 44.f, 30.f});
            skipItem->runAction(CCSequence::create(CCDelayTime::create(BOOT), CCRemoveSelf::create(), nullptr));
            m_buttonMenu->addChild(skipItem);
        }

        // scan line
        auto scan = CCDrawNode::create();
        scan->drawSegment({0.f, 0.f}, {w, 0.f}, 1.5f, {0.4f, 1.f, 1.f, 0.7f});
        scan->runAction(CCSequence::create(CCDelayTime::create(BOOT),
            CCMoveTo::create(std::max(BUILD, 0.3f), {0.f, h}), CCRemoveSelf::create(), nullptr));
        m_mainLayer->addChild(scan, 50);

        // title bar
        std::string name = std::string(lvl->m_levelName);
        if (name.size() > 18) name = name.substr(0, 16) + "..";
        auto tier = tierName(lvl);
        auto tl = label("death-lab@console:~$ analyze --run", FONT2, 0.8f, GREEN, {50.f, h - 13.f}, 0.f, at(0.f));
        auto cursor = label("_", FONT2, 0.8f, GREEN, {50.f + tl->getContentSize().width * 0.8f + 4.f, h - 13.f}, 0.f, at(0.f));
        cursor->runAction(CCRepeatForever::create(CCSequence::create(
            CCDelayTime::create(0.5f), CCFadeOut::create(0.01f), CCDelayTime::create(0.5f), CCFadeIn::create(0.01f), nullptr)));
        label(fmt::format("{} [{}]", name, tier), FONT2, 0.8f, SOFT, {w - 14.f, h - 13.f}, 1.f, at(0.f));

        // stuck points
        label("$ analyze --stuck", FONT2, 0.9f, GREEN, {16.f, h - 40.f}, 0.f, at(0.1f));
        auto stuck = computeStuck(d, wins);
        if (stuck.empty()) label("no deaths recorded yet", FONT2, 0.9f, SOFT, {16.f, h - 64.f}, 0.f, at(0.2f));
        for (size_t i = 0; i < stuck.size(); i++) {
            auto& st = stuck[i];
            float y = h - 64.f - 28.f * (float)i;
            float dl = at(0.2f + 0.1f * (float)i);
            auto c = st.lowData ? ccColor4F{0.6f, 0.7f, 0.8f, 1.f} : chanceColor(st.pass);
            fillRect(deco, 8.f, y - 17.f, 248.f, y + 9.f, {1.f, 1.f, 1.f, 0.04f}, 0.f, {0.f, 0.f, 0.f, 0.f});
            label(fmt::format("[#{}] {}-{}%", i + 1, st.lo, st.hi), FONT, 0.36f, col3(c), {14.f, y}, 0.f, dl);
            std::string info = st.lowData
                ? fmt::format("deaths={}  pass=?? (low data)", st.deaths)
                : fmt::format("deaths={}  pass~{}%  tries~{:.1f}", st.deaths, (int)std::lround(st.pass * 100.f), st.tries);
            label(info, FONT2, 0.8f, SOFT, {14.f, y - 12.f}, 0.f, dl);
            hbar(150.f, y - 3.f, 90.f, 6.f, {1.f, 1.f, 1.f, 0.07f}, dl);
            hbar(150.f, y - 3.f, 90.f * (st.lowData ? 0.f : st.pass), 6.f, soft(c, 0.7f), dl);
        }

        // gamemodes
        label("$ analyze --modes", FONT2, 0.9f, GREEN, {270.f, h - 40.f}, 0.f, at(0.1f));
        std::vector<int> mi(8);
        std::iota(mi.begin(), mi.end(), 0);
        std::sort(mi.begin(), mi.end(), [&](int a, int b) { return modes[a] > modes[b]; });
        int mmax = std::max(1, modes[mi[0]]);
        int shown = 0;
        for (int m : mi) {
            if (modes[m] <= 0 || shown >= 4) break;
            float y = h - 64.f - 28.f * (float)shown;
            float dl = at(0.2f + 0.1f * (float)shown);
            auto c = heatColor(modes[m], mmax);
            fillRect(deco, 262.f, y - 17.f, w - 8.f, y + 9.f, {1.f, 1.f, 1.f, 0.04f}, 0.f, {0.f, 0.f, 0.f, 0.f});
            label(MODES[m], FONT, 0.36f, col3(c), {270.f, y}, 0.f, dl);
            hbar(335.f, y - 3.f, 60.f * modes[m] / (float)mmax, 6.f, soft(c, 0.7f), dl);
            label("x" + std::to_string(modes[m]), FONT2, 0.9f, SOFT, {w - 14.f, y}, 1.f, dl);
            shown++;
        }
        if (!shown) label("no mode data yet", FONT2, 0.9f, SOFT, {270.f, h - 64.f}, 0.f, at(0.2f));

        // difficulty histogram across all levels
        std::map<std::string, int> tierDeaths;
        auto index = Mod::get()->getSavedValue<std::string>("index", ",");
        std::istringstream is(index);
        std::string key;
        while (std::getline(is, key, ',')) {
            if (key.empty()) continue;
            auto t = Mod::get()->getSavedValue<std::string>(key + "_t", "Unrated");
            auto st = loadArr<101>(key);
            tierDeaths[t] += std::accumulate(st.begin(), st.end(), 0);
        }
        int tmax = 1;
        for (auto& kv : tierDeaths) tmax = std::max(tmax, kv.second);
        label("$ analyze --difficulty --all-levels", FONT2, 0.9f, GREEN, {16.f, 84.f}, 0.f, at(0.6f));
        for (int t = 0; t < 7; t++) {
            int n = tierDeaths.count(TIERS[t]) ? tierDeaths[TIERS[t]] : 0;
            float cx = 44.f + 58.f * (float)t;
            float bh = 24.f * (float)n / (float)tmax;
            float dl = at(0.65f + 0.05f * (float)t);
            vbar(cx - 13.f, 28.f, 26.f, bh, soft(heatColor(n, tmax), 0.7f), dl);
            label(std::to_string(n), FONT2, 0.8f, {255, 255, 255}, {cx, 28.f + std::max(bh, 2.f) + 7.f}, 0.5f, dl);
            label(TIERS[t], FONT2, 0.7f, tier == TIERS[t] ? CYAN : SOFT, {cx, 14.f}, 0.5f, dl);
        }
        return true;
    }
};

class $modify(LabPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        if (!PlayLayer::get()) return;
        CCNode* parent = this->getChildByID("right-button-menu");
        if (!parent) {
            for (auto c : CCArrayExt<CCNode*>(this->getChildren())) {
                if (auto m = typeinfo_cast<CCMenu*>(c)) { parent = m; break; }
            }
        }
        if (!parent) return;
        auto lbl = CCLabelBMFont::create("DEATH LAB", FONT);
        lbl->setScale(0.45f);
        lbl->setColor({120, 255, 255});
        auto item = CCMenuItemSpriteExtra::create(lbl, this, menu_selector(LabPauseLayer::onLab));
        parent->addChild(item);
        parent->updateLayout();
    }

    void onLab(CCObject*) {
        auto pl = PlayLayer::get();
        if (!pl) return;
        auto key = levelKey(pl->m_level);
        DeathLab::create(pl->m_level, loadArr<101>(key),
            Mod::get()->getSavedValue<int>(key + "_w", 0), loadArr<8>(key + "_m"))->show();
    }
};

class $modify(StatsPlayLayer, PlayLayer) {
    struct Fields {
        std::array<int, 101> stats{};
        CCDrawNode* heat = nullptr;
        CCDrawNode* cur = nullptr;
        CCLabelBMFont* hotLbl = nullptr;
        float left = 0.f, y = 0.f;
    };

    void redrawHeat() {
        auto f = m_fields.self();
        f->heat->clear();
        int mx = *std::max_element(f->stats.begin(), f->stats.end());
        fillRect(f->heat, f->left - 2.f, f->y - 2.f, f->left + BAR_W + 2.f, f->y + 8.f,
                 {0.f, 0.f, 0.f, 0.6f}, 1.f, {0.3f, 0.9f, 1.f, 0.9f});
        for (int i = 0; i < 100; i++) {
            float x0 = f->left + BAR_W * i / 100.f;
            float x1 = f->left + BAR_W * (i + 1) / 100.f;
            auto c = heatColor(f->stats[i], mx);
            fillRect(f->heat, x0, f->y, x1, f->y + 6.f, c, 0.f, {0.f, 0.f, 0.f, 0.f});
        }
        if (mx > 0) {
            int worst = (int)(std::max_element(f->stats.begin(), f->stats.end()) - f->stats.begin());
            float x = f->left + BAR_W * (worst + 0.5f) / 100.f;
            CCPoint tri[3] = {CCPoint(x - 4.f, f->y - 8.f), CCPoint(x + 4.f, f->y - 8.f), CCPoint(x, f->y - 1.f)};
            f->heat->drawPolygon(tri, 3, {1.f, 0.2f, 0.2f, 1.f}, 0.f, {0.f, 0.f, 0.f, 0.f});
            f->hotLbl->setString(fmt::format("{}%", worst).c_str());
            f->hotLbl->setPosition({x, f->y - 15.f});
        } else {
            f->hotLbl->setString("");
        }
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        auto f = m_fields.self();
        f->stats = loadArr<101>(levelKey(level));
        auto win = CCDirector::get()->getWinSize();
        f->left = (win.width - BAR_W) / 2.f;
        f->y = win.height - 38.f;

        f->heat = CCDrawNode::create();
        m_uiLayer->addChild(f->heat, 900);
        f->cur = CCDrawNode::create();
        m_uiLayer->addChild(f->cur, 901);

        f->hotLbl = CCLabelBMFont::create("", FONT);
        f->hotLbl->setScale(0.3f);
        f->hotLbl->setColor({255, 90, 90});
        m_uiLayer->addChild(f->hotLbl, 902);

        redrawHeat();
        return true;
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        auto f = m_fields.self();
        if (!f->cur) return;
        float pct = std::clamp(this->getCurrentPercent(), 0.f, 100.f);
        float x = f->left + BAR_W * pct / 100.f;
        f->cur->clear();
        f->cur->drawSegment({x, f->y - 3.f}, {x, f->y + 9.f}, 1.2f, {1.f, 1.f, 1.f, 1.f});
    }

    void levelComplete() {
        PlayLayer::levelComplete();
        if (m_isPracticeMode) return;
        auto key = levelKey(m_level);
        Mod::get()->setSavedValue(key + "_w", Mod::get()->getSavedValue<int>(key + "_w", 0) + 1);
        registerLevel(m_level, key);
    }

    void destroyPlayer(PlayerObject* player, GameObject* obj) {
        bool wasDead = m_player1 && m_player1->m_isDead;
        PlayLayer::destroyPlayer(player, obj);
        if (wasDead || !m_player1 || !m_player1->m_isDead) return;
        if (m_isPracticeMode) return;

        auto f = m_fields.self();
        auto key = levelKey(m_level);
        int pct = std::clamp((int)this->getCurrentPercent(), 0, 100);
        f->stats[pct]++;
        saveArr(key, f->stats);
        auto modes = loadArr<8>(key + "_m");
        modes[modeIndex(m_player1)]++;
        saveArr(key + "_m", modes);
        registerLevel(m_level, key);
        redrawHeat();

        int worst = (int)(std::max_element(f->stats.begin(), f->stats.end()) - f->stats.begin());
        int total = std::accumulate(f->stats.begin(), f->stats.end(), 0);
        auto text = fmt::format("Died at {}%  (x{})\nMost deaths: {}% (x{})  |  Total: {}",
            pct, f->stats[pct], worst, f->stats[worst], total);

        auto win = CCDirector::get()->getWinSize();
        m_uiLayer->removeChildByTag(9123);
        auto lbl = CCLabelBMFont::create(text.c_str(), FONT);
        lbl->setTag(9123);
        lbl->setScale(0.4f);
        lbl->setColor({120, 255, 255});
        lbl->setPosition({win.width / 2.f, win.height - 75.f});
        m_uiLayer->addChild(lbl, 1000);
        lbl->runAction(CCSequence::create(
            CCDelayTime::create(2.f),
            CCFadeOut::create(0.4f),
            CCRemoveSelf::create(),
            nullptr));
    }
};
