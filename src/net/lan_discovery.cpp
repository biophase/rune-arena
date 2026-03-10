#include "net/lan_discovery.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using SocketLenType = int;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketLenType = socklen_t;
#endif

namespace {

constexpr const char* kDiscoveryMagic = "RUNE_ARENA";

double GetNowSeconds() {
    static const auto start = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(now - start).count();
}

void CloseSocketSafe(int& socket_fd) {
    if (socket_fd < 0) {
        return;
    }
#ifdef _WIN32
    closesocket(static_cast<SOCKET>(socket_fd));
#else
    close(socket_fd);
#endif
    socket_fd = -1;
}

void SetSocketNonBlocking(int socket_fd) {
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(static_cast<SOCKET>(socket_fd), FIONBIO, &mode);
#else
    const int current_flags = fcntl(socket_fd, F_GETFL, 0);
    fcntl(socket_fd, F_SETFL, current_flags | O_NONBLOCK);
#endif
}

}  // namespace

LanDiscovery::LanDiscovery() = default;

LanDiscovery::~LanDiscovery() { Stop(); }

bool LanDiscovery::EnsureSocketApiInitialized() {
    if (socket_api_initialized_) {
        return true;
    }

#ifdef _WIN32
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        return false;
    }
#endif

    socket_api_initialized_ = true;
    return true;
}

bool LanDiscovery::StartHostBroadcaster(const std::string& host_name, int game_port) {
    if (!EnsureSocketApiInitialized()) {
        return false;
    }

    CloseSocketSafe(broadcaster_socket_);

    broadcaster_socket_ = static_cast<int>(socket(AF_INET, SOCK_DGRAM, 0));
    if (broadcaster_socket_ < 0) {
        return false;
    }

    int broadcast_enabled = 1;
    setsockopt(broadcaster_socket_, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&broadcast_enabled),
               sizeof(broadcast_enabled));
    SetSocketNonBlocking(broadcaster_socket_);

    host_name_ = host_name;
    host_port_ = game_port;
    next_broadcast_time_seconds_ = 0.0;
    return true;
}

bool LanDiscovery::StartClientListener() {
    if (!EnsureSocketApiInitialized()) {
        return false;
    }

    CloseSocketSafe(listener_socket_);

    listener_socket_ = static_cast<int>(socket(AF_INET, SOCK_DGRAM, 0));
    if (listener_socket_ < 0) {
        return false;
    }

    int reuse = 1;
    setsockopt(listener_socket_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in bind_addr = {};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(7778);
    bind_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listener_socket_, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0) {
        CloseSocketSafe(listener_socket_);
        return false;
    }

    SetSocketNonBlocking(listener_socket_);
    return true;
}

void LanDiscovery::Stop() {
    CloseSocketSafe(broadcaster_socket_);
    CloseSocketSafe(listener_socket_);
    discovered_hosts_.clear();

#ifdef _WIN32
    if (socket_api_initialized_) {
        WSACleanup();
    }
#endif

    socket_api_initialized_ = false;
}

void LanDiscovery::Update() {
    const double now = GetNowSeconds();

    if (broadcaster_socket_ >= 0 && now >= next_broadcast_time_seconds_) {
        sockaddr_in target = {};
        target.sin_family = AF_INET;
        target.sin_port = htons(7778);
        target.sin_addr.s_addr = INADDR_BROADCAST;

        const std::string payload = std::string(kDiscoveryMagic) + "|" + host_name_ + "|" + std::to_string(host_port_);
        sendto(broadcaster_socket_, payload.data(), static_cast<int>(payload.size()), 0,
               reinterpret_cast<sockaddr*>(&target), sizeof(target));

        next_broadcast_time_seconds_ = now + 1.0;
    }

    if (listener_socket_ >= 0) {
        char buffer[512];
        sockaddr_in source = {};
        SocketLenType source_len = sizeof(source);

        while (true) {
            const int read_bytes = recvfrom(listener_socket_, buffer, sizeof(buffer) - 1, 0,
                                            reinterpret_cast<sockaddr*>(&source), &source_len);
            if (read_bytes <= 0) {
                break;
            }

            buffer[read_bytes] = '\0';
            const std::string packet(buffer);
            const size_t first = packet.find('|');
            const size_t second = packet.find('|', first == std::string::npos ? first : first + 1);
            if (first == std::string::npos || second == std::string::npos) {
                continue;
            }

            const std::string magic = packet.substr(0, first);
            if (magic != kDiscoveryMagic) {
                continue;
            }

            const std::string name = packet.substr(first + 1, second - first - 1);
            const std::string port_text = packet.substr(second + 1);
            const int port = atoi(port_text.c_str());

            DiscoveredHost host;
            host.name = name;
            host.ip = EndpointToIpString(source);
            host.port = port;
            host.last_seen_seconds = now;
            discovered_hosts_[host.ip] = host;
        }

        for (auto it = discovered_hosts_.begin(); it != discovered_hosts_.end();) {
            if (now - it->second.last_seen_seconds > 3.0) {
                it = discovered_hosts_.erase(it);
            } else {
                ++it;
            }
        }
    }
}

std::vector<DiscoveredHost> LanDiscovery::GetHosts() const {
    std::vector<DiscoveredHost> result;
    result.reserve(discovered_hosts_.size());
    for (const auto& [_, host] : discovered_hosts_) {
        result.push_back(host);
    }

    std::sort(result.begin(), result.end(), [](const DiscoveredHost& a, const DiscoveredHost& b) {
        return a.last_seen_seconds > b.last_seen_seconds;
    });
    return result;
}

std::string LanDiscovery::EndpointToIpString(const struct sockaddr_in& endpoint) {
    char buffer[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, reinterpret_cast<const void*>(&endpoint.sin_addr), buffer, sizeof(buffer));
    return buffer;
}
