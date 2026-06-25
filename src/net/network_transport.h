#pragma once

#include <cstdint>
#include <string>
#include <vector>

using NetworkPeerId = uint64_t;

enum class NetworkPacketReliability {
    Reliable,
    Unreliable,
};

enum class NetworkEventType {
    Connected,
    Received,
    Disconnected,
};

struct NetworkEvent {
    NetworkEventType type = NetworkEventType::Disconnected;
    NetworkPeerId peer_id = 0;
    uint8_t channel = 0;
    uint16_t port = 0;
    std::string address;
    std::vector<uint8_t> payload;
};

class INetworkTransport {
  public:
    virtual ~INetworkTransport() = default;

    // Future transports such as SteamTransport should implement this same game-oriented surface.
    virtual bool StartHost(int port) = 0;
    virtual bool StartClient() = 0;
    virtual bool ConnectToHost(const std::string& ip, int port) = 0;
    virtual void Disconnect(NetworkPeerId peer_id) = 0;
    virtual void Stop() = 0;

    virtual bool PollEvent(NetworkEvent* out_event) = 0;
    virtual bool SendPacket(NetworkPeerId peer_id, const std::vector<uint8_t>& packet_data,
                            NetworkPacketReliability reliability, uint8_t channel) = 0;
    virtual bool BroadcastPacket(const std::vector<uint8_t>& packet_data, NetworkPacketReliability reliability,
                                 uint8_t channel) = 0;

    virtual bool IsHost() const = 0;
    virtual bool IsRunning() const = 0;
    virtual uint32_t GetTimeMilliseconds() const = 0;
    virtual const std::string& GetLastError() const = 0;
};
