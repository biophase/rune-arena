#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <raylib.h>

#include "assets/map_loader.h"
#include "assets/sprite_metadata.h"
#include "config/config_manager.h"
#include "config/controls_manager.h"
#include "emitters/smoke_emitter.h"
#include "events/event_queue.h"
#include "modes/most_kills_mode.h"
#include "net/lan_discovery.h"
#include "net/network_manager.h"
#include "spells/spell_pattern_loader.h"

enum class AppScreen {
    MainMenu,
    Connecting,
    Lobby,
    InMatch,
    PostMatch,
};

class GameApp {
  public:
    explicit GameApp(bool force_windowed_launch = false);

    bool Initialize();
    void Run();
    void Shutdown();

  private:
    void Update(float dt);
    void Render();
    void CaptureFrameInputEdges();

    void UpdateMainMenu(float dt);
    void UpdateConnecting(float dt);
    void UpdateLobby(float dt);
    void UpdateMatch(float dt);
    void UpdatePostMatch(float dt);
    void UpdateClientVisualSmoothing(float dt);
    void ApplyClientLocalInputPreview(const ClientInputMessage& input, float dt);

    void StartAsHost();
    void StartAsClient(const std::string& ip, int port);
    void ReturnToMainMenu();

    void StartMatchAsHost();
    void ApplySnapshotToClientState(const ServerSnapshotMessage& snapshot);
    ServerSnapshotMessage BuildHostSnapshot() const;

    ClientInputMessage BuildLocalInput(int local_player_id);
    void SimulateHostGameplay(float dt);
    void SimulatePlayerFromInput(Player& player, const ClientInputMessage& input, float dt);
    void UpdateIceWalls(float dt);
    void UpdateProjectiles(float dt);
    void UpdateExplosions(float dt);
    void ResolvePlayerCollisions();
    void ResolvePlayerVsIceWalls(Player& player);
    void PushPlayersOutOfIceWall(const GridCoord& cell);
    void HandleMeleeHit(Player& attacker);
    void HandleEventsHost();
    void UpdateProjectileEmitters();
    void UpdateParticles(float dt);
    void SpawnProjectileExplosion(const Projectile& projectile, std::optional<int> excluded_target_id);

    bool TryPlaceRune(Player& player, Vector2 world_mouse);
    void CheckSpellPatterns(const RunePlacedEvent& event);

    bool IsTileRunePlaceable(const GridCoord& cell) const;
    bool IsCellOccupiedByRune(const GridCoord& cell) const;

    GridCoord WorldToCell(Vector2 world) const;
    Vector2 CellToWorldCenter(const GridCoord& cell) const;

    Player* FindPlayerById(int id);
    const Player* FindPlayerById(int id) const;

    void RenderWorld();
    void RenderMap();
    void RenderRunes();
    void RenderIceWalls();
    void RenderPlayers();
    void RenderMeleeAttacks();
    void RenderProjectiles();
    void RenderParticles();
    void RenderRunePlacementOverlay();
    void RenderDebugRuneCooldown();
    void UpdateCameraTarget();
    Vector2 GetRenderPlayerPosition(int player_id) const;

    static FacingDirection AimToFacing(Vector2 aim);
    static const char* FacingToSpriteFacing(FacingDirection facing);
    std::string GetClientLobbyStatusText() const;

    ConfigManager config_manager_;
    ControlsManager controls_manager_;
    ControlsBindings controls_bindings_;
    UserSettings settings_;
    MapLoader map_loader_;
    SpriteMetadataLoader sprite_metadata_;
    SpellPatternLoader spell_patterns_;
    SmokeEmitter smoke_emitter_;

    GameState state_;
    EventQueue event_queue_;
    MostKillsMode most_kills_mode_;

    NetworkManager network_manager_;
    LanDiscovery lan_discovery_;

    AppScreen app_screen_ = AppScreen::MainMenu;
    Camera2D camera_ = {};
    std::unordered_map<int, ClientInputMessage> latest_remote_inputs_;
    std::unordered_map<int, Vector2> render_player_positions_;

    int local_input_tick_ = 0;
    double snapshot_accumulator_ = 0.0;
    double lobby_broadcast_accumulator_ = 0.0;
    int winning_team_ = -1;

    char player_name_buffer_[64] = {};
    char join_ip_buffer_[64] = "127.0.0.1";
    std::vector<std::string> lobby_player_names_;
    std::string host_display_ip_ = "127.0.0.1";
    std::string resolved_map_path_;
    std::string resolved_tile_mapping_path_;
    std::string resolved_sprite_metadata_path_;
    std::string resolved_spell_pattern_path_;
    std::string main_menu_status_message_;
    bool main_menu_status_is_error_ = false;
    double connect_attempt_start_seconds_ = 0.0;

    float render_time_seconds_ = 0.0f;
    bool force_windowed_launch_ = false;
    bool initial_fullscreen_setting_ = true;

    bool pending_primary_pressed_ = false;
    bool pending_select_fire_ = false;
    bool pending_select_water_ = false;
};
