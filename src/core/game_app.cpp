#include "core/game_app.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <random>
#include <string>

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

bool IsHorizontalDirection(SpellDirection direction) {
    return direction == SpellDirection::Horizontal || direction == SpellDirection::Left ||
           direction == SpellDirection::Right;
}

float AimToDegrees(Vector2 aim_dir) {
    return std::atan2f(aim_dir.y, aim_dir.x) * (180.0f / PI);
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

}  // namespace

GameApp::GameApp(bool force_windowed_launch) : force_windowed_launch_(force_windowed_launch) {}

bool GameApp::Initialize() {
    const bool loaded_settings = config_manager_.Load(settings_);
    if (!loaded_settings) {
        settings_.fullscreen = true;
    }
    settings_.lobby_shrink_tiles_per_second =
        std::clamp(settings_.lobby_shrink_tiles_per_second, 0.0f, Constants::kMaxShrinkTilesPerSecond);
    settings_.lobby_min_arena_radius_tiles =
        std::clamp(settings_.lobby_min_arena_radius_tiles, 0.0f, Constants::kMaxArenaRadiusTiles);
    lobby_shrink_tiles_per_second_ = settings_.lobby_shrink_tiles_per_second;
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

    // Basic dark UI look.
    GuiSetStyle(DEFAULT, BACKGROUND_COLOR, ColorToInt(Color{25, 28, 34, 255}));
    GuiSetStyle(DEFAULT, TEXT_COLOR_NORMAL, ColorToInt(Color{221, 228, 245, 255}));
    GuiSetStyle(DEFAULT, BASE_COLOR_NORMAL, ColorToInt(Color{42, 47, 57, 255}));
    GuiSetStyle(DEFAULT, BASE_COLOR_FOCUSED, ColorToInt(Color{54, 62, 75, 255}));

    resolved_map_path_ = ResolveRuntimePath(Constants::kDefaultMapPath);
    resolved_tile_mapping_path_ = ResolveRuntimePath(Constants::kTileMappingPath);
    resolved_sprite_metadata_path_ = ResolveRuntimePath(Constants::kSpriteMetadataPath);
    resolved_spell_pattern_path_ = ResolveRuntimePath(Constants::kSpellPatternPath);

    if (!map_loader_.Load(resolved_map_path_, resolved_tile_mapping_path_, state_.map)) {
        // Fallback: small grass-only map.
        state_.map.width = 24;
        state_.map.height = 16;
        state_.map.cell_size = Constants::kRuneCellSize;
        state_.map.tiles.assign(static_cast<size_t>(state_.map.width * state_.map.height), TileType::Grass);
        state_.map.spawn_points = {{2, 2}, {state_.map.width - 3, state_.map.height - 3}};
    }

    sprite_metadata_.LoadFromFile(resolved_sprite_metadata_path_);
    spell_patterns_.LoadFromFile(resolved_spell_pattern_path_);
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

        while (accumulator >= Constants::kFixedDt) {
            Update(static_cast<float>(Constants::kFixedDt));
            accumulator -= Constants::kFixedDt;
        }

        Render();
    }
}

void GameApp::CaptureFrameInputEdges() {
    pending_primary_pressed_ = pending_primary_pressed_ || IsBindingPressed(controls_bindings_.primary_action);
    pending_select_fire_ = pending_select_fire_ || IsBindingPressed(controls_bindings_.select_fire_rune);
    pending_select_water_ = pending_select_water_ || IsBindingPressed(controls_bindings_.select_water_rune);
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
    settings_.lobby_min_arena_radius_tiles = lobby_min_arena_radius_tiles_;
    settings_.show_network_debug_panel = show_network_debug_panel_;
    config_manager_.Save(settings_);

    lan_discovery_.Stop();
    network_manager_.Stop();
    sprite_metadata_.Unload();
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
            lobby_shrink_tiles_per_second_ = lobby_update->shrink_tiles_per_second;
            lobby_min_arena_radius_tiles_ = lobby_update->min_arena_radius_tiles;
        }

        if (network_manager_.ConsumeMatchStart()) {
            state_.match.match_running = true;
            state_.match.match_finished = false;
            state_.match.time_remaining = static_cast<float>(Constants::kMatchDurationSeconds);
            app_screen_ = AppScreen::InMatch;
            state_.local_player_id = network_manager_.GetAssignedLocalPlayerId();
            pending_primary_pressed_ = false;
            pending_select_fire_ = false;
            pending_select_water_ = false;
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

        if (local_input.primary_pressed || local_input.select_fire || local_input.select_water) {
            ClientActionMessage action_message;
            action_message.player_id = local_input.player_id;
            action_message.seq = local_input.seq;
            action_message.primary_pressed = local_input.primary_pressed;
            action_message.select_fire = local_input.select_fire;
            action_message.select_water = local_input.select_water;
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
        UpdateClientVisualSmoothing(dt);
        UpdateDamagePopups(dt);
    }

    UpdateProjectileEmitters();
    UpdateParticles(dt);
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
        return;
    }

    std::unordered_map<int, Vector2> updated_positions;
    updated_positions.reserve(state_.players.size());

    if (network_manager_.IsHost()) {
        for (const auto& player : state_.players) {
            updated_positions[player.id] = player.pos;
        }
        render_player_positions_.swap(updated_positions);
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

    local_player->rune_place_cooldown_remaining = std::max(0.0f, local_player->rune_place_cooldown_remaining - dt);

    if (input.select_fire) {
        local_player->selected_rune_type = RuneType::Fire;
        local_player->rune_placing_mode = true;
    }
    if (input.select_water) {
        local_player->selected_rune_type = RuneType::Water;
        local_player->rune_placing_mode = true;
    }

    Vector2 aim_vector = {input.aim_x - local_player->pos.x, input.aim_y - local_player->pos.y};
    if (Vector2LengthSqr(aim_vector) > 0.0001f) {
        local_player->aim_dir = Vector2Normalize(aim_vector);
        local_player->facing = AimToFacing(local_player->aim_dir);
    }

    if (input.primary_pressed && local_player->rune_placing_mode &&
        local_player->rune_place_cooldown_remaining <= 0.0f) {
        local_player->rune_placing_mode = false;
        local_player->rune_place_cooldown_remaining = local_player->rune_place_cooldown_duration;
    }

    // Movement/facing prediction for client feel; authoritative state is reconciled on snapshots.
    Vector2 movement = {input.move_x, input.move_y};
    if (Vector2LengthSqr(movement) > 0.0001f) {
        movement = Vector2Normalize(movement);
    }

    const Vector2 acceleration = Vector2Scale(movement, Constants::kPlayerAcceleration);
    local_player->vel = Vector2Add(local_player->vel, Vector2Scale(acceleration, dt));
    const float damping = std::max(0.0f, 1.0f - Constants::kPlayerFriction * dt);
    local_player->vel = Vector2Scale(local_player->vel, damping);
    const float speed = Vector2Length(local_player->vel);
    if (speed > Constants::kPlayerMaxSpeed) {
        local_player->vel = Vector2Scale(Vector2Normalize(local_player->vel), Constants::kPlayerMaxSpeed);
    }
    local_player->pos = Vector2Add(local_player->pos, Vector2Scale(local_player->vel, dt));
    CollisionWorld::ResolvePlayerVsWorld(state_.map, *local_player);
    ResolvePlayerVsIceWalls(*local_player);

    if (local_player->melee_active_remaining > 0.0f) {
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
            const LobbyUiResult lobby_ui =
                DrawLobby(lobby_player_names_, false, host_display_ip_, Constants::kMatchDurationSeconds,
                          lobby_shrink_tiles_per_second_, lobby_min_arena_radius_tiles_,
                          most_kills_mode_.GetUiName(), GetClientLobbyStatusText());
            if (lobby_ui.request_leave) {
                ReturnToMainMenu();
            }
            break;
        }

        case AppScreen::Lobby: {
            const LobbyUiResult lobby_ui = DrawLobby(lobby_player_names_, network_manager_.IsHost(), host_display_ip_,
                                                     Constants::kMatchDurationSeconds, lobby_shrink_tiles_per_second_,
                                                     lobby_min_arena_radius_tiles_, most_kills_mode_.GetUiName(),
                                                     network_manager_.IsHost() ? "hosting/listening"
                                                                              : GetClientLobbyStatusText());
            if (lobby_ui.request_leave) {
                ReturnToMainMenu();
            }
            bool mode_settings_changed = false;
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
                settings_.lobby_shrink_tiles_per_second = lobby_shrink_tiles_per_second_;
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
            DrawMatchHud(state_, state_.local_player_id);
            RenderDebugRuneCooldown();
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
    map_loader_.Load(resolved_map_path_, resolved_tile_mapping_path_, state_.map);

    event_queue_.Clear();
    latest_remote_inputs_.clear();
    render_player_positions_.clear();
    remote_position_samples_.clear();
    pending_local_prediction_inputs_.clear();
    known_player_names_.clear();
    pending_primary_pressed_ = false;
    pending_select_fire_ = false;
    pending_select_water_ = false;
    lobby_broadcast_accumulator_ = 0.0;
    snapshot_accumulator_ = 0.0;
    connect_attempt_start_seconds_ = 0.0;
    winning_team_ = -1;
    host_server_tick_ = 0;
    local_input_seq_ = 0;
    main_menu_status_message_.clear();
    main_menu_status_is_error_ = false;
    lobby_shrink_tiles_per_second_ = settings_.lobby_shrink_tiles_per_second;
    lobby_min_arena_radius_tiles_ = settings_.lobby_min_arena_radius_tiles;
    app_screen_ = AppScreen::MainMenu;
}

void GameApp::StartMatchAsHost() {
    state_.match = MatchState{};
    state_.match.match_running = true;
    state_.match.match_finished = false;
    state_.match.time_remaining = static_cast<float>(Constants::kMatchDurationSeconds);
    state_.match.shrink_tiles_per_second = lobby_shrink_tiles_per_second_;
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
    state_.ice_walls.clear();
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
    pending_select_fire_ = false;
    pending_select_water_ = false;

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

    state_.local_player_id = 0;

    network_manager_.BroadcastMatchStart(MatchStartMessage{});
    network_manager_.BroadcastSnapshot(BuildHostSnapshot());

    app_screen_ = AppScreen::InMatch;
}

void GameApp::ApplySnapshotToClientState(const ServerSnapshotMessage& snapshot) {
    std::unordered_map<int, int> previous_hp;
    previous_hp.reserve(state_.players.size());
    for (const auto& player : state_.players) {
        previous_hp[player.id] = player.hp;
    }

    Vector2 previous_local_pos = {0.0f, 0.0f};
    Vector2 previous_local_vel = {0.0f, 0.0f};
    bool has_previous_local = false;
    if (state_.local_player_id >= 0) {
        if (const Player* previous_local = FindPlayerById(state_.local_player_id)) {
            previous_local_pos = previous_local->pos;
            previous_local_vel = previous_local->vel;
            has_previous_local = true;
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
        player.awaiting_respawn = player_snapshot.awaiting_respawn;
        player.respawn_remaining = player_snapshot.respawn_remaining;
        player.last_processed_move_seq = player_snapshot.last_processed_move_seq;
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
        state_.runes.push_back(rune);
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
    state_.explosions.clear();

    if (!network_manager_.IsHost()) {
        for (const auto& player : state_.players) {
            auto previous_hp_it = previous_hp.find(player.id);
            if (previous_hp_it == previous_hp.end()) {
                continue;
            }
            if (previous_hp_it->second > player.hp) {
                SpawnDamagePopup(player.pos, previous_hp_it->second - player.hp);
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
                if (Vector2LengthSqr(aim_vector) > 0.0001f) {
                    replay.aim_dir = Vector2Normalize(aim_vector);
                    replay.facing = AimToFacing(replay.aim_dir);
                }

                Vector2 movement = {pending_input.move_x, pending_input.move_y};
                if (Vector2LengthSqr(movement) > 0.0001f) {
                    movement = Vector2Normalize(movement);
                }
                const Vector2 acceleration = Vector2Scale(movement, Constants::kPlayerAcceleration);
                replay.vel = Vector2Add(replay.vel, Vector2Scale(acceleration, static_cast<float>(Constants::kFixedDt)));
                const float damping = std::max(0.0f, 1.0f - Constants::kPlayerFriction * static_cast<float>(Constants::kFixedDt));
                replay.vel = Vector2Scale(replay.vel, damping);
                const float speed = Vector2Length(replay.vel);
                if (speed > Constants::kPlayerMaxSpeed) {
                    replay.vel = Vector2Scale(Vector2Normalize(replay.vel), Constants::kPlayerMaxSpeed);
                }
                replay.pos = Vector2Add(replay.pos, Vector2Scale(replay.vel, static_cast<float>(Constants::kFixedDt)));
                CollisionWorld::ResolvePlayerVsWorld(state_.map, replay);
                ResolvePlayerVsIceWalls(replay);
            }

            const Vector2 base_pos = has_previous_local ? previous_local_pos : local_player->pos;
            const Vector2 base_vel = has_previous_local ? previous_local_vel : local_player->vel;
            const float error = Vector2Distance(base_pos, replay.pos);
            if (error > Constants::kPredictionHardSnapThresholdPx) {
                local_player->pos = replay.pos;
                local_player->vel = replay.vel;
            } else {
                const float alpha = std::clamp(Constants::kPredictionReconcileGain *
                                                   Constants::kNetworkSnapshotIntervalSeconds,
                                               0.0f, 1.0f);
                local_player->pos = Vector2Lerp(base_pos, replay.pos, alpha);
                local_player->vel = Vector2Lerp(base_vel, replay.vel, alpha);
            }
            local_player->aim_dir = replay.aim_dir;
            local_player->facing = replay.facing;

            if (error > 0.01f) {
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
    input.select_fire = pending_select_fire_;
    input.select_water = pending_select_water_;
    pending_primary_pressed_ = false;
    pending_select_fire_ = false;
    pending_select_water_ = false;
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
        input.select_fire = input.select_fire || action.select_fire;
        input.select_water = input.select_water || action.select_water;
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
        input.select_fire = false;
        input.select_water = false;
    }

    ResolvePlayerCollisions();
    UpdateIceWalls(dt);
    UpdateArena(dt);

    for (auto& player : state_.players) {
        if (player.melee_active_remaining > 0.0f && player.alive) {
            HandleMeleeHit(player);
        }
    }

    UpdateProjectiles(dt);
    UpdateExplosions(dt);
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

    if (input.select_fire) {
        player.selected_rune_type = RuneType::Fire;
        player.rune_placing_mode = true;
    }
    if (input.select_water) {
        player.selected_rune_type = RuneType::Water;
        player.rune_placing_mode = true;
    }
    Vector2 aim_vector = {input.aim_x - player.pos.x, input.aim_y - player.pos.y};
    if (Vector2LengthSqr(aim_vector) > 0.0001f) {
        player.aim_dir = Vector2Normalize(aim_vector);
        player.facing = AimToFacing(player.aim_dir);
    }

    player.melee_cooldown_remaining = std::max(0.0f, player.melee_cooldown_remaining - dt);
    player.melee_active_remaining = std::max(0.0f, player.melee_active_remaining - dt);
    player.rune_place_cooldown_remaining = std::max(0.0f, player.rune_place_cooldown_remaining - dt);

    if (input.primary_pressed) {
        if (player.rune_placing_mode) {
            if (TryPlaceRune(player, Vector2{input.aim_x, input.aim_y})) {
                player.action_state = PlayerActionState::RunePlacing;
            }
        } else if (player.melee_cooldown_remaining <= 0.0f) {
            player.melee_cooldown_remaining = Constants::kMeleeCooldownSeconds;
            player.melee_active_remaining = Constants::kMeleeActiveWindowSeconds;
            player.melee_hit_target_ids.clear();
            player.action_state = PlayerActionState::Slashing;
        }
    }

    Vector2 movement = {input.move_x, input.move_y};
    if (Vector2LengthSqr(movement) > 0.0001f) {
        movement = Vector2Normalize(movement);
    }

    Vector2 acceleration = Vector2Scale(movement, Constants::kPlayerAcceleration);
    player.vel = Vector2Add(player.vel, Vector2Scale(acceleration, dt));

    const float damping = std::max(0.0f, 1.0f - Constants::kPlayerFriction * dt);
    player.vel = Vector2Scale(player.vel, damping);

    const float speed = Vector2Length(player.vel);
    if (speed > Constants::kPlayerMaxSpeed) {
        player.vel = Vector2Scale(Vector2Normalize(player.vel), Constants::kPlayerMaxSpeed);
    }

    player.pos = Vector2Add(player.pos, Vector2Scale(player.vel, dt));
    CollisionWorld::ResolvePlayerVsWorld(state_.map, player);
    ResolvePlayerVsIceWalls(player);

    if (player.melee_active_remaining > 0.0f) {
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

    state_.match.arena_radius_tiles = std::max(
        state_.match.min_arena_radius_tiles,
        state_.match.arena_radius_tiles - state_.match.shrink_tiles_per_second * dt);
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
        player.pos = ComputeRespawnPosition(player);
        CollisionWorld::ResolvePlayerVsWorld(state_.map, player);
        ResolvePlayerVsIceWalls(player);
        render_player_positions_[player.id] = player.pos;
    }
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

void GameApp::SpawnDamagePopup(Vector2 world_pos, int amount) {
    if (amount <= 0) {
        return;
    }
    DamagePopup popup;
    popup.pos = world_pos;
    popup.amount = amount;
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
    SpawnDamagePopup(target.pos, damage);

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
    victim.outside_zone_damage_accumulator = 0.0f;
    victim.action_state = PlayerActionState::Idle;
    victim.vel = {0.0f, 0.0f};
    event_queue_.Push(PlayerDiedEvent{victim.id, killer_player_id});

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

void GameApp::ResolvePlayerVsIceWalls(Player& player) {
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
    const Rectangle aabb = {static_cast<float>(cell.x * cell_size), static_cast<float>(cell.y * cell_size),
                            static_cast<float>(cell_size), static_cast<float>(cell_size)};
    const Vector2 centroid = {aabb.x + aabb.width * 0.5f, aabb.y + aabb.height * 0.5f};

    for (auto& player : state_.players) {
        if (!player.alive) {
            continue;
        }

        Vector2 normal = {0.0f, 0.0f};
        float penetration = 0.0f;
        if (!CollisionWorld::CircleVsAabb(player.pos, player.radius, aabb, normal, penetration)) {
            continue;
        }

        Vector2 push_dir = Vector2Subtract(player.pos, centroid);
        if (Vector2LengthSqr(push_dir) <= 0.0001f) {
            push_dir = normal;
        } else {
            push_dir = Vector2Normalize(push_dir);
        }
        player.pos = Vector2Add(player.pos, Vector2Scale(push_dir, penetration + 0.5f));

        const float velocity_dot = Vector2DotProduct(player.vel, push_dir);
        if (velocity_dot < 0.0f) {
            player.vel = Vector2Subtract(player.vel, Vector2Scale(push_dir, velocity_dot));
        }
        CollisionWorld::ResolvePlayerVsWorld(state_.map, player);
    }
}

void GameApp::UpdateProjectiles(float dt) {
    for (auto& projectile : state_.projectiles) {
        if (!projectile.alive) {
            continue;
        }

        projectile.vel = Vector2Add(projectile.vel, Vector2Scale(projectile.acc, dt));
        const float drag_factor = std::max(0.0f, 1.0f - projectile.drag * dt);
        projectile.vel = Vector2Scale(projectile.vel, drag_factor);
        projectile.pos = Vector2Add(projectile.pos, Vector2Scale(projectile.vel, dt));

        bool destroy_projectile = false;
        std::optional<int> excluded_target_id;

        const GridCoord cell = WorldToCell(projectile.pos);
        if (!state_.map.IsInside(cell)) {
            destroy_projectile = true;
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
                                      [](const Rune& rune) { return !rune.active; }),
                       state_.runes.end());
}

bool GameApp::TryPlaceRune(Player& player, Vector2 world_mouse) {
    if (player.rune_place_cooldown_remaining > 0.0f) {
        return false;
    }

    const GridCoord cell = WorldToCell(world_mouse);
    if (!IsTileRunePlaceable(cell) || IsCellOccupiedByRune(cell)) {
        return false;
    }

    Rune rune;
    rune.id = state_.next_entity_id++;
    rune.owner_player_id = player.id;
    rune.owner_team = player.team;
    rune.cell = cell;
    rune.rune_type = player.selected_rune_type;
    rune.placement_order = state_.next_rune_placement_order++;
    rune.active = true;
    state_.runes.push_back(rune);

    event_queue_.Push(RunePlacedEvent{player.id, player.team, cell, player.selected_rune_type, rune.placement_order});
    player.rune_place_cooldown_remaining = player.rune_place_cooldown_duration;
    player.rune_placing_mode = false;
    return true;
}

void GameApp::CheckSpellPatterns(const RunePlacedEvent& event) {
    const auto& patterns = spell_patterns_.GetPatterns();
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
                        if (info->placement_constraint == PlacementConstraint::Latest &&
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

                                                                  if (info->placement_constraint ==
                                                                      PlacementConstraint::Latest) {
                                                                      return rune.placement_order ==
                                                                             event.placement_order;
                                                                  }
                                                                  if (info->placement_constraint ==
                                                                      PlacementConstraint::Old) {
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

                    GridCoord cast_origin = event.cell;
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

                    event_queue_.Push(
                        RunePatternCompletedEvent{pattern.spell_name, directional.direction, cast_origin, matched_cells});

                    if (pattern.spell_name == "fire_bolt") {
                        FireBoltSpell spell(CellToWorldCenter(cast_origin), event.player_id, directional.direction);
                        spell.Cast(state_, event_queue_);
                    } else if (pattern.spell_name == "ice_wall") {
                        IceWallSpell spell(cast_origin, event.player_id, directional.direction);
                        spell.Cast(state_, event_queue_);
                    }

                    for (auto& rune : state_.runes) {
                        for (const auto& matched_cell : matched_cells) {
                            if (rune.active && rune.cell == matched_cell) {
                                rune.active = false;
                                break;
                            }
                        }
                    }

                    return;
                }
            }
        }
    }
}

bool GameApp::IsTileRunePlaceable(const GridCoord& cell) const {
    if (!state_.map.IsInside(cell)) {
        return false;
    }

    const TileType tile = state_.map.GetTile(cell);
    return tile == TileType::Grass || tile == TileType::SpawnPoint;
}

bool GameApp::IsCellOccupiedByRune(const GridCoord& cell) const {
    return std::any_of(state_.runes.begin(), state_.runes.end(),
                       [&](const Rune& rune) { return rune.active && rune.cell == cell; });
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
    RenderRunes();
    RenderIceWalls();
    RenderProjectiles();
    RenderParticles();
    RenderPlayers();
    RenderMeleeAttacks();
    RenderDamagePopups();
    RenderRunePlacementOverlay();
    EndMode2D();
}

void GameApp::RenderMap() {
    if (state_.map.width <= 0 || state_.map.height <= 0) {
        return;
    }

    const bool has_texture = sprite_metadata_.IsLoaded();
    const Texture2D texture = sprite_metadata_.GetTexture();
    const bool has_grass_bitmask = has_texture && sprite_metadata_.HasBitmaskAnimation("tile_grass");
    const auto is_grass_family = [](TileType tile) {
        return tile == TileType::Grass || tile == TileType::SpawnPoint;
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

            if (has_texture && (tile == TileType::Grass || tile == TileType::SpawnPoint)) {
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
            } else if (has_texture && tile == TileType::Water) {
                const Rectangle src = InsetSourceRect(
                    sprite_metadata_.GetFrame("tile_water", "default", render_time_seconds_),
                    Constants::kAtlasSampleInsetPixels);
                DrawTexturePro(texture, src, dst, {0, 0}, 0.0f, tint);
            } else {
                Color fallback = Color{34, 120, 34, 255};
                if (tile == TileType::Water) fallback = Color{26, 96, 152, 255};
                if (dimmed) {
                    fallback = Color{static_cast<unsigned char>(fallback.r * Constants::kOutsideZoneTileBrightness),
                                     static_cast<unsigned char>(fallback.g * Constants::kOutsideZoneTileBrightness),
                                     static_cast<unsigned char>(fallback.b * Constants::kOutsideZoneTileBrightness), 255};
                }
                DrawRectangleRec(dst, fallback);
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
        const char* animation_name = rune.rune_type == RuneType::Fire ? "fire_rune" : "water_rune";

        if (has_texture && sprite_metadata_.HasAnimation(animation_name)) {
            const Rectangle src = InsetSourceRect(
                sprite_metadata_.GetFrame(animation_name, "default", render_time_seconds_ + rune.placement_order * 0.05f),
                Constants::kAtlasSampleInsetPixels);
            Rectangle dst = {center.x - 16.0f, center.y - 16.0f, 32.0f, 32.0f};
            dst = SnapRect(dst);
            DrawTexturePro(texture, src, dst, {0, 0}, 0.0f, WHITE);
        } else {
            const Color color =
                (rune.rune_type == RuneType::Fire) ? Color{255, 140, 44, 220} : Color{66, 180, 255, 220};
            DrawCircleV(center, 8.0f, color);
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
        std::string animation = "wizard_idle";
        if (player.action_state == PlayerActionState::Slashing) {
            animation = "wizard_slash";
        } else if (player.action_state == PlayerActionState::RunePlacing) {
            animation = "wizard_create_rune";
        } else if (is_moving) {
            animation = "wizard_walking";
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

            Color tint = WHITE;
            if (player.team == Constants::kTeamRed) {
                tint = Color{255, 215, 215, 255};
            } else {
                tint = Color{215, 230, 255, 255};
            }

            const Texture2D draw_texture = sprite_metadata_.GetTexture(mirror);
            DrawTexturePro(draw_texture, src, dst, {0, 0}, 0.0f, tint);
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

        const int font_size = Constants::kPlayerHealthTextFontSize;
        const char* hp_text = TextFormat("%d", std::max(0, player.hp));
        const int text_width = MeasureText(hp_text, font_size);
        const int text_x = static_cast<int>(health_bar.x + (health_bar.width - static_cast<float>(text_width)) * 0.5f);
        const int text_y =
            static_cast<int>(health_bar.y + (health_bar.height - static_cast<float>(font_size)) * 0.5f);
        DrawText(hp_text, text_x, text_y, font_size, BLACK);
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

    for (const auto& projectile : state_.projectiles) {
        if (!projectile.alive) {
            continue;
        }

        if (has_texture && sprite_metadata_.HasAnimation(projectile.animation_key)) {
            const Rectangle src =
                InsetSourceRect(sprite_metadata_.GetFrame(projectile.animation_key, "default", render_time_seconds_),
                                Constants::kAtlasSampleInsetPixels);
            Rectangle dst = {projectile.pos.x - 16.0f, projectile.pos.y - 16.0f, 32.0f, 32.0f};
            dst = SnapRect(dst);
            const Color tint = projectile.owner_team == Constants::kTeamRed ? Color{255, 215, 215, 255}
                                                                            : Color{215, 230, 255, 255};
            DrawTexturePro(texture, src, dst, {0, 0}, 0.0f, tint);
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
            Rectangle dst = {particle.pos.x - 16.0f, particle.pos.y - 16.0f, 32.0f, 32.0f};
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
        const char* text = TextFormat("-%d", popup.amount);
        const int font_size = Constants::kDamagePopupFontSize;
        const int text_w = MeasureText(text, font_size);
        const Vector2 draw_pos = {std::roundf(popup.pos.x - static_cast<float>(text_w) * 0.5f), std::roundf(popup.pos.y)};
        DrawText(text, static_cast<int>(draw_pos.x), static_cast<int>(draw_pos.y), font_size,
                 Color{255, 50, 50, alpha});
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
    const bool valid = !IsCellOccupiedByRune(mouse_cell) && IsTileRunePlaceable(mouse_cell);
    const char* animation_name = local_player->selected_rune_type == RuneType::Fire ? "fire_rune" : "water_rune";
    if (sprite_metadata_.IsLoaded() && sprite_metadata_.HasAnimation(animation_name)) {
        const Rectangle src =
            InsetSourceRect(sprite_metadata_.GetFrame(animation_name, "default", render_time_seconds_),
                            Constants::kAtlasSampleInsetPixels);
        Rectangle dst = {center.x - 16.0f, center.y - 16.0f, 32.0f, 32.0f};
        dst = SnapRect(dst);
        const Color tint = valid ? Color{255, 255, 255, 160} : Color{255, 96, 96, 160};
        DrawTexturePro(sprite_metadata_.GetTexture(), src, dst, {0, 0}, 0.0f, tint);
    } else {
        Color preview = local_player->selected_rune_type == RuneType::Fire ? Color{255, 130, 64, 160}
                                                                            : Color{80, 180, 255, 160};
        if (!valid) {
            preview = Color{220, 60, 60, 160};
        }
        DrawCircleV(center, 10.0f, preview);
        DrawCircleLinesV(center, 10.0f, Color{20, 26, 32, 255});
    }
}

void GameApp::RenderDebugRuneCooldown() {
    const Player* local_player = FindPlayerById(state_.local_player_id);
    if (!local_player) {
        return;
    }

    const float cooldown = local_player->rune_place_cooldown_remaining;
    if (cooldown <= 0.0f) {
        return;
    }

    const float duration = std::max(0.001f, local_player->rune_place_cooldown_duration);
    const float ratio = std::clamp(cooldown / duration, 0.0f, 1.0f);

    const float x = 16.0f;
    const float y = static_cast<float>(GetScreenHeight()) - 22.0f;
    const Rectangle bar = {x, y, Constants::kRuneCooldownBarWidth, Constants::kRuneCooldownBarHeight};

    const Color bar_fill = {static_cast<unsigned char>(Constants::kPlayerHealthBarFillR),
                            static_cast<unsigned char>(Constants::kPlayerHealthBarFillG),
                            static_cast<unsigned char>(Constants::kPlayerHealthBarFillB), 255};
    const Color bar_missing = {static_cast<unsigned char>(Constants::kPlayerHealthBarMissingR),
                               static_cast<unsigned char>(Constants::kPlayerHealthBarMissingG),
                               static_cast<unsigned char>(Constants::kPlayerHealthBarMissingB), 255};

    DrawRectangleRec(bar, bar_missing);
    Rectangle fill = bar;
    fill.width = std::roundf(fill.width * ratio);
    if (fill.width > 0.0f) {
        DrawRectangleRec(fill, bar_fill);
    }

    char cooldown_text[32] = {};
    std::snprintf(cooldown_text, sizeof(cooldown_text), "%.2fs", cooldown);
    const int font_size = Constants::kRuneCooldownTextFontSize;
    const int text_width = MeasureText(cooldown_text, font_size);
    const int text_x = static_cast<int>(bar.x + (bar.width - static_cast<float>(text_width)) * 0.5f);
    const int text_y = static_cast<int>(bar.y - static_cast<float>(font_size) - 2.0f);
    DrawText(cooldown_text, text_x, text_y, font_size, WHITE);
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

    camera_.offset = {std::roundf(static_cast<float>(GetScreenWidth()) * 0.5f),
                      std::roundf(static_cast<float>(GetScreenHeight()) * 0.5f)};
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
