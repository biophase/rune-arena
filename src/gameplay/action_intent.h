#pragma once

#include <string>

#include <raylib.h>

#include "net/network_messages.h"

struct ActionIntent {
    int actor_id = -1;
    int tick = 0;
    int seq = 0;
    Vector2 move = {0.0f, 0.0f};
    Vector2 aim_world = {0.0f, 0.0f};
    bool primary_pressed = false;
    bool mobility_pressed = false;
    int request_rune_type = -1;
    std::string request_item_id;
    bool toggle_inventory_mode = false;
    float local_dt = 0.0f;
};

inline ActionIntent BuildActionIntent(const ClientInputMessage& input) {
    ActionIntent intent;
    intent.actor_id = input.player_id;
    intent.tick = input.tick;
    intent.seq = input.seq;
    intent.move = {input.move_x, input.move_y};
    intent.aim_world = {input.aim_x, input.aim_y};
    intent.primary_pressed = input.primary_pressed;
    intent.mobility_pressed = input.grappling_pressed;
    intent.request_rune_type = input.request_rune_type;
    intent.request_item_id = input.request_item_id;
    intent.toggle_inventory_mode = input.toggle_inventory_mode;
    intent.local_dt = input.local_dt;
    return intent;
}
