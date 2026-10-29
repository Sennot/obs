#pragma once

#include <string>

namespace clicksync {

// Connects to OBS in the background and starts collecting. Call once from the mod's Loaded event.
void syncStart();

// Called from game hooks (main thread).
void syncLogButton(bool down, int button, bool player1, double levelTime, bool practice, long long tick);
void syncAttempt(std::string const& level, int levelId);
void syncLevelEnd();

}  // namespace clicksync
