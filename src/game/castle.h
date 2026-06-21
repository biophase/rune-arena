#pragma once

#include "game/game_types.h"

struct CastleState {
    int id = -1;
    int team = 0;
    GridCoord cell;
    int map_object_id = -1;
    int level = 1;
    float total_energy = 0.0f;
    float energy_into_current_level = 0.0f;
    float energy_needed_for_next_level = 100.0f;
    int charge_port_offset_x = 0;
    int charge_port_offset_y = 0;
};
