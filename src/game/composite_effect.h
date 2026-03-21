#pragma once

#include <string>
#include <vector>

#include <raylib.h>

enum class CompositeEffectSheet {
    Base32,
    Tall32x64,
    Large96x96,
};

enum class CompositeEffectLayer {
    Ground,
    YSorted,
};

struct CompositeEffectPartDefinition {
    std::string animation;
    CompositeEffectSheet sheet = CompositeEffectSheet::Base32;
    CompositeEffectLayer layer = CompositeEffectLayer::YSorted;
    float offset_x_tiles = 0.0f;
    float offset_y_tiles = 0.0f;
    float sort_anchor_y_tiles = 0.0f;
};

struct CompositeEffectDefinition {
    std::string id;
    std::vector<CompositeEffectPartDefinition> parts;
};

struct CompositeEffectInstance {
    int id = -1;
    std::string effect_id;
    Vector2 origin_world = {0.0f, 0.0f};
    float age_seconds = 0.0f;
    float duration_seconds = 0.0f;
    bool alive = true;
};
