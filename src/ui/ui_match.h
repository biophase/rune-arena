#pragma once

#include "game/game_state.h"

void DrawMatchHud(const GameState& state, int local_player_id);
void DrawPostMatch(const GameState& state, int winning_team);
