#include "assets/modular_character_asset.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "game/game_types.h"

namespace {

struct ModularFrameEntry {
    const nlohmann::json* frame_json = nullptr;
    int frame_index = -1;
    size_t source_order = 0;
};

const char* FacingToModularSuffix(FacingDirection facing) {
    switch (facing) {
        case FacingDirection::Top:
            return "n";
        case FacingDirection::TopRight:
            return "ne";
        case FacingDirection::Right:
            return "e";
        case FacingDirection::BottomRight:
            return "se";
        case FacingDirection::Bottom:
            return "s";
        case FacingDirection::BottomLeft:
            return "sw";
        case FacingDirection::Left:
            return "w";
        case FacingDirection::TopLeft:
            return "nw";
    }
    return "s";
}

int ExtractFrameIndex(const std::string& filename) {
    if (filename.empty()) {
        return -1;
    }

    size_t end = filename.find_last_of('.');
    if (end == std::string::npos) {
        end = filename.size();
    }

    size_t begin = end;
    while (begin > 0 && std::isdigit(static_cast<unsigned char>(filename[begin - 1])) != 0) {
        --begin;
    }
    if (begin == end) {
        return -1;
    }

    int value = 0;
    for (size_t i = begin; i < end; ++i) {
        value = value * 10 + (filename[i] - '0');
    }
    return value;
}

bool CollectFrameEntries(const nlohmann::json& frames_json, std::vector<ModularFrameEntry>& out_entries) {
    out_entries.clear();

    if (frames_json.is_array()) {
        out_entries.reserve(frames_json.size());
        size_t source_order = 0;
        for (const auto& frame_json : frames_json) {
            const std::string filename = frame_json.value("filename", std::string{});
            out_entries.push_back(ModularFrameEntry{&frame_json, ExtractFrameIndex(filename), source_order++});
        }
        return !out_entries.empty();
    }

    if (frames_json.is_object()) {
        out_entries.reserve(frames_json.size());
        size_t source_order = 0;
        for (const auto& item : frames_json.items()) {
            const auto& frame_json = item.value();
            const std::string filename = frame_json.value("filename", item.key());
            out_entries.push_back(ModularFrameEntry{&frame_json, ExtractFrameIndex(filename), source_order++});
        }

        std::stable_sort(out_entries.begin(), out_entries.end(),
                         [](const ModularFrameEntry& lhs, const ModularFrameEntry& rhs) {
                             if (lhs.frame_index >= 0 && rhs.frame_index >= 0 && lhs.frame_index != rhs.frame_index) {
                                 return lhs.frame_index < rhs.frame_index;
                             }
                             if (lhs.frame_index >= 0 && rhs.frame_index < 0) {
                                 return true;
                             }
                             if (lhs.frame_index < 0 && rhs.frame_index >= 0) {
                                 return false;
                             }
                             return lhs.source_order < rhs.source_order;
                         });
        return !out_entries.empty();
    }

    return false;
}

}  // namespace

ModularCharacterAsset::~ModularCharacterAsset() { Unload(); }

bool ModularCharacterAsset::LoadLayer(const std::string& layer_name, const std::string& json_path) {
    std::ifstream input(json_path);
    if (!input.is_open()) {
        TraceLog(LOG_ERROR, "Failed to open modular character metadata: %s", json_path.c_str());
        return false;
    }

    nlohmann::json json;
    input >> json;

    const auto frames_it = json.find("frames");
    const auto meta_it = json.find("meta");
    if (frames_it == json.end() || (!frames_it->is_array() && !frames_it->is_object()) || meta_it == json.end() ||
        !meta_it->is_object()) {
        TraceLog(LOG_ERROR, "Invalid modular character metadata: %s", json_path.c_str());
        return false;
    }

    const std::filesystem::path json_file_path(json_path);
    const std::filesystem::path texture_path =
        json_file_path.parent_path() / meta_it->value("image", std::string{});
    if (texture_path.empty()) {
        TraceLog(LOG_ERROR, "Missing modular character texture path: %s", json_path.c_str());
        return false;
    }

    Image source_image = LoadImage(texture_path.string().c_str());
    if (source_image.data == nullptr) {
        TraceLog(LOG_ERROR, "Failed to load modular character texture: %s", texture_path.string().c_str());
        return false;
    }

    ModularLayerAsset layer_asset;
    layer_asset.texture = LoadTextureFromImage(source_image);
    UnloadImage(source_image);
    if (layer_asset.texture.id == 0) {
        TraceLog(LOG_ERROR, "Failed to create modular character texture: %s", texture_path.string().c_str());
        return false;
    }
    SetTextureFilter(layer_asset.texture, TEXTURE_FILTER_POINT);

    std::vector<Rectangle> frame_rects;
    std::vector<float> frame_durations_seconds;
    std::vector<ModularFrameEntry> frame_entries;
    if (!CollectFrameEntries(*frames_it, frame_entries)) {
        TraceLog(LOG_ERROR, "Invalid modular character frame list: %s", json_path.c_str());
        UnloadTexture(layer_asset.texture);
        return false;
    }

    frame_rects.reserve(frame_entries.size());
    frame_durations_seconds.reserve(frame_entries.size());

    for (const ModularFrameEntry& frame_entry : frame_entries) {
        const auto& frame_json = *frame_entry.frame_json;
        const auto frame_it = frame_json.find("frame");
        if (frame_it == frame_json.end() || !frame_it->is_object()) {
            continue;
        }
        const Rectangle rect = {
            static_cast<float>(frame_it->value("x", 0)),
            static_cast<float>(frame_it->value("y", 0)),
            static_cast<float>(frame_it->value("w", 0)),
            static_cast<float>(frame_it->value("h", 0)),
        };
        if (rect.width <= 0.0f || rect.height <= 0.0f) {
            continue;
        }
        frame_rects.push_back(rect);
        frame_durations_seconds.push_back(static_cast<float>(frame_json.value("duration", 100)) / 1000.0f);
        if (layer_asset.frame_width == 0) {
            layer_asset.frame_width = static_cast<int>(rect.width);
        }
        if (layer_asset.frame_height == 0) {
            layer_asset.frame_height = static_cast<int>(rect.height);
        }
    }

    const auto frame_tags_it = meta_it->find("frameTags");
    if (frame_tags_it != meta_it->end() && frame_tags_it->is_array()) {
        for (const auto& tag_json : *frame_tags_it) {
            const std::string tag_name = tag_json.value("name", std::string{});
            if (tag_name.empty()) {
                continue;
            }
            const int from = tag_json.value("from", -1);
            const int to = tag_json.value("to", -1);
            if (from < 0 || to < from || static_cast<size_t>(to) >= frame_rects.size()) {
                continue;
            }

            ModularFrameRange range;
            const std::string direction = tag_json.value("direction", std::string("forward"));
            if (direction == "pingpong" && to > from) {
                for (int i = from; i <= to; ++i) {
                    range.frames.push_back(frame_rects[static_cast<size_t>(i)]);
                    range.frame_durations_seconds.push_back(frame_durations_seconds[static_cast<size_t>(i)]);
                }
                for (int i = to - 1; i > from; --i) {
                    range.frames.push_back(frame_rects[static_cast<size_t>(i)]);
                    range.frame_durations_seconds.push_back(frame_durations_seconds[static_cast<size_t>(i)]);
                }
            } else {
                for (int i = from; i <= to; ++i) {
                    range.frames.push_back(frame_rects[static_cast<size_t>(i)]);
                    range.frame_durations_seconds.push_back(frame_durations_seconds[static_cast<size_t>(i)]);
                }
            }

            if (range.IsValid()) {
                layer_asset.tags.emplace(tag_name, std::move(range));
            }
        }
    }

    layer_asset.loaded = true;

    auto existing_it = layers_.find(layer_name);
    if (existing_it != layers_.end()) {
        if (existing_it->second.texture.id != 0) {
            UnloadTexture(existing_it->second.texture);
        }
        existing_it->second = std::move(layer_asset);
    } else {
        layers_.emplace(layer_name, std::move(layer_asset));
    }

    return true;
}

void ModularCharacterAsset::Unload() {
    for (auto& entry : layers_) {
        if (entry.second.texture.id != 0) {
            UnloadTexture(entry.second.texture);
        }
    }
    layers_.clear();
}

bool ModularCharacterAsset::IsLoaded() const {
    for (const auto& entry : layers_) {
        if (entry.second.loaded) {
            return true;
        }
    }
    return false;
}

bool ModularCharacterAsset::HasLayer(const std::string& layer_name) const { return FindLayer(layer_name) != nullptr; }

bool ModularCharacterAsset::HasTag(const std::string& layer_name, const std::string& tag_name) const {
    return FindTag(layer_name, tag_name) != nullptr;
}

const Texture2D* ModularCharacterAsset::GetLayerTexture(const std::string& layer_name) const {
    const ModularLayerAsset* layer = FindLayer(layer_name);
    return layer != nullptr ? &layer->texture : nullptr;
}

Rectangle ModularCharacterAsset::GetFrame(const std::string& layer_name, const std::string& tag_name, float time_seconds) const {
    const ModularFrameRange* range = FindTag(layer_name, tag_name);
    if (range == nullptr || range->frames.empty()) {
        return Rectangle{};
    }

    float total_duration = 0.0f;
    for (float duration : range->frame_durations_seconds) {
        total_duration += duration;
    }
    if (total_duration <= 0.0f) {
        return range->frames.front();
    }

    float local_time = std::fmod(std::max(0.0f, time_seconds), total_duration);
    for (size_t i = 0; i < range->frames.size(); ++i) {
        const float duration = range->frame_durations_seconds[i];
        if (local_time < duration) {
            return range->frames[i];
        }
        local_time -= duration;
    }
    return range->frames.back();
}

float ModularCharacterAsset::GetTagDurationSeconds(const std::string& layer_name, const std::string& tag_name) const {
    const ModularFrameRange* range = FindTag(layer_name, tag_name);
    if (range == nullptr) {
        return 0.0f;
    }

    float total_duration = 0.0f;
    for (float duration : range->frame_durations_seconds) {
        total_duration += duration;
    }
    return total_duration;
}

int ModularCharacterAsset::GetTagFrameCount(const std::string& layer_name, const std::string& tag_name) const {
    const ModularFrameRange* range = FindTag(layer_name, tag_name);
    if (range == nullptr) {
        return 0;
    }
    return static_cast<int>(range->frames.size());
}

float ModularCharacterAsset::GetTagFrameStartSeconds(const std::string& layer_name, const std::string& tag_name,
                                                     int frame_index) const {
    const ModularFrameRange* range = FindTag(layer_name, tag_name);
    if (range == nullptr || frame_index <= 0) {
        return 0.0f;
    }

    float time_seconds = 0.0f;
    const int clamped_frame_index = std::min(frame_index, static_cast<int>(range->frame_durations_seconds.size()));
    for (int i = 0; i < clamped_frame_index; ++i) {
        time_seconds += range->frame_durations_seconds[static_cast<size_t>(i)];
    }
    return time_seconds;
}

int ModularCharacterAsset::GetFrameWidth(const std::string& layer_name) const {
    const ModularLayerAsset* layer = FindLayer(layer_name);
    return layer != nullptr ? layer->frame_width : 0;
}

int ModularCharacterAsset::GetFrameHeight(const std::string& layer_name) const {
    const ModularLayerAsset* layer = FindLayer(layer_name);
    return layer != nullptr ? layer->frame_height : 0;
}

std::string ModularCharacterAsset::BuildTag(const char* animation_name, FacingDirection facing) {
    return std::string("wiz_") + animation_name + "_" + FacingToModularSuffix(facing);
}

const ModularLayerAsset* ModularCharacterAsset::FindLayer(const std::string& layer_name) const {
    const auto it = layers_.find(layer_name);
    if (it == layers_.end() || !it->second.loaded) {
        return nullptr;
    }
    return &it->second;
}

const ModularFrameRange* ModularCharacterAsset::FindTag(const std::string& layer_name, const std::string& tag_name) const {
    const ModularLayerAsset* layer = FindLayer(layer_name);
    if (layer == nullptr) {
        return nullptr;
    }
    const auto tag_it = layer->tags.find(tag_name);
    if (tag_it == layer->tags.end()) {
        return nullptr;
    }
    return &tag_it->second;
}
