#include "core/game_app.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <random>
#include <string>
#include <unordered_set>

#include <raygui.h>
#include <raymath.h>

#include "collision/collision_world.h"
#include "core/constants.h"
#include "spells/fire_bolt_spell.h"
#include "spells/ice_wall_spell.h"
#include "ui/ui_lobby.h"
#include "ui/ui_main_menu.h"
#include "ui/ui_match.h"

namespace {

float ClampDt(float dt) { return std::min(dt, 0.1f); }

int DetermineWinningTeam(const MatchState& match) {
    if (match.red_team_kills > match.blue_team_kills) {
        return Constants::kTeamRed;
    }
    if (match.blue_team_kills > match.red_team_kills) {
        return Constants::kTeamBlue;
    }
    return -1;
}

int64_t MakeGridKey(const GridCoord& cell) {
    return (static_cast<int64_t>(cell.x) << 32) ^ static_cast<uint32_t>(cell.y);
}

std::vector<GridCoord> CollectConnectedActiveIceWallCells(const std::vector<IceWallPiece>& walls, const GridCoord& seed) {
    std::unordered_set<int64_t> active_cells;
    active_cells.reserve(walls.size() * 2);
    for (const auto& wall : walls) {
        if (wall.alive && wall.state == IceWallState::Active) {
            active_cells.insert(MakeGridKey(wall.cell));
        }
    }

    if (!active_cells.count(MakeGridKey(seed))) {
        return {};
    }

    std::vector<GridCoord> stack = {seed};
    std::unordered_set<int64_t> visited;
    visited.reserve(active_cells.size());
    visited.insert(MakeGridKey(seed));

    std::vector<GridCoord> component;
    component.reserve(active_cells.size());
    while (!stack.empty()) {
        const GridCoord current = stack.back();
        stack.pop_back();
        component.push_back(current);

        const GridCoord neighbors[4] = {
            {current.x - 1, current.y},
            {current.x + 1, current.y},
            {current.x, current.y - 1},
            {current.x, current.y + 1},
        };
        for (const GridCoord& neighbor : neighbors) {
            const int64_t key = MakeGridKey(neighbor);
            if (active_cells.count(key) && visited.insert(key).second) {
                stack.push_back(neighbor);
            }
        }
    }

    return component;
}

bool IntersectsTileComponent(const Vector2& center, float radius, int cell_size, const std::vector<GridCoord>& cells) {
    for (const GridCoord& cell : cells) {
        const Rectangle aabb = {static_cast<float>(cell.x * cell_size), static_cast<float>(cell.y * cell_size),
                                static_cast<float>(cell_size), static_cast<float>(cell_size)};
        Vector2 normal = {0.0f, 0.0f};
        float penetration = 0.0f;
        if (CollisionWorld::CircleVsAabb(center, radius, aabb, normal, penetration)) {
            return true;
        }
    }
    return false;
}

Vector2 MoveTowardResolvedPosition(const Vector2& current, const Vector2& resolved) {
    const Vector2 delta = Vector2Subtract(resolved, current);
    const float distance = Vector2Length(delta);
    if (distance <= Constants::kCollisionResolveSnapDistance || distance <= 0.0001f) {
        return resolved;
    }

    const float step =
        std::min(distance, std::max(Constants::kCollisionResolveMinStep, distance * Constants::kCollisionResolveBlendFactor));
    return Vector2Add(current, Vector2Scale(delta, step / distance));
}

bool IsHorizontalDirection(SpellDirection direction) {
    return direction == SpellDirection::Horizontal || direction == SpellDirection::Left ||
           direction == SpellDirection::Right;
}

bool HasStatusEffect(const Player& player, StatusEffectType type) {
    return std::any_of(player.status_effects.begin(), player.status_effects.end(),
                       [&](const StatusEffectInstance& status) {
                           return status.type == type && status.remaining_seconds > 0.0f;
                       });
}

float SmoothRootSigmoid(float t) {
    const float clamped = std::clamp(t, 0.0f, 1.0f);
    const float x = (clamped - 0.5f) * 10.0f;
    const float s = 1.0f / (1.0f + std::exp(-x));
    const float kMin = 1.0f / (1.0f + std::exp(5.0f));
    const float kMax = 1.0f / (1.0f + std::exp(-5.0f));
    return (s - kMin) / (kMax - kMin);
}

bool IsStaticFireBolt(const Projectile& projectile) {
    return projectile.animation_key == "fire_bolt_static" || projectile.animation_key == "fire_storm_static_large";
}

struct StatusEffectUiSpec {
    const char* animation = nullptr;
    bool is_buff = true;
};

StatusEffectUiSpec GetStatusEffectUiSpec(StatusEffectType type) {
    switch (type) {
        case StatusEffectType::Regeneration:
            return {"health_potion_small", true};
        case StatusEffectType::Stunned:
            return {"static_upgrade", false};
        case StatusEffectType::Rooted:
            return {"earth_rune_rooted_idle", false};
        case StatusEffectType::RootedRecovery:
            return {"earth_rune_rooted_idle", false};
    }
    return {"altar_rune_slot_generic", true};
}

Color GetRuneFallbackColor(RuneType rune_type) {
    switch (rune_type) {
        case RuneType::Fire:
            return Color{255, 140, 44, 220};
        case RuneType::Water:
            return Color{66, 180, 255, 220};
        case RuneType::Catalyst:
            return Color{208, 218, 116, 220};
        case RuneType::Earth:
            return Color{143, 110, 64, 220};
        case RuneType::FireStormDummy:
            return Color{255, 158, 64, 220};
        case RuneType::None:
            return Color{255, 255, 255, 0};
    }
    return Color{255, 255, 255, 0};
}

float GetRuneManaCost(RuneType rune_type) {
    switch (rune_type) {
        case RuneType::Fire:
            return Constants::kRuneManaCostFire;
        case RuneType::Water:
            return Constants::kRuneManaCostWater;
        case RuneType::Catalyst:
            return Constants::kRuneManaCostCatalyst;
        case RuneType::Earth:
            return Constants::kRuneManaCostEarth;
        case RuneType::FireStormDummy:
            return Constants::kRuneManaCostFireStormDummy;
        case RuneType::None:
            return 0.0f;
    }
    return 0.0f;
}

float GetRuneCooldownSeconds(RuneType rune_type) {
    switch (rune_type) {
        case RuneType::Fire:
            return Constants::kRuneCooldownFireSeconds;
        case RuneType::Water:
            return Constants::kRuneCooldownWaterSeconds;
        case RuneType::Catalyst:
            return Constants::kRuneCooldownCatalystSeconds;
        case RuneType::Earth:
            return Constants::kRuneCooldownEarthSeconds;
        case RuneType::FireStormDummy:
            return Constants::kRuneCooldownFireStormDummySeconds;
        case RuneType::None:
            return 0.0f;
    }
    return 0.0f;
}

float GetRuneActivationSeconds(RuneType rune_type) {
    switch (rune_type) {
        case RuneType::Fire:
            return Constants::kRuneActivationFireSeconds;
        case RuneType::Water:
            return Constants::kRuneActivationWaterSeconds;
        case RuneType::Catalyst:
            return Constants::kRuneActivationCatalystSeconds;
        case RuneType::Earth:
            return Constants::kRuneActivationEarthSeconds;
        case RuneType::FireStormDummy:
            return Constants::kRuneActivationFireStormDummySeconds;
        case RuneType::None:
            return 0.0f;
    }
    return 0.0f;
}

size_t GetRuneCooldownIndex(RuneType rune_type) {
    switch (rune_type) {
        case RuneType::Fire:
            return 0;
        case RuneType::Water:
            return 1;
        case RuneType::Catalyst:
            return 2;
        case RuneType::Earth:
            return 3;
        case RuneType::FireStormDummy:
            return 4;
        case RuneType::None:
            break;
    }
    return 0;
}

float GetPlayerRuneCooldownRemaining(const Player& player, RuneType rune_type) {
    if (rune_type == RuneType::None) {
        return 0.0f;
    }
    return player.rune_cooldown_remaining[GetRuneCooldownIndex(rune_type)];
}

float GetPlayerRuneCooldownTotal(const Player& player, RuneType rune_type) {
    if (rune_type == RuneType::None) {
        return 0.0f;
    }
    return player.rune_cooldown_total[GetRuneCooldownIndex(rune_type)];
}

void SetPlayerRuneCooldown(Player& player, RuneType rune_type, float remaining, float total) {
    if (rune_type == RuneType::None) {
        return;
    }
    const size_t index = GetRuneCooldownIndex(rune_type);
    player.rune_cooldown_remaining[index] = remaining;
    player.rune_cooldown_total[index] = total;
}

const char* GetRuneSpriteAnimationKey(RuneType rune_type) {
    switch (rune_type) {
        case RuneType::Fire:
            return "fire_rune";
        case RuneType::Water:
            return "water_rune";
        case RuneType::Catalyst:
            return "catalist_rune";
        case RuneType::Earth:
            return "earth_rune_idle";
        case RuneType::FireStormDummy:
            return "fire_storm_bottom";
        case RuneType::None:
            return "";
    }
    return "";
}

const char* GetRuneBornSpriteAnimationKey(RuneType rune_type) {
    switch (rune_type) {
        case RuneType::Fire:
            return "fire_rune_born";
        case RuneType::Water:
            return "water_rune_born";
        case RuneType::Catalyst:
            return "catalist_rune_born";
        case RuneType::Earth:
            return "roots_born";
        case RuneType::FireStormDummy:
            return "fire_storm_born_bot";
        case RuneType::None:
            return "rune_born";
    }
    return "rune_born";
}

const char* GetEarthRuneAnimationKey(const Rune& rune) {
    switch (rune.earth_trap_state) {
        case EarthRuneTrapState::IdleRune:
            return "earth_rune_idle";
        case EarthRuneTrapState::Slamming:
            return "earth_rune_slam";
        case EarthRuneTrapState::RootedIdle:
            return "earth_rune_rooted_idle";
        case EarthRuneTrapState::RootedDeath:
            return "earth_rune_rooted_death";
    }
    return "earth_rune_idle";
}

float AimToDegrees(Vector2 aim_dir) {
    return static_cast<float>(std::atan2(aim_dir.y, aim_dir.x) * (180.0 / PI));
}

Rectangle SnapRect(Rectangle rect) {
    rect.x = std::roundf(rect.x);
    rect.y = std::roundf(rect.y);
    rect.width = std::roundf(rect.width);
    rect.height = std::roundf(rect.height);
    return rect;
}

Rectangle InsetSourceRect(Rectangle src, float inset_pixels) {
    if (inset_pixels <= 0.0f) {
        return src;
    }

    const float sign_w = src.width >= 0.0f ? 1.0f : -1.0f;
    const float sign_h = src.height >= 0.0f ? 1.0f : -1.0f;
    const float abs_w = std::fabs(src.width);
    const float abs_h = std::fabs(src.height);

    if (abs_w > inset_pixels * 2.0f) {
        src.x += inset_pixels * sign_w;
        src.width = sign_w * (abs_w - inset_pixels * 2.0f);
    }
    if (abs_h > inset_pixels * 2.0f) {
        src.y += inset_pixels * sign_h;
        src.height = sign_h * (abs_h - inset_pixels * 2.0f);
    }
    return src;
}

std::string ResolveRuntimePath(const char* relative_path) {
    namespace fs = std::filesystem;
    const fs::path direct(relative_path);
    if (fs::exists(direct)) {
        return direct.string();
    }

    const fs::path app_dir(GetApplicationDirectory());
    const fs::path near_exe = app_dir / direct;
    if (fs::exists(near_exe)) {
        return near_exe.string();
    }

    return direct.string();
}

bool IsColumnPrototypeId(const std::string& prototype_id) {
    return prototype_id.rfind("column_", 0) == 0;
}

Rectangle GetMapObjectCollisionAabb(const MapObjectInstance& object, int cell_size) {
    Rectangle aabb = {static_cast<float>(object.cell.x * cell_size),
                      static_cast<float>(object.cell.y * cell_size),
                      static_cast<float>(cell_size),
                      static_cast<float>(cell_size)};
    if (!IsColumnPrototypeId(object.prototype_id)) {
        return aabb;
    }

    const float scale = 0.8f;
    const float inset = (1.0f - scale) * 0.5f * static_cast<float>(cell_size);
    aabb.x += inset;
    aabb.y += inset;
    aabb.width *= scale;
    aabb.height *= scale;
    return aabb;
}

bool IsLoadedSound(const Sound& sound) {
    return sound.frameCount > 0 && sound.stream.buffer != nullptr;
}

bool IsLoadedMusic(const Music& music) {
    return music.stream.buffer != nullptr && music.ctxData != nullptr;
}

}  // namespace

GameApp::GameApp(bool force_windowed_launch)
    : force_windowed_launch_(force_windowed_launch),
      rng_(std::random_device{}()),
      visual_rng_(std::random_device{}()) {}

bool GameApp::Initialize() {
    const bool loaded_settings = config_manager_.Load(settings_);
    if (!loaded_settings) {
        settings_.fullscreen = true;
    }
    settings_.lobby_shrink_tiles_per_second =
        std::clamp(settings_.lobby_shrink_tiles_per_second, 0.0f, Constants::kMaxShrinkTilesPerSecond);
    settings_.lobby_min_arena_radius_tiles =
        std::clamp(settings_.lobby_min_arena_radius_tiles, 0.0f, Constants::kMaxArenaRadiusTiles);
    settings_.lobby_match_length_seconds =
        std::max(Constants::kLobbyTimeStepSeconds, settings_.lobby_match_length_seconds);
    settings_.lobby_best_of_target_kills = std::max(1, settings_.lobby_best_of_target_kills | 1);
    settings_.lobby_shrink_start_seconds = std::max(0.0f, settings_.lobby_shrink_start_seconds);
    settings_.lobby_mode_type =
        (settings_.lobby_mode_type == static_cast<int>(MatchModeType::BestOfKills))
            ? static_cast<int>(MatchModeType::BestOfKills)
            : static_cast<int>(MatchModeType::MostKillsTimed);
    lobby_mode_type_ = static_cast<MatchModeType>(settings_.lobby_mode_type);
    lobby_round_time_seconds_ = settings_.lobby_match_length_seconds;
    lobby_best_of_target_kills_ = settings_.lobby_best_of_target_kills;
    lobby_shrink_tiles_per_second_ = settings_.lobby_shrink_tiles_per_second;
    lobby_shrink_start_seconds_ = settings_.lobby_shrink_start_seconds;
    lobby_min_arena_radius_tiles_ = settings_.lobby_min_arena_radius_tiles;
    show_network_debug_panel_ = settings_.show_network_debug_panel;
    initial_fullscreen_setting_ = settings_.fullscreen;
    controls_manager_.Load(controls_bindings_);
    controls_manager_.Save(controls_bindings_);

    std::snprintf(player_name_buffer_, sizeof(player_name_buffer_), "%s", settings_.player_name.c_str());
    std::snprintf(join_ip_buffer_, sizeof(join_ip_buffer_), "%s", "127.0.0.1");

    InitWindow(settings_.window_width, settings_.window_height, "Rune Arena");
    SetTargetFPS(144);
    const bool launch_fullscreen = !force_windowed_launch_;
    if (launch_fullscreen && !IsWindowFullscreen()) {
        ToggleFullscreen();
    }

    InitAudioDevice();
    audio_device_ready_ = IsAudioDeviceReady();

    // Basic dark UI look.
    GuiSetStyle(DEFAULT, BACKGROUND_COLOR, ColorToInt(Color{25, 28, 34, 255}));
    GuiSetStyle(DEFAULT, TEXT_COLOR_NORMAL, ColorToInt(Color{221, 228, 245, 255}));
    GuiSetStyle(DEFAULT, BASE_COLOR_NORMAL, ColorToInt(Color{42, 47, 57, 255}));
    GuiSetStyle(DEFAULT, BASE_COLOR_FOCUSED, ColorToInt(Color{54, 62, 75, 255}));

    resolved_map_path_ = ResolveRuntimePath(Constants::kDefaultMapPath);
    resolved_objects_config_path_ = ResolveRuntimePath(Constants::kObjectsConfigPath);
    resolved_composite_effects_path_ = ResolveRuntimePath(Constants::kCompositeEffectsPath);
    resolved_sprite_metadata_path_ = ResolveRuntimePath(Constants::kSpriteMetadataPath);
    resolved_sprite_metadata_tall_path_ = ResolveRuntimePath(Constants::kSpriteMetadataTallPath);
    resolved_sprite_metadata_96x96_path_ = ResolveRuntimePath(Constants::kSpriteMetadata96x96Path);
    resolved_spell_pattern_path_ = ResolveRuntimePath(Constants::kSpellPatternPath);
    resolved_menu_background_path_ = ResolveRuntimePath(Constants::kMenuBackgroundPath);

    objects_database_.LoadFromFile(resolved_objects_config_path_);
    composite_effects_loader_.LoadFromFile(resolved_composite_effects_path_);

    if (!map_loader_.Load(resolved_map_path_, &objects_database_, state_.map)) {
        // Fallback: small grass-only map.
        state_.map.width = 24;
        state_.map.height = 16;
        state_.map.cell_size = Constants::kRuneCellSize;
        state_.map.tiles.assign(static_cast<size_t>(state_.map.width * state_.map.height), TileType::Grass);
        state_.map.decorations.assign(static_cast<size_t>(state_.map.width * state_.map.height), "");
        state_.map.object_seeds.clear();
        state_.map.spawn_points = {{2, 2}, {state_.map.width - 3, state_.map.height - 3}};
    }

    sprite_metadata_.LoadFromFile(resolved_sprite_metadata_path_);
    sprite_metadata_tall_.LoadFromFile(resolved_sprite_metadata_tall_path_);
    sprite_metadata_96x96_.LoadFromFile(resolved_sprite_metadata_96x96_path_);
    spell_patterns_.LoadFromFile(resolved_spell_pattern_path_);
    if (FileExists(resolved_menu_background_path_.c_str())) {
        menu_background_texture_ = LoadTexture(resolved_menu_background_path_.c_str());
        has_menu_background_texture_ = (menu_background_texture_.id != 0);
    }
    LoadAudioAssets();
    camera_.target = {0.0f, 0.0f};
    camera_.offset = {static_cast<float>(settings_.window_width) * 0.5f,
                      static_cast<float>(settings_.window_height) * 0.5f};
    camera_.rotation = 0.0f;
    camera_.zoom = Constants::kCameraZoom;

    app_screen_ = AppScreen::MainMenu;
    if (!lan_discovery_.StartClientListener()) {
        main_menu_status_message_ = TextFormat("Discovery listener failed on UDP %d", Constants::kDiscoveryPort);
        main_menu_status_is_error_ = true;
    }
    return true;
}

void GameApp::Run() {
    double previous_time = GetTime();
    double accumulator = 0.0;

    while (!WindowShouldClose()) {
        const double now = GetTime();
        double frame_dt = now - previous_time;
        previous_time = now;

        frame_dt = std::min(frame_dt, 0.25);
        accumulator += frame_dt;
        render_time_seconds_ += static_cast<float>(frame_dt);
        CaptureFrameInputEdges();

        network_manager_.Poll();
        lan_discovery_.Update();
        UpdateAudioFrame();

        while (accumulator >= Constants::kFixedDt) {
            Update(static_cast<float>(Constants::kFixedDt));
            accumulator -= Constants::kFixedDt;
        }

        Render();
    }
}

void GameApp::CaptureFrameInputEdges() {
    pending_primary_pressed_ = pending_primary_pressed_ || IsBindingPressed(controls_bindings_.primary_action);
    pending_grappling_pressed_ = pending_grappling_pressed_ || IsBindingPressed(controls_bindings_.grappling_hook_action);
    if (IsBindingPressed(controls_bindings_.select_rune_slot_1)) pending_select_rune_slot_ = 0;
    if (IsBindingPressed(controls_bindings_.select_rune_slot_2)) pending_select_rune_slot_ = 1;
    if (IsBindingPressed(controls_bindings_.select_rune_slot_3)) pending_select_rune_slot_ = 2;
    if (IsBindingPressed(controls_bindings_.select_rune_slot_4)) pending_select_rune_slot_ = 3;
    if (IsBindingPressed(controls_bindings_.activate_item_slot_1)) pending_activate_item_slot_ = 0;
    if (IsBindingPressed(controls_bindings_.activate_item_slot_2)) pending_activate_item_slot_ = 1;
    if (IsBindingPressed(controls_bindings_.activate_item_slot_3)) pending_activate_item_slot_ = 2;
    if (IsBindingPressed(controls_bindings_.activate_item_slot_4)) pending_activate_item_slot_ = 3;
    pending_toggle_inventory_mode_ =
        pending_toggle_inventory_mode_ || IsBindingPressed(controls_bindings_.toggle_inventory_mode);
}

void GameApp::Shutdown() {
    std::string saved_name(player_name_buffer_);
    if (!saved_name.empty()) {
        settings_.player_name = saved_name;
    }
    settings_.window_width = GetScreenWidth();
    settings_.window_height = GetScreenHeight();
    if (force_windowed_launch_) {
        settings_.fullscreen = initial_fullscreen_setting_;
    } else {
        settings_.fullscreen = IsWindowFullscreen();
    }
    settings_.lobby_shrink_tiles_per_second = lobby_shrink_tiles_per_second_;
    settings_.lobby_mode_type = static_cast<int>(lobby_mode_type_);
    settings_.lobby_match_length_seconds = lobby_round_time_seconds_;
    settings_.lobby_best_of_target_kills = lobby_best_of_target_kills_;
    settings_.lobby_shrink_start_seconds = lobby_shrink_start_seconds_;
    settings_.lobby_min_arena_radius_tiles = lobby_min_arena_radius_tiles_;
    settings_.show_network_debug_panel = show_network_debug_panel_;
    config_manager_.Save(settings_);

    lan_discovery_.Stop();
    network_manager_.Stop();
    UnloadAudioAssets();
    sprite_metadata_.Unload();
    sprite_metadata_tall_.Unload();
    sprite_metadata_96x96_.Unload();
    if (has_menu_background_texture_) {
        UnloadTexture(menu_background_texture_);
        has_menu_background_texture_ = false;
    }
    if (audio_device_ready_) {
        CloseAudioDevice();
        audio_device_ready_ = false;
    }
    CloseWindow();
}

void GameApp::Update(float dt) {
    dt = ClampDt(dt);
    switch (app_screen_) {
        case AppScreen::MainMenu:
            UpdateMainMenu(dt);
            break;
        case AppScreen::Connecting:
            UpdateConnecting(dt);
            break;
        case AppScreen::Lobby:
            UpdateLobby(dt);
            break;
        case AppScreen::InMatch:
            UpdateMatch(dt);
            break;
        case AppScreen::PostMatch:
            UpdatePostMatch(dt);
            break;
    }
}

void GameApp::UpdateMainMenu(float /*dt*/) {
    // Discovery keeps running so joinable hosts appear while user is in the menu.
}

void GameApp::LoadAudioAssets() {
    if (!audio_device_ready_) {
        return;
    }

    auto load_sfx = [&](LoadedSfx& clip, const char* path, float volume) {
        const std::string resolved = ResolveRuntimePath(path);
        if (!FileExists(resolved.c_str())) {
            clip.loaded = false;
            return;
        }
        clip.sound = LoadSound(resolved.c_str());
        clip.loaded = IsLoadedSound(clip.sound);
        if (clip.loaded) {
            SetSoundVolume(clip.sound, volume);
        }
    };

    load_sfx(sfx_fireball_created_, Constants::kSfxFireballCreatedPath, Constants::kSfxVolumeFireballCreated);
    load_sfx(sfx_melee_attack_, Constants::kSfxMeleeAttackPath, Constants::kSfxVolumeMeleeAttack);
    load_sfx(sfx_create_rune_, Constants::kSfxCreateRunePath, Constants::kSfxVolumeCreateRune);
    load_sfx(sfx_explosion_, Constants::kSfxExplosionPath, Constants::kSfxVolumeExplosion);
    load_sfx(sfx_vase_breaking_, Constants::kSfxVaseBreakingPath, Constants::kSfxVolumeVaseBreaking);
    load_sfx(sfx_ice_wall_freeze_, Constants::kSfxIceWallFreezePath, Constants::kSfxVolumeIceWallFreeze);
    load_sfx(sfx_ice_wall_melt_, Constants::kSfxIceWallMeltPath, Constants::kSfxVolumeIceWallMelt);
    load_sfx(sfx_player_death_, Constants::kSfxPlayerDeathPath, Constants::kSfxVolumePlayerDeath);
    load_sfx(sfx_player_damaged_, Constants::kSfxPlayerDamagedPath, Constants::kSfxVolumePlayerDamaged);
    load_sfx(sfx_item_pickup_, Constants::kSfxItemPickupPath, Constants::kSfxVolumeItemPickup);
    load_sfx(sfx_drink_potion_, Constants::kSfxDrinkPotionPath, Constants::kSfxVolumeDrinkPotion);
    load_sfx(sfx_fire_storm_cast_, Constants::kSfxFireStormCastPath, Constants::kSfxVolumeFireStormCast);
    load_sfx(sfx_fire_storm_impact_, Constants::kSfxFireStormImpactPath, Constants::kSfxVolumeFireStormImpact);
    load_sfx(sfx_static_upgrade_, Constants::kSfxStaticUpgradePath, Constants::kSfxVolumeStaticUpgrade);
    load_sfx(sfx_static_bolt_impact_, Constants::kSfxStaticBoltImpactPath, Constants::kSfxVolumeStaticBoltImpact);
    load_sfx(sfx_grappling_throw_, Constants::kSfxGrapplingThrowPath, Constants::kSfxVolumeGrapplingThrow);
    load_sfx(sfx_grappling_latch_, Constants::kSfxGrapplingLatchPath, Constants::kSfxVolumeGrapplingLatch);
    load_sfx(sfx_earth_rune_launch_, Constants::kSfxEarthRuneLaunchPath, Constants::kSfxVolumeEarthRuneLaunch);
    load_sfx(sfx_earth_rune_impact_, Constants::kSfxEarthRuneImpactPath, Constants::kSfxVolumeEarthRuneImpact);
    for (size_t i = 0; i < sfx_footstep_dirt_.size(); ++i) {
        load_sfx(sfx_footstep_dirt_[i], Constants::kSfxFootstepDirtPaths[i], Constants::kSfxVolumeFootstepDirt);
    }

    auto load_music = [&](Music& music, bool& loaded, const char* path) {
        const std::string resolved = ResolveRuntimePath(path);
        if (!FileExists(resolved.c_str())) {
            loaded = false;
            return;
        }
        music = LoadMusicStream(resolved.c_str());
        loaded = IsLoadedMusic(music);
        if (loaded) {
            music.looping = false;
            SetMusicVolume(music, 0.0f);
        }
    };

    load_music(fire_storm_ambient_, has_fire_storm_ambient_, Constants::kFireStormAmbientPath);
    if (has_fire_storm_ambient_) {
        SeekMusicStream(fire_storm_ambient_, Constants::kFireStormAmbientLoopStartSeconds);
    }

    const std::string bgm_path = ResolveRuntimePath(Constants::kBgmForestDayPath);
    if (FileExists(bgm_path.c_str())) {
        bgm_forest_day_ = LoadMusicStream(bgm_path.c_str());
        has_bgm_forest_day_ = IsLoadedMusic(bgm_forest_day_);
        if (has_bgm_forest_day_) {
            SetMusicVolume(bgm_forest_day_, Constants::kBgmVolume);
            PlayMusicStream(bgm_forest_day_);
        }
    }
}

void GameApp::UnloadAudioAssets() {
    if (!audio_device_ready_) {
        return;
    }

    auto unload_sfx = [](LoadedSfx& clip) {
        if (!clip.loaded) {
            return;
        }
        UnloadSound(clip.sound);
        clip = LoadedSfx{};
    };

    unload_sfx(sfx_fireball_created_);
    unload_sfx(sfx_melee_attack_);
    unload_sfx(sfx_create_rune_);
    unload_sfx(sfx_explosion_);
    unload_sfx(sfx_vase_breaking_);
    unload_sfx(sfx_ice_wall_freeze_);
    unload_sfx(sfx_ice_wall_melt_);
    unload_sfx(sfx_player_death_);
    unload_sfx(sfx_player_damaged_);
    unload_sfx(sfx_item_pickup_);
    unload_sfx(sfx_drink_potion_);
    unload_sfx(sfx_fire_storm_cast_);
    unload_sfx(sfx_fire_storm_impact_);
    unload_sfx(sfx_static_upgrade_);
    unload_sfx(sfx_static_bolt_impact_);
    unload_sfx(sfx_grappling_throw_);
    unload_sfx(sfx_grappling_latch_);
    unload_sfx(sfx_earth_rune_launch_);
    unload_sfx(sfx_earth_rune_impact_);
    for (auto& clip : sfx_footstep_dirt_) {
        unload_sfx(clip);
    }

    if (has_fire_storm_ambient_) {
        StopMusicStream(fire_storm_ambient_);
        UnloadMusicStream(fire_storm_ambient_);
        has_fire_storm_ambient_ = false;
    }

    if (has_bgm_forest_day_) {
        StopMusicStream(bgm_forest_day_);
        UnloadMusicStream(bgm_forest_day_);
        has_bgm_forest_day_ = false;
    }
}

void GameApp::UpdateAudioFrame() {
    if (!audio_device_ready_) {
        return;
    }
    UpdateLocalFootstepAudio();
    if (has_bgm_forest_day_) {
        UpdateMusicStream(bgm_forest_day_);
    }

    if (!has_fire_storm_ambient_) {
        return;
    }

    UpdateMusicStream(fire_storm_ambient_);

    const float dt = GetFrameTime();
    const bool should_play = HasVisibleIdleFireStormDummy();
    const float fade_speed = (Constants::kFireStormAmbientFadeSeconds > 0.0f)
                                 ? (1.0f / Constants::kFireStormAmbientFadeSeconds)
                                 : 1000.0f;
    const float target_gain = should_play ? 1.0f : 0.0f;
    if (fire_storm_ambient_gain_ < target_gain) {
        fire_storm_ambient_gain_ = std::min(target_gain, fire_storm_ambient_gain_ + dt * fade_speed);
    } else if (fire_storm_ambient_gain_ > target_gain) {
        fire_storm_ambient_gain_ = std::max(target_gain, fire_storm_ambient_gain_ - dt * fade_speed);
    }

    if (should_play && !IsMusicStreamPlaying(fire_storm_ambient_)) {
        SeekMusicStream(fire_storm_ambient_, Constants::kFireStormAmbientLoopStartSeconds);
        PlayMusicStream(fire_storm_ambient_);
    }

    if (IsMusicStreamPlaying(fire_storm_ambient_)) {
        const float played = GetMusicTimePlayed(fire_storm_ambient_);
        if (played >= Constants::kFireStormAmbientLoopEndSeconds) {
            SeekMusicStream(fire_storm_ambient_, Constants::kFireStormAmbientLoopStartSeconds);
        }
    }

    SetMusicVolume(fire_storm_ambient_, Constants::kFireStormAmbientVolume * fire_storm_ambient_gain_);

    if (fire_storm_ambient_gain_ <= 0.001f) {
        if (IsMusicStreamPlaying(fire_storm_ambient_)) {
            StopMusicStream(fire_storm_ambient_);
            SeekMusicStream(fire_storm_ambient_, Constants::kFireStormAmbientLoopStartSeconds);
        }
        SetMusicVolume(fire_storm_ambient_, 0.0f);
    }
}

void GameApp::UpdateLocalFootstepAudio() {
    if (state_.local_player_id < 0) {
        has_local_footstep_prev_pos_ = false;
        local_footstep_distance_accumulator_ = 0.0f;
        return;
    }

    const Player* local_player = FindPlayerById(state_.local_player_id);
    if (local_player == nullptr || !local_player->alive) {
        has_local_footstep_prev_pos_ = false;
        local_footstep_distance_accumulator_ = 0.0f;
        return;
    }

    const bool deliberate_movement = IsBindingDown(controls_bindings_.move_left) ||
                                     IsBindingDown(controls_bindings_.move_right) ||
                                     IsBindingDown(controls_bindings_.move_up) ||
                                     IsBindingDown(controls_bindings_.move_down);
    const bool blocked = HasStatusEffect(*local_player, StatusEffectType::Stunned) ||
                         IsPlayerBeingPulled(local_player->id);
    const float step_distance =
        static_cast<float>(std::max(1, state_.map.cell_size)) * (2.0f / 3.0f);

    if (!has_local_footstep_prev_pos_) {
        local_footstep_prev_pos_ = local_player->pos;
        has_local_footstep_prev_pos_ = true;
        local_footstep_distance_accumulator_ = 0.0f;
        return;
    }

    const float frame_distance = Vector2Distance(local_footstep_prev_pos_, local_player->pos);
    local_footstep_prev_pos_ = local_player->pos;

    if (!deliberate_movement || blocked) {
        local_footstep_distance_accumulator_ = 0.0f;
        return;
    }

    local_footstep_distance_accumulator_ += frame_distance;
    while (local_footstep_distance_accumulator_ >= step_distance) {
        local_footstep_distance_accumulator_ -= step_distance;
        std::uniform_int_distribution<int> clip_dist(0, static_cast<int>(sfx_footstep_dirt_.size()) - 1);
        LoadedSfx& clip = sfx_footstep_dirt_[static_cast<size_t>(clip_dist(visual_rng_))];
        PlaySfxIfVisible(clip.sound, clip.loaded, local_player->pos);
    }
}

Vector2 GameApp::GetRenderGrapplingHookHeadPosition(int hook_id, Vector2 fallback) const {
    auto it = render_grappling_hook_head_positions_.find(hook_id);
    if (it != render_grappling_hook_head_positions_.end()) {
        return it->second;
    }
    return fallback;
}

bool GameApp::IsWorldPointInsideCameraView(Vector2 world_pos) const {
    if (camera_.zoom <= 0.0001f) {
        return false;
    }
    const float view_w = static_cast<float>(GetScreenWidth()) / camera_.zoom;
    const float view_h = static_cast<float>(GetScreenHeight()) / camera_.zoom;
    const float left = camera_.target.x - camera_.offset.x / camera_.zoom;
    const float top = camera_.target.y - camera_.offset.y / camera_.zoom;
    const float right = left + view_w;
    const float bottom = top + view_h;
    return world_pos.x >= left && world_pos.x <= right && world_pos.y >= top && world_pos.y <= bottom;
}

void GameApp::PlaySfxIfVisible(const Sound& sound, bool loaded, Vector2 world_pos) const {
    if (!audio_device_ready_ || !loaded) {
        return;
    }
    if (!IsWorldPointInsideCameraView(world_pos)) {
        return;
    }
    PlaySound(sound);
}

bool GameApp::HasVisibleIdleFireStormDummy() const {
    for (const auto& dummy : state_.fire_storm_dummies) {
        if (!dummy.alive || dummy.state != FireStormDummyState::Idle) {
            continue;
        }
        if (IsWorldPointInsideCameraView(CellToWorldCenter(dummy.cell))) {
            return true;
        }
    }
    return false;
}

void GameApp::UpdateConnecting(float /*dt*/) {
    if (network_manager_.IsHost()) {
        app_screen_ = AppScreen::Lobby;
        return;
    }

    if (const auto lobby_update = network_manager_.ConsumeLobbyState(); lobby_update.has_value()) {
        lobby_player_names_.clear();
        for (const auto& player : lobby_update->players) {
            lobby_player_names_.push_back(player.name);
        }
    }

    if (network_manager_.HasReceivedJoinAck() || network_manager_.HasReceivedLobbyState()) {
        app_screen_ = AppScreen::Lobby;
        return;
    }

    const ClientConnectionState state = network_manager_.GetClientConnectionState();
    const double now = GetTime();
    if (state == ClientConnectionState::Disconnected && (now - connect_attempt_start_seconds_) > 0.4) {
        network_manager_.Stop();
        app_screen_ = AppScreen::MainMenu;
        return;
    }

    constexpr double kConnectTimeoutSeconds = 10.0;
    if ((now - connect_attempt_start_seconds_) > kConnectTimeoutSeconds) {
        std::printf("[NET] Client connection timed out after %.1fs\n", kConnectTimeoutSeconds);
        network_manager_.Stop();
        app_screen_ = AppScreen::MainMenu;
    }
}

void GameApp::UpdateLobby(float dt) {
    if (network_manager_.IsHost()) {
        lobby_broadcast_accumulator_ += dt;

        lobby_player_names_.clear();
        lobby_player_names_.push_back(player_name_buffer_);
        known_player_names_[0] = player_name_buffer_;

        LobbyStateMessage lobby_state;
        lobby_state.host_can_start = true;
        lobby_state.shrink_tiles_per_second = lobby_shrink_tiles_per_second_;
        lobby_state.min_arena_radius_tiles = lobby_min_arena_radius_tiles_;
        lobby_state.players.push_back({0, player_name_buffer_});

        for (const auto& remote : network_manager_.GetRemotePlayers()) {
            lobby_player_names_.push_back(remote.name);
            known_player_names_[remote.player_id] = remote.name;
            lobby_state.players.push_back({remote.player_id, remote.name});
        }

        if (lobby_broadcast_accumulator_ >= 0.2) {
            lobby_broadcast_accumulator_ = 0.0;
            lobby_state.mode_type = static_cast<int>(lobby_mode_type_);
            lobby_state.round_time_seconds = lobby_round_time_seconds_;
            lobby_state.best_of_target_kills = lobby_best_of_target_kills_;
            network_manager_.BroadcastLobbyState(lobby_state);
        }
    } else {
        const auto lobby_update = network_manager_.ConsumeLobbyState();
        if (lobby_update.has_value()) {
            lobby_player_names_.clear();
            for (const auto& player : lobby_update->players) {
                lobby_player_names_.push_back(player.name);
                known_player_names_[player.player_id] = player.name;
            }
            lobby_mode_type_ = static_cast<MatchModeType>(lobby_update->mode_type);
            lobby_round_time_seconds_ = lobby_update->round_time_seconds;
            lobby_best_of_target_kills_ = lobby_update->best_of_target_kills;
            lobby_shrink_tiles_per_second_ = lobby_update->shrink_tiles_per_second;
            lobby_shrink_start_seconds_ = lobby_update->shrink_start_seconds;
            lobby_min_arena_radius_tiles_ = lobby_update->min_arena_radius_tiles;
        }

        if (network_manager_.ConsumeMatchStart()) {
            state_.match.match_running = true;
            state_.match.match_finished = false;
            state_.match.time_remaining = static_cast<float>(lobby_round_time_seconds_);
            app_screen_ = AppScreen::InMatch;
            state_.local_player_id = network_manager_.GetAssignedLocalPlayerId();
            pending_primary_pressed_ = false;
            pending_select_rune_slot_ = -1;
            pending_activate_item_slot_ = -1;
            pending_toggle_inventory_mode_ = false;
        }
    }
}

void GameApp::UpdateMatch(float dt) {
    if (network_manager_.IsHost()) {
        SimulateHostGameplay(dt);
        snapshot_accumulator_ += dt;
        const double snapshot_interval = static_cast<double>(Constants::kNetworkSnapshotIntervalSeconds);
        while (snapshot_accumulator_ >= snapshot_interval) {
            snapshot_accumulator_ -= snapshot_interval;
            network_manager_.BroadcastSnapshot(BuildHostSnapshot());
        }
        UpdateClientVisualSmoothing(dt);
    } else {
        ClientInputMessage local_input = BuildLocalInput(network_manager_.GetAssignedLocalPlayerId());
        local_input.local_dt = dt;
        ApplyClientLocalInputPreview(local_input, dt);

        ClientMoveMessage move_message;
        move_message.player_id = local_input.player_id;
        move_message.seq = local_input.seq;
        move_message.tick = local_input.tick;
        move_message.move_x = local_input.move_x;
        move_message.move_y = local_input.move_y;
        move_message.aim_x = local_input.aim_x;
        move_message.aim_y = local_input.aim_y;
        network_manager_.SendClientMove(move_message);

        if (local_input.primary_pressed || local_input.grappling_pressed ||
            local_input.request_rune_type != static_cast<int>(RuneType::None) || !local_input.request_item_id.empty() ||
            local_input.toggle_inventory_mode) {
            ClientActionMessage action_message;
            action_message.player_id = local_input.player_id;
            action_message.seq = local_input.seq;
            action_message.primary_pressed = local_input.primary_pressed;
            action_message.grappling_pressed = local_input.grappling_pressed;
            action_message.request_rune_type = local_input.request_rune_type;
            action_message.request_item_id = local_input.request_item_id;
            action_message.toggle_inventory_mode = local_input.toggle_inventory_mode;
            network_manager_.SendClientAction(action_message);
        }

        const auto snapshot = network_manager_.ConsumeLatestSnapshot();
        if (snapshot.has_value()) {
            ApplySnapshotToClientState(*snapshot);
            if (state_.match.match_finished) {
                winning_team_ = DetermineWinningTeam(state_.match);
                app_screen_ = AppScreen::PostMatch;
            }
        }
        UpdateGrapplingHooks(dt);
        UpdateClientVisualSmoothing(dt);
        UpdateDamagePopups(dt);
    }

    UpdateProjectileEmitters();
    UpdateParticles(dt);
    UpdateLightningEffects(dt);
    UpdateCompositeEffects(dt);
    UpdateFireStormCasts(dt);
    UpdateFireStormDummies(dt);
    camera_shake_time_remaining_ = std::max(0.0f, camera_shake_time_remaining_ - dt);
    UpdateCameraTarget();
}

void GameApp::UpdatePostMatch(float /*dt*/) {
    if (IsKeyPressed(KEY_ENTER)) {
        ReturnToMainMenu();
    }
}

void GameApp::UpdateClientVisualSmoothing(float dt) {
    (void)dt;
    if (state_.players.empty()) {
        render_player_positions_.clear();
        render_grappling_hook_head_positions_.clear();
        return;
    }

    std::unordered_map<int, Vector2> updated_positions;
    updated_positions.reserve(state_.players.size());

    if (network_manager_.IsHost()) {
        for (const auto& player : state_.players) {
            updated_positions[player.id] = player.pos;
        }
        render_player_positions_.swap(updated_positions);
        render_grappling_hook_head_positions_.clear();
        return;
    }

    const double target_time = GetTime() - static_cast<double>(Constants::kRemoteInterpolationDelaySeconds);
    for (const auto& player : state_.players) {
        if (player.id == state_.local_player_id) {
            updated_positions[player.id] = player.pos;
            continue;
        }

        auto samples_it = remote_position_samples_.find(player.id);
        if (samples_it == remote_position_samples_.end() || samples_it->second.empty()) {
            updated_positions[player.id] = player.pos;
            continue;
        }

        auto& samples = samples_it->second;
        while (samples.size() > 2 && samples[1].time_seconds <= target_time) {
            samples.pop_front();
        }

        if (samples.size() >= 2 && samples.front().time_seconds <= target_time &&
            samples[1].time_seconds >= target_time) {
            const double t0 = samples.front().time_seconds;
            const double t1 = samples[1].time_seconds;
            const float alpha =
                static_cast<float>(std::clamp((target_time - t0) / std::max(0.00001, t1 - t0), 0.0, 1.0));
            updated_positions[player.id] = Vector2Lerp(samples.front().pos, samples[1].pos, alpha);
        } else {
            updated_positions[player.id] = samples.back().pos;
        }
    }
    render_player_positions_.swap(updated_positions);

    std::unordered_map<int, Vector2> updated_hook_positions;
    updated_hook_positions.reserve(state_.grappling_hooks.size());
    for (const auto& hook : state_.grappling_hooks) {
        auto samples_it = grappling_head_position_samples_.find(hook.id);
        if (samples_it == grappling_head_position_samples_.end() || samples_it->second.empty()) {
            updated_hook_positions[hook.id] = hook.head_pos;
            continue;
        }

        auto& samples = samples_it->second;
        while (samples.size() > 2 && samples[1].time_seconds <= target_time) {
            samples.pop_front();
        }

        if (samples.size() >= 2 && samples.front().time_seconds <= target_time &&
            samples[1].time_seconds >= target_time) {
            const double t0 = samples.front().time_seconds;
            const double t1 = samples[1].time_seconds;
            const float alpha =
                static_cast<float>(std::clamp((target_time - t0) / std::max(0.00001, t1 - t0), 0.0, 1.0));
            updated_hook_positions[hook.id] = Vector2Lerp(samples.front().pos, samples[1].pos, alpha);
        } else {
            updated_hook_positions[hook.id] = samples.back().pos;
        }
    }
    render_grappling_hook_head_positions_.swap(updated_hook_positions);
}

void GameApp::ApplyClientLocalInputPreview(const ClientInputMessage& input, float dt) {
    if (network_manager_.IsHost()) {
        return;
    }
    if (state_.local_player_id < 0) {
        return;
    }

    Player* local_player = FindPlayerById(state_.local_player_id);
    if (!local_player || !local_player->alive) {
        return;
    }
    const bool is_stunned = HasStatusEffect(*local_player, StatusEffectType::Stunned);
    const bool is_pulled = IsPlayerBeingPulled(local_player->id);

    local_player->rune_place_cooldown_remaining = std::max(0.0f, local_player->rune_place_cooldown_remaining - dt);
    local_player->grappling_cooldown_remaining = std::max(0.0f, local_player->grappling_cooldown_remaining - dt);
    local_player->mana =
        std::clamp(local_player->mana + local_player->mana_regen_per_second * dt, 0.0f, local_player->max_mana);
    for (size_t i = 0; i < local_player->rune_cooldown_remaining.size(); ++i) {
        local_player->rune_cooldown_remaining[i] = std::max(0.0f, local_player->rune_cooldown_remaining[i] - dt);
    }

    if (!is_stunned && !is_pulled && input.toggle_inventory_mode) {
        local_player->inventory_mode = !local_player->inventory_mode;
    }
    if (!is_stunned && !is_pulled && input.request_rune_type != static_cast<int>(RuneType::None)) {
        const RuneType rune_type = static_cast<RuneType>(input.request_rune_type);
        const float mana_cost = GetRuneManaCost(rune_type);
        for (size_t i = 0; i < local_player->rune_slots.size(); ++i) {
            if (local_player->rune_slots[i] == rune_type) {
                local_player->selected_rune_slot = static_cast<int>(i);
                break;
            }
        }
        local_player->selected_rune_type = rune_type;
        local_player->rune_placing_mode = (rune_type != RuneType::None) &&
                                          (GetPlayerRuneCooldownRemaining(*local_player, rune_type) <= 0.0f) &&
                                          (local_player->mana >= mana_cost);
    }

    Vector2 aim_vector = {input.aim_x - local_player->pos.x, input.aim_y - local_player->pos.y};
    if (local_player->melee_active_remaining <= 0.0f && Vector2LengthSqr(aim_vector) > 0.0001f) {
        local_player->aim_dir = Vector2Normalize(aim_vector);
        local_player->facing = AimToFacing(local_player->aim_dir);
    }

    if (!is_stunned && !is_pulled && input.primary_pressed && local_player->rune_placing_mode &&
        local_player->rune_place_cooldown_remaining <= 0.0f) {
        local_player->rune_placing_mode = false;
        local_player->rune_place_cooldown_remaining = local_player->rune_place_cooldown_duration;
    }
    if (!is_stunned && !is_pulled && input.grappling_pressed && !local_player->rune_placing_mode &&
        local_player->grappling_cooldown_remaining <= 0.0f) {
        TryStartGrapplingHook(*local_player, Vector2{input.aim_x, input.aim_y}, false);
    }

    // Movement/facing prediction for client feel; authoritative state is reconciled on snapshots.
    Vector2 movement = {input.move_x, input.move_y};
    if (Vector2LengthSqr(movement) > 0.0001f) {
        movement = Vector2Normalize(movement);
    }

    const float movement_multiplier = GetPlayerMovementSpeedMultiplier(*local_player);
    const Vector2 acceleration = (is_stunned || is_pulled)
                                     ? Vector2{0.0f, 0.0f}
                                     : Vector2Scale(movement, Constants::kPlayerAcceleration * movement_multiplier);
    local_player->vel = Vector2Add(local_player->vel, Vector2Scale(acceleration, dt));
    const float damping = std::max(0.0f, 1.0f - Constants::kPlayerFriction * dt);
    local_player->vel = Vector2Scale(local_player->vel, damping);
    const float speed = Vector2Length(local_player->vel);
    if (speed > Constants::kPlayerMaxSpeed * movement_multiplier) {
        local_player->vel =
            Vector2Scale(Vector2Normalize(local_player->vel), Constants::kPlayerMaxSpeed * movement_multiplier);
    }
    local_player->pos = Vector2Add(local_player->pos, Vector2Scale(local_player->vel, dt));
    CollisionWorld::ResolvePlayerVsWorldLocal(state_.map, *local_player, !is_pulled);
    ResolvePlayerVsMapObjects(*local_player);
    ResolvePlayerVsIceWallsLocal(*local_player);

    if (is_stunned || is_pulled) {
        local_player->action_state = PlayerActionState::Idle;
    } else if (local_player->melee_active_remaining > 0.0f) {
        local_player->action_state = PlayerActionState::Slashing;
    } else if (local_player->rune_placing_mode) {
        local_player->action_state = PlayerActionState::RunePlacing;
    } else if (Vector2LengthSqr(local_player->vel) > 8.0f) {
        local_player->action_state = PlayerActionState::Walking;
    } else {
        local_player->action_state = PlayerActionState::Idle;
    }

    pending_local_prediction_inputs_.push_back(input);
    while (pending_local_prediction_inputs_.size() > static_cast<size_t>(Constants::kMoveInputBufferSize)) {
        pending_local_prediction_inputs_.pop_front();
    }
}

void GameApp::Render() {
    BeginDrawing();
    ClearBackground(Color{16, 18, 22, 255});

    switch (app_screen_) {
        case AppScreen::MainMenu: {
            if (has_menu_background_texture_) {
                Rectangle src = {0.0f, 0.0f, static_cast<float>(menu_background_texture_.width),
                                 static_cast<float>(menu_background_texture_.height)};
                Rectangle dst = {0.0f, 0.0f, static_cast<float>(GetScreenWidth()), static_cast<float>(GetScreenHeight())};
                DrawTexturePro(menu_background_texture_, src, dst, {0.0f, 0.0f}, 0.0f, WHITE);
            }

            const MainMenuUiResult ui_result =
                DrawMainMenu(player_name_buffer_, sizeof(player_name_buffer_), join_ip_buffer_, sizeof(join_ip_buffer_),
                             lan_discovery_.GetHosts(), config_manager_.GetConfigPath(), controls_bindings_,
                             controls_manager_.GetControlsPath(), show_network_debug_panel_, main_menu_status_message_,
                             main_menu_status_is_error_);

            if (ui_result.settings_changed) {
                show_network_debug_panel_ = ui_result.show_network_debug_panel;
                settings_.show_network_debug_panel = show_network_debug_panel_;
                config_manager_.Save(settings_);
            }

            if (ui_result.request_host) {
                StartAsHost();
            }
            if (ui_result.request_join) {
                const int join_port =
                    ui_result.selected_host_port > 0 ? ui_result.selected_host_port : Constants::kDefaultPort;
                StartAsClient(ui_result.selected_host_ip, join_port);
            }
            if (ui_result.request_apply_controls) {
                controls_bindings_ = ui_result.controls_bindings;
                if (controls_manager_.Save(controls_bindings_)) {
                    main_menu_status_message_ =
                        TextFormat("Controls saved: %s", controls_manager_.GetControlsPath().c_str());
                    main_menu_status_is_error_ = false;
                } else {
                    main_menu_status_message_ = "Failed to save controls.json";
                    main_menu_status_is_error_ = true;
                }
            }
            break;
        }

        case AppScreen::Connecting: {
            const std::string lobby_mode_name =
                (lobby_mode_type_ == MatchModeType::BestOfKills) ? "Best Of" : most_kills_mode_.GetUiName();
            const LobbyUiResult lobby_ui =
                DrawLobby(lobby_player_names_, false, host_display_ip_, static_cast<int>(lobby_mode_type_),
                          lobby_round_time_seconds_, lobby_best_of_target_kills_, lobby_shrink_tiles_per_second_,
                          lobby_shrink_start_seconds_, lobby_min_arena_radius_tiles_,
                          lobby_mode_name, GetClientLobbyStatusText());
            if (lobby_ui.request_leave) {
                ReturnToMainMenu();
            }
            break;
        }

        case AppScreen::Lobby: {
            const std::string lobby_mode_name =
                (lobby_mode_type_ == MatchModeType::BestOfKills) ? "Best Of" : most_kills_mode_.GetUiName();
            const LobbyUiResult lobby_ui = DrawLobby(lobby_player_names_, network_manager_.IsHost(), host_display_ip_,
                                                     static_cast<int>(lobby_mode_type_), lobby_round_time_seconds_,
                                                     lobby_best_of_target_kills_, lobby_shrink_tiles_per_second_,
                                                     lobby_shrink_start_seconds_, lobby_min_arena_radius_tiles_,
                                                     lobby_mode_name,
                                                     network_manager_.IsHost() ? "hosting/listening"
                                                                              : GetClientLobbyStatusText());
            if (lobby_ui.request_leave) {
                ReturnToMainMenu();
            }
            bool mode_settings_changed = false;
            if (network_manager_.IsHost() && lobby_ui.request_toggle_mode_type) {
                lobby_mode_type_ = (lobby_mode_type_ == MatchModeType::MostKillsTimed) ? MatchModeType::BestOfKills
                                                                                       : MatchModeType::MostKillsTimed;
                mode_settings_changed = true;
            }
            if (network_manager_.IsHost() && lobby_ui.request_decrease_round_time) {
                lobby_round_time_seconds_ =
                    std::max(Constants::kLobbyTimeStepSeconds,
                             lobby_round_time_seconds_ - Constants::kLobbyTimeStepSeconds);
                lobby_shrink_start_seconds_ = std::min(lobby_shrink_start_seconds_, static_cast<float>(lobby_round_time_seconds_));
                mode_settings_changed = true;
            }
            if (network_manager_.IsHost() && lobby_ui.request_increase_round_time) {
                lobby_round_time_seconds_ += Constants::kLobbyTimeStepSeconds;
                mode_settings_changed = true;
            }
            if (network_manager_.IsHost() && lobby_ui.request_decrease_best_of) {
                lobby_best_of_target_kills_ = std::max(1, lobby_best_of_target_kills_ - 2);
                if ((lobby_best_of_target_kills_ % 2) == 0) lobby_best_of_target_kills_ -= 1;
                mode_settings_changed = true;
            }
            if (network_manager_.IsHost() && lobby_ui.request_increase_best_of) {
                lobby_best_of_target_kills_ += 2;
                if ((lobby_best_of_target_kills_ % 2) == 0) lobby_best_of_target_kills_ += 1;
                mode_settings_changed = true;
            }
            if (network_manager_.IsHost() && lobby_ui.request_decrease_shrink_rate) {
                lobby_shrink_tiles_per_second_ = std::max(
                    0.0f, lobby_shrink_tiles_per_second_ - Constants::kShrinkTilesPerSecondStep);
                mode_settings_changed = true;
            }
            if (network_manager_.IsHost() && lobby_ui.request_increase_shrink_rate) {
                lobby_shrink_tiles_per_second_ =
                    std::min(Constants::kMaxShrinkTilesPerSecond,
                             lobby_shrink_tiles_per_second_ + Constants::kShrinkTilesPerSecondStep);
                mode_settings_changed = true;
            }
            if (network_manager_.IsHost() && lobby_ui.request_decrease_shrink_start) {
                lobby_shrink_start_seconds_ =
                    std::max(0.0f, lobby_shrink_start_seconds_ - Constants::kLobbyTimeStepSeconds);
                mode_settings_changed = true;
            }
            if (network_manager_.IsHost() && lobby_ui.request_increase_shrink_start) {
                lobby_shrink_start_seconds_ += Constants::kLobbyTimeStepSeconds;
                if (lobby_mode_type_ == MatchModeType::MostKillsTimed) {
                    lobby_shrink_start_seconds_ =
                        std::min(lobby_shrink_start_seconds_, static_cast<float>(lobby_round_time_seconds_));
                }
                mode_settings_changed = true;
            }
            if (network_manager_.IsHost() && lobby_ui.request_decrease_min_radius) {
                lobby_min_arena_radius_tiles_ = std::max(
                    0.0f, lobby_min_arena_radius_tiles_ - Constants::kMinArenaRadiusTilesStep);
                mode_settings_changed = true;
            }
            if (network_manager_.IsHost() && lobby_ui.request_increase_min_radius) {
                lobby_min_arena_radius_tiles_ = std::min(
                    Constants::kMaxArenaRadiusTiles, lobby_min_arena_radius_tiles_ + Constants::kMinArenaRadiusTilesStep);
                mode_settings_changed = true;
            }
            if (network_manager_.IsHost() && mode_settings_changed) {
                settings_.lobby_mode_type = static_cast<int>(lobby_mode_type_);
                settings_.lobby_match_length_seconds = lobby_round_time_seconds_;
                settings_.lobby_best_of_target_kills = lobby_best_of_target_kills_;
                settings_.lobby_shrink_tiles_per_second = lobby_shrink_tiles_per_second_;
                settings_.lobby_shrink_start_seconds = lobby_shrink_start_seconds_;
                settings_.lobby_min_arena_radius_tiles = lobby_min_arena_radius_tiles_;
                config_manager_.Save(settings_);
            }
            if (lobby_ui.request_start_match && network_manager_.IsHost()) {
                StartMatchAsHost();
            }
            break;
        }

        case AppScreen::InMatch:
            RenderWorld();
            if (const Player* local_player = FindPlayerById(state_.local_player_id);
                local_player != nullptr && local_player->inventory_mode) {
                DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Color{0, 0, 0, 115});
                const char* text = "Inventory mode (Press tab to toggle off/on)";
                const int font_size = 20;
                const int text_width = MeasureText(text, font_size);
                DrawText(text, (GetScreenWidth() - text_width) / 2, GetScreenHeight() / 2 - font_size / 2, font_size,
                         RAYWHITE);
            }
            DrawMatchHud(state_, state_.local_player_id);
            RenderBottomHud();
            RenderNetworkDebugPanel();
            break;

        case AppScreen::PostMatch:
            DrawPostMatch(state_, winning_team_);
            break;
    }

    EndDrawing();
}

void GameApp::StartAsHost() {
    std::printf("[UI] Host Game pressed\n");

    std::string saved_name(player_name_buffer_);
    if (!saved_name.empty()) {
        settings_.player_name = saved_name;
        config_manager_.Save(settings_);
    }

    network_manager_.SetLocalPlayerName(settings_.player_name);
    if (!network_manager_.StartHost(Constants::kDefaultPort)) {
        main_menu_status_message_ = "Host start failed: " + network_manager_.GetLastDebugMessage();
        main_menu_status_is_error_ = true;
        std::printf("[UI] %s\n", main_menu_status_message_.c_str());
        return;
    }

    main_menu_status_message_ = TextFormat("Host started on UDP %d", Constants::kDefaultPort);
    main_menu_status_is_error_ = false;

    lan_discovery_.Stop();
    if (!lan_discovery_.StartHostBroadcaster(settings_.player_name, Constants::kDefaultPort)) {
        std::printf("[UI] Warning: LAN discovery broadcaster failed to start\n");
    }

    host_display_ip_ = TextFormat("%s:%d", lan_discovery_.GetHostLocalIp().c_str(), Constants::kDefaultPort);
    lobby_player_names_.clear();
    lobby_player_names_.push_back(settings_.player_name);
    render_player_positions_.clear();
    remote_position_samples_.clear();
    pending_local_prediction_inputs_.clear();
    known_player_names_.clear();
    known_player_names_[0] = settings_.player_name;
    lobby_broadcast_accumulator_ = 0.0;
    snapshot_accumulator_ = 0.0;
    connect_attempt_start_seconds_ = 0.0;

    app_screen_ = AppScreen::Lobby;
}

void GameApp::StartAsClient(const std::string& ip, int port) {
    std::string saved_name(player_name_buffer_);
    if (!saved_name.empty()) {
        settings_.player_name = saved_name;
        config_manager_.Save(settings_);
    }

    network_manager_.SetLocalPlayerName(settings_.player_name);
    if (!network_manager_.StartClient()) {
        main_menu_status_message_ = "Client start failed: " + network_manager_.GetLastDebugMessage();
        main_menu_status_is_error_ = true;
        return;
    }

    if (!network_manager_.ConnectToHost(ip, port)) {
        main_menu_status_message_ = "Connect failed: " + network_manager_.GetLastDebugMessage();
        main_menu_status_is_error_ = true;
        network_manager_.Stop();
        return;
    }

    main_menu_status_message_.clear();
    main_menu_status_is_error_ = false;

    host_display_ip_ = TextFormat("%s:%d", ip.c_str(), port);
    lobby_player_names_.clear();
    render_player_positions_.clear();
    remote_position_samples_.clear();
    pending_local_prediction_inputs_.clear();
    lobby_broadcast_accumulator_ = 0.0;
    snapshot_accumulator_ = 0.0;
    connect_attempt_start_seconds_ = GetTime();
    app_screen_ = AppScreen::Connecting;
}

void GameApp::ReturnToMainMenu() {
    network_manager_.Stop();
    lan_discovery_.Stop();
    if (!lan_discovery_.StartClientListener()) {
        main_menu_status_message_ = TextFormat("Discovery listener failed on UDP %d", Constants::kDiscoveryPort);
        main_menu_status_is_error_ = true;
    }

    state_ = GameState{};
    map_loader_.Load(resolved_map_path_, &objects_database_, state_.map);

    event_queue_.Clear();
    latest_remote_inputs_.clear();
    render_player_positions_.clear();
    remote_position_samples_.clear();
    pending_local_prediction_inputs_.clear();
    known_player_names_.clear();
    pending_primary_pressed_ = false;
    pending_select_rune_slot_ = -1;
    pending_activate_item_slot_ = -1;
    pending_toggle_inventory_mode_ = false;
    pending_object_spawns_.clear();
    lobby_broadcast_accumulator_ = 0.0;
    snapshot_accumulator_ = 0.0;
    connect_attempt_start_seconds_ = 0.0;
    winning_team_ = -1;
    host_server_tick_ = 0;
    local_input_seq_ = 0;
    main_menu_status_message_.clear();
    main_menu_status_is_error_ = false;
    lobby_shrink_tiles_per_second_ = settings_.lobby_shrink_tiles_per_second;
    lobby_mode_type_ = static_cast<MatchModeType>(settings_.lobby_mode_type);
    lobby_round_time_seconds_ = settings_.lobby_match_length_seconds;
    lobby_best_of_target_kills_ = settings_.lobby_best_of_target_kills;
    lobby_shrink_start_seconds_ = settings_.lobby_shrink_start_seconds;
    lobby_min_arena_radius_tiles_ = settings_.lobby_min_arena_radius_tiles;
    app_screen_ = AppScreen::MainMenu;
}

void GameApp::StartMatchAsHost() {
    state_.match = MatchState{};
    state_.match.mode_type = lobby_mode_type_;
    state_.match.round_time_seconds = lobby_round_time_seconds_;
    state_.match.match_running = true;
    state_.match.match_finished = false;
    state_.match.time_remaining = static_cast<float>(lobby_round_time_seconds_);
    state_.match.best_of_target_kills = lobby_best_of_target_kills_;
    state_.match.shrink_tiles_per_second = lobby_shrink_tiles_per_second_;
    state_.match.shrink_start_seconds = lobby_shrink_start_seconds_;
    state_.match.arena_center_world = {static_cast<float>(state_.map.width * state_.map.cell_size) * 0.5f,
                                       static_cast<float>(state_.map.height * state_.map.cell_size) * 0.5f};
    const float map_diameter_tiles = static_cast<float>(std::max(state_.map.width, state_.map.height));
    const float map_radius_tiles = map_diameter_tiles * 0.5f;
    state_.match.min_arena_radius_tiles = std::clamp(lobby_min_arena_radius_tiles_, 0.0f, map_radius_tiles);
    state_.match.arena_radius_tiles = map_radius_tiles;
    state_.match.arena_radius_world = state_.match.arena_radius_tiles * static_cast<float>(state_.map.cell_size);

    state_.players.clear();
    state_.runes.clear();
    state_.projectiles.clear();
    state_.explosions.clear();
    state_.lightning_effects.clear();
    state_.ice_walls.clear();
    state_.map_objects.clear();
    state_.particles.clear();
    state_.next_entity_id = 1;
    state_.next_rune_placement_order = 1;
    latest_remote_inputs_.clear();
    render_player_positions_.clear();
    remote_position_samples_.clear();
    pending_local_prediction_inputs_.clear();
    host_server_tick_ = 0;
    snapshot_accumulator_ = 0.0;
    pending_primary_pressed_ = false;
    pending_select_rune_slot_ = -1;
    pending_activate_item_slot_ = -1;
    pending_toggle_inventory_mode_ = false;
    pending_object_spawns_.clear();

    Player host_player;
    host_player.id = 0;
    host_player.name = settings_.player_name;
    host_player.team = Constants::kTeamRed;
    state_.players.push_back(host_player);

    for (const auto& remote : network_manager_.GetRemotePlayers()) {
        Player player;
        player.id = remote.player_id;
        player.name = remote.name;
        player.team = (static_cast<int>(state_.players.size()) % 2 == 0) ? Constants::kTeamRed : Constants::kTeamBlue;
        state_.players.push_back(player);
    }

    std::vector<GridCoord> spawns = state_.map.spawn_points;
    if (spawns.empty()) {
        spawns.push_back({2, 2});
        spawns.push_back({state_.map.width - 3, state_.map.height - 3});
    }

    std::mt19937 rng(static_cast<unsigned int>(GetTime() * 1000.0));
    std::shuffle(spawns.begin(), spawns.end(), rng);

    for (size_t i = 0; i < state_.players.size(); ++i) {
        const GridCoord spawn = spawns[i % spawns.size()];
        state_.players[i].spawn_cell = spawn;
        state_.players[i].pos = CellToWorldCenter(spawn);
        state_.players[i].alive = true;
        state_.players[i].awaiting_respawn = false;
        state_.players[i].respawn_remaining = 0.0f;
        state_.players[i].outside_zone_damage_accumulator = 0.0f;
        render_player_positions_[state_.players[i].id] = state_.players[i].pos;
    }

    RebuildMapObjectsFromSeeds();

    state_.local_player_id = 0;

    network_manager_.BroadcastMatchStart(MatchStartMessage{});
    network_manager_.BroadcastSnapshot(BuildHostSnapshot());

    app_screen_ = AppScreen::InMatch;
}

void GameApp::ApplySnapshotToClientState(const ServerSnapshotMessage& snapshot) {
    struct PreviousPlayerState {
        int hp = 0;
        bool alive = false;
        PlayerActionState action_state = PlayerActionState::Idle;
    };

    std::unordered_map<int, int> previous_hp;
    previous_hp.reserve(state_.players.size());
    std::unordered_map<int, PreviousPlayerState> previous_player_state;
    previous_player_state.reserve(state_.players.size());
    std::unordered_map<int, std::array<std::string, 4>> previous_player_item_slots;
    previous_player_item_slots.reserve(state_.players.size());
    std::unordered_map<int, std::array<int, 4>> previous_player_item_counts;
    previous_player_item_counts.reserve(state_.players.size());
    for (const auto& player : state_.players) {
        previous_hp[player.id] = player.hp;
        previous_player_state[player.id] = {player.hp, player.alive, player.action_state};
        previous_player_item_slots[player.id] = player.item_slots;
        previous_player_item_counts[player.id] = player.item_slot_counts;
    }
    std::unordered_set<int> previous_dummy_ids;
    previous_dummy_ids.reserve(state_.fire_storm_dummies.size());
    for (const auto& dummy : state_.fire_storm_dummies) {
        previous_dummy_ids.insert(dummy.id);
    }
    std::unordered_set<int> previous_fire_storm_cast_ids;
    previous_fire_storm_cast_ids.reserve(state_.fire_storm_casts.size());
    for (const auto& cast : state_.fire_storm_casts) {
        previous_fire_storm_cast_ids.insert(cast.id);
    }
    std::unordered_set<int> previous_rune_ids;
    previous_rune_ids.reserve(state_.runes.size());
    std::unordered_map<int, bool> previous_rune_active;
    previous_rune_active.reserve(state_.runes.size());
    std::unordered_map<int, EarthRuneTrapState> previous_earth_rune_states;
    previous_earth_rune_states.reserve(state_.runes.size());
    std::unordered_map<int, bool> previous_earth_rune_roots_spawned;
    previous_earth_rune_roots_spawned.reserve(state_.runes.size());
    for (const auto& rune : state_.runes) {
        previous_rune_ids.insert(rune.id);
        previous_rune_active[rune.id] = rune.active;
        previous_earth_rune_states[rune.id] = rune.earth_trap_state;
        previous_earth_rune_roots_spawned[rune.id] = rune.earth_roots_spawned;
    }
    std::unordered_map<int, Vector2> previous_projectile_positions;
    previous_projectile_positions.reserve(state_.projectiles.size());
    std::unordered_map<int, std::string> previous_projectile_animation_keys;
    previous_projectile_animation_keys.reserve(state_.projectiles.size());
    for (const auto& projectile : state_.projectiles) {
        previous_projectile_positions[projectile.id] = projectile.pos;
        previous_projectile_animation_keys[projectile.id] = projectile.animation_key;
    }
    std::unordered_map<int, IceWallState> previous_wall_states;
    previous_wall_states.reserve(state_.ice_walls.size());
    for (const auto& wall : state_.ice_walls) {
        previous_wall_states[wall.id] = wall.state;
    }
    std::unordered_map<int, MapObjectState> previous_object_states;
    previous_object_states.reserve(state_.map_objects.size());
    std::unordered_map<int, Vector2> previous_consumable_positions;
    previous_consumable_positions.reserve(state_.map_objects.size());
    for (const auto& object : state_.map_objects) {
        previous_object_states[object.id] = object.state;
        if (object.type == ObjectType::Consumable && object.alive) {
            previous_consumable_positions[object.id] = CellToWorldCenter(object.cell);
        }
    }
    std::unordered_map<int, GrapplingHookPhase> previous_grappling_hook_phases;
    previous_grappling_hook_phases.reserve(state_.grappling_hooks.size());
    std::optional<GrapplingHook> previous_local_owned_hook;
    for (const auto& hook : state_.grappling_hooks) {
        previous_grappling_hook_phases[hook.id] = hook.phase;
        if (!network_manager_.IsHost() && state_.local_player_id >= 0 && hook.owner_player_id == state_.local_player_id &&
            hook.alive) {
            previous_local_owned_hook = hook;
        }
    }

    Vector2 previous_local_pos = {0.0f, 0.0f};
    Vector2 previous_local_vel = {0.0f, 0.0f};
    bool has_previous_local = false;
    Player previous_local_ui_state;
    bool has_previous_local_ui_state = false;
    if (state_.local_player_id >= 0) {
        if (const Player* previous_local = FindPlayerById(state_.local_player_id)) {
            previous_local_pos = previous_local->pos;
            previous_local_vel = previous_local->vel;
            has_previous_local = true;
            previous_local_ui_state = *previous_local;
            has_previous_local_ui_state = true;
        }
    }

    state_.match.time_remaining = snapshot.time_remaining;
    state_.match.shrink_tiles_per_second = snapshot.shrink_tiles_per_second;
    state_.match.min_arena_radius_tiles = snapshot.min_arena_radius_tiles;
    state_.match.arena_radius_tiles = snapshot.arena_radius_tiles;
    state_.match.arena_radius_world = snapshot.arena_radius_world;
    state_.match.arena_center_world = {snapshot.arena_center_world_x, snapshot.arena_center_world_y};
    state_.match.match_running = snapshot.match_running;
    state_.match.match_finished = snapshot.match_finished;
    state_.match.red_team_kills = snapshot.red_team_kills;
    state_.match.blue_team_kills = snapshot.blue_team_kills;

    const double now_seconds = GetTime();
    std::unordered_map<int, Vector2> updated_render_positions;
    state_.players.clear();
    for (const auto& player_snapshot : snapshot.players) {
        Player player;
        player.id = player_snapshot.id;
        auto known_name_it = known_player_names_.find(player.id);
        player.name = known_name_it != known_player_names_.end() ? known_name_it->second
                                                                 : TextFormat("Player%d", player.id);
        player.team = player_snapshot.team;
        player.pos = {player_snapshot.pos_x, player_snapshot.pos_y};
        player.vel = {player_snapshot.vel_x, player_snapshot.vel_y};
        player.aim_dir = {player_snapshot.aim_dir_x, player_snapshot.aim_dir_y};
        player.hp = player_snapshot.hp;
        player.kills = player_snapshot.kills;
        player.alive = player_snapshot.alive;
        player.facing = static_cast<FacingDirection>(player_snapshot.facing);
        player.action_state = static_cast<PlayerActionState>(player_snapshot.action_state);
        player.melee_active_remaining = player_snapshot.melee_active_remaining;
        player.rune_placing_mode = player_snapshot.rune_placing_mode;
        player.selected_rune_type = static_cast<RuneType>(player_snapshot.selected_rune_type);
        player.rune_place_cooldown_remaining = player_snapshot.rune_place_cooldown_remaining;
        player.mana = player_snapshot.mana;
        player.max_mana = player_snapshot.max_mana;
        player.grappling_cooldown_remaining = player_snapshot.grappling_cooldown_remaining;
        player.grappling_cooldown_total = player_snapshot.grappling_cooldown_total;
        player.status_effects.clear();
        for (const auto& status_snapshot : player_snapshot.status_effects) {
            StatusEffectInstance status;
            status.type = static_cast<StatusEffectType>(status_snapshot.type);
            status.remaining_seconds = status_snapshot.remaining_seconds;
            status.total_seconds = status_snapshot.total_seconds;
            status.magnitude_per_second = status_snapshot.magnitude_per_second;
            status.visible = status_snapshot.visible;
            status.is_buff = status_snapshot.is_buff;
            status.source_id = status_snapshot.source_id;
            status.progress = status_snapshot.progress;
            status.source_elapsed_seconds = status_snapshot.source_elapsed_seconds;
            status.burn_duration_seconds = status_snapshot.burn_duration_seconds;
            status.movement_speed_multiplier = status_snapshot.movement_speed_multiplier;
            status.source_active = status_snapshot.source_active;
            status.composite_effect_id = status_snapshot.composite_effect_id;
            status.accumulated_magnitude = 0.0f;
            player.status_effects.push_back(status);
        }
        for (size_t i = 0; i < player.item_slots.size() && i < player_snapshot.item_slots.size(); ++i) {
            player.item_slots[i] = player_snapshot.item_slots[i];
        }
        for (size_t i = 0; i < player.item_slot_counts.size() && i < player_snapshot.item_slot_counts.size(); ++i) {
            player.item_slot_counts[i] = player_snapshot.item_slot_counts[i];
        }
        for (size_t i = 0; i < player.item_slot_cooldown_remaining.size() &&
                           i < player_snapshot.item_slot_cooldown_remaining.size();
             ++i) {
            player.item_slot_cooldown_remaining[i] = player_snapshot.item_slot_cooldown_remaining[i];
        }
        for (size_t i = 0; i < player.item_slot_cooldown_total.size() &&
                           i < player_snapshot.item_slot_cooldown_total.size();
             ++i) {
            player.item_slot_cooldown_total[i] = player_snapshot.item_slot_cooldown_total[i];
        }
        player.awaiting_respawn = player_snapshot.awaiting_respawn;
        player.respawn_remaining = player_snapshot.respawn_remaining;
        player.last_processed_move_seq = player_snapshot.last_processed_move_seq;
        for (size_t i = 0; i < player.rune_cooldown_remaining.size() && i < player_snapshot.rune_cooldown_remaining.size();
             ++i) {
            player.rune_cooldown_remaining[i] = player_snapshot.rune_cooldown_remaining[i];
        }
        for (size_t i = 0; i < player.rune_cooldown_total.size() && i < player_snapshot.rune_cooldown_total.size(); ++i) {
            player.rune_cooldown_total[i] = player_snapshot.rune_cooldown_total[i];
        }
        if (has_previous_local_ui_state && player.id == state_.local_player_id) {
            player.rune_slots = previous_local_ui_state.rune_slots;
            player.selected_rune_slot = previous_local_ui_state.selected_rune_slot;
            player.inventory_mode = previous_local_ui_state.inventory_mode;
            player.ui_dragging_slot = previous_local_ui_state.ui_dragging_slot;
            player.ui_drag_source_family = previous_local_ui_state.ui_drag_source_family;
            player.ui_drag_source_index = previous_local_ui_state.ui_drag_source_index;
        }
        state_.players.push_back(player);

        if (!network_manager_.IsHost() && player.id != state_.local_player_id) {
            auto& samples = remote_position_samples_[player.id];
            samples.push_back(RemotePositionSample{now_seconds, player.pos});
            while (samples.size() > 32) {
                samples.pop_front();
            }
            updated_render_positions[player.id] = player.pos;
        } else {
            updated_render_positions[player.id] = player.pos;
        }
    }
    render_player_positions_.swap(updated_render_positions);

    state_.runes.clear();
    for (const auto& rune_snapshot : snapshot.runes) {
        Rune rune;
        rune.id = rune_snapshot.id;
        rune.owner_player_id = rune_snapshot.owner_player_id;
        rune.owner_team = rune_snapshot.owner_team;
        rune.cell = {rune_snapshot.x, rune_snapshot.y};
        rune.rune_type = static_cast<RuneType>(rune_snapshot.rune_type);
        rune.placement_order = rune_snapshot.placement_order;
        rune.active = rune_snapshot.active;
        rune.volatile_cast = rune_snapshot.volatile_cast;
        rune.activation_total_seconds = rune_snapshot.activation_total_seconds;
        rune.activation_remaining_seconds = rune_snapshot.activation_remaining_seconds;
        rune.creates_influence_zone = rune_snapshot.creates_influence_zone;
        rune.earth_trap_state = static_cast<EarthRuneTrapState>(rune_snapshot.earth_trap_state);
        rune.earth_state_time = rune_snapshot.earth_state_time;
        rune.earth_state_duration = rune_snapshot.earth_state_duration;
        rune.earth_roots_spawned = rune_snapshot.earth_roots_spawned;
        rune.earth_roots_group_id = rune_snapshot.earth_roots_group_id;
        state_.runes.push_back(rune);
    }
    RebuildInfluenceZones();
    if (!network_manager_.IsHost()) {
        for (const auto& rune : state_.runes) {
            const auto previous_active_it = previous_rune_active.find(rune.id);
            const bool became_active = rune.active &&
                                       (previous_active_it == previous_rune_active.end() || !previous_active_it->second);
            if (!became_active) {
                continue;
            }
            Particle rune_cast_vfx;
            rune_cast_vfx.pos = CellToWorldCenter(rune.cell);
            rune_cast_vfx.vel = {0.0f, 0.0f};
            rune_cast_vfx.acc = {0.0f, 0.0f};
            rune_cast_vfx.drag = 0.0f;
            rune_cast_vfx.animation_key = rune.volatile_cast ? "rune_cast_volatile_effect" : "rune_cast_effect";
            rune_cast_vfx.facing = "default";
            rune_cast_vfx.age_seconds = 0.0f;
            rune_cast_vfx.max_cycles = 1;
            rune_cast_vfx.alive = true;
            state_.particles.push_back(rune_cast_vfx);
            PlaySfxIfVisible(sfx_create_rune_.sound, sfx_create_rune_.loaded, rune_cast_vfx.pos);

            Vector2 start = CellToWorldCenter(rune.cell);
            if (const Player* caster = FindPlayerById(rune.owner_player_id)) {
                start = caster->pos;
            }
            SpawnLightningEffect(start, CellToWorldCenter(rune.cell), 0.3f, rune.volatile_cast);
        }
        for (const auto& rune : state_.runes) {
            if (rune.rune_type != RuneType::Earth || !rune.active) {
                continue;
            }
            const auto previous_state_it = previous_earth_rune_states.find(rune.id);
            const EarthRuneTrapState previous_state =
                (previous_state_it != previous_earth_rune_states.end()) ? previous_state_it->second
                                                                        : EarthRuneTrapState::IdleRune;
            if (previous_state != EarthRuneTrapState::Slamming && rune.earth_trap_state == EarthRuneTrapState::Slamming) {
                PlaySfxIfVisible(sfx_earth_rune_launch_.sound, sfx_earth_rune_launch_.loaded, CellToWorldCenter(rune.cell));
            }
            const auto previous_roots_it = previous_earth_rune_roots_spawned.find(rune.id);
            const bool roots_spawned_before =
                (previous_roots_it != previous_earth_rune_roots_spawned.end()) ? previous_roots_it->second : false;
            if (!roots_spawned_before && rune.earth_roots_spawned) {
                const Vector2 impact_center = CellToWorldCenter(rune.cell);
                PlaySfxIfVisible(sfx_earth_rune_impact_.sound, sfx_earth_rune_impact_.loaded, impact_center);
                if (IsWorldPointInsideCameraView(impact_center)) {
                    camera_shake_time_remaining_ =
                        std::max(camera_shake_time_remaining_, Constants::kCameraShakeDurationSeconds);
                }
            }
        }
    }

    state_.projectiles.clear();
    for (const auto& projectile_snapshot : snapshot.projectiles) {
        Projectile projectile;
        projectile.id = projectile_snapshot.id;
        projectile.owner_player_id = projectile_snapshot.owner_player_id;
        projectile.owner_team = projectile_snapshot.owner_team;
        projectile.pos = {projectile_snapshot.pos_x, projectile_snapshot.pos_y};
        projectile.vel = {projectile_snapshot.vel_x, projectile_snapshot.vel_y};
        projectile.radius = projectile_snapshot.radius;
        projectile.damage = projectile_snapshot.damage;
        projectile.animation_key = projectile_snapshot.animation_key;
        projectile.emitter_enabled = projectile_snapshot.emitter_enabled;
        projectile.emitter_emit_every_frames = projectile_snapshot.emitter_emit_every_frames;
        projectile.emitter_frame_counter = projectile_snapshot.emitter_frame_counter;
        projectile.alive = projectile_snapshot.alive;
        state_.projectiles.push_back(projectile);
    }
    if (!network_manager_.IsHost()) {
        std::unordered_set<int> current_projectile_ids;
        current_projectile_ids.reserve(state_.projectiles.size());
        for (const auto& projectile : state_.projectiles) {
            current_projectile_ids.insert(projectile.id);
            if (previous_projectile_positions.find(projectile.id) == previous_projectile_positions.end() &&
                projectile.animation_key == "projectile_fire_bolt") {
                PlaySfxIfVisible(sfx_fireball_created_.sound, sfx_fireball_created_.loaded, projectile.pos);
            }
            const auto previous_animation_it = previous_projectile_animation_keys.find(projectile.id);
            if (previous_animation_it != previous_projectile_animation_keys.end() &&
                previous_animation_it->second != projectile.animation_key && IsStaticFireBolt(projectile)) {
                Particle upgrade_vfx;
                upgrade_vfx.pos = projectile.pos;
                upgrade_vfx.vel = {0.0f, 0.0f};
                upgrade_vfx.acc = {0.0f, 0.0f};
                upgrade_vfx.drag = 0.0f;
                upgrade_vfx.animation_key = "static_upgrade";
                upgrade_vfx.facing = "default";
                upgrade_vfx.age_seconds = 0.0f;
                upgrade_vfx.max_cycles = 1;
                upgrade_vfx.alive = true;
                state_.particles.push_back(upgrade_vfx);
                PlaySfxIfVisible(sfx_static_upgrade_.sound, sfx_static_upgrade_.loaded, projectile.pos);
            }
        }
        for (const auto& [projectile_id, projectile_pos] : previous_projectile_positions) {
            if (current_projectile_ids.find(projectile_id) != current_projectile_ids.end()) {
                continue;
            }
            const auto previous_animation_it = previous_projectile_animation_keys.find(projectile_id);
            const bool was_static = previous_animation_it != previous_projectile_animation_keys.end() &&
                                    (previous_animation_it->second == "fire_bolt_static" ||
                                     previous_animation_it->second == "fire_storm_static_large");
            Particle explosion_vfx;
            explosion_vfx.pos = projectile_pos;
            explosion_vfx.vel = {0.0f, 0.0f};
            explosion_vfx.acc = {0.0f, 0.0f};
            explosion_vfx.drag = 0.0f;
            explosion_vfx.animation_key = "explosion";
            explosion_vfx.facing = "default";
            explosion_vfx.age_seconds = 0.0f;
            explosion_vfx.max_cycles = 1;
            explosion_vfx.alive = true;
            state_.particles.push_back(explosion_vfx);
            const float half_w = static_cast<float>(GetScreenWidth()) / std::max(0.001f, (2.0f * camera_.zoom));
            const float half_h = static_cast<float>(GetScreenHeight()) / std::max(0.001f, (2.0f * camera_.zoom));
            const float pad = Constants::kFireBoltExplosionRadius;
            const bool in_camera_view =
                projectile_pos.x >= (camera_.target.x - half_w - pad) &&
                projectile_pos.x <= (camera_.target.x + half_w + pad) &&
                projectile_pos.y >= (camera_.target.y - half_h - pad) &&
                projectile_pos.y <= (camera_.target.y + half_h + pad);
            if (in_camera_view) {
                camera_shake_time_remaining_ =
                    std::max(camera_shake_time_remaining_, Constants::kCameraShakeDurationSeconds);
            }
            if (was_static) {
                PlaySfxIfVisible(sfx_static_bolt_impact_.sound, sfx_static_bolt_impact_.loaded, projectile_pos);
            } else {
                PlaySfxIfVisible(sfx_explosion_.sound, sfx_explosion_.loaded, projectile_pos);
            }
        }
    }

    state_.ice_walls.clear();
    for (const auto& wall_snapshot : snapshot.ice_walls) {
        IceWallPiece wall;
        wall.id = wall_snapshot.id;
        wall.owner_player_id = wall_snapshot.owner_player_id;
        wall.owner_team = wall_snapshot.owner_team;
        wall.cell = {wall_snapshot.cell_x, wall_snapshot.cell_y};
        wall.state = static_cast<IceWallState>(wall_snapshot.state);
        wall.state_time = wall_snapshot.state_time;
        wall.hp = wall_snapshot.hp;
        wall.alive = wall_snapshot.alive;
        state_.ice_walls.push_back(wall);
    }
    if (!network_manager_.IsHost()) {
        bool played_freeze = false;
        bool played_melt = false;
        for (const auto& wall : state_.ice_walls) {
            const auto previous_state_it = previous_wall_states.find(wall.id);
            const Vector2 center = CellToWorldCenter(wall.cell);
            if (previous_state_it == previous_wall_states.end()) {
                if (!played_freeze) {
                    PlaySfxIfVisible(sfx_ice_wall_freeze_.sound, sfx_ice_wall_freeze_.loaded, center);
                    played_freeze = true;
                }
                continue;
            }
            if (previous_state_it->second != IceWallState::Dying && wall.state == IceWallState::Dying) {
                if (!played_melt) {
                    PlaySfxIfVisible(sfx_ice_wall_melt_.sound, sfx_ice_wall_melt_.loaded, center);
                    played_melt = true;
                }
            }
        }
    }
    state_.map_objects.clear();
    for (const auto& object_snapshot : snapshot.map_objects) {
        MapObjectInstance object;
        object.id = object_snapshot.id;
        object.prototype_id = object_snapshot.prototype_id;
        object.cell = {object_snapshot.cell_x, object_snapshot.cell_y};
        object.type = static_cast<ObjectType>(object_snapshot.object_type);
        object.hp = object_snapshot.hp;
        object.state = static_cast<MapObjectState>(object_snapshot.state);
        object.state_time = object_snapshot.state_time;
        object.death_duration = object_snapshot.death_duration;
        object.collision_enabled = object_snapshot.collision_enabled;
        object.alive = object_snapshot.alive;
        if (const ObjectPrototype* proto = FindObjectPrototype(object.prototype_id)) {
            object.walkable = proto->walkable;
            object.stops_projectiles = proto->stops_projectiles;
        }
        state_.map_objects.push_back(object);
    }
    if (!network_manager_.IsHost()) {
        std::unordered_set<int> current_object_ids;
        current_object_ids.reserve(state_.map_objects.size());
        for (const auto& object : state_.map_objects) {
            current_object_ids.insert(object.id);
            const auto previous_state_it = previous_object_states.find(object.id);
            if (previous_state_it == previous_object_states.end()) {
                continue;
            }
            if (previous_state_it->second != MapObjectState::Dying && object.state == MapObjectState::Dying) {
                PlaySfxIfVisible(sfx_vase_breaking_.sound, sfx_vase_breaking_.loaded, CellToWorldCenter(object.cell));
            }
        }
        for (const auto& [object_id, world_pos] : previous_consumable_positions) {
            if (current_object_ids.find(object_id) == current_object_ids.end()) {
                PlaySfxIfVisible(sfx_item_pickup_.sound, sfx_item_pickup_.loaded, world_pos);
            }
        }
    }
    state_.fire_storm_dummies.clear();
    for (const auto& dummy_snapshot : snapshot.fire_storm_dummies) {
        FireStormDummy dummy;
        dummy.id = dummy_snapshot.id;
        dummy.owner_player_id = dummy_snapshot.owner_player_id;
        dummy.owner_team = dummy_snapshot.owner_team;
        dummy.cell = {dummy_snapshot.cell_x, dummy_snapshot.cell_y};
        dummy.state = static_cast<FireStormDummyState>(dummy_snapshot.state);
        dummy.state_time = dummy_snapshot.state_time;
        dummy.state_duration = dummy_snapshot.state_duration;
        dummy.idle_lifetime_remaining_seconds = dummy_snapshot.idle_lifetime_remaining_seconds;
        dummy.alive = dummy_snapshot.alive;
        state_.fire_storm_dummies.push_back(dummy);
    }
    state_.fire_storm_casts.clear();
    for (const auto& cast_snapshot : snapshot.fire_storm_casts) {
        FireStormCast cast;
        cast.id = cast_snapshot.id;
        cast.owner_player_id = cast_snapshot.owner_player_id;
        cast.owner_team = cast_snapshot.owner_team;
        cast.center_cell = {cast_snapshot.center_cell_x, cast_snapshot.center_cell_y};
        cast.elapsed_seconds = cast_snapshot.elapsed_seconds;
        cast.duration_seconds = cast_snapshot.duration_seconds;
        cast.alive = cast_snapshot.alive;
        state_.fire_storm_casts.push_back(cast);
    }
    if (!network_manager_.IsHost()) {
        for (const auto& cast : state_.fire_storm_casts) {
            if (previous_fire_storm_cast_ids.find(cast.id) != previous_fire_storm_cast_ids.end()) {
                continue;
            }
            PlaySfxIfVisible(sfx_fire_storm_cast_.sound, sfx_fire_storm_cast_.loaded,
                             CellToWorldCenter(cast.center_cell));
        }
    }
    state_.earth_roots_groups.clear();
    for (const auto& roots_snapshot : snapshot.earth_roots_groups) {
        EarthRootsGroup group;
        group.id = roots_snapshot.id;
        group.owner_player_id = roots_snapshot.owner_player_id;
        group.owner_team = roots_snapshot.owner_team;
        group.center_cell = {roots_snapshot.center_cell_x, roots_snapshot.center_cell_y};
        group.state = static_cast<EarthRootsGroupState>(roots_snapshot.state);
        group.state_time = roots_snapshot.state_time;
        group.state_duration = roots_snapshot.state_duration;
        group.idle_lifetime_remaining_seconds = roots_snapshot.idle_lifetime_remaining_seconds;
        group.active_for_gameplay = roots_snapshot.active_for_gameplay;
        group.alive = roots_snapshot.alive;
        state_.earth_roots_groups.push_back(group);
    }
    state_.grappling_hooks.clear();
    for (const auto& hook_snapshot : snapshot.grappling_hooks) {
        GrapplingHook hook;
        hook.id = hook_snapshot.id;
        hook.owner_player_id = hook_snapshot.owner_player_id;
        hook.owner_team = hook_snapshot.owner_team;
        hook.head_pos = {hook_snapshot.head_pos_x, hook_snapshot.head_pos_y};
        hook.target_pos = {hook_snapshot.target_pos_x, hook_snapshot.target_pos_y};
        hook.latch_point = {hook_snapshot.latch_point_x, hook_snapshot.latch_point_y};
        hook.pull_destination = {hook_snapshot.pull_destination_x, hook_snapshot.pull_destination_y};
        hook.phase = static_cast<GrapplingHookPhase>(hook_snapshot.phase);
        hook.latch_target_type = static_cast<GrapplingHookLatchTargetType>(hook_snapshot.latch_target_type);
        hook.latch_target_id = hook_snapshot.latch_target_id;
        hook.latch_cell = {hook_snapshot.latch_cell_x, hook_snapshot.latch_cell_y};
        hook.latched = hook_snapshot.latched;
        hook.animation_time = hook_snapshot.animation_time;
        hook.pull_elapsed_seconds = hook_snapshot.pull_elapsed_seconds;
        hook.max_pull_duration_seconds = hook_snapshot.max_pull_duration_seconds;
        hook.alive = hook_snapshot.alive;
        if (!network_manager_.IsHost() && previous_local_owned_hook.has_value() &&
            state_.local_player_id >= 0 && hook.owner_player_id == state_.local_player_id &&
            previous_local_owned_hook->alive &&
            previous_local_owned_hook->phase == hook.phase &&
            (hook.phase == GrapplingHookPhase::Firing || hook.phase == GrapplingHookPhase::Retracting)) {
            hook.head_pos = previous_local_owned_hook->head_pos;
            hook.animation_time = previous_local_owned_hook->animation_time;
        }
        state_.grappling_hooks.push_back(hook);
        if (!network_manager_.IsHost()) {
            auto& samples = grappling_head_position_samples_[hook.id];
            samples.push_back(RemotePositionSample{now_seconds, hook.head_pos});
            while (samples.size() > 32) {
                samples.pop_front();
            }
        }
    }
    if (!network_manager_.IsHost()) {
        std::unordered_set<int> current_hook_ids;
        current_hook_ids.reserve(state_.grappling_hooks.size());
        for (const auto& hook : state_.grappling_hooks) {
            current_hook_ids.insert(hook.id);
        }
        for (auto it = grappling_head_position_samples_.begin(); it != grappling_head_position_samples_.end();) {
            if (current_hook_ids.find(it->first) == current_hook_ids.end()) {
                it = grappling_head_position_samples_.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (!network_manager_.IsHost()) {
        for (const auto& hook : state_.grappling_hooks) {
            const auto previous_phase_it = previous_grappling_hook_phases.find(hook.id);
            if (previous_phase_it == previous_grappling_hook_phases.end()) {
                if (const Player* owner = FindPlayerById(hook.owner_player_id)) {
                    PlaySfxIfVisible(sfx_grappling_throw_.sound, sfx_grappling_throw_.loaded, owner->pos);
                }
                continue;
            }
            if (previous_phase_it->second != GrapplingHookPhase::Pulling &&
                hook.phase == GrapplingHookPhase::Pulling) {
                PlaySfxIfVisible(sfx_grappling_latch_.sound, sfx_grappling_latch_.loaded, hook.latch_point);
            }
        }
        for (const auto& dummy : state_.fire_storm_dummies) {
            if (previous_dummy_ids.find(dummy.id) != previous_dummy_ids.end()) {
                continue;
            }
            fire_storm_dummy_lightning_seconds_remaining_.erase(dummy.id);
            fire_storm_dummy_lightning_cooldown_seconds_remaining_.erase(dummy.id);
        }
    }
    state_.explosions.clear();

    if (!network_manager_.IsHost()) {
        for (const auto& player : state_.players) {
            auto previous_hp_it = previous_hp.find(player.id);
            if (previous_hp_it == previous_hp.end()) {
                continue;
            }
            if (previous_hp_it->second > player.hp) {
                SpawnDamagePopup(player.pos, previous_hp_it->second - player.hp, false);
                PlaySfxIfVisible(sfx_player_damaged_.sound, sfx_player_damaged_.loaded, player.pos);
            } else if (previous_hp_it->second < player.hp) {
                SpawnDamagePopup(player.pos, player.hp - previous_hp_it->second, true);
            }

            const auto previous_state_it = previous_player_state.find(player.id);
            if (previous_state_it != previous_player_state.end()) {
                if (previous_state_it->second.alive && !player.alive) {
                    PlaySfxIfVisible(sfx_player_death_.sound, sfx_player_death_.loaded, player.pos);
                }
                if (previous_state_it->second.action_state != PlayerActionState::Slashing &&
                    player.action_state == PlayerActionState::Slashing) {
                    PlaySfxIfVisible(sfx_melee_attack_.sound, sfx_melee_attack_.loaded, player.pos);
                }
            }

            const auto previous_slots_it = previous_player_item_slots.find(player.id);
            const auto previous_counts_it = previous_player_item_counts.find(player.id);
            if (previous_slots_it != previous_player_item_slots.end() &&
                previous_counts_it != previous_player_item_counts.end()) {
                bool drank_small_potion = false;
                for (size_t i = 0; i < previous_counts_it->second.size() && i < player.item_slot_counts.size(); ++i) {
                    const int previous_count = previous_counts_it->second[i];
                    const int current_count = player.item_slot_counts[i];
                    const std::string& previous_item = previous_slots_it->second[i];
                    const std::string& current_item = player.item_slots[i];
                    const std::string& consumed_item = !previous_item.empty() ? previous_item : current_item;
                    if (previous_count > current_count && consumed_item == "health_small") {
                        drank_small_potion = true;
                        break;
                    }
                }
                if (drank_small_potion) {
                    PlaySfxIfVisible(sfx_drink_potion_.sound, sfx_drink_potion_.loaded, player.pos);
                }
            }
        }
    }

    if (!network_manager_.IsHost() && state_.local_player_id >= 0) {
        Player* local_player = FindPlayerById(state_.local_player_id);
        if (local_player != nullptr) {
            while (!pending_local_prediction_inputs_.empty() &&
                   pending_local_prediction_inputs_.front().seq <= local_player->last_processed_move_seq) {
                pending_local_prediction_inputs_.pop_front();
            }

            Player replay = *local_player;
            for (const auto& pending_input : pending_local_prediction_inputs_) {
                Vector2 aim_vector = {pending_input.aim_x - replay.pos.x, pending_input.aim_y - replay.pos.y};
                if (replay.melee_active_remaining <= 0.0f && Vector2LengthSqr(aim_vector) > 0.0001f) {
                    replay.aim_dir = Vector2Normalize(aim_vector);
                    replay.facing = AimToFacing(replay.aim_dir);
                }

                Vector2 movement = {pending_input.move_x, pending_input.move_y};
                if (Vector2LengthSqr(movement) > 0.0001f) {
                    movement = Vector2Normalize(movement);
                }
                const float step_dt = pending_input.local_dt > 0.0f ? pending_input.local_dt
                                                                    : static_cast<float>(Constants::kFixedDt);
                const float movement_multiplier = GetPlayerMovementSpeedMultiplier(replay);
                const Vector2 acceleration = Vector2Scale(movement, Constants::kPlayerAcceleration * movement_multiplier);
                replay.vel = Vector2Add(replay.vel, Vector2Scale(acceleration, step_dt));
                const float damping = std::max(0.0f, 1.0f - Constants::kPlayerFriction * step_dt);
                replay.vel = Vector2Scale(replay.vel, damping);
                const float speed = Vector2Length(replay.vel);
                if (speed > Constants::kPlayerMaxSpeed * movement_multiplier) {
                    replay.vel =
                        Vector2Scale(Vector2Normalize(replay.vel), Constants::kPlayerMaxSpeed * movement_multiplier);
                }
                replay.pos = Vector2Add(replay.pos, Vector2Scale(replay.vel, step_dt));
                const bool replay_is_pulled = IsPlayerBeingPulled(replay.id);
                CollisionWorld::ResolvePlayerVsWorldLocal(state_.map, replay, !replay_is_pulled);
                ResolvePlayerVsMapObjects(replay);
                ResolvePlayerVsIceWallsLocal(replay);
            }

            const Vector2 base_pos = has_previous_local ? previous_local_pos : local_player->pos;
            const Vector2 base_vel = has_previous_local ? previous_local_vel : local_player->vel;
            const float error = Vector2Distance(base_pos, replay.pos);
            const bool grappling_active = std::any_of(
                state_.grappling_hooks.begin(), state_.grappling_hooks.end(),
                [&](const GrapplingHook& hook) { return hook.alive && hook.owner_player_id == local_player->id; });
            const float hard_snap_threshold = grappling_active ? Constants::kPredictionHardSnapThresholdGrapplingPx
                                                               : Constants::kPredictionHardSnapThresholdPx;
            const float reconcile_gain = grappling_active ? Constants::kPredictionReconcileGainGrappling
                                                          : Constants::kPredictionReconcileGain;
            const float telemetry_threshold =
                grappling_active ? Constants::kPredictionCorrectionTelemetryThresholdGrapplingPx
                                 : Constants::kPredictionCorrectionTelemetryThresholdPx;
            if (error > hard_snap_threshold) {
                local_player->pos = replay.pos;
                local_player->vel = replay.vel;
            } else {
                const float alpha =
                    std::clamp(reconcile_gain * Constants::kNetworkSnapshotIntervalSeconds, 0.0f, 1.0f);
                local_player->pos = Vector2Lerp(base_pos, replay.pos, alpha);
                local_player->vel = Vector2Lerp(base_vel, replay.vel, alpha);
            }
            local_player->aim_dir = replay.aim_dir;
            local_player->facing = replay.facing;

            if (error > telemetry_threshold) {
                network_manager_.AddReconciliationCorrection();
            }
            render_player_positions_[local_player->id] = local_player->pos;
        }
    }

    int max_order = 0;
    int max_entity_id = 0;
    for (const auto& rune : state_.runes) {
        max_order = std::max(max_order, rune.placement_order);
        max_entity_id = std::max(max_entity_id, rune.id);
    }
    for (const auto& projectile : state_.projectiles) {
        max_entity_id = std::max(max_entity_id, projectile.id);
    }
    for (const auto& wall : state_.ice_walls) {
        max_entity_id = std::max(max_entity_id, wall.id);
    }
    for (const auto& object : state_.map_objects) {
        max_entity_id = std::max(max_entity_id, object.id);
    }
    for (const auto& dummy : state_.fire_storm_dummies) {
        max_entity_id = std::max(max_entity_id, dummy.id);
    }
    for (const auto& cast : state_.fire_storm_casts) {
        max_entity_id = std::max(max_entity_id, cast.id);
    }
    for (const auto& hook : state_.grappling_hooks) {
        max_entity_id = std::max(max_entity_id, hook.id);
    }
    state_.next_rune_placement_order = max_order + 1;
    state_.next_entity_id = max_entity_id + 1;

    if (state_.local_player_id < 0) {
        state_.local_player_id = network_manager_.GetAssignedLocalPlayerId();
    }

    for (auto it = remote_position_samples_.begin(); it != remote_position_samples_.end();) {
        const bool exists = std::any_of(state_.players.begin(), state_.players.end(),
                                        [&](const Player& player) { return player.id == it->first; });
        if (!exists) {
            it = remote_position_samples_.erase(it);
        } else {
            ++it;
        }
    }
}

ServerSnapshotMessage GameApp::BuildHostSnapshot() {
    ServerSnapshotMessage snapshot;
    snapshot.server_tick = ++host_server_tick_;
    snapshot.snapshot_id = snapshot.server_tick;
    snapshot.base_snapshot_id = 0;
    snapshot.is_delta = false;
    snapshot.time_remaining = state_.match.time_remaining;
    snapshot.shrink_tiles_per_second = state_.match.shrink_tiles_per_second;
    snapshot.min_arena_radius_tiles = state_.match.min_arena_radius_tiles;
    snapshot.arena_radius_tiles = state_.match.arena_radius_tiles;
    snapshot.arena_radius_world = state_.match.arena_radius_world;
    snapshot.arena_center_world_x = state_.match.arena_center_world.x;
    snapshot.arena_center_world_y = state_.match.arena_center_world.y;
    snapshot.match_running = state_.match.match_running;
    snapshot.match_finished = state_.match.match_finished;
    snapshot.red_team_kills = state_.match.red_team_kills;
    snapshot.blue_team_kills = state_.match.blue_team_kills;

    for (const auto& player : state_.players) {
        PlayerSnapshot player_snapshot;
        player_snapshot.id = player.id;
        player_snapshot.team = player.team;
        player_snapshot.pos_x = player.pos.x;
        player_snapshot.pos_y = player.pos.y;
        player_snapshot.vel_x = player.vel.x;
        player_snapshot.vel_y = player.vel.y;
        player_snapshot.aim_dir_x = player.aim_dir.x;
        player_snapshot.aim_dir_y = player.aim_dir.y;
        player_snapshot.hp = player.hp;
        player_snapshot.kills = player.kills;
        player_snapshot.alive = player.alive;
        player_snapshot.facing = static_cast<int>(player.facing);
        player_snapshot.action_state = static_cast<int>(player.action_state);
        player_snapshot.melee_active_remaining = player.melee_active_remaining;
        player_snapshot.rune_placing_mode = player.rune_placing_mode;
        player_snapshot.selected_rune_type = static_cast<int>(player.selected_rune_type);
        player_snapshot.rune_place_cooldown_remaining = player.rune_place_cooldown_remaining;
        player_snapshot.mana = player.mana;
        player_snapshot.max_mana = player.max_mana;
        player_snapshot.grappling_cooldown_remaining = player.grappling_cooldown_remaining;
        player_snapshot.grappling_cooldown_total = player.grappling_cooldown_total;
        player_snapshot.rune_cooldown_remaining.assign(player.rune_cooldown_remaining.begin(),
                                                       player.rune_cooldown_remaining.end());
        player_snapshot.rune_cooldown_total.assign(player.rune_cooldown_total.begin(), player.rune_cooldown_total.end());
        for (const auto& status : player.status_effects) {
            PlayerSnapshot::StatusEffectSnapshot status_snapshot;
            status_snapshot.type = static_cast<int>(status.type);
            status_snapshot.remaining_seconds = status.remaining_seconds;
            status_snapshot.total_seconds = status.total_seconds;
            status_snapshot.magnitude_per_second = status.magnitude_per_second;
            status_snapshot.visible = status.visible;
            status_snapshot.is_buff = status.is_buff;
            status_snapshot.source_id = status.source_id;
            status_snapshot.progress = status.progress;
            status_snapshot.source_elapsed_seconds = status.source_elapsed_seconds;
            status_snapshot.burn_duration_seconds = status.burn_duration_seconds;
            status_snapshot.movement_speed_multiplier = status.movement_speed_multiplier;
            status_snapshot.source_active = status.source_active;
            status_snapshot.composite_effect_id = status.composite_effect_id;
            player_snapshot.status_effects.push_back(status_snapshot);
        }
        player_snapshot.item_slots.assign(player.item_slots.begin(), player.item_slots.end());
        player_snapshot.item_slot_counts.assign(player.item_slot_counts.begin(), player.item_slot_counts.end());
        player_snapshot.item_slot_cooldown_remaining.assign(player.item_slot_cooldown_remaining.begin(),
                                                            player.item_slot_cooldown_remaining.end());
        player_snapshot.item_slot_cooldown_total.assign(player.item_slot_cooldown_total.begin(),
                                                        player.item_slot_cooldown_total.end());
        player_snapshot.awaiting_respawn = player.awaiting_respawn;
        player_snapshot.respawn_remaining = player.respawn_remaining;
        player_snapshot.last_processed_move_seq = player.last_processed_move_seq;
        snapshot.players.push_back(player_snapshot);
    }

    for (const auto& rune : state_.runes) {
        RuneSnapshot rune_snapshot;
        rune_snapshot.id = rune.id;
        rune_snapshot.owner_player_id = rune.owner_player_id;
        rune_snapshot.owner_team = rune.owner_team;
        rune_snapshot.x = rune.cell.x;
        rune_snapshot.y = rune.cell.y;
        rune_snapshot.rune_type = static_cast<int>(rune.rune_type);
        rune_snapshot.placement_order = rune.placement_order;
        rune_snapshot.active = rune.active;
        rune_snapshot.volatile_cast = rune.volatile_cast;
        rune_snapshot.activation_total_seconds = rune.activation_total_seconds;
        rune_snapshot.activation_remaining_seconds = rune.activation_remaining_seconds;
        rune_snapshot.creates_influence_zone = rune.creates_influence_zone;
        rune_snapshot.earth_trap_state = static_cast<int>(rune.earth_trap_state);
        rune_snapshot.earth_state_time = rune.earth_state_time;
        rune_snapshot.earth_state_duration = rune.earth_state_duration;
        rune_snapshot.earth_roots_spawned = rune.earth_roots_spawned;
        rune_snapshot.earth_roots_group_id = rune.earth_roots_group_id;
        snapshot.runes.push_back(rune_snapshot);
    }

    for (const auto& projectile : state_.projectiles) {
        ProjectileSnapshot projectile_snapshot;
        projectile_snapshot.id = projectile.id;
        projectile_snapshot.owner_player_id = projectile.owner_player_id;
        projectile_snapshot.owner_team = projectile.owner_team;
        projectile_snapshot.pos_x = projectile.pos.x;
        projectile_snapshot.pos_y = projectile.pos.y;
        projectile_snapshot.vel_x = projectile.vel.x;
        projectile_snapshot.vel_y = projectile.vel.y;
        projectile_snapshot.radius = projectile.radius;
        projectile_snapshot.damage = projectile.damage;
        projectile_snapshot.animation_key = projectile.animation_key;
        projectile_snapshot.emitter_enabled = projectile.emitter_enabled;
        projectile_snapshot.emitter_emit_every_frames = projectile.emitter_emit_every_frames;
        projectile_snapshot.emitter_frame_counter = projectile.emitter_frame_counter;
        projectile_snapshot.alive = projectile.alive;
        snapshot.projectiles.push_back(projectile_snapshot);
    }

    for (const auto& wall : state_.ice_walls) {
        IceWallSnapshot wall_snapshot;
        wall_snapshot.id = wall.id;
        wall_snapshot.owner_player_id = wall.owner_player_id;
        wall_snapshot.owner_team = wall.owner_team;
        wall_snapshot.cell_x = wall.cell.x;
        wall_snapshot.cell_y = wall.cell.y;
        wall_snapshot.state = static_cast<int>(wall.state);
        wall_snapshot.state_time = wall.state_time;
        wall_snapshot.hp = wall.hp;
        wall_snapshot.alive = wall.alive;
        snapshot.ice_walls.push_back(wall_snapshot);
    }

    for (const auto& object : state_.map_objects) {
        MapObjectSnapshot object_snapshot;
        object_snapshot.id = object.id;
        object_snapshot.prototype_id = object.prototype_id;
        object_snapshot.cell_x = object.cell.x;
        object_snapshot.cell_y = object.cell.y;
        object_snapshot.object_type = static_cast<int>(object.type);
        object_snapshot.hp = object.hp;
        object_snapshot.state = static_cast<int>(object.state);
        object_snapshot.state_time = object.state_time;
        object_snapshot.death_duration = object.death_duration;
        object_snapshot.collision_enabled = object.collision_enabled;
        object_snapshot.alive = object.alive;
        snapshot.map_objects.push_back(object_snapshot);
    }

    for (const auto& dummy : state_.fire_storm_dummies) {
        FireStormDummySnapshot dummy_snapshot;
        dummy_snapshot.id = dummy.id;
        dummy_snapshot.owner_player_id = dummy.owner_player_id;
        dummy_snapshot.owner_team = dummy.owner_team;
        dummy_snapshot.cell_x = dummy.cell.x;
        dummy_snapshot.cell_y = dummy.cell.y;
        dummy_snapshot.state = static_cast<int>(dummy.state);
        dummy_snapshot.state_time = dummy.state_time;
        dummy_snapshot.state_duration = dummy.state_duration;
        dummy_snapshot.idle_lifetime_remaining_seconds = dummy.idle_lifetime_remaining_seconds;
        dummy_snapshot.alive = dummy.alive;
        snapshot.fire_storm_dummies.push_back(dummy_snapshot);
    }
    for (const auto& cast : state_.fire_storm_casts) {
        FireStormCastSnapshot cast_snapshot;
        cast_snapshot.id = cast.id;
        cast_snapshot.owner_player_id = cast.owner_player_id;
        cast_snapshot.owner_team = cast.owner_team;
        cast_snapshot.center_cell_x = cast.center_cell.x;
        cast_snapshot.center_cell_y = cast.center_cell.y;
        cast_snapshot.elapsed_seconds = cast.elapsed_seconds;
        cast_snapshot.duration_seconds = cast.duration_seconds;
        cast_snapshot.alive = cast.alive;
        snapshot.fire_storm_casts.push_back(cast_snapshot);
    }
    for (const auto& group : state_.earth_roots_groups) {
        EarthRootsGroupSnapshot roots_snapshot;
        roots_snapshot.id = group.id;
        roots_snapshot.owner_player_id = group.owner_player_id;
        roots_snapshot.owner_team = group.owner_team;
        roots_snapshot.center_cell_x = group.center_cell.x;
        roots_snapshot.center_cell_y = group.center_cell.y;
        roots_snapshot.state = static_cast<int>(group.state);
        roots_snapshot.state_time = group.state_time;
        roots_snapshot.state_duration = group.state_duration;
        roots_snapshot.idle_lifetime_remaining_seconds = group.idle_lifetime_remaining_seconds;
        roots_snapshot.active_for_gameplay = group.active_for_gameplay;
        roots_snapshot.alive = group.alive;
        snapshot.earth_roots_groups.push_back(roots_snapshot);
    }
    for (const auto& hook : state_.grappling_hooks) {
        GrapplingHookSnapshot hook_snapshot;
        hook_snapshot.id = hook.id;
        hook_snapshot.owner_player_id = hook.owner_player_id;
        hook_snapshot.owner_team = hook.owner_team;
        hook_snapshot.head_pos_x = hook.head_pos.x;
        hook_snapshot.head_pos_y = hook.head_pos.y;
        hook_snapshot.target_pos_x = hook.target_pos.x;
        hook_snapshot.target_pos_y = hook.target_pos.y;
        hook_snapshot.latch_point_x = hook.latch_point.x;
        hook_snapshot.latch_point_y = hook.latch_point.y;
        hook_snapshot.pull_destination_x = hook.pull_destination.x;
        hook_snapshot.pull_destination_y = hook.pull_destination.y;
        hook_snapshot.phase = static_cast<int>(hook.phase);
        hook_snapshot.latch_target_type = static_cast<int>(hook.latch_target_type);
        hook_snapshot.latch_target_id = hook.latch_target_id;
        hook_snapshot.latch_cell_x = hook.latch_cell.x;
        hook_snapshot.latch_cell_y = hook.latch_cell.y;
        hook_snapshot.latched = hook.latched;
        hook_snapshot.animation_time = hook.animation_time;
        hook_snapshot.pull_elapsed_seconds = hook.pull_elapsed_seconds;
        hook_snapshot.max_pull_duration_seconds = hook.max_pull_duration_seconds;
        hook_snapshot.alive = hook.alive;
        snapshot.grappling_hooks.push_back(hook_snapshot);
    }

    return snapshot;
}

ClientInputMessage GameApp::BuildLocalInput(int local_player_id) {
    ClientInputMessage input;
    input.player_id = local_player_id;
    input.tick = ++local_input_tick_;
    input.seq = ++local_input_seq_;

    input.move_x = 0.0f;
    input.move_y = 0.0f;
    if (IsBindingDown(controls_bindings_.move_left)) input.move_x -= 1.0f;
    if (IsBindingDown(controls_bindings_.move_right)) input.move_x += 1.0f;
    if (IsBindingDown(controls_bindings_.move_up)) input.move_y -= 1.0f;
    if (IsBindingDown(controls_bindings_.move_down)) input.move_y += 1.0f;

    Vector2 mouse_world = GetScreenToWorld2D(GetMousePosition(), camera_);
    input.aim_x = mouse_world.x;
    input.aim_y = mouse_world.y;

    // Use frame-latched edge inputs to avoid missing events when fixed-step updates
    // do not run on every render frame.
    input.primary_pressed = pending_primary_pressed_;
    input.grappling_pressed = pending_grappling_pressed_;
    if (state_.local_player_id >= 0) {
        if (const Player* local_player = FindPlayerById(state_.local_player_id)) {
            if (pending_select_rune_slot_ >= 0 &&
                pending_select_rune_slot_ < static_cast<int>(local_player->rune_slots.size())) {
                input.request_rune_type = static_cast<int>(local_player->rune_slots[pending_select_rune_slot_]);
            }
            if (pending_activate_item_slot_ >= 0 &&
                pending_activate_item_slot_ < static_cast<int>(local_player->item_slots.size())) {
                input.request_item_id = local_player->item_slots[pending_activate_item_slot_];
            }
        }
    }
    input.toggle_inventory_mode = pending_toggle_inventory_mode_;
    pending_primary_pressed_ = false;
    pending_grappling_pressed_ = false;
    pending_select_rune_slot_ = -1;
    pending_activate_item_slot_ = -1;
    pending_toggle_inventory_mode_ = false;
    return input;
}

void GameApp::SimulateHostGameplay(float dt) {
    const ClientInputMessage host_input = BuildLocalInput(0);
    latest_remote_inputs_[0] = host_input;

    for (const auto& move : network_manager_.ConsumeHostMoveInputs()) {
        ClientInputMessage& input = latest_remote_inputs_[move.player_id];
        input.player_id = move.player_id;
        input.seq = move.seq;
        input.tick = move.tick;
        input.move_x = move.move_x;
        input.move_y = move.move_y;
        input.aim_x = move.aim_x;
        input.aim_y = move.aim_y;
    }

    for (const auto& action : network_manager_.ConsumeHostActionInputs()) {
        ClientInputMessage& input = latest_remote_inputs_[action.player_id];
        input.player_id = action.player_id;
        input.seq = std::max(input.seq, action.seq);
        input.primary_pressed = input.primary_pressed || action.primary_pressed;
        input.grappling_pressed = input.grappling_pressed || action.grappling_pressed;
        if (action.request_rune_type != static_cast<int>(RuneType::None)) input.request_rune_type = action.request_rune_type;
        if (!action.request_item_id.empty()) input.request_item_id = action.request_item_id;
        input.toggle_inventory_mode = input.toggle_inventory_mode || action.toggle_inventory_mode;
    }

    for (auto& player : state_.players) {
        auto input_it = latest_remote_inputs_.find(player.id);
        if (input_it == latest_remote_inputs_.end()) {
            continue;
        }
        SimulatePlayerFromInput(player, input_it->second, dt);
    }

    for (auto& [_, input] : latest_remote_inputs_) {
        input.primary_pressed = false;
        input.grappling_pressed = false;
        input.request_rune_type = static_cast<int>(RuneType::None);
        input.request_item_id.clear();
        input.toggle_inventory_mode = false;
    }

    ResolvePlayerCollisions();
    UpdateIceWalls(dt);
    UpdateRunes(dt);
    UpdateEarthRootsGroups(dt);
    UpdateArena(dt);

    for (auto& player : state_.players) {
        if (player.melee_active_remaining > 0.0f && player.alive) {
            HandleMeleeHit(player);
        }
    }

    UpdateProjectiles(dt);
    UpdateGrapplingHooks(dt);
    UpdateExplosions(dt);
    UpdateMapObjects(dt);
    UpdateRespawns(dt);
    UpdateDamagePopups(dt);
    most_kills_mode_.Update(state_, event_queue_, dt);
    HandleEventsHost();

    if (state_.match.match_finished) {
        winning_team_ = DetermineWinningTeam(state_.match);
        app_screen_ = AppScreen::PostMatch;
    }
}

void GameApp::SimulatePlayerFromInput(Player& player, const ClientInputMessage& input, float dt) {
    player.last_processed_move_seq = input.seq;

    if (!player.alive) {
        player.vel = {0.0f, 0.0f};
        player.melee_active_remaining = 0.0f;
        player.action_state = PlayerActionState::Idle;
        return;
    }

    player.last_input_tick = input.tick;
    const bool is_stunned = HasStatusEffect(player, StatusEffectType::Stunned);
    const bool is_pulled = IsPlayerBeingPulled(player.id);

    if (!is_stunned && !is_pulled && input.toggle_inventory_mode) {
        player.inventory_mode = !player.inventory_mode;
    }
    if (!is_stunned && !is_pulled && input.request_rune_type != static_cast<int>(RuneType::None)) {
        const RuneType rune_type = static_cast<RuneType>(input.request_rune_type);
        const float mana_cost = GetRuneManaCost(rune_type);
        for (size_t i = 0; i < player.rune_slots.size(); ++i) {
            if (player.rune_slots[i] == rune_type) {
                player.selected_rune_slot = static_cast<int>(i);
                break;
            }
        }
        player.selected_rune_type = rune_type;
        player.rune_placing_mode = (rune_type != RuneType::None) &&
                                   (GetPlayerRuneCooldownRemaining(player, rune_type) <= 0.0f) &&
                                   (player.mana >= mana_cost);
    }
    Vector2 aim_vector = {input.aim_x - player.pos.x, input.aim_y - player.pos.y};
    if (player.melee_active_remaining <= 0.0f && Vector2LengthSqr(aim_vector) > 0.0001f) {
        player.aim_dir = Vector2Normalize(aim_vector);
        player.facing = AimToFacing(player.aim_dir);
    }

    player.melee_cooldown_remaining = std::max(0.0f, player.melee_cooldown_remaining - dt);
    player.melee_active_remaining = std::max(0.0f, player.melee_active_remaining - dt);
    player.rune_place_cooldown_remaining = std::max(0.0f, player.rune_place_cooldown_remaining - dt);
    player.grappling_cooldown_remaining = std::max(0.0f, player.grappling_cooldown_remaining - dt);
    player.mana = std::clamp(player.mana + player.mana_regen_per_second * dt, 0.0f, player.max_mana);
    UpdatePlayerStatusEffects(player, dt);
    for (size_t i = 0; i < player.rune_cooldown_remaining.size(); ++i) {
        player.rune_cooldown_remaining[i] = std::max(0.0f, player.rune_cooldown_remaining[i] - dt);
    }
    for (size_t i = 0; i < player.item_slot_cooldown_remaining.size(); ++i) {
        player.item_slot_cooldown_remaining[i] = std::max(0.0f, player.item_slot_cooldown_remaining[i] - dt);
    }

    if (!is_stunned && !is_pulled && !input.request_item_id.empty()) {
        TryActivateItemById(player, input.request_item_id);
    }

    if (!is_stunned && !is_pulled && input.primary_pressed) {
        if (player.rune_placing_mode) {
            if (TryPlaceRune(player, Vector2{input.aim_x, input.aim_y})) {
                player.action_state = PlayerActionState::RunePlacing;
            }
        } else if (player.melee_cooldown_remaining <= 0.0f) {
            player.melee_cooldown_remaining = Constants::kMeleeCooldownSeconds;
            player.melee_active_remaining = Constants::kMeleeActiveWindowSeconds;
            player.melee_hit_target_ids.clear();
            player.melee_hit_object_ids.clear();
            player.action_state = PlayerActionState::Slashing;
            PlaySfxIfVisible(sfx_melee_attack_.sound, sfx_melee_attack_.loaded, player.pos);
        }
    }

    if (!is_stunned && !is_pulled && input.grappling_pressed && !player.rune_placing_mode &&
        player.grappling_cooldown_remaining <= 0.0f) {
        TryStartGrapplingHook(player, Vector2{input.aim_x, input.aim_y});
    }

    Vector2 movement = {input.move_x, input.move_y};
    if (Vector2LengthSqr(movement) > 0.0001f) {
        movement = Vector2Normalize(movement);
    }

    const float movement_multiplier = GetPlayerMovementSpeedMultiplier(player);
    Vector2 acceleration = (is_stunned || is_pulled)
                               ? Vector2{0.0f, 0.0f}
                               : Vector2Scale(movement, Constants::kPlayerAcceleration * movement_multiplier);
    player.vel = Vector2Add(player.vel, Vector2Scale(acceleration, dt));

    const float damping = std::max(0.0f, 1.0f - Constants::kPlayerFriction * dt);
    player.vel = Vector2Scale(player.vel, damping);

    const float speed = Vector2Length(player.vel);
    if (speed > Constants::kPlayerMaxSpeed * movement_multiplier) {
        player.vel = Vector2Scale(Vector2Normalize(player.vel), Constants::kPlayerMaxSpeed * movement_multiplier);
    }

    player.pos = Vector2Add(player.pos, Vector2Scale(player.vel, dt));
    CollisionWorld::ResolvePlayerVsWorldLocal(state_.map, player, !is_pulled);
    ResolvePlayerVsMapObjects(player);
    ResolvePlayerVsIceWallsLocal(player);

    if (is_stunned || is_pulled) {
        player.action_state = PlayerActionState::Idle;
    } else if (player.melee_active_remaining > 0.0f) {
        player.action_state = PlayerActionState::Slashing;
    } else if (player.rune_placing_mode) {
        player.action_state = PlayerActionState::RunePlacing;
    } else if (Vector2LengthSqr(player.vel) > 8.0f) {
        player.action_state = PlayerActionState::Walking;
    } else {
        player.action_state = PlayerActionState::Idle;
    }
}

void GameApp::UpdateArena(float dt) {
    if (!state_.match.match_running || state_.match.match_finished) {
        return;
    }
    if (state_.map.cell_size <= 0) {
        return;
    }

    if (state_.match.elapsed_seconds >= state_.match.shrink_start_seconds) {
        state_.match.arena_radius_tiles = std::max(
            state_.match.min_arena_radius_tiles,
            state_.match.arena_radius_tiles - state_.match.shrink_tiles_per_second * dt);
    }
    state_.match.arena_radius_world = state_.match.arena_radius_tiles * static_cast<float>(state_.map.cell_size);

    for (auto& player : state_.players) {
        if (!player.alive) {
            continue;
        }

        if (!IsOutsideArena(player.pos)) {
            player.outside_zone_damage_accumulator = 0.0f;
            continue;
        }

        player.outside_zone_damage_accumulator += Constants::kArenaUnsafeDamagePerSecond * dt;
        const int zone_damage = static_cast<int>(std::floor(player.outside_zone_damage_accumulator));
        if (zone_damage > 0) {
            player.outside_zone_damage_accumulator -= static_cast<float>(zone_damage);
            ApplyDamageToPlayer(player, -1, zone_damage, "outside_zone", false);
        }
    }
}

void GameApp::UpdateRespawns(float dt) {
    for (auto& player : state_.players) {
        if (!player.awaiting_respawn) {
            continue;
        }
        player.respawn_remaining = std::max(0.0f, player.respawn_remaining - dt);
        if (player.respawn_remaining > 0.0f) {
            continue;
        }

        player.awaiting_respawn = false;
        player.alive = true;
        player.hp = Constants::kMaxHp;
        player.vel = {0.0f, 0.0f};
        player.rune_placing_mode = false;
        player.melee_active_remaining = 0.0f;
        player.melee_cooldown_remaining = 0.0f;
        player.outside_zone_damage_accumulator = 0.0f;
        player.action_state = PlayerActionState::Idle;
        player.status_effects.clear();
        player.pos = ComputeRespawnPosition(player);
        CollisionWorld::ResolvePlayerVsWorld(state_.map, player);
        ResolvePlayerVsMapObjects(player);
        ResolvePlayerVsIceWalls(player);
        render_player_positions_[player.id] = player.pos;
    }
}

void GameApp::UpdatePlayerStatusEffects(Player& player, float dt) {
    if (!player.alive) {
        return;
    }

    for (auto& status : player.status_effects) {
        if (status.remaining_seconds <= 0.0f) {
            continue;
        }
        const float applied_dt = std::min(dt, status.remaining_seconds);
        status.remaining_seconds = std::max(0.0f, status.remaining_seconds - dt);

        switch (status.type) {
            case StatusEffectType::Regeneration: {
                status.accumulated_magnitude += status.magnitude_per_second * applied_dt;
                const int whole_heal = static_cast<int>(std::floor(status.accumulated_magnitude + 0.0001f));
                if (whole_heal > 0) {
                    status.accumulated_magnitude -= static_cast<float>(whole_heal);
                    ApplyImmediateHeal(player, whole_heal);
                }
                break;
            }
            case StatusEffectType::Stunned:
                break;
            case StatusEffectType::Rooted: {
                status.source_elapsed_seconds += applied_dt;
                const float burn_duration = std::max(0.001f, status.burn_duration_seconds);
                const float normalized = std::clamp(status.source_elapsed_seconds / burn_duration, 0.0f, 1.0f);
                status.progress = normalized;
                status.movement_speed_multiplier = 1.0f - SmoothRootSigmoid(normalized);
                status.accumulated_magnitude += Constants::kEarthRootedDamagePerSecond * applied_dt;
                const int whole_damage = static_cast<int>(std::floor(status.accumulated_magnitude + 0.0001f));
                if (whole_damage > 0) {
                    status.accumulated_magnitude -= static_cast<float>(whole_damage);
                    ApplyDamageToPlayer(player, -1, whole_damage, "earth_roots", false);
                }
                if (status.source_active) {
                    status.remaining_seconds =
                        std::max(status.remaining_seconds, Constants::kEarthRootedVisibleDurationSeconds);
                    status.total_seconds = std::max(status.total_seconds, Constants::kEarthRootedVisibleDurationSeconds);
                }
                break;
            }
            case StatusEffectType::RootedRecovery: {
                status.source_elapsed_seconds += applied_dt;
                const float recover_duration = std::max(0.001f, status.burn_duration_seconds);
                const float normalized = std::clamp(status.source_elapsed_seconds / recover_duration, 0.0f, 1.0f);
                const float base_progress = std::clamp(status.progress, 0.0f, 1.0f);
                const float active_progress = std::max(0.0f, base_progress * (1.0f - normalized));
                status.movement_speed_multiplier = 1.0f - SmoothRootSigmoid(active_progress);
                break;
            }
        }
    }

    if (HasStatusEffect(player, StatusEffectType::Stunned)) {
        player.rune_placing_mode = false;
        player.inventory_mode = false;
        player.melee_active_remaining = 0.0f;
        player.vel = {0.0f, 0.0f};
        player.action_state = PlayerActionState::Idle;
    }

    player.status_effects.erase(
        std::remove_if(player.status_effects.begin(), player.status_effects.end(),
                       [](const StatusEffectInstance& status) { return status.remaining_seconds <= 0.0f; }),
        player.status_effects.end());
}

float GameApp::GetPlayerMovementSpeedMultiplier(const Player& player) const {
    float multiplier = 1.0f;
    for (const auto& status : player.status_effects) {
        if (status.remaining_seconds <= 0.0f) {
            continue;
        }
        if (status.type == StatusEffectType::Rooted || status.type == StatusEffectType::RootedRecovery) {
            multiplier = std::min(multiplier, std::clamp(status.movement_speed_multiplier, 0.0f, 1.0f));
        }
    }
    return multiplier;
}

void GameApp::UpdateDamagePopups(float dt) {
    for (auto& popup : state_.damage_popups) {
        if (!popup.alive) {
            continue;
        }
        popup.age_seconds += dt;
        popup.pos.y -= popup.rise_per_second * dt;
        if (popup.age_seconds >= popup.lifetime_seconds) {
            popup.alive = false;
        }
    }

    state_.damage_popups.erase(
        std::remove_if(state_.damage_popups.begin(), state_.damage_popups.end(),
                       [](const DamagePopup& popup) { return !popup.alive; }),
        state_.damage_popups.end());
}

MapObjectInstance* GameApp::FindMapObjectById(int id) {
    auto it = std::find_if(state_.map_objects.begin(), state_.map_objects.end(),
                           [&](const MapObjectInstance& object) { return object.id == id; });
    return it == state_.map_objects.end() ? nullptr : &(*it);
}

const MapObjectInstance* GameApp::FindMapObjectById(int id) const {
    auto it = std::find_if(state_.map_objects.begin(), state_.map_objects.end(),
                           [&](const MapObjectInstance& object) { return object.id == id; });
    return it == state_.map_objects.end() ? nullptr : &(*it);
}

const ObjectPrototype* GameApp::FindObjectPrototype(const std::string& prototype_id) const {
    return objects_database_.FindById(prototype_id);
}

void GameApp::SpawnObjectInstanceAtCell(const std::string& prototype_id, const GridCoord& cell) {
    const ObjectPrototype* proto = FindObjectPrototype(prototype_id);
    if (proto == nullptr || proto->type == ObjectType::Terrain) {
        return;
    }
    if (!state_.map.IsInside(cell)) {
        return;
    }

    MapObjectInstance object;
    object.id = state_.next_entity_id++;
    object.prototype_id = prototype_id;
    object.cell = cell;
    object.type = proto->type;
    object.walkable = proto->walkable;
    object.stops_projectiles = proto->stops_projectiles;
    object.collision_enabled = (!proto->walkable || proto->stops_projectiles);
    object.hp = proto->destructible_hp;
    object.state = MapObjectState::Active;
    object.state_time = 0.0f;
    object.death_duration = 0.0f;
    object.alive = true;
    state_.map_objects.push_back(object);
}

void GameApp::RebuildMapObjectsFromSeeds() {
    state_.map_objects.clear();
    for (const auto& seed : state_.map.object_seeds) {
        SpawnObjectInstanceAtCell(seed.prototype_id, seed.cell);
    }
}

void GameApp::ResolvePlayerVsMapObjects(Player& player) {
    if (!player.alive) {
        return;
    }

    for (const auto& object : state_.map_objects) {
        if (!object.alive || !object.collision_enabled || object.walkable) {
            continue;
        }

        const Rectangle aabb = GetMapObjectCollisionAabb(object, state_.map.cell_size);
        Vector2 normal = {0.0f, 0.0f};
        float penetration = 0.0f;
        if (!CollisionWorld::CircleVsAabb(player.pos, player.radius, aabb, normal, penetration)) {
            continue;
        }

        player.pos = Vector2Add(player.pos, Vector2Scale(normal, penetration));
        const float velocity_dot = Vector2DotProduct(player.vel, normal);
        if (velocity_dot < 0.0f) {
            player.vel = Vector2Subtract(player.vel, Vector2Scale(normal, velocity_dot));
        }
    }
}

bool GameApp::ApplyObjectDamage(int object_instance_id, int amount, int /*source_player_id*/, const char* /*source*/) {
    MapObjectInstance* object = FindMapObjectById(object_instance_id);
    if (object == nullptr || !object->alive || amount <= 0 || object->hp <= 0) {
        return false;
    }

    const ObjectPrototype* proto = FindObjectPrototype(object->prototype_id);
    if (proto == nullptr || proto->type != ObjectType::Destructible) {
        return false;
    }

    object->hp = std::max(0, object->hp - amount);
    SpawnDamagePopup(CellToWorldCenter(object->cell), amount, false);

    if (object->hp > 0) {
        return true;
    }

    PlaySfxIfVisible(sfx_vase_breaking_.sound, sfx_vase_breaking_.loaded, CellToWorldCenter(object->cell));

    object->collision_enabled = false;

    for (const DropEntry& drop : proto->on_destroy_drops) {
        std::uniform_real_distribution<float> chance_dist(0.0f, 1.0f);
        if (chance_dist(rng_) > drop.chance) {
            continue;
        }
        const int min_count = std::max(1, drop.min_count);
        const int max_count = std::max(min_count, drop.max_count);
        std::uniform_int_distribution<int> count_dist(min_count, max_count);
        const int count = count_dist(rng_);
        for (int i = 0; i < count; ++i) {
            pending_object_spawns_.push_back({drop.object_id, object->cell});
        }
    }

    if (!proto->death_animation.empty()) {
        object->state = MapObjectState::Dying;
        object->state_time = 0.0f;
        object->death_duration = 0.4f;

        int frame_count = 0;
        float fps = 1.0f;
        const SpriteMetadataLoader* metadata =
            (proto->sprite_sheet == SpriteSheetType::Tall32x64) ? &sprite_metadata_tall_ : &sprite_metadata_;
        if (metadata->GetAnimationStats(proto->death_animation, "default", frame_count, fps) && frame_count > 0) {
            object->death_duration = static_cast<float>(frame_count) / std::max(0.001f, fps);
        }
    } else {
        object->alive = false;
    }

    return true;
}

bool GameApp::TryConsumeObject(int object_instance_id, int player_id) {
    MapObjectInstance* object = FindMapObjectById(object_instance_id);
    Player* player = FindPlayerById(player_id);
    if (object == nullptr || player == nullptr || !object->alive || !player->alive) {
        return false;
    }

    const ObjectPrototype* proto = FindObjectPrototype(object->prototype_id);
    if (proto == nullptr || proto->type != ObjectType::Consumable) {
        return false;
    }

    int target_slot = -1;
    for (size_t i = 0; i < player->item_slots.size(); ++i) {
        if (player->item_slots[i] == object->prototype_id && player->item_slot_counts[i] > 0) {
            target_slot = static_cast<int>(i);
            break;
        }
    }
    if (target_slot < 0) {
        for (size_t i = 0; i < player->item_slots.size(); ++i) {
            if (player->item_slots[i].empty() || player->item_slot_counts[i] <= 0) {
                target_slot = static_cast<int>(i);
                break;
            }
        }
    }
    if (target_slot < 0) {
        return false;
    }

    player->item_slots[target_slot] = object->prototype_id;
    player->item_slot_counts[target_slot] += 1;
    object->collision_enabled = false;
    object->alive = false;
    PlaySfxIfVisible(sfx_item_pickup_.sound, sfx_item_pickup_.loaded, CellToWorldCenter(object->cell));
    return true;
}

bool GameApp::TryActivateItemSlot(Player& player, int slot_index) {
    if (slot_index < 0 || slot_index >= static_cast<int>(player.item_slots.size())) {
        return false;
    }
    if (player.item_slots[slot_index].empty() || player.item_slot_counts[slot_index] <= 0) {
        return false;
    }
    if (player.item_slot_cooldown_remaining[slot_index] > 0.0f) {
        return false;
    }

    const ObjectPrototype* proto = FindObjectPrototype(player.item_slots[slot_index]);
    if (proto == nullptr || proto->type != ObjectType::Consumable) {
        return false;
    }

    const std::string prototype_id = player.item_slots[slot_index];

    for (const EffectSpec& effect : proto->consumable_effects) {
        if (effect.type == EffectType::IncreaseCurrentHealth) {
            ApplyImmediateHeal(player, std::max(0, effect.amount));
        } else if (effect.type == EffectType::SpawnObject) {
            const int min_count = std::max(1, effect.min_count);
            const int max_count = std::max(min_count, effect.max_count);
            std::uniform_int_distribution<int> count_dist(min_count, max_count);
            const int count = count_dist(rng_);
            for (int i = 0; i < count; ++i) {
                pending_object_spawns_.push_back({effect.object_id, WorldToCell(player.pos)});
            }
        }
    }

    if (prototype_id == "health_small") {
        AddRegenerationStatus(player, Constants::kPotionSmallRegenDurationSeconds, Constants::kPotionSmallRegenPerSecond);
    }

    PlaySfxIfVisible(sfx_drink_potion_.sound, sfx_drink_potion_.loaded, player.pos);
    player.item_slot_counts[slot_index] = std::max(0, player.item_slot_counts[slot_index] - 1);
    if (player.item_slot_counts[slot_index] == 0) {
        player.item_slots[slot_index].clear();
    }
    player.item_slot_cooldown_total[slot_index] = 0.25f;
    player.item_slot_cooldown_remaining[slot_index] = player.item_slot_cooldown_total[slot_index];
    return true;
}

bool GameApp::TryActivateItemById(Player& player, const std::string& prototype_id) {
    if (prototype_id.empty()) {
        return false;
    }
    for (int i = 0; i < static_cast<int>(player.item_slots.size()); ++i) {
        if (player.item_slots[i] == prototype_id && player.item_slot_counts[i] > 0) {
            return TryActivateItemSlot(player, i);
        }
    }
    return false;
}

void GameApp::ApplyImmediateHeal(Player& player, int amount) {
    if (amount <= 0 || !player.alive) {
        return;
    }
    const int before_hp = player.hp;
    player.hp = std::min(Constants::kMaxHp, player.hp + amount);
    const int healed = player.hp - before_hp;
    if (healed > 0) {
        SpawnDamagePopup(player.pos, healed, true);
    }
}

void GameApp::AddRegenerationStatus(Player& player, float duration_seconds, float amount_per_second) {
    StatusEffectInstance status;
    status.type = StatusEffectType::Regeneration;
    status.remaining_seconds = std::max(0.0f, duration_seconds);
    status.total_seconds = status.remaining_seconds;
    status.magnitude_per_second = std::max(0.0f, amount_per_second);
    status.accumulated_magnitude = 0.0f;
    status.visible = true;
    status.is_buff = true;
    status.composite_effect_id = "heal_effect";
    player.status_effects.push_back(status);
}

void GameApp::AddStunnedStatus(Player& player, float duration_seconds) {
    for (auto& status : player.status_effects) {
        if (status.type != StatusEffectType::Stunned) {
            continue;
        }
        status.total_seconds = std::max(status.total_seconds, duration_seconds);
        status.remaining_seconds = std::max(status.remaining_seconds, duration_seconds);
        return;
    }

    StatusEffectInstance status;
    status.type = StatusEffectType::Stunned;
    status.remaining_seconds = duration_seconds;
    status.total_seconds = duration_seconds;
    status.magnitude_per_second = 0.0f;
    status.accumulated_magnitude = 0.0f;
    status.visible = true;
    status.is_buff = false;
    status.composite_effect_id.clear();
    player.status_effects.push_back(status);
}

void GameApp::RefreshOrAddRootedStatus(Player& player, int source_id) {
    for (auto& status : player.status_effects) {
        if (status.type == StatusEffectType::RootedRecovery && status.source_id == source_id) {
            status.remaining_seconds = 0.0f;
        }
    }
    for (auto& status : player.status_effects) {
        if (status.type != StatusEffectType::Rooted || status.source_id != source_id) {
            continue;
        }
        status.visible = true;
        status.is_buff = false;
        status.source_active = true;
        status.remaining_seconds = std::max(status.remaining_seconds, Constants::kEarthRootedVisibleDurationSeconds);
        status.total_seconds = std::max(status.total_seconds, Constants::kEarthRootedVisibleDurationSeconds);
        return;
    }

    StatusEffectInstance status;
    status.type = StatusEffectType::Rooted;
    status.remaining_seconds = Constants::kEarthRootedVisibleDurationSeconds;
    status.total_seconds = Constants::kEarthRootedVisibleDurationSeconds;
    status.visible = true;
    status.is_buff = false;
    status.source_id = source_id;
    status.progress = 0.0f;
    status.source_elapsed_seconds = 0.0f;
    status.burn_duration_seconds = Constants::kEarthRootedBurnInSeconds;
    status.movement_speed_multiplier = 1.0f;
    status.source_active = true;
    status.accumulated_magnitude = 0.0f;
    player.status_effects.push_back(status);
}

void GameApp::UpdateMapObjects(float dt) {
    for (auto& object : state_.map_objects) {
        if (!object.alive) {
            continue;
        }

        if (object.state == MapObjectState::Dying) {
            object.state_time += dt;
            if (object.state_time >= object.death_duration) {
                object.alive = false;
            }
            continue;
        }

        if (object.type == ObjectType::Consumable) {
            const Rectangle aabb = GetMapObjectCollisionAabb(object, state_.map.cell_size);
            for (const auto& player : state_.players) {
                if (!player.alive) {
                    continue;
                }
                Vector2 normal = {0.0f, 0.0f};
                float penetration = 0.0f;
                if (!CollisionWorld::CircleVsAabb(player.pos, player.radius, aabb, normal, penetration)) {
                    continue;
                }
                TryConsumeObject(object.id, player.id);
                break;
            }
        }
    }

    for (const auto& pending : pending_object_spawns_) {
        SpawnObjectInstanceAtCell(pending.prototype_id, pending.cell);
    }
    pending_object_spawns_.clear();

    state_.map_objects.erase(
        std::remove_if(state_.map_objects.begin(), state_.map_objects.end(),
                       [](const MapObjectInstance& object) { return !object.alive; }),
        state_.map_objects.end());
}

void GameApp::SpawnDamagePopup(Vector2 world_pos, int amount, bool is_heal) {
    if (amount <= 0) {
        return;
    }
    DamagePopup popup;
    popup.pos = world_pos;
    popup.amount = amount;
    popup.is_heal = is_heal;
    popup.age_seconds = 0.0f;
    popup.lifetime_seconds = Constants::kDamagePopupLifetimeSeconds;
    popup.rise_per_second = Constants::kDamagePopupRisePerSecond;
    popup.alive = true;
    state_.damage_popups.push_back(popup);
}

bool GameApp::ApplyDamageToPlayer(Player& target, int attacker_player_id, int damage, const char* source,
                                  bool count_kill_for_attacker) {
    if (!target.alive || damage <= 0) {
        return false;
    }

    target.hp = std::max(0, target.hp - damage);
    event_queue_.Push(PlayerHitEvent{attacker_player_id, target.id, damage, source != nullptr ? source : "unknown"});
    SpawnDamagePopup(target.pos, damage, false);
    PlaySfxIfVisible(sfx_player_damaged_.sound, sfx_player_damaged_.loaded, target.pos);

    if (target.hp <= 0) {
        HandlePlayerDeath(target, attacker_player_id, count_kill_for_attacker);
    }
    return true;
}

void GameApp::HandlePlayerDeath(Player& victim, int killer_player_id, bool count_kill_for_attacker) {
    if (!victim.alive) {
        return;
    }

    victim.alive = false;
    victim.hp = 0;
    victim.awaiting_respawn = true;
    victim.respawn_remaining = Constants::kRespawnDelaySeconds;
    victim.rune_placing_mode = false;
    victim.melee_active_remaining = 0.0f;
    victim.melee_cooldown_remaining = 0.0f;
    victim.melee_hit_target_ids.clear();
    victim.melee_hit_object_ids.clear();
    victim.outside_zone_damage_accumulator = 0.0f;
    victim.action_state = PlayerActionState::Idle;
    victim.vel = {0.0f, 0.0f};
    victim.status_effects.clear();
    event_queue_.Push(PlayerDiedEvent{victim.id, killer_player_id});
    PlaySfxIfVisible(sfx_player_death_.sound, sfx_player_death_.loaded, victim.pos);

    if (!count_kill_for_attacker || killer_player_id < 0 || killer_player_id == victim.id) {
        return;
    }

    if (Player* killer = FindPlayerById(killer_player_id)) {
        killer->kills += 1;
        if (killer->team == Constants::kTeamRed) {
            state_.match.red_team_kills += 1;
        } else {
            state_.match.blue_team_kills += 1;
        }
    }
}

bool GameApp::IsOutsideArena(Vector2 world_pos) const {
    if (state_.match.arena_radius_world <= 0.0f) {
        return false;
    }
    const Vector2 delta = Vector2Subtract(world_pos, state_.match.arena_center_world);
    return Vector2LengthSqr(delta) > state_.match.arena_radius_world * state_.match.arena_radius_world;
}

Vector2 GameApp::ClampToArenaWithBuffer(Vector2 world_pos, float buffer_tiles) const {
    const float buffer_world = std::max(0.0f, buffer_tiles) * static_cast<float>(state_.map.cell_size);
    const float allowed_radius = std::max(0.0f, state_.match.arena_radius_world + buffer_world);
    const Vector2 center = state_.match.arena_center_world;
    const Vector2 delta = Vector2Subtract(world_pos, center);
    const float distance = Vector2Length(delta);
    if (distance <= allowed_radius || distance <= 0.0001f) {
        return world_pos;
    }
    return Vector2Add(center, Vector2Scale(delta, allowed_radius / distance));
}

Vector2 GameApp::ComputeRespawnPosition(const Player& player) const {
    Vector2 respawn = CellToWorldCenter(player.spawn_cell);
    respawn = ClampToArenaWithBuffer(respawn, Constants::kArenaSpawnBufferTiles);

    const float map_width = static_cast<float>(state_.map.width * state_.map.cell_size);
    const float map_height = static_cast<float>(state_.map.height * state_.map.cell_size);
    respawn.x = std::clamp(respawn.x, player.radius, std::max(player.radius, map_width - player.radius));
    respawn.y = std::clamp(respawn.y, player.radius, std::max(player.radius, map_height - player.radius));
    return respawn;
}

void GameApp::UpdateIceWalls(float dt) {
    int death_frame_count = 0;
    float death_fps = 1.0f;
    float death_duration = 0.5f;
    if (sprite_metadata_.GetAnimationStats("ice_wall_death", "default", death_frame_count, death_fps)) {
        death_duration = static_cast<float>(death_frame_count) / std::max(0.001f, death_fps);
    }

    for (auto& wall : state_.ice_walls) {
        if (!wall.alive) {
            continue;
        }

        wall.state_time += dt;
        switch (wall.state) {
            case IceWallState::Materializing:
                if (wall.state_time >= Constants::kIceWallMaterializeSeconds) {
                    wall.state = IceWallState::Active;
                    wall.state_time = 0.0f;
                    PushPlayersOutOfIceWall(wall.cell);
                }
                break;
            case IceWallState::Active:
                wall.hp = std::max(0.0f, wall.hp - Constants::kIceWallHpDecayPerSecond * dt);
                if (wall.hp <= 0.0f) {
                    wall.state = IceWallState::Dying;
                    wall.state_time = 0.0f;
                    PlaySfxIfVisible(sfx_ice_wall_melt_.sound, sfx_ice_wall_melt_.loaded, CellToWorldCenter(wall.cell));
                }
                break;
            case IceWallState::Dying:
                if (wall.state_time >= death_duration) {
                    wall.alive = false;
                }
                break;
        }
    }

    state_.ice_walls.erase(
        std::remove_if(state_.ice_walls.begin(), state_.ice_walls.end(),
                       [](const IceWallPiece& wall) { return !wall.alive; }),
        state_.ice_walls.end());
}

void GameApp::UpdateRunes(float dt) {
    std::vector<RunePlacedEvent> activated_runes;
    std::vector<int> expired_rune_ids;
    for (auto& rune : state_.runes) {
        if (!rune.active) {
            rune.activation_remaining_seconds = std::max(0.0f, rune.activation_remaining_seconds - dt);
            if (rune.activation_remaining_seconds > 0.0f) {
                continue;
            }

            rune.active = true;
            activated_runes.push_back(
                RunePlacedEvent{rune.owner_player_id, rune.owner_team, rune.cell, rune.rune_type, rune.placement_order});
            continue;
        }

        if (rune.rune_type != RuneType::Earth) {
            continue;
        }

        switch (rune.earth_trap_state) {
            case EarthRuneTrapState::IdleRune: {
                bool enemy_in_range = false;
                for (const auto& player : state_.players) {
                    if (!player.alive || player.team == rune.owner_team) {
                        continue;
                    }
                    const GridCoord player_cell = WorldToCell(player.pos);
                    if (std::abs(player_cell.x - rune.cell.x) <= 1 && std::abs(player_cell.y - rune.cell.y) <= 1) {
                        enemy_in_range = true;
                        break;
                    }
                }
                if (!enemy_in_range) {
                    break;
                }
                rune.earth_trap_state = EarthRuneTrapState::Slamming;
                rune.earth_state_time = 0.0f;
                int frame_count = 0;
                float fps = 1.0f;
                rune.earth_state_duration = Constants::kEarthRuneSlamFallbackSeconds;
                if (sprite_metadata_.GetAnimationStats("earth_rune_slam", "default", frame_count, fps) && frame_count > 0) {
                    rune.earth_state_duration =
                        (static_cast<float>(frame_count) / std::max(0.001f, fps)) * Constants::kEarthRuneSlamSlowdown;
                }
                rune.earth_roots_spawned = false;
                rune.creates_influence_zone = false;
                PlaySfxIfVisible(sfx_earth_rune_launch_.sound, sfx_earth_rune_launch_.loaded, CellToWorldCenter(rune.cell));
                break;
            }
            case EarthRuneTrapState::Slamming: {
                rune.earth_state_time += dt;
                if (!rune.earth_roots_spawned &&
                    rune.earth_state_time >= rune.earth_state_duration * Constants::kEarthRuneSlamSpawnRootsProgress) {
                    rune.earth_roots_group_id = SpawnEarthRootsGroup(rune.owner_player_id, rune.owner_team, rune.cell);
                    rune.earth_roots_spawned = true;
                    const Vector2 impact_center = CellToWorldCenter(rune.cell);
                    PlaySfxIfVisible(sfx_earth_rune_impact_.sound, sfx_earth_rune_impact_.loaded, impact_center);
                    if (IsWorldPointInsideCameraView(impact_center)) {
                        camera_shake_time_remaining_ =
                            std::max(camera_shake_time_remaining_, Constants::kCameraShakeDurationSeconds);
                    }
                }
                if (rune.earth_state_time >= rune.earth_state_duration) {
                    rune.earth_trap_state = EarthRuneTrapState::RootedIdle;
                    rune.earth_state_time = 0.0f;
                    rune.earth_state_duration = Constants::kEarthRuneTrapPersistSeconds;
                }
                break;
            }
            case EarthRuneTrapState::RootedIdle:
                rune.earth_state_time += dt;
                if (rune.earth_state_time >= rune.earth_state_duration) {
                    rune.earth_trap_state = EarthRuneTrapState::RootedDeath;
                    rune.earth_state_time = 0.0f;
                    int frame_count = 0;
                    float fps = 1.0f;
                    rune.earth_state_duration = Constants::kEarthRuneRootedDeathFallbackSeconds;
                    if (sprite_metadata_.GetAnimationStats("earth_rune_rooted_death", "default", frame_count, fps) &&
                        frame_count > 0) {
                        rune.earth_state_duration = static_cast<float>(frame_count) / std::max(0.001f, fps);
                    }
                }
                break;
            case EarthRuneTrapState::RootedDeath:
                rune.earth_state_time += dt;
                if (rune.earth_state_time >= rune.earth_state_duration) {
                    expired_rune_ids.push_back(rune.id);
                }
                break;
        }
    }

    if (!expired_rune_ids.empty()) {
        state_.runes.erase(
            std::remove_if(state_.runes.begin(), state_.runes.end(),
                           [&](const Rune& rune) {
                               return std::find(expired_rune_ids.begin(), expired_rune_ids.end(), rune.id) !=
                                      expired_rune_ids.end();
                           }),
            state_.runes.end());
    }

    if (!activated_runes.empty()) {
        RebuildInfluenceZones();
        for (const RunePlacedEvent& event : activated_runes) {
            event_queue_.Push(event);
        }
    } else if (!state_.influence_zones.empty()) {
        RebuildInfluenceZones();
    }
}

int GameApp::SpawnEarthRootsGroup(int owner_player_id, int owner_team, const GridCoord& center_cell) {
    EarthRootsGroup group;
    group.id = state_.next_entity_id++;
    group.owner_player_id = owner_player_id;
    group.owner_team = owner_team;
    group.center_cell = center_cell;
    group.state = EarthRootsGroupState::Born;
    group.state_time = 0.0f;
    int frame_count = 0;
    float fps = 1.0f;
    group.state_duration = 0.5f;
    if (sprite_metadata_.GetAnimationStats("roots_born_back", "default", frame_count, fps) && frame_count > 0) {
        group.state_duration = static_cast<float>(frame_count) / std::max(0.001f, fps);
    }
    group.idle_lifetime_remaining_seconds = Constants::kEarthRuneTrapPersistSeconds;
    group.active_for_gameplay = true;
    group.alive = true;
    state_.earth_roots_groups.push_back(group);
    return group.id;
}

void GameApp::UpdateEarthRootsGroups(float dt) {
    for (auto& player : state_.players) {
        for (auto& status : player.status_effects) {
            if (status.type == StatusEffectType::Rooted) {
                status.source_active = false;
            }
        }
    }

    for (auto& group : state_.earth_roots_groups) {
        if (!group.alive) {
            continue;
        }

        if (group.active_for_gameplay) {
            for (auto& player : state_.players) {
                if (!player.alive || player.team == group.owner_team) {
                    continue;
                }
                const GridCoord cell = WorldToCell(player.pos);
                if (std::abs(cell.x - group.center_cell.x) <= 1 && std::abs(cell.y - group.center_cell.y) <= 1) {
                    RefreshOrAddRootedStatus(player, group.id);
                }
            }
        }

        group.state_time += dt;
        switch (group.state) {
            case EarthRootsGroupState::Born:
                if (group.state_time >= group.state_duration) {
                    group.state = EarthRootsGroupState::Idle;
                    group.state_time = 0.0f;
                    group.state_duration = 0.0f;
                }
                break;
            case EarthRootsGroupState::Idle:
                group.idle_lifetime_remaining_seconds = std::max(0.0f, group.idle_lifetime_remaining_seconds - dt);
                if (group.idle_lifetime_remaining_seconds <= 0.0f) {
                    group.state = EarthRootsGroupState::Dying;
                    group.state_time = 0.0f;
                    group.active_for_gameplay = false;
                    int frame_count = 0;
                    float fps = 1.0f;
                    group.state_duration = 0.5f;
                    if (sprite_metadata_.GetAnimationStats("roots_death_back", "default", frame_count, fps) && frame_count > 0) {
                        group.state_duration = static_cast<float>(frame_count) / std::max(0.001f, fps);
                    }
                }
                break;
            case EarthRootsGroupState::Dying:
                if (group.state_time >= group.state_duration) {
                    group.alive = false;
                }
                break;
        }
    }

    for (auto& player : state_.players) {
        std::vector<StatusEffectInstance> recovery_to_add;
        for (auto& status : player.status_effects) {
            if (status.type != StatusEffectType::Rooted || status.remaining_seconds <= 0.0f || status.source_active) {
                continue;
            }
            StatusEffectInstance recovery;
            recovery.type = StatusEffectType::RootedRecovery;
            recovery.remaining_seconds = Constants::kEarthRootedRecoverSeconds;
            recovery.total_seconds = Constants::kEarthRootedRecoverSeconds;
            recovery.visible = false;
            recovery.is_buff = false;
            recovery.source_id = status.source_id;
            recovery.progress = status.progress;
            recovery.source_elapsed_seconds = 0.0f;
            recovery.burn_duration_seconds = Constants::kEarthRootedRecoverSeconds;
            recovery.movement_speed_multiplier = status.movement_speed_multiplier;
            recovery.source_active = false;
            recovery_to_add.push_back(recovery);
            status.remaining_seconds = 0.0f;
        }
        for (auto& recovery : recovery_to_add) {
            bool replaced = false;
            for (auto& existing : player.status_effects) {
                if (existing.type == StatusEffectType::RootedRecovery && existing.source_id == recovery.source_id) {
                    existing = recovery;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                player.status_effects.push_back(recovery);
            }
        }
    }

    state_.earth_roots_groups.erase(
        std::remove_if(state_.earth_roots_groups.begin(), state_.earth_roots_groups.end(),
                       [](const EarthRootsGroup& group) { return !group.alive; }),
        state_.earth_roots_groups.end());
}

void GameApp::RebuildInfluenceZones() {
    state_.influence_zones.clear();
    for (const auto& rune : state_.runes) {
        if (!rune.active || !rune.creates_influence_zone) {
            continue;
        }
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const GridCoord cell = {rune.cell.x + dx, rune.cell.y + dy};
                if (!state_.map.IsInside(cell)) {
                    continue;
                }
                auto it = std::find_if(state_.influence_zones.begin(), state_.influence_zones.end(),
                                       [&](const InfluenceZoneCell& zone) { return zone.cell == cell; });
                if (it != state_.influence_zones.end()) {
                    it->team = rune.owner_team;
                    it->source_rune_id = rune.id;
                } else {
                    state_.influence_zones.push_back(InfluenceZoneCell{rune.id, rune.owner_team, cell});
                }
            }
        }
    }

    for (const auto& rune : state_.runes) {
        if (!rune.active || !rune.volatile_cast) {
            continue;
        }
        state_.influence_zones.erase(
            std::remove_if(state_.influence_zones.begin(), state_.influence_zones.end(),
                           [&](const InfluenceZoneCell& zone) {
                               return zone.cell == rune.cell && zone.team != rune.owner_team;
                           }),
            state_.influence_zones.end());
    }
}

void GameApp::ResolvePlayerVsIceWalls(Player& player) {
    if (!player.alive) {
        return;
    }

    const int cell_size = state_.map.cell_size;
    std::unordered_set<int64_t> visited_components;
    for (const auto& wall : state_.ice_walls) {
        if (!wall.alive || wall.state != IceWallState::Active) {
            continue;
        }
        const int64_t key = MakeGridKey(wall.cell);
        if (visited_components.count(key)) {
            continue;
        }

        const std::vector<GridCoord> component = CollectConnectedActiveIceWallCells(state_.ice_walls, wall.cell);
        for (const GridCoord& cell : component) {
            visited_components.insert(MakeGridKey(cell));
        }
        if (!IntersectsTileComponent(player.pos, player.radius, cell_size, component)) {
            continue;
        }

        Vector2 resolved = player.pos;
        if (!CollisionWorld::FindClosestBoundaryExitForTileComponent(player.pos, player.radius, cell_size, component,
                                                                     resolved)) {
            continue;
        }

        const Vector2 new_pos = MoveTowardResolvedPosition(player.pos, resolved);
        const Vector2 push = Vector2Subtract(new_pos, player.pos);
        player.pos = new_pos;
        if (Vector2LengthSqr(push) <= 0.0001f) {
            continue;
        }
        const Vector2 normal = Vector2Normalize(push);
        const float velocity_dot = Vector2DotProduct(player.vel, normal);
        if (velocity_dot < 0.0f) {
            player.vel = Vector2Subtract(player.vel, Vector2Scale(normal, velocity_dot));
        }
    }
}

void GameApp::ResolvePlayerVsIceWallsLocal(Player& player) {
    if (!player.alive) {
        return;
    }

    const int cell_size = state_.map.cell_size;
    for (const auto& wall : state_.ice_walls) {
        if (!wall.alive || wall.state != IceWallState::Active) {
            continue;
        }

        const Rectangle aabb = {static_cast<float>(wall.cell.x * cell_size), static_cast<float>(wall.cell.y * cell_size),
                                static_cast<float>(cell_size), static_cast<float>(cell_size)};
        Vector2 normal = {0.0f, 0.0f};
        float penetration = 0.0f;
        if (!CollisionWorld::CircleVsAabb(player.pos, player.radius, aabb, normal, penetration)) {
            continue;
        }

        player.pos = Vector2Add(player.pos, Vector2Scale(normal, penetration));
        const float velocity_dot = Vector2DotProduct(player.vel, normal);
        if (velocity_dot < 0.0f) {
            player.vel = Vector2Subtract(player.vel, Vector2Scale(normal, velocity_dot));
        }
    }
}

void GameApp::PushPlayersOutOfIceWall(const GridCoord& cell) {
    const int cell_size = state_.map.cell_size;
    const std::vector<GridCoord> component = CollectConnectedActiveIceWallCells(state_.ice_walls, cell);
    if (component.empty()) {
        return;
    }

    for (auto& player : state_.players) {
        if (!player.alive) {
            continue;
        }

        if (!IntersectsTileComponent(player.pos, player.radius, cell_size, component)) {
            continue;
        }

        Vector2 resolved = player.pos;
        if (!CollisionWorld::FindClosestBoundaryExitForTileComponent(player.pos, player.radius, cell_size, component,
                                                                     resolved)) {
            continue;
        }

        const Vector2 new_pos = MoveTowardResolvedPosition(player.pos, resolved);
        const Vector2 push_dir = Vector2Subtract(new_pos, player.pos);
        player.pos = new_pos;
        if (Vector2LengthSqr(push_dir) > 0.0001f) {
            const Vector2 normal = Vector2Normalize(push_dir);
            const float velocity_dot = Vector2DotProduct(player.vel, normal);
            if (velocity_dot < 0.0f) {
                player.vel = Vector2Subtract(player.vel, Vector2Scale(normal, velocity_dot));
            }
        }
        CollisionWorld::ResolvePlayerVsWorld(state_.map, player);
    }
}

void GameApp::UpdateProjectiles(float dt) {
    for (auto& projectile : state_.projectiles) {
        if (!projectile.alive) {
            continue;
        }

        if (projectile.upgrade_pause_remaining > 0.0f) {
            projectile.upgrade_pause_remaining = std::max(0.0f, projectile.upgrade_pause_remaining - dt);
            if (projectile.upgrade_pause_remaining > 0.0f) {
                continue;
            }
            projectile.vel = projectile.resume_vel;
        }

        projectile.vel = Vector2Add(projectile.vel, Vector2Scale(projectile.acc, dt));
        const float drag_factor = std::max(0.0f, 1.0f - projectile.drag * dt);
        projectile.vel = Vector2Scale(projectile.vel, drag_factor);
        projectile.pos = Vector2Add(projectile.pos, Vector2Scale(projectile.vel, dt));

        if (!IsStaticFireBolt(projectile)) {
            for (const auto& dummy : state_.fire_storm_dummies) {
                if (!dummy.alive || dummy.owner_team != projectile.owner_team) {
                    continue;
                }
                const Rectangle cell_aabb = {static_cast<float>(dummy.cell.x * state_.map.cell_size),
                                             static_cast<float>(dummy.cell.y * state_.map.cell_size),
                                             static_cast<float>(state_.map.cell_size),
                                             static_cast<float>(state_.map.cell_size)};
                Vector2 normal = {0.0f, 0.0f};
                float penetration = 0.0f;
                if (!CollisionWorld::CircleVsAabb(projectile.pos, projectile.radius, cell_aabb, normal, penetration)) {
                    continue;
                }

                if (projectile.animation_key == "projectile_fire_bolt") {
                    projectile.animation_key = "fire_storm_static_large";
                    projectile.radius *= Constants::kStaticFireBoltScaleMultiplier;
                    projectile.resume_vel = Vector2Scale(projectile.vel, Constants::kStaticFireBoltSpeedMultiplier);
                    projectile.vel = {0.0f, 0.0f};
                    projectile.upgrade_pause_remaining = Constants::kStaticFireBoltUpgradePauseSeconds;

                    Particle upgrade_vfx;
                    upgrade_vfx.pos = projectile.pos;
                    upgrade_vfx.vel = {0.0f, 0.0f};
                    upgrade_vfx.acc = {0.0f, 0.0f};
                    upgrade_vfx.drag = 0.0f;
                    upgrade_vfx.animation_key = "static_upgrade";
                    upgrade_vfx.facing = "default";
                    upgrade_vfx.age_seconds = 0.0f;
                    upgrade_vfx.max_cycles = 1;
                    upgrade_vfx.alive = true;
                    state_.particles.push_back(upgrade_vfx);
                    PlaySfxIfVisible(sfx_static_upgrade_.sound, sfx_static_upgrade_.loaded, projectile.pos);
                }
                break;
            }
        }

        bool destroy_projectile = false;
        std::optional<int> excluded_target_id;

        const GridCoord cell = WorldToCell(projectile.pos);
        if (!state_.map.IsInside(cell)) {
            destroy_projectile = true;
        }

        if (!destroy_projectile) {
            for (auto& object : state_.map_objects) {
                if (!object.alive || !object.collision_enabled || !object.stops_projectiles) {
                    continue;
                }

                const Rectangle aabb = GetMapObjectCollisionAabb(object, state_.map.cell_size);
                Vector2 normal = {0.0f, 0.0f};
                float penetration = 0.0f;
                if (!CollisionWorld::CircleVsAabb(projectile.pos, projectile.radius, aabb, normal, penetration)) {
                    continue;
                }

                ApplyObjectDamage(object.id, projectile.damage, projectile.owner_player_id, "projectile");
                destroy_projectile = true;
                break;
            }
        }

        if (!destroy_projectile) {
            const int wall_cell_size = state_.map.cell_size;
            for (auto& wall : state_.ice_walls) {
                if (!wall.alive || wall.state != IceWallState::Active) {
                    continue;
                }

                const Rectangle aabb = {static_cast<float>(wall.cell.x * wall_cell_size),
                                        static_cast<float>(wall.cell.y * wall_cell_size),
                                        static_cast<float>(wall_cell_size), static_cast<float>(wall_cell_size)};
                Vector2 normal = {0.0f, 0.0f};
                float penetration = 0.0f;
                if (CollisionWorld::CircleVsAabb(projectile.pos, projectile.radius, aabb, normal, penetration)) {
                    wall.hp = std::max(0.0f, wall.hp - static_cast<float>(projectile.damage));
                    if (wall.hp <= 0.0f) {
                        wall.state = IceWallState::Dying;
                        wall.state_time = 0.0f;
                    }
                    destroy_projectile = true;
                    break;
                }
            }
        }

        if (!destroy_projectile) {
            for (auto& target : state_.players) {
                if (!target.alive || target.team == projectile.owner_team || target.id == projectile.owner_player_id) {
                    continue;
                }

                Vector2 normal = {0.0f, 0.0f};
                float penetration = 0.0f;
                if (!CollisionWorld::CircleVsCircle(projectile.pos, projectile.radius, target.pos, target.radius,
                                                    normal, penetration)) {
                    continue;
                }

                destroy_projectile = true;
                excluded_target_id = target.id;
                ApplyDamageToPlayer(target, projectile.owner_player_id, projectile.damage, "fire_bolt", true);
                if (IsStaticFireBolt(projectile)) {
                    AddStunnedStatus(target, Constants::kStaticFireBoltStunSeconds);
                }

                break;
            }
        }

        if (destroy_projectile) {
            SpawnProjectileExplosion(projectile, excluded_target_id);
            projectile.alive = false;
        }
    }

    state_.projectiles.erase(std::remove_if(state_.projectiles.begin(), state_.projectiles.end(),
                                            [](const Projectile& projectile) { return !projectile.alive; }),
                             state_.projectiles.end());
}

void GameApp::SpawnProjectileExplosion(const Projectile& projectile, std::optional<int> excluded_target_id) {
    Explosion explosion;
    explosion.owner_player_id = projectile.owner_player_id;
    explosion.owner_team = projectile.owner_team;
    explosion.pos = projectile.pos;
    explosion.damage = projectile.damage;
    explosion.radius = Constants::kFireBoltExplosionRadius;
    explosion.duration_seconds = Constants::kFireBoltExplosionFallbackDuration;

    if (excluded_target_id.has_value()) {
        explosion.excluded_target_ids.push_back(*excluded_target_id);
    }

    int frame_count = 0;
    float fps = 0.0f;
    if (sprite_metadata_.GetAnimationStats("explosion", "default", frame_count, fps)) {
        explosion.duration_seconds = static_cast<float>(frame_count) / std::max(0.001f, fps);
    }

    state_.explosions.push_back(explosion);

    const float half_w = static_cast<float>(GetScreenWidth()) / std::max(0.001f, (2.0f * camera_.zoom));
    const float half_h = static_cast<float>(GetScreenHeight()) / std::max(0.001f, (2.0f * camera_.zoom));
    const float pad = explosion.radius;
    const bool in_camera_view =
        projectile.pos.x >= (camera_.target.x - half_w - pad) && projectile.pos.x <= (camera_.target.x + half_w + pad) &&
        projectile.pos.y >= (camera_.target.y - half_h - pad) && projectile.pos.y <= (camera_.target.y + half_h + pad);
    if (in_camera_view) {
        camera_shake_time_remaining_ = std::max(camera_shake_time_remaining_, Constants::kCameraShakeDurationSeconds);
    }
    if (IsStaticFireBolt(projectile)) {
        PlaySfxIfVisible(sfx_static_bolt_impact_.sound, sfx_static_bolt_impact_.loaded, projectile.pos);
    } else {
        PlaySfxIfVisible(sfx_explosion_.sound, sfx_explosion_.loaded, projectile.pos);
    }

    Particle explosion_vfx;
    explosion_vfx.pos = projectile.pos;
    explosion_vfx.vel = {0.0f, 0.0f};
    explosion_vfx.acc = {0.0f, 0.0f};
    explosion_vfx.drag = 0.0f;
    explosion_vfx.animation_key = "explosion";
    explosion_vfx.facing = "default";
    explosion_vfx.age_seconds = 0.0f;
    explosion_vfx.max_cycles = 1;
    explosion_vfx.alive = true;
    state_.particles.push_back(explosion_vfx);
}

void GameApp::UpdateExplosions(float dt) {
    auto has_player_id = [](const std::vector<int>& ids, int player_id) {
        return std::find(ids.begin(), ids.end(), player_id) != ids.end();
    };

    for (auto& explosion : state_.explosions) {
        if (!explosion.alive) {
            continue;
        }

        explosion.age_seconds += dt;

        for (auto& target : state_.players) {
            if (!target.alive || target.team == explosion.owner_team || target.id == explosion.owner_player_id) {
                continue;
            }
            if (has_player_id(explosion.excluded_target_ids, target.id) ||
                has_player_id(explosion.already_hit_target_ids, target.id)) {
                continue;
            }

            Vector2 normal = {0.0f, 0.0f};
            float penetration = 0.0f;
            if (!CollisionWorld::CircleVsCircle(explosion.pos, explosion.radius, target.pos, target.radius, normal,
                                                penetration)) {
                continue;
            }

            explosion.already_hit_target_ids.push_back(target.id);
            ApplyDamageToPlayer(target, explosion.owner_player_id, explosion.damage, "fire_bolt_explosion", true);
        }

        if (explosion.age_seconds >= explosion.duration_seconds) {
            explosion.alive = false;
        }
    }

    state_.explosions.erase(
        std::remove_if(state_.explosions.begin(), state_.explosions.end(),
                       [](const Explosion& explosion) { return !explosion.alive; }),
        state_.explosions.end());
}

void GameApp::SpawnLightningEffect(Vector2 start, Vector2 end, float idle_duration_seconds, bool volatile_variant) {
    const float half_tile = 0.5f * static_cast<float>(std::max(1, state_.map.cell_size));
    const Vector2 half_tile_offset = {0.0f, half_tile};

    LightningEffect effect;
    effect.id = state_.next_entity_id++;
    effect.start = Vector2Add(start, half_tile_offset);
    effect.end = Vector2Add(end, half_tile_offset);
    effect.idle_duration = std::max(0.0f, idle_duration_seconds);
    effect.volatile_variant = volatile_variant;
    effect.elapsed = 0.0f;
    effect.dying = false;
    effect.death_elapsed = 0.0f;
    effect.death_duration = 0.25f;
    effect.alive = true;

    const Vector2 delta = Vector2Subtract(end, start);
    const float distance = Vector2Length(delta);
    const float target_len = static_cast<float>(std::max(1, state_.map.cell_size));
    effect.segment_count = std::max(1, static_cast<int>(std::round(distance / std::max(1.0f, target_len))));

    int idle_frame_count = 0;
    float idle_fps = 1.0f;
    float cycle_seconds = 0.25f;
    const char* idle_key = volatile_variant ? "lightning_volatile_magic_idle" : "lightning_magic_idle";
    const char* death_key = volatile_variant ? "lightning_volatile_magic_death" : "lightning_magic_death";
    if (sprite_metadata_.GetAnimationStats(idle_key, "default", idle_frame_count, idle_fps) &&
        idle_frame_count > 0) {
        cycle_seconds = static_cast<float>(idle_frame_count) / std::max(0.001f, idle_fps);
    }

    effect.segment_phase_offsets_seconds.resize(static_cast<size_t>(effect.segment_count), 0.0f);
    std::uniform_real_distribution<float> phase_dist(0.0f, std::max(0.0f, cycle_seconds - 0.0001f));
    for (float& phase : effect.segment_phase_offsets_seconds) {
        phase = phase_dist(rng_);
    }

    int death_frame_count = 0;
    float death_fps = 1.0f;
    if (sprite_metadata_.GetAnimationStats(death_key, "default", death_frame_count, death_fps) &&
        death_frame_count > 0) {
        effect.death_duration = static_cast<float>(death_frame_count) / std::max(0.001f, death_fps);
    }

    state_.lightning_effects.push_back(effect);
}

void GameApp::UpdateLightningEffects(float dt) {
    for (auto& effect : state_.lightning_effects) {
        if (!effect.alive) {
            continue;
        }
        if (!effect.dying) {
            effect.elapsed += dt;
            if (effect.elapsed >= effect.idle_duration) {
                effect.dying = true;
                effect.death_elapsed = 0.0f;
            }
        } else {
            effect.death_elapsed += dt;
            if (effect.death_elapsed >= effect.death_duration) {
                effect.alive = false;
            }
        }
    }

    state_.lightning_effects.erase(
        std::remove_if(state_.lightning_effects.begin(), state_.lightning_effects.end(),
                       [](const LightningEffect& effect) { return !effect.alive; }),
        state_.lightning_effects.end());
}

int GameApp::SpawnCompositeEffect(const std::string& effect_id, Vector2 origin_world) {
    const CompositeEffectDefinition* definition = composite_effects_loader_.FindById(effect_id);
    if (definition == nullptr) {
        return -1;
    }

    const auto get_metadata = [&](CompositeEffectSheet sheet) -> const SpriteMetadataLoader* {
        switch (sheet) {
            case CompositeEffectSheet::Base32:
                return &sprite_metadata_;
            case CompositeEffectSheet::Tall32x64:
                return &sprite_metadata_tall_;
            case CompositeEffectSheet::Large96x96:
                return &sprite_metadata_96x96_;
        }
        return &sprite_metadata_;
    };

    float duration_seconds = 0.0f;
    for (const auto& part : definition->parts) {
        int frame_count = 0;
        float fps = 1.0f;
        const SpriteMetadataLoader* metadata = get_metadata(part.sheet);
        if (metadata != nullptr && metadata->GetAnimationStats(part.animation, "default", frame_count, fps) &&
            frame_count > 0) {
            duration_seconds = std::max(duration_seconds, static_cast<float>(frame_count) / std::max(0.001f, fps));
        }
    }

    CompositeEffectInstance instance;
    instance.id = state_.next_entity_id++;
    instance.effect_id = effect_id;
    instance.origin_world = origin_world;
    instance.duration_seconds = duration_seconds;
    state_.composite_effects.push_back(instance);
    return instance.id;
}

bool GameApp::IsPlayerBeingPulled(int player_id) const {
    return std::any_of(state_.grappling_hooks.begin(), state_.grappling_hooks.end(),
                       [&](const GrapplingHook& hook) {
                           return hook.alive && hook.owner_player_id == player_id &&
                                  hook.phase == GrapplingHookPhase::Pulling;
                       });
}

bool GameApp::TryStartGrapplingHook(Player& player, Vector2 target_world, bool play_audio) {
    if (!player.alive || player.grappling_cooldown_remaining > 0.0f || IsPlayerBeingPulled(player.id)) {
        return false;
    }

    Vector2 delta = Vector2Subtract(target_world, player.pos);
    if (Vector2LengthSqr(delta) < 0.0001f) {
        delta = {1.0f, 0.0f};
    }
    const Vector2 direction = Vector2Normalize(delta);
    const float max_distance = Constants::kGrapplingHookRangeTiles * static_cast<float>(state_.map.cell_size);
    const Vector2 clamped_target = Vector2Add(player.pos, Vector2Scale(direction, max_distance));
    const Vector2 collision_offset = {0.0f, -0.5f * static_cast<float>(state_.map.cell_size)};

    GrapplingHook hook;
    hook.id = network_manager_.IsHost() ? state_.next_entity_id++ : next_predicted_entity_id_--;
    hook.owner_player_id = player.id;
    hook.owner_team = player.team;
    hook.head_pos = player.pos;
    hook.target_pos = clamped_target;
    hook.latch_point = clamped_target;
    hook.pull_destination = player.pos;
    hook.phase = GrapplingHookPhase::Firing;
    hook.latch_target_type = GrapplingHookLatchTargetType::None;
    hook.latch_target_id = -1;
    hook.latch_cell = WorldToCell(clamped_target);
    hook.latched = false;
    hook.animation_time = 0.0f;
    hook.pull_elapsed_seconds = 0.0f;
    hook.max_pull_duration_seconds = max_distance / std::max(1.0f, Constants::kGrapplingHookPullSpeed);
    hook.alive = true;

    const float step = std::max(1.0f, Constants::kGrapplingHookCollisionProbeStep);
    const float distance = Vector2Distance(player.pos, clamped_target);
    const int samples = std::max(1, static_cast<int>(std::ceil(distance / step)));
    for (int i = 1; i <= samples; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(samples);
        const Vector2 sample = Vector2Lerp(player.pos, clamped_target, t);
        const Vector2 collision_sample = Vector2Add(sample, collision_offset);
        bool found = false;

        for (const auto& wall : state_.ice_walls) {
            if (!wall.alive || wall.state == IceWallState::Dying) {
                continue;
            }
            const Rectangle aabb = {static_cast<float>(wall.cell.x * state_.map.cell_size),
                                    static_cast<float>(wall.cell.y * state_.map.cell_size),
                                    static_cast<float>(state_.map.cell_size),
                                    static_cast<float>(state_.map.cell_size)};
            if (CheckCollisionPointRec(collision_sample, aabb)) {
                hook.target_pos = sample;
                hook.latch_point = sample;
                hook.pull_destination =
                    Vector2Subtract(sample, Vector2Scale(direction, player.radius * 0.5f));
                hook.phase = GrapplingHookPhase::Firing;
                hook.latch_target_type = GrapplingHookLatchTargetType::IceWall;
                hook.latch_target_id = wall.id;
                hook.latch_cell = wall.cell;
                hook.latched = true;
                hook.pull_elapsed_seconds = 0.0f;
                found = true;
                break;
            }
        }
        if (found) {
            break;
        }

        for (const auto& object : state_.map_objects) {
            if (!object.alive || !object.stops_projectiles || !object.collision_enabled) {
                continue;
            }
            const Rectangle aabb = GetMapObjectCollisionAabb(object, state_.map.cell_size);
            if (CheckCollisionPointRec(collision_sample, aabb)) {
                hook.target_pos = sample;
                hook.latch_point = sample;
                hook.pull_destination =
                    Vector2Subtract(sample, Vector2Scale(direction, player.radius * 0.5f));
                hook.phase = GrapplingHookPhase::Firing;
                hook.latch_target_type = GrapplingHookLatchTargetType::MapObject;
                hook.latch_target_id = object.id;
                hook.latch_cell = object.cell;
                hook.latched = true;
                hook.pull_elapsed_seconds = 0.0f;
                found = true;
                break;
            }
        }
        if (found) {
            break;
        }
    }

    player.grappling_cooldown_remaining = Constants::kGrapplingHookCooldownSeconds;
    player.grappling_cooldown_total = Constants::kGrapplingHookCooldownSeconds;
    player.rune_placing_mode = false;
    player.inventory_mode = false;
    state_.grappling_hooks.push_back(hook);
    if (play_audio) {
        PlaySfxIfVisible(sfx_grappling_throw_.sound, sfx_grappling_throw_.loaded, player.pos);
    }
    return true;
}

void GameApp::UpdateGrapplingHooks(float dt) {
    for (auto& hook : state_.grappling_hooks) {
        if (!hook.alive) {
            continue;
        }
        if (!network_manager_.IsHost() && hook.owner_player_id != state_.local_player_id) {
            continue;
        }
        hook.animation_time += dt;

        Player* owner = FindPlayerById(hook.owner_player_id);
        if (owner == nullptr || !owner->alive) {
            hook.alive = false;
            continue;
        }

        if (hook.phase == GrapplingHookPhase::Firing) {
            const Vector2 to_target = Vector2Subtract(hook.target_pos, hook.head_pos);
            const float distance = Vector2Length(to_target);
            const float step = Constants::kGrapplingHookFireSpeed * dt;
            if (distance <= step || distance <= 0.0001f) {
                hook.head_pos = hook.target_pos;
                if (hook.latched) {
                    hook.phase = GrapplingHookPhase::Pulling;
                    hook.pull_elapsed_seconds = 0.0f;
                    if (hook.id >= 0) {
                        PlaySfxIfVisible(sfx_grappling_latch_.sound, sfx_grappling_latch_.loaded, hook.latch_point);
                    }
                } else {
                    hook.phase = GrapplingHookPhase::Retracting;
                }
            } else {
                hook.head_pos = Vector2Add(hook.head_pos, Vector2Scale(Vector2Normalize(to_target), step));
            }
            continue;
        }

        if (hook.phase == GrapplingHookPhase::Pulling) {
            hook.pull_elapsed_seconds += dt;
            bool latch_valid = true;
            if (hook.latch_target_type == GrapplingHookLatchTargetType::IceWall) {
                latch_valid = std::any_of(state_.ice_walls.begin(), state_.ice_walls.end(),
                                          [&](const IceWallPiece& wall) {
                                              return wall.id == hook.latch_target_id && wall.alive &&
                                                     wall.state != IceWallState::Dying;
                                          });
            } else if (hook.latch_target_type == GrapplingHookLatchTargetType::MapObject) {
                latch_valid = std::any_of(state_.map_objects.begin(), state_.map_objects.end(),
                                          [&](const MapObjectInstance& object) {
                                              return object.id == hook.latch_target_id && object.alive &&
                                                     object.collision_enabled && object.stops_projectiles;
                                          });
            }
            if (!latch_valid) {
                hook.alive = false;
                continue;
            }

            if (hook.pull_elapsed_seconds >= hook.max_pull_duration_seconds) {
                owner->vel = {0.0f, 0.0f};
                hook.alive = false;
                continue;
            }

            owner->vel = {0.0f, 0.0f};
            owner->rune_placing_mode = false;
            owner->inventory_mode = false;
            owner->melee_active_remaining = 0.0f;

            const Vector2 to_destination = Vector2Subtract(hook.pull_destination, owner->pos);
            const float distance = Vector2Length(to_destination);
            const float step = Constants::kGrapplingHookPullSpeed * dt;
            if (distance <= Constants::kGrapplingHookArrivalEpsilon || distance <= step) {
                owner->pos = hook.pull_destination;
                owner->vel = {0.0f, 0.0f};
                CollisionWorld::ResolvePlayerVsWorld(state_.map, *owner);
                ResolvePlayerVsMapObjects(*owner);
                ResolvePlayerVsIceWalls(*owner);
                hook.alive = false;
            } else {
                owner->pos = Vector2Add(owner->pos, Vector2Scale(Vector2Normalize(to_destination), step));
                CollisionWorld::ResolvePlayerVsWorld(state_.map, *owner, false);
                ResolvePlayerVsMapObjects(*owner);
                ResolvePlayerVsIceWalls(*owner);
            }
            hook.head_pos = hook.latch_point;
            continue;
        }

        if (hook.phase == GrapplingHookPhase::Retracting) {
            const Vector2 retract_target =
                Vector2Add(owner->pos, {0.0f, 0.5f * static_cast<float>(state_.map.cell_size)});
            const Vector2 to_owner = Vector2Subtract(retract_target, hook.head_pos);
            const float distance = Vector2Length(to_owner);
            const float step = Constants::kGrapplingHookFireSpeed * Constants::kGrapplingHookRetractSpeedMultiplier * dt;
            if (distance <= step || distance <= 0.0001f) {
                hook.head_pos = retract_target;
                hook.alive = false;
            } else {
                hook.head_pos = Vector2Add(hook.head_pos, Vector2Scale(Vector2Normalize(to_owner), step));
            }
        }
    }

    state_.grappling_hooks.erase(
        std::remove_if(state_.grappling_hooks.begin(), state_.grappling_hooks.end(),
                       [](const GrapplingHook& hook) { return !hook.alive; }),
        state_.grappling_hooks.end());
}

void GameApp::UpdateCompositeEffects(float dt) {
    for (auto& effect : state_.composite_effects) {
        if (!effect.alive) {
            continue;
        }
        effect.age_seconds += dt;
        if (effect.duration_seconds > 0.0f && effect.age_seconds >= effect.duration_seconds) {
            effect.alive = false;
        }
    }

    state_.composite_effects.erase(
        std::remove_if(state_.composite_effects.begin(), state_.composite_effects.end(),
                       [](const CompositeEffectInstance& effect) { return !effect.alive; }),
        state_.composite_effects.end());
}

float GameApp::GetCompositeEffectDurationSeconds(const std::string& effect_id) const {
    const CompositeEffectDefinition* definition = composite_effects_loader_.FindById(effect_id);
    if (definition == nullptr) {
        return 0.0f;
    }

    const auto get_metadata = [&](CompositeEffectSheet sheet) -> const SpriteMetadataLoader* {
        switch (sheet) {
            case CompositeEffectSheet::Base32:
                return &sprite_metadata_;
            case CompositeEffectSheet::Tall32x64:
                return &sprite_metadata_tall_;
            case CompositeEffectSheet::Large96x96:
                return &sprite_metadata_96x96_;
        }
        return &sprite_metadata_;
    };

    float duration_seconds = 0.0f;
    for (const auto& part : definition->parts) {
        int frame_count = 0;
        float fps = 1.0f;
        const SpriteMetadataLoader* metadata = get_metadata(part.sheet);
        if (metadata != nullptr && metadata->GetAnimationStats(part.animation, "default", frame_count, fps) &&
            frame_count > 0) {
            duration_seconds = std::max(duration_seconds, static_cast<float>(frame_count) / std::max(0.001f, fps));
        }
    }
    return duration_seconds;
}

void GameApp::UpdateFireStormCasts(float dt) {
    for (auto& cast : state_.fire_storm_casts) {
        if (!cast.alive) {
            continue;
        }
        cast.elapsed_seconds += dt;
        if (!fire_storm_cast_impact_played_[cast.id] &&
            cast.elapsed_seconds >= Constants::kFireStormImpactDelaySeconds) {
            const Vector2 impact_center = CellToWorldCenter(cast.center_cell);
            PlaySfxIfVisible(sfx_fire_storm_impact_.sound, sfx_fire_storm_impact_.loaded, impact_center);
            if (IsWorldPointInsideCameraView(impact_center)) {
                camera_shake_time_remaining_ =
                    std::max(camera_shake_time_remaining_, Constants::kCameraShakeDurationSeconds);
            }
            fire_storm_cast_impact_played_[cast.id] = true;
        }
        if (cast.elapsed_seconds < cast.duration_seconds) {
            continue;
        }

        if (network_manager_.IsHost()) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const GridCoord cell = {cast.center_cell.x + dx, cast.center_cell.y + dy};
                    if (!state_.map.IsInside(cell) || !IsTileRunePlaceable(cell) || IsCellOccupiedByFireStormDummy(cell)) {
                        continue;
                    }
                    SpawnFireStormDummyAtCell(cast.owner_player_id, cast.owner_team, cell, Constants::kFireStormLifetimeSeconds);
                }
            }
        }
        cast.alive = false;
    }

    for (auto it = fire_storm_cast_impact_played_.begin(); it != fire_storm_cast_impact_played_.end();) {
        const bool still_alive = std::any_of(state_.fire_storm_casts.begin(), state_.fire_storm_casts.end(),
                                             [&](const FireStormCast& cast) { return cast.id == it->first && cast.alive; });
        if (!still_alive) {
            it = fire_storm_cast_impact_played_.erase(it);
        } else {
            ++it;
        }
    }

    state_.fire_storm_casts.erase(
        std::remove_if(state_.fire_storm_casts.begin(), state_.fire_storm_casts.end(),
                       [](const FireStormCast& cast) { return !cast.alive; }),
        state_.fire_storm_casts.end());
}

void GameApp::UpdateFireStormDummies(float dt) {
    std::uniform_real_distribution<float> chance_dist(0.0f, 1.0f);
    std::uniform_int_distribution<int> frame_dist(Constants::kFireStormDummyLightningMinFrames,
                                                  Constants::kFireStormDummyLightningMaxFrames);
    int lightning_frame_count = 0;
    float lightning_fps = 8.0f;
    if (sprite_metadata_.GetAnimationStats("fire_storm_top_lightning", "default", lightning_frame_count, lightning_fps) &&
        lightning_frame_count > 0 && lightning_fps > 0.001f) {
        // use animation fps directly
    } else {
        lightning_fps = 8.0f;
    }
    const float frame_seconds = 1.0f / std::max(0.001f, lightning_fps);

    std::unordered_set<int> live_ids;
    for (auto& dummy : state_.fire_storm_dummies) {
        if (!dummy.alive) {
            continue;
        }
        live_ids.insert(dummy.id);
        dummy.state_time += dt;
        if (dummy.state == FireStormDummyState::Born && dummy.state_time >= dummy.state_duration) {
            dummy.state = FireStormDummyState::Idle;
            dummy.state_time = 0.0f;
            dummy.state_duration = 0.0f;
        } else if (dummy.state == FireStormDummyState::Idle && dummy.idle_lifetime_remaining_seconds > 0.0f) {
            dummy.idle_lifetime_remaining_seconds = std::max(0.0f, dummy.idle_lifetime_remaining_seconds - dt);
            if (dummy.idle_lifetime_remaining_seconds <= 0.0f) {
                dummy.state = FireStormDummyState::Dying;
                dummy.state_time = 0.0f;
                dummy.state_duration = GetCompositeEffectDurationSeconds("fire_storm_death");
            }
        } else if (dummy.state == FireStormDummyState::Dying && dummy.state_time >= dummy.state_duration) {
            dummy.alive = false;
        }

        float& lightning_seconds = fire_storm_dummy_lightning_seconds_remaining_[dummy.id];
        float& lightning_cooldown_seconds = fire_storm_dummy_lightning_cooldown_seconds_remaining_[dummy.id];
        if (dummy.state != FireStormDummyState::Idle || !dummy.alive) {
            lightning_seconds = 0.0f;
            lightning_cooldown_seconds = 0.0f;
            continue;
        }
        if (lightning_seconds > 0.0f) {
            lightning_seconds = std::max(0.0f, lightning_seconds - dt);
            if (lightning_seconds <= 0.0f) {
                lightning_cooldown_seconds =
                    static_cast<float>(Constants::kFireStormDummyLightningCooldownFrames) * frame_seconds;
            }
        } else if (lightning_cooldown_seconds > 0.0f) {
            lightning_cooldown_seconds = std::max(0.0f, lightning_cooldown_seconds - dt);
        } else if (chance_dist(visual_rng_) < Constants::kFireStormDummyLightningChancePerFrame) {
            lightning_seconds = static_cast<float>(frame_dist(visual_rng_)) * frame_seconds;
        }
    }

    state_.fire_storm_dummies.erase(
        std::remove_if(state_.fire_storm_dummies.begin(), state_.fire_storm_dummies.end(),
                       [](const FireStormDummy& dummy) { return !dummy.alive; }),
        state_.fire_storm_dummies.end());

    for (auto it = fire_storm_dummy_lightning_seconds_remaining_.begin();
         it != fire_storm_dummy_lightning_seconds_remaining_.end();) {
        if (live_ids.find(it->first) == live_ids.end()) {
            it = fire_storm_dummy_lightning_seconds_remaining_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = fire_storm_dummy_lightning_cooldown_seconds_remaining_.begin();
         it != fire_storm_dummy_lightning_cooldown_seconds_remaining_.end();) {
        if (live_ids.find(it->first) == live_ids.end()) {
            it = fire_storm_dummy_lightning_cooldown_seconds_remaining_.erase(it);
        } else {
            ++it;
        }
    }
}

void GameApp::UpdateProjectileEmitters() {
    for (auto& projectile : state_.projectiles) {
        smoke_emitter_.UpdateAndEmit(projectile, state_.particles);
    }
}

void GameApp::UpdateParticles(float dt) {
    for (auto& particle : state_.particles) {
        if (!particle.alive) {
            continue;
        }

        particle.vel = Vector2Add(particle.vel, Vector2Scale(particle.acc, dt));
        const float drag_factor = std::max(0.0f, 1.0f - particle.drag * dt);
        particle.vel = Vector2Scale(particle.vel, drag_factor);
        particle.pos = Vector2Add(particle.pos, Vector2Scale(particle.vel, dt));
        particle.age_seconds += dt;

        int frame_count = 0;
        float fps = 1.0f;
        float lifetime_seconds = 0.33f * static_cast<float>(std::max(1, particle.max_cycles));
        if (sprite_metadata_.GetAnimationStats(particle.animation_key, particle.facing, frame_count, fps)) {
            const float cycle_seconds = static_cast<float>(frame_count) / std::max(0.001f, fps);
            lifetime_seconds = cycle_seconds * static_cast<float>(std::max(1, particle.max_cycles));
        }
        if (particle.age_seconds >= lifetime_seconds) {
            particle.alive = false;
        }
    }

    state_.particles.erase(std::remove_if(state_.particles.begin(), state_.particles.end(),
                                          [](const Particle& particle) { return !particle.alive; }),
                           state_.particles.end());
}

void GameApp::ResolvePlayerCollisions() {
    for (size_t i = 0; i < state_.players.size(); ++i) {
        for (size_t j = i + 1; j < state_.players.size(); ++j) {
            Player& a = state_.players[i];
            Player& b = state_.players[j];
            if (!a.alive || !b.alive) {
                continue;
            }

            Vector2 normal = {0.0f, 0.0f};
            float penetration = 0.0f;
            if (!CollisionWorld::CircleVsCircle(a.pos, a.radius, b.pos, b.radius, normal, penetration)) {
                continue;
            }

            const Vector2 separation = Vector2Scale(normal, penetration * 0.5f);
            a.pos = Vector2Subtract(a.pos, separation);
            b.pos = Vector2Add(b.pos, separation);

            const float a_dot = Vector2DotProduct(a.vel, normal);
            if (a_dot > 0.0f) {
                a.vel = Vector2Subtract(a.vel, Vector2Scale(normal, a_dot));
            }

            const float b_dot = Vector2DotProduct(b.vel, normal);
            if (b_dot < 0.0f) {
                b.vel = Vector2Subtract(b.vel, Vector2Scale(normal, b_dot));
            }
        }
    }

    for (auto& player : state_.players) {
        ResolvePlayerVsMapObjects(player);
        ResolvePlayerVsIceWalls(player);
    }
}

void GameApp::HandleMeleeHit(Player& attacker) {
    const float attack_elapsed = Constants::kMeleeActiveWindowSeconds - attacker.melee_active_remaining;
    if (attack_elapsed < Constants::kMeleeHitStartSeconds || attack_elapsed > Constants::kMeleeHitEndSeconds) {
        return;
    }

    const Vector2 hit_center = Vector2Add(attacker.pos, Vector2Scale(attacker.aim_dir, Constants::kMeleeRange));

    for (auto& target : state_.players) {
        if (!target.alive || target.team == attacker.team || target.id == attacker.id) {
            continue;
        }
        if (std::find(attacker.melee_hit_target_ids.begin(), attacker.melee_hit_target_ids.end(), target.id) !=
            attacker.melee_hit_target_ids.end()) {
            continue;
        }

        Vector2 normal = {0.0f, 0.0f};
        float penetration = 0.0f;
        if (!CollisionWorld::CircleVsCircle(hit_center, Constants::kMeleeHitRadius, target.pos, target.radius, normal,
                                            penetration)) {
            continue;
        }

        ApplyDamageToPlayer(target, attacker.id, Constants::kMeleeDamage, "melee", true);
        attacker.melee_hit_target_ids.push_back(target.id);
    }

    for (auto& object : state_.map_objects) {
        if (!object.alive || object.hp <= 0) {
            continue;
        }
        if (std::find(attacker.melee_hit_object_ids.begin(), attacker.melee_hit_object_ids.end(), object.id) !=
            attacker.melee_hit_object_ids.end()) {
            continue;
        }

        const Rectangle aabb = GetMapObjectCollisionAabb(object, state_.map.cell_size);
        Vector2 normal = {0.0f, 0.0f};
        float penetration = 0.0f;
        if (!CollisionWorld::CircleVsAabb(hit_center, Constants::kMeleeHitRadius, aabb, normal, penetration)) {
            continue;
        }

        if (ApplyObjectDamage(object.id, Constants::kMeleeDamage, attacker.id, "melee")) {
            attacker.melee_hit_object_ids.push_back(object.id);
        }
    }

    for (auto& dummy : state_.fire_storm_dummies) {
        if (!dummy.alive || dummy.state == FireStormDummyState::Dying) {
            continue;
        }
        const Rectangle aabb = {static_cast<float>(dummy.cell.x * state_.map.cell_size),
                                static_cast<float>(dummy.cell.y * state_.map.cell_size),
                                static_cast<float>(state_.map.cell_size), static_cast<float>(state_.map.cell_size)};
        Vector2 normal = {0.0f, 0.0f};
        float penetration = 0.0f;
        if (!CollisionWorld::CircleVsAabb(hit_center, Constants::kMeleeHitRadius, aabb, normal, penetration)) {
            continue;
        }

        dummy.state = FireStormDummyState::Dying;
        dummy.state_time = 0.0f;
        dummy.state_duration = GetCompositeEffectDurationSeconds("fire_storm_death");
        fire_storm_dummy_lightning_seconds_remaining_[dummy.id] = 0.0f;
    }
}

void GameApp::HandleEventsHost() {
    size_t index = 0;
    while (index < event_queue_.GetEvents().size()) {
        const Event event = event_queue_.GetEvents()[index];
        if (std::holds_alternative<RunePlacedEvent>(event)) {
            CheckSpellPatterns(std::get<RunePlacedEvent>(event));
        } else if (std::holds_alternative<MatchEndedEvent>(event)) {
            winning_team_ = std::get<MatchEndedEvent>(event).winning_team;
        }
        ++index;
    }

    event_queue_.Clear();

    state_.runes.erase(std::remove_if(state_.runes.begin(), state_.runes.end(),
                                      [](const Rune& rune) {
                                          return !rune.active && rune.activation_remaining_seconds <= 0.0f &&
                                                 rune.activation_total_seconds <= 0.0f;
                                      }),
                       state_.runes.end());
    RebuildInfluenceZones();
}

bool GameApp::TryPlaceRune(Player& player, Vector2 world_mouse) {
    if (player.rune_place_cooldown_remaining > 0.0f) {
        return false;
    }
    if (player.selected_rune_type == RuneType::None) {
        return false;
    }

    const GridCoord cell = WorldToCell(world_mouse);
    const Vector2 cell_center = CellToWorldCenter(cell);
    if (Vector2Distance(player.pos, cell_center) > Constants::kRuneCastRangeWorld) {
        return false;
    }
    if (!IsTileRunePlaceable(cell) || IsCellOccupiedByRune(cell) || IsCellOccupiedByFireStormDummy(cell)) {
        return false;
    }
    if (player.selected_rune_type == RuneType::FireStormDummy) {
        return TryPlaceFireStormDummy(player, cell);
    }

    bool volatile_cast = false;
    for (const auto& influence : state_.influence_zones) {
        if (influence.cell == cell && influence.team != player.team) {
            volatile_cast = true;
            break;
        }
    }

    float mana_cost = GetRuneManaCost(player.selected_rune_type);
    float activation_seconds = GetRuneActivationSeconds(player.selected_rune_type);
    if (volatile_cast) {
        mana_cost *= Constants::kRuneVolatileManaMultiplier;
        activation_seconds *= Constants::kRuneVolatileActivationMultiplier;
    }
    if (player.mana < mana_cost) {
        return false;
    }
    if (GetPlayerRuneCooldownRemaining(player, player.selected_rune_type) > 0.0f) {
        return false;
    }

    Rune rune;
    rune.id = state_.next_entity_id++;
    rune.owner_player_id = player.id;
    rune.owner_team = player.team;
    rune.cell = cell;
    rune.rune_type = player.selected_rune_type;
    rune.placement_order = state_.next_rune_placement_order++;
    rune.active = false;
    rune.volatile_cast = volatile_cast;
    rune.activation_total_seconds = activation_seconds;
    rune.activation_remaining_seconds = activation_seconds;
    rune.creates_influence_zone = !volatile_cast;
    state_.runes.push_back(rune);

    Particle rune_cast_vfx;
    rune_cast_vfx.pos = cell_center;
    rune_cast_vfx.vel = {0.0f, 0.0f};
    rune_cast_vfx.acc = {0.0f, 0.0f};
    rune_cast_vfx.drag = 0.0f;
    rune_cast_vfx.animation_key = volatile_cast ? "rune_cast_volatile_effect" : "rune_cast_effect";
    rune_cast_vfx.facing = "default";
    rune_cast_vfx.age_seconds = 0.0f;
    rune_cast_vfx.max_cycles = 1;
    rune_cast_vfx.alive = true;
    state_.particles.push_back(rune_cast_vfx);

    SpawnLightningEffect(player.pos, cell_center, 0.3f, volatile_cast);
    PlaySfxIfVisible(sfx_create_rune_.sound, sfx_create_rune_.loaded, cell_center);

    player.mana = std::max(0.0f, player.mana - mana_cost);
    const float selected_cooldown = GetRuneCooldownSeconds(player.selected_rune_type);
    for (RuneType rune_type : {RuneType::Fire, RuneType::Water, RuneType::Catalyst, RuneType::Earth,
                               RuneType::FireStormDummy}) {
        if (rune_type == player.selected_rune_type) {
            SetPlayerRuneCooldown(player, rune_type, selected_cooldown, selected_cooldown);
            continue;
        }

        const float clamped_remaining =
            std::max(GetPlayerRuneCooldownRemaining(player, rune_type), Constants::kRuneMinCrossCooldownSeconds);
        const float clamped_total =
            std::max(GetPlayerRuneCooldownTotal(player, rune_type), Constants::kRuneMinCrossCooldownSeconds);
        SetPlayerRuneCooldown(player, rune_type, clamped_remaining, clamped_total);
    }
    player.rune_place_cooldown_duration = selected_cooldown;
    player.rune_place_cooldown_remaining = selected_cooldown;
    player.rune_placing_mode = false;
    return true;
}

bool GameApp::TryPlaceFireStormDummy(Player& player, const GridCoord& cell) {
    SpawnFireStormDummyAtCell(player.id, player.team, cell, -1.0f);

    const float selected_cooldown = GetRuneCooldownSeconds(player.selected_rune_type);
    for (RuneType rune_type : {RuneType::Fire, RuneType::Water, RuneType::Catalyst, RuneType::Earth,
                               RuneType::FireStormDummy}) {
        if (rune_type == player.selected_rune_type) {
            SetPlayerRuneCooldown(player, rune_type, selected_cooldown, selected_cooldown);
            continue;
        }

        const float clamped_remaining =
            std::max(GetPlayerRuneCooldownRemaining(player, rune_type), Constants::kRuneMinCrossCooldownSeconds);
        const float clamped_total =
            std::max(GetPlayerRuneCooldownTotal(player, rune_type), Constants::kRuneMinCrossCooldownSeconds);
        SetPlayerRuneCooldown(player, rune_type, clamped_remaining, clamped_total);
    }
    player.rune_place_cooldown_duration = selected_cooldown;
    player.rune_place_cooldown_remaining = selected_cooldown;
    player.rune_placing_mode = false;
    return true;
}

void GameApp::SpawnFireStormDummyAtCell(int owner_player_id, int owner_team, const GridCoord& cell,
                                        float idle_lifetime_seconds) {
    FireStormDummy dummy;
    dummy.id = state_.next_entity_id++;
    dummy.owner_player_id = owner_player_id;
    dummy.owner_team = owner_team;
    dummy.cell = cell;
    dummy.state = FireStormDummyState::Born;
    dummy.state_time = 0.0f;
    dummy.state_duration = GetCompositeEffectDurationSeconds("fire_storm_born");
    dummy.idle_lifetime_remaining_seconds = idle_lifetime_seconds;
    dummy.alive = true;
    state_.fire_storm_dummies.push_back(dummy);
    fire_storm_dummy_lightning_seconds_remaining_[dummy.id] = 0.0f;
    fire_storm_dummy_lightning_cooldown_seconds_remaining_[dummy.id] = 0.0f;
}

void GameApp::CheckSpellPatterns(const RunePlacedEvent& event) {
    struct PendingSpellMatch {
        std::string spell_name;
        SpellDirection direction = SpellDirection::Right;
        GridCoord cast_origin;
        std::vector<GridCoord> matched_cells;
    };

    const auto& patterns = spell_patterns_.GetPatterns();
    std::vector<PendingSpellMatch> pending_matches;
    for (const auto& pattern : patterns) {
        for (const auto& directional : pattern.directional_patterns) {
            for (const auto& variant : directional.variants) {
                const int rows = static_cast<int>(variant.size());
                if (rows <= 0) {
                    continue;
                }
                const int cols = static_cast<int>(variant[0].size());
                if (cols <= 0) {
                    continue;
                }

                std::vector<GridCoord> anchor_cells;
                for (int y = 0; y < rows; ++y) {
                    for (int x = 0; x < cols; ++x) {
                        const auto info = spell_patterns_.GetSymbolInfo(variant[y][x]);
                        if (!info.has_value()) {
                            continue;
                        }
                        const PlacementConstraint constraint =
                            pattern.order_relevant ? info->placement_constraint : PlacementConstraint::Any;
                        if (constraint == PlacementConstraint::Latest &&
                            info->rune_type == event.rune_type) {
                            anchor_cells.push_back({x, y});
                        }
                    }
                }
                if (anchor_cells.empty()) {
                    for (int y = 0; y < rows; ++y) {
                        for (int x = 0; x < cols; ++x) {
                            anchor_cells.push_back({x, y});
                        }
                    }
                }

                for (const GridCoord& anchor : anchor_cells) {
                    const GridCoord top_left = {event.cell.x - anchor.x, event.cell.y - anchor.y};

                    std::vector<GridCoord> matched_cells;
                    bool is_match = true;
                    for (int y = 0; y < rows && is_match; ++y) {
                        for (int x = 0; x < cols && is_match; ++x) {
                            const std::string& symbol = variant[y][x];
                            if (symbol.empty()) {
                                continue;
                            }
                            const auto info = spell_patterns_.GetSymbolInfo(symbol);
                            if (!info.has_value()) {
                                is_match = false;
                                break;
                            }

                            const GridCoord cell = {top_left.x + x, top_left.y + y};
                            if (!state_.map.IsInside(cell)) {
                                is_match = false;
                                break;
                            }

                            const auto rune_it = std::find_if(state_.runes.begin(), state_.runes.end(),
                                                              [&](const Rune& rune) {
                                                                  if (!rune.active || !(rune.cell == cell) ||
                                                                      rune.rune_type != info->rune_type) {
                                                                      return false;
                                                                  }

                                                                  const PlacementConstraint constraint =
                                                                      pattern.order_relevant
                                                                          ? info->placement_constraint
                                                                          : PlacementConstraint::Any;
                                                                  if (constraint == PlacementConstraint::Latest) {
                                                                      return rune.placement_order ==
                                                                             event.placement_order;
                                                                  }
                                                                  if (constraint == PlacementConstraint::Old) {
                                                                      return rune.placement_order <
                                                                             event.placement_order;
                                                                  }
                                                                  return true;
                                                              });

                            if (rune_it == state_.runes.end()) {
                                is_match = false;
                                break;
                            }

                            matched_cells.push_back(cell);
                        }
                    }

                    if (!is_match) {
                        continue;
                    }

                    std::sort(matched_cells.begin(), matched_cells.end(),
                              [](const GridCoord& a, const GridCoord& b) {
                                  return (a.y == b.y) ? (a.x < b.x) : (a.y < b.y);
                              });

                    GridCoord cast_origin = event.cell;
                    if (pattern.spell_name == "fire_storm") {
                        for (int y = 0; y < rows; ++y) {
                            for (int x = 0; x < cols; ++x) {
                                const auto info = spell_patterns_.GetSymbolInfo(variant[y][x]);
                                if (info.has_value() && info->rune_type == RuneType::Catalyst) {
                                    cast_origin = {top_left.x + x, top_left.y + y};
                                    break;
                                }
                            }
                        }
                    }
                    if (pattern.spell_name == "ice_wall" && !matched_cells.empty()) {
                        std::vector<GridCoord> ordered = matched_cells;
                        if (IsHorizontalDirection(directional.direction)) {
                            std::sort(ordered.begin(), ordered.end(),
                                      [](const GridCoord& a, const GridCoord& b) {
                                          return (a.x == b.x) ? (a.y < b.y) : (a.x < b.x);
                                      });
                        } else {
                            std::sort(ordered.begin(), ordered.end(),
                                      [](const GridCoord& a, const GridCoord& b) {
                                          return (a.y == b.y) ? (a.x < b.x) : (a.y < b.y);
                                      });
                        }
                        cast_origin = ordered[ordered.size() / 2];
                    }

                    const bool duplicate = std::any_of(
                        pending_matches.begin(), pending_matches.end(),
                        [&](const PendingSpellMatch& match) {
                            return match.spell_name == pattern.spell_name && match.direction == directional.direction &&
                                   match.cast_origin == cast_origin && match.matched_cells == matched_cells;
                        });
                    if (!duplicate) {
                        pending_matches.push_back(
                            PendingSpellMatch{pattern.spell_name, directional.direction, cast_origin, matched_cells});
                    }
                }
            }
        }
    }

    if (pending_matches.empty()) {
        return;
    }

    std::vector<GridCoord> consumed_cells;
    for (const auto& match : pending_matches) {
        event_queue_.Push(RunePatternCompletedEvent{match.spell_name, match.direction, match.cast_origin, match.matched_cells});

        if (match.spell_name == "fire_bolt") {
            FireBoltSpell spell(CellToWorldCenter(match.cast_origin), event.player_id, match.direction);
            spell.Cast(state_, event_queue_);
            PlaySfxIfVisible(sfx_fireball_created_.sound, sfx_fireball_created_.loaded, CellToWorldCenter(match.cast_origin));
        } else if (match.spell_name == "ice_wall") {
            IceWallSpell spell(match.cast_origin, event.player_id, match.direction);
            spell.Cast(state_, event_queue_);
            PlaySfxIfVisible(sfx_ice_wall_freeze_.sound, sfx_ice_wall_freeze_.loaded, CellToWorldCenter(match.cast_origin));
        } else if (match.spell_name == "fire_storm") {
            FireStormCast cast;
            cast.id = state_.next_entity_id++;
            cast.owner_player_id = event.player_id;
            cast.owner_team = event.team;
            cast.center_cell = match.cast_origin;
            cast.elapsed_seconds = 0.0f;
            cast.duration_seconds = GetCompositeEffectDurationSeconds("fire_storm_cast");
            if (cast.duration_seconds <= 0.0f) {
                cast.duration_seconds = 1.0f;
            }
            cast.alive = true;
            state_.fire_storm_casts.push_back(cast);
            PlaySfxIfVisible(sfx_fire_storm_cast_.sound, sfx_fire_storm_cast_.loaded, CellToWorldCenter(match.cast_origin));
        }

        for (const auto& matched_cell : match.matched_cells) {
            const bool already_consumed =
                std::any_of(consumed_cells.begin(), consumed_cells.end(), [&](const GridCoord& cell) { return cell == matched_cell; });
            if (!already_consumed) {
                consumed_cells.push_back(matched_cell);
            }
        }
    }

    for (auto& rune : state_.runes) {
        const bool should_consume =
            rune.active && std::any_of(consumed_cells.begin(), consumed_cells.end(),
                                       [&](const GridCoord& cell) { return rune.cell == cell; });
        if (!should_consume) {
            continue;
        }
        rune.active = false;
        rune.activation_total_seconds = 0.0f;
        rune.activation_remaining_seconds = 0.0f;
    }
}

bool GameApp::IsTileRunePlaceable(const GridCoord& cell) const {
    if (!state_.map.IsInside(cell)) {
        return false;
    }

    const TileType tile = state_.map.GetTile(cell);
    if (!(tile == TileType::Grass || tile == TileType::SpawnPoint)) {
        return false;
    }

    for (const auto& object : state_.map_objects) {
        if (!object.alive || object.cell.x != cell.x || object.cell.y != cell.y) {
            continue;
        }
        if (!object.walkable) {
            return false;
        }
    }
    return true;
}

bool GameApp::IsCellOccupiedByRune(const GridCoord& cell) const {
    return std::any_of(state_.runes.begin(), state_.runes.end(), [&](const Rune& rune) { return rune.cell == cell; });
}

bool GameApp::IsCellOccupiedByFireStormDummy(const GridCoord& cell) const {
    return std::any_of(state_.fire_storm_dummies.begin(), state_.fire_storm_dummies.end(),
                       [&](const FireStormDummy& dummy) { return dummy.alive && dummy.cell == cell; });
}

GridCoord GameApp::WorldToCell(Vector2 world) const {
    return {static_cast<int>(std::floor(world.x / state_.map.cell_size)),
            static_cast<int>(std::floor(world.y / state_.map.cell_size))};
}

Vector2 GameApp::CellToWorldCenter(const GridCoord& cell) const { return state_.map.CellCenterWorld(cell); }

Player* GameApp::FindPlayerById(int id) {
    auto it = std::find_if(state_.players.begin(), state_.players.end(),
                           [&](const Player& player) { return player.id == id; });
    return it == state_.players.end() ? nullptr : &(*it);
}

const Player* GameApp::FindPlayerById(int id) const {
    auto it = std::find_if(state_.players.begin(), state_.players.end(),
                           [&](const Player& player) { return player.id == id; });
    return it == state_.players.end() ? nullptr : &(*it);
}

void GameApp::RenderWorld() {
    UpdateCameraTarget();

    BeginMode2D(camera_);
    RenderMap();
    RenderNonTerrainDepthSorted();
    RenderMeleeAttacks();
    RenderDamagePopups();
    RenderRunePlacementOverlay();
    EndMode2D();
}

void GameApp::RenderNonTerrainDepthSorted() {
    enum class RenderKind {
        LegacyDecoration,
        MapObject,
        CompositeEffectPart,
        FireStormDummyPart,
        EarthRootsPart,
        StatusEffectPart,
        Rune,
        IceWall,
        LightningSegment,
        GrapplingHookSegment,
        Projectile,
        Particle,
        Player,
    };

    struct RenderItem {
        RenderKind kind = RenderKind::Player;
        float sort_y = 0.0f;
        size_t index = 0;
        size_t aux_index = 0;
        size_t sub_index = 0;
    };

    const auto kind_priority = [](RenderKind kind) {
        switch (kind) {
            case RenderKind::LegacyDecoration:
                return 0;
            case RenderKind::MapObject:
                return 1;
            case RenderKind::CompositeEffectPart:
                return 2;
            case RenderKind::FireStormDummyPart:
                return 3;
            case RenderKind::EarthRootsPart:
                return 4;
            case RenderKind::StatusEffectPart:
                return 5;
            case RenderKind::Rune:
                return 6;
            case RenderKind::IceWall:
                return 7;
            case RenderKind::LightningSegment:
                return 8;
            case RenderKind::GrapplingHookSegment:
                return 9;
            case RenderKind::Projectile:
                return 10;
            case RenderKind::Particle:
                return 11;
            case RenderKind::Player:
            default:
                return 12;
        }
    };

    std::vector<RenderItem> items;
    size_t lightning_segment_total = 0;
    for (const auto& effect : state_.lightning_effects) {
        if (effect.alive) {
            lightning_segment_total += static_cast<size_t>(std::max(1, effect.segment_count));
        }
    }
    size_t grappling_segment_total = 0;
    for (const auto& hook : state_.grappling_hooks) {
        if (!hook.alive) {
            continue;
        }
        const Player* owner = FindPlayerById(hook.owner_player_id);
        if (owner == nullptr || !owner->alive) {
            continue;
        }
        const Vector2 start = Vector2Add(GetRenderPlayerPosition(owner->id),
                                         {0.0f, 0.5f * static_cast<float>(state_.map.cell_size)});
        const Vector2 end =
            (hook.phase == GrapplingHookPhase::Pulling)
                ? hook.latch_point
                : ((!network_manager_.IsHost() && hook.owner_player_id == state_.local_player_id)
                       ? hook.head_pos
                       : GetRenderGrapplingHookHeadPosition(hook.id, hook.head_pos));
        const float distance = Vector2Distance(start, end);
        grappling_segment_total += static_cast<size_t>(std::max(2, static_cast<int>(std::floor(
            distance / std::max(1.0f, static_cast<float>(state_.map.cell_size))))));
    }
    items.reserve(state_.players.size() + state_.runes.size() + state_.projectiles.size() + state_.particles.size() +
                  state_.ice_walls.size() + state_.map.decorations.size() + state_.map_objects.size() +
                  lightning_segment_total + grappling_segment_total + state_.earth_roots_groups.size() * 17);

    for (size_t i = 0; i < state_.map.decorations.size(); ++i) {
        if (state_.map.decorations[i].empty()) {
            continue;
        }
        const int cell_x = static_cast<int>(i % static_cast<size_t>(std::max(1, state_.map.width)));
        const int cell_y = static_cast<int>(i / static_cast<size_t>(std::max(1, state_.map.width)));
        items.push_back({RenderKind::LegacyDecoration,
                         (static_cast<float>(cell_y) + 1.0f) * static_cast<float>(state_.map.cell_size), i});
    }

    for (size_t i = 0; i < state_.map_objects.size(); ++i) {
        const MapObjectInstance& object = state_.map_objects[i];
        if (!object.alive) {
            continue;
        }
        items.push_back({RenderKind::MapObject, (static_cast<float>(object.cell.y) + 1.0f) * static_cast<float>(state_.map.cell_size),
                         i});
    }

    for (size_t i = 0; i < state_.composite_effects.size(); ++i) {
        const CompositeEffectInstance& effect = state_.composite_effects[i];
        if (!effect.alive) {
            continue;
        }
        const CompositeEffectDefinition* definition = composite_effects_loader_.FindById(effect.effect_id);
        if (definition == nullptr) {
            continue;
        }
        for (size_t j = 0; j < definition->parts.size(); ++j) {
            const auto& part = definition->parts[j];
            if (part.layer != CompositeEffectLayer::YSorted) {
                continue;
            }
            const float cell = static_cast<float>(state_.map.cell_size);
            const float sort_y = effect.origin_world.y + 0.5f * cell + part.sort_anchor_y_tiles * cell;
            items.push_back({RenderKind::CompositeEffectPart, sort_y, i, 0, j});
        }
    }

    for (size_t i = 0; i < state_.fire_storm_dummies.size(); ++i) {
        const FireStormDummy& dummy = state_.fire_storm_dummies[i];
        if (!dummy.alive) {
            continue;
        }
        const char* effect_id = dummy.state == FireStormDummyState::Born
                                    ? "fire_storm_born"
                                    : (dummy.state == FireStormDummyState::Dying ? "fire_storm_death" : "fire_storm_idle");
        const CompositeEffectDefinition* definition = composite_effects_loader_.FindById(effect_id);
        if (definition == nullptr) {
            continue;
        }
        const Vector2 origin = CellToWorldCenter(dummy.cell);
        for (size_t j = 0; j < definition->parts.size(); ++j) {
            const auto& part = definition->parts[j];
            if (part.layer != CompositeEffectLayer::YSorted) {
                continue;
            }
            const float cell = static_cast<float>(state_.map.cell_size);
            const float sort_y = origin.y + 0.5f * cell + part.sort_anchor_y_tiles * cell;
            items.push_back({RenderKind::FireStormDummyPart, sort_y, i, 0, j});
        }

        if (dummy.state == FireStormDummyState::Idle) {
            auto lightning_it = fire_storm_dummy_lightning_seconds_remaining_.find(dummy.id);
            if (lightning_it != fire_storm_dummy_lightning_seconds_remaining_.end() && lightning_it->second > 0.0f) {
                const CompositeEffectDefinition* lightning_definition = composite_effects_loader_.FindById("fire_storm_lightning");
                if (lightning_definition != nullptr) {
                    for (size_t j = 0; j < lightning_definition->parts.size(); ++j) {
                        const auto& part = lightning_definition->parts[j];
                        if (part.layer != CompositeEffectLayer::YSorted) {
                            continue;
                        }
                        const float cell = static_cast<float>(state_.map.cell_size);
                        const float sort_y = origin.y + 0.5f * cell + part.sort_anchor_y_tiles * cell;
                        items.push_back({RenderKind::FireStormDummyPart, sort_y, i, 1, j});
                    }
                }
            }
        }
    }

    for (size_t i = 0; i < state_.earth_roots_groups.size(); ++i) {
        const EarthRootsGroup& group = state_.earth_roots_groups[i];
        if (!group.alive) {
            continue;
        }
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const size_t tile_index = static_cast<size_t>((dy + 1) * 3 + (dx + 1));
                const GridCoord cell = {group.center_cell.x + dx, group.center_cell.y + dy};
                const Vector2 center = CellToWorldCenter(cell);
                items.push_back({RenderKind::EarthRootsPart, center.y, i, tile_index, 0});
                if (!(dx == 0 && dy == 0)) {
                    items.push_back({RenderKind::EarthRootsPart, center.y + 8.0f, i, tile_index, 1});
                }
            }
        }
    }

    for (size_t i = 0; i < state_.players.size(); ++i) {
        const Player& player = state_.players[i];
        if (!player.alive) {
            continue;
        }
        const Vector2 origin = GetRenderPlayerPosition(player.id);
        for (size_t j = 0; j < player.status_effects.size(); ++j) {
            const StatusEffectInstance& status = player.status_effects[j];
            if (status.composite_effect_id.empty()) {
                continue;
            }
            const CompositeEffectDefinition* definition = composite_effects_loader_.FindById(status.composite_effect_id);
            if (definition == nullptr) {
                continue;
            }
            for (size_t k = 0; k < definition->parts.size(); ++k) {
                const auto& part = definition->parts[k];
                if (part.layer != CompositeEffectLayer::YSorted) {
                    continue;
                }
                const float cell = static_cast<float>(state_.map.cell_size);
                const float sort_y = origin.y + 0.5f * cell + part.sort_anchor_y_tiles * cell;
                items.push_back({RenderKind::StatusEffectPart, sort_y, i, j, k});
            }
        }
    }

    for (size_t i = 0; i < state_.runes.size(); ++i) {
        if (!state_.runes[i].active && state_.runes[i].activation_remaining_seconds <= 0.0f) {
            continue;
        }
        const Vector2 center = CellToWorldCenter(state_.runes[i].cell);
        items.push_back({RenderKind::Rune, center.y, i});
    }

    for (size_t i = 0; i < state_.ice_walls.size(); ++i) {
        if (!state_.ice_walls[i].alive) {
            continue;
        }
        const float base_y = (static_cast<float>(state_.ice_walls[i].cell.y) + 1.0f) * static_cast<float>(state_.map.cell_size);
        items.push_back({RenderKind::IceWall, base_y, i});
    }

    for (size_t i = 0; i < state_.lightning_effects.size(); ++i) {
        const LightningEffect& effect = state_.lightning_effects[i];
        if (!effect.alive) {
            continue;
        }
        const Vector2 delta = Vector2Subtract(effect.end, effect.start);
        const int segments = std::max(1, effect.segment_count);
        for (int seg = 0; seg < segments; ++seg) {
            const float t0 = static_cast<float>(seg) / static_cast<float>(segments);
            const float t1 = static_cast<float>(seg + 1) / static_cast<float>(segments);
            const Vector2 p0 = Vector2Add(effect.start, Vector2Scale(delta, t0));
            const Vector2 p1 = Vector2Add(effect.start, Vector2Scale(delta, t1));
            const float sort_y = (p0.y + p1.y) * 0.5f;
            items.push_back({RenderKind::LightningSegment, sort_y, i, 0, static_cast<size_t>(seg)});
        }
    }

    for (size_t i = 0; i < state_.grappling_hooks.size(); ++i) {
        const GrapplingHook& hook = state_.grappling_hooks[i];
        if (!hook.alive) {
            continue;
        }
        const Player* owner = FindPlayerById(hook.owner_player_id);
        if (owner == nullptr || !owner->alive) {
            continue;
        }
        const Vector2 start = GetRenderPlayerPosition(owner->id);
        const Vector2 end =
            (hook.phase == GrapplingHookPhase::Pulling)
                ? hook.latch_point
                : ((!network_manager_.IsHost() && hook.owner_player_id == state_.local_player_id)
                       ? hook.head_pos
                       : GetRenderGrapplingHookHeadPosition(hook.id, hook.head_pos));
        const float distance = Vector2Distance(start, end);
        const int segments = std::max(
            2, static_cast<int>(std::floor(distance / std::max(1.0f, static_cast<float>(state_.map.cell_size)))));
        for (int seg = 0; seg < segments; ++seg) {
            const float t = (segments <= 1) ? 0.0f : (static_cast<float>(seg) / static_cast<float>(segments - 1));
            const Vector2 p = Vector2Lerp(start, end, t);
            items.push_back({RenderKind::GrapplingHookSegment, p.y, i, static_cast<size_t>(segments),
                             static_cast<size_t>(seg)});
        }
    }

    for (size_t i = 0; i < state_.projectiles.size(); ++i) {
        if (!state_.projectiles[i].alive) {
            continue;
        }
        items.push_back({RenderKind::Projectile, state_.projectiles[i].pos.y, i});
    }

    for (size_t i = 0; i < state_.particles.size(); ++i) {
        if (!state_.particles[i].alive) {
            continue;
        }
        items.push_back({RenderKind::Particle, state_.particles[i].pos.y, i});
    }

    for (size_t i = 0; i < state_.players.size(); ++i) {
        const Player& player = state_.players[i];
        const Vector2 draw_pos = GetRenderPlayerPosition(player.id);
        items.push_back({RenderKind::Player, draw_pos.y + 16.0f, i});
    }

    std::sort(items.begin(), items.end(), [&](const RenderItem& a, const RenderItem& b) {
        const float dy = a.sort_y - b.sort_y;
        if (std::fabs(dy) > 0.001f) {
            return dy < 0.0f;
        }
        return kind_priority(a.kind) < kind_priority(b.kind);
    });

    const bool has_texture = sprite_metadata_.IsLoaded();
    const Texture2D texture = sprite_metadata_.GetTexture();
    const bool has_tall_texture = sprite_metadata_tall_.IsLoaded();
    const Texture2D tall_texture = sprite_metadata_tall_.GetTexture();
    const bool has_large_texture = sprite_metadata_96x96_.IsLoaded();
    const Texture2D large_texture = sprite_metadata_96x96_.GetTexture();

    const auto get_metadata = [&](CompositeEffectSheet sheet) -> const SpriteMetadataLoader* {
        switch (sheet) {
            case CompositeEffectSheet::Base32:
                return &sprite_metadata_;
            case CompositeEffectSheet::Tall32x64:
                return &sprite_metadata_tall_;
            case CompositeEffectSheet::Large96x96:
                return &sprite_metadata_96x96_;
        }
        return &sprite_metadata_;
    };
    const auto get_texture = [&](CompositeEffectSheet sheet) -> const Texture2D* {
        switch (sheet) {
            case CompositeEffectSheet::Base32:
                return has_texture ? &texture : nullptr;
            case CompositeEffectSheet::Tall32x64:
                return has_tall_texture ? &tall_texture : nullptr;
            case CompositeEffectSheet::Large96x96:
                return has_large_texture ? &large_texture : nullptr;
        }
        return nullptr;
    };

    for (const RenderItem& item : items) {
        switch (item.kind) {
            case RenderKind::LegacyDecoration: {
                if (!has_tall_texture || state_.map.width <= 0) {
                    break;
                }
                const std::string& animation_name = state_.map.decorations[item.index];
                if (animation_name.empty() || !sprite_metadata_tall_.HasAnimation(animation_name)) {
                    break;
                }
                const int cell_x = static_cast<int>(item.index % static_cast<size_t>(state_.map.width));
                const int cell_y = static_cast<int>(item.index / static_cast<size_t>(state_.map.width));

                const float dst_w = static_cast<float>(sprite_metadata_tall_.GetCellWidth());
                const float dst_h = static_cast<float>(sprite_metadata_tall_.GetCellHeight());
                Rectangle dst = {static_cast<float>(cell_x * state_.map.cell_size),
                                 static_cast<float>(cell_y * state_.map.cell_size) -
                                     (dst_h - static_cast<float>(state_.map.cell_size)),
                                 dst_w, dst_h};
                dst = SnapRect(dst);

                const Rectangle src = InsetSourceRect(
                    sprite_metadata_tall_.GetFrame(animation_name, "default", render_time_seconds_),
                    Constants::kAtlasSampleInsetPixels);
                DrawTexturePro(tall_texture, src, dst, {0, 0}, 0.0f, WHITE);
                break;
            }
            case RenderKind::MapObject: {
                const MapObjectInstance& object = state_.map_objects[item.index];
                const ObjectPrototype* proto = FindObjectPrototype(object.prototype_id);
                if (proto == nullptr) {
                    break;
                }

                const bool use_tall = proto->sprite_sheet == SpriteSheetType::Tall32x64;
                const SpriteMetadataLoader& metadata = use_tall ? sprite_metadata_tall_ : sprite_metadata_;
                const float dst_w = static_cast<float>(metadata.GetCellWidth() > 0 ? metadata.GetCellWidth() : state_.map.cell_size);
                const float dst_h = static_cast<float>(metadata.GetCellHeight() > 0 ? metadata.GetCellHeight() : state_.map.cell_size);
                Rectangle dst = {static_cast<float>(object.cell.x * state_.map.cell_size),
                                 static_cast<float>(object.cell.y * state_.map.cell_size) -
                                     (dst_h - static_cast<float>(state_.map.cell_size)),
                                 dst_w, dst_h};
                dst = SnapRect(dst);

                std::string animation = proto->idle_animation;
                float anim_time = render_time_seconds_;
                if (object.state == MapObjectState::Dying && !proto->death_animation.empty() &&
                    metadata.HasAnimation(proto->death_animation)) {
                    animation = proto->death_animation;
                    anim_time = object.state_time;
                }

                if (metadata.IsLoaded() && metadata.HasAnimation(animation)) {
                    const Texture2D draw_texture = use_tall ? tall_texture : texture;
                    const Rectangle src =
                        InsetSourceRect(metadata.GetFrame(animation, "default", anim_time), Constants::kAtlasSampleInsetPixels);
                    DrawTexturePro(draw_texture, src, dst, {0, 0}, 0.0f, WHITE);
                } else {
                    DrawRectangleRec(dst, Color{180, 120, 80, 220});
                    DrawRectangleLinesEx(dst, 1.0f, Color{30, 20, 12, 255});
                }
                break;
            }
            case RenderKind::CompositeEffectPart: {
                if (item.index >= state_.composite_effects.size()) {
                    break;
                }
                const CompositeEffectInstance& effect = state_.composite_effects[item.index];
                const CompositeEffectDefinition* definition = composite_effects_loader_.FindById(effect.effect_id);
                if (!effect.alive || definition == nullptr || item.sub_index >= definition->parts.size()) {
                    break;
                }
                const auto& part = definition->parts[item.sub_index];
                const SpriteMetadataLoader* metadata = get_metadata(part.sheet);
                const Texture2D* draw_texture = get_texture(part.sheet);
                if (metadata == nullptr || draw_texture == nullptr || !metadata->HasAnimation(part.animation)) {
                    break;
                }

                const float cell = static_cast<float>(state_.map.cell_size);
                const Vector2 draw_pos = {
                    effect.origin_world.x + part.offset_x_tiles * cell,
                    effect.origin_world.y + 0.5f * cell + part.offset_y_tiles * cell,
                };
                const Rectangle src =
                    InsetSourceRect(metadata->GetFrame(part.animation, "default", effect.age_seconds),
                                    Constants::kAtlasSampleInsetPixels);
                const float dst_w = static_cast<float>(metadata->GetCellWidth());
                const float dst_h = static_cast<float>(metadata->GetCellHeight());
                Rectangle dst = {draw_pos.x - dst_w * 0.5f, draw_pos.y - dst_h, dst_w, dst_h};
                DrawTexturePro(*draw_texture, src, SnapRect(dst), {0, 0}, 0.0f, WHITE);
                break;
            }
            case RenderKind::FireStormDummyPart: {
                if (item.index >= state_.fire_storm_dummies.size()) {
                    break;
                }
                const FireStormDummy& dummy = state_.fire_storm_dummies[item.index];
                if (!dummy.alive) {
                    break;
                }
                const char* effect_id = nullptr;
                if (item.aux_index == 1) {
                    effect_id = "fire_storm_lightning";
                } else {
                    effect_id = dummy.state == FireStormDummyState::Born
                                    ? "fire_storm_born"
                                    : (dummy.state == FireStormDummyState::Dying ? "fire_storm_death" : "fire_storm_idle");
                }
                const CompositeEffectDefinition* definition = composite_effects_loader_.FindById(effect_id);
                if (definition == nullptr || item.sub_index >= definition->parts.size()) {
                    break;
                }
                const auto& part = definition->parts[item.sub_index];
                if (!has_texture || !sprite_metadata_.HasAnimation(part.animation)) {
                    break;
                }
                const float cell = static_cast<float>(state_.map.cell_size);
                const Vector2 origin = CellToWorldCenter(dummy.cell);
                const Vector2 draw_pos = {
                    origin.x + part.offset_x_tiles * cell,
                    origin.y + 0.5f * cell + part.offset_y_tiles * cell,
                };
                const Rectangle src = InsetSourceRect(
                    sprite_metadata_.GetFrame(part.animation, "default", dummy.state_time),
                    Constants::kAtlasSampleInsetPixels);
                Rectangle dst = {draw_pos.x - 16.0f, draw_pos.y - 32.0f, 32.0f, 32.0f};
                DrawTexturePro(texture, src, SnapRect(dst), {0, 0}, 0.0f, WHITE);
                break;
            }
            case RenderKind::GrapplingHookSegment: {
                if (item.index >= state_.grappling_hooks.size() || !has_texture) {
                    break;
                }
                const GrapplingHook& hook = state_.grappling_hooks[item.index];
                const Player* owner = FindPlayerById(hook.owner_player_id);
                if (!hook.alive || owner == nullptr || !owner->alive) {
                    break;
                }

                const Vector2 start = Vector2Add(GetRenderPlayerPosition(owner->id),
                                                 {0.0f, 0.5f * static_cast<float>(state_.map.cell_size)});
                const Vector2 end =
                    (hook.phase == GrapplingHookPhase::Pulling)
                        ? hook.latch_point
                        : ((!network_manager_.IsHost() && hook.owner_player_id == state_.local_player_id)
                               ? hook.head_pos
                               : GetRenderGrapplingHookHeadPosition(hook.id, hook.head_pos));
                const Vector2 delta = Vector2Subtract(end, start);
                const float total_len = Vector2Length(delta);
                if (total_len <= 0.001f) {
                    break;
                }

                const int segments = std::max(2, static_cast<int>(item.aux_index));
                const int seg = static_cast<int>(item.sub_index);
                const float rotation = static_cast<float>(std::atan2(delta.y, delta.x) * RAD2DEG);
                const bool use_latched_animation =
                    hook.phase == GrapplingHookPhase::Pulling || hook.phase == GrapplingHookPhase::Retracting;
                const char* animation = nullptr;
                if (use_latched_animation) {
                    animation = "grappling_hook_latched";
                } else if (seg == segments - 1) {
                    animation = "grappling_hook_head";
                } else {
                    animation = "grappling_hook";
                }
                if (!sprite_metadata_.HasAnimation(animation)) {
                    break;
                }
                const Rectangle src = InsetSourceRect(
                    sprite_metadata_.GetFrame(animation, "default", hook.animation_time),
                    Constants::kAtlasSampleInsetPixels);
                const float seg_h = static_cast<float>(std::max(1, state_.map.cell_size));
                const float anchor_y = seg_h * Constants::kLightningAnchorYRatio;

                const float t0 = static_cast<float>(seg) / static_cast<float>(segments);
                const float t1 = static_cast<float>(seg + 1) / static_cast<float>(segments);
                const Vector2 p0 = Vector2Add(start, Vector2Scale(delta, t0));
                const Vector2 p1 = Vector2Add(start, Vector2Scale(delta, t1));
                const Vector2 seg_vec = Vector2Subtract(p1, p0);
                const float seg_len = std::max(1.0f, Vector2Length(seg_vec));
                Rectangle dst = {p0.x, p0.y - anchor_y, seg_len + 1.0f, seg_h};
                DrawTexturePro(texture, src, dst, {0.0f, anchor_y}, rotation, WHITE);
                break;
            }
            case RenderKind::StatusEffectPart: {
                if (item.index >= state_.players.size()) {
                    break;
                }
                const Player& player = state_.players[item.index];
                if (!player.alive || item.aux_index >= player.status_effects.size()) {
                    break;
                }
                const StatusEffectInstance& status = player.status_effects[item.aux_index];
                const CompositeEffectDefinition* definition = composite_effects_loader_.FindById(status.composite_effect_id);
                if (definition == nullptr || item.sub_index >= definition->parts.size()) {
                    break;
                }
                const auto& part = definition->parts[item.sub_index];
                if (!has_texture || !sprite_metadata_.HasAnimation(part.animation)) {
                    break;
                }
                const float cell = static_cast<float>(state_.map.cell_size);
                const Vector2 origin = GetRenderPlayerPosition(player.id);
                const Vector2 draw_pos = {
                    origin.x + part.offset_x_tiles * cell,
                    origin.y + 0.5f * cell + part.offset_y_tiles * cell,
                };
                const float animation_time = status.total_seconds - status.remaining_seconds;
                const Rectangle src = InsetSourceRect(
                    sprite_metadata_.GetFrame(part.animation, "default", animation_time),
                    Constants::kAtlasSampleInsetPixels);
                Rectangle dst = {draw_pos.x - 16.0f, draw_pos.y - 32.0f, 32.0f, 32.0f};
                DrawTexturePro(texture, src, SnapRect(dst), {0, 0}, 0.0f, WHITE);
                break;
            }
            case RenderKind::EarthRootsPart: {
                if (item.index >= state_.earth_roots_groups.size() || !has_texture) {
                    break;
                }
                const EarthRootsGroup& group = state_.earth_roots_groups[item.index];
                const int tile_index = static_cast<int>(item.aux_index);
                const int dx = tile_index % 3 - 1;
                const int dy = tile_index / 3 - 1;
                const GridCoord cell = {group.center_cell.x + dx, group.center_cell.y + dy};
                const Vector2 center = CellToWorldCenter(cell);
                const bool front = item.sub_index == 1;
                const char* animation_name = nullptr;
                float animation_time = 0.0f;
                switch (group.state) {
                    case EarthRootsGroupState::Born:
                        animation_name = front ? "roots_born_front" : "roots_born_back";
                        animation_time = group.state_time;
                        break;
                    case EarthRootsGroupState::Idle:
                        animation_name = front ? "roots_idle_front" : "roots_idle_back";
                        animation_time = 0.0f;
                        break;
                    case EarthRootsGroupState::Dying:
                        animation_name = front ? "roots_death_front" : "roots_death_back";
                        animation_time = group.state_time;
                        break;
                }
                if (animation_name == nullptr || !sprite_metadata_.HasAnimation(animation_name)) {
                    break;
                }
                const Rectangle src = InsetSourceRect(
                    sprite_metadata_.GetFrame(animation_name, "default", animation_time),
                    Constants::kAtlasSampleInsetPixels);
                Rectangle dst = {center.x - 16.0f, center.y - 16.0f, 32.0f, 32.0f};
                DrawTexturePro(texture, src, SnapRect(dst), {0, 0}, 0.0f, WHITE);
                break;
            }
            case RenderKind::Rune: {
                const Rune& rune = state_.runes[item.index];
                const Vector2 center = CellToWorldCenter(rune.cell);
                const char* animation_name = rune.active
                                                 ? (rune.rune_type == RuneType::Earth ? GetEarthRuneAnimationKey(rune)
                                                                                      : GetRuneSpriteAnimationKey(rune.rune_type))
                                                 : GetRuneBornSpriteAnimationKey(rune.rune_type);
                const float animation_time = rune.active
                                                 ? (rune.rune_type == RuneType::Earth
                                                        ? ((rune.earth_trap_state == EarthRuneTrapState::Slamming)
                                                               ? (rune.earth_state_time / Constants::kEarthRuneSlamSlowdown)
                                                               : rune.earth_state_time)
                                                        : (render_time_seconds_ + rune.placement_order * 0.05f))
                                                 : (rune.activation_total_seconds - rune.activation_remaining_seconds);

                const char* fallback_animation = rune.active ? "" : "rune_born";
                if (has_texture &&
                    (sprite_metadata_.HasAnimation(animation_name) ||
                     (fallback_animation[0] != '\0' && sprite_metadata_.HasAnimation(fallback_animation)))) {
                    if (!sprite_metadata_.HasAnimation(animation_name) && fallback_animation[0] != '\0') {
                        animation_name = fallback_animation;
                    }
                    const Rectangle src = InsetSourceRect(
                        sprite_metadata_.GetFrame(animation_name, "default", animation_time),
                        Constants::kAtlasSampleInsetPixels);
                    Rectangle dst = {center.x - 16.0f, center.y - 16.0f, 32.0f, 32.0f};
                    dst = SnapRect(dst);
                    DrawTexturePro(texture, src, dst, {0, 0}, 0.0f, WHITE);
                } else {
                    DrawCircleV(center, 8.0f, GetRuneFallbackColor(rune.rune_type));
                    DrawCircleLinesV(center, 8.0f, Color{20, 24, 32, 255});
                }
                break;
            }
            case RenderKind::IceWall: {
                const IceWallPiece& wall = state_.ice_walls[item.index];
                const Vector2 center = CellToWorldCenter(wall.cell);
                Rectangle dst = {center.x - 16.0f, center.y - 16.0f, 32.0f, 32.0f};
                dst = SnapRect(dst);

                const char* animation_name = "ice_wall_idle";
                float animation_time = render_time_seconds_;
                if (wall.state == IceWallState::Materializing) {
                    animation_name = "ice_wall_born";
                    animation_time = wall.state_time;
                } else if (wall.state == IceWallState::Dying) {
                    animation_name = "ice_wall_death";
                    animation_time = wall.state_time;
                }

                if (has_texture && sprite_metadata_.HasAnimation(animation_name)) {
                    const Rectangle src =
                        InsetSourceRect(sprite_metadata_.GetFrame(animation_name, "default", animation_time),
                                        Constants::kAtlasSampleInsetPixels);
                    DrawTexturePro(texture, src, dst, {0, 0}, 0.0f, WHITE);
                } else {
                    const Color fallback =
                        (wall.state == IceWallState::Dying) ? Color{140, 180, 230, 180} : Color{180, 220, 255, 255};
                    DrawRectangleRec(dst, fallback);
                    DrawRectangleLinesEx(dst, 1.0f, Color{60, 95, 140, 255});
                }
                break;
            }
            case RenderKind::LightningSegment: {
                if (item.index >= state_.lightning_effects.size()) {
                    break;
                }
                const LightningEffect& effect = state_.lightning_effects[item.index];
                if (!effect.alive) {
                    break;
                }
                const char* animation_key =
                    effect.dying ? (effect.volatile_variant ? "lightning_volatile_magic_death" : "lightning_magic_death")
                                 : (effect.volatile_variant ? "lightning_volatile_magic_idle" : "lightning_magic_idle");
                if (!has_texture || !sprite_metadata_.HasAnimation(animation_key)) {
                    break;
                }

                const Vector2 delta = Vector2Subtract(effect.end, effect.start);
                const float total_len = Vector2Length(delta);
                if (total_len <= 0.001f) {
                    break;
                }
                const int segments = std::max(1, effect.segment_count);
                const size_t seg = std::min(item.sub_index, static_cast<size_t>(segments - 1));
                const float t0 = static_cast<float>(seg) / static_cast<float>(segments);
                const float t1 = static_cast<float>(seg + 1) / static_cast<float>(segments);
                const Vector2 p0 = Vector2Add(effect.start, Vector2Scale(delta, t0));
                const Vector2 p1 = Vector2Add(effect.start, Vector2Scale(delta, t1));
                const Vector2 seg_vec = Vector2Subtract(p1, p0);
                const float seg_len = std::max(1.0f, Vector2Length(seg_vec));
                const float rotation = static_cast<float>(std::atan2(delta.y, delta.x) * RAD2DEG);

                float anim_time = effect.dying ? effect.death_elapsed : effect.elapsed;
                if (!effect.dying && seg < effect.segment_phase_offsets_seconds.size()) {
                    anim_time += effect.segment_phase_offsets_seconds[seg];
                }

                const Rectangle src =
                    InsetSourceRect(sprite_metadata_.GetFrame(animation_key, "default", anim_time),
                                    Constants::kAtlasSampleInsetPixels);
                const float seg_h = static_cast<float>(std::max(1, state_.map.cell_size));
                const float anchor_y = seg_h * Constants::kLightningAnchorYRatio;
                Rectangle dst = {p0.x, p0.y - anchor_y, seg_len, seg_h};
                DrawTexturePro(texture, src, dst, {0.0f, anchor_y}, rotation, WHITE);
                break;
            }
            case RenderKind::Projectile: {
                const Projectile& projectile = state_.projectiles[item.index];
                const bool use_large_static = projectile.animation_key == "fire_storm_static_large";
                const SpriteMetadataLoader* projectile_metadata =
                    use_large_static ? (has_large_texture ? &sprite_metadata_96x96_ : nullptr)
                                     : (has_texture ? &sprite_metadata_ : nullptr);
                const Texture2D* projectile_texture =
                    use_large_static ? (has_large_texture ? &large_texture : nullptr) : (has_texture ? &texture : nullptr);
                if (projectile_metadata != nullptr && projectile_texture != nullptr &&
                    projectile_metadata->HasAnimation(projectile.animation_key)) {
                    const Rectangle src = InsetSourceRect(
                        projectile_metadata->GetFrame(projectile.animation_key, "default", render_time_seconds_),
                        Constants::kAtlasSampleInsetPixels);
                    const float dst_w = use_large_static ? static_cast<float>(projectile_metadata->GetCellWidth())
                                                         : projectile.radius * 8.0f;
                    const float dst_h = use_large_static ? static_cast<float>(projectile_metadata->GetCellHeight())
                                                         : projectile.radius * 8.0f;
                    Rectangle dst = use_large_static
                                        ? Rectangle{projectile.pos.x, projectile.pos.y, dst_w, dst_h}
                                        : Rectangle{projectile.pos.x - dst_w * 0.5f, projectile.pos.y - dst_h * 0.5f,
                                                    dst_w, dst_h};
                    dst = SnapRect(dst);
                    const Color tint = IsStaticFireBolt(projectile)
                                           ? WHITE
                                           : (projectile.owner_team == Constants::kTeamRed ? Color{255, 215, 215, 255}
                                                                                           : Color{215, 230, 255, 255});
                    const Vector2 rotation_vel =
                        use_large_static && projectile.upgrade_pause_remaining > 0.0f ? projectile.resume_vel : projectile.vel;
                    const float rotation = use_large_static ? AimToDegrees(rotation_vel) : 0.0f;
                    const Vector2 origin = use_large_static ? Vector2{dst_w * 0.5f, dst_h * 0.5f} : Vector2{0.0f, 0.0f};
                    DrawTexturePro(*projectile_texture, src, dst, origin, rotation, tint);
                } else {
                    DrawCircleV(projectile.pos, projectile.radius,
                                projectile.owner_team == Constants::kTeamRed ? Color{255, 92, 48, 255}
                                                                             : Color{80, 180, 255, 255});
                }
                break;
            }
            case RenderKind::Particle: {
                const Particle& particle = state_.particles[item.index];
                if (has_texture && sprite_metadata_.HasAnimation(particle.animation_key)) {
                    const Rectangle src =
                        InsetSourceRect(sprite_metadata_.GetFrame(particle.animation_key, particle.facing, particle.age_seconds),
                                        Constants::kAtlasSampleInsetPixels);
                    const float particle_scale = particle.animation_key == "static_upgrade" ? 1.5f : 1.0f;
                    const float particle_size = 32.0f * particle_scale;
                    Rectangle dst = {particle.pos.x - particle_size * 0.5f, particle.pos.y - particle_size * 0.5f,
                                     particle_size, particle_size};
                    dst = SnapRect(dst);
                    const unsigned char alpha = particle.animation_key == "explosion" ? 255 : 200;
                    DrawTexturePro(texture, src, dst, {0, 0}, 0.0f, Color{255, 255, 255, alpha});
                } else {
                    DrawCircleV(particle.pos, 4.0f, Color{190, 190, 190, 160});
                }
                break;
            }
            case RenderKind::Player: {
                const Player& player = state_.players[item.index];
                const Vector2 draw_pos = GetRenderPlayerPosition(player.id);
                if (!player.alive) {
                    DrawCircleV(draw_pos, player.radius, Color{70, 70, 70, 180});
                    break;
                }

                const bool is_moving = Vector2LengthSqr(player.vel) > 8.0f;
                const char* base_prefix = player.team == Constants::kTeamRed ? "wizard_red_" : "wizard_blue_";
                const char* suffix = "idle";
                if (player.action_state == PlayerActionState::Slashing) {
                    suffix = "slash";
                } else if (player.action_state == PlayerActionState::RunePlacing) {
                    suffix = "create_rune";
                } else if (is_moving) {
                    suffix = "walking";
                }
                std::string animation = std::string(base_prefix) + suffix;
                if (!sprite_metadata_.HasAnimation(animation)) {
                    // Backward compatibility with legacy non-team animation keys.
                    animation = std::string("wizard_") + suffix;
                }

                const char* facing = FacingToSpriteFacing(player.facing);
                Rectangle dst = {draw_pos.x - 16.0f, draw_pos.y - 16.0f, 32.0f, 32.0f};
                dst = SnapRect(dst);
                const Rectangle sprite_rect = dst;

                if (has_texture && sprite_metadata_.HasAnimation(animation)) {
                    const bool mirror = player.facing == FacingDirection::Left;
                    Rectangle src = sprite_metadata_.GetFrame(animation, facing, render_time_seconds_);
                    if (mirror) {
                        src = sprite_metadata_.GetMirroredFrameRect(src);
                    }
                    src = InsetSourceRect(src, Constants::kAtlasSampleInsetPixels);

                    const Texture2D draw_texture = sprite_metadata_.GetTexture(mirror);
                    DrawTexturePro(draw_texture, src, dst, {0, 0}, 0.0f, WHITE);
                } else {
                    const Color fallback = (player.team == Constants::kTeamRed) ? RED : BLUE;
                    DrawCircleV(draw_pos, player.radius, fallback);
                }

                const Color bar_fill = {static_cast<unsigned char>(Constants::kPlayerHealthBarFillR),
                                        static_cast<unsigned char>(Constants::kPlayerHealthBarFillG),
                                        static_cast<unsigned char>(Constants::kPlayerHealthBarFillB), 255};
                const Color bar_missing = {static_cast<unsigned char>(Constants::kPlayerHealthBarMissingR),
                                           static_cast<unsigned char>(Constants::kPlayerHealthBarMissingG),
                                           static_cast<unsigned char>(Constants::kPlayerHealthBarMissingB), 255};

                Rectangle health_bar = {sprite_rect.x, sprite_rect.y - Constants::kPlayerHealthBarOffsetY,
                                        Constants::kPlayerHealthBarWidth, Constants::kPlayerHealthBarHeight};
                health_bar = SnapRect(health_bar);
                DrawRectangleRec(health_bar, bar_missing);

                const float hp_ratio =
                    std::clamp(static_cast<float>(player.hp) / static_cast<float>(Constants::kMaxHp), 0.0f, 1.0f);
                if (hp_ratio > 0.0f) {
                    Rectangle fill = health_bar;
                    fill.width = std::roundf(fill.width * hp_ratio);
                    if (fill.width > 0.0f) {
                        DrawRectangleRec(fill, bar_fill);
                    }
                }
                DrawRectangleLinesEx(health_bar, 1.0f, BLACK);

                if (player.id != state_.local_player_id && !player.name.empty()) {
                    const int name_font_size = 8;
                    const int name_width = MeasureText(player.name.c_str(), name_font_size);
                    const int name_x =
                        static_cast<int>(health_bar.x + (health_bar.width - static_cast<float>(name_width)) * 0.5f);
                    const int name_y = static_cast<int>(health_bar.y - static_cast<float>(name_font_size) - 2.0f);
                    DrawText(player.name.c_str(), name_x, name_y, name_font_size, WHITE);
                }
                break;
            }
        }
    }

}

void GameApp::RenderMap() {
    if (state_.map.width <= 0 || state_.map.height <= 0) {
        return;
    }

    const bool has_texture = sprite_metadata_.IsLoaded();
    const Texture2D texture = sprite_metadata_.GetTexture();
    const bool has_tall_texture = sprite_metadata_tall_.IsLoaded();
    const Texture2D tall_texture = sprite_metadata_tall_.GetTexture();
    const bool has_large_texture = sprite_metadata_96x96_.IsLoaded();
    const Texture2D large_texture = sprite_metadata_96x96_.GetTexture();

    const auto get_metadata = [&](CompositeEffectSheet sheet) -> const SpriteMetadataLoader* {
        switch (sheet) {
            case CompositeEffectSheet::Base32:
                return &sprite_metadata_;
            case CompositeEffectSheet::Tall32x64:
                return &sprite_metadata_tall_;
            case CompositeEffectSheet::Large96x96:
                return &sprite_metadata_96x96_;
        }
        return &sprite_metadata_;
    };
    const auto get_texture = [&](CompositeEffectSheet sheet) -> const Texture2D* {
        switch (sheet) {
            case CompositeEffectSheet::Base32:
                return has_texture ? &texture : nullptr;
            case CompositeEffectSheet::Tall32x64:
                return has_tall_texture ? &tall_texture : nullptr;
            case CompositeEffectSheet::Large96x96:
                return has_large_texture ? &large_texture : nullptr;
        }
        return nullptr;
    };
    const bool has_grass_dual_grid = has_texture && sprite_metadata_.HasDualGridAnimation("tile_grass");
    const bool has_grass_bitmask = has_texture && sprite_metadata_.HasBitmaskAnimation("tile_grass");
    const bool has_water_animation = has_texture && sprite_metadata_.HasAnimation("tile_water");
    const bool has_zone_overlay = has_texture && sprite_metadata_.HasAnimation("zone");
    const auto is_grass_family = [](TileType tile) {
        return tile == TileType::Grass || tile == TileType::SpawnPoint;
    };

    const auto sample_grass_for_dual_grid = [&](int x, int y) {
        if (state_.map.width <= 0 || state_.map.height <= 0) {
            return false;
        }
        x = std::clamp(x, 0, state_.map.width - 1);
        y = std::clamp(y, 0, state_.map.height - 1);
        return is_grass_family(state_.map.tiles[static_cast<size_t>(y * state_.map.width + x)]);
    };

    const auto compute_grass_dual_grid_mask = [&](int x, int y) {
        int mask = 0;
        if (sample_grass_for_dual_grid(x, y)) {
            mask |= 1;  // TL
        }
        if (sample_grass_for_dual_grid(x + 1, y)) {
            mask |= 2;  // TR
        }
        if (sample_grass_for_dual_grid(x + 1, y + 1)) {
            mask |= 4;  // BR
        }
        if (sample_grass_for_dual_grid(x, y + 1)) {
            mask |= 8;  // BL
        }
        return mask;
    };

    const auto compute_grass_bitmask = [&](int x, int y) {
        int mask = 0;
        const GridCoord north = {x, y - 1};
        const GridCoord west = {x - 1, y};
        const GridCoord east = {x + 1, y};
        const GridCoord south = {x, y + 1};
        const GridCoord north_west = {x - 1, y - 1};
        const GridCoord north_east = {x + 1, y - 1};
        const GridCoord south_west = {x - 1, y + 1};
        const GridCoord south_east = {x + 1, y + 1};

        const bool has_n = state_.map.IsInside(north) && is_grass_family(state_.map.GetTile(north));
        const bool has_w = state_.map.IsInside(west) && is_grass_family(state_.map.GetTile(west));
        const bool has_e = state_.map.IsInside(east) && is_grass_family(state_.map.GetTile(east));
        const bool has_s = state_.map.IsInside(south) && is_grass_family(state_.map.GetTile(south));

        // 8-bit layout:
        // NW=1, N=2, NE=4, W=8, E=16, SW=32, S=64, SE=128
        if (has_n) {
            mask |= 2;
        }
        if (has_w) {
            mask |= 8;
        }
        if (has_e) {
            mask |= 16;
        }
        if (has_s) {
            mask |= 64;
        }

        // Corner bits only count when both adjacent cardinals are present.
        if (has_n && has_w && state_.map.IsInside(north_west) && is_grass_family(state_.map.GetTile(north_west))) {
            mask |= 1;
        }
        if (has_n && has_e && state_.map.IsInside(north_east) && is_grass_family(state_.map.GetTile(north_east))) {
            mask |= 4;
        }
        if (has_s && has_w && state_.map.IsInside(south_west) && is_grass_family(state_.map.GetTile(south_west))) {
            mask |= 32;
        }
        if (has_s && has_e && state_.map.IsInside(south_east) && is_grass_family(state_.map.GetTile(south_east))) {
            mask |= 128;
        }

        return mask;
    };

    for (int y = 0; y < state_.map.height; ++y) {
        for (int x = 0; x < state_.map.width; ++x) {
            const TileType tile = state_.map.tiles[static_cast<size_t>(y * state_.map.width + x)];
            const Rectangle dst = SnapRect(
                {static_cast<float>(x * state_.map.cell_size), static_cast<float>(y * state_.map.cell_size),
                 static_cast<float>(state_.map.cell_size), static_cast<float>(state_.map.cell_size)});
            const Vector2 tile_center = {dst.x + dst.width * 0.5f, dst.y + dst.height * 0.5f};
            const bool dimmed = IsOutsideArena(tile_center);
            const Color tint = dimmed ? Color{static_cast<unsigned char>(255.0f * Constants::kOutsideZoneTileBrightness),
                                              static_cast<unsigned char>(255.0f * Constants::kOutsideZoneTileBrightness),
                                              static_cast<unsigned char>(255.0f * Constants::kOutsideZoneTileBrightness),
                                              255}
                                      : WHITE;

            const auto draw_zone_overlay = [&]() {
                if (!dimmed) {
                    return;
                }
                if (has_zone_overlay) {
                    const Rectangle zone_src =
                        InsetSourceRect(sprite_metadata_.GetFrame("zone", "default", render_time_seconds_),
                                        Constants::kAtlasSampleInsetPixels);
                    DrawTexturePro(texture, zone_src, dst, {0, 0}, 0.0f, Color{255, 255, 255, 128});
                } else {
                    DrawRectangleRec(dst, Color{0, 0, 0, 128});
                }
            };

            if (has_texture && has_grass_dual_grid) {
                const float half_cell = static_cast<float>(state_.map.cell_size) * 0.5f;
                const Rectangle terrain_dst =
                    SnapRect({dst.x + half_cell, dst.y + half_cell, dst.width, dst.height});

                if (has_water_animation) {
                    const Rectangle water_src = InsetSourceRect(
                        sprite_metadata_.GetFrame("tile_water", "default", render_time_seconds_),
                        Constants::kAtlasSampleInsetPixels);
                    DrawTexturePro(texture, water_src, terrain_dst, {0, 0}, 0.0f, WHITE);
                } else {
                    DrawRectangleRec(terrain_dst, Color{26, 96, 152, 255});
                }

                const int mask = compute_grass_dual_grid_mask(x, y);
                const bool has_background =
                    sprite_metadata_.HasDualGridLayer("tile_grass", mask, SpriteFrameLayer::Background);
                const bool has_foreground =
                    sprite_metadata_.HasDualGridLayer("tile_grass", mask, SpriteFrameLayer::Foreground);

                Rectangle src = {};
                if (has_background &&
                    sprite_metadata_.GetDualGridFrame("tile_grass", mask, render_time_seconds_,
                                                      SpriteFrameLayer::Background, src)) {
                    DrawTexturePro(texture, InsetSourceRect(src, Constants::kAtlasSampleInsetPixels), terrain_dst, {0, 0},
                                   0.0f, WHITE);
                }
                if (has_foreground &&
                    sprite_metadata_.GetDualGridFrame("tile_grass", mask, render_time_seconds_,
                                                      SpriteFrameLayer::Foreground, src)) {
                    DrawTexturePro(texture, InsetSourceRect(src, Constants::kAtlasSampleInsetPixels), terrain_dst, {0, 0},
                                   0.0f, WHITE);
                }
                if (!has_background && !has_foreground &&
                    sprite_metadata_.GetDualGridFrame("tile_grass", mask, render_time_seconds_, SpriteFrameLayer::Single,
                                                      src)) {
                    DrawTexturePro(texture, InsetSourceRect(src, Constants::kAtlasSampleInsetPixels), terrain_dst, {0, 0},
                                   0.0f, WHITE);
                }

                draw_zone_overlay();

                if (tile == TileType::SpawnPoint) {
                    DrawCircleV({dst.x + dst.width * 0.5f, dst.y + dst.height * 0.5f}, 3.0f,
                                Color{240, 240, 240, 180});
                }
            } else if (has_texture && (tile == TileType::Grass || tile == TileType::SpawnPoint)) {
                if (has_grass_bitmask) {
                    const int mask = compute_grass_bitmask(x, y);
                    const bool has_background =
                        sprite_metadata_.HasBitmaskLayer("tile_grass", mask, SpriteFrameLayer::Background);
                    const bool has_foreground =
                        sprite_metadata_.HasBitmaskLayer("tile_grass", mask, SpriteFrameLayer::Foreground);

                    Rectangle src = {};
                    if (has_background &&
                        sprite_metadata_.GetBitmaskFrame("tile_grass", mask, render_time_seconds_,
                                                         SpriteFrameLayer::Background, src)) {
                        DrawTexturePro(texture, InsetSourceRect(src, Constants::kAtlasSampleInsetPixels), dst, {0, 0},
                                       0.0f, tint);
                    }
                    if (has_foreground &&
                        sprite_metadata_.GetBitmaskFrame("tile_grass", mask, render_time_seconds_,
                                                         SpriteFrameLayer::Foreground, src)) {
                        DrawTexturePro(texture, InsetSourceRect(src, Constants::kAtlasSampleInsetPixels), dst, {0, 0},
                                       0.0f, tint);
                    }
                    if (!has_background && !has_foreground &&
                        sprite_metadata_.GetBitmaskFrame("tile_grass", mask, render_time_seconds_,
                                                         SpriteFrameLayer::Single, src)) {
                        DrawTexturePro(texture, InsetSourceRect(src, Constants::kAtlasSampleInsetPixels), dst, {0, 0},
                                       0.0f, tint);
                    }
                } else {
                    const Rectangle src = InsetSourceRect(
                        sprite_metadata_.GetFrame("tile_grass", "default", render_time_seconds_),
                        Constants::kAtlasSampleInsetPixels);
                    DrawTexturePro(texture, src, dst, {0, 0}, 0.0f, tint);
                }
                if (tile == TileType::SpawnPoint) {
                    DrawCircleV({dst.x + dst.width * 0.5f, dst.y + dst.height * 0.5f}, 3.0f,
                                Color{240, 240, 240, 180});
                }
                draw_zone_overlay();
            } else if (has_texture && tile == TileType::Water) {
                const Rectangle src = InsetSourceRect(
                    sprite_metadata_.GetFrame("tile_water", "default", render_time_seconds_),
                    Constants::kAtlasSampleInsetPixels);
                DrawTexturePro(texture, src, dst, {0, 0}, 0.0f, tint);
                draw_zone_overlay();
            } else {
                Color fallback = Color{34, 120, 34, 255};
                if (tile == TileType::Water) fallback = Color{26, 96, 152, 255};
                if (dimmed) {
                    fallback = Color{static_cast<unsigned char>(fallback.r * Constants::kOutsideZoneTileBrightness),
                                     static_cast<unsigned char>(fallback.g * Constants::kOutsideZoneTileBrightness),
                                     static_cast<unsigned char>(fallback.b * Constants::kOutsideZoneTileBrightness), 255};
                }
                DrawRectangleRec(dst, fallback);
                draw_zone_overlay();
            }
        }
    }

    for (const auto& zone : state_.influence_zones) {
        if (!state_.map.IsInside(zone.cell)) {
            continue;
        }
        const Rectangle dst = SnapRect(
            {static_cast<float>(zone.cell.x * state_.map.cell_size), static_cast<float>(zone.cell.y * state_.map.cell_size),
             static_cast<float>(state_.map.cell_size), static_cast<float>(state_.map.cell_size)});
        const char* animation = zone.team == Constants::kTeamRed ? "influence_zone_red" : "influence_zone_blue";
        if (has_texture && sprite_metadata_.HasAnimation(animation)) {
            const Rectangle src =
                InsetSourceRect(sprite_metadata_.GetFrame(animation, "default", render_time_seconds_),
                                Constants::kAtlasSampleInsetPixels);
            DrawTexturePro(texture, src, dst, {0, 0}, 0.0f, Color{255, 255, 255, 170});
        } else {
            const Color tint =
                zone.team == Constants::kTeamRed ? Color{255, 100, 100, 96} : Color{120, 150, 255, 96};
            DrawRectangleRec(dst, tint);
        }
    }

    for (const auto& effect : state_.composite_effects) {
        if (!effect.alive) {
            continue;
        }
        const CompositeEffectDefinition* definition = composite_effects_loader_.FindById(effect.effect_id);
        if (definition == nullptr) {
            continue;
        }
        for (const auto& part : definition->parts) {
            if (part.layer != CompositeEffectLayer::Ground) {
                continue;
            }
            const SpriteMetadataLoader* metadata = get_metadata(part.sheet);
            const Texture2D* draw_texture = get_texture(part.sheet);
            if (metadata == nullptr || draw_texture == nullptr || !metadata->HasAnimation(part.animation)) {
                continue;
            }

            const Vector2 part_center = {
                effect.origin_world.x + part.offset_x_tiles * static_cast<float>(state_.map.cell_size),
                effect.origin_world.y + 0.5f * static_cast<float>(state_.map.cell_size) +
                    part.offset_y_tiles * static_cast<float>(state_.map.cell_size),
            };
            const Rectangle src =
                InsetSourceRect(metadata->GetFrame(part.animation, "default", effect.age_seconds),
                                Constants::kAtlasSampleInsetPixels);
            const float dst_w = static_cast<float>(metadata->GetCellWidth());
            const float dst_h = static_cast<float>(metadata->GetCellHeight());
            Rectangle dst = {part_center.x - dst_w * 0.5f, part_center.y - dst_h * 0.5f, dst_w, dst_h};
            DrawTexturePro(*draw_texture, src, SnapRect(dst), {0, 0}, 0.0f, WHITE);
        }
    }

    const CompositeEffectDefinition* fire_storm_cast_definition = composite_effects_loader_.FindById("fire_storm_cast");
    if (fire_storm_cast_definition != nullptr) {
        for (const auto& cast : state_.fire_storm_casts) {
            if (!cast.alive) {
                continue;
            }
            const Vector2 origin = CellToWorldCenter(cast.center_cell);
            for (const auto& part : fire_storm_cast_definition->parts) {
                if (part.layer != CompositeEffectLayer::Ground) {
                    continue;
                }
                const SpriteMetadataLoader* metadata = get_metadata(part.sheet);
                const Texture2D* draw_texture = get_texture(part.sheet);
                if (metadata == nullptr || draw_texture == nullptr || !metadata->HasAnimation(part.animation)) {
                    continue;
                }
                const Vector2 part_center = {
                    origin.x + part.offset_x_tiles * static_cast<float>(state_.map.cell_size),
                    origin.y + 0.5f * static_cast<float>(state_.map.cell_size) +
                        part.offset_y_tiles * static_cast<float>(state_.map.cell_size),
                };
                const Rectangle src = InsetSourceRect(
                    metadata->GetFrame(part.animation, "default", cast.elapsed_seconds),
                    Constants::kAtlasSampleInsetPixels);
                const float dst_w = static_cast<float>(metadata->GetCellWidth());
                const float dst_h = static_cast<float>(metadata->GetCellHeight());
                Rectangle dst = {part_center.x - dst_w * 0.5f, part_center.y - dst_h * 0.5f, dst_w, dst_h};
                DrawTexturePro(*draw_texture, src, SnapRect(dst), {0, 0}, 0.0f, WHITE);
            }
        }
    }

    for (const auto& dummy : state_.fire_storm_dummies) {
        if (!dummy.alive) {
            continue;
        }
        const char* effect_id = dummy.state == FireStormDummyState::Born
                                    ? "fire_storm_born"
                                    : (dummy.state == FireStormDummyState::Dying ? "fire_storm_death" : "fire_storm_idle");
        const CompositeEffectDefinition* definition = composite_effects_loader_.FindById(effect_id);
        if (definition == nullptr) {
            continue;
        }
        for (const auto& part : definition->parts) {
            if (part.layer != CompositeEffectLayer::Ground) {
                continue;
            }
            const SpriteMetadataLoader* metadata = get_metadata(part.sheet);
            const Texture2D* draw_texture = get_texture(part.sheet);
            if (metadata == nullptr || draw_texture == nullptr || !metadata->HasAnimation(part.animation)) {
                continue;
            }
            const Vector2 origin = CellToWorldCenter(dummy.cell);
            const Vector2 part_center = {
                origin.x + part.offset_x_tiles * static_cast<float>(state_.map.cell_size),
                origin.y + 0.5f * static_cast<float>(state_.map.cell_size) +
                    part.offset_y_tiles * static_cast<float>(state_.map.cell_size),
            };
            const Rectangle src = InsetSourceRect(
                metadata->GetFrame(part.animation, "default", dummy.state_time),
                Constants::kAtlasSampleInsetPixels);
            const float dst_w = static_cast<float>(metadata->GetCellWidth());
            const float dst_h = static_cast<float>(metadata->GetCellHeight());
            Rectangle dst = {part_center.x - dst_w * 0.5f, part_center.y - dst_h * 0.5f, dst_w, dst_h};
            DrawTexturePro(*draw_texture, src, SnapRect(dst), {0, 0}, 0.0f, WHITE);
        }
    }

    for (const auto& player : state_.players) {
        if (!player.alive) {
            continue;
        }
        const Vector2 origin = GetRenderPlayerPosition(player.id);
        for (const auto& status : player.status_effects) {
            if (status.composite_effect_id.empty()) {
                continue;
            }
            const CompositeEffectDefinition* definition = composite_effects_loader_.FindById(status.composite_effect_id);
            if (definition == nullptr) {
                continue;
            }
            for (const auto& part : definition->parts) {
                if (part.layer != CompositeEffectLayer::Ground) {
                    continue;
                }
                const SpriteMetadataLoader* metadata = get_metadata(part.sheet);
                const Texture2D* draw_texture = get_texture(part.sheet);
                if (metadata == nullptr || draw_texture == nullptr || !metadata->HasAnimation(part.animation)) {
                    continue;
                }
                const Vector2 part_center = {
                    origin.x + part.offset_x_tiles * static_cast<float>(state_.map.cell_size),
                    origin.y + 0.5f * static_cast<float>(state_.map.cell_size) +
                        part.offset_y_tiles * static_cast<float>(state_.map.cell_size),
                };
                const float animation_time = status.total_seconds - status.remaining_seconds;
                const Rectangle src = InsetSourceRect(
                    metadata->GetFrame(part.animation, "default", animation_time),
                    Constants::kAtlasSampleInsetPixels);
                const float dst_w = static_cast<float>(metadata->GetCellWidth());
                const float dst_h = static_cast<float>(metadata->GetCellHeight());
                Rectangle dst = {part_center.x - dst_w * 0.5f, part_center.y - dst_h * 0.5f, dst_w, dst_h};
                DrawTexturePro(*draw_texture, src, SnapRect(dst), {0, 0}, 0.0f, WHITE);
            }
        }
    }
}

void GameApp::RenderRunes() {
    const bool has_texture = sprite_metadata_.IsLoaded();
    const Texture2D texture = sprite_metadata_.GetTexture();

    for (const auto& rune : state_.runes) {
        if (!rune.active) {
            continue;
        }

        const Vector2 center = CellToWorldCenter(rune.cell);
        const char* animation_name = GetRuneSpriteAnimationKey(rune.rune_type);

        if (has_texture && sprite_metadata_.HasAnimation(animation_name)) {
            const Rectangle src = InsetSourceRect(
                sprite_metadata_.GetFrame(animation_name, "default", render_time_seconds_ + rune.placement_order * 0.05f),
                Constants::kAtlasSampleInsetPixels);
            Rectangle dst = {center.x - 16.0f, center.y - 16.0f, 32.0f, 32.0f};
            dst = SnapRect(dst);
            DrawTexturePro(texture, src, dst, {0, 0}, 0.0f, WHITE);
        } else {
            DrawCircleV(center, 8.0f, GetRuneFallbackColor(rune.rune_type));
            DrawCircleLinesV(center, 8.0f, Color{20, 24, 32, 255});
        }
    }
}

void GameApp::RenderIceWalls() {
    const bool has_texture = sprite_metadata_.IsLoaded();
    const Texture2D texture = sprite_metadata_.GetTexture();

    for (const auto& wall : state_.ice_walls) {
        if (!wall.alive) {
            continue;
        }

        const Vector2 center = CellToWorldCenter(wall.cell);
        Rectangle dst = {center.x - 16.0f, center.y - 16.0f, 32.0f, 32.0f};
        dst = SnapRect(dst);

        const char* animation_name = "ice_wall_idle";
        float animation_time = render_time_seconds_;
        if (wall.state == IceWallState::Materializing) {
            animation_name = "ice_wall_born";
            animation_time = wall.state_time;
        } else if (wall.state == IceWallState::Dying) {
            animation_name = "ice_wall_death";
            animation_time = wall.state_time;
        }

        if (has_texture && sprite_metadata_.HasAnimation(animation_name)) {
            const Rectangle src = InsetSourceRect(sprite_metadata_.GetFrame(animation_name, "default", animation_time),
                                                  Constants::kAtlasSampleInsetPixels);
            DrawTexturePro(texture, src, dst, {0, 0}, 0.0f, WHITE);
        } else {
            const Color fallback =
                (wall.state == IceWallState::Dying) ? Color{140, 180, 230, 180} : Color{180, 220, 255, 255};
            DrawRectangleRec(dst, fallback);
            DrawRectangleLinesEx(dst, 1.0f, Color{60, 95, 140, 255});
        }
    }
}

void GameApp::RenderPlayers() {
    const bool has_texture = sprite_metadata_.IsLoaded();

    for (const auto& player : state_.players) {
        const Vector2 draw_pos = GetRenderPlayerPosition(player.id);

        if (!player.alive) {
            DrawCircleV(draw_pos, player.radius, Color{70, 70, 70, 180});
            continue;
        }

        const bool is_moving = Vector2LengthSqr(player.vel) > 8.0f;
        const char* base_prefix = player.team == Constants::kTeamRed ? "wizard_red_" : "wizard_blue_";
        const char* suffix = "idle";
        if (player.action_state == PlayerActionState::Slashing) {
            suffix = "slash";
        } else if (player.action_state == PlayerActionState::RunePlacing) {
            suffix = "create_rune";
        } else if (is_moving) {
            suffix = "walking";
        }
        std::string animation = std::string(base_prefix) + suffix;
        if (!sprite_metadata_.HasAnimation(animation)) {
            animation = std::string("wizard_") + suffix;
        }

        const char* facing = FacingToSpriteFacing(player.facing);
        Rectangle dst = {draw_pos.x - 16.0f, draw_pos.y - 16.0f, 32.0f, 32.0f};
        dst = SnapRect(dst);
        const Rectangle sprite_rect = dst;

        if (has_texture && sprite_metadata_.HasAnimation(animation)) {
            const bool mirror = player.facing == FacingDirection::Left;
            Rectangle src = sprite_metadata_.GetFrame(animation, facing, render_time_seconds_);
            if (mirror) {
                src = sprite_metadata_.GetMirroredFrameRect(src);
            }
            src = InsetSourceRect(src, Constants::kAtlasSampleInsetPixels);

            const Texture2D draw_texture = sprite_metadata_.GetTexture(mirror);
            DrawTexturePro(draw_texture, src, dst, {0, 0}, 0.0f, WHITE);
        } else {
            const Color fallback = (player.team == Constants::kTeamRed) ? RED : BLUE;
            DrawCircleV(draw_pos, player.radius, fallback);
        }

        const Color bar_fill = {static_cast<unsigned char>(Constants::kPlayerHealthBarFillR),
                                static_cast<unsigned char>(Constants::kPlayerHealthBarFillG),
                                static_cast<unsigned char>(Constants::kPlayerHealthBarFillB), 255};
        const Color bar_missing = {static_cast<unsigned char>(Constants::kPlayerHealthBarMissingR),
                                   static_cast<unsigned char>(Constants::kPlayerHealthBarMissingG),
                                   static_cast<unsigned char>(Constants::kPlayerHealthBarMissingB), 255};

        Rectangle health_bar = {sprite_rect.x, sprite_rect.y - Constants::kPlayerHealthBarOffsetY, Constants::kPlayerHealthBarWidth,
                                Constants::kPlayerHealthBarHeight};
        health_bar = SnapRect(health_bar);
        DrawRectangleRec(health_bar, bar_missing);

        const float hp_ratio = std::clamp(static_cast<float>(player.hp) / static_cast<float>(Constants::kMaxHp), 0.0f, 1.0f);
        if (hp_ratio > 0.0f) {
            Rectangle fill = health_bar;
            fill.width = std::roundf(fill.width * hp_ratio);
            if (fill.width > 0.0f) {
                DrawRectangleRec(fill, bar_fill);
            }
        }
        DrawRectangleLinesEx(health_bar, 10.0f, BLACK);

        const int font_size = Constants::kPlayerHealthTextFontSize;
        const char* hp_text = TextFormat("%d", std::max(0, player.hp));
        const int text_width = MeasureText(hp_text, font_size);
        const int text_x = static_cast<int>(health_bar.x + (health_bar.width - static_cast<float>(text_width)) * 0.5f);
        const int text_y =
            static_cast<int>(health_bar.y + (health_bar.height - static_cast<float>(font_size)) * 0.5f);
        DrawText(hp_text, text_x, text_y, font_size, BLACK);

        if (player.id != state_.local_player_id && !player.name.empty()) {
            const int name_font_size = 8;
            const int name_width = MeasureText(player.name.c_str(), name_font_size);
            const int name_x = static_cast<int>(health_bar.x + (health_bar.width - static_cast<float>(name_width)) * 0.5f);
            const int name_y = static_cast<int>(health_bar.y - static_cast<float>(name_font_size) - 2.0f);
            DrawText(player.name.c_str(), name_x, name_y, name_font_size, WHITE);
        }
    }
}

void GameApp::RenderMeleeAttacks() {
    if (!sprite_metadata_.IsLoaded() || !sprite_metadata_.HasAnimation("melee_attack")) {
        return;
    }

    const Texture2D texture = sprite_metadata_.GetTexture();
    for (const auto& player : state_.players) {
        if (!player.alive || player.melee_active_remaining <= 0.0f) {
            continue;
        }

        const float attack_elapsed = Constants::kMeleeActiveWindowSeconds - player.melee_active_remaining;
        const Vector2 draw_pos = GetRenderPlayerPosition(player.id);
        const float rotation = AimToDegrees(player.aim_dir);

        const Rectangle src =
            InsetSourceRect(sprite_metadata_.GetFrame("melee_attack", "default", attack_elapsed),
                            Constants::kAtlasSampleInsetPixels);

        const float visual_size = 32.0f * Constants::kMeleeAttackVisualScale;
        Rectangle dst = {draw_pos.x, draw_pos.y, visual_size, visual_size};
        dst = SnapRect(dst);

        // Rotate around player centroid and keep slash offset one cell to the east at 0 degrees.
        const Vector2 origin = {visual_size * 0.5f - Constants::kMeleeRange, visual_size * 0.5f};
        DrawTexturePro(texture, src, dst, origin, rotation, WHITE);
    }
}

void GameApp::RenderProjectiles() {
    const bool has_texture = sprite_metadata_.IsLoaded();
    const Texture2D texture = sprite_metadata_.GetTexture();
    const bool has_large_texture = sprite_metadata_96x96_.IsLoaded();
    const Texture2D large_texture = sprite_metadata_96x96_.GetTexture();

    for (const auto& projectile : state_.projectiles) {
        if (!projectile.alive) {
            continue;
        }

        const bool use_large_static = projectile.animation_key == "fire_storm_static_large";
        const SpriteMetadataLoader* projectile_metadata =
            use_large_static ? (has_large_texture ? &sprite_metadata_96x96_ : nullptr)
                             : (has_texture ? &sprite_metadata_ : nullptr);
        const Texture2D* projectile_texture =
            use_large_static ? (has_large_texture ? &large_texture : nullptr) : (has_texture ? &texture : nullptr);
        if (projectile_metadata != nullptr && projectile_texture != nullptr &&
            projectile_metadata->HasAnimation(projectile.animation_key)) {
            const Rectangle src = InsetSourceRect(
                projectile_metadata->GetFrame(projectile.animation_key, "default", render_time_seconds_),
                Constants::kAtlasSampleInsetPixels);
            const float dst_w = use_large_static ? static_cast<float>(projectile_metadata->GetCellWidth())
                                                 : projectile.radius * 8.0f;
            const float dst_h = use_large_static ? static_cast<float>(projectile_metadata->GetCellHeight())
                                                 : projectile.radius * 8.0f;
            Rectangle dst = use_large_static
                                ? Rectangle{projectile.pos.x, projectile.pos.y, dst_w, dst_h}
                                : Rectangle{projectile.pos.x - dst_w * 0.5f, projectile.pos.y - dst_h * 0.5f, dst_w,
                                            dst_h};
            dst = SnapRect(dst);
            const Color tint = IsStaticFireBolt(projectile)
                                   ? WHITE
                                   : (projectile.owner_team == Constants::kTeamRed ? Color{255, 215, 215, 255}
                                                                                   : Color{215, 230, 255, 255});
            const Vector2 rotation_vel =
                use_large_static && projectile.upgrade_pause_remaining > 0.0f ? projectile.resume_vel : projectile.vel;
            const float rotation = use_large_static ? AimToDegrees(rotation_vel) : 0.0f;
            const Vector2 origin = use_large_static ? Vector2{dst_w * 0.5f, dst_h * 0.5f} : Vector2{0.0f, 0.0f};
            DrawTexturePro(*projectile_texture, src, dst, origin, rotation, tint);
        } else {
            DrawCircleV(projectile.pos, projectile.radius,
                        projectile.owner_team == Constants::kTeamRed ? Color{255, 92, 48, 255}
                                                                     : Color{80, 180, 255, 255});
        }
    }
}

void GameApp::RenderParticles() {
    const bool has_texture = sprite_metadata_.IsLoaded();
    const Texture2D texture = sprite_metadata_.GetTexture();

    for (const auto& particle : state_.particles) {
        if (!particle.alive) {
            continue;
        }

        if (has_texture && sprite_metadata_.HasAnimation(particle.animation_key)) {
            const Rectangle src =
                InsetSourceRect(sprite_metadata_.GetFrame(particle.animation_key, particle.facing, particle.age_seconds),
                                Constants::kAtlasSampleInsetPixels);
            const float particle_scale = particle.animation_key == "static_upgrade" ? 1.5f : 1.0f;
            const float particle_size = 32.0f * particle_scale;
            Rectangle dst = {particle.pos.x - particle_size * 0.5f, particle.pos.y - particle_size * 0.5f,
                             particle_size, particle_size};
            dst = SnapRect(dst);
            const unsigned char alpha = particle.animation_key == "explosion" ? 255 : 200;
            DrawTexturePro(texture, src, dst, {0, 0}, 0.0f, Color{255, 255, 255, alpha});
        } else {
            DrawCircleV(particle.pos, 4.0f, Color{190, 190, 190, 160});
        }
    }
}

void GameApp::RenderDamagePopups() {
    for (const auto& popup : state_.damage_popups) {
        if (!popup.alive) {
            continue;
        }

        const float ratio = std::clamp(1.0f - popup.age_seconds / std::max(0.001f, popup.lifetime_seconds), 0.0f, 1.0f);
        const unsigned char alpha = static_cast<unsigned char>(255.0f * ratio);
        const char* text = popup.is_heal ? TextFormat("+%d", popup.amount) : TextFormat("-%d", popup.amount);
        const int font_size = Constants::kDamagePopupFontSize;
        const int text_w = MeasureText(text, font_size);
        const Vector2 draw_pos = {std::roundf(popup.pos.x - static_cast<float>(text_w) * 0.5f), std::roundf(popup.pos.y)};
        const Color tint = popup.is_heal ? Color{90, 230, 120, alpha} : Color{255, 50, 50, alpha};
        DrawText(text, static_cast<int>(draw_pos.x), static_cast<int>(draw_pos.y), font_size,
                 tint);
    }
}

void GameApp::RenderRunePlacementOverlay() {
    const Player* local_player = FindPlayerById(state_.local_player_id);
    if (!local_player || !local_player->rune_placing_mode) {
        return;
    }

    const GridCoord mouse_cell = WorldToCell(GetScreenToWorld2D(GetMousePosition(), camera_));

    for (int y = mouse_cell.y - Constants::kPlacementPreviewRadiusCells;
         y <= mouse_cell.y + Constants::kPlacementPreviewRadiusCells; ++y) {
        for (int x = mouse_cell.x - Constants::kPlacementPreviewRadiusCells;
             x <= mouse_cell.x + Constants::kPlacementPreviewRadiusCells; ++x) {
            const GridCoord cell = {x, y};
            if (!state_.map.IsInside(cell)) {
                continue;
            }
            if (!IsTileRunePlaceable(cell)) {
                continue;
            }
            if (Vector2Distance(local_player->pos, CellToWorldCenter(cell)) > Constants::kRuneCastRangeWorld) {
                continue;
            }

            const Rectangle dst = {static_cast<float>(x * state_.map.cell_size), static_cast<float>(y * state_.map.cell_size),
                                   static_cast<float>(state_.map.cell_size), static_cast<float>(state_.map.cell_size)};

            if (sprite_metadata_.IsLoaded() && sprite_metadata_.HasAnimation("grid_overlay")) {
                const Rectangle src =
                    InsetSourceRect(sprite_metadata_.GetFrame("grid_overlay", "default", render_time_seconds_),
                                    Constants::kAtlasSampleInsetPixels);
                DrawTexturePro(sprite_metadata_.GetTexture(), src, SnapRect(dst), {0, 0}, 0.0f,
                               Color{255, 255, 255, 128});
            } else {
                DrawRectangleLinesEx(dst, 1.0f, Color{190, 205, 255, 130});
            }
        }
    }

    if (!state_.map.IsInside(mouse_cell)) {
        return;
    }

    const Vector2 center = CellToWorldCenter(mouse_cell);
    const bool in_range = Vector2Distance(local_player->pos, center) <= Constants::kRuneCastRangeWorld;
    const bool valid = in_range && !IsCellOccupiedByRune(mouse_cell) && !IsCellOccupiedByFireStormDummy(mouse_cell) &&
                       IsTileRunePlaceable(mouse_cell);
    const char* animation_name = GetRuneSpriteAnimationKey(local_player->selected_rune_type);
    if (sprite_metadata_.IsLoaded() && sprite_metadata_.HasAnimation(animation_name)) {
        const Rectangle src =
            InsetSourceRect(sprite_metadata_.GetFrame(animation_name, "default", render_time_seconds_),
                            Constants::kAtlasSampleInsetPixels);
        Rectangle dst = {center.x - 16.0f, center.y - 16.0f, 32.0f, 32.0f};
        dst = SnapRect(dst);
        const Color tint = valid ? Color{255, 255, 255, 160} : Color{255, 96, 96, 160};
        DrawTexturePro(sprite_metadata_.GetTexture(), src, dst, {0, 0}, 0.0f, tint);
    } else {
        Color preview = GetRuneFallbackColor(local_player->selected_rune_type);
        preview.a = 160;
        if (!valid) {
            preview = Color{220, 60, 60, 160};
        }
        DrawCircleV(center, 10.0f, preview);
        DrawCircleLinesV(center, 10.0f, Color{20, 26, 32, 255});
    }
}

void GameApp::RenderBottomHud() {
    Player* local_player = FindPlayerById(state_.local_player_id);
    if (!local_player) {
        return;
    }
    const float scale = Constants::kHudScale;
    const float panel_width = Constants::kHudPanelWidth * scale;
    const float panel_height = (Constants::kHudPanelHeight - 28.0f) * scale;
    const float hud_x = (static_cast<float>(GetScreenWidth()) - panel_width) * 0.5f;
    const float hud_y = static_cast<float>(GetScreenHeight()) - panel_height - 4.0f;
    const Rectangle panel = {hud_x, hud_y, panel_width, panel_height};
    DrawRectangleRec(panel, Color{static_cast<unsigned char>(Constants::kHudBgR),
                                  static_cast<unsigned char>(Constants::kHudBgG),
                                  static_cast<unsigned char>(Constants::kHudBgB),
                                  static_cast<unsigned char>(Constants::kHudBgA)});
    DrawRectangleLinesEx(panel, std::max(1.0f, scale), Color{70, 82, 104, 255});

    auto draw_bar = [&](float x, float y, float width, float height, float value, float max_value, Color fill_color,
                        const char* label) {
        const Rectangle bg = {x, y, width, height};
        DrawRectangleRec(bg, Color{66, 72, 82, 255});
        const float ratio = (max_value > 0.0f) ? std::clamp(value / max_value, 0.0f, 1.0f) : 0.0f;
        Rectangle fill = bg;
        fill.width = std::roundf(fill.width * ratio);
        if (fill.width > 0.0f) {
            DrawRectangleRec(fill, fill_color);
        }
        DrawRectangleLinesEx(bg, std::max(1.0f, scale * 0.6f), Color{16, 18, 24, 255});
        const int font_size = static_cast<int>(std::roundf(8.0f * scale));
        const int text_width = MeasureText(label, font_size);
        const int text_x = static_cast<int>(x + (width - static_cast<float>(text_width)) * 0.5f);
        const int text_y = static_cast<int>(y + (height - static_cast<float>(font_size)) * 0.5f);
        DrawText(label, text_x, text_y, font_size, BLACK);
    };

    draw_bar(hud_x + 12.0f * scale, hud_y + 6.0f * scale, panel_width - 24.0f * scale,
             Constants::kHudBarHeight * scale,
             static_cast<float>(local_player->hp), static_cast<float>(Constants::kMaxHp),
             Color{static_cast<unsigned char>(Constants::kPlayerHealthBarFillR),
                   static_cast<unsigned char>(Constants::kPlayerHealthBarFillG),
                   static_cast<unsigned char>(Constants::kPlayerHealthBarFillB), 255},
             TextFormat("%d / %d", local_player->hp, Constants::kMaxHp));
    draw_bar(hud_x + 12.0f * scale, hud_y + 18.0f * scale, panel_width - 24.0f * scale,
             Constants::kHudBarHeight * scale,
             local_player->mana, local_player->max_mana,
             Color{static_cast<unsigned char>(Constants::kHudManaBarR),
                   static_cast<unsigned char>(Constants::kHudManaBarG),
                   static_cast<unsigned char>(Constants::kHudManaBarB), 255},
             TextFormat("%d / %d", static_cast<int>(std::roundf(local_player->mana)),
                        static_cast<int>(std::roundf(local_player->max_mana))));

    const std::vector<const StatusEffectInstance*> active_statuses = [&]() {
        std::vector<const StatusEffectInstance*> result;
        result.reserve(local_player->status_effects.size());
        const StatusEffectInstance* rooted_dominant = nullptr;
        for (const auto& status : local_player->status_effects) {
            if (status.remaining_seconds <= 0.0f || !status.visible) {
                continue;
            }
            if (status.type == StatusEffectType::Rooted) {
                if (rooted_dominant == nullptr ||
                    status.movement_speed_multiplier < rooted_dominant->movement_speed_multiplier - 0.0001f ||
                    (std::fabs(status.movement_speed_multiplier - rooted_dominant->movement_speed_multiplier) <= 0.0001f &&
                     status.remaining_seconds > rooted_dominant->remaining_seconds)) {
                    rooted_dominant = &status;
                }
                continue;
            }
            result.push_back(&status);
        }
        if (rooted_dominant != nullptr) {
            result.push_back(rooted_dominant);
        }
        return result;
    }();
    if (!active_statuses.empty()) {
        const float icon_diameter = 21.0f * scale;
        const float icon_spacing = 5.0f * scale;
        const float total_width =
            static_cast<float>(active_statuses.size()) * icon_diameter +
            static_cast<float>(std::max<int>(0, static_cast<int>(active_statuses.size()) - 1)) * icon_spacing;
        const float start_x = hud_x + (panel_width - total_width) * 0.5f;
        const float center_y = hud_y - 13.0f * scale;
        const Color badge_fill = Color{28, 32, 38, 235};
        const Color buff_color = Color{92, 214, 112, 255};
        const Color debuff_color = Color{224, 84, 84, 255};
        for (size_t i = 0; i < active_statuses.size(); ++i) {
            const StatusEffectInstance& status = *active_statuses[i];
            const StatusEffectUiSpec spec = GetStatusEffectUiSpec(status.type);
            const Vector2 center = {start_x + icon_diameter * 0.5f + static_cast<float>(i) * (icon_diameter + icon_spacing),
                                    center_y};
            const float radius = icon_diameter * 0.5f;
            DrawCircleV(center, radius, badge_fill);

            if (sprite_metadata_.IsLoaded() && spec.animation != nullptr && sprite_metadata_.HasAnimation(spec.animation)) {
                const float animation_time = render_time_seconds_;
                const Rectangle src =
                    InsetSourceRect(sprite_metadata_.GetFrame(spec.animation, "default", animation_time),
                                    Constants::kAtlasSampleInsetPixels);
                const float icon_size = icon_diameter * 1.44f;
                Rectangle icon_dst = {center.x - icon_size * 0.5f, center.y - icon_size * 0.5f, icon_size, icon_size};
                DrawTexturePro(sprite_metadata_.GetTexture(), src, SnapRect(icon_dst), {0, 0}, 0.0f, WHITE);
            } else {
                DrawCircleV(center, radius * 0.45f, Color{180, 184, 196, 255});
            }

            const float ratio =
                (status.total_seconds > 0.0f) ? std::clamp(status.remaining_seconds / status.total_seconds, 0.0f, 1.0f) : 0.0f;
            if (ratio > 0.0f) {
                const float start_angle = -90.0f;
                const float end_angle = start_angle + 360.0f * ratio;
                const Color ring_color = status.is_buff ? buff_color : debuff_color;
                DrawRing(center, radius - 2.5f * scale, radius, start_angle, end_angle, 48, ring_color);
            }
            DrawCircleLinesV(center, radius, BLACK);
        }
    }

    const Vector2 mouse = GetMousePosition();
    const float slot_row_height = Constants::kHudSlotSize * scale;
    const float slot_y = hud_y + panel_height - slot_row_height - 4.0f * scale;
    const float slot_spacing = 4.0f * scale;
    const int total_slots = Constants::kHudTotalSlotCount;
    for (int i = 0; i < total_slots; ++i) {
        const float slot_x = hud_x + 12.0f * scale + static_cast<float>(i) * (Constants::kHudSlotSize * scale + slot_spacing);
        const Rectangle slot = {slot_x, slot_y, Constants::kHudSlotSize * scale, Constants::kHudSlotSize * scale};
        const bool is_rune_slot = i < Constants::kHudRuneSlotCount;
        const bool is_item_slot = i >= Constants::kHudRuneSlotCount &&
                                  i < Constants::kHudRuneSlotCount + Constants::kHudItemSlotCount;
        const bool is_weapon_slot = i >= Constants::kHudRuneSlotCount + Constants::kHudItemSlotCount;
        const int slot_index = is_rune_slot ? i : (is_item_slot ? (i - Constants::kHudRuneSlotCount)
                                                                 : (i - Constants::kHudRuneSlotCount - Constants::kHudItemSlotCount));
        const bool hovered = CheckCollisionPointRec(mouse, slot);
        const int slot_font_size = static_cast<int>(std::roundf(8.0f * scale));
        const int binding_code = is_rune_slot
                                     ? (slot_index == 0   ? controls_bindings_.select_rune_slot_1
                                        : slot_index == 1 ? controls_bindings_.select_rune_slot_2
                                        : slot_index == 2 ? controls_bindings_.select_rune_slot_3
                                                          : controls_bindings_.select_rune_slot_4)
                                     : (is_item_slot
                                            ? (slot_index == 0   ? controls_bindings_.activate_item_slot_1
                                               : slot_index == 1 ? controls_bindings_.activate_item_slot_2
                                               : slot_index == 2 ? controls_bindings_.activate_item_slot_3
                                                                 : controls_bindings_.activate_item_slot_4)
                                            : (slot_index == 0 ? controls_bindings_.primary_action
                                                               : controls_bindings_.grappling_hook_action));
        const std::string slot_label_text = BindingToString(binding_code);
        const char* slot_label = slot_label_text.c_str();
        const int slot_label_width = MeasureText(slot_label, slot_font_size);
        DrawText(slot_label,
                 static_cast<int>(slot_x + (slot.width - static_cast<float>(slot_label_width)) * 0.5f),
                 static_cast<int>(slot_y + 3.0f * scale),
                 slot_font_size,
                 hovered ? Color{210, 214, 222, 255} : Color{180, 184, 196, 255});

        if (!is_weapon_slot && local_player->inventory_mode && hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            if (!local_player->ui_dragging_slot) {
                const bool has_content =
                    is_rune_slot ? (local_player->rune_slots[slot_index] != RuneType::None)
                                 : (!local_player->item_slots[slot_index].empty() && local_player->item_slot_counts[slot_index] > 0);
                if (has_content) {
                    local_player->ui_dragging_slot = true;
                    local_player->ui_drag_source_family = is_rune_slot ? SlotFamily::Rune : SlotFamily::Item;
                    local_player->ui_drag_source_index = slot_index;
                }
            } else if (local_player->ui_drag_source_family == (is_rune_slot ? SlotFamily::Rune : SlotFamily::Item)) {
                const int source_index = local_player->ui_drag_source_index;
                if (source_index >= 0 && source_index != slot_index) {
                    if (is_rune_slot) {
                        std::swap(local_player->rune_slots[source_index], local_player->rune_slots[slot_index]);
                        if (local_player->selected_rune_slot == source_index) {
                            local_player->selected_rune_slot = slot_index;
                        } else if (local_player->selected_rune_slot == slot_index) {
                            local_player->selected_rune_slot = source_index;
                        }
                        local_player->selected_rune_type = local_player->rune_slots[local_player->selected_rune_slot];
                    } else {
                        std::swap(local_player->item_slots[source_index], local_player->item_slots[slot_index]);
                        std::swap(local_player->item_slot_counts[source_index], local_player->item_slot_counts[slot_index]);
                        std::swap(local_player->item_slot_cooldown_remaining[source_index],
                                  local_player->item_slot_cooldown_remaining[slot_index]);
                        std::swap(local_player->item_slot_cooldown_total[source_index],
                                  local_player->item_slot_cooldown_total[slot_index]);
                    }
                }
                local_player->ui_dragging_slot = false;
                local_player->ui_drag_source_index = -1;
            }
        }

        if (is_rune_slot) {
            const RuneType rune_type = local_player->rune_slots[slot_index];
            const char* animation = GetRuneSpriteAnimationKey(rune_type);
            if (rune_type != RuneType::None && sprite_metadata_.IsLoaded() && sprite_metadata_.HasAnimation(animation)) {
                const Rectangle src =
                    InsetSourceRect(sprite_metadata_.GetFrame(animation, "default", render_time_seconds_),
                                    Constants::kAtlasSampleInsetPixels);
                const bool low_mana = local_player->mana < GetRuneManaCost(rune_type);
                const Color tint =
                    low_mana ? Color{static_cast<unsigned char>(Constants::kHudLowManaTintR),
                                     static_cast<unsigned char>(Constants::kHudLowManaTintG),
                                     static_cast<unsigned char>(Constants::kHudLowManaTintB),
                                     static_cast<unsigned char>(255.0f * Constants::kHudLowManaBrightness)}
                             : WHITE;
                const float base_icon_size = std::min(slot.width - 8.0f * scale, slot.height - 18.0f * scale);
                const float icon_size =
                    std::min({slot.width - 2.0f * scale, slot.height - 10.0f * scale, base_icon_size * 1.2f});
                Rectangle icon_dst = {slot.x + (slot.width - icon_size) * 0.5f, slot.y + 9.0f * scale, icon_size,
                                      icon_size};
                DrawTexturePro(sprite_metadata_.GetTexture(), src, SnapRect(icon_dst), {0, 0}, 0.0f, tint);
            }

            const float remaining = GetPlayerRuneCooldownRemaining(*local_player, rune_type);
            const float total = std::max(0.001f, GetPlayerRuneCooldownTotal(*local_player, rune_type));
            if (remaining > 0.0f) {
                const float ratio = std::clamp(remaining / total, 0.0f, 1.0f);
                DrawCircleSector({slot.x + slot.width * 0.5f, slot.y + slot.height * 0.5f}, slot.width * 0.45f, -90.0f,
                                 -90.0f + 360.0f * ratio, 32, Color{0, 0, 0, 150});
            }
            if (local_player->selected_rune_slot == slot_index) {
                const float underline_y = slot.y + slot.height - 2.0f * scale;
                DrawLineEx({slot.x + 4.0f * scale, underline_y}, {slot.x + slot.width - 4.0f * scale, underline_y},
                           std::max(2.0f, scale * 0.9f), Color{220, 228, 242, 255});
            }
        } else if (is_item_slot) {
            if (!local_player->item_slots[slot_index].empty()) {
                const ObjectPrototype* proto = FindObjectPrototype(local_player->item_slots[slot_index]);
                if (proto != nullptr && sprite_metadata_.IsLoaded() && sprite_metadata_.HasAnimation(proto->idle_animation)) {
                    const Rectangle src =
                        InsetSourceRect(sprite_metadata_.GetFrame(proto->idle_animation, "default", render_time_seconds_),
                                        Constants::kAtlasSampleInsetPixels);
                    const float base_icon_size = std::min(slot.width - 8.0f * scale, slot.height - 18.0f * scale);
                    const float icon_size =
                        std::min({slot.width - 2.0f * scale, slot.height - 10.0f * scale, base_icon_size * 1.2f});
                    Rectangle icon_dst = {slot.x + (slot.width - icon_size) * 0.5f, slot.y + 9.0f * scale, icon_size,
                                          icon_size};
                    DrawTexturePro(sprite_metadata_.GetTexture(), src, SnapRect(icon_dst), {0, 0}, 0.0f, WHITE);
                }
                DrawText(TextFormat("%d", local_player->item_slot_counts[slot_index]),
                         static_cast<int>(slot.x + slot.width - 12.0f * scale),
                         static_cast<int>(slot.y + slot.height - 12.0f * scale),
                         static_cast<int>(std::roundf(8.0f * scale)), WHITE);
            }
            const float remaining = local_player->item_slot_cooldown_remaining[slot_index];
            const float total = std::max(0.001f, local_player->item_slot_cooldown_total[slot_index]);
            if (remaining > 0.0f) {
                const float ratio = std::clamp(remaining / total, 0.0f, 1.0f);
                DrawCircleSector({slot.x + slot.width * 0.5f, slot.y + slot.height * 0.5f}, slot.width * 0.45f, -90.0f,
                                 -90.0f + 360.0f * ratio, 32, Color{0, 0, 0, 150});
            }
        } else if (is_weapon_slot) {
            const std::string& animation = local_player->weapon_slots[slot_index];
            if (!animation.empty() && sprite_metadata_.IsLoaded() && sprite_metadata_.HasAnimation(animation)) {
                const Rectangle src =
                    InsetSourceRect(sprite_metadata_.GetFrame(animation, "default", render_time_seconds_),
                                    Constants::kAtlasSampleInsetPixels);
                const float base_icon_size = std::min(slot.width - 8.0f * scale, slot.height - 18.0f * scale);
                const float icon_size =
                    std::min({slot.width - 2.0f * scale, slot.height - 10.0f * scale, base_icon_size * 1.2f});
                Rectangle icon_dst = {slot.x + (slot.width - icon_size) * 0.5f, slot.y + 9.0f * scale, icon_size,
                                      icon_size};
                DrawTexturePro(sprite_metadata_.GetTexture(), src, SnapRect(icon_dst), {0, 0}, 0.0f, WHITE);
            }
            const float remaining = slot_index == 0 ? local_player->melee_cooldown_remaining
                                                    : local_player->grappling_cooldown_remaining;
            const float total = std::max(0.001f, slot_index == 0 ? Constants::kMeleeCooldownSeconds
                                                                 : local_player->grappling_cooldown_total);
            if (remaining > 0.0f) {
                const float ratio = std::clamp(remaining / total, 0.0f, 1.0f);
                DrawCircleSector({slot.x + slot.width * 0.5f, slot.y + slot.height * 0.5f}, slot.width * 0.45f, -90.0f,
                                 -90.0f + 360.0f * ratio, 32, Color{0, 0, 0, 150});
            }
        }

        if (i == Constants::kHudRuneSlotCount - 1 || i == Constants::kHudRuneSlotCount + Constants::kHudItemSlotCount - 1) {
            const float divider_x = slot.x + slot.width + slot_spacing * 0.5f;
            DrawLineEx({divider_x, slot_y + 6.0f * scale},
                       {divider_x, slot_y + slot_row_height - 6.0f * scale},
                       std::max(1.0f, scale * 0.75f), Color{90, 100, 120, 255});
        }
    }
}

void GameApp::RenderNetworkDebugPanel() {
    if (!show_network_debug_panel_) {
        return;
    }

    const NetTelemetry& t = network_manager_.GetTelemetry();

    const float x = 12.0f;
    const float y = 12.0f;
    const float width = 408.0f;
    const float height = network_debug_panel_minimized_ ? 34.0f : 224.0f;
    const Rectangle panel = {x, y, width, height};

    DrawRectangleRec(panel, Color{20, 24, 30, 220});
    DrawRectangleLinesEx(panel, 1.0f, Color{70, 82, 104, 255});
    DrawText("Net Debug", static_cast<int>(x + 10.0f), static_cast<int>(y + 8.0f), 14, RAYWHITE);

    if (GuiButton({x + width - 28.0f, y + 6.0f, 20.0f, 20.0f}, network_debug_panel_minimized_ ? "+" : "-")) {
        network_debug_panel_minimized_ = !network_debug_panel_minimized_;
    }

    if (network_debug_panel_minimized_) {
        return;
    }

    const char* role = network_manager_.IsHost() ? "Host" : "Client";
    const std::string status_text = network_manager_.IsHost() ? "authoritative" : GetClientLobbyStatusText();

    int text_y = static_cast<int>(y + 34.0f);
    const int line_h = 16;
    DrawText(TextFormat("Role: %s  |  Status: %s", role, status_text.c_str()), static_cast<int>(x + 10.0f), text_y, 13,
             RAYWHITE);
    text_y += line_h;
    DrawText(TextFormat("Up: %.1f KB/s (%0.1f pkt/s)  Down: %.1f KB/s (%0.1f pkt/s)", t.bytes_per_sec_up / 1024.0f,
                        t.packets_per_sec_up, t.bytes_per_sec_down / 1024.0f, t.packets_per_sec_down),
             static_cast<int>(x + 10.0f), text_y, 13, Color{184, 210, 255, 255});
    text_y += line_h;
    DrawText(TextFormat("Snapshots tx/rx: %llu / %llu  avg bytes tx/rx: %.1f / %.1f",
                        static_cast<unsigned long long>(t.snapshots_sent_total),
                        static_cast<unsigned long long>(t.snapshots_received_total), t.average_snapshot_bytes_sent,
                        t.average_snapshot_bytes_received),
             static_cast<int>(x + 10.0f), text_y, 13, Color{184, 210, 255, 255});
    text_y += line_h;
    DrawText(TextFormat("Keyframe tx/rx: %llu / %llu  Delta tx/rx: %llu / %llu",
                        static_cast<unsigned long long>(t.keyframe_snapshots_sent_total),
                        static_cast<unsigned long long>(t.keyframe_snapshots_received_total),
                        static_cast<unsigned long long>(t.delta_snapshots_sent_total),
                        static_cast<unsigned long long>(t.delta_snapshots_received_total)),
             static_cast<int>(x + 10.0f), text_y, 13, Color{190, 226, 180, 255});
    text_y += line_h;
    DrawText(TextFormat("Dropped snapshots: %llu  Missing-base deltas: %llu",
                        static_cast<unsigned long long>(t.dropped_snapshots_total),
                        static_cast<unsigned long long>(t.dropped_delta_missing_base_total)),
             static_cast<int>(x + 10.0f), text_y, 13,
             (t.dropped_snapshots_total > 0 || t.dropped_delta_missing_base_total > 0) ? Color{255, 175, 130, 255}
                                                                                         : Color{186, 226, 186, 255});
    text_y += line_h;
    DrawText(TextFormat("Reconcile corrections: %.2f/s (total %llu)", t.reconciliation_corrections_per_sec,
                        static_cast<unsigned long long>(t.reconciliation_corrections_total)),
             static_cast<int>(x + 10.0f), text_y, 13, Color{235, 220, 185, 255});
    text_y += line_h;

    const char* hint_1 = "Tune: lower interpolation delay first (100ms -> 75 -> 50 on LAN).";
    const char* hint_2 = "If missing-base deltas rise, shorten keyframe interval.";
    if (t.reconciliation_corrections_per_sec > 8.0f) {
        hint_2 = "High reconcile rate: increase hard snap threshold or lower reconcile gain.";
    } else if (t.dropped_delta_missing_base_total == 0 && t.dropped_snapshots_total == 0) {
        hint_2 = "If still stuttery, raise snapshot rate or reduce render load.";
    }
    DrawText(hint_1, static_cast<int>(x + 10.0f), text_y, 12, Color{210, 214, 220, 255});
    text_y += 14;
    DrawText(hint_2, static_cast<int>(x + 10.0f), text_y, 12, Color{210, 214, 220, 255});
}

void GameApp::UpdateCameraTarget() {
    const Player* local_player = FindPlayerById(state_.local_player_id);
    if (!local_player && !state_.players.empty()) {
        local_player = &state_.players.front();
    }

    if (local_player) {
        const Vector2 render_pos = GetRenderPlayerPosition(local_player->id);
        camera_.target = {std::roundf(render_pos.x), std::roundf(render_pos.y)};
    }

    Vector2 base_offset = {std::roundf(static_cast<float>(GetScreenWidth()) * 0.5f),
                           std::roundf(static_cast<float>(GetScreenHeight()) * 0.5f)};

    if (camera_shake_time_remaining_ > 0.0f) {
        const float shake_alpha = std::clamp(camera_shake_time_remaining_ / Constants::kCameraShakeDurationSeconds, 0.0f,
                                             1.0f);
        const float amplitude = Constants::kCameraShakePixels * shake_alpha;
        std::uniform_real_distribution<float> shake_dist(-amplitude, amplitude);
        base_offset.x += shake_dist(rng_);
        base_offset.y += shake_dist(rng_);
    }

    camera_.offset = base_offset;
}

Vector2 GameApp::GetRenderPlayerPosition(int player_id) const {
    auto it = render_player_positions_.find(player_id);
    if (it != render_player_positions_.end()) {
        return it->second;
    }
    if (const Player* player = FindPlayerById(player_id)) {
        return player->pos;
    }
    return {0.0f, 0.0f};
}

std::string GameApp::GetClientLobbyStatusText() const {
    switch (network_manager_.GetClientConnectionState()) {
        case ClientConnectionState::ConnectingTransport:
            return "connecting transport...";
        case ClientConnectionState::WaitingJoinAck:
            return "connected; waiting join_ack...";
        case ClientConnectionState::WaitingLobbyState:
            return "join_ack received; waiting lobby_state...";
        case ClientConnectionState::ReadyInLobby:
            return "connected";
        case ClientConnectionState::Disconnected:
            return "disconnected";
        case ClientConnectionState::Idle:
        default:
            return "idle";
    }
}

FacingDirection GameApp::AimToFacing(Vector2 aim) {
    if (std::fabs(aim.x) > std::fabs(aim.y)) {
        return aim.x >= 0.0f ? FacingDirection::Right : FacingDirection::Left;
    }
    return aim.y >= 0.0f ? FacingDirection::Bottom : FacingDirection::Top;
}

const char* GameApp::FacingToSpriteFacing(FacingDirection facing) {
    switch (facing) {
        case FacingDirection::Top:
            return "top";
        case FacingDirection::Bottom:
            return "bot";
        case FacingDirection::Left:
        case FacingDirection::Right:
        default:
            return "side";
    }
}
