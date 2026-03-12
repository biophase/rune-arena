#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "game/map_object.h"

class ObjectsDatabase {
  public:
    bool LoadFromFile(const std::string& path);

    bool IsLoaded() const { return loaded_; }
    const std::string& GetPath() const { return path_; }
    const std::string& GetLastError() const { return last_error_; }

    const ObjectPrototype* FindById(const std::string& id) const;
    const ObjectPrototype* FindByMapKey(unsigned char r, unsigned char g, unsigned char b) const;

  private:
    static uint32_t PackRgb(unsigned char r, unsigned char g, unsigned char b);

    bool loaded_ = false;
    std::string path_;
    std::string last_error_;

    std::vector<ObjectPrototype> prototypes_;
    std::unordered_map<std::string, size_t> by_id_;
    std::unordered_map<uint32_t, size_t> by_map_color_;
};
