#include "assets/composite_effects_loader.h"

#include <fstream>

#include <nlohmann/json.hpp>
#include <raylib.h>

namespace {

CompositeEffectLayer ParseLayer(const std::string& text, bool& ok) {
    ok = true;
    if (text == "ground") {
        return CompositeEffectLayer::Ground;
    }
    if (text == "ysorted") {
        return CompositeEffectLayer::YSorted;
    }
    ok = false;
    return CompositeEffectLayer::YSorted;
}

CompositeEffectSheet ParseSheet(const std::string& text, bool& ok) {
    ok = true;
    if (text == "base32") {
        return CompositeEffectSheet::Base32;
    }
    if (text == "tall32x64") {
        return CompositeEffectSheet::Tall32x64;
    }
    if (text == "large96x96") {
        return CompositeEffectSheet::Large96x96;
    }
    ok = false;
    return CompositeEffectSheet::Base32;
}

}  // namespace

bool CompositeEffectsLoader::LoadFromFile(const std::string& path) {
    loaded_ = false;
    last_error_.clear();
    definitions_.clear();

    std::ifstream input(path);
    if (!input.is_open()) {
        last_error_ = "failed to open composite effects file";
        TraceLog(LOG_WARNING, "CompositeEffects: %s (%s)", last_error_.c_str(), path.c_str());
        return false;
    }

    nlohmann::json json;
    try {
        input >> json;
    } catch (const std::exception& ex) {
        last_error_ = std::string("invalid JSON: ") + ex.what();
        TraceLog(LOG_ERROR, "CompositeEffects: %s", last_error_.c_str());
        return false;
    }

    const auto effects_it = json.find("effects");
    if (effects_it == json.end() || !effects_it->is_object()) {
        last_error_ = "missing required key 'effects'";
        TraceLog(LOG_ERROR, "CompositeEffects: %s", last_error_.c_str());
        return false;
    }

    for (auto it = effects_it->begin(); it != effects_it->end(); ++it) {
        if (!it.value().is_object()) {
            last_error_ = "effect definition must be an object";
            TraceLog(LOG_ERROR, "CompositeEffects: %s (id=%s)", last_error_.c_str(), it.key().c_str());
            return false;
        }

        CompositeEffectDefinition definition;
        definition.id = it.key();

        const auto parts_it = it.value().find("parts");
        if (parts_it == it.value().end() || !parts_it->is_array() || parts_it->empty()) {
            last_error_ = "effect must contain a non-empty parts array";
            TraceLog(LOG_ERROR, "CompositeEffects: %s (id=%s)", last_error_.c_str(), definition.id.c_str());
            return false;
        }

        for (const auto& part_json : *parts_it) {
            if (!part_json.is_object()) {
                last_error_ = "effect part must be an object";
                TraceLog(LOG_ERROR, "CompositeEffects: %s (id=%s)", last_error_.c_str(), definition.id.c_str());
                return false;
            }

            CompositeEffectPartDefinition part;
            bool ok = false;
            part.animation = part_json.value("animation", "");
            if (part.animation.empty()) {
                last_error_ = "effect part missing animation";
                TraceLog(LOG_ERROR, "CompositeEffects: %s (id=%s)", last_error_.c_str(), definition.id.c_str());
                return false;
            }

            part.sheet = ParseSheet(part_json.value("sheet", "base32"), ok);
            if (!ok) {
                last_error_ = "invalid effect part sheet";
                TraceLog(LOG_ERROR, "CompositeEffects: %s (id=%s)", last_error_.c_str(), definition.id.c_str());
                return false;
            }

            part.layer = ParseLayer(part_json.value("layer", "ysorted"), ok);
            if (!ok) {
                last_error_ = "invalid effect part layer";
                TraceLog(LOG_ERROR, "CompositeEffects: %s (id=%s)", last_error_.c_str(), definition.id.c_str());
                return false;
            }

            part.offset_x_tiles = part_json.value("offset_x_tiles", 0.0f);
            part.offset_y_tiles = part_json.value("offset_y_tiles", 0.0f);
            if (part_json.contains("sort_anchor_y_tiles")) {
                part.sort_anchor_y_tiles = part_json.value("sort_anchor_y_tiles", 0.0f);
            } else {
                // Legacy compatibility: previous schema counted from bottom to top.
                const float legacy_from_bottom = part_json.value("sort_anchor_from_bottom_tiles", 0.0f);
                part.sort_anchor_y_tiles = 1.0f - legacy_from_bottom;
            }

            definition.parts.push_back(part);
        }

        definitions_[definition.id] = definition;
    }

    loaded_ = true;
    TraceLog(LOG_INFO, "CompositeEffects: loaded %d definitions from %s", static_cast<int>(definitions_.size()),
             path.c_str());
    return true;
}

const CompositeEffectDefinition* CompositeEffectsLoader::FindById(const std::string& id) const {
    const auto it = definitions_.find(id);
    if (it == definitions_.end()) {
        return nullptr;
    }
    return &it->second;
}

bool CompositeEffectsLoader::IsLoaded() const { return loaded_; }

const std::string& CompositeEffectsLoader::GetLastError() const { return last_error_; }
