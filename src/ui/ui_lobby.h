#pragma once

#include <string>
#include <vector>

struct LobbyUiResult {
    bool request_start_match = false;
    bool request_leave = false;
};

LobbyUiResult DrawLobby(const std::vector<std::string>& player_names, bool is_host, const std::string& host_display_ip,
                        int round_time_seconds, const std::string& mode_name, const std::string& connection_status);
