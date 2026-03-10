#include "ui/ui_lobby.h"

#include <raygui.h>
#include <raylib.h>

LobbyUiResult DrawLobby(const std::vector<std::string>& player_names, bool is_host, const std::string& host_display_ip,
                        int round_time_seconds, const std::string& mode_name) {
    LobbyUiResult result;

    const int panel_w = 720;
    const int panel_h = 460;
    const int panel_x = (GetScreenWidth() - panel_w) / 2;
    const int panel_y = 80;

    DrawRectangle(panel_x, panel_y, panel_w, panel_h, Color{20, 23, 30, 255});
    DrawRectangleLines(panel_x, panel_y, panel_w, panel_h, Color{75, 82, 95, 255});

    DrawText("Lobby", panel_x + 20, panel_y + 16, 30, RAYWHITE);
    DrawText(TextFormat("Host: %s", host_display_ip.c_str()), panel_x + 20, panel_y + 62, 20,
             Color{196, 205, 228, 255});
    DrawText(TextFormat("Mode: %s", mode_name.c_str()), panel_x + 20, panel_y + 92, 20, Color{196, 205, 228, 255});
    DrawText(TextFormat("Round Time: %d", round_time_seconds), panel_x + 20, panel_y + 122, 20,
             Color{196, 205, 228, 255});

    DrawText("Players", panel_x + 20, panel_y + 170, 22, RAYWHITE);
    for (size_t i = 0; i < player_names.size(); ++i) {
        const int y = panel_y + 202 + static_cast<int>(i) * 30;
        DrawRectangle(panel_x + 20, y, 460, 24, Color{39, 43, 53, 255});
        DrawText(player_names[i].c_str(), panel_x + 28, y + 4, 16, Color{230, 232, 236, 255});
    }

    if (is_host) {
        if (GuiButton({static_cast<float>(panel_x + 520), static_cast<float>(panel_y + 220), 170, 48},
                      "Start Match")) {
            result.request_start_match = true;
        }
    } else {
        DrawText("Waiting for host to start...", panel_x + 520, panel_y + 238, 16, Color{195, 200, 214, 255});
    }

    if (GuiButton({static_cast<float>(panel_x + 520), static_cast<float>(panel_y + 280), 170, 36}, "Leave")) {
        result.request_leave = true;
    }

    return result;
}
