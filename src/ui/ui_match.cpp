#include "ui/ui_match.h"

#include <algorithm>

#include <raylib.h>

#include "core/constants.h"

void DrawMatchHud(const GameState& state, int local_player_id) {
    DrawRectangle(10, 10, 360, 88, Color{0, 0, 0, 130});
    if (state.match.mode_type == MatchModeType::BestOfKills) {
        DrawText(TextFormat("Best Of: %d", state.match.best_of_target_kills), 22, 20, 24, RAYWHITE);
    } else {
        const int total_seconds = std::max(0, static_cast<int>(state.match.time_remaining));
        DrawText(TextFormat("Time: %d:%02d", total_seconds / 60, total_seconds % 60), 22, 20, 24, RAYWHITE);
    }
    DrawText(TextFormat("Red: %d   Blue: %d", state.match.red_team_kills, state.match.blue_team_kills), 22, 48, 20,
             RAYWHITE);
}

void DrawPostMatch(const GameState& state, int winning_team) {
    const int w = GetScreenWidth();
    const int h = GetScreenHeight();
    DrawRectangle(0, 0, w, h, Color{13, 15, 19, 255});

    const Rectangle panel = {static_cast<float>(w / 2 - 520), 44.0f, 1040.0f, static_cast<float>(h - 120)};
    DrawRectangleRec(panel, Color{20, 24, 30, 245});
    DrawRectangleLinesEx(panel, 1.0f, Color{80, 88, 102, 255});

    const char* title = "Match Summary";
    const char* winner = "Draw";
    if (winning_team == Constants::kTeamRed) {
        winner = "Red Team Wins";
    } else if (winning_team == Constants::kTeamBlue) {
        winner = "Blue Team Wins";
    }

    DrawText(title, static_cast<int>(panel.x + 24.0f), static_cast<int>(panel.y + 18.0f), 34, RAYWHITE);
    DrawText(winner, static_cast<int>(panel.x + 24.0f), static_cast<int>(panel.y + 58.0f), 24, Color{255, 228, 120, 255});

    const Rectangle top_left = {panel.x + 24.0f, panel.y + 100.0f, panel.width * 0.5f - 36.0f, 220.0f};
    const Rectangle top_right = {panel.x + panel.width * 0.5f + 12.0f, panel.y + 100.0f, panel.width * 0.5f - 36.0f, 220.0f};
    const Rectangle bottom = {panel.x + 24.0f, panel.y + 344.0f, panel.width - 48.0f, panel.height - 420.0f};

    auto draw_team_panel = [&](const Rectangle& rect, int team, const char* label, Color accent, int total_kills) {
        DrawRectangleRec(rect, Color{26, 31, 38, 220});
        DrawRectangleLinesEx(rect, 1.0f, Color{72, 82, 96, 255});
        DrawText(label, static_cast<int>(rect.x + 14.0f), static_cast<int>(rect.y + 12.0f), 24, accent);
        int row_y = static_cast<int>(rect.y + 48.0f);
        for (const auto& player : state.players) {
            if (player.team != team) {
                continue;
            }
            const std::string name = player.name.empty() ? TextFormat("Player%d", player.id) : player.name;
            DrawText(name.c_str(), static_cast<int>(rect.x + 14.0f), row_y, 20, RAYWHITE);
            const std::string kills = TextFormat("%d", player.kills);
            DrawText(kills.c_str(), static_cast<int>(rect.x + rect.width - 22.0f - MeasureText(kills.c_str(), 20)), row_y, 20,
                     Color{220, 220, 220, 255});
            row_y += 28;
        }
        DrawLineEx({rect.x + 12.0f, rect.y + rect.height - 42.0f}, {rect.x + rect.width - 12.0f, rect.y + rect.height - 42.0f},
                   1.0f, Color{72, 82, 96, 255});
        const std::string total = TextFormat("Total Kills: %d", total_kills);
        DrawText(total.c_str(), static_cast<int>(rect.x + 14.0f), static_cast<int>(rect.y + rect.height - 32.0f), 20, accent);
    };

    draw_team_panel(top_left, Constants::kTeamBlue, "Blue Team", Color{120, 170, 255, 255}, state.match.blue_team_kills);
    draw_team_panel(top_right, Constants::kTeamRed, "Red Team", Color{255, 120, 120, 255}, state.match.red_team_kills);

    DrawRectangleRec(bottom, Color{26, 31, 38, 220});
    DrawRectangleLinesEx(bottom, 1.0f, Color{72, 82, 96, 255});
    DrawText("Kill Progression", static_cast<int>(bottom.x + 14.0f), static_cast<int>(bottom.y + 12.0f), 24, RAYWHITE);

    const Rectangle plot = {bottom.x + 54.0f, bottom.y + 46.0f, bottom.width - 78.0f, bottom.height - 92.0f};
    DrawRectangleLinesEx(plot, 1.0f, Color{92, 100, 115, 255});
    const int max_kills = std::max(1, std::max(state.match.red_team_kills, state.match.blue_team_kills));
    const float max_time = std::max(1.0f, static_cast<float>(state.match.round_time_seconds));
    for (int i = 0; i <= max_kills; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(max_kills);
        const float y = plot.y + plot.height - t * plot.height;
        DrawLineEx({plot.x, y}, {plot.x + plot.width, y}, 1.0f, Color{44, 48, 56, 255});
        DrawText(TextFormat("%d", i), static_cast<int>(plot.x - 26.0f), static_cast<int>(y - 8.0f), 16, Color{180, 188, 205, 255});
    }

    auto draw_plot = [&](Color color, bool red_team) {
        if (state.match.kill_timeline.empty()) {
            return;
        }
        for (size_t i = 1; i < state.match.kill_timeline.size(); ++i) {
            const KillTimelinePoint& a = state.match.kill_timeline[i - 1];
            const KillTimelinePoint& b = state.match.kill_timeline[i];
            const float ax = plot.x + std::clamp(a.elapsed_seconds / max_time, 0.0f, 1.0f) * plot.width;
            const float bx = plot.x + std::clamp(b.elapsed_seconds / max_time, 0.0f, 1.0f) * plot.width;
            const float ay = plot.y + plot.height -
                             (static_cast<float>(red_team ? a.red_team_kills : a.blue_team_kills) / static_cast<float>(max_kills)) *
                                 plot.height;
            const float by = plot.y + plot.height -
                             (static_cast<float>(red_team ? b.red_team_kills : b.blue_team_kills) / static_cast<float>(max_kills)) *
                                 plot.height;
            DrawLineEx({ax, ay}, {bx, by}, 3.0f, color);
        }
    };
    draw_plot(Color{255, 120, 120, 255}, true);
    draw_plot(Color{120, 170, 255, 255}, false);

    DrawRectangle(static_cast<int>(plot.x), static_cast<int>(bottom.y + bottom.height - 28.0f), 16, 6, Color{120, 170, 255, 255});
    DrawText("Blue", static_cast<int>(plot.x + 22.0f), static_cast<int>(bottom.y + bottom.height - 34.0f), 18, Color{200, 210, 230, 255});
    DrawRectangle(static_cast<int>(plot.x + 90.0f), static_cast<int>(bottom.y + bottom.height - 28.0f), 16, 6,
                  Color{255, 120, 120, 255});
    DrawText("Red", static_cast<int>(plot.x + 112.0f), static_cast<int>(bottom.y + bottom.height - 34.0f), 18, Color{200, 210, 230, 255});

    const Rectangle leave_button = {static_cast<float>(w / 2 - 80), static_cast<float>(h - 92), 160.0f, 42.0f};
    DrawRectangleRec(leave_button, Color{64, 72, 86, 255});
    DrawRectangleLinesEx(leave_button, 1.0f, Color{120, 130, 148, 255});
    DrawText("Leave", static_cast<int>(leave_button.x + (leave_button.width - MeasureText("Leave", 24)) * 0.5f),
             static_cast<int>(leave_button.y + 9.0f), 24, RAYWHITE);
}
