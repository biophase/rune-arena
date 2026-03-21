#include "ui/ui_match.h"

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

    const char* title = "Match Ended";
    const char* winner = "Draw";
    if (winning_team == Constants::kTeamRed) {
        winner = "Red Team Wins";
    } else if (winning_team == Constants::kTeamBlue) {
        winner = "Blue Team Wins";
    }

    DrawText(title, w / 2 - MeasureText(title, 42) / 2, 140, 42, RAYWHITE);
    DrawText(winner, w / 2 - MeasureText(winner, 34) / 2, 210, 34, Color{255, 228, 120, 255});

    DrawText(TextFormat("Final Score: Red %d - %d Blue", state.match.red_team_kills, state.match.blue_team_kills),
             w / 2 - 210, 280, 26, RAYWHITE);
    DrawText("Press ENTER to return to Main Menu", w / 2 - 200, 360, 20, Color{180, 188, 205, 255});
}
