#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <raylib.h>

enum class SpriteFrameLayer {
    Single,
    Background,
    Foreground,
};

struct SpriteFacingData {
    std::vector<Rectangle> frames;
    std::vector<Rectangle> frames_background;
    std::vector<Rectangle> frames_foreground;
    float fps = 1.0f;

    bool HasAnyFrames() const {
        return !frames.empty() || !frames_background.empty() || !frames_foreground.empty();
    }
};

struct SpriteAnimationData {
    std::unordered_map<std::string, SpriteFacingData> facings;
    std::unordered_map<int, SpriteFacingData> bitmask_variants;
};

class SpriteMetadataLoader {
  public:
    SpriteMetadataLoader() = default;
    ~SpriteMetadataLoader();

    bool LoadFromFile(const std::string& json_path);
    void Unload();

    bool IsLoaded() const;
    const Texture2D& GetTexture(bool use_mirrored = false) const;
    bool HasAnimation(const std::string& animation_name) const;
    bool HasBitmaskAnimation(const std::string& animation_name) const;
    Rectangle GetFrame(const std::string& animation_name, const std::string& facing, float time_seconds) const;
    bool HasBitmaskLayer(const std::string& animation_name, int bitmask, SpriteFrameLayer layer) const;
    bool GetBitmaskFrame(const std::string& animation_name, int bitmask, float time_seconds, SpriteFrameLayer layer,
                         Rectangle& out_frame) const;
    Rectangle GetMirroredFrameRect(const Rectangle& source_rect) const;
    bool GetAnimationStats(const std::string& animation_name, const std::string& facing, int& out_frame_count,
                           float& out_fps) const;

  private:
    const SpriteFacingData* ResolveFacing(const std::string& animation_name,
                                          const std::string& facing) const;
    const SpriteFacingData* ResolveBitmaskVariant(const std::string& animation_name, int bitmask) const;
    bool ResolveFrameFromData(const SpriteFacingData& frame_data, float time_seconds, SpriteFrameLayer layer,
                              Rectangle& out_frame) const;

    Texture2D texture_ = {};
    Texture2D mirrored_texture_ = {};
    bool loaded_ = false;
    int cell_width_ = 32;
    int cell_height_ = 32;
    int num_columns_ = 0;
    std::unordered_map<std::string, SpriteAnimationData> animations_;
};
