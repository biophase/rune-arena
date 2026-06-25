#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "net/discovery_service.h"

class LanDiscovery : public IDiscoveryService {
  public:
    LanDiscovery();
    ~LanDiscovery() override;

    bool StartHostBroadcaster(const std::string& host_name, int game_port) override;
    bool StartClientListener() override;
    void Stop() override;

    void Update() override;
    std::vector<DiscoveredHost> GetHosts() const override;
    std::string GetHostLocalIp() const override;

  private:
    bool EnsureSocketApiInitialized();
    static std::string EndpointToIpString(const struct sockaddr_in& endpoint);

    bool socket_api_initialized_ = false;

    int broadcaster_socket_ = -1;
    int listener_socket_ = -1;

    std::string host_name_;
    std::string host_local_ip_ = "127.0.0.1";
    int host_port_ = 0;
    double next_broadcast_time_seconds_ = 0.0;

    std::unordered_map<std::string, DiscoveredHost> discovered_hosts_;
};
