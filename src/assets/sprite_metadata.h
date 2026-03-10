#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <raylib.h>

struct SpriteFacingData {
    std::vector<Rectangle> frames;
    float fps = 1.0f;
};

struct SpriteAnimationData {
    std::unordered_map<std::string, SpriteFacingData> facings;
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
    Rectangle GetFrame(const std::string& animation_name, const std::string& facing, float time_seconds) const;
    Rectangle GetMirroredFrameRect(const Rectangle& source_rect) const;
    bool GetAnimationStats(const std::string& animation_name, const std::string& facing, int& out_frame_count,
                           float& out_fps) const;

  private:
    const SpriteFacingData* ResolveFacing(const std::string& animation_name,
                                          const std::string& facing) const;

    Texture2D texture_ = {};
    Texture2D mirrored_texture_ = {};
    bool loaded_ = false;
    int cell_width_ = 32;
    int cell_height_ = 32;
    int num_columns_ = 0;
    std::unordered_map<std::string, SpriteAnimationData> animations_;
};
