#include "ui/ui_lobby.h"

#include <raylib.h>
#include <raygui.h>

LobbyUiResult DrawLobby(const std::vector<std::string>& player_names, bool is_host, const std::string& host_display_ip,
                        int round_time_seconds, float shrink_tiles_per_second, float min_arena_radius_tiles,
                        const std::string& mode_name, const std::string& connection_status) {
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
    DrawText(TextFormat("Arena Shrink: %.2f tiles/s", shrink_tiles_per_second), panel_x + 20, panel_y + 152, 20,
             Color{196, 205, 228, 255});
    DrawText(TextFormat("Min Zone Radius: %.0f tiles", min_arena_radius_tiles), panel_x + 20, panel_y + 182, 20,
             Color{196, 205, 228, 255});
    DrawText(TextFormat("Status: %s", connection_status.c_str()), panel_x + 20, panel_y + 212, 18,
             Color{168, 220, 188, 255});

    DrawText("Players", panel_x + 20, panel_y + 246, 22, RAYWHITE);
    for (size_t i = 0; i < player_names.size(); ++i) {
        const int y = panel_y + 278 + static_cast<int>(i) * 30;
        DrawRectangle(panel_x + 20, y, 460, 24, Color{39, 43, 53, 255});
        DrawText(player_names[i].c_str(), panel_x + 28, y + 4, 16, Color{230, 232, 236, 255});
    }
    if (player_names.empty()) {
        DrawText("(no players yet)", panel_x + 28, panel_y + 278 + 4, 16, Color{170, 176, 190, 255});
    }

    if (is_host) {
        if (GuiButton({static_cast<float>(panel_x + 520), static_cast<float>(panel_y + 278), 170, 48},
                      "Start Match")) {
            result.request_start_match = true;
        }
        if (GuiButton({static_cast<float>(panel_x + 520), static_cast<float>(panel_y + 152), 52, 32}, "-")) {
            result.request_decrease_shrink_rate = true;
        }
        if (GuiButton({static_cast<float>(panel_x + 578), static_cast<float>(panel_y + 152), 52, 32}, "+")) {
            result.request_increase_shrink_rate = true;
        }
        if (GuiButton({static_cast<float>(panel_x + 520), static_cast<float>(panel_y + 182), 52, 32}, "-")) {
            result.request_decrease_min_radius = true;
        }
        if (GuiButton({static_cast<float>(panel_x + 578), static_cast<float>(panel_y + 182), 52, 32}, "+")) {
            result.request_increase_min_radius = true;
        }
    } else {
        DrawText("Waiting for host to start...", panel_x + 520, panel_y + 296, 16, Color{195, 200, 214, 255});
    }

    if (GuiButton({static_cast<float>(panel_x + 520), static_cast<float>(panel_y + 338), 170, 36}, "Leave")) {
        result.request_leave = true;
    }

    return result;
}
