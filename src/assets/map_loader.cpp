#include "assets/map_loader.h"

#include <unordered_set>

#include <raylib.h>

#include "core/constants.h"

bool MapLoader::Load(const std::string& image_path, const ObjectsDatabase* objects_db, MapData& out_map) const {
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
            const size_t index = static_cast<size_t>(y * out_map.width + x);

            TileType tile = TileType::Unknown;

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
                tile = TileType::Grass;
                const size_t warn_key = (static_cast<size_t>(pixel.r) << 16u) | (static_cast<size_t>(pixel.g) << 8u) |
                                        static_cast<size_t>(pixel.b);
                if (warned_unknown_colors.insert(warn_key).second) {
                    TraceLog(LOG_WARNING, "Map color not mapped in objects.json: [%d,%d,%d]. Falling back to grass.",
                             pixel.r, pixel.g, pixel.b);
                }
            }

            out_map.tiles[index] = tile;
        }
    }

    UnloadImageColors(pixels);
    UnloadImage(map_image);
    return true;
}
