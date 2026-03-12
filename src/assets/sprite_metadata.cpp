#include "assets/sprite_metadata.h"

#include <cctype>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

namespace {

void ParseFrameList(const nlohmann::json& json_node, const char* key, int cell_width, int cell_height, int num_columns,
                    int num_rows, const char* animation_name, const char* variant_name,
                    std::vector<Rectangle>& out_frames) {
    const auto frames_it = json_node.find(key);
    if (frames_it == json_node.end() || !frames_it->is_array()) {
        return;
    }

    for (const auto& frame_coord : *frames_it) {
        if (!frame_coord.is_array() || frame_coord.size() < 2) {
            continue;
        }
        // Frame convention: [row, col].
        const int cell_y = frame_coord[0].get<int>();
        const int cell_x = frame_coord[1].get<int>();
        if (cell_x < 0 || cell_y < 0) {
            TraceLog(LOG_WARNING, "Negative sprite frame index skipped: %s/%s [%d,%d]", animation_name, variant_name,
                     cell_y, cell_x);
            continue;
        }
        if (num_columns > 0 && cell_x >= num_columns) {
            TraceLog(LOG_WARNING, "Sprite frame column out of range (num_columns=%d): %s/%s [%d,%d]", num_columns,
                     animation_name, variant_name, cell_y, cell_x);
            continue;
        }
        if (num_rows > 0 && cell_y >= num_rows) {
            TraceLog(LOG_WARNING, "Sprite frame row out of range (num_rows=%d): %s/%s [%d,%d]", num_rows,
                     animation_name, variant_name, cell_y, cell_x);
            continue;
        }
        out_frames.push_back(Rectangle{static_cast<float>(cell_x * cell_width), static_cast<float>(cell_y * cell_height),
                                       static_cast<float>(cell_width), static_cast<float>(cell_height)});
    }
}

void ParseFrameData(const nlohmann::json& json_node, int cell_width, int cell_height, int num_columns, int num_rows,
                    const char* animation_name, const char* variant_name, SpriteFacingData& out_data) {
    out_data.fps = json_node.value("fps", 1.0f);
    ParseFrameList(json_node, "frames", cell_width, cell_height, num_columns, num_rows, animation_name, variant_name,
                   out_data.frames);
    ParseFrameList(json_node, "frames_background", cell_width, cell_height, num_columns, num_rows, animation_name,
                   variant_name, out_data.frames_background);
    ParseFrameList(json_node, "frames_foreground", cell_width, cell_height, num_columns, num_rows, animation_name,
                   variant_name, out_data.frames_foreground);
}

const std::vector<Rectangle>* SelectFramesForLayer(const SpriteFacingData& frame_data, SpriteFrameLayer layer) {
    switch (layer) {
        case SpriteFrameLayer::Background:
            if (!frame_data.frames_background.empty()) {
                return &frame_data.frames_background;
            }
            if (!frame_data.frames.empty()) {
                return &frame_data.frames;
            }
            if (!frame_data.frames_foreground.empty()) {
                return &frame_data.frames_foreground;
            }
            break;
        case SpriteFrameLayer::Foreground:
            if (!frame_data.frames_foreground.empty()) {
                return &frame_data.frames_foreground;
            }
            if (!frame_data.frames.empty()) {
                return &frame_data.frames;
            }
            if (!frame_data.frames_background.empty()) {
                return &frame_data.frames_background;
            }
            break;
        case SpriteFrameLayer::Single:
        default:
            if (!frame_data.frames.empty()) {
                return &frame_data.frames;
            }
            if (!frame_data.frames_background.empty()) {
                return &frame_data.frames_background;
            }
            if (!frame_data.frames_foreground.empty()) {
                return &frame_data.frames_foreground;
            }
            break;
    }
    return nullptr;
}

bool HasFramesForLayer(const SpriteFacingData& frame_data, SpriteFrameLayer layer) {
    switch (layer) {
        case SpriteFrameLayer::Background:
            return !frame_data.frames_background.empty();
        case SpriteFrameLayer::Foreground:
            return !frame_data.frames_foreground.empty();
        case SpriteFrameLayer::Single:
        default:
            return !frame_data.frames.empty();
    }
}

bool IsIntegerKey(const std::string& key) {
    if (key.empty()) {
        return false;
    }
    size_t start = 0;
    if (key[0] == '-') {
        if (key.size() == 1) {
            return false;
        }
        start = 1;
    }
    for (size_t i = start; i < key.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(key[i]))) {
            return false;
        }
    }
    return true;
}

}  // namespace

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
        if (facings_it != animation_it.value().end() && facings_it->is_object()) {
            for (auto facing_it = facings_it->begin(); facing_it != facings_it->end(); ++facing_it) {
                SpriteFacingData facing_data;
                ParseFrameData(facing_it.value(), cell_width_, cell_height_, num_columns_, num_rows,
                               animation_it.key().c_str(), facing_it.key().c_str(), facing_data);
                if (facing_data.HasAnyFrames()) {
                    animation_data.facings[facing_it.key()] = facing_data;
                }
            }
        }

        const auto bitmask_it = animation_it.value().find("bitmask");
        if (bitmask_it != animation_it.value().end() && bitmask_it->is_object()) {
            for (auto mask_it = bitmask_it->begin(); mask_it != bitmask_it->end(); ++mask_it) {
                if (!IsIntegerKey(mask_it.key())) {
                    continue;
                }
                int mask_value = 0;
                try {
                    mask_value = std::stoi(mask_it.key());
                } catch (const std::exception&) {
                    TraceLog(LOG_WARNING, "Invalid bitmask key '%s' for animation '%s'", mask_it.key().c_str(),
                             animation_it.key().c_str());
                    continue;
                }

                SpriteFacingData variant_data;
                const std::string variant_name = std::string("bitmask:") + mask_it.key();
                ParseFrameData(mask_it.value(), cell_width_, cell_height_, num_columns_, num_rows,
                               animation_it.key().c_str(), variant_name.c_str(), variant_data);
                if (variant_data.HasAnyFrames()) {
                    animation_data.bitmask_variants[mask_value] = variant_data;
                }
            }
        }

        const auto dual_grid_it = animation_it.value().find("dual_grid");
        if (dual_grid_it != animation_it.value().end() && dual_grid_it->is_object()) {
            const nlohmann::json* dual_grid_masks = &(*dual_grid_it);
            const auto masks_it = dual_grid_it->find("masks");
            if (masks_it != dual_grid_it->end() && masks_it->is_object()) {
                dual_grid_masks = &(*masks_it);
            }

            for (auto mask_it = dual_grid_masks->begin(); mask_it != dual_grid_masks->end(); ++mask_it) {
                if (!IsIntegerKey(mask_it.key())) {
                    continue;
                }

                int mask_value = 0;
                try {
                    mask_value = std::stoi(mask_it.key());
                } catch (const std::exception&) {
                    TraceLog(LOG_WARNING, "Invalid dual-grid key '%s' for animation '%s'", mask_it.key().c_str(),
                             animation_it.key().c_str());
                    continue;
                }

                SpriteFacingData variant_data;
                const std::string variant_name = std::string("dual_grid:") + mask_it.key();
                ParseFrameData(mask_it.value(), cell_width_, cell_height_, num_columns_, num_rows,
                               animation_it.key().c_str(), variant_name.c_str(), variant_data);
                if (variant_data.HasAnyFrames()) {
                    animation_data.dual_grid_variants[mask_value] = variant_data;
                }
            }
        }

        if (!animation_data.facings.empty() || !animation_data.bitmask_variants.empty() ||
            !animation_data.dual_grid_variants.empty()) {
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

int SpriteMetadataLoader::GetCellWidth() const { return cell_width_; }

int SpriteMetadataLoader::GetCellHeight() const { return cell_height_; }

bool SpriteMetadataLoader::HasAnimation(const std::string& animation_name) const {
    return animations_.find(animation_name) != animations_.end();
}

bool SpriteMetadataLoader::HasDualGridAnimation(const std::string& animation_name) const {
    const auto it = animations_.find(animation_name);
    return it != animations_.end() && !it->second.dual_grid_variants.empty();
}

bool SpriteMetadataLoader::HasBitmaskAnimation(const std::string& animation_name) const {
    const auto it = animations_.find(animation_name);
    return it != animations_.end() && !it->second.bitmask_variants.empty();
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

const SpriteFacingData* SpriteMetadataLoader::ResolveDualGridVariant(const std::string& animation_name, int mask) const {
    const auto animation_it = animations_.find(animation_name);
    if (animation_it == animations_.end()) {
        return nullptr;
    }

    const auto exact_it = animation_it->second.dual_grid_variants.find(mask);
    if (exact_it != animation_it->second.dual_grid_variants.end()) {
        return &exact_it->second;
    }

    const auto full_it = animation_it->second.dual_grid_variants.find(15);
    if (full_it != animation_it->second.dual_grid_variants.end()) {
        return &full_it->second;
    }

    const auto empty_it = animation_it->second.dual_grid_variants.find(0);
    if (empty_it != animation_it->second.dual_grid_variants.end()) {
        return &empty_it->second;
    }

    if (!animation_it->second.dual_grid_variants.empty()) {
        return &animation_it->second.dual_grid_variants.begin()->second;
    }
    return nullptr;
}

const SpriteFacingData* SpriteMetadataLoader::ResolveBitmaskVariant(const std::string& animation_name, int bitmask) const {
    const auto animation_it = animations_.find(animation_name);
    if (animation_it == animations_.end()) {
        return nullptr;
    }

    const auto exact_it = animation_it->second.bitmask_variants.find(bitmask);
    if (exact_it != animation_it->second.bitmask_variants.end()) {
        return &exact_it->second;
    }

    // 8-bit map fallback: keep cardinal connections (N/W/E/S) and drop diagonal bits.
    constexpr int kCardinalBits = 2 | 8 | 16 | 64;
    const int cardinal_mask = bitmask & kCardinalBits;
    const auto cardinal_it = animation_it->second.bitmask_variants.find(cardinal_mask);
    if (cardinal_it != animation_it->second.bitmask_variants.end()) {
        return &cardinal_it->second;
    }

    const auto all_cardinals_it = animation_it->second.bitmask_variants.find(kCardinalBits);
    if (all_cardinals_it != animation_it->second.bitmask_variants.end()) {
        return &all_cardinals_it->second;
    }

    const auto all_neighbors_it = animation_it->second.bitmask_variants.find(15);
    if (all_neighbors_it != animation_it->second.bitmask_variants.end()) {
        return &all_neighbors_it->second;
    }

    const auto empty_neighbors_it = animation_it->second.bitmask_variants.find(0);
    if (empty_neighbors_it != animation_it->second.bitmask_variants.end()) {
        return &empty_neighbors_it->second;
    }

    if (!animation_it->second.bitmask_variants.empty()) {
        return &animation_it->second.bitmask_variants.begin()->second;
    }
    return nullptr;
}

bool SpriteMetadataLoader::ResolveFrameFromData(const SpriteFacingData& frame_data, float time_seconds,
                                                SpriteFrameLayer layer, Rectangle& out_frame) const {
    const std::vector<Rectangle>* frames = SelectFramesForLayer(frame_data, layer);
    if (!frames || frames->empty()) {
        return false;
    }

    const float fps = frame_data.fps > 0.0f ? frame_data.fps : 1.0f;
    const int index = static_cast<int>(time_seconds * fps) % static_cast<int>(frames->size());
    out_frame = (*frames)[static_cast<size_t>(index)];
    return true;
}

Rectangle SpriteMetadataLoader::GetFrame(const std::string& animation_name, const std::string& facing,
                                         float time_seconds) const {
    const SpriteFacingData* facing_data = ResolveFacing(animation_name, facing);
    if (!facing_data) {
        return Rectangle{0, 0, static_cast<float>(cell_width_), static_cast<float>(cell_height_)};
    }

    Rectangle frame = {0, 0, static_cast<float>(cell_width_), static_cast<float>(cell_height_)};
    if (!ResolveFrameFromData(*facing_data, time_seconds, SpriteFrameLayer::Single, frame)) {
        return Rectangle{0, 0, static_cast<float>(cell_width_), static_cast<float>(cell_height_)};
    }
    return frame;
}

bool SpriteMetadataLoader::HasDualGridLayer(const std::string& animation_name, int mask, SpriteFrameLayer layer) const {
    const SpriteFacingData* data = ResolveDualGridVariant(animation_name, mask);
    if (!data) {
        return false;
    }
    return HasFramesForLayer(*data, layer);
}

bool SpriteMetadataLoader::GetDualGridFrame(const std::string& animation_name, int mask, float time_seconds,
                                            SpriteFrameLayer layer, Rectangle& out_frame) const {
    const SpriteFacingData* data = ResolveDualGridVariant(animation_name, mask);
    if (!data) {
        return false;
    }
    return ResolveFrameFromData(*data, time_seconds, layer, out_frame);
}

bool SpriteMetadataLoader::HasBitmaskLayer(const std::string& animation_name, int bitmask, SpriteFrameLayer layer) const {
    const SpriteFacingData* data = ResolveBitmaskVariant(animation_name, bitmask);
    if (!data) {
        return false;
    }
    return HasFramesForLayer(*data, layer);
}

bool SpriteMetadataLoader::GetBitmaskFrame(const std::string& animation_name, int bitmask, float time_seconds,
                                           SpriteFrameLayer layer, Rectangle& out_frame) const {
    const SpriteFacingData* data = ResolveBitmaskVariant(animation_name, bitmask);
    if (!data) {
        return false;
    }
    return ResolveFrameFromData(*data, time_seconds, layer, out_frame);
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
    if (!facing_data) {
        return false;
    }

    const std::vector<Rectangle>* frames = SelectFramesForLayer(*facing_data, SpriteFrameLayer::Single);
    if (!frames || frames->empty()) {
        return false;
    }
    out_frame_count = static_cast<int>(frames->size());
    out_fps = facing_data->fps > 0.0f ? facing_data->fps : 1.0f;
    return true;
}
