#pragma once

#include <string>
#include <unordered_map>
#include <vector>

struct DiscoveredHost {
    std::string name;
    std::string ip;
    int port = 0;
    double last_seen_seconds = 0.0;
};

class LanDiscovery {
  public:
    LanDiscovery();
    ~LanDiscovery();

    bool StartHostBroadcaster(const std::string& host_name, int game_port);
    bool StartClientListener();
    void Stop();

    void Update();
    std::vector<DiscoveredHost> GetHosts() const;

  private:
    bool EnsureSocketApiInitialized();
    static std::string EndpointToIpString(const struct sockaddr_in& endpoint);

    bool socket_api_initialized_ = false;

    int broadcaster_socket_ = -1;
    int listener_socket_ = -1;

    std::string host_name_;
    int host_port_ = 0;
    double next_broadcast_time_seconds_ = 0.0;

    std::unordered_map<std::string, DiscoveredHost> discovered_hosts_;
};
