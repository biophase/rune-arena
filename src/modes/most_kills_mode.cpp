#include "modes/most_kills_mode.h"

#include <algorithm>

std::string MostKillsMode::GetUiName() const { return "Most Kills"; }

void MostKillsMode::Update(GameState& state, EventQueue& event_queue, float dt) {
    if (!state.match.match_running || state.match.match_finished) {
        return;
    }

    state.match.elapsed_seconds += dt;

    if (state.match.mode_type == MatchModeType::BestOfKills) {
        if (state.match.red_team_kills >= state.match.best_of_target_kills ||
            state.match.blue_team_kills >= state.match.best_of_target_kills) {
            state.match.match_running = false;
            state.match.match_finished = true;
            event_queue.Push(MatchEndedEvent{GetWinningTeam(state)});
        }
        return;
    }

    state.match.time_remaining = std::max(0.0f, state.match.time_remaining - dt);
    if (state.match.time_remaining <= 0.0f) {
        state.match.match_running = false;
        state.match.match_finished = true;
        event_queue.Push(MatchEndedEvent{GetWinningTeam(state)});
    }
}

bool MostKillsMode::IsFinished(const GameState& state) const { return state.match.match_finished; }

int MostKillsMode::GetWinningTeam(const GameState& state) const {
    if (state.match.red_team_kills > state.match.blue_team_kills) {
        return 0;
    }
    if (state.match.blue_team_kills > state.match.red_team_kills) {
        return 1;
    }
    return -1;
}
