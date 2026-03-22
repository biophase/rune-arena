#pragma once

#include <string>
#include <vector>

#include "game/game_types.h"

enum class ObjectType {
    Terrain,
    Decoration,
    Destructible,
    Consumable,
};

enum class SpriteSheetType {
    Base32,
    Tall32x64,
    Large128x128,
};

enum class EffectType {
    IncreaseCurrentHealth,
    IncreaseCurrentMana,
    SpawnObject,
};

struct DropEntry {
    std::string object_id;
    float chance = 1.0f;
    int min_count = 1;
    int max_count = 1;
};

struct EffectSpec {
    EffectType type = EffectType::IncreaseCurrentHealth;
    int amount = 0;
    std::string object_id;
    int min_count = 1;
    int max_count = 1;
};

struct ObjectPrototype {
    std::string id;
    ObjectType type = ObjectType::Terrain;

    bool has_map_key = false;
    unsigned char map_r = 0;
    unsigned char map_g = 0;
    unsigned char map_b = 0;

    SpriteSheetType sprite_sheet = SpriteSheetType::Base32;
    SpriteSheetType shadow_sheet = SpriteSheetType::Base32;
    std::string idle_animation;
    std::string death_animation;
    std::string shadow_animation;

    bool walkable = true;
    bool stops_projectiles = false;
    bool masked_occluder = false;
    bool has_collision_box_override = false;
    int collision_box_x = 0;
    int collision_box_y = 0;
    int collision_box_w = 0;
    int collision_box_h = 0;

    TileType terrain_tile = TileType::Grass;
    bool terrain_is_spawn_point = false;

    int destructible_hp = 0;
    std::vector<DropEntry> on_destroy_drops;
    std::vector<EffectSpec> consumable_effects;
};

struct MapObjectSeed {
    std::string prototype_id;
    GridCoord cell;
};

enum class MapObjectState {
    Active,
    Dying,
};

struct MapObjectInstance {
    int id = -1;
    std::string prototype_id;
    GridCoord cell;

    ObjectType type = ObjectType::Decoration;
    bool walkable = true;
    bool stops_projectiles = false;
    bool collision_enabled = true;

    int hp = 0;
    MapObjectState state = MapObjectState::Active;
    float state_time = 0.0f;
    float death_duration = 0.0f;

    bool alive = true;
};
