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
    double xdTps = 240.0;   // TPS xdBot numbers its macro frames with (its TPS setting, 240 when that is off)
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

// Steady-clock span where the recording was paused: none of it is in the video.
struct Pause {
    double fromMs = 0;
    double toMs = -1;       // -1 = still paused when the recording stopped
};

// Everything the OBS connection learned about one recording.
struct ObsTiming {
    std::vector<Anchor> anchors;
    bool haveStart = false; // RecordStateChanged STARTED arrived while connected
    double startMs = 0;     // ...at this steady time: OBS hooks its encoders there, so it is video frame 0
    double stopLagMs = -1;  // STOPPING -> STOPPED: OBS flushing frames still in its pipeline, -1 = not seen
    std::vector<Pause> pauses;
};

struct Mapping {
    enum class Method { None, Anchor, Start };
    Method method = Method::None;
    double offsetS = 0;     // video seconds = tsMs / 1000 + offsetS - time paused before tsMs
    double bestRttMs = 0;
    int anchors = 0;
    // How far OBS's outputDuration runs behind the picture: frames wait for audio buffering (up to 960 ms)
    // and the encoder before they are counted.
    bool lagKnown = false;
    double lagMs = 0;
    double stopLagMs = -1;
    std::vector<Pause> pauses;

    bool hasVideo() const { return method != Method::None; }
    // Video second of steady time tsMs; false when that moment is not in the video (no mapping, paused).
    bool videoAt(double tsMs, double& videoS) const;
};

// Frame 0 of the file is the moment the recording started. GetRecordStatus can't be used for that on its own:
// its outputDuration counts frames that already went through audio buffering and the encoder, so it lags the
// picture by up to a second, and by a different amount in every OBS session. It only fills in when the start
// was missed (mod connected to a running recording), corrected by the lag OBS showed while stopping.
Mapping computeMapping(ObsTiming const& timing);

// xdBot's frame number for a level time: floor(level time * its TPS) + 1 - its Frame Offset, never below 0.
long long xdbotFrame(double levelTime, double xdTps, int frameOffset);

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
