#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "net/network_messages.h"
#include "net/network_transport.h"

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
    uint64_t keyframe_snapshots_sent_total = 0;
    uint64_t delta_snapshots_sent_total = 0;
    uint64_t keyframe_snapshots_received_total = 0;
    uint64_t delta_snapshots_received_total = 0;
    uint64_t dropped_delta_missing_base_total = 0;
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
    explicit NetworkManager(std::unique_ptr<INetworkTransport> transport = nullptr);
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
    std::optional<MatchStartMessage> ConsumeMatchStart();
    void SendChatSubmit(const ChatSubmitMessage& message);
    std::vector<ChatSubmitMessage> ConsumeHostChatSubmits();
    void BroadcastConsoleMessage(const ConsoleMessageNet& message);
    void SendConsoleMessageToPlayer(int player_id, const ConsoleMessageNet& message);
    std::vector<ConsoleMessageNet> ConsumeConsoleMessages();
    void BroadcastFireSpiritLaunch(const FireSpiritLaunchMessage& message);
    std::vector<FireSpiritLaunchMessage> ConsumeFireSpiritLaunches();
    void BroadcastFireWaveStart(const FireWaveStartMessage& message);
    std::vector<FireWaveStartMessage> ConsumeFireWaveStarts();
    void BroadcastMapTransferBegin(const MapTransferBeginMessage& message);
    void BroadcastMapTransferChunk(const MapTransferChunkMessage& message);
    void BroadcastMapTransferComplete(const MapTransferCompleteMessage& message);
    std::optional<MapTransferBeginMessage> ConsumeMapTransferBegin();
    std::vector<MapTransferChunkMessage> ConsumeMapTransferChunks();
    std::optional<MapTransferCompleteMessage> ConsumeMapTransferComplete();

    int GetAssignedLocalPlayerId() const;
    std::vector<RemotePlayerInfo> GetRemotePlayers() const;
    std::vector<RemotePlayerInfo> ConsumeDisconnectedRemotePlayers();
    const NetTelemetry& GetTelemetry() const;
    void AddReconciliationCorrection();

  private:
    struct PeerInfo {
        int player_id = -1;
        std::string name;
        int last_acked_snapshot_id = 0;
        int last_keyframe_snapshot_id_sent = 0;
    };

    void SendPacketToPeer(NetworkPeerId peer_id, const std::vector<uint8_t>& packet_data,
                          NetworkPacketReliability reliability, uint8_t channel, bool is_snapshot);
    void BroadcastPacket(const std::vector<uint8_t>& packet_data, NetworkPacketReliability reliability, uint8_t channel,
                         bool is_snapshot);
    void RegisterOutgoingPacket(size_t bytes, bool is_snapshot);
    void RegisterIncomingPacket(size_t bytes, bool is_snapshot);
    void UpdateRateTelemetry();

    std::unique_ptr<INetworkTransport> transport_;
    bool is_host_ = false;
    bool connected_ = false;

    NetworkPeerId server_peer_id_ = 0;

    int next_remote_player_id_ = 1;
    std::string local_player_name_ = "Player";
    int assigned_local_player_id_ = -1;

    std::unordered_map<NetworkPeerId, PeerInfo> peers_;
    std::unordered_map<int, ServerSnapshotMessage> host_snapshot_history_;
    std::unordered_map<int, ServerSnapshotMessage> client_snapshot_history_;
    int last_client_applied_snapshot_id_ = 0;

    std::vector<ClientMoveMessage> pending_host_moves_;
    std::vector<ClientActionMessage> pending_host_actions_;
    std::vector<ChatSubmitMessage> pending_host_chat_submits_;
    std::optional<ServerSnapshotMessage> latest_snapshot_;
    std::optional<LobbyStateMessage> latest_lobby_state_;
    std::optional<MatchStartMessage> pending_match_start_;
    std::vector<ConsoleMessageNet> pending_console_messages_;
    std::vector<FireSpiritLaunchMessage> pending_fire_spirit_launches_;
    std::vector<FireWaveStartMessage> pending_fire_wave_starts_;
    std::vector<RemotePlayerInfo> disconnected_remote_players_;
    std::optional<MapTransferBeginMessage> latest_map_transfer_begin_;
    std::vector<MapTransferChunkMessage> pending_map_transfer_chunks_;
    std::optional<MapTransferCompleteMessage> latest_map_transfer_complete_;
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
