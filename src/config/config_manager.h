#pragma once

#include <string>

#include "config/user_settings.h"

class ConfigManager {
  public:
    ConfigManager();

    bool Load(UserSettings& settings) const;
    bool Save(const UserSettings& settings) const;
    const std::string& GetConfigPath() const;

  private:
    std::string config_path_;
};
