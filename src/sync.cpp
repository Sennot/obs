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
    std::vector<Anchor> anchors;
    std::unordered_map<std::string, double> pending;  // request id -> send time
    bool recActive = false;
    double recStartMs = 0;
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
        g.attempts.clear();
        g.anchors.clear();
        g.attemptNo = 0;
        return;
    }
    Mapping mapping = computeMapping(g.anchors, g.recActive || !g.recPath.empty(), g.recStartMs);
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

    for (auto const& path : {file, dir / "latest.json"}) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << json;
    }
    log::info("ClickSync: saved {} events ({}, sync={}) to {}", g.events.size(), why,
              mapping.method == Mapping::Method::Anchor ? "anchor" : mapping.method == Mapping::Method::Start ? "start" : "none",
              file.string());

    g.events.clear();
    g.attempts.clear();
    g.anchors.clear();
    g.attemptNo = 0;
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
            if (password.empty()) log::warn("ClickSync: OBS requires a password, but the 'OBS Password' setting is empty");
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
            std::lock_guard lock(g.mutex);
            if (state == "OBS_WEBSOCKET_OUTPUT_STARTED") {
                g.recActive = true;
                g.recStartMs = nowMs();
                g.recPath.clear();
                g.events.clear();
                g.attempts.clear();
                g.anchors.clear();
                g.attemptNo = 0;
                log::info("ClickSync: OBS recording started");
            } else if (state == "OBS_WEBSOCKET_OUTPUT_STOPPED") {
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
        double durationMs = data["outputDuration"].asDouble().unwrapOr(0.0);
        std::lock_guard lock(g.mutex);
        double recv = nowMs();
        auto it = g.pending.find(id);
        double send = it != g.pending.end() ? it->second : recv;
        g.pending.erase(id);
        if (id == "init") {
            if (active && !g.recActive) {
                g.recActive = true;
                g.recStartMs = recv - durationMs;
            }
        } else if (active && g.recActive) {
            g.anchors.push_back({send, recv, durationMs / 1000.0});
            if (g.anchors.size() > 7200) g.anchors.erase(g.anchors.begin(), g.anchors.begin() + 3600);
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
        int code = g_ws.closeCode();
        if (code == 4009)
            log::warn("ClickSync: OBS rejected the password (close code 4009). Check 'OBS Password' in the mod settings");
        else
            log::info("ClickSync: OBS connection closed (code {}), will retry", code);
    };
    g_ws.start();
}

void syncLogButton(bool down, int button, bool player1, double levelTime, bool practice) {
    if (!Mod::get()->getSettingValue<bool>("enabled")) return;
    Event e;
    e.tsMs = nowMs();
    e.levelTime = levelTime;
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
