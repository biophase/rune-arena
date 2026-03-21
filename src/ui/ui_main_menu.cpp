#include "ui/ui_main_menu.h"

#include <array>
#include <cstdio>

#include <raylib.h>
#include <raygui.h>

namespace {

enum class MainMenuPage {
    Home,
    Settings,
    Controls,
};

struct ActionRow {
    const char* label;
    int ControlsBindings::*member;
};

constexpr std::array<ActionRow, 15> kActionRows = {{
    {"Move Left", &ControlsBindings::move_left},
    {"Move Right", &ControlsBindings::move_right},
    {"Move Up", &ControlsBindings::move_up},
    {"Move Down", &ControlsBindings::move_down},
    {"Primary Action", &ControlsBindings::primary_action},
    {"Grappling Hook", &ControlsBindings::grappling_hook_action},
    {"Rune Slot 1", &ControlsBindings::select_rune_slot_1},
    {"Rune Slot 2", &ControlsBindings::select_rune_slot_2},
    {"Rune Slot 3", &ControlsBindings::select_rune_slot_3},
    {"Rune Slot 4", &ControlsBindings::select_rune_slot_4},
    {"Item Slot 1", &ControlsBindings::activate_item_slot_1},
    {"Item Slot 2", &ControlsBindings::activate_item_slot_2},
    {"Item Slot 3", &ControlsBindings::activate_item_slot_3},
    {"Item Slot 4", &ControlsBindings::activate_item_slot_4},
    {"Inventory Toggle", &ControlsBindings::toggle_inventory_mode},
}};

}  // namespace

MainMenuUiResult DrawMainMenu(char* player_name_buffer, int player_name_buffer_size, char* join_ip_buffer,
                              int join_ip_buffer_size, const std::vector<DiscoveredHost>& discovered_hosts,
                              const std::string& config_path, const ControlsBindings& current_bindings,
                              const std::string& controls_path, bool show_network_debug_panel,
                              const std::string& status_message, bool status_is_error) {
    MainMenuUiResult result;
    result.controls_bindings = current_bindings;
    result.show_network_debug_panel = show_network_debug_panel;

    const int center_x = GetScreenWidth() / 2;
    static MainMenuPage page = MainMenuPage::Home;
    const int panel_width = (page == MainMenuPage::Controls) ? 620 : 520;
    const int panel_height = (page == MainMenuPage::Controls) ? 520 : 440;
    const int panel_x = center_x - panel_width / 2;
    const int panel_y = 90;

    DrawRectangle(panel_x, panel_y, panel_width, panel_height, Color{25, 28, 34, 255});
    DrawRectangleLines(panel_x, panel_y, panel_width, panel_height, Color{68, 75, 88, 255});

    static bool edit_name = false;
    static int selected_host_index = -1;
    static ControlsBindings draft_bindings = current_bindings;
    static bool draft_show_network_debug_panel = true;
    static int rebinding_action = -1;
    static int rebind_block_frames = 0;
    if (selected_host_index >= static_cast<int>(discovered_hosts.size())) {
        selected_host_index = -1;
    }

    if (page == MainMenuPage::Home) {
        DrawText("Rune Arena", panel_x + 20, panel_y + 16, 30, RAYWHITE);

        Rectangle name_box = {static_cast<float>(panel_x + 20), static_cast<float>(panel_y + 70), 320, 32};
        Rectangle join_box = {static_cast<float>(panel_x + 20), static_cast<float>(panel_y + 138), 320, 32};

        GuiLabel({name_box.x, name_box.y - 22, 180, 20}, "Player Name");
        GuiTextBox(name_box, player_name_buffer, player_name_buffer_size, edit_name);
        if (GuiButton({name_box.x + name_box.width + 10, name_box.y, 84, 32}, edit_name ? "Lock" : "Edit")) {
            edit_name = !edit_name;
        }

        GuiLabel({join_box.x, join_box.y - 22, 240, 20}, "Selected Host IP");
        GuiTextBox(join_box, join_ip_buffer, join_ip_buffer_size, false);

        if (GuiButton({static_cast<float>(panel_x + 20), static_cast<float>(panel_y + 190), 180, 40}, "Host Game")) {
            result.request_host = true;
        }
        if (GuiButton({static_cast<float>(panel_x + 420), static_cast<float>(panel_y + 190), 80, 40}, "Settings")) {
            page = MainMenuPage::Settings;
            draft_bindings = current_bindings;
            draft_show_network_debug_panel = show_network_debug_panel;
            rebinding_action = -1;
        }

        const bool can_join =
            selected_host_index >= 0 && selected_host_index < static_cast<int>(discovered_hosts.size());
        if (!can_join) {
            GuiDisable();
        }
        if (GuiButton({static_cast<float>(panel_x + 220), static_cast<float>(panel_y + 190), 180, 40}, "Join Game") &&
            can_join) {
            result.request_join = true;
            result.selected_host_ip = discovered_hosts[static_cast<size_t>(selected_host_index)].ip;
            result.selected_host_port = discovered_hosts[static_cast<size_t>(selected_host_index)].port;
        }
        if (!can_join) {
            GuiEnable();
        }

        if (!status_message.empty()) {
            DrawText(status_message.c_str(), panel_x + 20, panel_y + 236, 14,
                     status_is_error ? Color{255, 122, 122, 255} : Color{154, 214, 255, 255});
        }

        DrawText("Discovered LAN Hosts", panel_x + 20, panel_y + 254, 18, Color{190, 198, 220, 255});
        DrawText("Select one host, then press Join Game.", panel_x + 230, panel_y + 194, 13,
                 can_join ? Color{150, 210, 150, 255} : Color{210, 180, 120, 255});
        int row = 0;
        for (const DiscoveredHost& host : discovered_hosts) {
            if (row >= 4) {
                break;
            }
            const int y = panel_y + 282 + row * 28;
            const Rectangle row_rect = {static_cast<float>(panel_x + 20), static_cast<float>(y), 470.0f, 24.0f};
            const bool selected = row == selected_host_index;
            DrawRectangleRec(row_rect, selected ? Color{63, 78, 101, 255} : Color{40, 44, 53, 255});
            DrawRectangleLinesEx(row_rect, 1.0f, selected ? Color{122, 165, 219, 255} : Color{66, 72, 84, 255});
            const std::string row_text = host.name + "  " + host.ip + ":" + std::to_string(host.port);
            DrawText(row_text.c_str(), panel_x + 28, y + 5, 14, RAYWHITE);

            if (CheckCollisionPointRec(GetMousePosition(), row_rect) && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                selected_host_index = row;
                std::snprintf(join_ip_buffer, static_cast<size_t>(join_ip_buffer_size), "%s", host.ip.c_str());
            }
            ++row;
        }

        DrawText(TextFormat("Config: %s", config_path.c_str()), panel_x + 20, panel_y + panel_height - 24, 12,
                 Color{140, 147, 160, 255});
    } else if (page == MainMenuPage::Settings) {
        DrawText("Settings", panel_x + 20, panel_y + 16, 30, RAYWHITE);
        bool updated_debug_panel = draft_show_network_debug_panel;
        GuiCheckBox({static_cast<float>(panel_x + 20), static_cast<float>(panel_y + 88), 24, 24},
                    "Show Network Debug Panel", &updated_debug_panel);
        if (updated_debug_panel != draft_show_network_debug_panel) {
            draft_show_network_debug_panel = updated_debug_panel;
        }
        if (draft_show_network_debug_panel != show_network_debug_panel) {
            result.settings_changed = true;
            result.show_network_debug_panel = draft_show_network_debug_panel;
        }
        if (GuiButton({static_cast<float>(panel_x + 20), static_cast<float>(panel_y + 130), 180, 40}, "Controls")) {
            page = MainMenuPage::Controls;
            draft_bindings = current_bindings;
            rebinding_action = -1;
            rebind_block_frames = 0;
        }
        if (GuiButton({static_cast<float>(panel_x + 20), static_cast<float>(panel_y + 180), 180, 36}, "Back")) {
            page = MainMenuPage::Home;
        }
        DrawText(TextFormat("Controls Config: %s", controls_path.c_str()), panel_x + 20, panel_y + panel_height - 24,
                 12, Color{140, 147, 160, 255});
    } else {
        DrawText("Controls", panel_x + 20, panel_y + 16, 30, RAYWHITE);

        if (rebinding_action >= 0) {
            if (rebind_block_frames > 0) {
                rebind_block_frames -= 1;
            } else if (IsKeyPressed(KEY_ESCAPE)) {
                rebinding_action = -1;
            } else if (const auto captured = PollAnyBindingPressed(); captured.has_value()) {
                draft_bindings.*(kActionRows[static_cast<size_t>(rebinding_action)].member) = *captured;
                rebinding_action = -1;
            }
        }

        DrawText("Action", panel_x + 20, panel_y + 58, 18, Color{190, 198, 220, 255});
        DrawText("Binding", panel_x + 260, panel_y + 58, 18, Color{190, 198, 220, 255});

        for (size_t i = 0; i < kActionRows.size(); ++i) {
            const float row_y = static_cast<float>(panel_y + 86 + static_cast<int>(i) * 48);
            DrawText(kActionRows[i].label, panel_x + 20, static_cast<int>(row_y + 8), 18, RAYWHITE);
            const int binding = draft_bindings.*(kActionRows[i].member);
            DrawText(BindingToString(binding).c_str(), panel_x + 260, static_cast<int>(row_y + 8), 18,
                     Color{180, 220, 255, 255});

            const bool is_rebinding = rebinding_action == static_cast<int>(i);
            if (is_rebinding) {
                GuiDisable();
            }
            if (GuiButton({static_cast<float>(panel_x + 440), row_y, 150, 34}, is_rebinding ? "Waiting..." : "Rebind")) {
                rebinding_action = static_cast<int>(i);
                rebind_block_frames = 1;
            }
            if (is_rebinding) {
                GuiEnable();
            }
        }

        DrawText(rebinding_action >= 0 ? "Press any key/mouse button (Esc to cancel)"
                                       : "Tip: Click Rebind, then press a key or mouse button.",
                 panel_x + 20, panel_y + panel_height - 80, 14, Color{188, 198, 216, 255});

        if (GuiButton({static_cast<float>(panel_x + 20), static_cast<float>(panel_y + panel_height - 44), 140, 30},
                      "Accept")) {
            result.request_apply_controls = true;
            result.controls_bindings = draft_bindings;
            page = MainMenuPage::Settings;
            rebinding_action = -1;
        }
        if (GuiButton({static_cast<float>(panel_x + 180), static_cast<float>(panel_y + panel_height - 44), 140, 30},
                      "Cancel")) {
            draft_bindings = current_bindings;
            page = MainMenuPage::Settings;
            rebinding_action = -1;
        }
    }

    return result;
}
