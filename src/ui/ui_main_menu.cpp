#include "ui/ui_main_menu.h"

#include <cstdio>

#include <raygui.h>
#include <raylib.h>

MainMenuUiResult DrawMainMenu(char* player_name_buffer, int player_name_buffer_size, char* join_ip_buffer,
                              int join_ip_buffer_size, const std::vector<DiscoveredHost>& discovered_hosts,
                              const std::string& config_path) {
    MainMenuUiResult result;

    const int center_x = GetScreenWidth() / 2;
    const int panel_width = 520;
    const int panel_height = 440;
    const int panel_x = center_x - panel_width / 2;
    const int panel_y = 90;

    DrawRectangle(panel_x, panel_y, panel_width, panel_height, Color{25, 28, 34, 255});
    DrawRectangleLines(panel_x, panel_y, panel_width, panel_height, Color{68, 75, 88, 255});

    DrawText("Rune Arena", panel_x + 20, panel_y + 16, 30, RAYWHITE);

    Rectangle name_box = {static_cast<float>(panel_x + 20), static_cast<float>(panel_y + 70), 320, 32};
    Rectangle join_box = {static_cast<float>(panel_x + 20), static_cast<float>(panel_y + 138), 320, 32};

    static bool edit_name = false;
    static bool edit_join_ip = false;

    GuiLabel({name_box.x, name_box.y - 22, 180, 20}, "Player Name");
    GuiTextBox(name_box, player_name_buffer, player_name_buffer_size, edit_name);
    if (GuiButton({name_box.x + name_box.width + 10, name_box.y, 84, 32}, edit_name ? "Lock" : "Edit")) {
        edit_name = !edit_name;
    }

    GuiLabel({join_box.x, join_box.y - 22, 180, 20}, "Host IP");
    GuiTextBox(join_box, join_ip_buffer, join_ip_buffer_size, edit_join_ip);
    if (GuiButton({join_box.x + join_box.width + 10, join_box.y, 84, 32}, edit_join_ip ? "Lock" : "Edit")) {
        edit_join_ip = !edit_join_ip;
    }

    if (GuiButton({static_cast<float>(panel_x + 20), static_cast<float>(panel_y + 190), 180, 40}, "Host Game")) {
        result.request_host = true;
    }

    if (GuiButton({static_cast<float>(panel_x + 220), static_cast<float>(panel_y + 190), 180, 40}, "Join Game")) {
        result.request_join = true;
    }

    DrawText("Discovered LAN Hosts", panel_x + 20, panel_y + 254, 18, Color{190, 198, 220, 255});
    int row = 0;
    for (const DiscoveredHost& host : discovered_hosts) {
        if (row >= 4) {
            break;
        }
        const int y = panel_y + 282 + row * 28;
        DrawRectangle(panel_x + 20, y, 470, 24, Color{40, 44, 53, 255});
        DrawRectangleLines(panel_x + 20, y, 470, 24, Color{66, 72, 84, 255});
        const std::string row_text = host.name + "  " + host.ip + ":" + std::to_string(host.port);
        DrawText(row_text.c_str(), panel_x + 28, y + 5, 14, RAYWHITE);
        ++row;
    }

    DrawText(TextFormat("Config: %s", config_path.c_str()), panel_x + 20, panel_y + panel_height - 24, 12,
             Color{140, 147, 160, 255});

    return result;
}
