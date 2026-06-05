#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "game/game_types.h"
#include "game/player.h"

enum class EquipmentActionKind {
    None,
    Melee,
    Projectile,
    GrapplingHook,
    Dash,
};

struct AttackProfile {
    std::string id;
    EquipmentActionKind kind = EquipmentActionKind::None;
    std::string family_type;
    float cooldown_seconds = 0.0f;
    float windup_seconds = 0.0f;
    float active_window_seconds = 0.0f;
    float hit_start_seconds = 0.0f;
    float hit_end_seconds = 0.0f;
    float range = 0.0f;
    int damage = 0;
    std::string hit_shape_id;
    std::string attack_animation = "melee_attack";
    PlayerActionState action_state = PlayerActionState::Idle;
};

struct MobilityProfile {
    std::string id;
    EquipmentActionKind kind = EquipmentActionKind::None;
    std::string family_type;
    float cooldown_seconds = 0.0f;
    float range_tiles = 0.0f;
};

struct EquipmentItemDefinition {
    std::string id;
    EquipmentSlot slot = EquipmentSlot::Inventory;
    std::string weapon_family_id;
    std::string mobility_family_id;
    std::string hud_icon_animation;
    std::vector<std::string> modular_layers;
};

class EquipmentRegistry {
  public:
    bool LoadFromFile(const std::string& path);

    bool IsLoaded() const { return loaded_; }
    const std::string& GetLastError() const { return last_error_; }

    const EquipmentItemDefinition* FindItem(const std::string& item_id) const;
    const AttackProfile* ResolvePrimaryAttack(const Player& player) const;
    const MobilityProfile* ResolveMobility(const Player& player) const;
    const EquipmentItemDefinition* ResolveEquippedItem(const Player& player, EquipmentSlot slot) const;
    std::vector<std::string> CollectVisibleLayers(const Player& player) const;
    bool GrantsPrimaryAction(const Player& player) const { return ResolvePrimaryAttack(player) != nullptr; }
    bool GrantsMobilityAction(const Player& player) const { return ResolveMobility(player) != nullptr; }

  private:
    struct WeaponFamilyDefinition {
        std::string id;
        std::string type;
        std::string attack_profile_id;
    };

    struct MobilityFamilyDefinition {
        std::string id;
        std::string type;
        std::string profile_id;
    };

    bool loaded_ = false;
    std::string last_error_;
    std::unordered_map<std::string, EquipmentItemDefinition> items_;
    std::unordered_map<std::string, WeaponFamilyDefinition> weapon_families_;
    std::unordered_map<std::string, MobilityFamilyDefinition> mobility_families_;
    std::unordered_map<std::string, AttackProfile> attack_profiles_;
    std::unordered_map<std::string, MobilityProfile> mobility_profiles_;
};
