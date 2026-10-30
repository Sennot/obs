#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>

#include "crypto.hpp"
#include "session.hpp"

using namespace clicksync;

static std::string hex(std::array<uint8_t, 32> const& d) {
    static char const* h = "0123456789abcdef";
    std::string s;
    for (auto b : d) {
        s.push_back(h[b >> 4]);
        s.push_back(h[b & 15]);
    }
    return s;
}

static bool near(double a, double b, double eps = 1e-9) { return std::fabs(a - b) < eps; }

int main() {
    // SHA-256 known vectors
    assert(hex(sha256("")) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    assert(hex(sha256("abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    assert(hex(sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
           "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    // Base64 vectors
    auto b64 = [](std::string const& s) { return base64(reinterpret_cast<uint8_t const*>(s.data()), s.size()); };
    assert(b64("") == "");
    assert(b64("f") == "Zg==");
    assert(b64("fo") == "Zm8=");
    assert(b64("foo") == "Zm9v");
    assert(b64("foobar") == "Zm9vYmFy");

    // obs-websocket v5 auth, reference value computed with Python hashlib
    assert(obsAuth("supersecretpassword", "lM1GncleQOaCu9lT1yeUZhFYnqhsLLP1G5lAGo3ixaI",
                   "+IxH4CnCiqpX1rM9scsNynZzbOe4KhDeYcTNS3PDaeY") == "q8SbQgXa26BTSTIoKeGRlnYvcswB3DJdYnKEGOmKgaQ=");

    // Mapping: best round trips win, median offset
    {
        std::vector<Anchor> a = {
            {1000, 1010, 5.000 + 1.005 - 1.0},   // mid 1005 ms
            {2000, 2400, 99.0},                  // slow reply, ignored by the lowest-RTT selection
            {3000, 3008, 7.004},                 // mid 3004 ms -> offset 4.000
        };
        ObsTiming t;
        t.anchors = a;
        Mapping m = computeMapping(t);
        assert(m.method == Mapping::Method::Anchor);
        assert(m.anchors == 3);
        assert(near(m.bestRttMs, 8.0));
    }
    {
        ObsTiming t;
        t.anchors = {{1000, 1000, 5.0}};
        Mapping m = computeMapping(t);
        assert(near(m.offsetS, 4.0));              // video 5.0 at steady 1.0 s -> offset 4.0
        assert(!m.lagKnown);
    }
    {
        ObsTiming t;
        t.haveStart = true;
        t.startMs = 2500;
        Mapping m = computeMapping(t);
        assert(m.method == Mapping::Method::Start);
        assert(near(m.offsetS, -2.5));
        assert(computeMapping(ObsTiming{}).method == Mapping::Method::None);
    }

    // The start wins over GetRecordStatus: outputDuration runs ~1 s behind the picture when OBS's audio
    // buffering is at 960 ms (session 2026-10-31 00-27-53: every attempt 1.02 s late in the video)
    {
        ObsTiming t;
        t.haveStart = true;
        t.startMs = 10000;
        for (int i = 1; i <= 10; ++i) {
            double at = 10000 + i * 1000.0;
            t.anchors.push_back({at - 0.1, at + 0.1, (at - 10000 - 1020) / 1000.0});
        }
        Mapping m = computeMapping(t);
        assert(m.method == Mapping::Method::Start);
        assert(m.lagKnown && near(m.lagMs, 1020.0, 1e-6));
        double v = 0;
        assert(m.videoAt(14917, v) && near(v, 4.917, 1e-9));
    }

    // Without the start the anchors are corrected by the time OBS took to flush on stop
    {
        ObsTiming t;
        t.anchors = {{20000, 20000, (20000 - 10000 - 1040) / 1000.0}};
        t.stopLagMs = 1040;
        Mapping m = computeMapping(t);
        assert(m.method == Mapping::Method::Anchor);
        double v = 0;
        assert(m.videoAt(15000, v) && near(v, 5.0, 1e-9));
        assert(m.lagKnown && near(m.lagMs, 1040.0));
    }

    // Paused spans are not in the video
    {
        ObsTiming t;
        t.haveStart = true;
        t.startMs = 1000;
        t.pauses = {{3000, 5000}, {8000, -1}};
        Mapping m = computeMapping(t);
        double v = 0;
        assert(m.videoAt(2000, v) && near(v, 1.0));
        assert(!m.videoAt(4000, v));
        assert(m.videoAt(6000, v) && near(v, 3.0));   // 5 s since start, 2 s of them paused
        assert(!m.videoAt(9000, v));                  // never resumed
        std::vector<Event> ev(1);
        ev[0].tsMs = 4000;
        assert(buildJson(ev, {}, m, "").find("\"video_s\": null") != std::string::npos);
        assert(buildJson(ev, {}, m, "").find("\"pauses\": 2") != std::string::npos);
    }

    // JSON output
    {
        std::vector<Event> ev(2);
        ev[0].tsMs = 3500; ev[0].levelTime = 1.0; ev[0].down = true; ev[0].level = "A \"q\""; ev[0].attempt = 1;
        ev[1].tsMs = 3600; ev[1].levelTime = 1.1; ev[1].down = false; ev[1].attempt = 1;
        std::vector<Attempt> at = {{3000, 1, "A", 7}};
        Mapping m;
        m.method = Mapping::Method::Anchor;
        m.offsetS = 2.0;
        m.anchors = 4;
        std::string j = buildJson(ev, at, m, "C:\\v\\out.mkv");
        assert(j.find("\"video_s\": 5.5000") != std::string::npos);
        assert(j.find("\"video_s\": 5.6000") != std::string::npos);
        assert(j.find("\"frame\": 241") != std::string::npos);
        assert(j.find("\"frame\": 265") != std::string::npos);
        assert(j.find("A \\\"q\\\"") != std::string::npos);
        assert(j.find("C:\\\\v\\\\out.mkv") != std::string::npos);

        std::string none = buildJson(ev, at, Mapping{}, "");
        assert(none.find("\"video_s\": null") != std::string::npos);
        assert(buildJson({}, {}, Mapping{}, "").find("\"events\": []") != std::string::npos);
    }

    // only sessions with video time are worth a file
    {
        Mapping none;
        Mapping start; start.method = Mapping::Method::Start;
        Mapping anchor; anchor.method = Mapping::Method::Anchor;
        assert(!shouldSave(none, false));
        assert(shouldSave(none, true));
        assert(shouldSave(start, false));
        assert(shouldSave(anchor, false));
    }

    // TPS from tick / level time, median, whole numbers snap
    {
        std::vector<Event> ev(4);
        for (auto& e : ev) { e.levelTime = 10.0; e.tick = 2400; }   // 240 tps
        ev[3].levelTime = 0.5; ev[3].tick = 100;                    // too early to count
        assert(near(estimateTps(ev, 123.0), 240.0, 1e-9));
        assert(near(estimateTps({}, 123.0), 123.0));
        std::vector<Event> fast(2);
        for (auto& e : fast) { e.levelTime = 2.0; e.tick = 960; }   // 480 tps
        assert(near(estimateTps(fast, 240.0), 480.0, 1e-9));
        std::vector<Event> noisy(1);
        noisy[0].levelTime = 4.0; noisy[0].tick = 960;              // 240.0 exactly
        assert(near(estimateTps(noisy, 0.0), 240.0));
        std::vector<Event> odd(1);
        odd[0].levelTime = 10.0; odd[0].tick = 5995;                // 599.5 stays
        assert(near(estimateTps(odd, 0.0), 599.5, 1e-9));
    }

    // frame = xdBot numbering: floor(level time * xdBot TPS) + 1 - frame offset, whatever TPS the game ran at;
    // the tick and the measured tps are written out
    {
        std::vector<Event> ev(2);
        ev[0].tsMs = 1000; ev[0].levelTime = 2.0; ev[0].tick = 960; ev[0].frameOffset = 0; ev[0].down = true;
        ev[1].tsMs = 1100; ev[1].levelTime = 2.1; ev[1].tick = 1008; ev[1].frameOffset = 2; ev[1].down = false;
        Mapping m;
        m.method = Mapping::Method::Start;
        std::string j = buildJson(ev, {}, m, "");
        assert(j.find("\"frame\": 481") != std::string::npos);
        assert(j.find("\"frame\": 503") != std::string::npos);
        assert(j.find("\"tick\": 960") != std::string::npos);
        assert(j.find("\"tps\": 480.0") != std::string::npos);
    }

    // Rela V.gdr (xdBot, 240 TPS macro) played with the game at 480 TPS: steps 62 / 182 / 900 are macro
    // frames 32 / 92 / 451
    {
        assert(xdbotFrame(62 / 480.0, 240.0, 0) == 32);
        assert(xdbotFrame(182 / 480.0, 240.0, 0) == 92);
        assert(xdbotFrame(900 / 480.0, 240.0, 0) == 451);
        assert(xdbotFrame(0.0, 240.0, 0) == 1);
        assert(xdbotFrame(0.0, 240.0, 5) == 0);       // xdBot never goes below 0
        assert(xdbotFrame(1.0, 360.0, 0) == 361);
    }

    std::puts("core tests passed");
    return 0;
}
