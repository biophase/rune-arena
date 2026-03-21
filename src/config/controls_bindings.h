#pragma once

#include <optional>
#include <string>

#include <raylib.h>

struct ControlsBindings {
    int move_left = KEY_A;
    int move_right = KEY_D;
    int move_up = KEY_W;
    int move_down = KEY_S;
    int primary_action = -1000;   // Mouse left encoded
    int grappling_hook_action = -1001;  // Mouse right encoded
    int select_rune_slot_1 = KEY_ONE;
    int select_rune_slot_2 = KEY_TWO;
    int select_rune_slot_3 = KEY_THREE;
    int select_rune_slot_4 = KEY_FOUR;
    int activate_item_slot_1 = KEY_FIVE;
    int activate_item_slot_2 = KEY_SIX;
    int activate_item_slot_3 = KEY_SEVEN;
    int activate_item_slot_4 = KEY_EIGHT;
    int toggle_inventory_mode = KEY_TAB;
};

int EncodeMouseBinding(int mouse_button);
bool IsMouseBinding(int binding_code);
int DecodeMouseBinding(int binding_code);

bool IsBindingDown(int binding_code);
bool IsBindingPressed(int binding_code);
std::optional<int> PollAnyBindingPressed();
std::string BindingToString(int binding_code);
