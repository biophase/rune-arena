#pragma once

#include <optional>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "net/network_messages.h"

struct _ENetHost;
struct _ENetPeer;
typedef struct _ENetHost ENetHost;
typedef struct _ENetPeer ENetPeer;

struct RemotePlayerInfo {
    int player_id = -1;
    std::string name;
};

struct NetTelemetry {
    uint64_t bytes_sent_total = 0;
    uint64_t bytes_received_total = 0;
    uint64_t packets_sent_total = 0;
    uint64_t packets_received_total = 0;
    uint64_t snapshot_bytes_sent_total = 0;
    uint64_t snapshot_bytes_received_total = 0;
    uint64_t snapshots_sent_total = 0;
    uint64_t snapshots_received_total = 0;
    uint64_t dropped_snapshots_total = 0;
    uint64_t reconciliation_corrections_total = 0;

    float bytes_per_sec_up = 0.0f;
    float bytes_per_sec_down = 0.0f;
    float packets_per_sec_up = 0.0f;
    float packets_per_sec_down = 0.0f;
    float reconciliation_corrections_per_sec = 0.0f;
    float average_snapshot_bytes_sent = 0.0f;
    float average_snapshot_bytes_received = 0.0f;
};

enum class ClientConnectionState {
    Idle,
    ConnectingTransport,
    WaitingJoinAck,
    WaitingLobbyState,
    ReadyInLobby,
    Disconnected,
};

class NetworkManager {
  public:
    NetworkManager();
    ~NetworkManager();

    bool StartHost(int port);
    bool StartClient();
    bool ConnectToHost(const std::string& ip, int port);
    void Stop();

    void SetLocalPlayerName(const std::string& player_name);

    void Poll();
    bool IsHost() const;
    bool IsConnected() const;
    ClientConnectionState GetClientConnectionState() const;
    bool HasReceivedJoinAck() const;
    bool HasReceivedLobbyState() const;
    const std::string& GetLastDebugMessage() const;

    void SendClientMove(const ClientMoveMessage& message);
    void SendClientAction(const ClientActionMessage& message);
    std::vector<ClientMoveMessage> ConsumeHostMoveInputs();
    std::vector<ClientActionMessage> ConsumeHostActionInputs();

    void BroadcastSnapshot(const ServerSnapshotMessage& message);
    std::optional<ServerSnapshotMessage> ConsumeLatestSnapshot();

    void BroadcastLobbyState(const LobbyStateMessage& message);
    std::optional<LobbyStateMessage> ConsumeLobbyState();

    void BroadcastMatchStart(const MatchStartMessage& message);
    bool ConsumeMatchStart();

    int GetAssignedLocalPlayerId() const;
    std::vector<RemotePlayerInfo> GetRemotePlayers() const;
    const NetTelemetry& GetTelemetry() const;
    void AddReconciliationCorrection();

  private:
    struct PeerInfo {
        int player_id = -1;
        std::string name;
    };

    bool EnsureEnetInitialized();
    void SendJsonToPeer(ENetPeer* peer, const nlohmann::json& json, bool reliable, uint8_t channel);
    void BroadcastJson(const nlohmann::json& json, bool reliable, uint8_t channel);
    void RegisterOutgoingPacket(size_t bytes, bool is_snapshot);
    void RegisterIncomingPacket(size_t bytes, bool is_snapshot);
    void UpdateRateTelemetry();

    bool enet_initialized_ = false;
    bool is_host_ = false;
    bool connected_ = false;

    ENetHost* host_ = nullptr;
    ENetPeer* server_peer_ = nullptr;

    int next_remote_player_id_ = 1;
    std::string local_player_name_ = "Player";
    int assigned_local_player_id_ = -1;

    std::unordered_map<ENetPeer*, PeerInfo> peers_;

    std::vector<ClientMoveMessage> pending_host_moves_;
    std::vector<ClientActionMessage> pending_host_actions_;
    std::optional<ServerSnapshotMessage> latest_snapshot_;
    std::optional<LobbyStateMessage> latest_lobby_state_;
    bool pending_match_start_ = false;
    bool client_received_lobby_state_ = false;
    ClientConnectionState client_connection_state_ = ClientConnectionState::Idle;
    std::string last_debug_message_ = "idle";
    NetTelemetry telemetry_;
    uint64_t rate_window_bytes_sent_ = 0;
    uint64_t rate_window_bytes_received_ = 0;
    uint64_t rate_window_packets_sent_ = 0;
    uint64_t rate_window_packets_received_ = 0;
    uint64_t rate_window_reconciliation_corrections_ = 0;
    uint32_t last_rate_update_ms_ = 0;
    int last_received_snapshot_tick_ = -1;
};
