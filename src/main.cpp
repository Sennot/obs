#include <Geode/Geode.hpp>
#include <Geode/loader/ModEvent.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include "sync.hpp"

using namespace geode::prelude;

$on_mod(Loaded) {
    clicksync::syncStart();
}

// Every button press/release the game processes, including ones injected by a replay bot.
class $modify(CSGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
        auto* pl = PlayLayer::get();
        if (!pl || static_cast<GJBaseGameLayer*>(pl) != this) return;
        clicksync::syncLogButton(down, button, isPlayer1, m_gameState.m_levelTime, pl->m_isPracticeMode);
    }
};

class $modify(CSPlayLayer, PlayLayer) {
    void resetLevel() {
        PlayLayer::resetLevel();
        if (m_level) clicksync::syncAttempt(std::string(m_level->m_levelName), m_level->m_levelID.value());
    }

    void levelComplete() {
        PlayLayer::levelComplete();
        clicksync::syncLevelEnd();
    }

    void onQuit() {
        clicksync::syncLevelEnd();
        PlayLayer::onQuit();
    }
};
