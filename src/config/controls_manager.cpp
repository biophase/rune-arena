#include "config/controls_manager.h"

#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>
#include <platform_folders.h>

#include "core/constants.h"

ControlsManager::ControlsManager() {
    try {
        const std::filesystem::path config_dir =
            std::filesystem::path(sago::getConfigHome()) / Constants::kConfigFolderName;
        std::filesystem::create_directories(config_dir);
        controls_path_ = (config_dir / Constants::kControlsFileName).string();
    } catch (const std::filesystem::filesystem_error&) {
        const std::filesystem::path fallback_dir = std::filesystem::path(".rune_arena_config");
        std::filesystem::create_directories(fallback_dir);
        controls_path_ = (fallback_dir / Constants::kControlsFileName).string();
    }
}

bool ControlsManager::Load(ControlsBindings& bindings) const {
    std::ifstream input(controls_path_);
    if (!input.is_open()) {
        return false;
    }

    nlohmann::json json;
    input >> json;

    bindings.move_left = json.value("move_left", bindings.move_left);
    bindings.move_right = json.value("move_right", bindings.move_right);
    bindings.move_up = json.value("move_up", bindings.move_up);
    bindings.move_down = json.value("move_down", bindings.move_down);
    bindings.primary_action = json.value("primary_action", bindings.primary_action);
    bindings.select_fire_rune = json.value("select_fire_rune", bindings.select_fire_rune);
    bindings.select_water_rune = json.value("select_water_rune", bindings.select_water_rune);
    return true;
}

bool ControlsManager::Save(const ControlsBindings& bindings) const {
    nlohmann::json json;
    json["move_left"] = bindings.move_left;
    json["move_right"] = bindings.move_right;
    json["move_up"] = bindings.move_up;
    json["move_down"] = bindings.move_down;
    json["primary_action"] = bindings.primary_action;
    json["select_fire_rune"] = bindings.select_fire_rune;
    json["select_water_rune"] = bindings.select_water_rune;

    std::ofstream output(controls_path_);
    if (!output.is_open()) {
        return false;
    }

    output << json.dump(2);
    return true;
}

const std::string& ControlsManager::GetControlsPath() const { return controls_path_; }
