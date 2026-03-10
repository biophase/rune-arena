#include "assets/map_loader.h"

#include <fstream>
#include <string>
#include <unordered_map>

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

}  // namespace

bool MapLoader::Load(const std::string& image_path, const std::string& mapping_path, MapData& out_map) const {
    std::ifstream mapping_file(mapping_path);
    if (!mapping_file.is_open()) {
        TraceLog(LOG_ERROR, "Failed to open tile mapping: %s", mapping_path.c_str());
        return false;
    }

    nlohmann::json mapping_json;
    mapping_file >> mapping_json;

    std::unordered_map<RgbKey, TileType, RgbKeyHasher> color_to_tile;
    for (auto it = mapping_json.begin(); it != mapping_json.end(); ++it) {
        if (!it.value().is_array() || it.value().size() < 3) {
            continue;
        }
        const RgbKey key = {it.value()[0].get<int>(), it.value()[1].get<int>(), it.value()[2].get<int>()};
        color_to_tile[key] = TileTypeFromName(it.key());
    }

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
    out_map.spawn_points.clear();

    for (int y = 0; y < out_map.height; ++y) {
        for (int x = 0; x < out_map.width; ++x) {
            const Color pixel = pixels[y * out_map.width + x];
            const RgbKey key = {pixel.r, pixel.g, pixel.b};

            TileType tile = TileType::Unknown;
            auto it = color_to_tile.find(key);
            if (it != color_to_tile.end()) {
                tile = it->second;
            }

            out_map.tiles[static_cast<size_t>(y * out_map.width + x)] = tile;
            if (tile == TileType::SpawnPoint) {
                out_map.spawn_points.push_back({x, y});
            }
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
