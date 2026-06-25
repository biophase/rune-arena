#pragma once

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <deque>
#include <future>
#include <random>

#include <raylib.h>

#include "assets/map_loader.h"
#include "assets/composite_effects_loader.h"
#include "assets/modular_character_asset.h"
#include "assets/objects_database.h"
#include "assets/sprite_metadata.h"
#include "config/config_manager.h"
#include "config/controls_manager.h"
#include "emitters/smoke_emitter.h"
#include "events/event_queue.h"
#include "game/game_state.h"
#include "gameplay/equipment_registry.h"
#include "gameplay/hit_shape_library.h"
#include "gameplay/loot_tables.h"
#include "gameplay/spell_runtime_registry.h"
#include "modes/most_kills_mode.h"
#include "net/discovery_service.h"
#include "net/network_manager.h"
#include "spells/spell_pattern_loader.h"

enum class AppScreen {
    MainMenu,
    Connecting,
    Lobby,
    LoadingMatchMap,
    InMatch,
    PostMatch,
};

struct InfluenceDistanceField {
    int width = 0;
    int height = 0;
    std::vector<Color> pixels;
};

struct InfluenceBuildRequest {
    uint64_t signature = 0;
    int generation = 0;
    int map_width = 0;
    int map_height = 0;
    int cell_size = 0;
    int samples_per_tile = 1;
    std::vector<InfluenceZoneCell> zones;
};

struct InfluenceBuildResult {
    uint64_t signature = 0;
    int generation = 0;
    InfluenceDistanceField red_field;
    InfluenceDistanceField blue_field;
};

class GameApp {
  public:
    explicit GameApp(bool force_windowed_launch = false);

    bool Initialize();
    void Run();
    void Shutdown();

  private:
    struct ActiveModularAttackVisual;

    void Update(float dt);
    void Render();
    void CaptureFrameInputEdges();

    void UpdateMainMenu(float dt);
    void UpdateConnecting(float dt);
    void UpdateLobby(float dt);
    void UpdateLoadingMatchMap(float dt);
    void UpdateMatch(float dt);
    void UpdatePostMatch(float dt);
    void UpdateClientVisualSmoothing(float dt);
    void ApplyClientLocalInputPreview(const ClientInputMessage& input, float dt);
    void PumpInfluenceFieldBuilds();
    void StartPendingInfluenceFieldBuild();
    void ApplyInfluenceBuildResult(InfluenceBuildResult&& result);

    void StartAsHost();
    void StartAsClient(const std::string& ip, int port);
    void ReturnToMainMenu();
    void RefreshLobbyMapCatalog();
    void SetSelectedLobbyMapIndex(int index);
    std::string BuildLobbyMapOptionsText() const;
    std::string GetSelectedLobbyMapLabel() const;
    bool RebuildLobbyMapPreview();
    void ClearLobbyMapPreviewTexture();
    void ApplyLobbyPreviewTextureFromPngBytes(const std::vector<uint8_t>& png_bytes);
    void PumpMapTransferMessages();
    void BeginClientMatchMapLoading(const MatchStartMessage& message);
    bool FinalizeClientTransferredMap();
    bool SendSelectedMapToClients(int transfer_id, const std::vector<uint8_t>& file_bytes);
    bool LoadMapOrFallback(const std::string& preferred_map_path);

    void StartMatchAsHost();
    void ApplySnapshotToClientState(const ServerSnapshotMessage& snapshot);
    ServerSnapshotMessage BuildHostSnapshot();
    void ClearInfluenceZoneVisuals();
    void DrainIncomingConsoleMessages();
    void UpdateConsoleMessages(float dt);
    void AddConsoleMessage(const ConsoleMessage& message);
    void BroadcastConsoleMessageToAll(const ConsoleMessage& message);
    void SendConsoleMessageToPlayer(int player_id, const ConsoleMessage& message);
    void SendConsoleMessageToTeam(int team, const ConsoleMessage& message);
    void HandleHostChatSubmit(const ChatSubmitMessage& message);
    void HandleDisconnectedRemotePlayers();
    void MaybeBroadcastMatchCountdown();
    void RecordKillTimelinePoint();
    bool TryOpenChatInput();
    void UpdateChatInput();
    void CancelChatInput();
    void SubmitChatInput(bool team_only);
    bool TryBuildPrivateChatSubmit(const std::string& text, ChatSubmitMessage* out_message) const;

    ClientInputMessage BuildLocalInput(int local_player_id);
    void SimulateHostGameplay(float dt);
    void SimulatePlayerFromInput(Player& player, const ClientInputMessage& input, float dt);
    void UpdateArena(float dt);
    void UpdateRespawns(float dt);
    void UpdateDamagePopups(float dt);
    void UpdateMapObjects(float dt);
    void UpdateIceWalls(float dt);
    void UpdateRunes(float dt);
    void UpdateCastles(float dt);
    void UpdatePlayerStatusEffects(Player& player, float dt);
    void UpdateProjectiles(float dt);
    void UpdateExplosions(float dt);
    void UpdateLightningEffects(float dt);
    void UpdateGrapplingHooks(float dt);
    void UpdateCompositeEffects(float dt);
    void UpdateFireStormCasts(float dt);
    void UpdateFireStormDummies(float dt);
    void UpdateEarthRootsGroups(float dt);
    void ResolvePlayerCollisions();
    void ResolvePlayerVsMapObjects(Player& player);
    void ResolvePlayerVsIceWalls(Player& player);
    void ResolvePlayerVsIceWallsLocal(Player& player);
    void PushPlayersOutOfIceWall(const GridCoord& cell);
    void HandleMeleeHit(Player& attacker);
    void HandleEventsHost();
    void UpdateProjectileEmitters();
    void UpdateSnowParticleEmitters(float dt);
    void UpdateFireStormArcVisuals(float dt);
    void UpdateParticles(float dt);
    void UpdateHammerImpactEffects(float dt);
    void SpawnDamageHitParticles(Vector2 target_pos, std::optional<Vector2> damage_world_pos);
    void SpawnHammerImpactEffect(Vector2 world_pos);
    void SpawnProjectileExplosion(const Projectile& projectile, std::optional<int> excluded_target_id);
    void SpawnLightningEffect(Vector2 start, Vector2 end, float idle_duration_seconds, bool volatile_variant = false,
                              const char* born_animation_key = nullptr, const char* idle_animation_key = nullptr,
                              const char* death_animation_key = nullptr);
    bool TryStartGrapplingHook(Player& player, Vector2 target_world, bool play_audio = true);
    void SpawnDamagePopup(Vector2 world_pos, int amount, bool is_heal = false);
    bool ApplyDamageToPlayer(Player& target, int attacker_player_id, int damage, const char* source,
                             bool count_kill_for_attacker,
                             std::optional<Vector2> damage_world_pos = std::nullopt);
    void HandlePlayerDeath(Player& victim, int killer_player_id, bool count_kill_for_attacker);
    bool IsZoneEnabled() const;
    bool IsOutsideArena(Vector2 world_pos) const;
    Vector2 ClampToArenaWithBuffer(Vector2 world_pos, float buffer_tiles) const;
    Vector2 ComputeRespawnPosition(const Player& player) const;

    bool TryPlaceRune(Player& player, Vector2 world_mouse);
    bool TryPlaceFireStormDummy(Player& player, const GridCoord& cell);
    bool IsVolatileCastCellForPlayer(const Player& player, const GridCoord& cell) const;
    float GetRunePlacementManaCost(const Player& player, RuneType rune_type, const GridCoord& cell,
                                   bool* out_volatile_cast = nullptr) const;
    void SpawnVolatileCastCasterFx(int player_id);
    void UpdateVolatileCastCasterFx(float dt);
    void ConvertRuneToFireStorm(Rune& rune, int caster_player_id, int caster_team, bool temporary, bool source_rune);
    void BeginFireStormRuneRevert(Rune& rune, bool restore_after_death);
    void FinalizeFireStormRuneDeath(Rune& rune);
    void SpawnFireStormConvertedRuneAtCell(int owner_player_id, int owner_team, const GridCoord& cell, bool source_rune);
    void SpawnStormArcVisuals(const FireStormCast& cast);
    void SpawnStormArcSparkParticle(Vector2 world_pos, float visual_z);
    void SpawnFireStormDummyAtCell(int owner_player_id, int owner_team, const GridCoord& cell,
                                   float idle_lifetime_seconds);
    int SpawnEarthRootsGroup(int owner_player_id, int owner_team, const GridCoord& center_cell);
    void RebuildAltarsFromMapObjects();
    void UpdateAltars(float dt);
    void CheckSpellPatterns(const RunePlacedEvent& event);

    bool IsTileRunePlaceable(const GridCoord& cell) const;
    bool IsCellOccupiedByRune(const GridCoord& cell) const;
    bool IsCellOccupiedByFireStormDummy(const GridCoord& cell) const;
    bool IsPlayerBeingPulled(int player_id) const;
    bool IsRuneAvailableToPlayer(const Player& player, RuneType rune_type) const;
    bool IsCastleEquippableRune(RuneType rune_type) const;
    int GetRuneRequiredCastleLevel(RuneType rune_type) const;
    int GetCastleLoadoutCapacity(int castle_level) const;
    float GetCastleEnergyRequirementForLevel(int castle_level) const;
    int ComputeCastleLevel(float total_energy, float* out_energy_into_current_level = nullptr,
                           float* out_energy_needed_for_next_level = nullptr) const;
    const CastleState* FindCastleById(int id) const;
    CastleState* FindCastleById(int id);
    const CastleState* FindCastleByTeam(int team) const;
    CastleState* FindCastleByTeam(int team);
    const CastleState* GetAlliedCastleForPlayer(const Player& player) const;
    bool IsPlayerWithinCastleRange(const Player& player, const CastleState& castle) const;
    Vector2 GetCastleChargePortWorld(const CastleState& castle) const;
    void RefreshCastleDerivedState(CastleState& castle);
    void InitializeCastlesFromSpawnPoints();
    void NormalizePlayerRuneLoadout(Player& player) const;
    bool EquipRuneToCastleLoadout(Player& player, RuneType rune_type, int castle_level);
    bool UnequipRuneFromCastleLoadout(Player& player, RuneType rune_type);
    bool IsCastleManagedRuneSlot(int slot_index) const;
    bool IsCastleCharging(int castle_id) const;
    void OpenLocalInventoryUiForCurrentContext();
    void CloseLocalInventoryUi();

    GridCoord WorldToCell(Vector2 world) const;
    Vector2 CellToWorldCenter(const GridCoord& cell) const;

    Player* FindPlayerById(int id);
    const Player* FindPlayerById(int id) const;
    MapObjectInstance* FindMapObjectById(int id);
    const MapObjectInstance* FindMapObjectById(int id) const;
    const ObjectPrototype* FindObjectPrototype(const std::string& prototype_id) const;
    void RebuildMapObjectsFromSeeds();
    int SpawnObjectInstanceAtCell(const std::string& prototype_id, const GridCoord& cell, int forced_id = -1);
    int SpawnDroppedEquipmentItem(const std::string& prototype_id, const GridCoord& cell,
                                  std::optional<int> blocked_player_id = std::nullopt);
    void RegisterPickupBlockForDroppedObject(int object_instance_id, int blocked_player_id, Vector2 origin_world,
                                             float unlock_radius);
    bool CanPlayerPickUpObject(const Player& player, const MapObjectInstance& object) const;
    void UpdateDroppedItemPickupBlocks();
    bool ApplyObjectDamage(int object_instance_id, int amount, int source_player_id, const char* source,
                           std::optional<Vector2> damage_world_pos = std::nullopt);
    bool TryConsumeObject(int object_instance_id, int player_id);
    bool TryActivateItemSlot(Player& player, int slot_index);
    bool TryActivateItemById(Player& player, const std::string& prototype_id);
    void ApplyImmediateHeal(Player& player, int amount);
    void ApplyImmediateManaRestore(Player& player, int amount);
    void CancelRegenerationStatuses(Player& player);
    void CancelInventoryDrag(Player& player, bool drop_to_world_if_unresolved = false);
    void BeginInventoryDrag(Player& player, SlotFamily family, int slot_index);
    void DropInventoryDrag(Player& player, SlotFamily family, int slot_index);
    void QueueLocalInventorySync(const Player& player);
    void ApplyInventoryLayoutSync(Player& player, const ClientActionMessage& action);
    bool PendingLocalInventorySyncMatches(const Player& player) const;
    bool TryDropDraggedSlotToWorld(Player& player, SlotFamily family, int slot_index, bool single_instance);
    void AddRegenerationStatus(Player& player, float duration_seconds, float amount_per_second);
    void AddManaRegenerationStatus(Player& player, float duration_seconds, float amount_per_second);
    void AddStunnedStatus(Player& player, float duration_seconds);
    void AddFrozenStatus(Player& player, float duration_seconds);
    void RefreshOrAddRootedStatus(Player& player, int source_id);
    void SpawnIceShardDeathParticle(Vector2 position, Vector2 travel_velocity);
    void SpawnIceWaveShardSnowParticle(const Projectile& projectile);
    void SpawnFrozenStatusSnowParticle(const Player& player);
    void SpawnIceWallHealSnowBurst(const IceWallPiece& wall, int healed_amount);
    void RebuildInfluenceZones();
    int SpawnCompositeEffect(const std::string& effect_id, Vector2 origin_world);
    float GetCompositeEffectDurationSeconds(const std::string& effect_id) const;

    void RenderWorld();
    void RenderMap();
    void RenderMapForeground();
    void RenderGroundMapObjects();
    void EnsureWorldLayerRenderTarget();
    void EnsureShadowLayerRenderTarget();
    void UpdateObjectShadowLayer();
    void DrawObjectShadowLayer();
    enum class DepthSortedRenderPass {
        UnderInfluenceOverlay,
        OverInfluenceOverlay,
    };
    void RenderNonTerrainDepthSorted(DepthSortedRenderPass pass);
    void RenderInfluenceZoneOverlay();
    void RenderInfluenceZoneAnimatedTiles();
    bool ShouldRenderInfluenceTeam(int team) const;
    bool IsInfluenceZoneSystemEnabled() const;
    void RenderRunes();
    void RenderIceWalls();
    void RenderPlayers();
    void RenderPlayerOverlays();
    void RenderMeleeAttacks();
    void RenderGrapplingHooks();
    void RenderProjectiles();
    void RenderParticles();
    void RenderDamagePopups();
    void RenderRunePlacementOverlay();
    void RenderLocalGrapplingPreview();
    void RenderDebugCollisionOverlay();
    void RenderZoneFillOverlay();
    void RenderZoneBorderOverlay();
    void RenderMapBoundsFadeOverlay();
    void RenderBottomHud();
    void RenderConsoleLog() const;
    void RenderChatInput() const;
    void RenderFpsCounter();
    void RenderNetworkDebugPanel();
    void RenderInGameMenu();
    void RenderMapLoadingScreen() const;
    void UpdateCameraTarget();
    Vector2 GetRenderPlayerPosition(int player_id) const;
    bool IsWorldPointInsideCameraView(Vector2 world_pos) const;
    float GetPlayerMovementSpeedMultiplier(const Player& player) const;
    float GetPlayerRuneCastRangeWorld(const Player& player) const;
    float GetPlayerGrapplingRangeWorld(const Player& player) const;
    float GetPlayerBaseAcceleration(const Player& player) const;
    float GetPlayerAccelerationMultiplier(const Player& player) const;
    bool CanPlayerStartGrapplingPreview(const Player& player) const;
    void PlaySfxIfVisible(const Sound& sound, bool loaded, Vector2 world_pos) const;
    bool HasVisibleIdleFireStormDummy() const;
    void LoadAudioAssets();
    void UnloadAudioAssets();
    void UpdateAudioFrame();
    void UpdateLocalFootstepAudio();
    Vector2 GetRenderGrapplingHookHeadPosition(int hook_id, Vector2 fallback) const;
    const ActiveModularAttackVisual* FindActiveModularAttackVisual(int player_id) const;
    float GetPlayerLockedMeleeAimRadians(const Player& player) const;
    float GetPlayerAttackVisualRotationDegrees(const Player& player) const;
    void LoadRenderShaders();
    void UnloadRenderShaders();
    void UpdateDamageFlashVisuals(float dt);
    bool DrawMaskedOccluder(const Rectangle& world_dst, const Texture2D& texture, const Rectangle& src, float sort_y);
    void DrawWorldLayerWithZonePostProcess();
    bool RenderModularTreeObject(const MapObjectInstance& object, const ObjectPrototype& proto, float sort_y);
    bool RenderModularTreeShadow(const MapObjectInstance& object, const ObjectPrototype& proto);
    bool DrawWindAnimatedMapObject(const MapObjectInstance& object, const ObjectPrototype& proto, const Texture2D& texture,
                                   Rectangle src, Rectangle dst);
    Rectangle GetMapObjectSpriteRect(const MapObjectInstance& object, const ObjectPrototype* proto, float sprite_width,
                                     float sprite_height, float visual_scale = 1.0f, bool snap = true) const;
    std::string ResolveMapObjectAnimation(const MapObjectInstance& object, const ObjectPrototype& proto,
                                          const SpriteMetadataLoader& metadata, float* out_anim_time = nullptr) const;
    float GetMapObjectAnimationDurationSeconds(const ObjectPrototype& proto, const std::string& animation_name) const;
    Rectangle GetMapObjectCollisionAabb(const MapObjectInstance& object, const ObjectPrototype* proto) const;

    static FacingDirection AimToFacing(Vector2 aim);
    static const char* FacingToSpriteFacing(FacingDirection facing);
    bool PlayerHasEquippedWeapon(const Player& player, const char* weapon_id) const;
    const AttackProfile* GetEquippedPrimaryAttack(const Player& player) const;
    const MobilityProfile* GetEquippedMobility(const Player& player) const;
    void UpdateActiveModularAttackVisuals(float dt);
    std::vector<std::string> GetEquippedModularLayers(const Player& player) const;
    std::string ResolveSwordAttackModularTag(const Player& player) const;
    std::string ResolvePrimaryWeaponAttackModularTag(const Player& player) const;
    Vector2 ResolveHammerImpactWorldPosition(const Player& player) const;
    float GetModularTagFrameStartSeconds(const std::string& tag_name, int frame_index, std::string* out_layer_name = nullptr) const;
    bool ShouldPlayImmediateMeleeSwingSfx(const Player& player) const;
    std::string ResolvePlayerModularAnimationName(const Player& player) const;
    std::string ResolvePlayerModularTag(const Player& player) const;
    float ResolvePlayerModularTime(const Player& player) const;
    float GetPlayerDamageFlashAmount(const Player& player) const;
    float GetObjectDamageFlashAmount(const MapObjectInstance& object) const;
    Rectangle GetPlayerCollisionRect(const Player& player) const;
    Rectangle GetPlayerCollisionRect(Vector2 center) const;
    float GetPlayerCollisionSupportDistance(Vector2 direction) const;
    bool LoadModularTreeAssetMetadata();
    Rectangle GetModularTreeSpriteRect(const MapObjectInstance& object, const char* layer_name = "trunk") const;
    float GetMapObjectWindPhaseOffset(const MapObjectInstance& object) const;
    bool TryEquipItem(Player& player, const std::string& item_id, const MapObjectInstance* source_object = nullptr);
    void RegisterSpellRuntimes();
    bool CastRuntimeSpell(const SpellRuntimeMatch& match);
    Rectangle GetPlayerSpriteRect(Vector2 draw_pos, const std::string& layer_name = "main") const;
    void RenderPlayerModularLayers(const Player& player, Vector2 draw_pos) const;
    void RenderVolatileCastCasterFx(const Player& player, Vector2 draw_pos) const;
    std::string GetClientLobbyStatusText() const;
    std::string GetPlayerDisplayName(int player_id) const;
    Rectangle GetCameraWorldCullRect(float padding_world = 0.0f) const;
    void GetVisibleCellBounds(int padding_cells, int* out_min_x, int* out_min_y, int* out_max_x, int* out_max_y) const;
    bool IsWorldRectVisible(const Rectangle& world_rect, float padding_world = 0.0f) const;
    void RebuildStaticRenderCaches();

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
    SpriteMetadataLoader sprite_metadata_128x128_;
    std::vector<Texture2D> sprite_sheet_128x128_team_variants_;
    ModularCharacterAsset modular_player_asset_;
    ModularCharacterAsset modular_tree_asset_;
    SpellPatternLoader spell_patterns_;
    EquipmentRegistry equipment_registry_;
    HitShapeLibrary hit_shape_library_;
    LootTableLibrary loot_table_library_;
    SpellRuntimeRegistry spell_runtime_registry_;
    SmokeEmitter smoke_emitter_;

    struct LoadedSfx {
        Sound sound = {};
        bool loaded = false;
    };

    GameState state_;
    EventQueue event_queue_;
    MostKillsMode most_kills_mode_;

    NetworkManager network_manager_;
    std::unique_ptr<IDiscoveryService> discovery_service_;

    AppScreen app_screen_ = AppScreen::MainMenu;
    Camera2D camera_ = {};
    std::unordered_map<int, ClientInputMessage> latest_remote_inputs_;
    std::unordered_map<int, Vector2> render_player_positions_;
    std::unordered_map<int, Vector2> render_grappling_hook_head_positions_;
    std::unordered_map<int, std::string> known_player_names_;
    struct ActiveModularAttackVisual {
        std::string tag;
        std::string weapon_item_id;
        float elapsed_seconds = 0.0f;
        float duration_seconds = 0.0f;
        float snapped_angle_radians = 0.0f;
        float locked_target_angle_radians = 0.0f;
        float impact_time_seconds = 0.0f;
        float recovery_start_time_seconds = 0.0f;
        float attack_end_time_seconds = 0.0f;
        bool uses_continuous_orientation = false;
        bool swing_event_played = false;
        bool impact_event_played = false;
    };
    std::unordered_map<int, ActiveModularAttackVisual> active_modular_attack_visuals_;
    struct HammerImpactEffect {
        Vector2 world_pos = {0.0f, 0.0f};
        float age_seconds = 0.0f;
        float duration_seconds = 0.0f;
        bool alive = true;
    };
    struct StormArcVisual {
        int cast_id = -1;
        Vector2 start_world = {0.0f, 0.0f};
        Vector2 end_world = {0.0f, 0.0f};
        float elapsed_seconds = 0.0f;
        float duration_seconds = 0.0f;
        float peak_height = 0.0f;
        float rotation_degrees = 0.0f;
        float next_spark_emit_seconds = 0.0f;
        bool alive = true;
    };
    std::vector<HammerImpactEffect> hammer_impact_effects_;
    std::vector<StormArcVisual> storm_arc_visuals_;
    std::unordered_map<int, bool> fire_storm_cast_impact_triggered_;
    std::unordered_map<int, bool> fire_storm_cast_arcs_spawned_;
    std::unordered_map<int, bool> fire_storm_cast_conversion_sfx_triggered_;
    struct ConsoleEntry {
        ConsoleMessage message;
        float age_seconds = 0.0f;
    };
    std::vector<ConsoleEntry> console_entries_;
    std::unordered_map<int, PlayerActionState> previous_player_action_states_;
    struct VolatileCastCasterFx {
        float age_seconds = 0.0f;
        float duration_seconds = 0.0f;
    };
    std::unordered_map<int, VolatileCastCasterFx> volatile_cast_caster_fx_;
    std::unordered_map<int, int> previous_player_hp_;
    std::unordered_map<int, float> player_damage_flash_remaining_;
    std::unordered_map<int, int> previous_object_hp_;
    std::unordered_map<int, float> object_damage_flash_remaining_;
    std::unordered_map<int, float> rooted_unit_damage_accumulators_;
    std::vector<float> castle_level_energy_requirements_ = {100.0f, 300.0f, 600.0f, 1000.0f};
    struct StaticDecorationRenderItem {
        float sort_y = 0.0f;
        size_t decoration_index = 0;
    };
    std::vector<std::vector<StaticDecorationRenderItem>> static_decoration_render_chunks_;
    int static_decoration_chunk_cols_ = 0;
    int static_decoration_chunk_rows_ = 0;

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
    struct LobbyMapEntry {
        std::string key;
        std::string label;
        std::string resolved_path;
    };
    std::vector<LobbyMapEntry> lobby_map_catalog_;
    int lobby_selected_map_index_ = 0;
    bool lobby_map_dropdown_edit_mode_ = false;
    int lobby_preview_generation_ = 0;
    int applied_lobby_preview_generation_ = -1;
    std::string lobby_selected_map_key_;
    std::string lobby_selected_map_label_ = "(no maps)";
    std::string applied_lobby_preview_map_key_;
    std::vector<uint8_t> lobby_preview_png_bytes_;
    std::string lobby_preview_status_text_ = "Preview unavailable";
    Texture2D lobby_map_preview_texture_ = {};
    bool has_lobby_map_preview_texture_ = false;
    std::string resolved_map_path_;
    std::string resolved_default_map_path_;
    std::string resolved_host_selected_map_path_;
    std::string resolved_client_cached_map_path_;
    std::string resolved_objects_config_path_;
    std::string resolved_composite_effects_path_;
    std::string resolved_sprite_metadata_path_;
    std::string resolved_sprite_metadata_tall_path_;
    std::string resolved_sprite_metadata_96x96_path_;
    std::string resolved_sprite_metadata_128x128_path_;
    std::string resolved_modular_player_main_path_;
    std::string resolved_modular_player_shadow_path_;
    std::string resolved_modular_tree_canopy_background_path_;
    std::string resolved_modular_tree_trunk_path_;
    std::string resolved_modular_tree_canopy_foreground_path_;
    std::string resolved_modular_tree_shadow_path_;
    std::string resolved_modular_tree_outline_mask_path_;
    std::string resolved_modular_tree_asset_metadata_path_;
    std::string resolved_spell_pattern_path_;
    std::string resolved_castle_level_requirements_path_;
    std::string resolved_equipment_profiles_path_;
    std::string resolved_hit_shapes_path_;
    std::string resolved_loot_tables_path_;
    std::string resolved_menu_background_path_;
    std::string resolved_occluder_reveal_shader_path_;
    std::string resolved_water_gradient_shader_path_;
    std::string resolved_zone_post_process_shader_path_;
    std::string resolved_zone_fill_overlay_shader_path_;
    std::string resolved_zone_border_overlay_shader_path_;
    std::string resolved_map_bounds_fade_shader_path_;
    std::string resolved_influence_zone_overlay_shader_path_;
    std::string resolved_damage_flash_shader_path_;
    std::string resolved_tree_composite_shader_path_;
    std::string resolved_tree_composite_no_reveal_shader_path_;
    std::string resolved_tree_wind_shader_path_;
    std::string main_menu_status_message_;
    bool main_menu_status_is_error_ = false;
    double connect_attempt_start_seconds_ = 0.0;
    MatchModeType lobby_mode_type_ = MatchModeType::MostKillsTimed;
    int lobby_round_time_seconds_ = Constants::kDefaultMatchDurationSeconds;
    int lobby_best_of_target_kills_ = Constants::kDefaultBestOfTargetKills;
    bool lobby_zone_enabled_ = true;
    float lobby_shrink_tiles_per_second_ = Constants::kDefaultShrinkTilesPerSecond;
    float lobby_shrink_start_seconds_ = Constants::kDefaultShrinkStartSeconds;
    float lobby_min_arena_radius_tiles_ = Constants::kDefaultMinArenaRadiusTiles;
    struct PendingMapTransfer {
        int transfer_id = 0;
        std::string map_key;
        std::string map_filename;
        uint32_t total_bytes = 0;
        uint32_t chunk_count = 0;
        uint32_t checksum = 0;
        std::vector<std::vector<uint8_t>> chunks;
        std::vector<bool> received_chunks;
        size_t received_bytes = 0;
        bool complete_received = false;
    };
    std::optional<PendingMapTransfer> pending_client_map_transfer_;
    int next_map_transfer_id_ = 1;
    int expected_match_transfer_id_ = 0;
    std::string expected_match_map_key_;
    std::string map_loading_status_text_ = "Waiting for map transfer...";

    float render_time_seconds_ = 0.0f;
    Vector2 modular_tree_anchor_pixels_ = {0.0f, 0.0f};
    bool has_modular_tree_anchor_pixels_ = false;
    bool force_windowed_launch_ = false;
    bool initial_fullscreen_setting_ = true;
    bool request_app_exit_ = false;
    bool show_network_debug_panel_ = true;
    bool network_debug_panel_minimized_ = false;
    bool in_game_menu_open_ = false;
    enum class InGameMenuPage {
        Home,
        Settings,
    };
    InGameMenuPage in_game_menu_page_ = InGameMenuPage::Home;
    enum class InventoryUiMode {
        Closed,
        Inventory,
        CastleLoadout,
    };
    InventoryUiMode local_inventory_ui_mode_ = InventoryUiMode::Closed;
    bool pending_open_initial_loadout_ui_ = false;

    bool pending_primary_pressed_ = false;
    bool pending_grappling_pressed_ = false;
    bool grappling_preview_armed_ = false;
    float grappling_preview_started_time_ = 0.0f;
    bool pending_escape_pressed_ = false;
    bool pending_enter_pressed_ = false;
    bool pending_enter_shift_down_ = false;
    bool pending_backspace_pressed_ = false;
    std::string pending_text_input_;
    bool escape_pressed_this_update_ = false;
    bool enter_pressed_this_update_ = false;
    bool enter_shift_down_this_update_ = false;
    bool backspace_pressed_this_update_ = false;
    std::string text_input_this_update_;
    bool chat_input_active_ = false;
    std::string chat_input_buffer_;
    int last_match_countdown_announcement_seconds_ = 11;
    int pending_select_rune_slot_ = -1;
    int pending_activate_item_slot_ = -1;
    bool pending_toggle_inventory_mode_ = false;
    bool pending_world_drop_request_ = false;
    SlotFamily pending_world_drop_family_ = SlotFamily::Item;
    int pending_world_drop_slot_index_ = -1;
    bool pending_world_drop_single_instance_ = false;
    std::optional<Player> pending_local_inventory_sync_;
    bool pending_local_inventory_sync_dirty_ = false;
    struct PendingObjectSpawn {
        std::string prototype_id;
        GridCoord cell;
        std::optional<int> blocked_player_id = std::nullopt;
        int reserved_object_id = -1;
    };
    std::vector<PendingObjectSpawn> pending_object_spawns_;
    struct DroppedItemPickupBlock {
        int object_instance_id = -1;
        int blocked_player_id = -1;
        Vector2 origin_world = {0.0f, 0.0f};
        float unlock_radius = 0.0f;
    };
    std::unordered_map<int, DroppedItemPickupBlock> dropped_item_pickup_blocks_;
    std::unordered_map<int, int> castle_charge_lightning_effect_ids_;
    std::unordered_map<std::string, int> loot_quota_remaining_;
    int next_predicted_entity_id_ = -1;
    std::mt19937 rng_;
    std::mt19937 visual_rng_;
    Texture2D menu_background_texture_ = {};
    bool has_menu_background_texture_ = false;
    RenderTexture2D world_layer_target_ = {};
    bool has_world_layer_target_ = false;
    RenderTexture2D shadow_layer_target_ = {};
    bool has_shadow_layer_target_ = false;
    Shader occluder_reveal_shader_ = {};
    bool has_occluder_reveal_shader_ = false;
    Shader water_gradient_shader_ = {};
    bool has_water_gradient_shader_ = false;
    Shader zone_post_process_shader_ = {};
    bool has_zone_post_process_shader_ = false;
    Shader zone_fill_overlay_shader_ = {};
    bool has_zone_fill_overlay_shader_ = false;
    Shader zone_border_overlay_shader_ = {};
    bool has_zone_border_overlay_shader_ = false;
    Shader map_bounds_fade_shader_ = {};
    bool has_map_bounds_fade_shader_ = false;
    Shader influence_zone_overlay_shader_ = {};
    bool has_influence_zone_overlay_shader_ = false;
    Shader damage_flash_shader_ = {};
    bool has_damage_flash_shader_ = false;
    Shader tree_composite_shader_ = {};
    bool has_tree_composite_shader_ = false;
    Shader tree_composite_no_reveal_shader_ = {};
    bool has_tree_composite_no_reveal_shader_ = false;
    Shader tree_wind_shader_ = {};
    bool has_tree_wind_shader_ = false;
    int occluder_reveal_count_loc_ = -1;
    int occluder_reveal_data_loc_ = -1;
    int occluder_reveal_screen_height_loc_ = -1;
    int occluder_reveal_inside_alpha_loc_ = -1;
    int occluder_reveal_source_rect_loc_ = -1;
    int water_gradient_screen_height_loc_ = -1;
    int water_gradient_start_loc_ = -1;
    int water_gradient_end_loc_ = -1;
    int zone_post_process_screen_height_loc_ = -1;
    int zone_post_process_camera_target_loc_ = -1;
    int zone_post_process_camera_offset_loc_ = -1;
    int zone_post_process_camera_zoom_loc_ = -1;
    int zone_post_process_zone_center_loc_ = -1;
    int zone_post_process_zone_radius_loc_ = -1;
    int zone_fill_overlay_screen_height_loc_ = -1;
    int zone_fill_overlay_camera_target_loc_ = -1;
    int zone_fill_overlay_camera_offset_loc_ = -1;
    int zone_fill_overlay_camera_zoom_loc_ = -1;
    int zone_fill_overlay_zone_center_loc_ = -1;
    int zone_fill_overlay_zone_radius_loc_ = -1;
    int zone_fill_overlay_zone_fill_rect_loc_ = -1;
    int zone_border_overlay_screen_height_loc_ = -1;
    int zone_border_overlay_camera_target_loc_ = -1;
    int zone_border_overlay_camera_offset_loc_ = -1;
    int zone_border_overlay_camera_zoom_loc_ = -1;
    int zone_border_overlay_zone_center_loc_ = -1;
    int zone_border_overlay_zone_radius_loc_ = -1;
    int map_bounds_fade_screen_height_loc_ = -1;
    int map_bounds_fade_camera_target_loc_ = -1;
    int map_bounds_fade_camera_offset_loc_ = -1;
    int map_bounds_fade_camera_zoom_loc_ = -1;
    int map_bounds_fade_fade_rect_min_loc_ = -1;
    int map_bounds_fade_fade_rect_max_loc_ = -1;
    int influence_zone_overlay_screen_height_loc_ = -1;
    int influence_zone_overlay_camera_target_loc_ = -1;
    int influence_zone_overlay_camera_offset_loc_ = -1;
    int influence_zone_overlay_camera_zoom_loc_ = -1;
    int influence_zone_overlay_map_size_world_loc_ = -1;
    int influence_zone_overlay_signed_distance_range_loc_ = -1;
    int influence_zone_overlay_tint_loc_ = -1;
    int influence_zone_overlay_pattern_phase_loc_ = -1;
    int influence_zone_overlay_pattern_frame_loc_ = -1;
    int influence_zone_overlay_to_distance_texture_loc_ = -1;
    int influence_zone_overlay_blend_t_loc_ = -1;
    int damage_flash_amount_loc_ = -1;
    int tree_composite_trunk_texture_loc_ = -1;
    int tree_composite_canopy_foreground_texture_loc_ = -1;
    int tree_composite_mask_texture_loc_ = -1;
    int tree_composite_canopy_background_rect_loc_ = -1;
    int tree_composite_trunk_rect_loc_ = -1;
    int tree_composite_canopy_foreground_rect_loc_ = -1;
    int tree_composite_mask_rect_loc_ = -1;
    int tree_composite_time_loc_ = -1;
    int tree_composite_sway_strength_loc_ = -1;
    int tree_composite_sway_speed_loc_ = -1;
    int tree_composite_phase_offset_loc_ = -1;
    int tree_composite_gradient_start_loc_ = -1;
    int tree_composite_screen_height_loc_ = -1;
    int tree_composite_inside_alpha_loc_ = -1;
    int tree_composite_reveal_count_loc_ = -1;
    int tree_composite_reveal_data_loc_ = -1;
    int tree_composite_no_reveal_trunk_texture_loc_ = -1;
    int tree_composite_no_reveal_canopy_foreground_texture_loc_ = -1;
    int tree_composite_no_reveal_mask_texture_loc_ = -1;
    int tree_composite_no_reveal_canopy_background_rect_loc_ = -1;
    int tree_composite_no_reveal_trunk_rect_loc_ = -1;
    int tree_composite_no_reveal_canopy_foreground_rect_loc_ = -1;
    int tree_composite_no_reveal_mask_rect_loc_ = -1;
    int tree_composite_no_reveal_time_loc_ = -1;
    int tree_composite_no_reveal_sway_strength_loc_ = -1;
    int tree_composite_no_reveal_sway_speed_loc_ = -1;
    int tree_composite_no_reveal_phase_offset_loc_ = -1;
    int tree_composite_no_reveal_gradient_start_loc_ = -1;
    int tree_wind_frame_rect_loc_ = -1;
    int tree_wind_time_loc_ = -1;
    int tree_wind_sway_strength_loc_ = -1;
    int tree_wind_sway_speed_loc_ = -1;
    int tree_wind_phase_offset_loc_ = -1;
    int tree_wind_gradient_start_loc_ = -1;
    float camera_shake_time_remaining_ = 0.0f;
    Texture2D influence_zone_distance_red_from_texture_ = {};
    Texture2D influence_zone_distance_blue_from_texture_ = {};
    Texture2D influence_zone_distance_red_to_texture_ = {};
    Texture2D influence_zone_distance_blue_to_texture_ = {};
    bool has_influence_zone_distance_red_from_texture_ = false;
    bool has_influence_zone_distance_blue_from_texture_ = false;
    bool has_influence_zone_distance_red_to_texture_ = false;
    bool has_influence_zone_distance_blue_to_texture_ = false;
    float influence_zone_transition_elapsed_seconds_ = Constants::kInfluenceZoneTransitionSeconds;
    uint64_t influence_zone_signature_ = 0;
    bool has_influence_zone_signature_ = false;
    std::unordered_map<int, float> fire_storm_dummy_lightning_seconds_remaining_;
    std::unordered_map<int, float> fire_storm_dummy_lightning_cooldown_seconds_remaining_;
    std::unordered_map<int, bool> fire_storm_cast_impact_played_;
    struct AltarInstance {
        int output_object_id = -1;
        GridCoord output_cell;
        std::vector<int> slot_object_ids;
        int required_slot_count = 0;
        std::vector<int> fulfilled_slot_rune_ids;
    };
    std::vector<AltarInstance> altars_;
    InfluenceDistanceField influence_zone_distance_red_from_field_;
    InfluenceDistanceField influence_zone_distance_blue_from_field_;
    InfluenceDistanceField influence_zone_distance_red_to_field_;
    InfluenceDistanceField influence_zone_distance_blue_to_field_;
    std::optional<InfluenceBuildRequest> pending_influence_build_request_;
    std::future<InfluenceBuildResult> influence_build_future_;
    bool influence_build_in_flight_ = false;
    int influence_build_generation_ = 0;

    bool audio_device_ready_ = false;
    LoadedSfx sfx_fireball_created_;
    LoadedSfx sfx_melee_attack_;
    LoadedSfx sfx_create_rune_;
    LoadedSfx sfx_volatile_cast_;
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
    LoadedSfx sfx_earth_rune_launch_;
    LoadedSfx sfx_earth_rune_impact_;
    LoadedSfx sfx_castle_level_up_;
    LoadedSfx sfx_castle_equip_rune_;
    LoadedSfx sfx_castle_unequip_rune_;
    std::array<LoadedSfx, 3> sfx_ice_wave_cast_{};
    std::array<LoadedSfx, 3> sfx_ice_wave_impact_{};
    std::array<LoadedSfx, 2> sfx_hammer_swing_{};
    std::array<LoadedSfx, 3> sfx_hammer_impact_{};
    std::array<LoadedSfx, 3> sfx_zone_damage_{};
    std::array<LoadedSfx, 5> sfx_footstep_dirt_{};
    Music bgm_forest_day_ = {};
    bool has_bgm_forest_day_ = false;
    Music bgm_outside_game_ = {};
    bool has_bgm_outside_game_ = false;
    bool bgm_forest_day_active_last_frame_ = false;
    bool bgm_outside_game_active_last_frame_ = false;
    Music fire_storm_ambient_ = {};
    bool has_fire_storm_ambient_ = false;
    float fire_storm_ambient_gain_ = 0.0f;
    Vector2 local_footstep_prev_pos_ = {0.0f, 0.0f};
    bool has_local_footstep_prev_pos_ = false;
    float local_footstep_distance_accumulator_ = 0.0f;
};
