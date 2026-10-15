#include "ws_client.hpp"

#include "crypto.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <chrono>
#include <cstdint>
#include <random>
#include <thread>

#pragma comment(lib, "ws2_32.lib")

namespace clicksync {

namespace {

constexpr uint64_t kMaxMessage = 16ull * 1024 * 1024;

SOCKET toSocket(unsigned long long s) { return static_cast<SOCKET>(s); }

}  // namespace

void WsClient::start() {
    if (m_started.exchange(true)) return;
    std::thread([this] { run(); }).detach();
}

void WsClient::closeSocket() {
    std::lock_guard lock(m_sendMutex);
    if (m_sock != ~0ull) {
        closesocket(toSocket(m_sock));
        m_sock = ~0ull;
    }
}

bool WsClient::connectAndUpgrade(Endpoint const& ep) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(ep.host.c_str(), std::to_string(ep.port).c_str(), &hints, &res) != 0 || !res) return false;

    SOCKET s = INVALID_SOCKET;
    for (addrinfo* p = res; p; p = p->ai_next) {
        s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        if (connect(s, p->ai_addr, static_cast<int>(p->ai_addrlen)) == 0) break;
        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if (s == INVALID_SOCKET) return false;

    BOOL nodelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char const*>(&nodelay), sizeof(nodelay));
    DWORD timeout = 5000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char const*>(&timeout), sizeof(timeout));
    {
        std::lock_guard lock(m_sendMutex);
        m_sock = static_cast<unsigned long long>(s);
    }

    std::random_device rd;
    unsigned char keyRaw[16];
    for (auto& b : keyRaw) b = static_cast<unsigned char>(rd());
    std::string key = base64(keyRaw, sizeof(keyRaw));

    std::string req = "GET / HTTP/1.1\r\nHost: " + ep.host + ":" + std::to_string(ep.port) +
                      "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
                      "\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Protocol: obswebsocket.json\r\n\r\n";
    if (::send(s, req.data(), static_cast<int>(req.size()), 0) != static_cast<int>(req.size())) {
        closeSocket();
        return false;
    }

    std::string head;
    char c;
    while (head.size() < 8192) {
        int r = recv(s, &c, 1, 0);
        if (r <= 0) {
            closeSocket();
            return false;
        }
        head.push_back(c);
        if (head.size() >= 4 && head.compare(head.size() - 4, 4, "\r\n\r\n") == 0) break;
    }
    if (head.find(" 101 ") == std::string::npos) {
        closeSocket();
        return false;
    }
    timeout = 100;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char const*>(&timeout), sizeof(timeout));
    return true;
}

int WsClient::recvExact(unsigned char* buf, size_t n, bool allowTimeoutAtStart) {
    size_t got = 0;
    while (got < n) {
        int r = recv(toSocket(m_sock), reinterpret_cast<char*>(buf + got), static_cast<int>(n - got), 0);
        if (r > 0) {
            got += static_cast<size_t>(r);
            continue;
        }
        if (r == 0) return -1;
        if (WSAGetLastError() == WSAETIMEDOUT) {
            if (got == 0 && allowTimeoutAtStart) return 0;
            continue;  // mid-frame: keep waiting
        }
        return -1;
    }
    return 1;
}

bool WsClient::sendFrame(unsigned char opcode, std::string const& payload) {
    std::string f;
    f.push_back(static_cast<char>(0x80 | opcode));
    size_t n = payload.size();
    if (n < 126) {
        f.push_back(static_cast<char>(0x80 | n));
    } else if (n < 65536) {
        f.push_back(static_cast<char>(0x80 | 126));
        f.push_back(static_cast<char>(n >> 8));
        f.push_back(static_cast<char>(n & 0xff));
    } else {
        f.push_back(static_cast<char>(0x80 | 127));
        for (int i = 7; i >= 0; --i) f.push_back(static_cast<char>((static_cast<uint64_t>(n) >> (8 * i)) & 0xff));
    }
    std::random_device rd;
    unsigned char mask[4];
    for (auto& b : mask) b = static_cast<unsigned char>(rd());
    for (auto b : mask) f.push_back(static_cast<char>(b));
    for (size_t i = 0; i < n; ++i) f.push_back(static_cast<char>(static_cast<unsigned char>(payload[i]) ^ mask[i & 3]));

    std::lock_guard lock(m_sendMutex);
    if (m_sock == ~0ull) return false;
    size_t sent = 0;
    while (sent < f.size()) {
        int r = ::send(toSocket(m_sock), f.data() + sent, static_cast<int>(f.size() - sent), 0);
        if (r <= 0) return false;
        sent += static_cast<size_t>(r);
    }
    return true;
}

bool WsClient::send(std::string const& text) { return m_connected.load() && sendFrame(0x1, text); }

int WsClient::recvMessage(std::string& out) {
    out.clear();
    bool fragmenting = false;
    for (;;) {
        unsigned char h[2];
        int r = recvExact(h, 2, !fragmenting);
        if (r <= 0) return r;
        bool fin = (h[0] & 0x80) != 0;
        int op = h[0] & 0x0f;
        bool masked = (h[1] & 0x80) != 0;
        uint64_t len = h[1] & 0x7f;
        if (len == 126) {
            unsigned char e[2];
            if (recvExact(e, 2, false) <= 0) return -1;
            len = (static_cast<uint64_t>(e[0]) << 8) | e[1];
        } else if (len == 127) {
            unsigned char e[8];
            if (recvExact(e, 8, false) <= 0) return -1;
            len = 0;
            for (int i = 0; i < 8; ++i) len = (len << 8) | e[i];
        }
        unsigned char mask[4] = {0, 0, 0, 0};
        if (masked && recvExact(mask, 4, false) <= 0) return -1;
        if (len > kMaxMessage) return -1;
        std::string payload(static_cast<size_t>(len), '\0');
        if (len && recvExact(reinterpret_cast<unsigned char*>(payload.data()), static_cast<size_t>(len), false) <= 0) return -1;
        if (masked)
            for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>(static_cast<unsigned char>(payload[i]) ^ mask[i & 3]);

        if (op == 0x8) {                                // close (first 2 bytes = status code)
            if (payload.size() >= 2)
                m_closeCode = (static_cast<unsigned char>(payload[0]) << 8) | static_cast<unsigned char>(payload[1]);
            return -1;
        }
        if (op == 0x9) {                                // ping -> pong
            sendFrame(0xA, payload);
            if (!fragmenting) return 0;
            continue;
        }
        if (op == 0xA) {                                // pong
            if (!fragmenting) return 0;
            continue;
        }
        out += payload;                                 // text / binary / continuation
        if (fin) return 1;
        fragmenting = true;
    }
}

void WsClient::run() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;
    for (;;) {
        Endpoint ep = endpoint ? endpoint() : Endpoint{"127.0.0.1", 4455};
        if (connectAndUpgrade(ep)) {
            m_closeCode = 0;
            m_connected = true;
            if (onOpen) onOpen();
            auto lastTick = std::chrono::steady_clock::now();
            for (;;) {
                std::string msg;
                int r = recvMessage(msg);
                if (r < 0) break;
                if (r > 0 && onMessage) onMessage(msg);
                auto now = std::chrono::steady_clock::now();
                if (now - lastTick >= std::chrono::milliseconds(100)) {
                    lastTick = now;
                    if (onTick) onTick();
                }
            }
            m_connected = false;
            closeSocket();
            if (onClose) onClose();
        }
        std::this_thread::sleep_for(std::chrono::seconds(3));
    }
}

}  // namespace clicksync
