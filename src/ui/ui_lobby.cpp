#include "ui/ui_lobby.h"

#include <raylib.h>
#include <raygui.h>

#include "game/game_types.h"

namespace {

std::string FormatMmSs(int total_seconds) {
    const int minutes = total_seconds / 60;
    const int seconds = total_seconds % 60;
    return TextFormat("%d:%02d", minutes, seconds);
}

}  // namespace

LobbyUiResult DrawLobby(const std::vector<std::string>& player_names, bool is_host, const std::string& host_display_ip,
                        int mode_type, int round_time_seconds, int best_of_target_kills,
                        float shrink_tiles_per_second, float shrink_start_seconds, float min_arena_radius_tiles,
                        const std::string& mode_name, const std::string& connection_status,
                        const std::string& selected_map_label, const std::string& map_options_text,
                        int selected_map_index, bool map_dropdown_edit_mode, const Texture2D* preview_texture,
                        bool has_preview_texture, const std::string& preview_status_text) {
    LobbyUiResult result;
    result.selected_map_index = selected_map_index;
    result.map_dropdown_edit_mode = map_dropdown_edit_mode;

    const int panel_w = 980;
    const int panel_h = 560;
    const int panel_x = (GetScreenWidth() - panel_w) / 2;
    const int panel_y = 64;

    DrawRectangle(panel_x, panel_y, panel_w, panel_h, Color{20, 23, 30, 255});
    DrawRectangleLines(panel_x, panel_y, panel_w, panel_h, Color{75, 82, 95, 255});

    DrawText("Lobby", panel_x + 20, panel_y + 16, 30, RAYWHITE);
    DrawText(TextFormat("Host: %s", host_display_ip.c_str()), panel_x + 20, panel_y + 62, 20,
             Color{196, 205, 228, 255});
    DrawText(TextFormat("Mode: %s", mode_name.c_str()), panel_x + 20, panel_y + 92, 20, Color{196, 205, 228, 255});
    if (mode_type == static_cast<int>(MatchModeType::MostKillsTimed)) {
        DrawText(TextFormat("Round Time: %s", FormatMmSs(round_time_seconds).c_str()), panel_x + 20, panel_y + 122, 20,
                 Color{196, 205, 228, 255});
    } else {
        DrawText(TextFormat("Best Of Kills: %d", best_of_target_kills), panel_x + 20, panel_y + 122, 20,
                 Color{196, 205, 228, 255});
    }
    DrawText(TextFormat("Arena Shrink: %.2f tiles/s", shrink_tiles_per_second), panel_x + 20, panel_y + 152, 20,
             Color{196, 205, 228, 255});
    DrawText(TextFormat("Shrink Starts: %s", FormatMmSs(static_cast<int>(shrink_start_seconds)).c_str()), panel_x + 20,
             panel_y + 182, 20, Color{196, 205, 228, 255});
    DrawText(TextFormat("Min Zone Radius: %.0f tiles", min_arena_radius_tiles), panel_x + 20, panel_y + 212, 20,
             Color{196, 205, 228, 255});
    DrawText(TextFormat("Status: %s", connection_status.c_str()), panel_x + 20, panel_y + 242, 18,
             Color{168, 220, 188, 255});
    DrawText(TextFormat("Map: %s", selected_map_label.c_str()), panel_x + 20, panel_y + 270, 18,
             Color{196, 205, 228, 255});

    DrawText("Players", panel_x + 20, panel_y + 304, 22, RAYWHITE);
    for (size_t i = 0; i < player_names.size(); ++i) {
        const int y = panel_y + 336 + static_cast<int>(i) * 30;
        DrawRectangle(panel_x + 20, y, 430, 24, Color{39, 43, 53, 255});
        DrawText(player_names[i].c_str(), panel_x + 28, y + 4, 16, Color{230, 232, 236, 255});
    }
    if (player_names.empty()) {
        DrawText("(no players yet)", panel_x + 28, panel_y + 336 + 4, 16, Color{170, 176, 190, 255});
    }

    const Rectangle preview_bounds = {static_cast<float>(panel_x + 728), static_cast<float>(panel_y + 58), 220.0f, 220.0f};
    DrawText("Map Preview", static_cast<int>(preview_bounds.x), panel_y + 30, 22, RAYWHITE);
    DrawRectangleRec(preview_bounds, Color{28, 31, 39, 255});
    DrawRectangleLinesEx(preview_bounds, 1.0f, Color{75, 82, 95, 255});
    if (has_preview_texture && preview_texture != nullptr && preview_texture->id != 0) {
        const float texture_w = static_cast<float>(preview_texture->width);
        const float texture_h = static_cast<float>(preview_texture->height);
        const float scale =
            std::min(preview_bounds.width / std::max(1.0f, texture_w), preview_bounds.height / std::max(1.0f, texture_h));
        const float draw_w = texture_w * scale;
        const float draw_h = texture_h * scale;
        const Rectangle src = {0.0f, 0.0f, texture_w, texture_h};
        const Rectangle dst = {preview_bounds.x + (preview_bounds.width - draw_w) * 0.5f,
                               preview_bounds.y + (preview_bounds.height - draw_h) * 0.5f, draw_w, draw_h};
        DrawTexturePro(*preview_texture, src, dst, {0.0f, 0.0f}, 0.0f, WHITE);
    } else {
        DrawText("No preview", static_cast<int>(preview_bounds.x) + 56, static_cast<int>(preview_bounds.y) + 96, 18,
                 Color{170, 176, 190, 255});
    }
    if (!preview_status_text.empty()) {
        DrawText(preview_status_text.c_str(), panel_x + 728, panel_y + 288, 16, Color{170, 176, 190, 255});
    }

    if (is_host) {
        if (GuiButton({static_cast<float>(panel_x + 728), static_cast<float>(panel_y + 458), 220, 48},
                      "Start Match")) {
            result.request_start_match = true;
        }
        if (GuiButton({static_cast<float>(panel_x + 520), static_cast<float>(panel_y + 92), 110, 32}, "Toggle")) {
            result.request_toggle_mode_type = true;
        }
        const Rectangle dropdown_bounds = {static_cast<float>(panel_x + 520), static_cast<float>(panel_y + 270), 180.0f, 32.0f};
        if (GuiDropdownBox(dropdown_bounds, map_options_text.c_str(), &result.selected_map_index,
                           result.map_dropdown_edit_mode)) {
            result.map_dropdown_edit_mode = !result.map_dropdown_edit_mode;
            result.selected_map_changed = !result.map_dropdown_edit_mode && result.selected_map_index != selected_map_index;
        }
        if (mode_type == static_cast<int>(MatchModeType::MostKillsTimed)) {
            if (GuiButton({static_cast<float>(panel_x + 520), static_cast<float>(panel_y + 122), 52, 32}, "-")) {
                result.request_decrease_round_time = true;
            }
            if (GuiButton({static_cast<float>(panel_x + 578), static_cast<float>(panel_y + 122), 52, 32}, "+")) {
                result.request_increase_round_time = true;
            }
        } else {
            if (GuiButton({static_cast<float>(panel_x + 520), static_cast<float>(panel_y + 122), 52, 32}, "-")) {
                result.request_decrease_best_of = true;
            }
            if (GuiButton({static_cast<float>(panel_x + 578), static_cast<float>(panel_y + 122), 52, 32}, "+")) {
                result.request_increase_best_of = true;
            }
        }
        if (GuiButton({static_cast<float>(panel_x + 520), static_cast<float>(panel_y + 152), 52, 32}, "-")) {
            result.request_decrease_shrink_rate = true;
        }
        if (GuiButton({static_cast<float>(panel_x + 578), static_cast<float>(panel_y + 152), 52, 32}, "+")) {
            result.request_increase_shrink_rate = true;
        }
        if (GuiButton({static_cast<float>(panel_x + 520), static_cast<float>(panel_y + 182), 52, 32}, "-")) {
            result.request_decrease_shrink_start = true;
        }
        if (GuiButton({static_cast<float>(panel_x + 578), static_cast<float>(panel_y + 182), 52, 32}, "+")) {
            result.request_increase_shrink_start = true;
        }
        if (GuiButton({static_cast<float>(panel_x + 520), static_cast<float>(panel_y + 212), 52, 32}, "-")) {
            result.request_decrease_min_radius = true;
        }
        if (GuiButton({static_cast<float>(panel_x + 578), static_cast<float>(panel_y + 212), 52, 32}, "+")) {
            result.request_increase_min_radius = true;
        }
    } else {
        DrawText("Waiting for host to start...", panel_x + 520, panel_y + 336, 16, Color{195, 200, 214, 255});
    }

    if (GuiButton({static_cast<float>(panel_x + 728), static_cast<float>(panel_y + 510), 220, 36}, "Leave")) {
        result.request_leave = true;
    }

    return result;
}
