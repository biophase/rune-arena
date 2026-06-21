#include "ui/ui_match.h"

#include <algorithm>
#include <cmath>

#include <raylib.h>

#include "core/constants.h"

void DrawMatchHud(const GameState& state, int local_player_id) {
    int local_team = Constants::kTeamBlue;
    for (const auto& player : state.players) {
        if (player.id == local_player_id) {
            local_team = player.team;
            break;
        }
    }
    const int enemy_team = local_team == Constants::kTeamBlue ? Constants::kTeamRed : Constants::kTeamBlue;
    const auto find_castle = [&](int team) -> const CastleState* {
        for (const auto& castle : state.castles) {
            if (castle.team == team) {
                return &castle;
            }
        }
        return nullptr;
    };
    const CastleState* own_castle = find_castle(local_team);
    const CastleState* enemy_castle = find_castle(enemy_team);

    const int screen_w = GetScreenWidth();
    const Rectangle left_panel = {12.0f, 10.0f, 320.0f, 62.0f};
    const Rectangle right_panel = {static_cast<float>(screen_w) - 332.0f, 10.0f, 320.0f, 62.0f};
    const Rectangle center_panel = {static_cast<float>(screen_w / 2 - 96), 10.0f, 192.0f, 46.0f};

    auto draw_castle_panel = [&](const Rectangle& rect, const CastleState* castle, bool right_align) {
        DrawRectangleRec(rect, Color{0, 0, 0, 148});
        DrawRectangleLinesEx(rect, 1.0f, Color{72, 82, 96, 255});
        if (castle == nullptr) {
            return;
        }
        const float bar_x = right_align ? rect.x + 56.0f : rect.x + 12.0f;
        const float bar_w = rect.width - 128.0f;
        const float bar_y = rect.y + 12.0f;
        const float ratio =
            castle->energy_needed_for_next_level > 0.0f
                ? std::clamp(castle->energy_into_current_level / castle->energy_needed_for_next_level, 0.0f, 1.0f)
                : 0.0f;
        DrawRectangleRec({bar_x, bar_y, bar_w, 12.0f}, Color{52, 58, 68, 255});
        DrawRectangleRec({bar_x, bar_y, bar_w * ratio, 12.0f},
                         right_align ? Color{208, 96, 96, 255} : Color{104, 160, 242, 255});
        DrawRectangleLinesEx({bar_x, bar_y, bar_w, 12.0f}, 1.0f, Color{16, 18, 24, 255});
        const std::string energy_text =
            TextFormat("%d / %d", static_cast<int>(std::round(castle->energy_into_current_level)),
                       static_cast<int>(std::round(castle->energy_needed_for_next_level)));
        const int energy_x = right_align
                                 ? static_cast<int>(rect.x + rect.width - 12.0f - MeasureText(energy_text.c_str(), 16))
                                 : static_cast<int>(bar_x + bar_w + 8.0f);
        DrawText(energy_text.c_str(), energy_x, static_cast<int>(rect.y + 8.0f), 16, RAYWHITE);
        const std::string level_text = TextFormat("Castle level: %d", castle->level);
        const int level_x = right_align
                                ? static_cast<int>(rect.x + rect.width - 12.0f - MeasureText(level_text.c_str(), 18))
                                : static_cast<int>(rect.x + 12.0f);
        DrawText(level_text.c_str(), level_x, static_cast<int>(rect.y + 32.0f), 18, RAYWHITE);
    };

    draw_castle_panel(left_panel, own_castle, false);
    draw_castle_panel(right_panel, enemy_castle, true);

    DrawRectangleRec(center_panel, Color{0, 0, 0, 148});
    DrawRectangleLinesEx(center_panel, 1.0f, Color{72, 82, 96, 255});
    if (state.match.mode_type == MatchModeType::BestOfKills) {
        DrawText(TextFormat("Best Of: %d", state.match.best_of_target_kills), static_cast<int>(center_panel.x + 18.0f),
                 static_cast<int>(center_panel.y + 8.0f), 18, RAYWHITE);
    } else {
        const int total_seconds = std::max(0, static_cast<int>(state.match.time_remaining));
        DrawText(TextFormat("Time: %d:%02d", total_seconds / 60, total_seconds % 60),
                 static_cast<int>(center_panel.x + 28.0f), static_cast<int>(center_panel.y + 8.0f), 18, RAYWHITE);
    }
    DrawText(TextFormat("Blue %d  Red %d", state.match.blue_team_kills, state.match.red_team_kills),
             static_cast<int>(center_panel.x + 20.0f), static_cast<int>(center_panel.y + 26.0f), 14, RAYWHITE);
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
