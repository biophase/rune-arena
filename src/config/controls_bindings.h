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
    int select_fire_rune = KEY_ONE;
    int select_water_rune = KEY_TWO;
};

int EncodeMouseBinding(int mouse_button);
bool IsMouseBinding(int binding_code);
int DecodeMouseBinding(int binding_code);

bool IsBindingDown(int binding_code);
bool IsBindingPressed(int binding_code);
std::optional<int> PollAnyBindingPressed();
std::string BindingToString(int binding_code);
