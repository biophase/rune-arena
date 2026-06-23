#include "assets/map_loader.h"

#include <filesystem>
#include <unordered_set>

#include <raylib.h>

#include "core/constants.h"

namespace {

namespace fs = std::filesystem;

size_t PackWarnColor(const Color& pixel) {
    return (static_cast<size_t>(pixel.r) << 16u) | (static_cast<size_t>(pixel.g) << 8u) | static_cast<size_t>(pixel.b);
}

std::string BuildLayeredTerrainPath(const fs::path& map_dir) {
    const std::string map_name = map_dir.filename().string();
    return (map_dir / "exports" / (map_name + "-terrain.png")).string();
}

std::string BuildLayeredObjectsPath(const fs::path& map_dir) {
    const std::string map_name = map_dir.filename().string();
    return (map_dir / "exports" / (map_name + "-objects.png")).string();
}

bool LoadImagePixels(const std::string& image_path, Image* out_image, Color** out_pixels) {
    if (out_image == nullptr || out_pixels == nullptr) {
        return false;
    }
    *out_image = LoadImage(image_path.c_str());
    if (!out_image->data) {
        TraceLog(LOG_ERROR, "Failed to load map image: %s", image_path.c_str());
        return false;
    }
    *out_pixels = LoadImageColors(*out_image);
    if (*out_pixels == nullptr) {
        UnloadImage(*out_image);
        *out_image = {};
        return false;
    }
    return true;
}

void ResetMapData(int width, int height, MapData& out_map) {
    out_map.width = width;
    out_map.height = height;
    out_map.cell_size = Constants::kRuneCellSize;
    out_map.tiles.assign(static_cast<size_t>(out_map.width * out_map.height), TileType::Unknown);
    out_map.decorations.assign(static_cast<size_t>(out_map.width * out_map.height), "");
    out_map.object_seeds.clear();
    out_map.spawn_points.clear();
}

bool LoadLegacyMap(const std::string& image_path, const ObjectsDatabase* objects_db, MapData& out_map) {
    Image map_image = {};
    Color* pixels = nullptr;
    if (!LoadImagePixels(image_path, &map_image, &pixels)) {
        return false;
    }

    ResetMapData(map_image.width, map_image.height, out_map);

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
                        tile = proto->has_terrain_tile_override ? proto->terrain_tile : TileType::Grass;
                        out_map.object_seeds.push_back(MapObjectSeed{proto->id, {x, y}});
                    }
                    if (proto->terrain_is_spawn_point || proto->terrain_tile == TileType::SpawnPoint) {
                        out_map.spawn_points.push_back({x, y});
                    }
                }
            }

            if (tile == TileType::Unknown) {
                tile = TileType::Grass;
                const size_t warn_key = PackWarnColor(pixel);
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

bool LoadLayeredMap(const fs::path& map_dir, const ObjectsDatabase* objects_db, MapData& out_map) {
    const std::string terrain_path = BuildLayeredTerrainPath(map_dir);
    const std::string objects_path = BuildLayeredObjectsPath(map_dir);

    Image terrain_image = {};
    Color* terrain_pixels = nullptr;
    if (!LoadImagePixels(terrain_path, &terrain_image, &terrain_pixels)) {
        return false;
    }

    Image objects_image = {};
    Color* objects_pixels = nullptr;
    if (!LoadImagePixels(objects_path, &objects_image, &objects_pixels)) {
        UnloadImageColors(terrain_pixels);
        UnloadImage(terrain_image);
        return false;
    }

    if (terrain_image.width != objects_image.width || terrain_image.height != objects_image.height) {
        TraceLog(LOG_ERROR, "Layered map dimensions mismatch: %s and %s", terrain_path.c_str(), objects_path.c_str());
        UnloadImageColors(terrain_pixels);
        UnloadImage(terrain_image);
        UnloadImageColors(objects_pixels);
        UnloadImage(objects_image);
        return false;
    }

    ResetMapData(terrain_image.width, terrain_image.height, out_map);

    const bool objects_loaded = (objects_db != nullptr && objects_db->IsLoaded());
    std::unordered_set<size_t> warned_unknown_terrain_colors;
    std::unordered_set<size_t> warned_unknown_object_colors;
    std::unordered_set<size_t> warned_terrain_in_object_layer_colors;

    for (int y = 0; y < out_map.height; ++y) {
        for (int x = 0; x < out_map.width; ++x) {
            const Color pixel = terrain_pixels[y * out_map.width + x];
            const size_t index = static_cast<size_t>(y * out_map.width + x);

            TileType tile = TileType::Grass;
            if (objects_loaded) {
                const ObjectPrototype* proto = objects_db->FindByMapKey(pixel.r, pixel.g, pixel.b);
                if (proto != nullptr && proto->type == ObjectType::Terrain) {
                    tile = proto->terrain_tile;
                    if (proto->terrain_is_spawn_point || proto->terrain_tile == TileType::SpawnPoint) {
                        out_map.spawn_points.push_back({x, y});
                    }
                } else {
                    const size_t warn_key = PackWarnColor(pixel);
                    if (warned_unknown_terrain_colors.insert(warn_key).second) {
                        TraceLog(LOG_WARNING,
                                 "Terrain layer color not mapped to terrain in objects.json: [%d,%d,%d]. Falling back to grass.",
                                 pixel.r, pixel.g, pixel.b);
                    }
                }
            }
            out_map.tiles[index] = tile;
        }
    }

    if (objects_loaded) {
        for (int y = 0; y < out_map.height; ++y) {
            for (int x = 0; x < out_map.width; ++x) {
                const Color pixel = objects_pixels[y * out_map.width + x];
                if (pixel.a == 0) {
                    continue;
                }

                const ObjectPrototype* proto = objects_db->FindByMapKey(pixel.r, pixel.g, pixel.b);
                if (proto == nullptr) {
                    const size_t warn_key = PackWarnColor(pixel);
                    if (warned_unknown_object_colors.insert(warn_key).second) {
                        TraceLog(LOG_WARNING, "Objects layer color not mapped in objects.json: [%d,%d,%d]. Ignoring.",
                                 pixel.r, pixel.g, pixel.b);
                    }
                    continue;
                }

                if (proto->type == ObjectType::Terrain) {
                    if (proto->terrain_is_spawn_point || proto->terrain_tile == TileType::SpawnPoint) {
                        const size_t index = static_cast<size_t>(y * out_map.width + x);
                        out_map.tiles[index] = proto->terrain_tile;
                        out_map.spawn_points.push_back({x, y});
                        continue;
                    }
                    const size_t warn_key = PackWarnColor(pixel);
                    if (warned_terrain_in_object_layer_colors.insert(warn_key).second) {
                        TraceLog(LOG_WARNING,
                                 "Terrain prototype color used in objects layer: [%d,%d,%d]. Ignoring object-layer terrain.",
                                 pixel.r, pixel.g, pixel.b);
                    }
                    continue;
                }

                const size_t index = static_cast<size_t>(y * out_map.width + x);
                if (proto->has_terrain_tile_override) {
                    out_map.tiles[index] = proto->terrain_tile;
                    if (proto->terrain_is_spawn_point || proto->terrain_tile == TileType::SpawnPoint) {
                        out_map.spawn_points.push_back({x, y});
                    }
                }
                out_map.object_seeds.push_back(MapObjectSeed{proto->id, {x, y}});
            }
        }
    }

    UnloadImageColors(terrain_pixels);
    UnloadImage(terrain_image);
    UnloadImageColors(objects_pixels);
    UnloadImage(objects_image);
    return true;
}

}  // namespace

bool MapLoader::Load(const std::string& image_path, const ObjectsDatabase* objects_db, MapData& out_map) const {
    const fs::path input_path(image_path);
    if (!image_path.empty() && fs::exists(input_path) && fs::is_directory(input_path)) {
        return LoadLayeredMap(input_path, objects_db, out_map);
    }
    return LoadLegacyMap(image_path, objects_db, out_map);
}
