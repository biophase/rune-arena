#pragma once

#include <string>
#include <vector>

#include <raylib.h>

#include "game/game_types.h"
#include "game/damage_popup.h"
#include "game/ice_wall.h"
#include "game/explosion.h"
#include "game/match_state.h"
#include "game/player.h"
#include "game/projectile.h"
#include "game/rune.h"
#include "particles/particle.h"

struct MapData {
    int width = 0;
    int height = 0;
    int cell_size = 32;
    std::vector<TileType> tiles;
    std::vector<std::string> decorations;
    std::vector<GridCoord> spawn_points;

    bool IsInside(const GridCoord& cell) const {
        return cell.x >= 0 && cell.y >= 0 && cell.x < width && cell.y < height;
    }

    TileType GetTile(const GridCoord& cell) const {
        if (!IsInside(cell)) {
            return TileType::Unknown;
        }
        return tiles[static_cast<size_t>(cell.y * width + cell.x)];
    }

    Vector2 CellCenterWorld(const GridCoord& cell) const {
        return {
            (static_cast<float>(cell.x) + 0.5f) * static_cast<float>(cell_size),
            (static_cast<float>(cell.y) + 0.5f) * static_cast<float>(cell_size),
        };
    }
};

struct GameState {
    MatchState match;
    std::vector<Player> players;
    std::vector<Rune> runes;
    std::vector<Projectile> projectiles;
    std::vector<Explosion> explosions;
    std::vector<IceWallPiece> ice_walls;
    std::vector<Particle> particles;
    std::vector<DamagePopup> damage_popups;
    MapData map;

    int local_player_id = -1;
    int next_entity_id = 1;
    int next_rune_placement_order = 1;
};
