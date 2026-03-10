#pragma once

#include <string>

#include "game/game_state.h"

class MapLoader {
  public:
    bool Load(const std::string& image_path, const std::string& mapping_path, MapData& out_map) const;

  private:
    static TileType TileTypeFromName(const std::string& tile_name);
};
