#pragma once

#include <raylib.h>

#include "game/game_state.h"

class CollisionWorld {
  public:
    static bool CircleVsCircle(const Vector2& a_center, float a_radius, const Vector2& b_center,
                               float b_radius, Vector2& out_normal, float& out_penetration);

    static bool CircleVsAabb(const Vector2& center, float radius, const Rectangle& box,
                             Vector2& out_normal, float& out_penetration);

    static void ResolvePlayerVsWorld(const MapData& map, Player& player);
};
