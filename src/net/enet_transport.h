#pragma once

#include <string>
#include <unordered_map>

#include "net/network_transport.h"

struct _ENetHost;
struct _ENetPeer;
typedef struct _ENetHost ENetHost;
typedef struct _ENetPeer ENetPeer;

class ENetTransport : public INetworkTransport {
  public:
    ENetTransport();
    ~ENetTransport() override;

    bool StartHost(int port) override;
    bool StartClient() override;
    bool ConnectToHost(const std::string& ip, int port) override;
    void Disconnect(NetworkPeerId peer_id) override;
    void Stop() override;

    bool PollEvent(NetworkEvent* out_event) override;
    bool SendPacket(NetworkPeerId peer_id, const std::vector<uint8_t>& packet_data,
                    NetworkPacketReliability reliability, uint8_t channel) override;
    bool BroadcastPacket(const std::vector<uint8_t>& packet_data, NetworkPacketReliability reliability,
                         uint8_t channel) override;

    bool IsHost() const override;
    bool IsRunning() const override;
    uint32_t GetTimeMilliseconds() const override;
    const std::string& GetLastError() const override;

  private:
    bool EnsureEnetInitialized();
    NetworkPeerId RegisterPeer(ENetPeer* peer);
    void UnregisterPeer(ENetPeer* peer);
    ENetPeer* FindPeer(NetworkPeerId peer_id) const;
    static NetworkPeerId InvalidPeerId();

    bool enet_initialized_ = false;
    bool is_host_ = false;
    ENetHost* host_ = nullptr;
    std::string last_error_;
    NetworkPeerId next_peer_id_ = 1;
    std::unordered_map<NetworkPeerId, ENetPeer*> peers_by_id_;
    std::unordered_map<ENetPeer*, NetworkPeerId> peer_ids_;
};
