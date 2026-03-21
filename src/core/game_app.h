#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <deque>
#include <random>

#include <raylib.h>

#include "assets/map_loader.h"
#include "assets/composite_effects_loader.h"
#include "assets/objects_database.h"
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
    ServerSnapshotMessage BuildHostSnapshot();

    ClientInputMessage BuildLocalInput(int local_player_id);
    void SimulateHostGameplay(float dt);
    void SimulatePlayerFromInput(Player& player, const ClientInputMessage& input, float dt);
    void UpdateArena(float dt);
    void UpdateRespawns(float dt);
    void UpdateDamagePopups(float dt);
    void UpdateMapObjects(float dt);
    void UpdateIceWalls(float dt);
    void UpdateRunes(float dt);
    void UpdatePlayerStatusEffects(Player& player, float dt);
    void UpdateProjectiles(float dt);
    void UpdateExplosions(float dt);
    void UpdateLightningEffects(float dt);
    void UpdateGrapplingHooks(float dt);
    void UpdateCompositeEffects(float dt);
    void UpdateFireStormCasts(float dt);
    void UpdateFireStormDummies(float dt);
    void ResolvePlayerCollisions();
    void ResolvePlayerVsMapObjects(Player& player);
    void ResolvePlayerVsIceWalls(Player& player);
    void ResolvePlayerVsIceWallsLocal(Player& player);
    void PushPlayersOutOfIceWall(const GridCoord& cell);
    void HandleMeleeHit(Player& attacker);
    void HandleEventsHost();
    void UpdateProjectileEmitters();
    void UpdateParticles(float dt);
    void SpawnProjectileExplosion(const Projectile& projectile, std::optional<int> excluded_target_id);
    void SpawnLightningEffect(Vector2 start, Vector2 end, float idle_duration_seconds, bool volatile_variant = false);
    bool TryStartGrapplingHook(Player& player, Vector2 target_world);
    void SpawnDamagePopup(Vector2 world_pos, int amount, bool is_heal = false);
    bool ApplyDamageToPlayer(Player& target, int attacker_player_id, int damage, const char* source,
                             bool count_kill_for_attacker);
    void HandlePlayerDeath(Player& victim, int killer_player_id, bool count_kill_for_attacker);
    bool IsOutsideArena(Vector2 world_pos) const;
    Vector2 ClampToArenaWithBuffer(Vector2 world_pos, float buffer_tiles) const;
    Vector2 ComputeRespawnPosition(const Player& player) const;

    bool TryPlaceRune(Player& player, Vector2 world_mouse);
    bool TryPlaceFireStormDummy(Player& player, const GridCoord& cell);
    void SpawnFireStormDummyAtCell(int owner_player_id, int owner_team, const GridCoord& cell,
                                   float idle_lifetime_seconds);
    void CheckSpellPatterns(const RunePlacedEvent& event);

    bool IsTileRunePlaceable(const GridCoord& cell) const;
    bool IsCellOccupiedByRune(const GridCoord& cell) const;
    bool IsCellOccupiedByFireStormDummy(const GridCoord& cell) const;
    bool IsPlayerBeingPulled(int player_id) const;

    GridCoord WorldToCell(Vector2 world) const;
    Vector2 CellToWorldCenter(const GridCoord& cell) const;

    Player* FindPlayerById(int id);
    const Player* FindPlayerById(int id) const;
    MapObjectInstance* FindMapObjectById(int id);
    const MapObjectInstance* FindMapObjectById(int id) const;
    const ObjectPrototype* FindObjectPrototype(const std::string& prototype_id) const;
    void RebuildMapObjectsFromSeeds();
    void SpawnObjectInstanceAtCell(const std::string& prototype_id, const GridCoord& cell);
    bool ApplyObjectDamage(int object_instance_id, int amount, int source_player_id, const char* source);
    bool TryConsumeObject(int object_instance_id, int player_id);
    bool TryActivateItemSlot(Player& player, int slot_index);
    bool TryActivateItemById(Player& player, const std::string& prototype_id);
    void ApplyImmediateHeal(Player& player, int amount);
    void AddRegenerationStatus(Player& player, float duration_seconds, float amount_per_second);
    void AddStunnedStatus(Player& player, float duration_seconds);
    void RebuildInfluenceZones();
    int SpawnCompositeEffect(const std::string& effect_id, Vector2 origin_world);
    float GetCompositeEffectDurationSeconds(const std::string& effect_id) const;

    void RenderWorld();
    void RenderMap();
    void RenderNonTerrainDepthSorted();
    void RenderRunes();
    void RenderIceWalls();
    void RenderPlayers();
    void RenderMeleeAttacks();
    void RenderGrapplingHooks();
    void RenderProjectiles();
    void RenderParticles();
    void RenderDamagePopups();
    void RenderRunePlacementOverlay();
    void RenderBottomHud();
    void RenderNetworkDebugPanel();
    void UpdateCameraTarget();
    Vector2 GetRenderPlayerPosition(int player_id) const;
    bool IsWorldPointInsideCameraView(Vector2 world_pos) const;
    void PlaySfxIfVisible(const Sound& sound, bool loaded, Vector2 world_pos) const;
    bool HasVisibleIdleFireStormDummy() const;
    void LoadAudioAssets();
    void UnloadAudioAssets();
    void UpdateAudioFrame();
    void UpdateLocalFootstepAudio();
    Vector2 GetRenderGrapplingHookHeadPosition(int hook_id, Vector2 fallback) const;

    static FacingDirection AimToFacing(Vector2 aim);
    static const char* FacingToSpriteFacing(FacingDirection facing);
    std::string GetClientLobbyStatusText() const;

    ConfigManager config_manager_;
    ControlsManager controls_manager_;
    ControlsBindings controls_bindings_;
    UserSettings settings_;
    MapLoader map_loader_;
    ObjectsDatabase objects_database_;
    CompositeEffectsLoader composite_effects_loader_;
    SpriteMetadataLoader sprite_metadata_;
    SpriteMetadataLoader sprite_metadata_tall_;
    SpriteMetadataLoader sprite_metadata_96x96_;
    SpellPatternLoader spell_patterns_;
    SmokeEmitter smoke_emitter_;

    struct LoadedSfx {
        Sound sound = {};
        bool loaded = false;
    };

    GameState state_;
    EventQueue event_queue_;
    MostKillsMode most_kills_mode_;

    NetworkManager network_manager_;
    LanDiscovery lan_discovery_;

    AppScreen app_screen_ = AppScreen::MainMenu;
    Camera2D camera_ = {};
    std::unordered_map<int, ClientInputMessage> latest_remote_inputs_;
    std::unordered_map<int, Vector2> render_player_positions_;
    std::unordered_map<int, Vector2> render_grappling_hook_head_positions_;
    std::unordered_map<int, std::string> known_player_names_;

    struct RemotePositionSample {
        double time_seconds = 0.0;
        Vector2 pos = {0.0f, 0.0f};
    };
    std::unordered_map<int, std::deque<RemotePositionSample>> remote_position_samples_;
    std::unordered_map<int, std::deque<RemotePositionSample>> grappling_head_position_samples_;
    std::deque<ClientInputMessage> pending_local_prediction_inputs_;

    int local_input_tick_ = 0;
    int local_input_seq_ = 0;
    int host_server_tick_ = 0;
    double snapshot_accumulator_ = 0.0;
    double lobby_broadcast_accumulator_ = 0.0;
    int winning_team_ = -1;

    char player_name_buffer_[64] = {};
    char join_ip_buffer_[64] = "127.0.0.1";
    std::vector<std::string> lobby_player_names_;
    std::string host_display_ip_ = "127.0.0.1";
    std::string resolved_map_path_;
    std::string resolved_objects_config_path_;
    std::string resolved_composite_effects_path_;
    std::string resolved_sprite_metadata_path_;
    std::string resolved_sprite_metadata_tall_path_;
    std::string resolved_sprite_metadata_96x96_path_;
    std::string resolved_spell_pattern_path_;
    std::string resolved_menu_background_path_;
    std::string main_menu_status_message_;
    bool main_menu_status_is_error_ = false;
    double connect_attempt_start_seconds_ = 0.0;
    MatchModeType lobby_mode_type_ = MatchModeType::MostKillsTimed;
    int lobby_round_time_seconds_ = Constants::kDefaultMatchDurationSeconds;
    int lobby_best_of_target_kills_ = Constants::kDefaultBestOfTargetKills;
    float lobby_shrink_tiles_per_second_ = Constants::kDefaultShrinkTilesPerSecond;
    float lobby_shrink_start_seconds_ = Constants::kDefaultShrinkStartSeconds;
    float lobby_min_arena_radius_tiles_ = Constants::kDefaultMinArenaRadiusTiles;

    float render_time_seconds_ = 0.0f;
    bool force_windowed_launch_ = false;
    bool initial_fullscreen_setting_ = true;
    bool show_network_debug_panel_ = true;
    bool network_debug_panel_minimized_ = false;

    bool pending_primary_pressed_ = false;
    bool pending_grappling_pressed_ = false;
    int pending_select_rune_slot_ = -1;
    int pending_activate_item_slot_ = -1;
    bool pending_toggle_inventory_mode_ = false;
    std::vector<MapObjectSeed> pending_object_spawns_;
    std::mt19937 rng_;
    std::mt19937 visual_rng_;
    Texture2D menu_background_texture_ = {};
    bool has_menu_background_texture_ = false;
    float camera_shake_time_remaining_ = 0.0f;
    std::unordered_map<int, float> fire_storm_dummy_lightning_seconds_remaining_;
    std::unordered_map<int, float> fire_storm_dummy_lightning_cooldown_seconds_remaining_;
    std::unordered_map<int, bool> fire_storm_cast_impact_played_;

    bool audio_device_ready_ = false;
    LoadedSfx sfx_fireball_created_;
    LoadedSfx sfx_melee_attack_;
    LoadedSfx sfx_create_rune_;
    LoadedSfx sfx_explosion_;
    LoadedSfx sfx_vase_breaking_;
    LoadedSfx sfx_ice_wall_freeze_;
    LoadedSfx sfx_ice_wall_melt_;
    LoadedSfx sfx_player_death_;
    LoadedSfx sfx_player_damaged_;
    LoadedSfx sfx_item_pickup_;
    LoadedSfx sfx_drink_potion_;
    LoadedSfx sfx_fire_storm_cast_;
    LoadedSfx sfx_fire_storm_impact_;
    LoadedSfx sfx_static_upgrade_;
    LoadedSfx sfx_static_bolt_impact_;
    LoadedSfx sfx_grappling_throw_;
    LoadedSfx sfx_grappling_latch_;
    std::array<LoadedSfx, 5> sfx_footstep_dirt_{};
    Music bgm_forest_day_ = {};
    bool has_bgm_forest_day_ = false;
    Music fire_storm_ambient_ = {};
    bool has_fire_storm_ambient_ = false;
    float fire_storm_ambient_gain_ = 0.0f;
    Vector2 local_footstep_prev_pos_ = {0.0f, 0.0f};
    bool has_local_footstep_prev_pos_ = false;
    float local_footstep_distance_accumulator_ = 0.0f;
};
