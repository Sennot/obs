#pragma once

// Minimal blocking RFC 6455 WebSocket client (ws://, Windows/Winsock) used for obs-websocket.
// Runs its own thread; all callbacks are invoked on that thread. No Geode dependency.

#include <atomic>
#include <functional>
#include <mutex>
#include <string>

namespace clicksync {

class WsClient {
public:
    struct Endpoint {
        std::string host;
        int port = 4455;
    };

    std::function<Endpoint()> endpoint;                      // queried before every connection attempt
    std::function<void()> onOpen;                            // after the HTTP upgrade succeeded
    std::function<void()> onClose;                           // connection lost / closed
    std::function<void(std::string const&)> onMessage;       // one complete text message
    std::function<void()> onTick;                            // every ~100 ms while connected

    void start();
    bool send(std::string const& text);
    bool connected() const { return m_connected.load(); }
    int closeCode() const { return m_closeCode.load(); }   // last close code sent by the server, 0 = none

private:
    void run();
    bool connectAndUpgrade(Endpoint const& ep);
    int recvMessage(std::string& out);                       // 1 message, 0 nothing yet, -1 error
    int recvExact(unsigned char* buf, size_t n, bool allowTimeoutAtStart);
    bool sendFrame(unsigned char opcode, std::string const& payload);
    void closeSocket();

    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_started{false};
    std::atomic<int> m_closeCode{0};
    std::mutex m_sendMutex;
    unsigned long long m_sock = ~0ull;  // SOCKET
};

}  // namespace clicksync
