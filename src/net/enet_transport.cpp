#include "net/enet_transport.h"

#include <chrono>

#include <enet/enet.h>

namespace {

std::string AddressToString(const ENetAddress& address) {
    char ip[64] = {};
    enet_address_get_host_ip(&address, ip, sizeof(ip));
    return std::string(ip);
}

uint32_t FallbackTimeMilliseconds() {
    static const auto start = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count());
}

}  // namespace

ENetTransport::ENetTransport() = default;

ENetTransport::~ENetTransport() { Stop(); }

bool ENetTransport::StartHost(int port) {
    Stop();
    if (!EnsureEnetInitialized()) {
        last_error_ = "enet initialization failed";
        return false;
    }

    ENetAddress address;
    address.host = ENET_HOST_ANY;
    address.port = static_cast<enet_uint16>(port);

    host_ = enet_host_create(&address, 16, 2, 0, 0);
    if (host_ == nullptr) {
        last_error_ = "failed to bind/listen on UDP port (in use or blocked)";
        return false;
    }

    is_host_ = true;
    last_error_.clear();
    return true;
}

bool ENetTransport::StartClient() {
    Stop();
    if (!EnsureEnetInitialized()) {
        last_error_ = "enet initialization failed";
        return false;
    }

    host_ = enet_host_create(nullptr, 1, 2, 0, 0);
    if (host_ == nullptr) {
        last_error_ = "failed to create ENet client host";
        return false;
    }

    is_host_ = false;
    last_error_.clear();
    return true;
}

bool ENetTransport::ConnectToHost(const std::string& ip, int port) {
    if (host_ == nullptr || is_host_) {
        last_error_ = "client transport is not running";
        return false;
    }

    ENetAddress address;
    address.port = static_cast<enet_uint16>(port);
    if (enet_address_set_host(&address, ip.c_str()) != 0) {
        last_error_ = "failed to resolve host address";
        return false;
    }

    ENetPeer* peer = enet_host_connect(host_, &address, 2, 0);
    if (peer == nullptr) {
        last_error_ = "connect start failed";
        return false;
    }

    RegisterPeer(peer);
    last_error_.clear();
    return true;
}

void ENetTransport::Disconnect(NetworkPeerId peer_id) {
    ENetPeer* peer = FindPeer(peer_id);
    if (peer == nullptr) {
        return;
    }

    enet_peer_disconnect(peer, 0);
    if (host_ != nullptr) {
        enet_host_flush(host_);
    }
}

void ENetTransport::Stop() {
    if (host_ != nullptr) {
        enet_host_destroy(host_);
        host_ = nullptr;
    }

    peers_by_id_.clear();
    peer_ids_.clear();
    next_peer_id_ = 1;
    is_host_ = false;

    if (enet_initialized_) {
        enet_deinitialize();
        enet_initialized_ = false;
    }
}

bool ENetTransport::PollEvent(NetworkEvent* out_event) {
    if (host_ == nullptr || out_event == nullptr) {
        return false;
    }

    ENetEvent event;
    if (enet_host_service(host_, &event, 0) <= 0) {
        return false;
    }

    switch (event.type) {
        case ENET_EVENT_TYPE_CONNECT: {
            out_event->type = NetworkEventType::Connected;
            out_event->peer_id = RegisterPeer(event.peer);
            out_event->channel = 0;
            out_event->port = event.peer->address.port;
            out_event->address = AddressToString(event.peer->address);
            out_event->payload.clear();
            return true;
        }
        case ENET_EVENT_TYPE_RECEIVE: {
            out_event->type = NetworkEventType::Received;
            out_event->peer_id = RegisterPeer(event.peer);
            out_event->channel = event.channelID;
            out_event->port = event.peer->address.port;
            out_event->address = AddressToString(event.peer->address);
            out_event->payload.assign(event.packet->data, event.packet->data + event.packet->dataLength);
            enet_packet_destroy(event.packet);
            return true;
        }
        case ENET_EVENT_TYPE_DISCONNECT: {
            out_event->type = NetworkEventType::Disconnected;
            out_event->peer_id = RegisterPeer(event.peer);
            out_event->channel = 0;
            out_event->port = event.peer->address.port;
            out_event->address = AddressToString(event.peer->address);
            out_event->payload.clear();
            UnregisterPeer(event.peer);
            return true;
        }
        case ENET_EVENT_TYPE_NONE:
            break;
    }

    return false;
}

bool ENetTransport::SendPacket(NetworkPeerId peer_id, const std::vector<uint8_t>& packet_data,
                               NetworkPacketReliability reliability, uint8_t channel) {
    ENetPeer* peer = FindPeer(peer_id);
    if (peer == nullptr) {
        return false;
    }

    ENetPacket* packet = enet_packet_create(packet_data.data(), packet_data.size(),
                                            reliability == NetworkPacketReliability::Reliable
                                                ? ENET_PACKET_FLAG_RELIABLE
                                                : 0);
    if (packet == nullptr) {
        return false;
    }

    enet_peer_send(peer, channel, packet);
    if (host_ != nullptr) {
        enet_host_flush(host_);
    }
    return true;
}

bool ENetTransport::BroadcastPacket(const std::vector<uint8_t>& packet_data, NetworkPacketReliability reliability,
                                    uint8_t channel) {
    if (host_ == nullptr) {
        return false;
    }

    ENetPacket* packet = enet_packet_create(packet_data.data(), packet_data.size(),
                                            reliability == NetworkPacketReliability::Reliable
                                                ? ENET_PACKET_FLAG_RELIABLE
                                                : 0);
    if (packet == nullptr) {
        return false;
    }

    enet_host_broadcast(host_, channel, packet);
    enet_host_flush(host_);
    return true;
}

bool ENetTransport::IsHost() const { return is_host_; }

bool ENetTransport::IsRunning() const { return host_ != nullptr; }

uint32_t ENetTransport::GetTimeMilliseconds() const {
    return enet_initialized_ ? enet_time_get() : FallbackTimeMilliseconds();
}

const std::string& ENetTransport::GetLastError() const { return last_error_; }

bool ENetTransport::EnsureEnetInitialized() {
    if (enet_initialized_) {
        return true;
    }

    if (enet_initialize() != 0) {
        return false;
    }

    enet_initialized_ = true;
    return true;
}

NetworkPeerId ENetTransport::RegisterPeer(ENetPeer* peer) {
    if (peer == nullptr) {
        return InvalidPeerId();
    }

    const auto existing = peer_ids_.find(peer);
    if (existing != peer_ids_.end()) {
        return existing->second;
    }

    const NetworkPeerId peer_id = next_peer_id_++;
    peer_ids_[peer] = peer_id;
    peers_by_id_[peer_id] = peer;
    return peer_id;
}

void ENetTransport::UnregisterPeer(ENetPeer* peer) {
    const auto it = peer_ids_.find(peer);
    if (it == peer_ids_.end()) {
        return;
    }

    peers_by_id_.erase(it->second);
    peer_ids_.erase(it);
}

ENetPeer* ENetTransport::FindPeer(NetworkPeerId peer_id) const {
    const auto it = peers_by_id_.find(peer_id);
    return it != peers_by_id_.end() ? it->second : nullptr;
}

NetworkPeerId ENetTransport::InvalidPeerId() { return 0; }
