#include "assets/sprite_metadata.h"

#include <cmath>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

SpriteMetadataLoader::~SpriteMetadataLoader() { Unload(); }

bool SpriteMetadataLoader::LoadFromFile(const std::string& json_path) {
    Unload();

    std::ifstream input(json_path);
    if (!input.is_open()) {
        TraceLog(LOG_ERROR, "Failed to open sprite metadata: %s", json_path.c_str());
        return false;
    }

    nlohmann::json json;
    input >> json;

    cell_width_ = json.value("cell_width", 32);
    cell_height_ = json.value("cell_height", 32);
    num_columns_ = json.value("num_columns", 0);

    const std::filesystem::path json_file_path(json_path);
    const std::filesystem::path texture_path =
        json_file_path.parent_path() / json.value("texture", std::string("sprite_sheet.png"));

    Image source_image = LoadImage(texture_path.string().c_str());
    if (source_image.data == nullptr) {
        TraceLog(LOG_ERROR, "Failed to load sprite sheet image: %s", texture_path.string().c_str());
        return false;
    }

    texture_ = LoadTextureFromImage(source_image);
    if (texture_.id == 0) {
        TraceLog(LOG_ERROR, "Failed to create sprite sheet texture: %s", texture_path.string().c_str());
        UnloadImage(source_image);
        return false;
    }

    Image mirrored_image = ImageCopy(source_image);
    ImageFlipHorizontal(&mirrored_image);
    mirrored_texture_ = LoadTextureFromImage(mirrored_image);

    UnloadImage(mirrored_image);
    UnloadImage(source_image);

    SetTextureFilter(texture_, TEXTURE_FILTER_POINT);
    if (mirrored_texture_.id != 0) {
        SetTextureFilter(mirrored_texture_, TEXTURE_FILTER_POINT);
    } else {
        TraceLog(LOG_WARNING, "Failed to create mirrored sprite sheet texture, falling back to default texture");
    }
    if (num_columns_ <= 0 && cell_width_ > 0) {
        num_columns_ = texture_.width / cell_width_;
    }
    const int num_rows = (cell_height_ > 0) ? (texture_.height / cell_height_) : 0;

    const auto animations_it = json.find("animations");
    if (animations_it == json.end() || !animations_it->is_object()) {
        TraceLog(LOG_ERROR, "Invalid animations section in sprite metadata");
        Unload();
        return false;
    }

    for (auto animation_it = animations_it->begin(); animation_it != animations_it->end(); ++animation_it) {
        SpriteAnimationData animation_data;
        const auto facings_it = animation_it.value().find("facings");
        if (facings_it == animation_it.value().end() || !facings_it->is_object()) {
            continue;
        }

        for (auto facing_it = facings_it->begin(); facing_it != facings_it->end(); ++facing_it) {
            SpriteFacingData facing_data;
            facing_data.fps = facing_it.value().value("fps", 1.0f);

            const auto frames_it = facing_it.value().find("frames");
            if (frames_it != facing_it.value().end() && frames_it->is_array()) {
                for (const auto& frame_coord : *frames_it) {
                    if (!frame_coord.is_array() || frame_coord.size() < 2) {
                        continue;
                    }
                    // Frame convention: [row, col].
                    const int cell_y = frame_coord[0].get<int>();
                    const int cell_x = frame_coord[1].get<int>();
                    if (cell_x < 0 || cell_y < 0) {
                        TraceLog(LOG_WARNING, "Negative sprite frame index skipped: %s/%s [%d,%d]",
                                 animation_it.key().c_str(), facing_it.key().c_str(), cell_y, cell_x);
                        continue;
                    }
                    if (num_columns_ > 0 && cell_x >= num_columns_) {
                        TraceLog(LOG_WARNING,
                                 "Sprite frame column out of range (num_columns=%d): %s/%s [%d,%d]",
                                 num_columns_, animation_it.key().c_str(), facing_it.key().c_str(), cell_y, cell_x);
                        continue;
                    }
                    if (num_rows > 0 && cell_y >= num_rows) {
                        TraceLog(LOG_WARNING, "Sprite frame row out of range (num_rows=%d): %s/%s [%d,%d]", num_rows,
                                 animation_it.key().c_str(), facing_it.key().c_str(), cell_y, cell_x);
                        continue;
                    }
                    facing_data.frames.push_back(
                        Rectangle{static_cast<float>(cell_x * cell_width_), static_cast<float>(cell_y * cell_height_),
                                  static_cast<float>(cell_width_), static_cast<float>(cell_height_)});
                }
            }

            if (!facing_data.frames.empty()) {
                animation_data.facings[facing_it.key()] = facing_data;
            }
        }

        if (!animation_data.facings.empty()) {
            animations_[animation_it.key()] = animation_data;
        }
    }

    loaded_ = true;
    return true;
}

void SpriteMetadataLoader::Unload() {
    if (texture_.id != 0) {
        UnloadTexture(texture_);
        texture_ = {};
    }
    if (mirrored_texture_.id != 0) {
        UnloadTexture(mirrored_texture_);
        mirrored_texture_ = {};
    }
    loaded_ = false;
    animations_.clear();
    num_columns_ = 0;
}

bool SpriteMetadataLoader::IsLoaded() const { return loaded_; }

const Texture2D& SpriteMetadataLoader::GetTexture(bool use_mirrored) const {
    if (use_mirrored && mirrored_texture_.id != 0) {
        return mirrored_texture_;
    }
    return texture_;
}

bool SpriteMetadataLoader::HasAnimation(const std::string& animation_name) const {
    return animations_.find(animation_name) != animations_.end();
}

const SpriteFacingData* SpriteMetadataLoader::ResolveFacing(const std::string& animation_name,
                                                             const std::string& facing) const {
    const auto animation_it = animations_.find(animation_name);
    if (animation_it == animations_.end()) {
        return nullptr;
    }

    const auto facing_it = animation_it->second.facings.find(facing);
    if (facing_it != animation_it->second.facings.end()) {
        return &facing_it->second;
    }

    const auto default_it = animation_it->second.facings.find("default");
    if (default_it != animation_it->second.facings.end()) {
        return &default_it->second;
    }

    const auto side_it = animation_it->second.facings.find("side");
    if (side_it != animation_it->second.facings.end()) {
        return &side_it->second;
    }

    return nullptr;
}

Rectangle SpriteMetadataLoader::GetFrame(const std::string& animation_name, const std::string& facing,
                                         float time_seconds) const {
    const SpriteFacingData* facing_data = ResolveFacing(animation_name, facing);
    if (!facing_data || facing_data->frames.empty()) {
        return Rectangle{0, 0, static_cast<float>(cell_width_), static_cast<float>(cell_height_)};
    }

    const float fps = facing_data->fps > 0.0f ? facing_data->fps : 1.0f;
    const int index = static_cast<int>(time_seconds * fps) % static_cast<int>(facing_data->frames.size());
    return facing_data->frames[static_cast<size_t>(index)];
}

Rectangle SpriteMetadataLoader::GetMirroredFrameRect(const Rectangle& source_rect) const {
    if (num_columns_ <= 0 || cell_width_ <= 0) {
        return source_rect;
    }

    const float cell_width = static_cast<float>(cell_width_);
    const int source_col = static_cast<int>(std::floor(source_rect.x / cell_width));
    if (source_col < 0 || source_col >= num_columns_) {
        return source_rect;
    }

    const int mirrored_col = num_columns_ - 1 - source_col;
    const float source_local_x = source_rect.x - (static_cast<float>(source_col) * cell_width);

    Rectangle mirrored = source_rect;
    mirrored.x = static_cast<float>(mirrored_col) * cell_width + source_local_x;
    return mirrored;
}

bool SpriteMetadataLoader::GetAnimationStats(const std::string& animation_name, const std::string& facing,
                                             int& out_frame_count, float& out_fps) const {
    const SpriteFacingData* facing_data = ResolveFacing(animation_name, facing);
    if (!facing_data || facing_data->frames.empty()) {
        return false;
    }
    out_frame_count = static_cast<int>(facing_data->frames.size());
    out_fps = facing_data->fps > 0.0f ? facing_data->fps : 1.0f;
    return true;
}
