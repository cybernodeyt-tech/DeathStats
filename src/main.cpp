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
public:
    static DeathLab* create(GJGameLevel* lvl, const std::array<int, 101>& d, int wins,
                            const std::array<int, 8>& modes) {
        auto ret = new DeathLab();
        if (ret->init(lvl, d, wins, modes)) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

protected:
    bool init(GJGameLevel* lvl, const std::array<int, 101>& d, int wins,
              const std::array<int, 8>& modes) {
        if (!Popup::init(440.f, 270.f)) return false;
        if (m_bgSprite) m_bgSprite->setVisible(false);
        auto sz = m_mainLayer->getContentSize();
        float w = sz.width > 0.f ? sz.width : 440.f;
        float h = sz.height > 0.f ? sz.height : 270.f;
        const ccColor3B CYAN = {120, 255, 255};
        const ccColor3B SOFT = {200, 220, 230};
        const float T0 = 0.55f;

        auto panel = CCDrawNode::create();
        fillRect(panel, 0, 0, w, h, {0.02f, 0.05f, 0.1f, 0.97f}, 2.f, {0.2f, 0.9f, 1.f, 1.f});
        for (float y = 10.f; y < h; y += 20.f)
            panel->drawSegment({10.f, y}, {w - 10.f, y}, 0.5f, {0.2f, 0.8f, 1.f, 0.06f});
        ccColor4F neon = {0.4f, 1.f, 1.f, 1.f};
        panel->drawSegment({4.f, 4.f}, {24.f, 4.f}, 2.f, neon);
        panel->drawSegment({4.f, 4.f}, {4.f, 24.f}, 2.f, neon);
        panel->drawSegment({w - 4.f, 4.f}, {w - 24.f, 4.f}, 2.f, neon);
        panel->drawSegment({w - 4.f, 4.f}, {w - 4.f, 24.f}, 2.f, neon);
        panel->drawSegment({4.f, h - 4.f}, {24.f, h - 4.f}, 2.f, neon);
        panel->drawSegment({4.f, h - 4.f}, {4.f, h - 24.f}, 2.f, neon);
        panel->drawSegment({w - 4.f, h - 4.f}, {w - 24.f, h - 4.f}, 2.f, neon);
        panel->drawSegment({w - 4.f, h - 4.f}, {w - 4.f, h - 24.f}, 2.f, neon);
        panel->drawSegment({10.f, h - 36.f}, {w - 10.f, h - 36.f}, 1.f, {0.3f, 0.9f, 1.f, 0.5f});
        panel->drawSegment({255.f, 96.f}, {255.f, h - 40.f}, 1.f, {0.3f, 0.9f, 1.f, 0.3f});
        panel->drawSegment({10.f, 90.f}, {w - 10.f, 90.f}, 1.f, {0.3f, 0.9f, 1.f, 0.5f});
        m_mainLayer->addChild(panel, -1);

        auto label = [&](const std::string& t, const char* font, float sc, ccColor3B col,
                         CCPoint pos, float ax, float delay) {
            auto l = CCLabelBMFont::create(t.c_str(), font);
            l->setScale(sc);
            l->setColor(col);
            l->setAnchorPoint({ax, 0.5f});
            l->setPosition(pos);
            l->setOpacity(0);
            l->runAction(CCSequence::create(CCDelayTime::create(delay), CCFadeIn::create(0.25f), nullptr));
            m_mainLayer->addChild(l, 5);
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

        // boot line
        auto boot = CCLabelBMFont::create("> INITIALIZING DEATH ANALYZER_", FONT2);
        boot->setColor({90, 255, 140});
        boot->setPosition({w / 2.f, h / 2.f});
        boot->setOpacity(0);
        boot->runAction(CCSequence::create(CCFadeIn::create(0.15f), CCDelayTime::create(0.3f),
            CCFadeOut::create(0.15f), CCRemoveSelf::create(), nullptr));
        m_mainLayer->addChild(boot, 10);

        // scan line
        auto scan = CCDrawNode::create();
        scan->drawSegment({0.f, 0.f}, {w, 0.f}, 1.5f, {0.4f, 1.f, 1.f, 0.9f});
        scan->runAction(CCSequence::create(CCDelayTime::create(0.2f),
            CCMoveTo::create(0.8f, {0.f, h}), CCRemoveSelf::create(), nullptr));
        m_mainLayer->addChild(scan, 50);

        // header
        std::string name = std::string(lvl->m_levelName);
        if (name.size() > 20) name = name.substr(0, 18) + "..";
        auto tier = tierName(lvl);
        label("DEATH ANALYZER", FONT, 0.5f, CYAN, {16.f, h - 20.f}, 0.f, T0);
        label(fmt::format("{}  |  {}", name, tier), FONT2, 0.9f, SOFT, {w - 16.f, h - 20.f}, 1.f, T0);

        // stuck points
        label("STUCK POINTS", FONT, 0.36f, CYAN, {16.f, h - 50.f}, 0.f, T0);
        auto stuck = computeStuck(d, wins);
        if (stuck.empty()) label("No deaths recorded yet.", FONT2, 0.9f, SOFT, {16.f, h - 74.f}, 0.f, T0);
        for (size_t i = 0; i < stuck.size(); i++) {
            auto& s = stuck[i];
            float y = h - 74.f - 28.f * (float)i;
            float dl = T0 + 0.08f * (float)i;
            auto c = s.lowData ? ccColor4F{0.6f, 0.7f, 0.8f, 1.f} : chanceColor(s.pass);
            label(fmt::format("#{}  {}-{}%", i + 1, s.lo, s.hi), FONT, 0.38f, col3(c), {16.f, y}, 0.f, dl);
            std::string info = s.lowData
                ? fmt::format("{} deaths  |  low data", s.deaths)
                : fmt::format("{} deaths  |  pass ~{}%  |  ~{:.1f} tries", s.deaths, (int)std::lround(s.pass * 100.f), s.tries);
            label(info, FONT2, 0.8f, SOFT, {16.f, y - 13.f}, 0.f, dl);
            hbar(140.f, y - 3.f, 100.f, 6.f, {1.f, 1.f, 1.f, 0.08f}, dl);
            hbar(140.f, y - 3.f, 100.f * (s.lowData ? 0.f : s.pass), 6.f, c, dl);
        }

        // gamemodes
        label("BY GAMEMODE", FONT, 0.36f, CYAN, {270.f, h - 50.f}, 0.f, T0);
        std::vector<int> mi(8);
        std::iota(mi.begin(), mi.end(), 0);
        std::sort(mi.begin(), mi.end(), [&](int a, int b) { return modes[a] > modes[b]; });
        int mmax = std::max(1, modes[mi[0]]);
        int shown = 0;
        for (int m : mi) {
            if (modes[m] <= 0 || shown >= 4) break;
            float y = h - 74.f - 28.f * (float)shown;
            float dl = T0 + 0.08f * (float)shown;
            auto c = heatColor(modes[m], mmax);
            label(MODES[m], FONT, 0.38f, col3(c), {270.f, y}, 0.f, dl);
            hbar(330.f, y - 3.f, 70.f * modes[m] / (float)mmax, 6.f, c, dl);
            label(std::to_string(modes[m]), FONT2, 0.9f, SOFT, {w - 16.f, y}, 1.f, dl);
            shown++;
        }
        if (!shown) label("No mode data yet", FONT2, 0.9f, SOFT, {270.f, h - 74.f}, 0.f, T0);

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
        label("ALL LEVELS BY DIFFICULTY", FONT, 0.3f, CYAN, {16.f, 78.f}, 0.f, T0);
        for (int t = 0; t < 7; t++) {
            int n = tierDeaths.count(TIERS[t]) ? tierDeaths[TIERS[t]] : 0;
            float cx = 44.f + 58.f * (float)t;
            float bh = 24.f * (float)n / (float)tmax;
            float dl = T0 + 0.05f * (float)t;
            vbar(cx - 13.f, 26.f, 26.f, bh, heatColor(n, tmax), dl);
            label(std::to_string(n), FONT, 0.28f, {255, 255, 255}, {cx, 26.f + std::max(bh, 2.f) + 7.f}, 0.5f, dl);
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
