#include "net/network_manager.h"

#include <cstring>

#include <enet/enet.h>
#include <nlohmann/json.hpp>

namespace {

nlohmann::json WrapPacket(const std::string& type, const nlohmann::json& payload) {
    return {{"type", type}, {"payload", payload}};
}

}  // namespace

NetworkManager::NetworkManager() = default;

NetworkManager::~NetworkManager() { Stop(); }

bool NetworkManager::EnsureEnetInitialized() {
    if (enet_initialized_) {
        return true;
    }

    if (enet_initialize() != 0) {
        return false;
    }

    enet_initialized_ = true;
    return true;
}

bool NetworkManager::StartHost(int port) {
    Stop();
    if (!EnsureEnetInitialized()) {
        return false;
    }

    ENetAddress address;
    address.host = ENET_HOST_ANY;
    address.port = static_cast<enet_uint16>(port);

    host_ = enet_host_create(&address, 16, 2, 0, 0);
    if (!host_) {
        return false;
    }

    is_host_ = true;
    connected_ = true;
    assigned_local_player_id_ = 0;
    return true;
}

bool NetworkManager::StartClient() {
    Stop();
    if (!EnsureEnetInitialized()) {
        return false;
    }

    host_ = enet_host_create(nullptr, 1, 2, 0, 0);
    if (!host_) {
        return false;
    }

    is_host_ = false;
    connected_ = false;
    return true;
}

bool NetworkManager::ConnectToHost(const std::string& ip, int port) {
    if (!host_ || is_host_) {
        return false;
    }

    ENetAddress address;
    address.port = static_cast<enet_uint16>(port);
    if (enet_address_set_host(&address, ip.c_str()) != 0) {
        return false;
    }

    server_peer_ = enet_host_connect(host_, &address, 2, 0);
    return server_peer_ != nullptr;
}

void NetworkManager::Stop() {
    if (host_) {
        enet_host_destroy(host_);
        host_ = nullptr;
    }

    server_peer_ = nullptr;
    peers_.clear();
    pending_host_inputs_.clear();
    latest_snapshot_.reset();
    latest_lobby_state_.reset();
    pending_match_start_ = false;

    is_host_ = false;
    connected_ = false;
    assigned_local_player_id_ = -1;
    next_remote_player_id_ = 1;

    if (enet_initialized_) {
        enet_deinitialize();
        enet_initialized_ = false;
    }
}

void NetworkManager::SetLocalPlayerName(const std::string& player_name) { local_player_name_ = player_name; }

void NetworkManager::Poll() {
    if (!host_) {
        return;
    }

    ENetEvent event;
    while (enet_host_service(host_, &event, 0) > 0) {
        switch (event.type) {
            case ENET_EVENT_TYPE_CONNECT: {
                if (is_host_) {
                    PeerInfo info;
                    info.player_id = next_remote_player_id_++;
                    info.name = "Player" + std::to_string(info.player_id);
                    peers_[event.peer] = info;
                } else {
                    connected_ = true;
                    nlohmann::json join_payload = {{"name", local_player_name_}};
                    SendJsonToPeer(server_peer_, WrapPacket("join", join_payload), true);
                }
                break;
            }
            case ENET_EVENT_TYPE_RECEIVE: {
                std::string raw(reinterpret_cast<const char*>(event.packet->data), event.packet->dataLength);
                nlohmann::json json = nlohmann::json::parse(raw, nullptr, false);
                if (!json.is_object()) {
                    enet_packet_destroy(event.packet);
                    break;
                }

                const std::string type = json.value("type", std::string{});
                const nlohmann::json payload = json.value("payload", nlohmann::json::object());

                if (is_host_) {
                    if (type == "join") {
                        const auto it = peers_.find(event.peer);
                        if (it != peers_.end()) {
                            it->second.name = payload.value("name", it->second.name);
                            nlohmann::json ack_payload = {{"player_id", it->second.player_id}};
                            SendJsonToPeer(event.peer, WrapPacket("join_ack", ack_payload), true);
                        }
                    } else if (type == "client_input") {
                        auto input = ClientInputFromJson(payload);
                        if (input.has_value()) {
                            pending_host_inputs_.push_back(*input);
                        }
                    }
                } else {
                    if (type == "join_ack") {
                        assigned_local_player_id_ = payload.value("player_id", -1);
                    } else if (type == "snapshot") {
                        latest_snapshot_ = ServerSnapshotFromJson(payload);
                    } else if (type == "lobby_state") {
                        latest_lobby_state_ = LobbyStateFromJson(payload);
                    } else if (type == "match_start") {
                        pending_match_start_ = true;
                    }
                }

                enet_packet_destroy(event.packet);
                break;
            }
            case ENET_EVENT_TYPE_DISCONNECT: {
                if (is_host_) {
                    peers_.erase(event.peer);
                } else {
                    connected_ = false;
                    server_peer_ = nullptr;
                }
                break;
            }
            case ENET_EVENT_TYPE_NONE:
                break;
        }
    }
}

bool NetworkManager::IsHost() const { return is_host_; }

bool NetworkManager::IsConnected() const {
    if (is_host_) {
        return host_ != nullptr;
    }
    return connected_;
}

void NetworkManager::SendClientInput(const ClientInputMessage& message) {
    if (is_host_ || !server_peer_) {
        return;
    }
    // Reliable for v1 so one-shot actions (key presses/clicks) are not dropped.
    SendJsonToPeer(server_peer_, WrapPacket("client_input", ToJson(message)), true);
}

std::vector<ClientInputMessage> NetworkManager::ConsumeHostInputs() {
    std::vector<ClientInputMessage> out;
    out.swap(pending_host_inputs_);
    return out;
}

void NetworkManager::BroadcastSnapshot(const ServerSnapshotMessage& message) {
    if (!is_host_) {
        return;
    }
    BroadcastJson(WrapPacket("snapshot", ToJson(message)), false);
}

std::optional<ServerSnapshotMessage> NetworkManager::ConsumeLatestSnapshot() {
    auto out = latest_snapshot_;
    latest_snapshot_.reset();
    return out;
}

void NetworkManager::BroadcastLobbyState(const LobbyStateMessage& message) {
    if (!is_host_) {
        return;
    }
    BroadcastJson(WrapPacket("lobby_state", ToJson(message)), true);
}

std::optional<LobbyStateMessage> NetworkManager::ConsumeLobbyState() {
    auto out = latest_lobby_state_;
    latest_lobby_state_.reset();
    return out;
}

void NetworkManager::BroadcastMatchStart(const MatchStartMessage& message) {
    if (!is_host_) {
        return;
    }
    BroadcastJson(WrapPacket("match_start", ToJson(message)), true);
}

bool NetworkManager::ConsumeMatchStart() {
    const bool out = pending_match_start_;
    pending_match_start_ = false;
    return out;
}

int NetworkManager::GetAssignedLocalPlayerId() const { return assigned_local_player_id_; }

std::vector<RemotePlayerInfo> NetworkManager::GetRemotePlayers() const {
    std::vector<RemotePlayerInfo> result;
    for (const auto& [_, peer] : peers_) {
        result.push_back({peer.player_id, peer.name});
    }
    return result;
}

void NetworkManager::SendJsonToPeer(ENetPeer* peer, const nlohmann::json& json, bool reliable) {
    if (!peer) {
        return;
    }

    const std::string payload = json.dump();
    ENetPacket* packet =
        enet_packet_create(payload.data(), payload.size(), reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
    if (packet) {
        enet_peer_send(peer, 0, packet);
        enet_host_flush(host_);
    }
}

void NetworkManager::BroadcastJson(const nlohmann::json& json, bool reliable) {
    if (!host_) {
        return;
    }

    const std::string payload = json.dump();
    ENetPacket* packet =
        enet_packet_create(payload.data(), payload.size(), reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
    if (packet) {
        enet_host_broadcast(host_, 0, packet);
        enet_host_flush(host_);
    }
}
