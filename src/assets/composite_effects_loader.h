#pragma once

#include <string>
#include <unordered_map>

#include "game/composite_effect.h"

class CompositeEffectsLoader {
  public:
    bool LoadFromFile(const std::string& path);
    const CompositeEffectDefinition* FindById(const std::string& id) const;
    bool IsLoaded() const;
    const std::string& GetLastError() const;

  private:
    bool loaded_ = false;
    std::string last_error_;
    std::unordered_map<std::string, CompositeEffectDefinition> definitions_;
};
