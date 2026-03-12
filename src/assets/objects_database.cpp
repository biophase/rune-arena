#include "assets/objects_database.h"

#include <algorithm>
#include <fstream>

#include <nlohmann/json.hpp>
#include <raylib.h>

namespace {

ObjectType ParseObjectType(const std::string& value, bool& ok) {
    ok = true;
    if (value == "Terrain") return ObjectType::Terrain;
    if (value == "Decoration") return ObjectType::Decoration;
    if (value == "Destructible") return ObjectType::Destructible;
    if (value == "Consumable") return ObjectType::Consumable;
    ok = false;
    return ObjectType::Terrain;
}

SpriteSheetType ParseSpriteSheetType(const std::string& value, bool& ok) {
    ok = true;
    if (value == "base32") return SpriteSheetType::Base32;
    if (value == "tall32x64") return SpriteSheetType::Tall32x64;
    ok = false;
    return SpriteSheetType::Base32;
}

EffectType ParseEffectType(const std::string& value, bool& ok) {
    ok = true;
    if (value == "increase_current_health") return EffectType::IncreaseCurrentHealth;
    if (value == "spawn_object") return EffectType::SpawnObject;
    ok = false;
    return EffectType::IncreaseCurrentHealth;
}

TileType ParseTerrainTileType(const std::string& value) {
    if (value == "tile_water") {
        return TileType::Water;
    }
    if (value == "team_spawn_point") {
        return TileType::SpawnPoint;
    }
    return TileType::Grass;
}

}  // namespace

uint32_t ObjectsDatabase::PackRgb(unsigned char r, unsigned char g, unsigned char b) {
    return (static_cast<uint32_t>(r) << 16u) | (static_cast<uint32_t>(g) << 8u) | static_cast<uint32_t>(b);
}

const ObjectPrototype* ObjectsDatabase::FindById(const std::string& id) const {
    auto it = by_id_.find(id);
    if (it == by_id_.end() || it->second >= prototypes_.size()) {
        return nullptr;
    }
    return &prototypes_[it->second];
}

const ObjectPrototype* ObjectsDatabase::FindByMapKey(unsigned char r, unsigned char g, unsigned char b) const {
    auto it = by_map_color_.find(PackRgb(r, g, b));
    if (it == by_map_color_.end() || it->second >= prototypes_.size()) {
        return nullptr;
    }
    return &prototypes_[it->second];
}

bool ObjectsDatabase::LoadFromFile(const std::string& path) {
    loaded_ = false;
    path_ = path;
    last_error_.clear();
    prototypes_.clear();
    by_id_.clear();
    by_map_color_.clear();

    std::ifstream file(path);
    if (!file.is_open()) {
        last_error_ = "failed to open objects database";
        TraceLog(LOG_WARNING, "ObjectsDatabase: %s (%s)", last_error_.c_str(), path.c_str());
        return false;
    }

    nlohmann::json json;
    try {
        file >> json;
    } catch (const std::exception& ex) {
        last_error_ = std::string("invalid JSON: ") + ex.what();
        TraceLog(LOG_ERROR, "ObjectsDatabase: %s", last_error_.c_str());
        return false;
    }

    const auto objects_it = json.find("objects");
    if (objects_it == json.end() || !objects_it->is_object()) {
        last_error_ = "missing required object key 'objects'";
        TraceLog(LOG_ERROR, "ObjectsDatabase: %s", last_error_.c_str());
        return false;
    }

    for (auto it = objects_it->begin(); it != objects_it->end(); ++it) {
        if (!it.value().is_object()) {
            last_error_ = "object prototype entry must be a JSON object";
            TraceLog(LOG_ERROR, "ObjectsDatabase: %s (id=%s)", last_error_.c_str(), it.key().c_str());
            return false;
        }

        ObjectPrototype proto;
        proto.id = it.key();

        const std::string type_str = it.value().value("type", "");
        bool type_ok = false;
        proto.type = ParseObjectType(type_str, type_ok);
        if (!type_ok) {
            last_error_ = "unknown object type '" + type_str + "'";
            TraceLog(LOG_ERROR, "ObjectsDatabase: %s (id=%s)", last_error_.c_str(), proto.id.c_str());
            return false;
        }

        const auto sprite_it = it.value().find("sprite");
        if (sprite_it == it.value().end() || !sprite_it->is_object()) {
            last_error_ = "missing required object key 'sprite'";
            TraceLog(LOG_ERROR, "ObjectsDatabase: %s (id=%s)", last_error_.c_str(), proto.id.c_str());
            return false;
        }

        const std::string sheet_str = sprite_it->value("sheet", "base32");
        bool sheet_ok = false;
        proto.sprite_sheet = ParseSpriteSheetType(sheet_str, sheet_ok);
        if (!sheet_ok) {
            last_error_ = "unknown sprite sheet type '" + sheet_str + "'";
            TraceLog(LOG_ERROR, "ObjectsDatabase: %s (id=%s)", last_error_.c_str(), proto.id.c_str());
            return false;
        }

        proto.idle_animation = sprite_it->value("idle", "");
        proto.death_animation = sprite_it->value("death", "");
        if (proto.idle_animation.empty()) {
            last_error_ = "missing sprite.idle animation key";
            TraceLog(LOG_ERROR, "ObjectsDatabase: %s (id=%s)", last_error_.c_str(), proto.id.c_str());
            return false;
        }

        proto.walkable = it.value().value("walkable", true);
        proto.stops_projectiles = it.value().value("stops_projectiles", false);

        const auto map_key_it = it.value().find("map_key");
        if (map_key_it != it.value().end() && !map_key_it->is_null()) {
            if (!map_key_it->is_array() || map_key_it->size() < 3) {
                last_error_ = "map_key must be RGB array or null";
                TraceLog(LOG_ERROR, "ObjectsDatabase: %s (id=%s)", last_error_.c_str(), proto.id.c_str());
                return false;
            }
            const int r = (*map_key_it)[0].get<int>();
            const int g = (*map_key_it)[1].get<int>();
            const int b = (*map_key_it)[2].get<int>();
            if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255) {
                last_error_ = "map_key RGB values must be in [0,255]";
                TraceLog(LOG_ERROR, "ObjectsDatabase: %s (id=%s)", last_error_.c_str(), proto.id.c_str());
                return false;
            }
            proto.has_map_key = true;
            proto.map_r = static_cast<unsigned char>(r);
            proto.map_g = static_cast<unsigned char>(g);
            proto.map_b = static_cast<unsigned char>(b);
        }

        if (proto.type == ObjectType::Terrain) {
            std::string terrain_tile_name = it.value().value("terrain_tile", "");
            if (terrain_tile_name.empty()) {
                if (proto.id == "water" || proto.idle_animation == "tile_water") {
                    terrain_tile_name = "tile_water";
                } else if (proto.id == "team_spawn_point") {
                    terrain_tile_name = "team_spawn_point";
                } else {
                    terrain_tile_name = "tile_grass";
                }
            }
            proto.terrain_tile = ParseTerrainTileType(terrain_tile_name);
            proto.terrain_is_spawn_point = (proto.terrain_tile == TileType::SpawnPoint);
        }

        if (proto.type == ObjectType::Destructible) {
            const auto destructible_it = it.value().find("destructible");
            if (destructible_it != it.value().end() && destructible_it->is_object()) {
                proto.destructible_hp = std::max(1, destructible_it->value("hp", 1));
                const auto on_destroy_it = destructible_it->find("on_destroy");
                if (on_destroy_it != destructible_it->end() && on_destroy_it->is_object()) {
                    const auto drops_it = on_destroy_it->find("drops");
                    if (drops_it != on_destroy_it->end() && drops_it->is_array()) {
                        for (const auto& drop_json : *drops_it) {
                            if (!drop_json.is_object()) {
                                last_error_ = "drop entry must be an object";
                                TraceLog(LOG_ERROR, "ObjectsDatabase: %s (id=%s)", last_error_.c_str(),
                                         proto.id.c_str());
                                return false;
                            }
                            DropEntry drop;
                            drop.object_id = drop_json.value("object_id", "");
                            drop.chance = drop_json.value("chance", 1.0f);
                            drop.min_count = std::max(1, drop_json.value("min", 1));
                            drop.max_count = std::max(1, drop_json.value("max", 1));
                            if (drop.object_id.empty()) {
                                last_error_ = "drop.object_id is required";
                                TraceLog(LOG_ERROR, "ObjectsDatabase: %s (id=%s)", last_error_.c_str(),
                                         proto.id.c_str());
                                return false;
                            }
                            if (drop.chance < 0.0f || drop.chance > 1.0f) {
                                last_error_ = "drop.chance must be in [0,1]";
                                TraceLog(LOG_ERROR, "ObjectsDatabase: %s (id=%s)", last_error_.c_str(),
                                         proto.id.c_str());
                                return false;
                            }
                            if (drop.max_count < drop.min_count) {
                                std::swap(drop.max_count, drop.min_count);
                            }
                            proto.on_destroy_drops.push_back(drop);
                        }
                    }
                }
            }
            if (proto.destructible_hp <= 0) {
                proto.destructible_hp = 1;
            }
        }

        if (proto.type == ObjectType::Consumable) {
            const auto consumable_it = it.value().find("consumable");
            if (consumable_it != it.value().end() && consumable_it->is_object()) {
                const auto effects_it = consumable_it->find("effects");
                if (effects_it != consumable_it->end() && effects_it->is_array()) {
                    for (const auto& effect_json : *effects_it) {
                        if (!effect_json.is_object()) {
                            last_error_ = "consumable effect entry must be an object";
                            TraceLog(LOG_ERROR, "ObjectsDatabase: %s (id=%s)", last_error_.c_str(),
                                     proto.id.c_str());
                            return false;
                        }
                        const std::string effect_str = effect_json.value("type", "");
                        bool effect_ok = false;
                        EffectSpec spec;
                        spec.type = ParseEffectType(effect_str, effect_ok);
                        if (!effect_ok) {
                            last_error_ = "unknown effect type '" + effect_str + "'";
                            TraceLog(LOG_ERROR, "ObjectsDatabase: %s (id=%s)", last_error_.c_str(), proto.id.c_str());
                            return false;
                        }

                        if (spec.type == EffectType::IncreaseCurrentHealth) {
                            spec.amount = effect_json.value("amount", 0);
                        } else if (spec.type == EffectType::SpawnObject) {
                            spec.object_id = effect_json.value("object_id", "");
                            spec.min_count = std::max(1, effect_json.value("min", 1));
                            spec.max_count = std::max(1, effect_json.value("max", 1));
                            if (spec.max_count < spec.min_count) {
                                std::swap(spec.max_count, spec.min_count);
                            }
                            if (spec.object_id.empty()) {
                                last_error_ = "spawn_object effect requires object_id";
                                TraceLog(LOG_ERROR, "ObjectsDatabase: %s (id=%s)", last_error_.c_str(),
                                         proto.id.c_str());
                                return false;
                            }
                        }
                        proto.consumable_effects.push_back(spec);
                    }
                }
            }
        }

        const size_t index = prototypes_.size();
        prototypes_.push_back(proto);
        by_id_[proto.id] = index;
    }

    for (size_t i = 0; i < prototypes_.size(); ++i) {
        const ObjectPrototype& proto = prototypes_[i];
        if (proto.has_map_key) {
            const uint32_t color_key = PackRgb(proto.map_r, proto.map_g, proto.map_b);
            const auto existing = by_map_color_.find(color_key);
            if (existing != by_map_color_.end()) {
                last_error_ = "duplicate map_key RGB in objects database";
                TraceLog(LOG_ERROR, "ObjectsDatabase: %s (%s and %s)", last_error_.c_str(),
                         prototypes_[existing->second].id.c_str(), proto.id.c_str());
                loaded_ = false;
                return false;
            }
            by_map_color_[color_key] = i;
        }

        for (const DropEntry& drop : proto.on_destroy_drops) {
            if (FindById(drop.object_id) == nullptr) {
                last_error_ = "drop references unknown object_id '" + drop.object_id + "'";
                TraceLog(LOG_ERROR, "ObjectsDatabase: %s (id=%s)", last_error_.c_str(), proto.id.c_str());
                loaded_ = false;
                return false;
            }
        }
        for (const EffectSpec& effect : proto.consumable_effects) {
            if (effect.type == EffectType::SpawnObject && FindById(effect.object_id) == nullptr) {
                last_error_ = "spawn_object effect references unknown object_id '" + effect.object_id + "'";
                TraceLog(LOG_ERROR, "ObjectsDatabase: %s (id=%s)", last_error_.c_str(), proto.id.c_str());
                loaded_ = false;
                return false;
            }
        }
    }

    loaded_ = true;
    TraceLog(LOG_INFO, "ObjectsDatabase: loaded %d prototypes from %s", static_cast<int>(prototypes_.size()),
             path.c_str());
    return true;
}
