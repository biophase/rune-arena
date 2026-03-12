#include "assets/map_loader.h"

#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <nlohmann/json.hpp>
#include <raylib.h>

#include "core/constants.h"

namespace {

struct RgbKey {
    int r = 0;
    int g = 0;
    int b = 0;

    bool operator==(const RgbKey& other) const { return r == other.r && g == other.g && b == other.b; }
};

struct RgbKeyHasher {
    size_t operator()(const RgbKey& key) const {
        return static_cast<size_t>((key.r << 16) ^ (key.g << 8) ^ key.b);
    }
};

struct TileMappingEntry {
    TileType base_tile = TileType::Unknown;
    std::string decoration_animation;
};

TileType LegacyTileTypeFromName(const std::string& tile_name) {
    if (tile_name == "tile_grass") {
        return TileType::Grass;
    }
    if (tile_name == "tile_water") {
        return TileType::Water;
    }
    if (tile_name == "team_spawn_point") {
        return TileType::SpawnPoint;
    }
    return TileType::Unknown;
}

std::unordered_map<RgbKey, TileMappingEntry, RgbKeyHasher> LoadLegacyMapping(const std::string& mapping_path) {
    std::unordered_map<RgbKey, TileMappingEntry, RgbKeyHasher> color_to_tile;

    std::ifstream mapping_file(mapping_path);
    if (!mapping_file.is_open()) {
        return color_to_tile;
    }

    nlohmann::json mapping_json;
    try {
        mapping_file >> mapping_json;
    } catch (const std::exception& ex) {
        TraceLog(LOG_WARNING, "Failed to parse legacy tile mapping JSON (%s): %s", mapping_path.c_str(), ex.what());
        return color_to_tile;
    }

    for (auto it = mapping_json.begin(); it != mapping_json.end(); ++it) {
        if (!it.value().is_array() || it.value().size() < 3) {
            continue;
        }
        const RgbKey key = {it.value()[0].get<int>(), it.value()[1].get<int>(), it.value()[2].get<int>()};
        TileMappingEntry entry;
        entry.base_tile = LegacyTileTypeFromName(it.key());
        if (entry.base_tile == TileType::Unknown) {
            // Legacy non-terrain markers are decorations on top of grass.
            entry.base_tile = TileType::Grass;
            entry.decoration_animation = it.key();
        }
        color_to_tile[key] = entry;
    }

    return color_to_tile;
}

}  // namespace

bool MapLoader::Load(const std::string& image_path, const std::string& mapping_path, const ObjectsDatabase* objects_db,
                     MapData& out_map) const {
    const auto legacy_mapping = LoadLegacyMapping(mapping_path);

    Image map_image = LoadImage(image_path.c_str());
    if (!map_image.data) {
        TraceLog(LOG_ERROR, "Failed to load map image: %s", image_path.c_str());
        return false;
    }

    Color* pixels = LoadImageColors(map_image);
    if (!pixels) {
        UnloadImage(map_image);
        return false;
    }

    out_map.width = map_image.width;
    out_map.height = map_image.height;
    out_map.cell_size = Constants::kRuneCellSize;
    out_map.tiles.assign(static_cast<size_t>(out_map.width * out_map.height), TileType::Unknown);
    out_map.decorations.assign(static_cast<size_t>(out_map.width * out_map.height), "");
    out_map.object_seeds.clear();
    out_map.spawn_points.clear();

    const bool objects_loaded = (objects_db != nullptr && objects_db->IsLoaded());
    std::unordered_set<size_t> warned_unknown_colors;

    for (int y = 0; y < out_map.height; ++y) {
        for (int x = 0; x < out_map.width; ++x) {
            const Color pixel = pixels[y * out_map.width + x];
            const RgbKey key = {pixel.r, pixel.g, pixel.b};
            const size_t index = static_cast<size_t>(y * out_map.width + x);

            TileType tile = TileType::Unknown;
            std::string decoration_animation;

            if (objects_loaded) {
                const ObjectPrototype* proto = objects_db->FindByMapKey(pixel.r, pixel.g, pixel.b);
                if (proto != nullptr) {
                    if (proto->type == ObjectType::Terrain) {
                        tile = proto->terrain_tile;
                    } else {
                        tile = TileType::Grass;
                        out_map.object_seeds.push_back(MapObjectSeed{proto->id, {x, y}});
                    }
                    if (proto->terrain_is_spawn_point || proto->terrain_tile == TileType::SpawnPoint) {
                        out_map.spawn_points.push_back({x, y});
                    }
                }
            }

            if (tile == TileType::Unknown) {
                const auto legacy_it = legacy_mapping.find(key);
                if (legacy_it != legacy_mapping.end()) {
                    tile = legacy_it->second.base_tile;
                    decoration_animation = legacy_it->second.decoration_animation;
                    if (tile == TileType::SpawnPoint) {
                        out_map.spawn_points.push_back({x, y});
                    }
                }
            }

            if (tile == TileType::Unknown) {
                // Incremental migration safety: unknown map colors default to grass.
                tile = TileType::Grass;
                const size_t warn_key = (static_cast<size_t>(pixel.r) << 16u) | (static_cast<size_t>(pixel.g) << 8u) |
                                        static_cast<size_t>(pixel.b);
                if (warned_unknown_colors.insert(warn_key).second) {
                    TraceLog(LOG_WARNING,
                             "Map color not mapped in objects/legacy mapping: [%d,%d,%d]. Falling back to grass.",
                             pixel.r, pixel.g, pixel.b);
                }
            }

            out_map.tiles[index] = tile;
            out_map.decorations[index] = decoration_animation;
        }
    }

    UnloadImageColors(pixels);
    UnloadImage(map_image);
    return true;
}

TileType MapLoader::TileTypeFromName(const std::string& tile_name) {
    if (tile_name == "tile_grass") {
        return TileType::Grass;
    }
    if (tile_name == "tile_water") {
        return TileType::Water;
    }
    if (tile_name == "team_spawn_point") {
        return TileType::SpawnPoint;
    }
    return TileType::Unknown;
}
