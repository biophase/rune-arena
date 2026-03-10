#include "config/controls_bindings.h"

#include <array>

namespace {

constexpr int kMouseBindingBase = -1000;

}  // namespace

int EncodeMouseBinding(int mouse_button) { return kMouseBindingBase - mouse_button; }

bool IsMouseBinding(int binding_code) { return binding_code <= kMouseBindingBase; }

int DecodeMouseBinding(int binding_code) { return kMouseBindingBase - binding_code; }

bool IsBindingDown(int binding_code) {
    if (IsMouseBinding(binding_code)) {
        return IsMouseButtonDown(DecodeMouseBinding(binding_code));
    }
    return IsKeyDown(binding_code);
}

bool IsBindingPressed(int binding_code) {
    if (IsMouseBinding(binding_code)) {
        return IsMouseButtonPressed(DecodeMouseBinding(binding_code));
    }
    return IsKeyPressed(binding_code);
}

std::optional<int> PollAnyBindingPressed() {
    constexpr std::array<int, 7> mouse_buttons = {MOUSE_LEFT_BUTTON, MOUSE_RIGHT_BUTTON, MOUSE_MIDDLE_BUTTON,
                                                   MOUSE_BUTTON_SIDE, MOUSE_BUTTON_EXTRA, MOUSE_BUTTON_FORWARD,
                                                   MOUSE_BUTTON_BACK};
    for (int button : mouse_buttons) {
        if (IsMouseButtonPressed(button)) {
            return EncodeMouseBinding(button);
        }
    }

    const int key = GetKeyPressed();
    if (key != 0) {
        return key;
    }
    return std::nullopt;
}

std::string BindingToString(int binding_code) {
    if (IsMouseBinding(binding_code)) {
        switch (DecodeMouseBinding(binding_code)) {
            case MOUSE_LEFT_BUTTON:
                return "Mouse Left";
            case MOUSE_RIGHT_BUTTON:
                return "Mouse Right";
            case MOUSE_MIDDLE_BUTTON:
                return "Mouse Middle";
            case MOUSE_BUTTON_SIDE:
                return "Mouse Side";
            case MOUSE_BUTTON_EXTRA:
                return "Mouse Extra";
            case MOUSE_BUTTON_FORWARD:
                return "Mouse Forward";
            case MOUSE_BUTTON_BACK:
                return "Mouse Back";
            default:
                return "Mouse ?";
        }
    }

    const char* key_name = GetKeyName(binding_code);
    if (key_name != nullptr && key_name[0] != '\0') {
        return std::string(key_name);
    }
    return TextFormat("Key %d", binding_code);
}
