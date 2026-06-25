#pragma once

#include <string>
#include <vector>

struct DiscoveredHost {
    std::string name;
    std::string ip;
    int port = 0;
    double last_seen_seconds = 0.0;
};

class IDiscoveryService {
  public:
    virtual ~IDiscoveryService() = default;

    // Future discovery backends such as Steam lobbies should plug in behind this interface.
    virtual bool StartHostBroadcaster(const std::string& host_name, int game_port) = 0;
    virtual bool StartClientListener() = 0;
    virtual void Stop() = 0;

    virtual void Update() = 0;
    virtual std::vector<DiscoveredHost> GetHosts() const = 0;
    virtual std::string GetHostLocalIp() const = 0;
};
