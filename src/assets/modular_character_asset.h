#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <raylib.h>

#include "game/game_types.h"

struct ModularFrameRange {
    std::vector<Rectangle> frames;
    std::vector<float> frame_durations_seconds;

    bool IsValid() const { return !frames.empty() && frames.size() == frame_durations_seconds.size(); }
};

struct ModularLayerAsset {
    Texture2D texture = {};
    std::unordered_map<std::string, ModularFrameRange> tags;
    int frame_width = 0;
    int frame_height = 0;
    bool loaded = false;
};

class ModularCharacterAsset {
  public:
    ModularCharacterAsset() = default;
    ~ModularCharacterAsset();

    bool LoadLayer(const std::string& layer_name, const std::string& json_path);
    void Unload();

    bool IsLoaded() const;
    bool HasLayer(const std::string& layer_name) const;
    bool HasTag(const std::string& layer_name, const std::string& tag_name) const;
    const Texture2D* GetLayerTexture(const std::string& layer_name) const;
    Rectangle GetFrame(const std::string& layer_name, const std::string& tag_name, float time_seconds) const;
    int GetFrameWidth(const std::string& layer_name) const;
    int GetFrameHeight(const std::string& layer_name) const;

    static std::string BuildTag(const char* animation_name, FacingDirection facing);

  private:
    const ModularLayerAsset* FindLayer(const std::string& layer_name) const;
    const ModularFrameRange* FindTag(const std::string& layer_name, const std::string& tag_name) const;

    std::unordered_map<std::string, ModularLayerAsset> layers_;
};
