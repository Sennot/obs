#pragma once

// Geode-free model of one recording session: logged inputs, OBS timing anchors,
// the mapping "game steady-clock -> video time" and the JSON that Click Sampler reads.

#include <string>
#include <vector>

namespace clicksync {

struct Event {
    double tsMs = 0;        // steady clock (ms) when the game processed the input
    double levelTime = 0;   // GJGameState::m_levelTime (seconds)
    bool down = false;
    int button = 1;
    bool p2 = false;
    bool practice = false;
    int attempt = 0;
    std::string level;
    int levelId = 0;
    long long tick = -1;    // physics steps done when the input was processed (GJGameState::m_currentProgress), -1 = unknown
    int frameOffset = 0;    // xdBot's "Frame Offset" setting at that moment
};

struct Attempt {
    double tsMs = 0;
    int number = 0;
    std::string level;
    int levelId = 0;
};

// One GetRecordStatus round trip: OBS answered "video is at videoS" somewhere between sendMs and recvMs.
struct Anchor {
    double sendMs = 0;
    double recvMs = 0;
    double videoS = 0;
};

struct Mapping {
    enum class Method { None, Anchor, Start };
    Method method = Method::None;
    double offsetS = 0;     // video seconds = tsMs / 1000 + offsetS
    double bestRttMs = 0;
    int anchors = 0;
};

Mapping computeMapping(std::vector<Anchor> const& anchors, bool haveStart, double startMs);

// A session is only worth a file when it knows where its inputs are in a video. The Click Sampler plugin
// refuses anything else, so the rest is just clutter (and used to overwrite latest.json).
bool shouldSave(Mapping const& mapping, bool saveWithoutObs);

// Ticks per second the game really ran at, from tick / level time of the logged inputs (median).
// `fallback` when there is nothing to measure.
double estimateTps(std::vector<Event> const& events, double fallback);

std::string jsonEscape(std::string const& s);

std::string buildJson(std::vector<Event> const& events, std::vector<Attempt> const& attempts,
                      Mapping const& mapping, std::string const& obsPath, double tps = 240.0);

}  // namespace clicksync
