#include "session.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace clicksync {

Mapping computeMapping(std::vector<Anchor> const& anchors, bool haveStart, double startMs) {
    Mapping m;
    m.anchors = static_cast<int>(anchors.size());
    if (!anchors.empty()) {
        // Median offset of the lowest-latency round trips: robust against slow replies.
        std::vector<Anchor> sorted = anchors;
        std::sort(sorted.begin(), sorted.end(), [](Anchor const& a, Anchor const& b) {
            return (a.recvMs - a.sendMs) < (b.recvMs - b.sendMs);
        });
        size_t take = std::min<size_t>(sorted.size(), 8);
        std::vector<double> offsets;
        for (size_t i = 0; i < take; ++i) {
            double mid = (sorted[i].sendMs + sorted[i].recvMs) / 2.0;
            offsets.push_back(sorted[i].videoS - mid / 1000.0);
        }
        std::sort(offsets.begin(), offsets.end());
        m.method = Mapping::Method::Anchor;
        m.offsetS = offsets[offsets.size() / 2];
        m.bestRttMs = sorted.front().recvMs - sorted.front().sendMs;
    } else if (haveStart) {
        m.method = Mapping::Method::Start;
        m.offsetS = -startMs / 1000.0;
    }
    return m;
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
    bool hasVideo = mapping.method != Mapping::Method::None;
    auto videoOf = [&](double tsMs) { return tsMs / 1000.0 + mapping.offsetS; };
    char const* method = mapping.method == Mapping::Method::Anchor  ? "anchor"
                         : mapping.method == Mapping::Method::Start ? "start"
                                                                    : "none";

    std::string j = "{\n  \"format\": \"clicksync/1\",\n";
    j += "  \"tps\": " + num(tps, 1) + ",\n";
    j += "  \"obs\": {\n    \"recording\": \"" + jsonEscape(obsPath) + "\",\n";
    j += std::string("    \"sync_method\": \"") + method + "\",\n";
    j += "    \"anchors\": " + std::to_string(mapping.anchors) + ",\n";
    j += "    \"best_rtt_ms\": " + num(mapping.bestRttMs, 2) + "\n  },\n";

    j += "  \"attempts\": [";
    for (size_t i = 0; i < attempts.size(); ++i) {
        auto const& a = attempts[i];
        j += i ? ",\n    " : "\n    ";
        j += "{\"n\": " + std::to_string(a.number) + ", \"level\": \"" + jsonEscape(a.level) + "\", \"level_id\": " +
             std::to_string(a.levelId) + ", \"video_s\": " + (hasVideo ? num(videoOf(a.tsMs)) : "null") + "}";
    }
    j += attempts.empty() ? "],\n" : "\n  ],\n";

    j += "  \"events\": [";
    for (size_t i = 0; i < events.size(); ++i) {
        auto const& e = events[i];
        j += i ? ",\n    " : "\n    ";
        j += "{\"video_s\": " + (hasVideo ? num(videoOf(e.tsMs)) : std::string("null"));
        j += ", \"level_time\": " + num(e.levelTime, 5);
        j += ", \"frame\": " + std::to_string(static_cast<long long>(std::llround(e.levelTime * tps)));
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
