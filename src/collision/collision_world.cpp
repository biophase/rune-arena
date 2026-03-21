#include "collision/collision_world.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <queue>
#include <unordered_set>
#include <vector>

#include <raymath.h>

namespace {

float ClampFloat(float value, float min_value, float max_value) {
    return std::max(min_value, std::min(value, max_value));
}

int64_t MakeCellKey(const GridCoord& cell) {
    return (static_cast<int64_t>(cell.x) << 32) ^ static_cast<uint32_t>(cell.y);
}

Rectangle CellAabb(const GridCoord& cell, int cell_size) {
    return Rectangle{static_cast<float>(cell.x * cell_size), static_cast<float>(cell.y * cell_size),
                     static_cast<float>(cell_size), static_cast<float>(cell_size)};
}

bool IntersectsAnyOccupiedCell(const Vector2& center, float radius, int cell_size,
                               const std::vector<GridCoord>& occupied_cells) {
    for (const GridCoord& cell : occupied_cells) {
        Vector2 normal = {0.0f, 0.0f};
        float penetration = 0.0f;
        if (CollisionWorld::CircleVsAabb(center, radius, CellAabb(cell, cell_size), normal, penetration)) {
            return true;
        }
    }
    return false;
}

bool FindClosestBoundaryExitInternal(const Vector2& center, float radius, int cell_size,
                                     const std::vector<GridCoord>& occupied_cells, Vector2& out_position) {
    if (occupied_cells.empty()) {
        return false;
    }

    std::unordered_set<int64_t> occupied_set;
    occupied_set.reserve(occupied_cells.size() * 2);
    for (const GridCoord& cell : occupied_cells) {
        occupied_set.insert(MakeCellKey(cell));
    }

    constexpr float kExitEpsilon = 0.01f;
    bool found = false;
    float best_distance_sq = std::numeric_limits<float>::max();
    Vector2 best_position = center;

    auto try_candidate = [&](Vector2 candidate) {
        if (IntersectsAnyOccupiedCell(candidate, radius, cell_size, occupied_cells)) {
            return;
        }
        const float dx = candidate.x - center.x;
        const float dy = candidate.y - center.y;
        const float distance_sq = dx * dx + dy * dy;
        if (!found || distance_sq < best_distance_sq) {
            found = true;
            best_distance_sq = distance_sq;
            best_position = candidate;
        }
    };

    for (const GridCoord& cell : occupied_cells) {
        const float x0 = static_cast<float>(cell.x * cell_size);
        const float y0 = static_cast<float>(cell.y * cell_size);
        const float x1 = x0 + static_cast<float>(cell_size);
        const float y1 = y0 + static_cast<float>(cell_size);

        if (!occupied_set.count(MakeCellKey({cell.x - 1, cell.y}))) {
            try_candidate({x0 - radius - kExitEpsilon, ClampFloat(center.y, y0, y1)});
        }
        if (!occupied_set.count(MakeCellKey({cell.x + 1, cell.y}))) {
            try_candidate({x1 + radius + kExitEpsilon, ClampFloat(center.y, y0, y1)});
        }
        if (!occupied_set.count(MakeCellKey({cell.x, cell.y - 1}))) {
            try_candidate({ClampFloat(center.x, x0, x1), y0 - radius - kExitEpsilon});
        }
        if (!occupied_set.count(MakeCellKey({cell.x, cell.y + 1}))) {
            try_candidate({ClampFloat(center.x, x0, x1), y1 + radius + kExitEpsilon});
        }
    }

    if (!found) {
        return false;
    }

    out_position = best_position;
    return true;
}

Vector2 MoveTowardResolvedPosition(const Vector2& current, const Vector2& resolved) {
    const Vector2 delta = Vector2Subtract(resolved, current);
    const float distance = Vector2Length(delta);
    if (distance <= Constants::kCollisionResolveSnapDistance || distance <= 0.0001f) {
        return resolved;
    }

    const float step =
        std::min(distance, std::max(Constants::kCollisionResolveMinStep, distance * Constants::kCollisionResolveBlendFactor));
    return Vector2Add(current, Vector2Scale(delta, step / distance));
}

}  // namespace

bool CollisionWorld::CircleVsCircle(const Vector2& a_center, float a_radius, const Vector2& b_center,
                                    float b_radius, Vector2& out_normal, float& out_penetration) {
    const Vector2 delta = {b_center.x - a_center.x, b_center.y - a_center.y};
    const float distance_sq = delta.x * delta.x + delta.y * delta.y;
    const float radius_sum = a_radius + b_radius;
    if (distance_sq >= radius_sum * radius_sum) {
        return false;
    }

    const float distance = sqrtf(distance_sq);
    if (distance <= 0.0001f) {
        out_normal = {1.0f, 0.0f};
        out_penetration = radius_sum;
        return true;
    }

    out_normal = {delta.x / distance, delta.y / distance};
    out_penetration = radius_sum - distance;
    return true;
}

bool CollisionWorld::CircleVsAabb(const Vector2& center, float radius, const Rectangle& box,
                                  Vector2& out_normal, float& out_penetration) {
    const Vector2 closest = {
        ClampFloat(center.x, box.x, box.x + box.width),
        ClampFloat(center.y, box.y, box.y + box.height),
    };

    const Vector2 delta = {center.x - closest.x, center.y - closest.y};
    const float distance_sq = delta.x * delta.x + delta.y * delta.y;
    if (distance_sq >= radius * radius) {
        return false;
    }

    const float distance = sqrtf(distance_sq);
    if (distance <= 0.0001f) {
        const float dist_left = center.x - box.x;
        const float dist_right = (box.x + box.width) - center.x;
        const float dist_top = center.y - box.y;
        const float dist_bottom = (box.y + box.height) - center.y;

        out_normal = {-1.0f, 0.0f};
        float min_exit = dist_left;
        if (dist_right < min_exit) {
            min_exit = dist_right;
            out_normal = {1.0f, 0.0f};
        }
        if (dist_top < min_exit) {
            min_exit = dist_top;
            out_normal = {0.0f, -1.0f};
        }
        if (dist_bottom < min_exit) {
            min_exit = dist_bottom;
            out_normal = {0.0f, 1.0f};
        }
        out_penetration = radius + std::max(0.0f, min_exit);
        return true;
    }

    out_normal = {delta.x / distance, delta.y / distance};
    out_penetration = radius - distance;
    return true;
}

bool CollisionWorld::FindClosestBoundaryExitForTileComponent(const Vector2& center, float radius, int cell_size,
                                                             const std::vector<GridCoord>& occupied_cells,
                                                             Vector2& out_position) {
    return FindClosestBoundaryExitInternal(center, radius, cell_size, occupied_cells, out_position);
}

void CollisionWorld::ResolvePlayerVsWorldLocal(const MapData& map, Player& player, bool collide_with_water) {
    const int cell_size = map.cell_size;
    if (collide_with_water) {
        const GridCoord min_cell = {static_cast<int>((player.pos.x - player.radius) / cell_size),
                                    static_cast<int>((player.pos.y - player.radius) / cell_size)};
        const GridCoord max_cell = {static_cast<int>((player.pos.x + player.radius) / cell_size),
                                    static_cast<int>((player.pos.y + player.radius) / cell_size)};

        for (int y = min_cell.y; y <= max_cell.y; ++y) {
            for (int x = min_cell.x; x <= max_cell.x; ++x) {
                const GridCoord cell = {x, y};
                if (!map.IsInside(cell) || map.GetTile(cell) != TileType::Water) {
                    continue;
                }

                Vector2 normal = {0.0f, 0.0f};
                float penetration = 0.0f;
                if (!CircleVsAabb(player.pos, player.radius, CellAabb(cell, cell_size), normal, penetration)) {
                    continue;
                }

                player.pos.x += normal.x * penetration;
                player.pos.y += normal.y * penetration;

                const float velocity_dot = player.vel.x * normal.x + player.vel.y * normal.y;
                if (velocity_dot < 0.0f) {
                    player.vel.x -= normal.x * velocity_dot;
                    player.vel.y -= normal.y * velocity_dot;
                }
            }
        }
    }

    if (map.width <= 0 || map.height <= 0 || map.cell_size <= 0) {
        return;
    }

    const float map_width = static_cast<float>(map.width * map.cell_size);
    const float map_height = static_cast<float>(map.height * map.cell_size);
    const float min_x = player.radius;
    const float min_y = player.radius;
    const float max_x = std::max(min_x, map_width - player.radius);
    const float max_y = std::max(min_y, map_height - player.radius);

    if (player.pos.x < min_x) {
        player.pos.x = min_x;
        if (player.vel.x < 0.0f) {
            player.vel.x = 0.0f;
        }
    } else if (player.pos.x > max_x) {
        player.pos.x = max_x;
        if (player.vel.x > 0.0f) {
            player.vel.x = 0.0f;
        }
    }

    if (player.pos.y < min_y) {
        player.pos.y = min_y;
        if (player.vel.y < 0.0f) {
            player.vel.y = 0.0f;
        }
    } else if (player.pos.y > max_y) {
        player.pos.y = max_y;
        if (player.vel.y > 0.0f) {
            player.vel.y = 0.0f;
        }
    }
}

void CollisionWorld::ResolvePlayerVsWorld(const MapData& map, Player& player, bool collide_with_water) {
    const int cell_size = map.cell_size;
    if (collide_with_water) {
        const GridCoord min_cell = {static_cast<int>((player.pos.x - player.radius) / cell_size),
                                    static_cast<int>((player.pos.y - player.radius) / cell_size)};
        const GridCoord max_cell = {static_cast<int>((player.pos.x + player.radius) / cell_size),
                                    static_cast<int>((player.pos.y + player.radius) / cell_size)};

        std::vector<GridCoord> overlap_seeds;
        overlap_seeds.reserve(8);

        for (int y = min_cell.y; y <= max_cell.y; ++y) {
            for (int x = min_cell.x; x <= max_cell.x; ++x) {
                const GridCoord cell = {x, y};
                if (!map.IsInside(cell)) {
                    continue;
                }

                const TileType tile = map.GetTile(cell);
                if (tile != TileType::Water) {
                    continue;
                }

                const Rectangle aabb = {static_cast<float>(x * cell_size), static_cast<float>(y * cell_size),
                                        static_cast<float>(cell_size), static_cast<float>(cell_size)};

                Vector2 normal = {0.0f, 0.0f};
                float penetration = 0.0f;
                if (!CircleVsAabb(player.pos, player.radius, aabb, normal, penetration)) {
                    continue;
                }
                overlap_seeds.push_back(cell);
            }
        }

        std::unordered_set<int64_t> visited;
        visited.reserve(overlap_seeds.size() * 8 + 8);
        for (const GridCoord& seed : overlap_seeds) {
            const int64_t seed_key = MakeCellKey(seed);
            if (visited.count(seed_key)) {
                continue;
            }

            std::queue<GridCoord> queue;
            std::vector<GridCoord> component;
            queue.push(seed);
            visited.insert(seed_key);

            while (!queue.empty()) {
                const GridCoord current = queue.front();
                queue.pop();
                component.push_back(current);

                const GridCoord neighbors[4] = {
                    {current.x - 1, current.y},
                    {current.x + 1, current.y},
                    {current.x, current.y - 1},
                    {current.x, current.y + 1},
                };
                for (const GridCoord& neighbor : neighbors) {
                    if (!map.IsInside(neighbor) || map.GetTile(neighbor) != TileType::Water) {
                        continue;
                    }
                    const int64_t key = MakeCellKey(neighbor);
                    if (visited.insert(key).second) {
                        queue.push(neighbor);
                    }
                }
            }

            if (!IntersectsAnyOccupiedCell(player.pos, player.radius, cell_size, component)) {
                continue;
            }

            Vector2 resolved = player.pos;
            if (!FindClosestBoundaryExitInternal(player.pos, player.radius, cell_size, component, resolved)) {
                continue;
            }

            const Vector2 new_pos = MoveTowardResolvedPosition(player.pos, resolved);
            const Vector2 push = Vector2Subtract(new_pos, player.pos);
            player.pos = new_pos;
            if (Vector2LengthSqr(push) > 0.0001f) {
                const Vector2 normal = Vector2Normalize(push);
                const float velocity_dot = player.vel.x * normal.x + player.vel.y * normal.y;
                if (velocity_dot < 0.0f) {
                    player.vel.x -= normal.x * velocity_dot;
                    player.vel.y -= normal.y * velocity_dot;
                }
            }
        }
    }

    if (map.width <= 0 || map.height <= 0 || map.cell_size <= 0) {
        return;
    }

    const float map_width = static_cast<float>(map.width * map.cell_size);
    const float map_height = static_cast<float>(map.height * map.cell_size);

    const float min_x = player.radius;
    const float min_y = player.radius;
    const float max_x = std::max(min_x, map_width - player.radius);
    const float max_y = std::max(min_y, map_height - player.radius);

    if (player.pos.x < min_x) {
        player.pos.x = min_x;
        if (player.vel.x < 0.0f) {
            player.vel.x = 0.0f;
        }
    } else if (player.pos.x > max_x) {
        player.pos.x = max_x;
        if (player.vel.x > 0.0f) {
            player.vel.x = 0.0f;
        }
    }

    if (player.pos.y < min_y) {
        player.pos.y = min_y;
        if (player.vel.y < 0.0f) {
            player.vel.y = 0.0f;
        }
    } else if (player.pos.y > max_y) {
        player.pos.y = max_y;
        if (player.vel.y > 0.0f) {
            player.vel.y = 0.0f;
        }
    }
}
