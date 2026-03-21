#include "config/config_manager.h"

#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>
#include <platform_folders.h>

#include "core/constants.h"

ConfigManager::ConfigManager() {
    try {
        const std::filesystem::path config_dir =
            std::filesystem::path(sago::getConfigHome()) / Constants::kConfigFolderName;
        std::filesystem::create_directories(config_dir);
        config_path_ = (config_dir / Constants::kConfigFileName).string();
    } catch (const std::filesystem::filesystem_error&) {
        // Workspace fallback for sandboxed environments where user config dirs are blocked.
        const std::filesystem::path fallback_dir = std::filesystem::path(".rune_arena_config");
        std::filesystem::create_directories(fallback_dir);
        config_path_ = (fallback_dir / Constants::kConfigFileName).string();
    }
}

bool ConfigManager::Load(UserSettings& settings) const {
    std::ifstream input(config_path_);
    if (!input.is_open()) {
        return false;
    }

    nlohmann::json json;
    input >> json;

    settings.player_name = json.value("player_name", settings.player_name);
    settings.window_width = json.value("window_width", settings.window_width);
    settings.window_height = json.value("window_height", settings.window_height);
    settings.fullscreen = json.value("fullscreen", settings.fullscreen);
    settings.show_network_debug_panel = json.value("show_network_debug_panel", settings.show_network_debug_panel);
    settings.lobby_shrink_tiles_per_second =
        json.value("lobby_shrink_tiles_per_second", settings.lobby_shrink_tiles_per_second);
    settings.lobby_min_arena_radius_tiles =
        json.value("lobby_min_arena_radius_tiles", settings.lobby_min_arena_radius_tiles);
    settings.lobby_mode_type = json.value("lobby_mode_type", settings.lobby_mode_type);
    settings.lobby_match_length_seconds =
        json.value("lobby_match_length_seconds", settings.lobby_match_length_seconds);
    settings.lobby_best_of_target_kills =
        json.value("lobby_best_of_target_kills", settings.lobby_best_of_target_kills);
    settings.lobby_shrink_start_seconds =
        json.value("lobby_shrink_start_seconds", settings.lobby_shrink_start_seconds);
    return true;
}

bool ConfigManager::Save(const UserSettings& settings) const {
    nlohmann::json json;
    json["player_name"] = settings.player_name;
    json["window_width"] = settings.window_width;
    json["window_height"] = settings.window_height;
    json["fullscreen"] = settings.fullscreen;
    json["show_network_debug_panel"] = settings.show_network_debug_panel;
    json["lobby_shrink_tiles_per_second"] = settings.lobby_shrink_tiles_per_second;
    json["lobby_min_arena_radius_tiles"] = settings.lobby_min_arena_radius_tiles;
    json["lobby_mode_type"] = settings.lobby_mode_type;
    json["lobby_match_length_seconds"] = settings.lobby_match_length_seconds;
    json["lobby_best_of_target_kills"] = settings.lobby_best_of_target_kills;
    json["lobby_shrink_start_seconds"] = settings.lobby_shrink_start_seconds;

    std::ofstream output(config_path_);
    if (!output.is_open()) {
        return false;
    }

    output << json.dump(2);
    return true;
}

const std::string& ConfigManager::GetConfigPath() const { return config_path_; }
