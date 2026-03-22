#pragma once

#include <string>
#include <vector>

#include "config/controls_bindings.h"
#include "net/lan_discovery.h"

struct MainMenuUiResult {
    bool request_host = false;
    bool request_join = false;
    bool request_exit_app = false;
    std::string selected_host_ip;
    int selected_host_port = 0;
    bool settings_changed = false;
    bool show_network_debug_panel = true;
    bool request_apply_controls = false;
    ControlsBindings controls_bindings;
};

MainMenuUiResult DrawMainMenu(char* player_name_buffer, int player_name_buffer_size, char* join_ip_buffer,
                              int join_ip_buffer_size,
                              const std::vector<DiscoveredHost>& discovered_hosts, const std::string& config_path,
                              const ControlsBindings& current_bindings, const std::string& controls_path,
                              bool show_network_debug_panel, const std::string& status_message, bool status_is_error);
