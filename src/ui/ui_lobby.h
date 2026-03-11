#pragma once

#include <string>
#include <vector>

struct LobbyUiResult {
    bool request_start_match = false;
    bool request_leave = false;
    bool request_decrease_shrink_rate = false;
    bool request_increase_shrink_rate = false;
    bool request_decrease_min_radius = false;
    bool request_increase_min_radius = false;
};

LobbyUiResult DrawLobby(const std::vector<std::string>& player_names, bool is_host, const std::string& host_display_ip,
                        int round_time_seconds, float shrink_tiles_per_second, float min_arena_radius_tiles,
                        const std::string& mode_name, const std::string& connection_status);
