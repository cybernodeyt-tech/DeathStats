#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <algorithm>
#include <array>
#include <numeric>
#include <sstream>
#include <functional>

using namespace geode::prelude;

static const char* FONT = "bigFont.fnt";
constexpr float BAR_W = 240.f;

static std::string levelKey(GJGameLevel* lvl) {
    if (lvl->m_levelID.value() != 0) return fmt::format("lvl{}", lvl->m_levelID.value());
    return fmt::format("loc{}", std::hash<std::string>{}(std::string(lvl->m_levelName)));
}

static std::array<int, 101> loadStats(const std::string& key) {
    std::array<int, 101> a{};
    auto s = Mod::get()->getSavedValue<std::string>(key, "");
    std::istringstream ss(s);
    for (auto& v : a) {
        if (!(ss >> v)) break;
    }
    return a;
}

static void saveStats(const std::string& key, const std::array<int, 101>& a) {
    std::string s;
    for (int v : a) s += std::to_string(v) + " ";
    Mod::get()->setSavedValue(key, s);
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

static void fillRect(CCDrawNode* n, float x0, float y0, float x1, float y1,
                     ccColor4F fill, float bw, ccColor4F bc) {
    CCPoint v[4] = {CCPoint(x0, y0), CCPoint(x1, y0), CCPoint(x1, y1), CCPoint(x0, y1)};
    n->drawPolygon(v, 4, fill, bw, bc);
}

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
        f->stats = loadStats(levelKey(level));
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

        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        auto lbl = CCLabelBMFont::create("STATS", FONT);
        lbl->setScale(0.35f);
        lbl->setColor({120, 255, 255});
        auto item = CCMenuItemSpriteExtra::create(lbl, this, menu_selector(StatsPlayLayer::onOpen));
        item->setPosition({f->left + BAR_W + 32.f, f->y + 3.f});
        menu->addChild(item);
        m_uiLayer->addChild(menu, 903);

        redrawHeat();
        return true;
    }

    void onOpen(CCObject*) {
        auto f = m_fields.self();
        std::vector<int> idx(101);
        std::iota(idx.begin(), idx.end(), 0);
        std::sort(idx.begin(), idx.end(), [&](int a, int b) { return f->stats[a] > f->stats[b]; });
        int total = std::accumulate(f->stats.begin(), f->stats.end(), 0);
        int mx = std::max(1, *std::max_element(f->stats.begin(), f->stats.end()));

        std::string t = fmt::format("<cj>TOTAL DEATHS: {}</c>\n\n", total);
        int shown = 0;
        for (int i : idx) {
            if (f->stats[i] <= 0 || shown >= 8) break;
            float r = (float)f->stats[i] / (float)mx;
            const char* tag = r >= 0.75f ? "cr" : r >= 0.5f ? "co" : r >= 0.25f ? "cy" : "cg";
            t += fmt::format("<{}>#{}   {}%   -   {} deaths</c>\n", tag, shown + 1, i, f->stats[i]);
            shown++;
        }
        if (!shown) t += "No deaths recorded yet.\n";
        t += "\n<cr>red</c> = most deaths    <cy>yellow</c> = some    <cg>green</c> = few";
        FLAlertLayer::create(nullptr, "DEATH STATS", t, "Close", nullptr, 360.f)->show();
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

    void destroyPlayer(PlayerObject* player, GameObject* obj) {
        bool wasDead = m_player1 && m_player1->m_isDead;
        PlayLayer::destroyPlayer(player, obj);
        if (wasDead || !m_player1 || !m_player1->m_isDead) return;
        if (m_isPracticeMode) return;

        auto f = m_fields.self();
        int pct = std::clamp((int)this->getCurrentPercent(), 0, 100);
        f->stats[pct]++;
        saveStats(levelKey(m_level), f->stats);
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
