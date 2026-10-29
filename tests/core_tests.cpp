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
        Mapping m = computeMapping(a, false, 0);
        assert(m.method == Mapping::Method::Anchor);
        assert(m.anchors == 3);
        assert(near(m.bestRttMs, 8.0));
    }
    {
        std::vector<Anchor> a = {{1000, 1000, 5.0}};
        Mapping m = computeMapping(a, false, 0);
        assert(near(m.offsetS, 4.0));              // video 5.0 at steady 1.0 s -> offset 4.0
    }
    {
        Mapping m = computeMapping({}, true, 2500);
        assert(m.method == Mapping::Method::Start);
        assert(near(m.offsetS, -2.5));
        assert(computeMapping({}, false, 0).method == Mapping::Method::None);
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
        assert(j.find("\"frame\": 240") != std::string::npos);
        assert(j.find("\"frame\": 264") != std::string::npos);
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

    // frame = xdBot numbering: tick + 1 - frame offset; the tick and the measured tps are written out
    {
        std::vector<Event> ev(2);
        ev[0].tsMs = 1000; ev[0].levelTime = 2.0; ev[0].tick = 960; ev[0].frameOffset = 0; ev[0].down = true;
        ev[1].tsMs = 1100; ev[1].levelTime = 2.1; ev[1].tick = 1008; ev[1].frameOffset = 2; ev[1].down = false;
        Mapping m;
        m.method = Mapping::Method::Start;
        std::string j = buildJson(ev, {}, m, "");
        assert(j.find("\"frame\": 961") != std::string::npos);
        assert(j.find("\"frame\": 1007") != std::string::npos);
        assert(j.find("\"tick\": 960") != std::string::npos);
        assert(j.find("\"tps\": 480.0") != std::string::npos);
    }

    std::puts("core tests passed");
    return 0;
}
