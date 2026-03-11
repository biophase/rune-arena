#include "net/network_manager.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <enet/enet.h>
#include <nlohmann/json.hpp>

namespace {

constexpr enet_uint8 kChannelReliable = 0;
constexpr enet_uint8 kChannelRealtime = 1;

nlohmann::json WrapPacket(const std::string& type, const nlohmann::json& payload) {
    return {{"type", type}, {"payload", payload}};
}

void NetLog(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    std::printf("\n");
    va_end(args);
}

std::string AddressToString(const ENetAddress& address) {
    char ip[64] = {};
    enet_address_get_host_ip(&address, ip, sizeof(ip));
    return std::string(ip);
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
        last_debug_message_ = "enet initialization failed";
        NetLog("[NET] Host start failed: %s", last_debug_message_.c_str());
        return false;
    }

    ENetAddress address;
    address.host = ENET_HOST_ANY;
    address.port = static_cast<enet_uint16>(port);

    host_ = enet_host_create(&address, 16, 2, 0, 0);
    if (!host_) {
        last_debug_message_ = "failed to bind/listen on UDP port (in use or blocked)";
        NetLog("[NET] Host start failed on UDP %d: %s", port, last_debug_message_.c_str());
        return false;
    }

    is_host_ = true;
    connected_ = true;
    assigned_local_player_id_ = 0;
    client_connection_state_ = ClientConnectionState::Idle;
    last_debug_message_ = "host listening";
    NetLog("[NET] Host started on UDP port %d", port);
    return true;
}

bool NetworkManager::StartClient() {
    Stop();
    if (!EnsureEnetInitialized()) {
        last_debug_message_ = "enet initialization failed";
        NetLog("[NET] Client start failed: %s", last_debug_message_.c_str());
        return false;
    }

    host_ = enet_host_create(nullptr, 1, 2, 0, 0);
    if (!host_) {
        last_debug_message_ = "failed to create ENet client host";
        NetLog("[NET] Client start failed: %s", last_debug_message_.c_str());
        return false;
    }

    is_host_ = false;
    connected_ = false;
    client_connection_state_ = ClientConnectionState::Disconnected;
    last_debug_message_ = "client initialized";
    return true;
}

bool NetworkManager::ConnectToHost(const std::string& ip, int port) {
    if (!host_ || is_host_) {
        return false;
    }

    ENetAddress address;
    address.port = static_cast<enet_uint16>(port);
    if (enet_address_set_host(&address, ip.c_str()) != 0) {
        last_debug_message_ = "failed to resolve host address";
        client_connection_state_ = ClientConnectionState::Disconnected;
        return false;
    }

    assigned_local_player_id_ = -1;
    latest_lobby_state_.reset();
    client_received_lobby_state_ = false;
    pending_match_start_ = false;
    client_connection_state_ = ClientConnectionState::ConnectingTransport;
    last_debug_message_ = "connecting transport";

    server_peer_ = enet_host_connect(host_, &address, 2, 0);
    if (server_peer_) {
        NetLog("[NET] Client connecting to %s:%d", ip.c_str(), port);
    } else {
        NetLog("[NET] Client failed to start connect to %s:%d", ip.c_str(), port);
        client_connection_state_ = ClientConnectionState::Disconnected;
        last_debug_message_ = "connect start failed";
    }
    return server_peer_ != nullptr;
}

void NetworkManager::Stop() {
    if (host_) {
        enet_host_destroy(host_);
        host_ = nullptr;
    }

    server_peer_ = nullptr;
    peers_.clear();
    pending_host_moves_.clear();
    pending_host_actions_.clear();
    latest_snapshot_.reset();
    latest_lobby_state_.reset();
    pending_match_start_ = false;
    client_received_lobby_state_ = false;

    is_host_ = false;
    connected_ = false;
    assigned_local_player_id_ = -1;
    next_remote_player_id_ = 1;
    client_connection_state_ = ClientConnectionState::Idle;
    last_debug_message_ = "stopped";
    last_received_snapshot_tick_ = -1;
    rate_window_bytes_sent_ = 0;
    rate_window_bytes_received_ = 0;
    rate_window_packets_sent_ = 0;
    rate_window_packets_received_ = 0;
    rate_window_reconciliation_corrections_ = 0;
    last_rate_update_ms_ = enet_time_get();
    telemetry_ = NetTelemetry{};

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
                    NetLog("[NET] Host transport connect from %s:%u -> assigned temp id=%d",
                           AddressToString(event.peer->address).c_str(), event.peer->address.port, info.player_id);
                } else {
                    connected_ = true;
                    client_connection_state_ = ClientConnectionState::WaitingJoinAck;
                    last_debug_message_ = "transport connected, waiting join_ack";
                    nlohmann::json join_payload = {{"name", local_player_name_}};
                    SendJsonToPeer(server_peer_, WrapPacket("join", join_payload), true, kChannelReliable);
                    NetLog("[NET] Client transport connected, sent join name='%s'", local_player_name_.c_str());
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
                            SendJsonToPeer(event.peer, WrapPacket("join_ack", ack_payload), true, kChannelReliable);
                            NetLog("[NET] Host received join from '%s' (%s), sent join_ack player_id=%d",
                                   it->second.name.c_str(), AddressToString(event.peer->address).c_str(),
                                   it->second.player_id);
                        }
                    } else if (type == "client_move") {
                        auto move = ClientMoveFromJson(payload);
                        if (move.has_value()) {
                            pending_host_moves_.push_back(*move);
                        }
                    } else if (type == "client_action") {
                        auto action = ClientActionFromJson(payload);
                        if (action.has_value()) {
                            pending_host_actions_.push_back(*action);
                        }
                    }
                } else {
                    if (type == "join_ack") {
                        assigned_local_player_id_ = payload.value("player_id", -1);
                        client_connection_state_ =
                            client_received_lobby_state_ ? ClientConnectionState::ReadyInLobby
                                                         : ClientConnectionState::WaitingLobbyState;
                        last_debug_message_ =
                            client_received_lobby_state_ ? "join_ack + lobby_state received" : "join_ack received";
                        NetLog("[NET] Client received join_ack player_id=%d", assigned_local_player_id_);
                    } else if (type == "snapshot") {
                        latest_snapshot_ = ServerSnapshotFromJson(payload);
                        if (latest_snapshot_.has_value()) {
                            const int tick = latest_snapshot_->server_tick;
                            if (last_received_snapshot_tick_ >= 0 && tick > last_received_snapshot_tick_ + 1) {
                                telemetry_.dropped_snapshots_total +=
                                    static_cast<uint64_t>(tick - (last_received_snapshot_tick_ + 1));
                            }
                            if (tick > last_received_snapshot_tick_) {
                                last_received_snapshot_tick_ = tick;
                            }
                        }
                    } else if (type == "lobby_state") {
                        latest_lobby_state_ = LobbyStateFromJson(payload);
                        client_received_lobby_state_ = latest_lobby_state_.has_value();
                        if (assigned_local_player_id_ >= 0) {
                            client_connection_state_ = ClientConnectionState::ReadyInLobby;
                            last_debug_message_ = "lobby_state received";
                        } else if (connected_) {
                            client_connection_state_ = ClientConnectionState::WaitingJoinAck;
                            last_debug_message_ = "lobby_state received before join_ack";
                        }
                        if (latest_lobby_state_.has_value()) {
                            NetLog("[NET] Client received lobby_state with %zu players",
                                   latest_lobby_state_->players.size());
                        }
                    } else if (type == "match_start") {
                        pending_match_start_ = true;
                        NetLog("[NET] Client received match_start");
                    }
                }

                RegisterIncomingPacket(event.packet->dataLength, type == "snapshot");

                enet_packet_destroy(event.packet);
                break;
            }
            case ENET_EVENT_TYPE_DISCONNECT: {
                if (is_host_) {
                    NetLog("[NET] Host disconnect from %s:%u", AddressToString(event.peer->address).c_str(),
                           event.peer->address.port);
                    peers_.erase(event.peer);
                } else {
                    connected_ = false;
                    server_peer_ = nullptr;
                    client_connection_state_ = ClientConnectionState::Disconnected;
                    last_debug_message_ = "disconnected";
                    NetLog("[NET] Client disconnected from host");
                }
                break;
            }
            case ENET_EVENT_TYPE_NONE:
                break;
        }
    }
    UpdateRateTelemetry();
}

bool NetworkManager::IsHost() const { return is_host_; }

bool NetworkManager::IsConnected() const {
    if (is_host_) {
        return host_ != nullptr;
    }
    return connected_;
}

ClientConnectionState NetworkManager::GetClientConnectionState() const { return client_connection_state_; }

bool NetworkManager::HasReceivedJoinAck() const { return assigned_local_player_id_ >= 0; }

bool NetworkManager::HasReceivedLobbyState() const { return client_received_lobby_state_; }

const std::string& NetworkManager::GetLastDebugMessage() const { return last_debug_message_; }

void NetworkManager::SendClientMove(const ClientMoveMessage& message) {
    if (is_host_ || !server_peer_) {
        return;
    }
    SendJsonToPeer(server_peer_, WrapPacket("client_move", ToJson(message)), false, kChannelRealtime);
}

void NetworkManager::SendClientAction(const ClientActionMessage& message) {
    if (is_host_ || !server_peer_) {
        return;
    }
    SendJsonToPeer(server_peer_, WrapPacket("client_action", ToJson(message)), true, kChannelReliable);
}

std::vector<ClientMoveMessage> NetworkManager::ConsumeHostMoveInputs() {
    std::vector<ClientMoveMessage> out;
    out.swap(pending_host_moves_);
    return out;
}

std::vector<ClientActionMessage> NetworkManager::ConsumeHostActionInputs() {
    std::vector<ClientActionMessage> out;
    out.swap(pending_host_actions_);
    return out;
}

void NetworkManager::BroadcastSnapshot(const ServerSnapshotMessage& message) {
    if (!is_host_) {
        return;
    }
    BroadcastJson(WrapPacket("snapshot", ToJson(message)), false, kChannelRealtime);
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
    BroadcastJson(WrapPacket("lobby_state", ToJson(message)), true, kChannelReliable);
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
    BroadcastJson(WrapPacket("match_start", ToJson(message)), true, kChannelReliable);
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

const NetTelemetry& NetworkManager::GetTelemetry() const { return telemetry_; }

void NetworkManager::AddReconciliationCorrection() {
    telemetry_.reconciliation_corrections_total += 1;
    rate_window_reconciliation_corrections_ += 1;
}

void NetworkManager::SendJsonToPeer(ENetPeer* peer, const nlohmann::json& json, bool reliable, uint8_t channel) {
    if (!peer) {
        return;
    }

    const std::string payload = json.dump();
    ENetPacket* packet =
        enet_packet_create(payload.data(), payload.size(), reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
    if (packet) {
        enet_peer_send(peer, channel, packet);
        RegisterOutgoingPacket(payload.size(), false);
        enet_host_flush(host_);
    }
}

void NetworkManager::BroadcastJson(const nlohmann::json& json, bool reliable, uint8_t channel) {
    if (!host_) {
        return;
    }

    const std::string payload = json.dump();
    ENetPacket* packet =
        enet_packet_create(payload.data(), payload.size(), reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
    if (packet) {
        enet_host_broadcast(host_, channel, packet);
        RegisterOutgoingPacket(payload.size(), channel == kChannelRealtime && !reliable);
        enet_host_flush(host_);
    }
}

void NetworkManager::RegisterOutgoingPacket(size_t bytes, bool is_snapshot) {
    telemetry_.bytes_sent_total += static_cast<uint64_t>(bytes);
    telemetry_.packets_sent_total += 1;
    rate_window_bytes_sent_ += static_cast<uint64_t>(bytes);
    rate_window_packets_sent_ += 1;
    if (is_snapshot) {
        telemetry_.snapshot_bytes_sent_total += static_cast<uint64_t>(bytes);
        telemetry_.snapshots_sent_total += 1;
        telemetry_.average_snapshot_bytes_sent =
            static_cast<float>(telemetry_.snapshot_bytes_sent_total) /
            static_cast<float>(std::max<uint64_t>(1, telemetry_.snapshots_sent_total));
    }
}

void NetworkManager::RegisterIncomingPacket(size_t bytes, bool is_snapshot) {
    telemetry_.bytes_received_total += static_cast<uint64_t>(bytes);
    telemetry_.packets_received_total += 1;
    rate_window_bytes_received_ += static_cast<uint64_t>(bytes);
    rate_window_packets_received_ += 1;
    if (is_snapshot) {
        telemetry_.snapshot_bytes_received_total += static_cast<uint64_t>(bytes);
        telemetry_.snapshots_received_total += 1;
        telemetry_.average_snapshot_bytes_received =
            static_cast<float>(telemetry_.snapshot_bytes_received_total) /
            static_cast<float>(std::max<uint64_t>(1, telemetry_.snapshots_received_total));
    }
}

void NetworkManager::UpdateRateTelemetry() {
    const uint32_t now = enet_time_get();
    if (last_rate_update_ms_ == 0) {
        last_rate_update_ms_ = now;
        return;
    }
    const uint32_t elapsed_ms = now - last_rate_update_ms_;
    if (elapsed_ms < 1000) {
        return;
    }

    const float elapsed_seconds = static_cast<float>(elapsed_ms) / 1000.0f;
    telemetry_.bytes_per_sec_up = static_cast<float>(rate_window_bytes_sent_) / elapsed_seconds;
    telemetry_.bytes_per_sec_down = static_cast<float>(rate_window_bytes_received_) / elapsed_seconds;
    telemetry_.packets_per_sec_up = static_cast<float>(rate_window_packets_sent_) / elapsed_seconds;
    telemetry_.packets_per_sec_down = static_cast<float>(rate_window_packets_received_) / elapsed_seconds;
    telemetry_.reconciliation_corrections_per_sec =
        static_cast<float>(rate_window_reconciliation_corrections_) / elapsed_seconds;

    rate_window_bytes_sent_ = 0;
    rate_window_bytes_received_ = 0;
    rate_window_packets_sent_ = 0;
    rate_window_packets_received_ = 0;
    rate_window_reconciliation_corrections_ = 0;
    last_rate_update_ms_ = now;
}
