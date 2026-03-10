#pragma once

#include <optional>
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

    void SendClientInput(const ClientInputMessage& message);
    std::vector<ClientInputMessage> ConsumeHostInputs();

    void BroadcastSnapshot(const ServerSnapshotMessage& message);
    std::optional<ServerSnapshotMessage> ConsumeLatestSnapshot();

    void BroadcastLobbyState(const LobbyStateMessage& message);
    std::optional<LobbyStateMessage> ConsumeLobbyState();

    void BroadcastMatchStart(const MatchStartMessage& message);
    bool ConsumeMatchStart();

    int GetAssignedLocalPlayerId() const;
    std::vector<RemotePlayerInfo> GetRemotePlayers() const;

  private:
    struct PeerInfo {
        int player_id = -1;
        std::string name;
    };

    bool EnsureEnetInitialized();
    void SendJsonToPeer(ENetPeer* peer, const nlohmann::json& json, bool reliable);
    void BroadcastJson(const nlohmann::json& json, bool reliable);

    bool enet_initialized_ = false;
    bool is_host_ = false;
    bool connected_ = false;

    ENetHost* host_ = nullptr;
    ENetPeer* server_peer_ = nullptr;

    int next_remote_player_id_ = 1;
    std::string local_player_name_ = "Player";
    int assigned_local_player_id_ = -1;

    std::unordered_map<ENetPeer*, PeerInfo> peers_;

    std::vector<ClientInputMessage> pending_host_inputs_;
    std::optional<ServerSnapshotMessage> latest_snapshot_;
    std::optional<LobbyStateMessage> latest_lobby_state_;
    bool pending_match_start_ = false;
};
