#pragma once

#include <string>
#include <vector>

struct LobbyUiResult {
    bool request_start_match = false;
    bool request_leave = false;
    bool request_toggle_mode_type = false;
    bool request_decrease_round_time = false;
    bool request_increase_round_time = false;
    bool request_decrease_best_of = false;
    bool request_increase_best_of = false;
    bool request_decrease_shrink_rate = false;
    bool request_increase_shrink_rate = false;
    bool request_decrease_shrink_start = false;
    bool request_increase_shrink_start = false;
    bool request_decrease_min_radius = false;
    bool request_increase_min_radius = false;
};

LobbyUiResult DrawLobby(const std::vector<std::string>& player_names, bool is_host, const std::string& host_display_ip,
                        int mode_type, int round_time_seconds, int best_of_target_kills,
                        float shrink_tiles_per_second, float shrink_start_seconds, float min_arena_radius_tiles,
                        const std::string& mode_name, const std::string& connection_status);
