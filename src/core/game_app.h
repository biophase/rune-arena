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
#include "emitters/smoke_emitter.h"
#include "events/event_queue.h"
#include "modes/most_kills_mode.h"
#include "net/lan_discovery.h"
#include "net/network_manager.h"
#include "spells/spell_pattern_loader.h"

enum class AppScreen {
    MainMenu,
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
    void UpdateLobby(float dt);
    void UpdateMatch(float dt);
    void UpdatePostMatch(float dt);

    void StartAsHost();
    void StartAsClient(const std::string& ip);
    void ReturnToMainMenu();

    void StartMatchAsHost();
    void ApplySnapshotToClientState(const ServerSnapshotMessage& snapshot);
    ServerSnapshotMessage BuildHostSnapshot() const;

    ClientInputMessage BuildLocalInput(int local_player_id);
    void SimulateHostGameplay(float dt);
    void SimulatePlayerFromInput(Player& player, const ClientInputMessage& input, float dt);
    void UpdateProjectiles(float dt);
    void ResolvePlayerCollisions();
    void HandleMeleeHit(Player& attacker);
    void HandleEventsHost();
    void UpdateProjectileEmitters();
    void UpdateParticles(float dt);

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
    void RenderPlayers();
    void RenderProjectiles();
    void RenderParticles();
    void RenderRunePlacementOverlay();
    void RenderDebugRuneCooldown();
    void UpdateCameraTarget();

    static FacingDirection AimToFacing(Vector2 aim);
    static const char* FacingToSpriteFacing(FacingDirection facing);

    ConfigManager config_manager_;
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

    int local_input_tick_ = 0;
    double snapshot_accumulator_ = 0.0;
    int winning_team_ = -1;

    char player_name_buffer_[64] = {};
    char join_ip_buffer_[64] = "127.0.0.1";
    std::vector<std::string> lobby_player_names_;
    std::string host_display_ip_ = "127.0.0.1";

    float render_time_seconds_ = 0.0f;
    bool force_windowed_launch_ = false;
    bool initial_fullscreen_setting_ = true;

    bool pending_primary_pressed_ = false;
    bool pending_select_fire_ = false;
    bool pending_select_water_ = false;
};
