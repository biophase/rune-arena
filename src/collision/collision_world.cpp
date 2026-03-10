#include "collision/collision_world.h"

#include <algorithm>

namespace {

float ClampFloat(float value, float min_value, float max_value) {
    return std::max(min_value, std::min(value, max_value));
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
        out_normal = {0.0f, -1.0f};
        out_penetration = radius;
        return true;
    }

    out_normal = {delta.x / distance, delta.y / distance};
    out_penetration = radius - distance;
    return true;
}

void CollisionWorld::ResolvePlayerVsWorld(const MapData& map, Player& player) {
    const int cell_size = map.cell_size;
    const GridCoord min_cell = {static_cast<int>((player.pos.x - player.radius) / cell_size),
                                static_cast<int>((player.pos.y - player.radius) / cell_size)};
    const GridCoord max_cell = {static_cast<int>((player.pos.x + player.radius) / cell_size),
                                static_cast<int>((player.pos.y + player.radius) / cell_size)};

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

            player.pos.x += normal.x * penetration;
            player.pos.y += normal.y * penetration;

            const float velocity_dot = player.vel.x * normal.x + player.vel.y * normal.y;
            if (velocity_dot < 0.0f) {
                player.vel.x -= normal.x * velocity_dot;
                player.vel.y -= normal.y * velocity_dot;
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
