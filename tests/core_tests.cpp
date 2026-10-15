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

    std::puts("core tests passed");
    return 0;
}
