#pragma once

#include <string>
#include <vector>

#include "net/lan_discovery.h"

struct MainMenuUiResult {
    bool request_host = false;
    bool request_join = false;
};

MainMenuUiResult DrawMainMenu(char* player_name_buffer, int player_name_buffer_size, char* join_ip_buffer,
                              int join_ip_buffer_size,
                              const std::vector<DiscoveredHost>& discovered_hosts,
                              const std::string& config_path);
