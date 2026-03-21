#pragma once

#include <string>

#include "assets/objects_database.h"
#include "game/game_state.h"

class MapLoader {
  public:
    bool Load(const std::string& image_path, const ObjectsDatabase* objects_db, MapData& out_map) const;
};
