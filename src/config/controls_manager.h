#pragma once

#include <string>

#include "config/controls_bindings.h"

class ControlsManager {
  public:
    ControlsManager();

    bool Load(ControlsBindings& bindings) const;
    bool Save(const ControlsBindings& bindings) const;
    const std::string& GetControlsPath() const;

  private:
    std::string controls_path_;
};
