#pragma once
// UDPTransport.h
// UDP socket implementing ITransport for the AirLift WiFi path.
//
// The Teensy streams the same binary packet format over UDP that it normally
// sends over serial. The PC listens on a fixed port; the Teensy sends datagrams
// to the PC's IP (or broadcast). Commands back to the Teensy go to its IP:port.
//
// Protocol note: UDP is connectionless, so "connect" here means "bind the
// receive socket and record the Teensy's address for transmit". The firmware
// must be told the PC's IP out-of-band (via serial or hardcoded) or use
// UDP broadcast for discovery.
//
// Requires: Winsock2 on Windows, POSIX sockets on Linux/macOS.

#include "ITransport.h"

#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "Ws2_32.lib")
  using sock_t = SOCKET;
  static constexpr sock_t INVALID_SOCK = INVALID_SOCKET;
  static void close_sock(sock_t s) { closesocket(s); }
#else
  #include <arpa/inet.h>
  #include <fcntl.h>
  #include <netinet/in.h>
  #include <sys/select.h>
  #include <sys/socket.h>
  #include <unistd.h>
  using sock_t = int;
  static constexpr sock_t INVALID_SOCK = -1;
  static void close_sock(sock_t s) { ::close(s); }
#endif

class UDPTransport : public ITransport {
public:
    // listen_port  : UDP port this PC listens on for Teensy packets
    // teensy_ip    : IP address of the Teensy (or "" for receive-only / broadcast TX)
    // teensy_port  : UDP port the Teensy listens on for commands
    UDPTransport(uint16_t listen_port  = 5005,
                 const std::string& teensy_ip   = "",
                 uint16_t teensy_port = 5006)
        : listen_port_(listen_port),
          teensy_ip_(teensy_ip),
          teensy_port_(teensy_port),
          rx_sock_(INVALID_SOCK),
          tx_sock_(INVALID_SOCK) {}

    ~UDPTransport() override { disconnect(); }

    bool connect() override {
#ifdef _WIN32
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) return false;
#endif
        // RX socket: bind to listen_port
        rx_sock_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (rx_sock_ == INVALID_SOCK) return false;

        // Allow reuse so restarting the app doesn't hit EADDRINUSE for a few seconds
        int reuse = 1;
#ifdef _WIN32
        setsockopt(rx_sock_, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
#else
        setsockopt(rx_sock_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port        = htons(listen_port_);
        if (::bind(rx_sock_, (sockaddr*)&addr, sizeof(addr)) != 0) {
            close_sock(rx_sock_); rx_sock_ = INVALID_SOCK; return false;
        }

        // TX socket (for sending commands to Teensy)
        if (!teensy_ip_.empty()) {
            tx_sock_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (tx_sock_ == INVALID_SOCK) {
                close_sock(rx_sock_); rx_sock_ = INVALID_SOCK; return false;
            }
            teensy_addr_.sin_family = AF_INET;
            teensy_addr_.sin_port   = htons(teensy_port_);
#ifdef _WIN32
            InetPtonA(AF_INET, teensy_ip_.c_str(), &teensy_addr_.sin_addr);
#else
            inet_pton(AF_INET, teensy_ip_.c_str(), &teensy_addr_.sin_addr);
#endif
        }

        return true;
    }

    void disconnect() override {
        if (rx_sock_ != INVALID_SOCK) { close_sock(rx_sock_); rx_sock_ = INVALID_SOCK; }
        if (tx_sock_ != INVALID_SOCK) { close_sock(tx_sock_); tx_sock_ = INVALID_SOCK; }
#ifdef _WIN32
        WSACleanup();
#endif
    }

    // Send command bytes to Teensy
    int writeBytes(const char* data, int len) override {
        if (tx_sock_ == INVALID_SOCK || teensy_ip_.empty()) return -1;
        return (int)::sendto(tx_sock_, data, len, 0,
                             (sockaddr*)&teensy_addr_, sizeof(teensy_addr_));
    }

    // Read up to `len` bytes from the RX socket within `timeout_ms`.
    // For UDP we use select() so we don't block the reader thread indefinitely.
    int readBytes(char* buf, int len, unsigned int timeout_ms) override {
        if (rx_sock_ == INVALID_SOCK) return 0;

        fd_set fds; FD_ZERO(&fds); FD_SET(rx_sock_, &fds);
        timeval tv;
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

#ifdef _WIN32
        int r = ::select(0, &fds, nullptr, nullptr, &tv);
#else
        int r = ::select((int)rx_sock_ + 1, &fds, nullptr, nullptr, &tv);
#endif
        if (r <= 0) return 0;

        sockaddr_in from{};
        socklen_t from_len = sizeof(from);
        int received = (int)::recvfrom(rx_sock_, buf, len, 0,
                                       (sockaddr*)&from, &from_len);

        // If Teensy IP was not specified, learn it from first packet
        if (received > 0 && teensy_ip_.empty() && tx_sock_ != INVALID_SOCK) {
            teensy_addr_ = from;
            teensy_addr_.sin_port = htons(teensy_port_);
        }

        return received > 0 ? received : 0;
    }

    void flushReceiver() override {
        // Drain any queued datagrams non-blockingly
        if (rx_sock_ == INVALID_SOCK) return;
        char discard[1024];
        for (int i = 0; i < 64; ++i) {
            fd_set fds; FD_ZERO(&fds); FD_SET(rx_sock_, &fds);
            timeval tv{}; // zero timeout = poll
#ifdef _WIN32
            if (::select(0, &fds, nullptr, nullptr, &tv) <= 0) break;
#else
            if (::select((int)rx_sock_ + 1, &fds, nullptr, nullptr, &tv) <= 0) break;
#endif
            ::recvfrom(rx_sock_, discard, sizeof(discard), 0, nullptr, nullptr);
        }
    }

    // Accessors for display in the UI
    uint16_t    listenPort()  const { return listen_port_; }
    std::string teensyIP()    const { return teensy_ip_; }
    uint16_t    teensyPort()  const { return teensy_port_; }

private:
    uint16_t    listen_port_;
    std::string teensy_ip_;
    uint16_t    teensy_port_;
    sock_t      rx_sock_;
    sock_t      tx_sock_;
    sockaddr_in teensy_addr_{};
};
