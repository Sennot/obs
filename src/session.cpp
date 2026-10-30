#include "session.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace clicksync {

namespace {

// Paused milliseconds between the recording start and tsMs.
double pausedBefore(std::vector<Pause> const& pauses, double tsMs) {
    double total = 0;
    for (auto const& p : pauses) {
        if (p.fromMs >= tsMs) continue;
        double to = p.toMs < 0 ? tsMs : std::min(p.toMs, tsMs);
        total += to - p.fromMs;
    }
    return total;
}

}  // namespace

bool Mapping::videoAt(double tsMs, double& videoS) const {
    if (method == Method::None) return false;
    for (auto const& p : pauses) {
        if (tsMs >= p.fromMs && (p.toMs < 0 || tsMs < p.toMs)) return false;
    }
    videoS = (tsMs - pausedBefore(pauses, tsMs)) / 1000.0 + offsetS;
    return true;
}

Mapping computeMapping(ObsTiming const& timing) {
    Mapping m;
    m.anchors = static_cast<int>(timing.anchors.size());
    m.stopLagMs = timing.stopLagMs;
    m.pauses = timing.pauses;

    bool haveAnchors = !timing.anchors.empty();
    double anchorOffset = 0;
    if (haveAnchors) {
        // Median offset of the lowest-latency round trips: robust against slow replies.
        std::vector<Anchor> sorted = timing.anchors;
        std::sort(sorted.begin(), sorted.end(), [](Anchor const& a, Anchor const& b) {
            return (a.recvMs - a.sendMs) < (b.recvMs - b.sendMs);
        });
        size_t take = std::min<size_t>(sorted.size(), 8);
        std::vector<double> offsets;
        for (size_t i = 0; i < take; ++i) {
            double mid = (sorted[i].sendMs + sorted[i].recvMs) / 2.0;
            offsets.push_back(sorted[i].videoS - (mid - pausedBefore(timing.pauses, mid)) / 1000.0);
        }
        std::sort(offsets.begin(), offsets.end());
        anchorOffset = offsets[offsets.size() / 2];
        m.bestRttMs = sorted.front().recvMs - sorted.front().sendMs;
    }

    if (timing.haveStart) {
        m.method = Mapping::Method::Start;
        m.offsetS = -timing.startMs / 1000.0;
        if (haveAnchors) {
            m.lagKnown = true;
            m.lagMs = (m.offsetS - anchorOffset) * 1000.0;
        }
    } else if (haveAnchors) {
        m.method = Mapping::Method::Anchor;
        m.offsetS = anchorOffset;
        if (timing.stopLagMs >= 0) {
            m.lagKnown = true;
            m.lagMs = timing.stopLagMs;
            m.offsetS += timing.stopLagMs / 1000.0;
        }
    }
    return m;
}

long long xdbotFrame(double levelTime, double xdTps, int frameOffset) {
    // the epsilon keeps a level time that is a whole frame (62 steps at 480 TPS = 31 frames at 240) from
    // landing one frame short through floating point
    long long frame = static_cast<long long>(std::floor(levelTime * xdTps + 1e-6)) + 1 - frameOffset;
    return frame < 0 ? 0 : frame;
}

bool shouldSave(Mapping const& mapping, bool saveWithoutObs) {
    return mapping.hasVideo() || saveWithoutObs;
}

double estimateTps(std::vector<Event> const& events, double fallback) {
    std::vector<double> samples;
    for (auto const& e : events) {
        // early inputs say little: one step more or less is a big share of a short time
        if (e.tick > 0 && e.levelTime >= 1.0) samples.push_back(static_cast<double>(e.tick) / e.levelTime);
    }
    if (samples.empty()) return fallback;
    std::sort(samples.begin(), samples.end());
    double tps = samples[samples.size() / 2];
    double rounded = std::round(tps);
    // TPS is a whole number in practice, keep the fraction only when it clearly isn't
    return std::fabs(tps - rounded) < 0.1 ? rounded : tps;
}

std::string jsonEscape(std::string const& s) {
    std::string out;
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}

namespace {

std::string num(double v, int digits = 4) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", digits, v);
    return buf;
}

}  // namespace

std::string buildJson(std::vector<Event> const& events, std::vector<Attempt> const& attempts,
                      Mapping const& mapping, std::string const& obsPath, double tps) {
    auto videoOf = [&](double tsMs) {
        double v;
        return mapping.videoAt(tsMs, v) ? num(v) : std::string("null");
    };
    tps = estimateTps(events, tps);
    char const* method = mapping.method == Mapping::Method::Anchor  ? "anchor"
                         : mapping.method == Mapping::Method::Start ? "start"
                                                                    : "none";

    std::string j = "{\n  \"format\": \"clicksync/1\",\n";
    j += "  \"tps\": " + num(tps, 1) + ",\n";
    j += "  \"obs\": {\n    \"recording\": \"" + jsonEscape(obsPath) + "\",\n";
    j += std::string("    \"sync_method\": \"") + method + "\",\n";
    j += "    \"anchors\": " + std::to_string(mapping.anchors) + ",\n";
    j += "    \"best_rtt_ms\": " + num(mapping.bestRttMs, 2) + ",\n";
    j += "    \"pipeline_lag_ms\": " + (mapping.lagKnown ? num(mapping.lagMs, 1) : std::string("null")) + ",\n";
    j += "    \"stop_lag_ms\": " + (mapping.stopLagMs >= 0 ? num(mapping.stopLagMs, 1) : std::string("null")) + ",\n";
    j += "    \"pauses\": " + std::to_string(mapping.pauses.size()) + "\n  },\n";

    j += "  \"attempts\": [";
    for (size_t i = 0; i < attempts.size(); ++i) {
        auto const& a = attempts[i];
        j += i ? ",\n    " : "\n    ";
        j += "{\"n\": " + std::to_string(a.number) + ", \"level\": \"" + jsonEscape(a.level) + "\", \"level_id\": " +
             std::to_string(a.levelId) + ", \"video_s\": " + videoOf(a.tsMs) + "}";
    }
    j += attempts.empty() ? "],\n" : "\n  ],\n";

    j += "  \"events\": [";
    for (size_t i = 0; i < events.size(); ++i) {
        auto const& e = events[i];
        j += i ? ",\n    " : "\n    ";
        j += "{\"video_s\": " + videoOf(e.tsMs);
        j += ", \"level_time\": " + num(e.levelTime, 5);
        // the frame number xdBot's macro editor shows for this input (verified against a recorded .gdr)
        j += ", \"frame\": " + std::to_string(xdbotFrame(e.levelTime, e.xdTps, e.frameOffset));
        if (e.tick >= 0) j += ", \"tick\": " + std::to_string(e.tick);
        j += std::string(", \"down\": ") + (e.down ? "true" : "false");
        j += ", \"button\": " + std::to_string(e.button);
        j += std::string(", \"p2\": ") + (e.p2 ? "true" : "false");
        j += std::string(", \"practice\": ") + (e.practice ? "true" : "false");
        j += ", \"attempt\": " + std::to_string(e.attempt);
        j += ", \"level\": \"" + jsonEscape(e.level) + "\", \"level_id\": " + std::to_string(e.levelId) + "}";
    }
    j += events.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return j;
}

}  // namespace clicksync

