#include "sync.hpp"

#include <Geode/Geode.hpp>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>

#include "crypto.hpp"
#include "session.hpp"
#include "ws_client.hpp"

using namespace geode::prelude;

namespace clicksync {

namespace {

double nowMs() {
    using namespace std::chrono;
    static auto const epoch = steady_clock::now();
    return duration<double, std::milli>(steady_clock::now() - epoch).count();
}

struct State {
    std::mutex mutex;
    std::vector<Event> events;
    std::vector<Attempt> attempts;
    ObsTiming timing;
    std::unordered_map<std::string, double> pending;  // request id -> send time
    bool recActive = false;
    double stoppingMs = -1;
    std::string recPath;
    std::string level;
    int levelId = 0;
    int attemptNo = 0;
    double lastRequestMs = 0;
    int requestSeq = 0;
};

State g;
WsClient g_ws;
bool g_started = false;

std::string quote(std::string const& s) { return "\"" + jsonEscape(s) + "\""; }

bool boolSetting(char const* key) { return Mod::get()->getSettingValue<bool>(key); }

// xdBot subtracts its "Frame Offset" setting from every frame it records. Settings are public, no offsets needed.
int xdbotFrameOffset() {
    auto* xd = Loader::get()->getLoadedMod("zilko.xdbot");
    if (!xd || !xd->hasSetting("frame_offset")) return 0;
    return static_cast<int>(xd->getSettingValue<int64_t>("frame_offset"));
}

// xdBot numbers macro frames with its own TPS (saved values "macro_tps_enabled" / "macro_tps"), not with the
// steps the game really runs: at 480 game TPS a 240 TPS macro still says frame 32 where the step counter says 63.
double xdbotTps() {
    auto* xd = Loader::get()->getLoadedMod("zilko.xdbot");
    // getSavedValue with a default would write that default into xdBot's save, so check first
    if (!xd || !xd->hasSavedValue("macro_tps_enabled") || !xd->hasSavedValue("macro_tps")) return 240.0;
    if (!xd->getSavedValue<bool>("macro_tps_enabled")) return 240.0;
    double tps = xd->getSavedValue<double>("macro_tps");
    return tps > 0 ? tps : 240.0;
}

void resetSessionLocked() {
    g.events.clear();
    g.attempts.clear();
    g.timing = {};
    g.stoppingMs = -1;
    g.attemptNo = 0;
}

void notifyMainThread(std::string text) {
    if (!boolSetting("notifications")) return;
    Loader::get()->queueInMainThread([text = std::move(text)] {
        Notification::create(text, NotificationIcon::Success, 2.5f)->show();
    });
}

std::filesystem::path outputDir() {
    auto custom = Mod::get()->getSettingValue<std::string>("output-dir");
    if (!custom.empty()) return std::filesystem::u8path(custom);
    if (auto const* home = std::getenv("USERPROFILE")) {
        return std::filesystem::u8path(home) / "Documents" / "ClickSampler" / "Sync";
    }
    return Mod::get()->getSaveDir() / "sessions";
}

// Must be called with g.mutex held. Clears the session.
void flushLocked(char const* why) {
    if (g.events.empty()) {
        resetSessionLocked();
        return;
    }
    Mapping mapping = computeMapping(g.timing);
    bool hasVideo = mapping.hasVideo();
    if (!shouldSave(mapping, boolSetting("save-without-obs"))) {
        log::info("ClickSync: dropped {} inputs ({}), OBS was not recording", g.events.size(), why);
        resetSessionLocked();
        return;
    }
    std::string json = buildJson(g.events, g.attempts, mapping, g.recPath);

    std::error_code ec;
    auto dir = outputDir();
    std::filesystem::create_directories(dir, ec);

    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tm);
    auto file = dir / (std::string("clicksync_") + stamp + ".json");

    // latest.json is what the plugin's "latest" button opens, so only a session with video time may take it over
    std::vector<std::filesystem::path> targets = {file};
    if (hasVideo) targets.push_back(dir / "latest.json");
    for (auto const& path : targets) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << json;
    }
    log::info("ClickSync: saved {} events ({}, sync={}, OBS lag {} ms, stop lag {} ms, {} pauses) to {}",
              g.events.size(), why,
              mapping.method == Mapping::Method::Anchor ? "anchor" : mapping.method == Mapping::Method::Start ? "start" : "none",
              mapping.lagKnown ? std::to_string(static_cast<int>(mapping.lagMs)) : std::string("?"),
              static_cast<int>(mapping.stopLagMs), mapping.pauses.size(), file.string());
    if (mapping.method == Mapping::Method::Anchor) {
        log::warn("ClickSync: the recording started before OBS was connected, video time is less exact");
    }

    if (hasVideo) notifyMainThread("ClickSync: saved " + std::to_string(g.events.size()) + " clicks");

    resetSessionLocked();
}

void requestStatus(std::string const& id) {
    g.pending[id] = nowMs();
    g_ws.send("{\"op\":6,\"d\":{\"requestType\":\"GetRecordStatus\",\"requestId\":" + quote(id) + "}}");
}

void handleMessage(std::string const& text) {
    auto parsed = matjson::parse(text);
    if (!parsed) return;
    auto root = std::move(parsed).unwrap();
    int op = root["op"].asInt().unwrapOr(-1);
    auto const& d = root["d"];

    if (op == 0) {  // Hello
        std::string identify = "{\"op\":1,\"d\":{\"rpcVersion\":1,\"eventSubscriptions\":64";  // Outputs
        if (d.contains("authentication")) {
            auto password = Mod::get()->getSettingValue<std::string>("obs-password");
            auto challenge = d["authentication"]["challenge"].asString().unwrapOr("");
            auto salt = d["authentication"]["salt"].asString().unwrapOr("");
            identify += ",\"authentication\":" + quote(obsAuth(password, salt, challenge));
        }
        identify += "}}";
        g_ws.send(identify);
    } else if (op == 2) {  // Identified
        log::info("ClickSync: connected to OBS");
        std::lock_guard lock(g.mutex);
        requestStatus("init");
    } else if (op == 5) {  // Event
        auto type = d["eventType"].asString().unwrapOr("");
        if (type == "RecordStateChanged") {
            auto state = d["eventData"]["outputState"].asString().unwrapOr("");
            auto path = d["eventData"]["outputPath"].asString().unwrapOr("");
            double now = nowMs();
            std::lock_guard lock(g.mutex);
            if (state == "OBS_WEBSOCKET_OUTPUT_STARTED") {
                // OBS sends this when it hooks the encoders: the next frame it draws is frame 0 of the file
                resetSessionLocked();
                g.recActive = true;
                g.recPath.clear();
                g.timing.haveStart = true;
                g.timing.startMs = now;
                log::info("ClickSync: OBS recording started");
                notifyMainThread("ClickSync: logging clicks for the OBS recording");
            } else if (state == "OBS_WEBSOCKET_OUTPUT_STOPPING") {
                g.stoppingMs = now;
            } else if (state == "OBS_WEBSOCKET_OUTPUT_PAUSED") {
                if (g.recActive) g.timing.pauses.push_back({now, -1});
            } else if (state == "OBS_WEBSOCKET_OUTPUT_RESUMED") {
                if (!g.timing.pauses.empty() && g.timing.pauses.back().toMs < 0) g.timing.pauses.back().toMs = now;
            } else if (state == "OBS_WEBSOCKET_OUTPUT_STOPPED") {
                // OBS keeps writing until the frame of the stop request is through its pipeline
                if (g.stoppingMs >= 0) g.timing.stopLagMs = now - g.stoppingMs;
                g.recActive = false;
                g.recPath = path;
                flushLocked("obs stopped");
                g.recPath.clear();
            }
        }
    } else if (op == 7) {  // RequestResponse
        auto id = d["requestId"].asString().unwrapOr("");
        auto const& data = d["responseData"];
        bool active = data["outputActive"].asBool().unwrapOr(false);
        bool paused = data["outputPaused"].asBool().unwrapOr(false);
        double durationMs = data["outputDuration"].asDouble().unwrapOr(0.0);
        std::lock_guard lock(g.mutex);
        double recv = nowMs();
        auto it = g.pending.find(id);
        double send = it != g.pending.end() ? it->second : recv;
        g.pending.erase(id);
        if (id == "init") {
            // a recording that was already running: its start is unknown, the anchors have to do
            if (active && !g.recActive) {
                g.recActive = true;
                log::warn("ClickSync: OBS was already recording, start the recording after the game for exact sync");
            }
        } else if (active && !paused && g.recActive) {
            auto& anchors = g.timing.anchors;
            anchors.push_back({send, recv, durationMs / 1000.0});
            if (anchors.size() > 7200) anchors.erase(anchors.begin(), anchors.begin() + 3600);
        }
    }
}

void handleTick() {
    std::lock_guard lock(g.mutex);
    if (!g.recActive) return;
    double now = nowMs();
    if (now - g.lastRequestMs >= 1000.0) {
        g.lastRequestMs = now;
        requestStatus("a" + std::to_string(++g.requestSeq));
    }
}

}  // namespace

void syncStart() {
    if (g_started) return;
    g_started = true;
    g_ws.endpoint = [] {
        WsClient::Endpoint ep;
        ep.host = Mod::get()->getSettingValue<std::string>("obs-host");
        ep.port = static_cast<int>(Mod::get()->getSettingValue<int64_t>("obs-port"));
        return ep;
    };
    g_ws.onMessage = handleMessage;
    g_ws.onTick = handleTick;
    g_ws.onClose = [] {
        std::lock_guard lock(g.mutex);
        g.pending.clear();
        log::info("ClickSync: OBS connection closed, will retry");
    };
    g_ws.start();
}

void syncLogButton(bool down, int button, bool player1, double levelTime, bool practice, long long tick) {
    if (!Mod::get()->getSettingValue<bool>("enabled")) return;
    Event e;
    e.tsMs = nowMs();
    e.levelTime = levelTime;
    e.tick = tick;
    e.frameOffset = xdbotFrameOffset();
    e.xdTps = xdbotTps();
    e.down = down;
    e.button = button;
    e.p2 = !player1;
    e.practice = practice;
    std::lock_guard lock(g.mutex);
    e.attempt = g.attemptNo;
    e.level = g.level;
    e.levelId = g.levelId;
    g.events.push_back(std::move(e));
}

void syncAttempt(std::string const& level, int levelId) {
    if (!Mod::get()->getSettingValue<bool>("enabled")) return;
    std::lock_guard lock(g.mutex);
    g.level = level;
    g.levelId = levelId;
    ++g.attemptNo;
    g.attempts.push_back({nowMs(), g.attemptNo, level, levelId});
}

void syncLevelEnd() {
    std::lock_guard lock(g.mutex);
    // While OBS records, everything stays in one session until the recording stops.
    if (!g.recActive && !g.events.empty()) flushLocked("level end");
}

}  // namespace clicksync
