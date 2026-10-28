#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <algorithm>
#include <array>
#include <numeric>
#include <sstream>
#include <functional>

using namespace geode::prelude;

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

class $modify(StatsPlayLayer, PlayLayer) {
    void destroyPlayer(PlayerObject* player, GameObject* obj) {
        bool wasDead = m_player1 && m_player1->m_isDead;
        PlayLayer::destroyPlayer(player, obj);
        if (wasDead || !m_player1 || !m_player1->m_isDead) return;
        if (m_isPracticeMode) return;

        int pct = std::clamp((int)this->getCurrentPercent(), 0, 100);
        auto key = levelKey(m_level);
        auto stats = loadStats(key);
        stats[pct]++;
        saveStats(key, stats);

        int worst = 0;
        for (int i = 1; i <= 100; i++) {
            if (stats[i] > stats[worst]) worst = i;
        }
        int total = std::accumulate(stats.begin(), stats.end(), 0);
        auto text = fmt::format("Died at {}%  (x{})\nMost deaths: {}% (x{})  |  Total: {}",
            pct, stats[pct], worst, stats[worst], total);

        auto win = CCDirector::get()->getWinSize();
        m_uiLayer->removeChildByTag(9123);
        auto lbl = CCLabelBMFont::create(text.c_str(), "bigFont.fnt");
        lbl->setTag(9123);
        lbl->setScale(0.45f);
        lbl->setPosition({win.width / 2.f, win.height - 45.f});
        m_uiLayer->addChild(lbl, 1000);
        lbl->runAction(CCSequence::create(
            CCDelayTime::create(2.f),
            CCFadeOut::create(0.4f),
            CCRemoveSelf::create(),
            nullptr));
    }
};
