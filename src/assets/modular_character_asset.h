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
    std::vector<Texture2D> team_variant_textures;
    int source_team_index = 0;
    std::unordered_map<std::string, ModularFrameRange> tags;
    int frame_width = 0;
    int frame_height = 0;
    bool loaded = false;
};

struct ModularLayerLoadOptions {
    bool generate_team_variants = false;
    std::string palette_map_path;
    int team_count = 0;
    int source_team_index = 0;
};

class ModularCharacterAsset {
  public:
    ModularCharacterAsset() = default;
    ~ModularCharacterAsset();

    bool LoadLayer(const std::string& layer_name, const std::string& json_path,
                   const ModularLayerLoadOptions& options = {});
    void Unload();

    bool IsLoaded() const;
    bool HasLayer(const std::string& layer_name) const;
    bool HasTag(const std::string& layer_name, const std::string& tag_name) const;
    const Texture2D* GetLayerTexture(const std::string& layer_name, int team_index = -1) const;
    Rectangle GetFrame(const std::string& layer_name, const std::string& tag_name, float time_seconds) const;
    float GetTagDurationSeconds(const std::string& layer_name, const std::string& tag_name) const;
    int GetTagFrameCount(const std::string& layer_name, const std::string& tag_name) const;
    float GetTagFrameStartSeconds(const std::string& layer_name, const std::string& tag_name, int frame_index) const;
    int GetFrameWidth(const std::string& layer_name) const;
    int GetFrameHeight(const std::string& layer_name) const;

    static std::string BuildTag(const char* animation_name, FacingDirection facing);

  private:
    const ModularLayerAsset* FindLayer(const std::string& layer_name) const;
    const ModularFrameRange* FindTag(const std::string& layer_name, const std::string& tag_name) const;

    std::unordered_map<std::string, ModularLayerAsset> layers_;
};
