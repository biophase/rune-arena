#include "gameplay/equipment_registry.h"

#include <algorithm>
#include <fstream>

#include <nlohmann/json.hpp>

namespace {

EquipmentSlot ParseEquipmentSlot(const std::string& value, bool* ok) {
    if (ok != nullptr) {
        *ok = true;
    }
    if (value == "primary_weapon") {
        return EquipmentSlot::PrimaryWeapon;
    }
    if (value == "mobility") {
        return EquipmentSlot::Mobility;
    }
    if (value == "inventory") {
        return EquipmentSlot::Inventory;
    }
    if (ok != nullptr) {
        *ok = false;
    }
    return EquipmentSlot::Inventory;
}

EquipmentActionKind ParseActionKind(const std::string& value) {
    if (value == "melee") {
        return EquipmentActionKind::Melee;
    }
    if (value == "projectile") {
        return EquipmentActionKind::Projectile;
    }
    if (value == "grappling_hook") {
        return EquipmentActionKind::GrapplingHook;
    }
    if (value == "dash") {
        return EquipmentActionKind::Dash;
    }
    return EquipmentActionKind::None;
}

PlayerActionState ParsePlayerActionState(const std::string& value) {
    if (value == "walking") {
        return PlayerActionState::Walking;
    }
    if (value == "slashing") {
        return PlayerActionState::Slashing;
    }
    if (value == "rune_placing") {
        return PlayerActionState::RunePlacing;
    }
    return PlayerActionState::Idle;
}

size_t SlotToIndex(EquipmentSlot slot) {
    switch (slot) {
        case EquipmentSlot::PrimaryWeapon:
            return 0;
        case EquipmentSlot::Mobility:
            return 1;
        case EquipmentSlot::Inventory:
        default:
            return 0;
    }
}

}  // namespace

bool EquipmentRegistry::LoadFromFile(const std::string& path) {
    loaded_ = false;
    last_error_.clear();
    items_.clear();
    weapon_families_.clear();
    mobility_families_.clear();
    attack_profiles_.clear();
    mobility_profiles_.clear();

    std::ifstream file(path);
    if (!file.is_open()) {
        last_error_ = "failed to open equipment profiles";
        return false;
    }

    nlohmann::json json;
    try {
        file >> json;
    } catch (const std::exception& ex) {
        last_error_ = std::string("invalid JSON: ") + ex.what();
        return false;
    }

    if (const auto attack_profiles_it = json.find("attack_profiles");
        attack_profiles_it != json.end() && attack_profiles_it->is_object()) {
        for (auto it = attack_profiles_it->begin(); it != attack_profiles_it->end(); ++it) {
            const auto& value = it.value();
            AttackProfile profile;
            profile.id = it.key();
            profile.kind = ParseActionKind(value.value("kind", ""));
            profile.family_type = value.value("family_type", "");
            profile.cooldown_seconds = value.value("cooldown_seconds", 0.0f);
            profile.windup_seconds = value.value("windup_seconds", 0.0f);
            profile.active_window_seconds = value.value("active_window_seconds", 0.0f);
            profile.hit_start_seconds = value.value("hit_start_seconds", profile.windup_seconds);
            profile.hit_end_seconds = value.value("hit_end_seconds", profile.active_window_seconds);
            profile.range = value.value("range", 0.0f);
            profile.damage = value.value("damage", 0);
            profile.hit_shape_id = value.value("hit_shape_id", "");
            profile.attack_animation = value.value("attack_animation", "melee_attack");
            profile.action_state = ParsePlayerActionState(value.value("action_state", "idle"));
            attack_profiles_.emplace(profile.id, std::move(profile));
        }
    }

    if (const auto mobility_profiles_it = json.find("mobility_profiles");
        mobility_profiles_it != json.end() && mobility_profiles_it->is_object()) {
        for (auto it = mobility_profiles_it->begin(); it != mobility_profiles_it->end(); ++it) {
            const auto& value = it.value();
            MobilityProfile profile;
            profile.id = it.key();
            profile.kind = ParseActionKind(value.value("kind", ""));
            profile.family_type = value.value("family_type", "");
            profile.cooldown_seconds = value.value("cooldown_seconds", 0.0f);
            profile.range_tiles = value.value("range_tiles", 0.0f);
            mobility_profiles_.emplace(profile.id, std::move(profile));
        }
    }

    if (const auto weapon_families_it = json.find("weapon_families");
        weapon_families_it != json.end() && weapon_families_it->is_object()) {
        for (auto it = weapon_families_it->begin(); it != weapon_families_it->end(); ++it) {
            const auto& value = it.value();
            WeaponFamilyDefinition family;
            family.id = it.key();
            family.type = value.value("type", "");
            family.attack_profile_id = value.value("attack_profile_id", "");
            weapon_families_.emplace(family.id, std::move(family));
        }
    }

    if (const auto mobility_families_it = json.find("mobility_families");
        mobility_families_it != json.end() && mobility_families_it->is_object()) {
        for (auto it = mobility_families_it->begin(); it != mobility_families_it->end(); ++it) {
            const auto& value = it.value();
            MobilityFamilyDefinition family;
            family.id = it.key();
            family.type = value.value("type", "");
            family.profile_id = value.value("profile_id", "");
            mobility_families_.emplace(family.id, std::move(family));
        }
    }

    if (const auto items_it = json.find("items"); items_it != json.end() && items_it->is_object()) {
        for (auto it = items_it->begin(); it != items_it->end(); ++it) {
            const auto& value = it.value();
            bool ok = false;
            EquipmentItemDefinition item;
            item.id = it.key();
            item.slot = ParseEquipmentSlot(value.value("slot", "inventory"), &ok);
            if (!ok) {
                last_error_ = "unknown equipment slot for item '" + item.id + "'";
                return false;
            }
            item.weapon_family_id = value.value("weapon_family_id", "");
            item.mobility_family_id = value.value("mobility_family_id", "");
            item.hud_icon_animation = value.value("hud_icon_animation", item.id);
            if (const auto layers_it = value.find("modular_layers");
                layers_it != value.end() && layers_it->is_array()) {
                for (const auto& layer_json : *layers_it) {
                    if (layer_json.is_string()) {
                        item.modular_layers.push_back(layer_json.get<std::string>());
                    }
                }
            }
            items_.emplace(item.id, std::move(item));
        }
    }

    loaded_ = true;
    return true;
}

const EquipmentItemDefinition* EquipmentRegistry::FindItem(const std::string& item_id) const {
    auto it = items_.find(item_id);
    return it == items_.end() ? nullptr : &it->second;
}

const EquipmentItemDefinition* EquipmentRegistry::ResolveEquippedItem(const Player& player, EquipmentSlot slot) const {
    if (slot == EquipmentSlot::Inventory) {
        return nullptr;
    }
    const size_t index = SlotToIndex(slot);
    if (index >= player.weapon_slots.size()) {
        return nullptr;
    }
    return FindItem(player.weapon_slots[index]);
}

const AttackProfile* EquipmentRegistry::ResolvePrimaryAttack(const Player& player) const {
    const EquipmentItemDefinition* item = ResolveEquippedItem(player, EquipmentSlot::PrimaryWeapon);
    if (item == nullptr || item->weapon_family_id.empty()) {
        return nullptr;
    }
    const auto family_it = weapon_families_.find(item->weapon_family_id);
    if (family_it == weapon_families_.end()) {
        return nullptr;
    }
    const auto profile_it = attack_profiles_.find(family_it->second.attack_profile_id);
    if (profile_it == attack_profiles_.end()) {
        return nullptr;
    }
    return &profile_it->second;
}

const MobilityProfile* EquipmentRegistry::ResolveMobility(const Player& player) const {
    const EquipmentItemDefinition* item = ResolveEquippedItem(player, EquipmentSlot::Mobility);
    if (item == nullptr || item->mobility_family_id.empty()) {
        return nullptr;
    }
    const auto family_it = mobility_families_.find(item->mobility_family_id);
    if (family_it == mobility_families_.end()) {
        return nullptr;
    }
    const auto profile_it = mobility_profiles_.find(family_it->second.profile_id);
    if (profile_it == mobility_profiles_.end()) {
        return nullptr;
    }
    return &profile_it->second;
}

std::vector<std::string> EquipmentRegistry::CollectVisibleLayers(const Player& player) const {
    std::vector<std::string> layers;
    for (EquipmentSlot slot : {EquipmentSlot::PrimaryWeapon, EquipmentSlot::Mobility}) {
        const EquipmentItemDefinition* item = ResolveEquippedItem(player, slot);
        if (item == nullptr) {
            continue;
        }
        for (const std::string& layer : item->modular_layers) {
            if (!layer.empty() && std::find(layers.begin(), layers.end(), layer) == layers.end()) {
                layers.push_back(layer);
            }
        }
    }
    return layers;
}
