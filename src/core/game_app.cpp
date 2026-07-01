#include "core/game_app.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <exception>
#include <future>
#include <limits>
#include <random>
#include <string>
#include <unordered_set>

#include <nlohmann/json.hpp>
#include <raygui.h>
#include <raymath.h>
#include <rlgl.h>

#include "collision/spatial_hash_grid.h"
#include "collision/collision_world.h"
#include "core/constants.h"
#include "gameplay/action_intent.h"
#include "gameplay/snapshot_translation.h"
#include "net/lan_discovery.h"
#include "spells/fire_bolt_spell.h"
#include "spells/fire_flower_spell.h"
#include "spells/ice_wave_spell.h"
#include "spells/ice_wall_spell.h"
#include "ui/ui_lobby.h"
#include "ui/ui_main_menu.h"
#include "ui/ui_match.h"

namespace {

float ClampDt(float dt) { return std::min(dt, 0.1f); }

float ComputeDamageFlashAmount(float remaining_seconds) {
    if (remaining_seconds <= 0.0f) {
        return 0.0f;
    }
    const float normalized = std::clamp(
        remaining_seconds / std::max(0.0001f, Constants::kDamageFlashDurationSeconds), 0.0f, 1.0f);
    return normalized * normalized;
}

bool IsOutsideZoneDamageSource(const char* source) {
    return source != nullptr && std::strcmp(source, "outside_zone") == 0;
}

bool IsModularTreePrototypeId(const std::string& prototype_id) { return prototype_id == "tree_1"; }

std::string ToLowerAscii(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

std::string NormalizeSpawnCheatName(std::string text) {
    text = ToLowerAscii(std::move(text));
    for (char& ch : text) {
        if (ch == '-' || ch == ' ') {
            ch = '_';
        }
    }
    return text;
}

float GetIceWallDeathAnimationSampleTime(const SpriteMetadataLoader& metadata, float hp) {
    int death_frame_count = 0;
    float death_fps = 1.0f;
    float max_sample_time = 0.0f;
    if (metadata.GetAnimationStats("ice_wall_death", "default", death_frame_count, death_fps) && death_frame_count > 0) {
        max_sample_time = static_cast<float>(death_frame_count - 1) / std::max(0.001f, death_fps);
    }
    const float health_ratio = std::clamp(hp / std::max(0.001f, Constants::kIceWallMaxHp), 0.0f, 1.0f);
    return (1.0f - health_ratio) * std::max(0.0f, max_sample_time);
}

bool LoadAnchorPixelsFromMetadataFile(const std::string& metadata_path, Vector2* out_anchor_pixels) {
    if (out_anchor_pixels == nullptr) {
        return false;
    }

    std::ifstream input(metadata_path);
    if (!input.is_open()) {
        TraceLog(LOG_ERROR, "Failed to open modular asset metadata: %s", metadata_path.c_str());
        return false;
    }

    nlohmann::json json;
    input >> json;
    const auto anchor_it = json.find("anchor");
    if (anchor_it == json.end() || !anchor_it->is_object()) {
        TraceLog(LOG_ERROR, "Missing anchor in modular asset metadata: %s", metadata_path.c_str());
        return false;
    }

    *out_anchor_pixels = {
        static_cast<float>(anchor_it->value("x", 0)),
        static_cast<float>(anchor_it->value("y", 0)),
    };
    return true;
}

int DetermineWinningTeam(const MatchState& match) {
    if (match.red_team_kills > match.blue_team_kills) {
        return Constants::kTeamRed;
    }
    if (match.blue_team_kills > match.red_team_kills) {
        return Constants::kTeamBlue;
    }
    return -1;
}

using ColorKey = uint32_t;

ColorKey PackColorKey(const Color& color) {
    return static_cast<ColorKey>(color.r) | (static_cast<ColorKey>(color.g) << 8u) |
           (static_cast<ColorKey>(color.b) << 16u) | (static_cast<ColorKey>(color.a) << 24u);
}

bool BuildPaletteReplacementMaps(const std::string& palette_map_path, int team_count, int source_team_index,
                                 std::vector<std::unordered_map<ColorKey, Color>>* out_maps) {
    if (out_maps == nullptr || team_count <= 0 || source_team_index < 0 || source_team_index >= team_count ||
        palette_map_path.empty() || !FileExists(palette_map_path.c_str())) {
        return false;
    }

    Image palette_image = LoadImage(palette_map_path.c_str());
    if (palette_image.data == nullptr) {
        return false;
    }
    ImageFormat(&palette_image, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);

    bool valid = false;
    if (palette_image.width >= team_count && palette_image.width > source_team_index) {
        const Color* pixels = static_cast<const Color*>(palette_image.data);
        out_maps->assign(static_cast<size_t>(team_count), {});
        for (int y = 0; y < palette_image.height; ++y) {
            const Color source_color = pixels[y * palette_image.width + source_team_index];
            if (source_color.a == 0) {
                continue;
            }
            valid = true;
            const ColorKey source_key = PackColorKey(source_color);
            for (int team_index = 0; team_index < team_count; ++team_index) {
                (*out_maps)[static_cast<size_t>(team_index)][source_key] =
                    pixels[y * palette_image.width + team_index];
            }
        }
    }

    UnloadImage(palette_image);
    return valid;
}

Texture2D CreateRemappedTextureVariant(const std::string& texture_path,
                                       const std::unordered_map<ColorKey, Color>& replacement_map) {
    if (texture_path.empty() || !FileExists(texture_path.c_str())) {
        return {};
    }

    Image variant_image = LoadImage(texture_path.c_str());
    if (variant_image.data == nullptr) {
        return {};
    }
    ImageFormat(&variant_image, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    Color* pixels = static_cast<Color*>(variant_image.data);
    const int pixel_count = variant_image.width * variant_image.height;
    for (int i = 0; i < pixel_count; ++i) {
        const auto replacement_it = replacement_map.find(PackColorKey(pixels[i]));
        if (replacement_it != replacement_map.end()) {
            pixels[i] = replacement_it->second;
        }
    }

    Texture2D texture = LoadTextureFromImage(variant_image);
    if (texture.id != 0) {
        SetTextureFilter(texture, TEXTURE_FILTER_POINT);
    }
    UnloadImage(variant_image);
    return texture;
}

void UnloadTextureVector(std::vector<Texture2D>* textures) {
    if (textures == nullptr) {
        return;
    }
    for (Texture2D& texture : *textures) {
        if (texture.id != 0) {
            UnloadTexture(texture);
            texture = {};
        }
    }
    textures->clear();
}

std::vector<float> LoadCastleLevelRequirementsOrDefault(const std::string& json_path,
                                                        const std::vector<float>& fallback) {
    std::ifstream input(json_path);
    if (!input.is_open()) {
        return fallback;
    }

    try {
        nlohmann::json root;
        input >> root;
        const nlohmann::json* values = root.contains("level_energy_requirements")
                                           ? &root.at("level_energy_requirements")
                                           : (root.is_array() ? &root : nullptr);
        if (values == nullptr || !values->is_array()) {
            return fallback;
        }

        std::vector<float> loaded;
        loaded.reserve(values->size());
        for (const auto& entry : *values) {
            if (!entry.is_number()) {
                continue;
            }
            loaded.push_back(std::max(0.0f, entry.get<float>()));
        }
        if (!loaded.empty()) {
            return loaded;
        }
    } catch (const std::exception&) {
    }
    return fallback;
}

int64_t MakeGridKey(const GridCoord& cell) {
    return (static_cast<int64_t>(cell.x) << 32) ^ static_cast<uint32_t>(cell.y);
}

uint64_t ComputeInfluenceZoneSignature(const std::vector<InfluenceZoneCell>& zones) {
    std::vector<uint64_t> keys;
    keys.reserve(zones.size());
    for (const auto& zone : zones) {
        uint64_t packed = static_cast<uint64_t>(static_cast<uint32_t>(zone.cell.x)) << 32;
        packed ^= static_cast<uint64_t>(static_cast<uint32_t>(zone.cell.y)) << 1;
        packed ^= static_cast<uint64_t>(static_cast<uint32_t>(zone.team & 0x1));
        keys.push_back(packed);
    }
    std::sort(keys.begin(), keys.end());
    uint64_t hash = 1469598103934665603ull;
    for (uint64_t key : keys) {
        hash ^= key;
        hash *= 1099511628211ull;
    }
    hash ^= static_cast<uint64_t>(keys.size());
    hash *= 1099511628211ull;
    return hash;
}

std::vector<GridCoord> CollectConnectedActiveIceWallCells(const std::vector<IceWallPiece>& walls, const GridCoord& seed) {
    std::unordered_set<int64_t> active_cells;
    active_cells.reserve(walls.size() * 2);
    for (const auto& wall : walls) {
        if (wall.alive && wall.state == IceWallState::Active) {
            active_cells.insert(MakeGridKey(wall.cell));
        }
    }

    if (!active_cells.count(MakeGridKey(seed))) {
        return {};
    }

    std::vector<GridCoord> stack = {seed};
    std::unordered_set<int64_t> visited;
    visited.reserve(active_cells.size());
    visited.insert(MakeGridKey(seed));

    std::vector<GridCoord> component;
    component.reserve(active_cells.size());
    while (!stack.empty()) {
        const GridCoord current = stack.back();
        stack.pop_back();
        component.push_back(current);

        const GridCoord neighbors[4] = {
            {current.x - 1, current.y},
            {current.x + 1, current.y},
            {current.x, current.y - 1},
            {current.x, current.y + 1},
        };
        for (const GridCoord& neighbor : neighbors) {
            const int64_t key = MakeGridKey(neighbor);
            if (active_cells.count(key) && visited.insert(key).second) {
                stack.push_back(neighbor);
            }
        }
    }

    return component;
}

std::unordered_map<int64_t, int> BuildIceWallComponentDistances(const std::vector<IceWallPiece>& walls, const GridCoord& seed) {
    std::unordered_set<int64_t> active_cells;
    active_cells.reserve(walls.size() * 2);
    for (const auto& wall : walls) {
        if (wall.alive && wall.state == IceWallState::Active) {
            active_cells.insert(MakeGridKey(wall.cell));
        }
    }

    const int64_t seed_key = MakeGridKey(seed);
    if (!active_cells.count(seed_key)) {
        return {};
    }

    std::vector<GridCoord> queue = {seed};
    size_t queue_index = 0;
    std::unordered_map<int64_t, int> distances;
    distances.reserve(active_cells.size());
    distances.emplace(seed_key, 0);

    while (queue_index < queue.size()) {
        const GridCoord current = queue[queue_index++];
        const int current_distance = distances[MakeGridKey(current)];
        const GridCoord neighbors[4] = {
            {current.x - 1, current.y},
            {current.x + 1, current.y},
            {current.x, current.y - 1},
            {current.x, current.y + 1},
        };
        for (const GridCoord& neighbor : neighbors) {
            const int64_t key = MakeGridKey(neighbor);
            if (!active_cells.count(key) || distances.count(key)) {
                continue;
            }
            distances.emplace(key, current_distance + 1);
            queue.push_back(neighbor);
        }
    }

    return distances;
}

Rectangle MakeCenteredRect(Vector2 center, float width, float height) {
    return Rectangle{center.x - width * 0.5f, center.y - height * 0.5f, width, height};
}

bool IntersectsTileComponent(const Rectangle& box, int cell_size, const std::vector<GridCoord>& cells) {
    for (const GridCoord& cell : cells) {
        const Rectangle aabb = {static_cast<float>(cell.x * cell_size), static_cast<float>(cell.y * cell_size),
                                static_cast<float>(cell_size), static_cast<float>(cell_size)};
        Vector2 normal = {0.0f, 0.0f};
        float penetration = 0.0f;
        if (CollisionWorld::AabbVsAabb(box, aabb, normal, penetration)) {
            return true;
        }
    }
    return false;
}

Vector2 MoveTowardResolvedPosition(const Vector2& current, const Vector2& resolved) {
    const Vector2 delta = Vector2Subtract(resolved, current);
    const float distance = Vector2Length(delta);
    if (distance <= Constants::kCollisionResolveSnapDistance || distance <= 0.0001f) {
        return resolved;
    }

    const float step =
        std::min(distance, std::max(Constants::kCollisionResolveMinStep, distance * Constants::kCollisionResolveBlendFactor));
    return Vector2Add(current, Vector2Scale(delta, step / distance));
}

bool IsHorizontalDirection(SpellDirection direction) {
    return direction == SpellDirection::Horizontal || direction == SpellDirection::Left ||
           direction == SpellDirection::Right;
}

bool HasStatusEffect(const Player& player, StatusEffectType type) {
    return std::any_of(player.status_effects.begin(), player.status_effects.end(),
                       [&](const StatusEffectInstance& status) {
                           return status.type == type && status.remaining_seconds > 0.0f;
                       });
}

bool IsIceShardProjectileAnimation(const std::string& animation_key) {
    return animation_key == "ice_shard_projectile_idle";
}

bool IsIceShardProjectile(const Projectile& projectile) { return IsIceShardProjectileAnimation(projectile.animation_key); }

bool IsSnowParticleAnimation(const std::string& animation_key) {
    return animation_key == "snow_particle_born" || animation_key == "snow_particle_idle" ||
           animation_key == "snow_particle_death";
}

bool IsSparkParticleAnimation(const std::string& animation_key) {
    return animation_key == "spark_particle_born" || animation_key == "spark_particle_idle" ||
           animation_key == "spark_particle_death";
}

float GetFireStormConversionTriggerSeconds() {
    return Constants::kFireStormImpactDelaySeconds + Constants::kFireStormStormProjectileTravelSeconds;
}

float GetAnimationDurationSeconds(const SpriteMetadataLoader& metadata, const std::string& animation_key,
                                  const std::string& facing, int max_cycles, float fallback_seconds) {
    int frame_count = 0;
    float fps = 0.0f;
    if (!metadata.GetAnimationStats(animation_key, facing, frame_count, fps)) {
        return fallback_seconds;
    }
    const float cycle_seconds = static_cast<float>(frame_count) / std::max(0.001f, fps);
    return cycle_seconds * static_cast<float>(std::max(1, max_cycles));
}

Vector2 GetParticleRenderCenter(const Particle& particle, double now_seconds) {
    Vector2 center = particle.pos;
    if (particle.use_visual_z) {
        center.y -= particle.visual_z;
        if (particle.sway_amplitude > 0.0f && particle.sway_frequency_hz > 0.0f) {
            const float phase =
                particle.sway_phase + static_cast<float>(now_seconds) * particle.sway_frequency_hz * 2.0f * PI;
            center.x += particle.sway_amplitude * std::sin(phase);
        }
    }
    return center;
}

float EvaluateStormArcHeight(float normalized_time) {
    const float t = std::clamp(normalized_time, 0.0f, 1.0f);
    return std::sin(t * PI) * Constants::kFireStormStormArcPeakHeight;
}

float EvaluateFireSpiritArcHeight(float normalized_time, float peak_height) {
    const float t = std::clamp(normalized_time, 0.0f, 1.0f);
    return std::sin(t * PI) * std::max(0.0f, peak_height);
}

float GetFireWaveHalfAngleRadians() {
    return Constants::kFireWaveHalfAngleDegrees * DEG2RAD;
}

float GetFireWaveProgress(const FireWaveSegment& segment, float simulation_time_seconds) {
    return std::clamp((simulation_time_seconds - segment.start_time_seconds) /
                          std::max(0.001f, segment.duration_seconds),
                      0.0f, 1.0f);
}

float GetFireWaveElapsedSeconds(const FireWaveSegment& segment, float simulation_time_seconds) {
    return std::max(0.0f, simulation_time_seconds - segment.start_time_seconds);
}

float GetFireWaveTravelDistance(const FireWaveSegment& segment, float simulation_time_seconds) {
    return segment.range_world * GetFireWaveProgress(segment, simulation_time_seconds);
}

float GetFireWaveRadiusAtDistance(float travel_distance) {
    return std::max(0.0f, travel_distance * std::tan(GetFireWaveHalfAngleRadians()));
}

Vector2 GetFireWaveCenterWorld(const FireWaveSegment& segment, float simulation_time_seconds) {
    const float distance = GetFireWaveTravelDistance(segment, simulation_time_seconds);
    const Vector2 dir = {std::cos(segment.direction_radians), std::sin(segment.direction_radians)};
    return Vector2Add(segment.origin_world, Vector2Scale(dir, distance));
}

int64_t MakeCellKey(const GridCoord& cell) {
    return (static_cast<int64_t>(cell.x) << 32) ^ static_cast<uint32_t>(cell.y);
}

bool IsEmbersWalkableTile(TileType tile) {
    return tile == TileType::Grass || tile == TileType::SpawnPoint || tile == TileType::StoneTiles;
}

struct OccluderRevealCircle {
    Vector2 screen_center = {0.0f, 0.0f};
    float inner_radius_px = 0.0f;
    float outer_radius_px = 0.0f;
};

constexpr int kInfluenceDistanceSamplesPerTile = 2;
constexpr float kInfluenceDistanceOuterTailPx = 6.0f;
constexpr float kInfluenceDistanceInnerTailPx = 22.0f;
constexpr int kStaticRenderChunkSizeTiles = 16;
constexpr int kConsoleMaxVisibleLines = 5;
constexpr int kConsoleCharsPerLine = 48;
constexpr float kConsoleLifetimeSeconds = 5.0f;
constexpr int kChatMaxChars = 180;

struct TeamVisualStyle {
    Color primary_color;
    const char* display_name;
};

const TeamVisualStyle& GetTeamVisualStyle(int team) {
    static const TeamVisualStyle kBlueStyle{{120, 170, 255, 255}, "Blue"};
    static const TeamVisualStyle kRedStyle{{255, 120, 120, 255}, "Red"};
    return team == Constants::kTeamRed ? kRedStyle : kBlueStyle;
}

Color TeamUiColor(int team) {
    return GetTeamVisualStyle(team).primary_color;
}

const char* TeamDisplayName(int team) {
    return GetTeamVisualStyle(team).display_name;
}

ConsoleTextSpanMessage MakeConsoleSpan(std::string text, Color color) {
    return {std::move(text), color.r, color.g, color.b, color.a};
}

Color SpanColor(const ConsoleTextSpanMessage& span) { return Color{span.r, span.g, span.b, span.a}; }

float NormalizeAngleRadians(float radians) {
    while (radians > PI) {
        radians -= 2.0f * PI;
    }
    while (radians < -PI) {
        radians += 2.0f * PI;
    }
    return radians;
}

float LerpAngleRadiansShortest(float from, float to, float t) {
    const float delta = NormalizeAngleRadians(to - from);
    return NormalizeAngleRadians(from + delta * std::clamp(t, 0.0f, 1.0f));
}

float EaseSigmoid01(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    constexpr float kSharpness = 20.0f;
    const float y = 1.0f / (1.0f + std::exp(-kSharpness * (t - 0.5f)));
    const float y0 = 1.0f / (1.0f + std::exp(kSharpness * 0.5f));
    const float y1 = 1.0f / (1.0f + std::exp(-kSharpness * 0.5f));
    return (y - y0) / std::max(0.0001f, y1 - y0);
}

std::unordered_set<int64_t> CollectInfluenceZoneCells(const std::vector<InfluenceZoneCell>& zones, int team) {
    std::unordered_set<int64_t> active_cells;
    active_cells.reserve(zones.size() * 2);
    for (const auto& zone : zones) {
        if (zone.team == team) {
            active_cells.insert(MakeGridKey(zone.cell));
        }
    }
    return active_cells;
}

void DistanceTransform1D(const std::vector<float>& input, int n, std::vector<float>* output) {
    if (output == nullptr || n <= 0) {
        return;
    }
    constexpr float kInf = 1.0e20f;
    output->assign(static_cast<size_t>(n), 0.0f);
    std::vector<int> v(static_cast<size_t>(n), 0);
    std::vector<float> z(static_cast<size_t>(n + 1), 0.0f);

    int k = 0;
    v[0] = 0;
    z[0] = -kInf;
    z[1] = kInf;

    for (int q = 1; q < n; ++q) {
        float s = 0.0f;
        while (true) {
            const int vk = v[static_cast<size_t>(k)];
            s = ((input[static_cast<size_t>(q)] + static_cast<float>(q * q)) -
                 (input[static_cast<size_t>(vk)] + static_cast<float>(vk * vk))) /
                std::max(1.0f, 2.0f * static_cast<float>(q - vk));
            if (s > z[static_cast<size_t>(k)]) {
                break;
            }
            --k;
        }
        ++k;
        v[static_cast<size_t>(k)] = q;
        z[static_cast<size_t>(k)] = s;
        z[static_cast<size_t>(k + 1)] = kInf;
    }

    k = 0;
    for (int q = 0; q < n; ++q) {
        while (z[static_cast<size_t>(k + 1)] < static_cast<float>(q)) {
            ++k;
        }
        const float dx = static_cast<float>(q - v[static_cast<size_t>(k)]);
        (*output)[static_cast<size_t>(q)] = dx * dx + input[static_cast<size_t>(v[static_cast<size_t>(k)])];
    }
}

std::vector<float> DistanceTransform2D(const std::vector<float>& input, int width, int height) {
    if (width <= 0 || height <= 0 || input.size() != static_cast<size_t>(width * height)) {
        return {};
    }

    std::vector<float> temp(static_cast<size_t>(width * height), 0.0f);
    std::vector<float> output(static_cast<size_t>(width * height), 0.0f);
    std::vector<float> line_input;
    std::vector<float> line_output;

    line_input.resize(static_cast<size_t>(height));
    for (int x = 0; x < width; ++x) {
        for (int y = 0; y < height; ++y) {
            line_input[static_cast<size_t>(y)] = input[static_cast<size_t>(y * width + x)];
        }
        DistanceTransform1D(line_input, height, &line_output);
        for (int y = 0; y < height; ++y) {
            temp[static_cast<size_t>(y * width + x)] = line_output[static_cast<size_t>(y)];
        }
    }

    line_input.resize(static_cast<size_t>(width));
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            line_input[static_cast<size_t>(x)] = temp[static_cast<size_t>(y * width + x)];
        }
        DistanceTransform1D(line_input, width, &line_output);
        for (int x = 0; x < width; ++x) {
            output[static_cast<size_t>(y * width + x)] = line_output[static_cast<size_t>(x)];
        }
    }

    return output;
}

void BuildInfluenceDistanceTexture(Texture2D* texture, bool* has_texture,
                                   const std::vector<Color>& pixels, int tex_width, int tex_height) {
    if (texture == nullptr || has_texture == nullptr || tex_width <= 0 || tex_height <= 0 ||
        pixels.size() != static_cast<size_t>(tex_width * tex_height)) {
        return;
    }
    if (*has_texture && (texture->width != tex_width || texture->height != tex_height)) {
        UnloadTexture(*texture);
        *texture = {};
        *has_texture = false;
    }
    if (!*has_texture) {
        Image image = GenImageColor(tex_width, tex_height, BLACK);
        *texture = LoadTextureFromImage(image);
        UnloadImage(image);
        *has_texture = (texture->id != 0);
        if (*has_texture) {
            SetTextureFilter(*texture, TEXTURE_FILTER_BILINEAR);
            SetTextureWrap(*texture, TEXTURE_WRAP_CLAMP);
        }
    }
    if (!*has_texture) {
        return;
    }

    UpdateTexture(*texture, pixels.data());
}

void BuildInfluenceDistanceField(InfluenceDistanceField* field, const std::unordered_set<int64_t>& active_cells,
                                 int map_width, int map_height, int cell_size, int samples_per_tile) {
    if (field == nullptr) {
        return;
    }

    const int tex_width = std::max(1, map_width * samples_per_tile);
    const int tex_height = std::max(1, map_height * samples_per_tile);
    field->width = tex_width;
    field->height = tex_height;

    const float min_signed_dist = -kInfluenceDistanceOuterTailPx;
    const float max_signed_dist = kInfluenceDistanceInnerTailPx;
    const float sample_step = static_cast<float>(cell_size) / static_cast<float>(samples_per_tile);

    field->pixels.assign(static_cast<size_t>(tex_width * tex_height), Color{0, 0, 0, 255});
    if (active_cells.empty()) {
        return;
    }

    std::vector<uint8_t> inside_mask(static_cast<size_t>(tex_width * tex_height), 0);
    for (const int64_t key : active_cells) {
        const GridCoord cell = {static_cast<int>(key >> 32), static_cast<int>(key & 0xffffffff)};
        if (cell.x < 0 || cell.y < 0 || cell.x >= map_width || cell.y >= map_height) {
            continue;
        }
        const int x0 = cell.x * samples_per_tile;
        const int y0 = cell.y * samples_per_tile;
        for (int sy = 0; sy < samples_per_tile; ++sy) {
            for (int sx = 0; sx < samples_per_tile; ++sx) {
                const int x = x0 + sx;
                const int y = y0 + sy;
                inside_mask[static_cast<size_t>(y * tex_width + x)] = 1;
            }
        }
    }

    constexpr float kInf = 1.0e20f;
    std::vector<float> inside_features(static_cast<size_t>(tex_width * tex_height), kInf);
    std::vector<float> outside_features(static_cast<size_t>(tex_width * tex_height), kInf);
    for (size_t i = 0; i < inside_mask.size(); ++i) {
        if (inside_mask[i] != 0) {
            inside_features[i] = 0.0f;
        } else {
            outside_features[i] = 0.0f;
        }
    }

    const std::vector<float> dist_to_inside_sq = DistanceTransform2D(inside_features, tex_width, tex_height);
    const std::vector<float> dist_to_outside_sq = DistanceTransform2D(outside_features, tex_width, tex_height);

    for (int y = 0; y < tex_height; ++y) {
        for (int x = 0; x < tex_width; ++x) {
            const size_t index = static_cast<size_t>(y * tex_width + x);
            const bool inside = inside_mask[index] != 0;
            const float dist_sq = inside ? dist_to_outside_sq[index] : dist_to_inside_sq[index];
            float signed_dist = std::sqrt(std::max(0.0f, dist_sq)) * sample_step;
            signed_dist = inside ? signed_dist : -signed_dist;
            signed_dist = std::clamp(signed_dist, min_signed_dist, max_signed_dist);
            const float encoded = (signed_dist - min_signed_dist) / (max_signed_dist - min_signed_dist);
            const unsigned char value = static_cast<unsigned char>(std::roundf(encoded * 255.0f));
            field->pixels[index] = Color{value, value, value, 255};
        }
    }
}

void ClearInfluenceDistanceField(InfluenceDistanceField* field) {
    if (field == nullptr) {
        return;
    }
    field->width = 0;
    field->height = 0;
    field->pixels.clear();
}

bool HasInfluenceDistanceField(const InfluenceDistanceField& field) {
    return field.width > 0 && field.height > 0 &&
           field.pixels.size() == static_cast<size_t>(field.width * field.height);
}

void CopyInfluenceDistanceField(const InfluenceDistanceField& src, InfluenceDistanceField* dst) {
    if (dst == nullptr) {
        return;
    }
    *dst = src;
}

void BlendInfluenceDistanceField(const InfluenceDistanceField& from, const InfluenceDistanceField& to,
                                 float t, InfluenceDistanceField* out) {
    if (out == nullptr) {
        return;
    }
    if (!HasInfluenceDistanceField(from)) {
        *out = to;
        return;
    }
    if (!HasInfluenceDistanceField(to)) {
        *out = from;
        return;
    }
    if (from.width != to.width || from.height != to.height ||
        from.pixels.size() != to.pixels.size()) {
        *out = to;
        return;
    }

    out->width = to.width;
    out->height = to.height;
    out->pixels.resize(to.pixels.size());
    const float blend = std::clamp(t, 0.0f, 1.0f);
    for (size_t i = 0; i < to.pixels.size(); ++i) {
        const float from_v = static_cast<float>(from.pixels[i].r);
        const float to_v = static_cast<float>(to.pixels[i].r);
        const unsigned char value = static_cast<unsigned char>(std::round(from_v + (to_v - from_v) * blend));
        out->pixels[i] = Color{value, value, value, 255};
    }
}

void BuildZeroInfluenceDistanceField(int width, int height, InfluenceDistanceField* field) {
    if (field == nullptr) {
        return;
    }
    field->width = std::max(0, width);
    field->height = std::max(0, height);
    field->pixels.assign(static_cast<size_t>(field->width * field->height), Color{0, 0, 0, 255});
}

InfluenceBuildResult BuildInfluenceFields(const InfluenceBuildRequest& request) {
    InfluenceBuildResult result;
    result.signature = request.signature;
    result.generation = request.generation;
    const auto red_cells = CollectInfluenceZoneCells(request.zones, Constants::kTeamRed);
    const auto blue_cells = CollectInfluenceZoneCells(request.zones, Constants::kTeamBlue);
    BuildInfluenceDistanceField(&result.red_field, red_cells, request.map_width, request.map_height, request.cell_size,
                                request.samples_per_tile);
    BuildInfluenceDistanceField(&result.blue_field, blue_cells, request.map_width, request.map_height,
                                request.cell_size, request.samples_per_tile);
    return result;
}

bool ContainsWorldPointExpanded(const Rectangle& rect, Vector2 point, float radius) {
    return point.x >= rect.x - radius && point.x <= rect.x + rect.width + radius &&
           point.y >= rect.y - radius && point.y <= rect.y + rect.height + radius;
}

float SmoothRootSigmoid(float t) {
    const float clamped = std::clamp(t, 0.0f, 1.0f);
    const float x = (clamped - 0.5f) * 10.0f;
    const float s = 1.0f / (1.0f + std::exp(-x));
    const float kMin = 1.0f / (1.0f + std::exp(5.0f));
    const float kMax = 1.0f / (1.0f + std::exp(-5.0f));
    return (s - kMin) / (kMax - kMin);
}

bool IsStaticFireBolt(const Projectile& projectile) {
    return projectile.animation_key == "fire_storm_static_large";
}

struct StatusEffectUiSpec {
    const char* animation = nullptr;
    bool is_buff = true;
};

StatusEffectUiSpec GetStatusEffectUiSpec(StatusEffectType type) {
    switch (type) {
        case StatusEffectType::Regeneration:
            return {"health_potion_small", true};
        case StatusEffectType::ManaRegeneration:
            return {"mana_potion", true};
        case StatusEffectType::Stunned:
            return {"static_upgrade", false};
        case StatusEffectType::Rooted:
            return {"earth_rune_rooted_idle", false};
        case StatusEffectType::RootedRecovery:
            return {"earth_rune_rooted_idle", false};
        case StatusEffectType::Frozen:
            return {"ice_shard_projectile_idle", false};
        case StatusEffectType::Burning:
            return {"embers_idle", false};
    }
    return {"altar_rune_slot_generic", true};
}

Color GetRuneFallbackColor(RuneType rune_type) {
    switch (rune_type) {
        case RuneType::Fire:
            return Color{255, 140, 44, 220};
        case RuneType::Water:
            return Color{66, 180, 255, 220};
        case RuneType::Catalyst:
            return Color{208, 218, 116, 220};
        case RuneType::Earth:
            return Color{143, 110, 64, 220};
        case RuneType::FireStormDummy:
            return Color{255, 158, 64, 220};
        case RuneType::None:
            return Color{255, 255, 255, 0};
    }
    return Color{255, 255, 255, 0};
}

float GetRuneManaCost(RuneType rune_type) {
    switch (rune_type) {
        case RuneType::Fire:
            return Constants::kRuneManaCostFire;
        case RuneType::Water:
            return Constants::kRuneManaCostWater;
        case RuneType::Catalyst:
            return Constants::kRuneManaCostCatalyst;
        case RuneType::Earth:
            return Constants::kRuneManaCostEarth;
        case RuneType::FireStormDummy:
            return Constants::kRuneManaCostFireStormDummy;
        case RuneType::None:
            return 0.0f;
    }
    return 0.0f;
}

float GetRuneCooldownSeconds(RuneType rune_type) {
    switch (rune_type) {
        case RuneType::Fire:
            return Constants::kRuneCooldownFireSeconds;
        case RuneType::Water:
            return Constants::kRuneCooldownWaterSeconds;
        case RuneType::Catalyst:
            return Constants::kRuneCooldownCatalystSeconds;
        case RuneType::Earth:
            return Constants::kRuneCooldownEarthSeconds;
        case RuneType::FireStormDummy:
            return Constants::kRuneCooldownFireStormDummySeconds;
        case RuneType::None:
            return 0.0f;
    }
    return 0.0f;
}

float GetRuneActivationSeconds(RuneType rune_type) {
    switch (rune_type) {
        case RuneType::Fire:
            return Constants::kRuneActivationFireSeconds;
        case RuneType::Water:
            return Constants::kRuneActivationWaterSeconds;
        case RuneType::Catalyst:
            return Constants::kRuneActivationCatalystSeconds;
        case RuneType::Earth:
            return Constants::kRuneActivationEarthSeconds;
        case RuneType::FireStormDummy:
            return Constants::kRuneActivationFireStormDummySeconds;
        case RuneType::None:
            return 0.0f;
    }
    return 0.0f;
}

const std::array<RuneType, 5>& GetAllCastableRuneTypes() {
    static const std::array<RuneType, 5> kRunes = {
        RuneType::Fire,
        RuneType::Water,
        RuneType::Catalyst,
        RuneType::Earth,
        RuneType::FireStormDummy,
    };
    return kRunes;
}

size_t GetRuneCooldownIndex(RuneType rune_type) {
    switch (rune_type) {
        case RuneType::Fire:
            return 0;
        case RuneType::Water:
            return 1;
        case RuneType::Catalyst:
            return 2;
        case RuneType::Earth:
            return 3;
        case RuneType::FireStormDummy:
            return 4;
        case RuneType::None:
            break;
    }
    return 0;
}

bool RuneUsesCharges(RuneType rune_type) {
    return rune_type == RuneType::Catalyst;
}

int GetInitialRuneCharges(RuneType rune_type) {
    switch (rune_type) {
        case RuneType::Catalyst:
            return 0;
        case RuneType::Fire:
        case RuneType::Water:
        case RuneType::Earth:
        case RuneType::FireStormDummy:
        case RuneType::None:
            return 0;
    }
    return 0;
}

const char* GetRuneChargePickupObjectId(RuneType rune_type) {
    switch (rune_type) {
        case RuneType::Catalyst:
            return "catalyst_charge_pickup";
        case RuneType::Fire:
        case RuneType::Water:
        case RuneType::Earth:
        case RuneType::FireStormDummy:
        case RuneType::None:
            return nullptr;
    }
    return nullptr;
}

std::optional<RuneType> GetRuneTypeForChargePickupPrototype(const std::string& prototype_id) {
    if (prototype_id == "catalyst_charge_pickup") {
        return RuneType::Catalyst;
    }
    return std::nullopt;
}

int GetPlayerRuneChargeCount(const Player& player, RuneType rune_type) {
    if (rune_type == RuneType::None || !RuneUsesCharges(rune_type)) {
        return 0;
    }
    return player.rune_charge_counts[GetRuneCooldownIndex(rune_type)];
}

int GetMaxRuneCharges(RuneType rune_type) {
    switch (rune_type) {
        case RuneType::Catalyst:
            return Constants::kCatalystChargeMaxHeld;
        case RuneType::Fire:
        case RuneType::Water:
        case RuneType::Earth:
        case RuneType::FireStormDummy:
        case RuneType::None:
            return 0;
    }
    return 0;
}

void SetPlayerRuneChargeCount(Player& player, RuneType rune_type, int count) {
    if (rune_type == RuneType::None || !RuneUsesCharges(rune_type)) {
        return;
    }
    player.rune_charge_counts[GetRuneCooldownIndex(rune_type)] =
        std::clamp(count, 0, std::max(0, GetMaxRuneCharges(rune_type)));
}

float GetPlayerRuneCooldownRemaining(const Player& player, RuneType rune_type) {
    if (rune_type == RuneType::None) {
        return 0.0f;
    }
    return player.rune_cooldown_remaining[GetRuneCooldownIndex(rune_type)];
}

float GetPlayerRuneCooldownTotal(const Player& player, RuneType rune_type) {
    if (rune_type == RuneType::None) {
        return 0.0f;
    }
    return player.rune_cooldown_total[GetRuneCooldownIndex(rune_type)];
}

void SetPlayerRuneCooldown(Player& player, RuneType rune_type, float remaining, float total) {
    if (rune_type == RuneType::None) {
        return;
    }
    const size_t index = GetRuneCooldownIndex(rune_type);
    player.rune_cooldown_remaining[index] = remaining;
    player.rune_cooldown_total[index] = total;
}

void ApplyRuneCooldownsAfterCast(Player& player, RuneType selected_rune_type) {
    const float selected_cooldown = GetRuneCooldownSeconds(selected_rune_type);
    for (RuneType rune_type : GetAllCastableRuneTypes()) {
        if (rune_type == selected_rune_type) {
            SetPlayerRuneCooldown(player, rune_type, selected_cooldown, selected_cooldown);
            continue;
        }

        const float clamped_remaining =
            std::max(GetPlayerRuneCooldownRemaining(player, rune_type), Constants::kRuneMinCrossCooldownSeconds);
        const float clamped_total =
            std::max(GetPlayerRuneCooldownTotal(player, rune_type), Constants::kRuneMinCrossCooldownSeconds);
        SetPlayerRuneCooldown(player, rune_type, clamped_remaining, clamped_total);
    }
}

const char* GetRuneSpriteAnimationKey(RuneType rune_type) {
    switch (rune_type) {
        case RuneType::Fire:
            return "fire_rune";
        case RuneType::Water:
            return "water_rune";
        case RuneType::Catalyst:
            return "catalist_rune";
        case RuneType::Earth:
            return "earth_rune_idle";
        case RuneType::FireStormDummy:
            return "fire_storm_bottom";
        case RuneType::None:
            return "";
    }
    return "";
}

const char* GetRuneBornSpriteAnimationKey(RuneType rune_type) {
    switch (rune_type) {
        case RuneType::Fire:
            return "fire_rune_born";
        case RuneType::Water:
            return "water_rune_born";
        case RuneType::Catalyst:
            return "catalist_rune_born";
        case RuneType::Earth:
            return "roots_born";
        case RuneType::FireStormDummy:
            return "fire_storm_born_bot";
        case RuneType::None:
            return "rune_born";
    }
    return "rune_born";
}

const char* GetEarthRuneAnimationKey(const Rune& rune) {
    switch (rune.earth_trap_state) {
        case EarthRuneTrapState::IdleRune:
            return "earth_rune_idle";
        case EarthRuneTrapState::Slamming:
            return "earth_rune_slam";
        case EarthRuneTrapState::RootedIdle:
            return "earth_rune_rooted_idle";
        case EarthRuneTrapState::RootedDeath:
            return "earth_rune_rooted_death";
    }
    return "earth_rune_idle";
}

const char* GetFireStormRuneAnimationKey(const Rune& rune) {
    switch (rune.fire_storm_visual_state) {
        case FireStormRuneVisualState::Born:
            return "fire_storm_born_bot";
        case FireStormRuneVisualState::Dying:
            return "fire_storm_death_bot";
        case FireStormRuneVisualState::Idle:
        case FireStormRuneVisualState::None:
            return "fire_storm_bottom";
    }
    return "fire_storm_bottom";
}

const char* GetFireStormRuneTopAnimationKey(const Rune& rune) {
    switch (rune.fire_storm_visual_state) {
        case FireStormRuneVisualState::Born:
            return "fire_storm_born_top";
        case FireStormRuneVisualState::Dying:
            return "fire_storm_death_top";
        case FireStormRuneVisualState::Idle:
        case FireStormRuneVisualState::None:
            return "fire_storm_top";
    }
    return "fire_storm_top";
}

const char* GetFireStormRuneBackgroundAnimationKey(const Rune& rune) {
    return "fire_rune_background";
}

const char* GetFireStormRuneGroundOverlayAnimationKey(const Rune& rune) {
    switch (rune.fire_storm_visual_state) {
        case FireStormRuneVisualState::Born:
            return "fire_storm_born_ground";
        case FireStormRuneVisualState::Dying:
            return "fire_storm_ground";
        case FireStormRuneVisualState::Idle:
        case FireStormRuneVisualState::None:
            return "fire_storm_ground";
    }
    return "fire_storm_ground";
}

float AimToDegrees(Vector2 aim_dir) {
    return static_cast<float>(std::atan2(aim_dir.y, aim_dir.x) * (180.0 / PI));
}

Rectangle SnapRect(Rectangle rect) {
    rect.x = std::roundf(rect.x);
    rect.y = std::roundf(rect.y);
    rect.width = std::roundf(rect.width);
    rect.height = std::roundf(rect.height);
    return rect;
}

Rectangle InsetSourceRect(Rectangle src, float inset_pixels) {
    if (inset_pixels <= 0.0f) {
        return src;
    }

    const float sign_w = src.width >= 0.0f ? 1.0f : -1.0f;
    const float sign_h = src.height >= 0.0f ? 1.0f : -1.0f;
    const float abs_w = std::fabs(src.width);
    const float abs_h = std::fabs(src.height);

    if (abs_w > inset_pixels * 2.0f) {
        src.x += inset_pixels * sign_w;
        src.width = sign_w * (abs_w - inset_pixels * 2.0f);
    }
    if (abs_h > inset_pixels * 2.0f) {
        src.y += inset_pixels * sign_h;
        src.height = sign_h * (abs_h - inset_pixels * 2.0f);
    }
    return src;
}

float HashToUnitFloat(int x, int y) {
    uint32_t value = static_cast<uint32_t>(x) * 0x1f123bb5u;
    value ^= static_cast<uint32_t>(y) * 0x5f356495u;
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    value ^= value >> 16u;
    return static_cast<float>(value & 0x00ffffffu) / static_cast<float>(0x01000000u);
}

std::string ResolveRuntimePath(const char* relative_path) {
    namespace fs = std::filesystem;
    const fs::path direct(relative_path);
    if (fs::exists(direct)) {
        return direct.string();
    }

    const fs::path app_dir(GetApplicationDirectory());
    const fs::path near_exe = app_dir / direct;
    if (fs::exists(near_exe)) {
        return near_exe.string();
    }

    return direct.string();
}

uint32_t ComputeByteChecksum(const std::vector<uint8_t>& bytes) {
    uint32_t hash = 2166136261u;
    for (uint8_t byte : bytes) {
        hash ^= static_cast<uint32_t>(byte);
        hash *= 16777619u;
    }
    return hash;
}

bool ReadFileBytes(const std::string& path, std::vector<uint8_t>* out_bytes) {
    if (out_bytes == nullptr) {
        return false;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    file.seekg(0, std::ios::end);
    const std::streamsize size = file.tellg();
    if (size < 0) {
        return false;
    }
    file.seekg(0, std::ios::beg);
    out_bytes->resize(static_cast<size_t>(size));
    if (size == 0) {
        return true;
    }
    file.read(reinterpret_cast<char*>(out_bytes->data()), size);
    return file.good() || file.eof();
}

bool WriteFileBytes(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    if (!bytes.empty()) {
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    return file.good();
}

bool IsPngMapFilePath(const std::string& path) {
    namespace fs = std::filesystem;
    const fs::path file_path(path);
    const std::string extension = file_path.extension().string();
    return fs::exists(file_path) && fs::is_regular_file(file_path) && (extension == ".png" || extension == ".PNG");
}

bool IsLayeredMapDirectoryPath(const std::string& path) {
    namespace fs = std::filesystem;
    const fs::path map_dir(path);
    if (!fs::exists(map_dir) || !fs::is_directory(map_dir)) {
        return false;
    }
    const std::string map_name = map_dir.filename().string();
    return fs::exists(map_dir / "exports" / (map_name + "-terrain.png")) &&
           fs::exists(map_dir / "exports" / (map_name + "-objects.png"));
}

std::string BuildLayeredMapTerrainPngPath(const std::string& map_dir_path) {
    namespace fs = std::filesystem;
    const fs::path map_dir(map_dir_path);
    const std::string map_name = map_dir.filename().string();
    return (map_dir / "exports" / (map_name + "-terrain.png")).string();
}

std::string BuildLayeredMapObjectsPngPath(const std::string& map_dir_path) {
    namespace fs = std::filesystem;
    const fs::path map_dir(map_dir_path);
    const std::string map_name = map_dir.filename().string();
    return (map_dir / "exports" / (map_name + "-objects.png")).string();
}

void AppendU32(std::vector<uint8_t>* out, uint32_t value) {
    if (out == nullptr) {
        return;
    }
    out->push_back(static_cast<uint8_t>(value & 0xffu));
    out->push_back(static_cast<uint8_t>((value >> 8u) & 0xffu));
    out->push_back(static_cast<uint8_t>((value >> 16u) & 0xffu));
    out->push_back(static_cast<uint8_t>((value >> 24u) & 0xffu));
}

bool ReadU32(const std::vector<uint8_t>& bytes, size_t* offset, uint32_t* out_value) {
    if (offset == nullptr || out_value == nullptr || *offset + 4 > bytes.size()) {
        return false;
    }
    const size_t i = *offset;
    *out_value = static_cast<uint32_t>(bytes[i]) | (static_cast<uint32_t>(bytes[i + 1]) << 8u) |
                 (static_cast<uint32_t>(bytes[i + 2]) << 16u) | (static_cast<uint32_t>(bytes[i + 3]) << 24u);
    *offset += 4;
    return true;
}

void AppendString(std::vector<uint8_t>* out, const std::string& value) {
    AppendU32(out, static_cast<uint32_t>(value.size()));
    if (out != nullptr) {
        out->insert(out->end(), value.begin(), value.end());
    }
}

bool ReadString(const std::vector<uint8_t>& bytes, size_t* offset, std::string* out_value) {
    uint32_t size = 0;
    if (offset == nullptr || out_value == nullptr || !ReadU32(bytes, offset, &size) || *offset + size > bytes.size()) {
        return false;
    }
    out_value->assign(reinterpret_cast<const char*>(bytes.data() + *offset), size);
    *offset += size;
    return true;
}

void AppendBlob(std::vector<uint8_t>* out, const std::vector<uint8_t>& bytes) {
    AppendU32(out, static_cast<uint32_t>(bytes.size()));
    if (out != nullptr) {
        out->insert(out->end(), bytes.begin(), bytes.end());
    }
}

bool ReadBlob(const std::vector<uint8_t>& bytes, size_t* offset, std::vector<uint8_t>* out_blob) {
    uint32_t size = 0;
    if (offset == nullptr || out_blob == nullptr || !ReadU32(bytes, offset, &size) || *offset + size > bytes.size()) {
        return false;
    }
    out_blob->assign(bytes.begin() + static_cast<std::ptrdiff_t>(*offset),
                     bytes.begin() + static_cast<std::ptrdiff_t>(*offset + size));
    *offset += size;
    return true;
}

bool BuildMapTransferPayload(const std::string& source_path, std::string* out_transfer_filename, std::vector<uint8_t>* out_bytes) {
    if (out_transfer_filename == nullptr || out_bytes == nullptr) {
        return false;
    }
    namespace fs = std::filesystem;
    out_bytes->clear();
    out_transfer_filename->clear();

    if (IsPngMapFilePath(source_path)) {
        *out_transfer_filename = fs::path(source_path).filename().string();
        return ReadFileBytes(source_path, out_bytes);
    }

    if (!IsLayeredMapDirectoryPath(source_path)) {
        return false;
    }

    const std::string terrain_path = BuildLayeredMapTerrainPngPath(source_path);
    const std::string objects_path = BuildLayeredMapObjectsPngPath(source_path);
    std::vector<uint8_t> terrain_bytes;
    std::vector<uint8_t> objects_bytes;
    if (!ReadFileBytes(terrain_path, &terrain_bytes) || !ReadFileBytes(objects_path, &objects_bytes)) {
        return false;
    }

    const std::string map_name = fs::path(source_path).filename().string();
    *out_transfer_filename = map_name + ".mapbundle";
    out_bytes->insert(out_bytes->end(), {'R', 'A', 'M', 'B'});
    out_bytes->push_back(static_cast<uint8_t>(1));  // bundle version
    out_bytes->push_back(static_cast<uint8_t>(1));  // layered bundle
    AppendString(out_bytes, map_name);
    AppendBlob(out_bytes, terrain_bytes);
    AppendBlob(out_bytes, objects_bytes);
    return true;
}

bool ReconstructTransferredMap(const std::vector<uint8_t>& bytes, const std::string& transfer_filename, int transfer_id,
                               std::string* out_resolved_path) {
    if (out_resolved_path == nullptr) {
        return false;
    }
    namespace fs = std::filesystem;
    *out_resolved_path = {};

    const bool is_bundle = bytes.size() >= 6 && bytes[0] == 'R' && bytes[1] == 'A' && bytes[2] == 'M' && bytes[3] == 'B';
    if (!is_bundle) {
        const fs::path temp_path =
            fs::temp_directory_path() /
            TextFormat("rune-arena-map-transfer-%d-%s", transfer_id, transfer_filename.c_str());
        if (!WriteFileBytes(temp_path.string(), bytes)) {
            return false;
        }
        *out_resolved_path = temp_path.string();
        return true;
    }

    size_t offset = 4;
    if (offset + 2 > bytes.size()) {
        return false;
    }
    const uint8_t version = bytes[offset++];
    const uint8_t format = bytes[offset++];
    if (version != 1 || format != 1) {
        return false;
    }

    std::string map_name;
    std::vector<uint8_t> terrain_bytes;
    std::vector<uint8_t> objects_bytes;
    if (!ReadString(bytes, &offset, &map_name) || !ReadBlob(bytes, &offset, &terrain_bytes) ||
        !ReadBlob(bytes, &offset, &objects_bytes) || offset != bytes.size()) {
        return false;
    }

    const fs::path map_dir =
        fs::temp_directory_path() / TextFormat("rune-arena-map-transfer-%d", transfer_id) / map_name;
    const fs::path exports_dir = map_dir / "exports";
    std::error_code ec;
    fs::create_directories(exports_dir, ec);
    if (ec) {
        return false;
    }

    const fs::path terrain_path = exports_dir / (map_name + "-terrain.png");
    const fs::path objects_path = exports_dir / (map_name + "-objects.png");
    if (!WriteFileBytes(terrain_path.string(), terrain_bytes) || !WriteFileBytes(objects_path.string(), objects_bytes)) {
        return false;
    }
    *out_resolved_path = map_dir.string();
    return true;
}

bool IsColumnPrototypeId(const std::string& prototype_id) {
    return prototype_id.rfind("column_", 0) == 0;
}

bool BlocksRunePlacementAtCell(const MapObjectInstance& object, const GridCoord& cell, const ObjectPrototype* proto) {
    if (!object.alive || proto == nullptr || proto->walkable) {
        return false;
    }

    if (!proto->blocked_tiles.empty()) {
        return std::any_of(proto->blocked_tiles.begin(), proto->blocked_tiles.end(),
                           [&](const GridCoord& blocked_offset) {
                               return cell.x == object.cell.x + blocked_offset.x &&
                                      cell.y == object.cell.y + blocked_offset.y;
                           });
    }

    if (object.prototype_id == "tree_1") {
        return cell.y == object.cell.y && (cell.x == object.cell.x + 1 || cell.x == object.cell.x + 2);
    }

    return object.cell == cell;
}

bool IsFlatRenderedMapObjectPrototype(const ObjectPrototype* proto) {
    return proto != nullptr && proto->flat_render;
}

int GetSpriteSheetHeight(SpriteSheetType sheet_type) {
    switch (sheet_type) {
        case SpriteSheetType::Base32:
            return 32;
        case SpriteSheetType::Tall32x64:
            return 64;
        case SpriteSheetType::Large128x128:
            return 128;
    }
    return 32;
}

int GetSpriteSheetWidth(SpriteSheetType sheet_type) {
    switch (sheet_type) {
        case SpriteSheetType::Base32:
            return 32;
        case SpriteSheetType::Tall32x64:
            return 32;
        case SpriteSheetType::Large128x128:
            return 128;
    }
    return 32;
}

bool IsLoadedSound(const Sound& sound) {
    return sound.frameCount > 0 && sound.stream.buffer != nullptr;
}

bool IsLoadedMusic(const Music& music) {
    return music.stream.buffer != nullptr && music.ctxData != nullptr;
}

}  // namespace

GameApp::GameApp(bool force_windowed_launch)
    : force_windowed_launch_(force_windowed_launch),
      discovery_service_(std::make_unique<LanDiscovery>()),
      rng_(std::random_device{}()),
      visual_rng_(std::random_device{}()) {}

bool GameApp::Initialize() {
    const bool loaded_settings = config_manager_.Load(settings_);
    if (!loaded_settings) {
        settings_.fullscreen = true;
    }
    settings_.lobby_shrink_tiles_per_second =
        std::clamp(settings_.lobby_shrink_tiles_per_second, 0.0f, Constants::kMaxShrinkTilesPerSecond);
    settings_.lobby_min_arena_radius_tiles =
        std::clamp(settings_.lobby_min_arena_radius_tiles, 0.0f, Constants::kMaxArenaRadiusTiles);
    settings_.lobby_match_length_seconds =
        std::max(Constants::kLobbyTimeStepSeconds, settings_.lobby_match_length_seconds);
    settings_.lobby_best_of_target_kills = std::max(1, settings_.lobby_best_of_target_kills | 1);
    settings_.lobby_shrink_start_seconds = std::max(0.0f, settings_.lobby_shrink_start_seconds);
    settings_.lobby_mode_type =
        (settings_.lobby_mode_type == static_cast<int>(MatchModeType::BestOfKills))
            ? static_cast<int>(MatchModeType::BestOfKills)
            : static_cast<int>(MatchModeType::MostKillsTimed);
    lobby_mode_type_ = static_cast<MatchModeType>(settings_.lobby_mode_type);
    lobby_round_time_seconds_ = settings_.lobby_match_length_seconds;
    lobby_best_of_target_kills_ = settings_.lobby_best_of_target_kills;
    lobby_zone_enabled_ = settings_.lobby_zone_enabled;
    lobby_shrink_tiles_per_second_ = settings_.lobby_shrink_tiles_per_second;
    lobby_shrink_start_seconds_ = settings_.lobby_shrink_start_seconds;
    lobby_min_arena_radius_tiles_ = settings_.lobby_min_arena_radius_tiles;
    show_network_debug_panel_ = settings_.show_network_debug_panel;
    initial_fullscreen_setting_ = settings_.fullscreen;
    controls_manager_.Load(controls_bindings_);
    controls_manager_.Save(controls_bindings_);

    std::snprintf(player_name_buffer_, sizeof(player_name_buffer_), "%s", settings_.player_name.c_str());
    std::snprintf(join_ip_buffer_, sizeof(join_ip_buffer_), "%s", "127.0.0.1");

    InitWindow(settings_.window_width, settings_.window_height, "Rune Arena");
    SetExitKey(KEY_NULL);
    SetTargetFPS(144);
    const bool launch_fullscreen = !force_windowed_launch_;
    if (launch_fullscreen && !IsWindowFullscreen()) {
        ToggleFullscreen();
    }

    InitAudioDevice();
    audio_device_ready_ = IsAudioDeviceReady();

    // Basic dark UI look.
    GuiSetStyle(DEFAULT, BACKGROUND_COLOR, ColorToInt(Color{25, 28, 34, 255}));
    GuiSetStyle(DEFAULT, TEXT_COLOR_NORMAL, ColorToInt(Color{221, 228, 245, 255}));
    GuiSetStyle(DEFAULT, BASE_COLOR_NORMAL, ColorToInt(Color{42, 47, 57, 255}));
    GuiSetStyle(DEFAULT, BASE_COLOR_FOCUSED, ColorToInt(Color{54, 62, 75, 255}));

    resolved_default_map_path_ = ResolveRuntimePath(Constants::kDefaultMapPath);
    resolved_map_path_ = resolved_default_map_path_;
    resolved_host_selected_map_path_ = resolved_default_map_path_;
    resolved_objects_config_path_ = ResolveRuntimePath(Constants::kObjectsConfigPath);
    resolved_composite_effects_path_ = ResolveRuntimePath(Constants::kCompositeEffectsPath);
    resolved_sprite_metadata_path_ = ResolveRuntimePath(Constants::kSpriteMetadataPath);
    resolved_sprite_metadata_tall_path_ = ResolveRuntimePath(Constants::kSpriteMetadataTallPath);
    resolved_sprite_metadata_96x96_path_ = ResolveRuntimePath(Constants::kSpriteMetadata96x96Path);
    resolved_sprite_metadata_128x128_path_ = ResolveRuntimePath(Constants::kSpriteMetadata128x128Path);
    resolved_modular_player_main_path_ = ResolveRuntimePath(Constants::kModularPlayerMainMetadataPath);
    resolved_modular_player_shadow_path_ = ResolveRuntimePath(Constants::kModularPlayerShadowMetadataPath);
    const std::string resolved_modular_player_sword_path = ResolveRuntimePath(Constants::kModularPlayerSwordMetadataPath);
    const std::string resolved_modular_player_hammer_path = ResolveRuntimePath(Constants::kModularPlayerHammerMetadataPath);
    const std::string resolved_modular_player_ghook_path = ResolveRuntimePath(Constants::kModularPlayerGhookMetadataPath);
    const std::string resolved_modular_player_fx_path = ResolveRuntimePath(Constants::kModularPlayerFxMetadataPath);
    resolved_modular_tree_canopy_background_path_ =
        ResolveRuntimePath(Constants::kModularTreeCanopyBackgroundMetadataPath);
    resolved_modular_tree_trunk_path_ = ResolveRuntimePath(Constants::kModularTreeTrunkMetadataPath);
    resolved_modular_tree_canopy_foreground_path_ =
        ResolveRuntimePath(Constants::kModularTreeCanopyForegroundMetadataPath);
    resolved_modular_tree_shadow_path_ = ResolveRuntimePath(Constants::kModularTreeShadowMetadataPath);
    resolved_modular_tree_outline_mask_path_ = ResolveRuntimePath(Constants::kModularTreeOutlineMaskMetadataPath);
    resolved_modular_tree_asset_metadata_path_ = ResolveRuntimePath(Constants::kModularTreeAssetMetadataPath);
    resolved_spell_pattern_path_ = ResolveRuntimePath(Constants::kSpellPatternPath);
    resolved_castle_level_requirements_path_ = ResolveRuntimePath(Constants::kCastleLevelRequirementsPath);
    resolved_equipment_profiles_path_ = ResolveRuntimePath(Constants::kEquipmentProfilesPath);
    resolved_hit_shapes_path_ = ResolveRuntimePath(Constants::kHitShapesPath);
    resolved_loot_tables_path_ = ResolveRuntimePath(Constants::kLootTablesPath);
    resolved_menu_background_path_ = ResolveRuntimePath(Constants::kMenuBackgroundPath);
    resolved_occluder_reveal_shader_path_ = ResolveRuntimePath(Constants::kOccluderRevealShaderPath);
    resolved_water_gradient_shader_path_ = ResolveRuntimePath(Constants::kWaterGradientShaderPath);
    resolved_zone_post_process_shader_path_ = ResolveRuntimePath(Constants::kZonePostProcessShaderPath);
    resolved_zone_fill_overlay_shader_path_ = ResolveRuntimePath(Constants::kZoneFillOverlayShaderPath);
    resolved_zone_border_overlay_shader_path_ = ResolveRuntimePath(Constants::kZoneBorderOverlayShaderPath);
    resolved_map_bounds_fade_shader_path_ = ResolveRuntimePath(Constants::kMapBoundsFadeShaderPath);
    resolved_influence_zone_overlay_shader_path_ = ResolveRuntimePath(Constants::kInfluenceZoneOverlayShaderPath);
    resolved_damage_flash_shader_path_ = ResolveRuntimePath(Constants::kDamageFlashShaderPath);
    resolved_tree_composite_shader_path_ = ResolveRuntimePath(Constants::kTreeCompositeShaderPath);
    resolved_tree_composite_no_reveal_shader_path_ = ResolveRuntimePath(Constants::kTreeCompositeNoRevealShaderPath);
    resolved_tree_wind_shader_path_ = ResolveRuntimePath(Constants::kTreeWindShaderPath);

    objects_database_.LoadFromFile(resolved_objects_config_path_);
    composite_effects_loader_.LoadFromFile(resolved_composite_effects_path_);

    if (!map_loader_.Load(resolved_map_path_, &objects_database_, state_.map)) {
        // Fallback: small grass-only map.
        state_.map.width = 24;
        state_.map.height = 16;
        state_.map.cell_size = Constants::kRuneCellSize;
        state_.map.tiles.assign(static_cast<size_t>(state_.map.width * state_.map.height), TileType::Grass);
        state_.map.decorations.assign(static_cast<size_t>(state_.map.width * state_.map.height), "");
        state_.map.object_seeds.clear();
        state_.map.spawn_points = {{2, 2}, {state_.map.width - 3, state_.map.height - 3}};
    }
    RefreshLobbyMapCatalog();
    SetSelectedLobbyMapIndex(lobby_selected_map_index_);

    sprite_metadata_.LoadFromFile(resolved_sprite_metadata_path_);
    sprite_metadata_tall_.LoadFromFile(resolved_sprite_metadata_tall_path_);
    sprite_metadata_96x96_.LoadFromFile(resolved_sprite_metadata_96x96_path_);
    sprite_metadata_128x128_.LoadFromFile(resolved_sprite_metadata_128x128_path_);
    UnloadTextureVector(&sprite_sheet_128x128_team_variants_);
    std::vector<std::unordered_map<ColorKey, Color>> castle_palette_maps;
    if (BuildPaletteReplacementMaps(ResolveRuntimePath(Constants::kPlayerColorsMapPath), Constants::kTeamCount,
                                    Constants::kTeamBlue, &castle_palette_maps)) {
        const std::string castle_texture_path = ResolveRuntimePath(Constants::kSpriteSheet128x128TexturePath);
        sprite_sheet_128x128_team_variants_.resize(static_cast<size_t>(Constants::kTeamCount));
        for (int team_index = 0; team_index < Constants::kTeamCount; ++team_index) {
            sprite_sheet_128x128_team_variants_[static_cast<size_t>(team_index)] =
                CreateRemappedTextureVariant(castle_texture_path, castle_palette_maps[static_cast<size_t>(team_index)]);
        }
    }
    ModularLayerLoadOptions modular_player_main_options;
    modular_player_main_options.generate_team_variants = true;
    modular_player_main_options.palette_map_path = ResolveRuntimePath(Constants::kPlayerColorsMapPath);
    modular_player_main_options.team_count = Constants::kTeamCount;
    modular_player_main_options.source_team_index = Constants::kTeamBlue;
    modular_player_asset_.LoadLayer("main", resolved_modular_player_main_path_, modular_player_main_options);
    modular_player_asset_.LoadLayer("shadow", resolved_modular_player_shadow_path_);
    modular_player_asset_.LoadLayer("sword", resolved_modular_player_sword_path);
    if (FileExists(resolved_modular_player_hammer_path.c_str())) {
        modular_player_asset_.LoadLayer("hammer", resolved_modular_player_hammer_path);
    }
    modular_player_asset_.LoadLayer("ghook", resolved_modular_player_ghook_path);
    if (FileExists(resolved_modular_player_fx_path.c_str())) {
        modular_player_asset_.LoadLayer("fx", resolved_modular_player_fx_path);
    }
    modular_tree_asset_.LoadLayer("canopy_background", resolved_modular_tree_canopy_background_path_);
    modular_tree_asset_.LoadLayer("trunk", resolved_modular_tree_trunk_path_);
    modular_tree_asset_.LoadLayer("canopy_foreground", resolved_modular_tree_canopy_foreground_path_);
    modular_tree_asset_.LoadLayer("shadow", resolved_modular_tree_shadow_path_);
    if (FileExists(resolved_modular_tree_outline_mask_path_.c_str())) {
        modular_tree_asset_.LoadLayer("outline_mask", resolved_modular_tree_outline_mask_path_);
    }
    LoadModularTreeAssetMetadata();
    castle_level_energy_requirements_ =
        LoadCastleLevelRequirementsOrDefault(resolved_castle_level_requirements_path_, castle_level_energy_requirements_);
    spell_patterns_.LoadFromFile(resolved_spell_pattern_path_);
    equipment_registry_.LoadFromFile(resolved_equipment_profiles_path_);
    hit_shape_library_.LoadFromFile(resolved_hit_shapes_path_);
    loot_table_library_.LoadFromFile(resolved_loot_tables_path_);
    RegisterSpellRuntimes();
    if (FileExists(resolved_menu_background_path_.c_str())) {
        menu_background_texture_ = LoadTexture(resolved_menu_background_path_.c_str());
        has_menu_background_texture_ = (menu_background_texture_.id != 0);
    }
    RebuildLobbyMapPreview();
    LoadRenderShaders();
    LoadAudioAssets();
    camera_.target = {0.0f, 0.0f};
    camera_.offset = {static_cast<float>(settings_.window_width) * 0.5f,
                      static_cast<float>(settings_.window_height) * 0.5f};
    camera_.rotation = 0.0f;
    camera_.zoom = Constants::kCameraZoom;

    app_screen_ = AppScreen::MainMenu;
    if (!discovery_service_->StartClientListener()) {
        main_menu_status_message_ = TextFormat("Discovery listener failed on UDP %d", Constants::kDiscoveryPort);
        main_menu_status_is_error_ = true;
    }
    return true;
}

void GameApp::Run() {
    double previous_time = GetTime();
    double accumulator = 0.0;

    while (!WindowShouldClose() && !request_app_exit_) {
        const double now = GetTime();
        double frame_dt = now - previous_time;
        previous_time = now;

        frame_dt = std::min(frame_dt, 0.25);
        accumulator += frame_dt;
        render_time_seconds_ += static_cast<float>(frame_dt);
        CaptureFrameInputEdges();

        network_manager_.Poll();
        discovery_service_->Update();
        UpdateAudioFrame();

        while (accumulator >= Constants::kFixedDt) {
            Update(static_cast<float>(Constants::kFixedDt));
            accumulator -= Constants::kFixedDt;
        }

        Render();
    }
}

void GameApp::CaptureFrameInputEdges() {
    pending_primary_pressed_ = pending_primary_pressed_ || IsBindingPressed(controls_bindings_.primary_action);
    const bool grappling_pressed = IsBindingPressed(controls_bindings_.grappling_hook_action);
    const bool grappling_released = IsBindingReleased(controls_bindings_.grappling_hook_action);
    const bool grappling_down = IsBindingDown(controls_bindings_.grappling_hook_action);
    if (grappling_pressed && app_screen_ == AppScreen::InMatch && !in_game_menu_open_ && !chat_input_active_) {
        if (const Player* local_player = FindPlayerById(state_.local_player_id);
            local_player != nullptr && CanPlayerStartGrapplingPreview(*local_player)) {
            grappling_preview_armed_ = true;
            grappling_preview_started_time_ = render_time_seconds_;
        }
    }
    if (grappling_preview_armed_ && !grappling_down) {
        if (grappling_released && app_screen_ == AppScreen::InMatch && !in_game_menu_open_ && !chat_input_active_) {
            if (const Player* local_player = FindPlayerById(state_.local_player_id);
                local_player != nullptr && CanPlayerStartGrapplingPreview(*local_player)) {
                pending_grappling_pressed_ = true;
            }
        }
        grappling_preview_armed_ = false;
    } else if (grappling_preview_armed_) {
        if (app_screen_ != AppScreen::InMatch || in_game_menu_open_ || chat_input_active_) {
            grappling_preview_armed_ = false;
        } else if (const Player* local_player = FindPlayerById(state_.local_player_id);
                   local_player == nullptr || !CanPlayerStartGrapplingPreview(*local_player)) {
            grappling_preview_armed_ = false;
        }
    }
    pending_escape_pressed_ = pending_escape_pressed_ || IsKeyPressed(KEY_ESCAPE);
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
        pending_enter_pressed_ = true;
        pending_enter_shift_down_ = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    }
    pending_backspace_pressed_ = pending_backspace_pressed_ || IsKeyPressed(KEY_BACKSPACE);
    for (int ch = GetCharPressed(); ch > 0; ch = GetCharPressed()) {
        if (ch >= 32 && ch <= 126) {
            pending_text_input_.push_back(static_cast<char>(ch));
        }
    }
    if (IsBindingPressed(controls_bindings_.select_rune_slot_1)) pending_select_rune_slot_ = 0;
    if (IsBindingPressed(controls_bindings_.select_rune_slot_2)) pending_select_rune_slot_ = 1;
    if (IsBindingPressed(controls_bindings_.select_rune_slot_3)) pending_select_rune_slot_ = 2;
    if (IsBindingPressed(controls_bindings_.select_rune_slot_4)) pending_select_rune_slot_ = 3;
    if (IsBindingPressed(controls_bindings_.activate_item_slot_1)) pending_activate_item_slot_ = 0;
    if (IsBindingPressed(controls_bindings_.activate_item_slot_2)) pending_activate_item_slot_ = 1;
    if (IsBindingPressed(controls_bindings_.activate_item_slot_3)) pending_activate_item_slot_ = 2;
    if (IsBindingPressed(controls_bindings_.activate_item_slot_4)) pending_activate_item_slot_ = 3;
    pending_toggle_inventory_mode_ =
        pending_toggle_inventory_mode_ || IsBindingPressed(controls_bindings_.toggle_inventory_mode);
}

void GameApp::Shutdown() {
    std::string saved_name(player_name_buffer_);
    if (!saved_name.empty()) {
        settings_.player_name = saved_name;
    }
    settings_.window_width = GetScreenWidth();
    settings_.window_height = GetScreenHeight();
    if (force_windowed_launch_) {
        settings_.fullscreen = initial_fullscreen_setting_;
    } else {
        settings_.fullscreen = IsWindowFullscreen();
    }
    settings_.lobby_shrink_tiles_per_second = lobby_shrink_tiles_per_second_;
    settings_.lobby_mode_type = static_cast<int>(lobby_mode_type_);
    settings_.lobby_match_length_seconds = lobby_round_time_seconds_;
    settings_.lobby_best_of_target_kills = lobby_best_of_target_kills_;
    settings_.lobby_zone_enabled = lobby_zone_enabled_;
    settings_.lobby_shrink_start_seconds = lobby_shrink_start_seconds_;
    settings_.lobby_min_arena_radius_tiles = lobby_min_arena_radius_tiles_;
    settings_.show_network_debug_panel = show_network_debug_panel_;
    config_manager_.Save(settings_);

    discovery_service_->Stop();
    network_manager_.Stop();
    UnloadAudioAssets();
    UnloadRenderShaders();
    sprite_metadata_.Unload();
    sprite_metadata_tall_.Unload();
    sprite_metadata_96x96_.Unload();
    sprite_metadata_128x128_.Unload();
    UnloadTextureVector(&sprite_sheet_128x128_team_variants_);
    modular_player_asset_.Unload();
    modular_tree_asset_.Unload();
    if (has_menu_background_texture_) {
        UnloadTexture(menu_background_texture_);
        has_menu_background_texture_ = false;
    }
    ClearLobbyMapPreviewTexture();
    if (has_shadow_layer_target_) {
        UnloadRenderTexture(shadow_layer_target_);
        shadow_layer_target_ = {};
        has_shadow_layer_target_ = false;
    }
    if (has_world_layer_target_) {
        UnloadRenderTexture(world_layer_target_);
        world_layer_target_ = {};
        has_world_layer_target_ = false;
    }
    if (has_fire_spirit_trail_target_) {
        UnloadRenderTexture(fire_spirit_trail_target_);
        fire_spirit_trail_target_ = {};
        has_fire_spirit_trail_target_ = false;
    }
    if (has_fire_spirit_trail_lowres_target_) {
        UnloadRenderTexture(fire_spirit_trail_lowres_target_);
        fire_spirit_trail_lowres_target_ = {};
        has_fire_spirit_trail_lowres_target_ = false;
    }
    if (has_influence_zone_distance_red_from_texture_) {
        UnloadTexture(influence_zone_distance_red_from_texture_);
        influence_zone_distance_red_from_texture_ = {};
        has_influence_zone_distance_red_from_texture_ = false;
    }
    if (has_influence_zone_distance_blue_from_texture_) {
        UnloadTexture(influence_zone_distance_blue_from_texture_);
        influence_zone_distance_blue_from_texture_ = {};
        has_influence_zone_distance_blue_from_texture_ = false;
    }
    if (has_influence_zone_distance_red_to_texture_) {
        UnloadTexture(influence_zone_distance_red_to_texture_);
        influence_zone_distance_red_to_texture_ = {};
        has_influence_zone_distance_red_to_texture_ = false;
    }
    if (has_influence_zone_distance_blue_to_texture_) {
        UnloadTexture(influence_zone_distance_blue_to_texture_);
        influence_zone_distance_blue_to_texture_ = {};
        has_influence_zone_distance_blue_to_texture_ = false;
    }
    if (audio_device_ready_) {
        CloseAudioDevice();
        audio_device_ready_ = false;
    }
    CloseWindow();
}

void GameApp::LoadRenderShaders() {
    UnloadRenderShaders();
    if (FileExists(resolved_occluder_reveal_shader_path_.c_str())) {
        occluder_reveal_shader_ = LoadShader(nullptr, resolved_occluder_reveal_shader_path_.c_str());
        has_occluder_reveal_shader_ = (occluder_reveal_shader_.id != 0);
        if (has_occluder_reveal_shader_) {
            occluder_reveal_count_loc_ = GetShaderLocation(occluder_reveal_shader_, "uRevealCount");
            occluder_reveal_data_loc_ = GetShaderLocation(occluder_reveal_shader_, "uRevealData");
            occluder_reveal_screen_height_loc_ = GetShaderLocation(occluder_reveal_shader_, "uScreenHeight");
            occluder_reveal_inside_alpha_loc_ = GetShaderLocation(occluder_reveal_shader_, "uInsideAlpha");
            occluder_reveal_source_rect_loc_ = GetShaderLocation(occluder_reveal_shader_, "uSourceRectPx");
        }
    }

    if (FileExists(resolved_water_gradient_shader_path_.c_str())) {
        water_gradient_shader_ = LoadShader(nullptr, resolved_water_gradient_shader_path_.c_str());
        has_water_gradient_shader_ = (water_gradient_shader_.id != 0);
        if (has_water_gradient_shader_) {
            water_gradient_screen_height_loc_ = GetShaderLocation(water_gradient_shader_, "uScreenHeight");
            water_gradient_start_loc_ = GetShaderLocation(water_gradient_shader_, "uGradientStart");
            water_gradient_end_loc_ = GetShaderLocation(water_gradient_shader_, "uGradientEnd");
        }
    }

    if (FileExists(resolved_zone_post_process_shader_path_.c_str())) {
        zone_post_process_shader_ = LoadShader(nullptr, resolved_zone_post_process_shader_path_.c_str());
        has_zone_post_process_shader_ = (zone_post_process_shader_.id != 0);
        if (has_zone_post_process_shader_) {
            zone_post_process_screen_height_loc_ = GetShaderLocation(zone_post_process_shader_, "uScreenHeight");
            zone_post_process_camera_target_loc_ = GetShaderLocation(zone_post_process_shader_, "uCameraTarget");
            zone_post_process_camera_offset_loc_ = GetShaderLocation(zone_post_process_shader_, "uCameraOffset");
            zone_post_process_camera_zoom_loc_ = GetShaderLocation(zone_post_process_shader_, "uCameraZoom");
            zone_post_process_zone_center_loc_ = GetShaderLocation(zone_post_process_shader_, "uZoneCenter");
            zone_post_process_zone_radius_loc_ = GetShaderLocation(zone_post_process_shader_, "uZoneRadius");
        }
    }

    if (FileExists(resolved_zone_fill_overlay_shader_path_.c_str())) {
        zone_fill_overlay_shader_ = LoadShader(nullptr, resolved_zone_fill_overlay_shader_path_.c_str());
        has_zone_fill_overlay_shader_ = (zone_fill_overlay_shader_.id != 0);
        if (has_zone_fill_overlay_shader_) {
            zone_fill_overlay_screen_height_loc_ = GetShaderLocation(zone_fill_overlay_shader_, "uScreenHeight");
            zone_fill_overlay_camera_target_loc_ = GetShaderLocation(zone_fill_overlay_shader_, "uCameraTarget");
            zone_fill_overlay_camera_offset_loc_ = GetShaderLocation(zone_fill_overlay_shader_, "uCameraOffset");
            zone_fill_overlay_camera_zoom_loc_ = GetShaderLocation(zone_fill_overlay_shader_, "uCameraZoom");
            zone_fill_overlay_zone_center_loc_ = GetShaderLocation(zone_fill_overlay_shader_, "uZoneCenter");
            zone_fill_overlay_zone_radius_loc_ = GetShaderLocation(zone_fill_overlay_shader_, "uZoneRadius");
            zone_fill_overlay_zone_fill_rect_loc_ = GetShaderLocation(zone_fill_overlay_shader_, "uZoneFillRectPx");
        }
    }

    if (FileExists(resolved_zone_border_overlay_shader_path_.c_str())) {
        zone_border_overlay_shader_ = LoadShader(nullptr, resolved_zone_border_overlay_shader_path_.c_str());
        has_zone_border_overlay_shader_ = (zone_border_overlay_shader_.id != 0);
        if (has_zone_border_overlay_shader_) {
            zone_border_overlay_screen_height_loc_ = GetShaderLocation(zone_border_overlay_shader_, "uScreenHeight");
            zone_border_overlay_camera_target_loc_ = GetShaderLocation(zone_border_overlay_shader_, "uCameraTarget");
            zone_border_overlay_camera_offset_loc_ = GetShaderLocation(zone_border_overlay_shader_, "uCameraOffset");
            zone_border_overlay_camera_zoom_loc_ = GetShaderLocation(zone_border_overlay_shader_, "uCameraZoom");
            zone_border_overlay_zone_center_loc_ = GetShaderLocation(zone_border_overlay_shader_, "uZoneCenter");
            zone_border_overlay_zone_radius_loc_ = GetShaderLocation(zone_border_overlay_shader_, "uZoneRadius");
        }
    }

    if (FileExists(resolved_map_bounds_fade_shader_path_.c_str())) {
        map_bounds_fade_shader_ = LoadShader(nullptr, resolved_map_bounds_fade_shader_path_.c_str());
        has_map_bounds_fade_shader_ = (map_bounds_fade_shader_.id != 0);
        if (has_map_bounds_fade_shader_) {
            map_bounds_fade_screen_height_loc_ = GetShaderLocation(map_bounds_fade_shader_, "uScreenHeight");
            map_bounds_fade_camera_target_loc_ = GetShaderLocation(map_bounds_fade_shader_, "uCameraTarget");
            map_bounds_fade_camera_offset_loc_ = GetShaderLocation(map_bounds_fade_shader_, "uCameraOffset");
            map_bounds_fade_camera_zoom_loc_ = GetShaderLocation(map_bounds_fade_shader_, "uCameraZoom");
            map_bounds_fade_fade_rect_min_loc_ = GetShaderLocation(map_bounds_fade_shader_, "uFadeRectMin");
            map_bounds_fade_fade_rect_max_loc_ = GetShaderLocation(map_bounds_fade_shader_, "uFadeRectMax");
        }
    }

    if (FileExists(resolved_influence_zone_overlay_shader_path_.c_str())) {
        influence_zone_overlay_shader_ = LoadShader(nullptr, resolved_influence_zone_overlay_shader_path_.c_str());
        has_influence_zone_overlay_shader_ = (influence_zone_overlay_shader_.id != 0);
        if (has_influence_zone_overlay_shader_) {
            influence_zone_overlay_screen_height_loc_ = GetShaderLocation(influence_zone_overlay_shader_, "uScreenHeight");
            influence_zone_overlay_camera_target_loc_ = GetShaderLocation(influence_zone_overlay_shader_, "uCameraTarget");
            influence_zone_overlay_camera_offset_loc_ = GetShaderLocation(influence_zone_overlay_shader_, "uCameraOffset");
            influence_zone_overlay_camera_zoom_loc_ = GetShaderLocation(influence_zone_overlay_shader_, "uCameraZoom");
            influence_zone_overlay_map_size_world_loc_ = GetShaderLocation(influence_zone_overlay_shader_, "uMapSizeWorld");
            influence_zone_overlay_signed_distance_range_loc_ =
                GetShaderLocation(influence_zone_overlay_shader_, "uSignedDistanceRange");
            influence_zone_overlay_tint_loc_ = GetShaderLocation(influence_zone_overlay_shader_, "uTint");
            influence_zone_overlay_pattern_phase_loc_ = GetShaderLocation(influence_zone_overlay_shader_, "uPatternPhase");
            influence_zone_overlay_pattern_frame_loc_ = GetShaderLocation(influence_zone_overlay_shader_, "uPatternFrame");
            influence_zone_overlay_to_distance_texture_loc_ =
                GetShaderLocation(influence_zone_overlay_shader_, "uToDistanceTexture");
            influence_zone_overlay_blend_t_loc_ = GetShaderLocation(influence_zone_overlay_shader_, "uBlendT");
        }
    }

    if (FileExists(resolved_damage_flash_shader_path_.c_str())) {
        damage_flash_shader_ = LoadShader(nullptr, resolved_damage_flash_shader_path_.c_str());
        has_damage_flash_shader_ = (damage_flash_shader_.id != 0);
        if (has_damage_flash_shader_) {
            damage_flash_amount_loc_ = GetShaderLocation(damage_flash_shader_, "uFlashAmount");
        }
    }

    if (FileExists(resolved_tree_composite_shader_path_.c_str())) {
        tree_composite_shader_ = LoadShader(nullptr, resolved_tree_composite_shader_path_.c_str());
        has_tree_composite_shader_ = (tree_composite_shader_.id != 0);
        if (has_tree_composite_shader_) {
            tree_composite_trunk_texture_loc_ = GetShaderLocation(tree_composite_shader_, "uTrunkTexture");
            tree_composite_canopy_foreground_texture_loc_ =
                GetShaderLocation(tree_composite_shader_, "uCanopyForegroundTexture");
            tree_composite_mask_texture_loc_ = GetShaderLocation(tree_composite_shader_, "uMaskTexture");
            tree_composite_canopy_background_rect_loc_ =
                GetShaderLocation(tree_composite_shader_, "uCanopyBackgroundRectPx");
            tree_composite_trunk_rect_loc_ = GetShaderLocation(tree_composite_shader_, "uTrunkRectPx");
            tree_composite_canopy_foreground_rect_loc_ =
                GetShaderLocation(tree_composite_shader_, "uCanopyForegroundRectPx");
            tree_composite_mask_rect_loc_ = GetShaderLocation(tree_composite_shader_, "uMaskRectPx");
            tree_composite_time_loc_ = GetShaderLocation(tree_composite_shader_, "uTime");
            tree_composite_sway_strength_loc_ = GetShaderLocation(tree_composite_shader_, "uSwayStrengthPixels");
            tree_composite_sway_speed_loc_ = GetShaderLocation(tree_composite_shader_, "uSwaySpeed");
            tree_composite_phase_offset_loc_ = GetShaderLocation(tree_composite_shader_, "uPhaseOffset");
            tree_composite_gradient_start_loc_ = GetShaderLocation(tree_composite_shader_, "uGradientStart");
            tree_composite_screen_height_loc_ = GetShaderLocation(tree_composite_shader_, "uScreenHeight");
            tree_composite_inside_alpha_loc_ = GetShaderLocation(tree_composite_shader_, "uInsideAlpha");
            tree_composite_reveal_count_loc_ = GetShaderLocation(tree_composite_shader_, "uRevealCount");
            tree_composite_reveal_data_loc_ = GetShaderLocation(tree_composite_shader_, "uRevealData");
        }
    }

    if (FileExists(resolved_tree_composite_no_reveal_shader_path_.c_str())) {
        tree_composite_no_reveal_shader_ = LoadShader(nullptr, resolved_tree_composite_no_reveal_shader_path_.c_str());
        has_tree_composite_no_reveal_shader_ = (tree_composite_no_reveal_shader_.id != 0);
        if (has_tree_composite_no_reveal_shader_) {
            tree_composite_no_reveal_trunk_texture_loc_ =
                GetShaderLocation(tree_composite_no_reveal_shader_, "uTrunkTexture");
            tree_composite_no_reveal_canopy_foreground_texture_loc_ =
                GetShaderLocation(tree_composite_no_reveal_shader_, "uCanopyForegroundTexture");
            tree_composite_no_reveal_mask_texture_loc_ =
                GetShaderLocation(tree_composite_no_reveal_shader_, "uMaskTexture");
            tree_composite_no_reveal_canopy_background_rect_loc_ =
                GetShaderLocation(tree_composite_no_reveal_shader_, "uCanopyBackgroundRectPx");
            tree_composite_no_reveal_trunk_rect_loc_ =
                GetShaderLocation(tree_composite_no_reveal_shader_, "uTrunkRectPx");
            tree_composite_no_reveal_canopy_foreground_rect_loc_ =
                GetShaderLocation(tree_composite_no_reveal_shader_, "uCanopyForegroundRectPx");
            tree_composite_no_reveal_mask_rect_loc_ =
                GetShaderLocation(tree_composite_no_reveal_shader_, "uMaskRectPx");
            tree_composite_no_reveal_time_loc_ = GetShaderLocation(tree_composite_no_reveal_shader_, "uTime");
            tree_composite_no_reveal_sway_strength_loc_ =
                GetShaderLocation(tree_composite_no_reveal_shader_, "uSwayStrengthPixels");
            tree_composite_no_reveal_sway_speed_loc_ =
                GetShaderLocation(tree_composite_no_reveal_shader_, "uSwaySpeed");
            tree_composite_no_reveal_phase_offset_loc_ =
                GetShaderLocation(tree_composite_no_reveal_shader_, "uPhaseOffset");
            tree_composite_no_reveal_gradient_start_loc_ =
                GetShaderLocation(tree_composite_no_reveal_shader_, "uGradientStart");
        }
    }

    if (FileExists(resolved_tree_wind_shader_path_.c_str())) {
        tree_wind_shader_ = LoadShader(nullptr, resolved_tree_wind_shader_path_.c_str());
        has_tree_wind_shader_ = (tree_wind_shader_.id != 0);
        if (has_tree_wind_shader_) {
            tree_wind_frame_rect_loc_ = GetShaderLocation(tree_wind_shader_, "uFrameRectPx");
            tree_wind_time_loc_ = GetShaderLocation(tree_wind_shader_, "uTime");
            tree_wind_sway_strength_loc_ = GetShaderLocation(tree_wind_shader_, "uSwayStrengthPixels");
            tree_wind_sway_speed_loc_ = GetShaderLocation(tree_wind_shader_, "uSwaySpeed");
            tree_wind_screen_height_loc_ = GetShaderLocation(tree_wind_shader_, "uScreenHeight");
            tree_wind_camera_target_loc_ = GetShaderLocation(tree_wind_shader_, "uCameraTarget");
            tree_wind_camera_offset_loc_ = GetShaderLocation(tree_wind_shader_, "uCameraOffset");
            tree_wind_camera_zoom_loc_ = GetShaderLocation(tree_wind_shader_, "uCameraZoom");
            tree_wind_gradient_start_loc_ = GetShaderLocation(tree_wind_shader_, "uGradientStart");
        }
    }

}

void GameApp::UnloadRenderShaders() {
    if (has_occluder_reveal_shader_) {
        UnloadShader(occluder_reveal_shader_);
    }
    occluder_reveal_shader_ = {};
    has_occluder_reveal_shader_ = false;
    if (has_water_gradient_shader_) {
        UnloadShader(water_gradient_shader_);
    }
    water_gradient_shader_ = {};
    has_water_gradient_shader_ = false;
    if (has_zone_post_process_shader_) {
        UnloadShader(zone_post_process_shader_);
    }
    zone_post_process_shader_ = {};
    has_zone_post_process_shader_ = false;
    if (has_zone_fill_overlay_shader_) {
        UnloadShader(zone_fill_overlay_shader_);
    }
    zone_fill_overlay_shader_ = {};
    has_zone_fill_overlay_shader_ = false;
    if (has_zone_border_overlay_shader_) {
        UnloadShader(zone_border_overlay_shader_);
    }
    zone_border_overlay_shader_ = {};
    has_zone_border_overlay_shader_ = false;
    if (has_map_bounds_fade_shader_) {
        UnloadShader(map_bounds_fade_shader_);
    }
    map_bounds_fade_shader_ = {};
    has_map_bounds_fade_shader_ = false;
    if (has_influence_zone_overlay_shader_) {
        UnloadShader(influence_zone_overlay_shader_);
    }
    influence_zone_overlay_shader_ = {};
    has_influence_zone_overlay_shader_ = false;
    if (has_damage_flash_shader_) {
        UnloadShader(damage_flash_shader_);
    }
    damage_flash_shader_ = {};
    has_damage_flash_shader_ = false;
    if (has_tree_composite_shader_) {
        UnloadShader(tree_composite_shader_);
    }
    tree_composite_shader_ = {};
    has_tree_composite_shader_ = false;
    if (has_tree_composite_no_reveal_shader_) {
        UnloadShader(tree_composite_no_reveal_shader_);
    }
    tree_composite_no_reveal_shader_ = {};
    has_tree_composite_no_reveal_shader_ = false;
    if (has_tree_wind_shader_) {
        UnloadShader(tree_wind_shader_);
    }
    tree_wind_shader_ = {};
    has_tree_wind_shader_ = false;
    occluder_reveal_count_loc_ = -1;
    occluder_reveal_data_loc_ = -1;
    occluder_reveal_screen_height_loc_ = -1;
    occluder_reveal_inside_alpha_loc_ = -1;
    occluder_reveal_source_rect_loc_ = -1;
    water_gradient_screen_height_loc_ = -1;
    water_gradient_start_loc_ = -1;
    water_gradient_end_loc_ = -1;
    zone_post_process_screen_height_loc_ = -1;
    zone_post_process_camera_target_loc_ = -1;
    zone_post_process_camera_offset_loc_ = -1;
    zone_post_process_camera_zoom_loc_ = -1;
    zone_post_process_zone_center_loc_ = -1;
    zone_post_process_zone_radius_loc_ = -1;
    zone_fill_overlay_screen_height_loc_ = -1;
    zone_fill_overlay_camera_target_loc_ = -1;
    zone_fill_overlay_camera_offset_loc_ = -1;
    zone_fill_overlay_camera_zoom_loc_ = -1;
    zone_fill_overlay_zone_center_loc_ = -1;
    zone_fill_overlay_zone_radius_loc_ = -1;
    zone_fill_overlay_zone_fill_rect_loc_ = -1;
    zone_border_overlay_screen_height_loc_ = -1;
    zone_border_overlay_camera_target_loc_ = -1;
    zone_border_overlay_camera_offset_loc_ = -1;
    zone_border_overlay_camera_zoom_loc_ = -1;
    zone_border_overlay_zone_center_loc_ = -1;
    zone_border_overlay_zone_radius_loc_ = -1;
    map_bounds_fade_screen_height_loc_ = -1;
    map_bounds_fade_camera_target_loc_ = -1;
    map_bounds_fade_camera_offset_loc_ = -1;
    map_bounds_fade_camera_zoom_loc_ = -1;
    map_bounds_fade_fade_rect_min_loc_ = -1;
    map_bounds_fade_fade_rect_max_loc_ = -1;
    influence_zone_overlay_screen_height_loc_ = -1;
    influence_zone_overlay_camera_target_loc_ = -1;
    influence_zone_overlay_camera_offset_loc_ = -1;
    influence_zone_overlay_camera_zoom_loc_ = -1;
    influence_zone_overlay_map_size_world_loc_ = -1;
    influence_zone_overlay_signed_distance_range_loc_ = -1;
    influence_zone_overlay_tint_loc_ = -1;
    influence_zone_overlay_pattern_phase_loc_ = -1;
    influence_zone_overlay_pattern_frame_loc_ = -1;
    influence_zone_overlay_to_distance_texture_loc_ = -1;
    influence_zone_overlay_blend_t_loc_ = -1;
    damage_flash_amount_loc_ = -1;
    tree_composite_trunk_texture_loc_ = -1;
    tree_composite_canopy_foreground_texture_loc_ = -1;
    tree_composite_mask_texture_loc_ = -1;
    tree_composite_canopy_background_rect_loc_ = -1;
    tree_composite_trunk_rect_loc_ = -1;
    tree_composite_canopy_foreground_rect_loc_ = -1;
    tree_composite_mask_rect_loc_ = -1;
    tree_composite_time_loc_ = -1;
    tree_composite_sway_strength_loc_ = -1;
    tree_composite_sway_speed_loc_ = -1;
    tree_composite_phase_offset_loc_ = -1;
    tree_composite_gradient_start_loc_ = -1;
    tree_composite_screen_height_loc_ = -1;
    tree_composite_inside_alpha_loc_ = -1;
    tree_composite_reveal_count_loc_ = -1;
    tree_composite_reveal_data_loc_ = -1;
    tree_composite_no_reveal_trunk_texture_loc_ = -1;
    tree_composite_no_reveal_canopy_foreground_texture_loc_ = -1;
    tree_composite_no_reveal_mask_texture_loc_ = -1;
    tree_composite_no_reveal_canopy_background_rect_loc_ = -1;
    tree_composite_no_reveal_trunk_rect_loc_ = -1;
    tree_composite_no_reveal_canopy_foreground_rect_loc_ = -1;
    tree_composite_no_reveal_mask_rect_loc_ = -1;
    tree_composite_no_reveal_time_loc_ = -1;
    tree_composite_no_reveal_sway_strength_loc_ = -1;
    tree_composite_no_reveal_sway_speed_loc_ = -1;
    tree_composite_no_reveal_phase_offset_loc_ = -1;
    tree_composite_no_reveal_gradient_start_loc_ = -1;
    tree_wind_frame_rect_loc_ = -1;
    tree_wind_time_loc_ = -1;
    tree_wind_sway_strength_loc_ = -1;
    tree_wind_sway_speed_loc_ = -1;
    tree_wind_screen_height_loc_ = -1;
    tree_wind_camera_target_loc_ = -1;
    tree_wind_camera_offset_loc_ = -1;
    tree_wind_camera_zoom_loc_ = -1;
    tree_wind_gradient_start_loc_ = -1;
}

bool GameApp::DrawMaskedOccluder(const Rectangle& world_dst, const Texture2D& texture, const Rectangle& src, float sort_y) {
    if (!has_occluder_reveal_shader_) {
        return false;
    }

    std::array<OccluderRevealCircle, Constants::kOccluderRevealMaxCircles> circles = {};
    int circle_count = 0;
    const auto try_add_circle = [&](Vector2 world_center, float radius_world, float target_sort_y) {
        if (circle_count >= Constants::kOccluderRevealMaxCircles || target_sort_y >= sort_y - 0.01f ||
            !ContainsWorldPointExpanded(world_dst, world_center, radius_world)) {
            return;
        }

        const Vector2 screen_center = GetWorldToScreen2D(world_center, camera_);
        const float radius_px = radius_world * camera_.zoom;
        const float falloff_px = Constants::kOccluderRevealFalloffWorld * camera_.zoom;
        circles[static_cast<size_t>(circle_count++)] = {screen_center, radius_px, radius_px + falloff_px};
    };

    for (const Player& player : state_.players) {
        if (!player.alive) {
            continue;
        }
        const Vector2 world_center = GetRenderPlayerPosition(player.id);
        try_add_circle(world_center, Constants::kOccluderRevealPlayerRadiusWorld, world_center.y + 16.0f);
    }

    for (const MapObjectInstance& object : state_.map_objects) {
        if (!object.alive || object.type != ObjectType::Consumable) {
            continue;
        }
        const Vector2 world_center = CellToWorldCenter(object.cell);
        try_add_circle(world_center, Constants::kOccluderRevealItemRadiusWorld, world_center.y);
    }

    for (const Rune& rune : state_.runes) {
        if (!rune.active && rune.activation_remaining_seconds <= 0.0f) {
            continue;
        }
        const Vector2 world_center = CellToWorldCenter(rune.cell);
        try_add_circle(world_center, Constants::kOccluderRevealItemRadiusWorld, world_center.y);
    }

    if (circle_count <= 0) {
        return false;
    }

    std::array<float, Constants::kOccluderRevealMaxCircles * 4> reveal_data = {};
    for (int i = 0; i < circle_count; ++i) {
        const size_t base = static_cast<size_t>(i) * 4;
        reveal_data[base + 0] = circles[static_cast<size_t>(i)].screen_center.x;
        reveal_data[base + 1] = circles[static_cast<size_t>(i)].screen_center.y;
        reveal_data[base + 2] = circles[static_cast<size_t>(i)].inner_radius_px;
        reveal_data[base + 3] = circles[static_cast<size_t>(i)].outer_radius_px;
    }

    const float screen_height = static_cast<float>(GetScreenHeight());
    const float inside_alpha = Constants::kOccluderRevealInsideAlpha;
    const float source_rect_px[4] = {src.x, src.y, src.width, src.height};
    SetShaderValue(occluder_reveal_shader_, occluder_reveal_count_loc_, &circle_count, SHADER_UNIFORM_INT);
    SetShaderValueV(occluder_reveal_shader_, occluder_reveal_data_loc_, reveal_data.data(), SHADER_UNIFORM_VEC4, circle_count);
    SetShaderValue(occluder_reveal_shader_, occluder_reveal_screen_height_loc_, &screen_height, SHADER_UNIFORM_FLOAT);
    SetShaderValue(occluder_reveal_shader_, occluder_reveal_inside_alpha_loc_, &inside_alpha, SHADER_UNIFORM_FLOAT);
    SetShaderValueV(occluder_reveal_shader_, occluder_reveal_source_rect_loc_, source_rect_px, SHADER_UNIFORM_VEC4, 1);

    BeginShaderMode(occluder_reveal_shader_);
    DrawTexturePro(texture, src, world_dst, {0, 0}, 0.0f, WHITE);
    EndShaderMode();
    return true;
}

bool GameApp::LoadModularTreeAssetMetadata() {
    has_modular_tree_anchor_pixels_ =
        LoadAnchorPixelsFromMetadataFile(resolved_modular_tree_asset_metadata_path_, &modular_tree_anchor_pixels_);
    if (!has_modular_tree_anchor_pixels_) {
        modular_tree_anchor_pixels_ = {0.0f, 0.0f};
    }
    return has_modular_tree_anchor_pixels_;
}

Rectangle GameApp::GetMapObjectCollisionAabb(const MapObjectInstance& object, const ObjectPrototype* proto) const {
    const int cell_size = state_.map.cell_size;
    Rectangle aabb = {static_cast<float>(object.cell.x * cell_size), static_cast<float>(object.cell.y * cell_size),
                      static_cast<float>(cell_size), static_cast<float>(cell_size)};
    if (proto != nullptr && proto->has_collision_box_override) {
        const Rectangle sprite_rect =
            GetMapObjectSpriteRect(object, proto, static_cast<float>(GetSpriteSheetWidth(proto->sprite_sheet)),
                                   static_cast<float>(GetSpriteSheetHeight(proto->sprite_sheet)), 1.0f, false);
        const float sprite_x = sprite_rect.x;
        const float sprite_y = sprite_rect.y;
        return {sprite_x + static_cast<float>(proto->collision_box_x), sprite_y + static_cast<float>(proto->collision_box_y),
                static_cast<float>(proto->collision_box_w), static_cast<float>(proto->collision_box_h)};
    }

    if (proto == nullptr || !IsColumnPrototypeId(object.prototype_id)) {
        return aabb;
    }

    const float scale = 0.8f;
    const float inset = (1.0f - scale) * 0.5f * static_cast<float>(cell_size);
    aabb.x += inset;
    aabb.y += inset;
    aabb.width *= scale;
    aabb.height *= scale;
    return aabb;
}

Rectangle GameApp::GetMapObjectSpriteRect(const MapObjectInstance& object, const ObjectPrototype* proto, float sprite_width,
                                          float sprite_height, float visual_scale, bool snap) const {
    const float cell_size = static_cast<float>(state_.map.cell_size);
    const float base_w = std::max(sprite_width, cell_size);
    const float base_h = std::max(sprite_height, cell_size);
    const float draw_w = base_w * visual_scale;
    const float draw_h = base_h * visual_scale;

    Rectangle dst = {};
    if (proto != nullptr && IsModularTreePrototypeId(object.prototype_id) && has_modular_tree_anchor_pixels_) {
        dst = {static_cast<float>(object.cell.x * state_.map.cell_size) - modular_tree_anchor_pixels_.x,
               static_cast<float>(object.cell.y * state_.map.cell_size) - modular_tree_anchor_pixels_.y, draw_w, draw_h};
    } else if (proto != nullptr && proto->sprite_anchor == MapObjectSpriteAnchor::CellCenter) {
        const Vector2 center = CellToWorldCenter(object.cell);
        dst = {center.x - draw_w * 0.5f, center.y - draw_h * 0.5f, draw_w, draw_h};
    } else {
        dst = {static_cast<float>(object.cell.x * state_.map.cell_size),
               static_cast<float>(object.cell.y * state_.map.cell_size) - (base_h - cell_size), draw_w, draw_h};
        dst.x += (base_w - draw_w) * 0.5f;
        dst.y += (base_h - draw_h) * 0.5f;
    }
    if (proto != nullptr) {
        dst.x += static_cast<float>(proto->sprite_offset_x) * visual_scale;
        dst.y += static_cast<float>(proto->sprite_offset_y) * visual_scale;
    }
    return snap ? SnapRect(dst) : dst;
}

std::string GameApp::ResolveMapObjectAnimation(const MapObjectInstance& object, const ObjectPrototype& proto,
                                               const SpriteMetadataLoader& metadata, float* out_anim_time) const {
    float anim_time = render_time_seconds_;
    std::string animation = proto.idle_animation;
    if (object.state == MapObjectState::Spawning && !proto.born_animation.empty() && metadata.HasAnimation(proto.born_animation)) {
        animation = proto.born_animation;
        anim_time = object.state_time;
    } else if (!proto.charging_animation.empty() && metadata.HasAnimation(proto.charging_animation)) {
        const auto castle_it =
            std::find_if(state_.castles.begin(), state_.castles.end(),
                         [&](const CastleState& castle) { return castle.map_object_id == object.id; });
        if (castle_it != state_.castles.end() && IsCastleCharging(castle_it->id)) {
            animation = proto.charging_animation;
        }
    } else if (object.state == MapObjectState::Dying && !proto.death_animation.empty() &&
               metadata.HasAnimation(proto.death_animation)) {
        animation = proto.death_animation;
        anim_time = object.state_time;
    }
    if (out_anim_time != nullptr) {
        *out_anim_time = anim_time;
    }
    return animation;
}

float GameApp::GetMapObjectAnimationDurationSeconds(const ObjectPrototype& proto, const std::string& animation_name) const {
    if (animation_name.empty()) {
        return 0.0f;
    }

    const SpriteMetadataLoader* metadata = &sprite_metadata_;
    switch (proto.sprite_sheet) {
        case SpriteSheetType::Base32:
            metadata = &sprite_metadata_;
            break;
        case SpriteSheetType::Tall32x64:
            metadata = &sprite_metadata_tall_;
            break;
        case SpriteSheetType::Large128x128:
            metadata = &sprite_metadata_128x128_;
            break;
    }

    int frame_count = 0;
    float fps = 1.0f;
    if (metadata != nullptr && metadata->GetAnimationStats(animation_name, "default", frame_count, fps) && frame_count > 0) {
        return static_cast<float>(frame_count) / std::max(0.001f, fps);
    }
    return 0.0f;
}

Rectangle GameApp::GetModularTreeSpriteRect(const MapObjectInstance& object, const char* layer_name) const {
    const int frame_width = std::max(modular_tree_asset_.GetFrameWidth(layer_name), state_.map.cell_size);
    const int frame_height = std::max(modular_tree_asset_.GetFrameHeight(layer_name), state_.map.cell_size);
    const float world_x = static_cast<float>(object.cell.x * state_.map.cell_size);
    const float world_y = static_cast<float>(object.cell.y * state_.map.cell_size);
    if (has_modular_tree_anchor_pixels_) {
        return SnapRect({world_x - modular_tree_anchor_pixels_.x, world_y - modular_tree_anchor_pixels_.y,
                         static_cast<float>(frame_width),
                         static_cast<float>(frame_height)});
    }
    return SnapRect({static_cast<float>(object.cell.x * state_.map.cell_size),
                     world_y - (static_cast<float>(frame_height) - static_cast<float>(state_.map.cell_size)),
                     static_cast<float>(frame_width), static_cast<float>(frame_height)});
}

float GameApp::GetMapObjectWindPhaseOffset(const MapObjectInstance& object) const {
    return HashToUnitFloat(object.cell.x, object.cell.y) * 2.0f * PI;
}

bool GameApp::DrawWindAnimatedMapObject(const MapObjectInstance& object, const ObjectPrototype& proto, const Texture2D& texture,
                                        Rectangle src, Rectangle dst) {
    if (proto.wind_strength_pixels <= 0.0f || !has_tree_wind_shader_) {
        DrawTexturePro(texture, src, dst, {0, 0}, 0.0f, WHITE);
        return true;
    }

    const float frame_rect[4] = {src.x, src.y, src.width, src.height};
    const float screen_height = static_cast<float>(GetScreenHeight());
    const float camera_target[2] = {camera_.target.x, camera_.target.y};
    const float camera_offset[2] = {camera_.offset.x, camera_.offset.y};
    const float camera_zoom = camera_.zoom;
    const float time_seconds = render_time_seconds_;
    const float sway_strength = proto.wind_strength_pixels;
    const float sway_speed = Constants::kTreeWindSpeed * proto.wind_speed_multiplier;
    const float gradient_start = proto.wind_gradient_start;
    SetShaderValueV(tree_wind_shader_, tree_wind_frame_rect_loc_, frame_rect, SHADER_UNIFORM_VEC4, 1);
    SetShaderValue(tree_wind_shader_, tree_wind_screen_height_loc_, &screen_height, SHADER_UNIFORM_FLOAT);
    SetShaderValueV(tree_wind_shader_, tree_wind_camera_target_loc_, camera_target, SHADER_UNIFORM_VEC2, 1);
    SetShaderValueV(tree_wind_shader_, tree_wind_camera_offset_loc_, camera_offset, SHADER_UNIFORM_VEC2, 1);
    SetShaderValue(tree_wind_shader_, tree_wind_camera_zoom_loc_, &camera_zoom, SHADER_UNIFORM_FLOAT);
    SetShaderValue(tree_wind_shader_, tree_wind_time_loc_, &time_seconds, SHADER_UNIFORM_FLOAT);
    SetShaderValue(tree_wind_shader_, tree_wind_sway_strength_loc_, &sway_strength, SHADER_UNIFORM_FLOAT);
    SetShaderValue(tree_wind_shader_, tree_wind_sway_speed_loc_, &sway_speed, SHADER_UNIFORM_FLOAT);
    SetShaderValue(tree_wind_shader_, tree_wind_gradient_start_loc_, &gradient_start, SHADER_UNIFORM_FLOAT);

    BeginShaderMode(tree_wind_shader_);
    DrawTexturePro(texture, src, dst, {0, 0}, 0.0f, WHITE);
    EndShaderMode();
    return true;
}

bool GameApp::RenderModularTreeShadow(const MapObjectInstance& object, const ObjectPrototype& proto) {
    if (!IsModularTreePrototypeId(proto.id) || !modular_tree_asset_.HasLayer("shadow")) {
        return false;
    }

    const Texture2D* shadow_texture = modular_tree_asset_.GetLayerTexture("shadow");
    if (shadow_texture == nullptr) {
        return false;
    }

    std::string shadow_tag;
    if (modular_tree_asset_.HasTag("shadow", "tree")) {
        shadow_tag = "tree";
    } else if (modular_tree_asset_.HasTag("shadow", "tree_shadow")) {
        shadow_tag = "tree_shadow";
    } else {
        return false;
    }

    const Rectangle src = modular_tree_asset_.GetFrame("shadow", shadow_tag, render_time_seconds_);
    if (src.width <= 0.0f || src.height <= 0.0f) {
        return false;
    }

    const Rectangle dst = GetModularTreeSpriteRect(object, "shadow");
    if (proto.masked_occluder) {
        const float sort_y = (static_cast<float>(object.cell.y) + 1.0f) * static_cast<float>(state_.map.cell_size);
        if (DrawMaskedOccluder(dst, *shadow_texture, src, sort_y)) {
            return true;
        }
    }
    if (!has_tree_wind_shader_) {
        DrawTexturePro(*shadow_texture, src, dst, {0, 0}, 0.0f, WHITE);
        return true;
    }

    const float frame_rect[4] = {src.x, src.y, src.width, src.height};
    const float screen_height = static_cast<float>(GetScreenHeight());
    const float camera_target[2] = {camera_.target.x, camera_.target.y};
    const float camera_offset[2] = {camera_.offset.x, camera_.offset.y};
    const float camera_zoom = camera_.zoom;
    const float time_seconds = render_time_seconds_;
    const float sway_strength = proto.wind_strength_pixels > 0.0f ? proto.wind_strength_pixels : Constants::kTreeWindSwayStrengthPixels;
    const float sway_speed = Constants::kTreeWindSpeed * proto.wind_speed_multiplier;
    const float gradient_start = proto.wind_gradient_start;
    SetShaderValueV(tree_wind_shader_, tree_wind_frame_rect_loc_, frame_rect, SHADER_UNIFORM_VEC4, 1);
    SetShaderValue(tree_wind_shader_, tree_wind_screen_height_loc_, &screen_height, SHADER_UNIFORM_FLOAT);
    SetShaderValueV(tree_wind_shader_, tree_wind_camera_target_loc_, camera_target, SHADER_UNIFORM_VEC2, 1);
    SetShaderValueV(tree_wind_shader_, tree_wind_camera_offset_loc_, camera_offset, SHADER_UNIFORM_VEC2, 1);
    SetShaderValue(tree_wind_shader_, tree_wind_camera_zoom_loc_, &camera_zoom, SHADER_UNIFORM_FLOAT);
    SetShaderValue(tree_wind_shader_, tree_wind_time_loc_, &time_seconds, SHADER_UNIFORM_FLOAT);
    SetShaderValue(tree_wind_shader_, tree_wind_sway_strength_loc_, &sway_strength, SHADER_UNIFORM_FLOAT);
    SetShaderValue(tree_wind_shader_, tree_wind_sway_speed_loc_, &sway_speed, SHADER_UNIFORM_FLOAT);
    SetShaderValue(tree_wind_shader_, tree_wind_gradient_start_loc_, &gradient_start, SHADER_UNIFORM_FLOAT);

    BeginShaderMode(tree_wind_shader_);
    DrawTexturePro(*shadow_texture, src, dst, {0, 0}, 0.0f, WHITE);
    EndShaderMode();
    return true;
}

bool GameApp::RenderModularTreeObject(const MapObjectInstance& object, const ObjectPrototype& proto, float sort_y) {
    if (!IsModularTreePrototypeId(proto.id) || !modular_tree_asset_.HasTag("canopy_background", "tree") ||
        !modular_tree_asset_.HasTag("trunk", "tree") || !modular_tree_asset_.HasTag("canopy_foreground", "tree")) {
        return false;
    }

    const Texture2D* canopy_background_texture = modular_tree_asset_.GetLayerTexture("canopy_background");
    const Texture2D* trunk_texture = modular_tree_asset_.GetLayerTexture("trunk");
    const Texture2D* canopy_foreground_texture = modular_tree_asset_.GetLayerTexture("canopy_foreground");
    const Texture2D* outline_mask_texture =
        modular_tree_asset_.HasTag("outline_mask", "tree") ? modular_tree_asset_.GetLayerTexture("outline_mask") : nullptr;
    if (canopy_background_texture == nullptr || trunk_texture == nullptr || canopy_foreground_texture == nullptr) {
        return false;
    }

    const Rectangle canopy_background_src =
        modular_tree_asset_.GetFrame("canopy_background", "tree", render_time_seconds_);
    const Rectangle trunk_src = modular_tree_asset_.GetFrame("trunk", "tree", render_time_seconds_);
    const Rectangle canopy_foreground_src =
        modular_tree_asset_.GetFrame("canopy_foreground", "tree", render_time_seconds_);
    const Rectangle outline_mask_src =
        outline_mask_texture != nullptr ? modular_tree_asset_.GetFrame("outline_mask", "tree", render_time_seconds_) : Rectangle{};
    const Rectangle dst = GetModularTreeSpriteRect(object, "trunk");

    const auto draw_tree_layers_simple = [&]() {
        DrawWindAnimatedMapObject(object, proto, *canopy_background_texture, canopy_background_src, dst);
        DrawTexturePro(*trunk_texture, trunk_src, dst, {0, 0}, 0.0f, WHITE);
        DrawWindAnimatedMapObject(object, proto, *canopy_foreground_texture, canopy_foreground_src, dst);
    };

    if (!has_tree_composite_shader_ && !has_tree_composite_no_reveal_shader_) {
        draw_tree_layers_simple();
        return true;
    }

    std::array<float, Constants::kOccluderRevealMaxCircles * 4> reveal_data = {};
    int circle_count = 0;
    if (proto.masked_occluder) {
        const auto try_add_circle = [&](Vector2 world_center, float radius_world, float target_sort_y) {
            if (circle_count >= Constants::kOccluderRevealMaxCircles || target_sort_y >= sort_y - 0.01f ||
                !ContainsWorldPointExpanded(dst, world_center, radius_world)) {
                return;
            }

            const Vector2 screen_center = GetWorldToScreen2D(world_center, camera_);
            const float radius_px = radius_world * camera_.zoom;
            const float falloff_px = Constants::kOccluderRevealFalloffWorld * camera_.zoom;
            const size_t base = static_cast<size_t>(circle_count++) * 4;
            reveal_data[base + 0] = screen_center.x;
            reveal_data[base + 1] = screen_center.y;
            reveal_data[base + 2] = radius_px;
            reveal_data[base + 3] = radius_px + falloff_px;
        };

        for (const Player& player : state_.players) {
            if (!player.alive) {
                continue;
            }
            const Vector2 world_center = GetRenderPlayerPosition(player.id);
            try_add_circle(world_center, Constants::kOccluderRevealPlayerRadiusWorld, world_center.y + 16.0f);
        }

        for (const MapObjectInstance& other : state_.map_objects) {
            if (!other.alive || other.type != ObjectType::Consumable) {
                continue;
            }
            const Vector2 world_center = CellToWorldCenter(other.cell);
            try_add_circle(world_center, Constants::kOccluderRevealItemRadiusWorld, world_center.y);
        }

        for (const Rune& rune : state_.runes) {
            if (!rune.active && rune.activation_remaining_seconds <= 0.0f) {
                continue;
            }
            const Vector2 world_center = CellToWorldCenter(rune.cell);
            try_add_circle(world_center, Constants::kOccluderRevealItemRadiusWorld, world_center.y);
        }
    }

    const bool use_reveal_shader = circle_count > 0 && has_tree_composite_shader_;
    const bool use_no_reveal_shader = circle_count <= 0 && has_tree_composite_no_reveal_shader_;
    if (!use_reveal_shader && !use_no_reveal_shader) {
        draw_tree_layers_simple();
        return true;
    }

    const float canopy_background_rect[4] = {canopy_background_src.x, canopy_background_src.y,
                                             canopy_background_src.width, canopy_background_src.height};
    const float trunk_rect[4] = {trunk_src.x, trunk_src.y, trunk_src.width, trunk_src.height};
    const float canopy_foreground_rect[4] = {canopy_foreground_src.x, canopy_foreground_src.y,
                                             canopy_foreground_src.width, canopy_foreground_src.height};
    const float mask_rect[4] = {outline_mask_src.x, outline_mask_src.y, outline_mask_src.width, outline_mask_src.height};
    const float time_seconds = render_time_seconds_;
    const float sway_strength = proto.wind_strength_pixels > 0.0f ? proto.wind_strength_pixels : Constants::kTreeWindSwayStrengthPixels;
    const float sway_speed = Constants::kTreeWindSpeed * proto.wind_speed_multiplier;
    const float phase_offset = GetMapObjectWindPhaseOffset(object);
    const float gradient_start = proto.wind_gradient_start;
    if (use_reveal_shader) {
        const float screen_height = static_cast<float>(GetScreenHeight());
        const float inside_alpha = Constants::kOccluderRevealInsideAlpha;
        rlDrawRenderBatchActive();
        BeginShaderMode(tree_composite_shader_);
        SetShaderValueTexture(tree_composite_shader_, tree_composite_trunk_texture_loc_, *trunk_texture);
        SetShaderValueTexture(tree_composite_shader_, tree_composite_canopy_foreground_texture_loc_,
                              *canopy_foreground_texture);
        SetShaderValueTexture(tree_composite_shader_, tree_composite_mask_texture_loc_,
                              outline_mask_texture != nullptr ? *outline_mask_texture : *canopy_background_texture);
        SetShaderValueV(tree_composite_shader_, tree_composite_canopy_background_rect_loc_, canopy_background_rect,
                        SHADER_UNIFORM_VEC4, 1);
        SetShaderValueV(tree_composite_shader_, tree_composite_trunk_rect_loc_, trunk_rect, SHADER_UNIFORM_VEC4, 1);
        SetShaderValueV(tree_composite_shader_, tree_composite_canopy_foreground_rect_loc_, canopy_foreground_rect,
                        SHADER_UNIFORM_VEC4, 1);
        SetShaderValueV(tree_composite_shader_, tree_composite_mask_rect_loc_, mask_rect, SHADER_UNIFORM_VEC4, 1);
        SetShaderValue(tree_composite_shader_, tree_composite_time_loc_, &time_seconds, SHADER_UNIFORM_FLOAT);
        SetShaderValue(tree_composite_shader_, tree_composite_sway_strength_loc_, &sway_strength, SHADER_UNIFORM_FLOAT);
        SetShaderValue(tree_composite_shader_, tree_composite_sway_speed_loc_, &sway_speed, SHADER_UNIFORM_FLOAT);
        SetShaderValue(tree_composite_shader_, tree_composite_phase_offset_loc_, &phase_offset, SHADER_UNIFORM_FLOAT);
        SetShaderValue(tree_composite_shader_, tree_composite_gradient_start_loc_, &gradient_start, SHADER_UNIFORM_FLOAT);
        SetShaderValue(tree_composite_shader_, tree_composite_screen_height_loc_, &screen_height, SHADER_UNIFORM_FLOAT);
        SetShaderValue(tree_composite_shader_, tree_composite_inside_alpha_loc_, &inside_alpha, SHADER_UNIFORM_FLOAT);
        SetShaderValue(tree_composite_shader_, tree_composite_reveal_count_loc_, &circle_count, SHADER_UNIFORM_INT);
        SetShaderValueV(tree_composite_shader_, tree_composite_reveal_data_loc_, reveal_data.data(), SHADER_UNIFORM_VEC4,
                        circle_count);
        DrawTexturePro(*canopy_background_texture, canopy_background_src, dst, {0, 0}, 0.0f, WHITE);
        EndShaderMode();
    } else {
        rlDrawRenderBatchActive();
        BeginShaderMode(tree_composite_no_reveal_shader_);
        SetShaderValueTexture(tree_composite_no_reveal_shader_, tree_composite_no_reveal_trunk_texture_loc_,
                              *trunk_texture);
        SetShaderValueTexture(tree_composite_no_reveal_shader_,
                              tree_composite_no_reveal_canopy_foreground_texture_loc_, *canopy_foreground_texture);
        SetShaderValueTexture(tree_composite_no_reveal_shader_, tree_composite_no_reveal_mask_texture_loc_,
                              outline_mask_texture != nullptr ? *outline_mask_texture : *canopy_background_texture);
        SetShaderValueV(tree_composite_no_reveal_shader_, tree_composite_no_reveal_canopy_background_rect_loc_,
                        canopy_background_rect, SHADER_UNIFORM_VEC4, 1);
        SetShaderValueV(tree_composite_no_reveal_shader_, tree_composite_no_reveal_trunk_rect_loc_, trunk_rect,
                        SHADER_UNIFORM_VEC4, 1);
        SetShaderValueV(tree_composite_no_reveal_shader_, tree_composite_no_reveal_canopy_foreground_rect_loc_,
                        canopy_foreground_rect, SHADER_UNIFORM_VEC4, 1);
        SetShaderValueV(tree_composite_no_reveal_shader_, tree_composite_no_reveal_mask_rect_loc_, mask_rect,
                        SHADER_UNIFORM_VEC4, 1);
        SetShaderValue(tree_composite_no_reveal_shader_, tree_composite_no_reveal_time_loc_, &time_seconds,
                       SHADER_UNIFORM_FLOAT);
        SetShaderValue(tree_composite_no_reveal_shader_, tree_composite_no_reveal_sway_strength_loc_, &sway_strength,
                       SHADER_UNIFORM_FLOAT);
        SetShaderValue(tree_composite_no_reveal_shader_, tree_composite_no_reveal_sway_speed_loc_, &sway_speed,
                       SHADER_UNIFORM_FLOAT);
        SetShaderValue(tree_composite_no_reveal_shader_, tree_composite_no_reveal_phase_offset_loc_, &phase_offset,
                       SHADER_UNIFORM_FLOAT);
        SetShaderValue(tree_composite_no_reveal_shader_, tree_composite_no_reveal_gradient_start_loc_, &gradient_start,
                       SHADER_UNIFORM_FLOAT);
        DrawTexturePro(*canopy_background_texture, canopy_background_src, dst, {0, 0}, 0.0f, WHITE);
        EndShaderMode();
    }
    rlDrawRenderBatchActive();
    return true;
}

void GameApp::Update(float dt) {
    dt = ClampDt(dt);
    escape_pressed_this_update_ = pending_escape_pressed_;
    pending_escape_pressed_ = false;
    enter_pressed_this_update_ = pending_enter_pressed_;
    pending_enter_pressed_ = false;
    enter_shift_down_this_update_ = pending_enter_shift_down_;
    pending_enter_shift_down_ = false;
    backspace_pressed_this_update_ = pending_backspace_pressed_;
    pending_backspace_pressed_ = false;
    text_input_this_update_.swap(pending_text_input_);
    pending_text_input_.clear();
    PumpInfluenceFieldBuilds();
    switch (app_screen_) {
        case AppScreen::MainMenu:
            UpdateMainMenu(dt);
            break;
        case AppScreen::Connecting:
            UpdateConnecting(dt);
            break;
        case AppScreen::Lobby:
            UpdateLobby(dt);
            break;
        case AppScreen::LoadingMatchMap:
            UpdateLoadingMatchMap(dt);
            break;
        case AppScreen::InMatch:
            UpdateMatch(dt);
            break;
        case AppScreen::PostMatch:
            UpdatePostMatch(dt);
            break;
    }
}

void GameApp::PumpInfluenceFieldBuilds() {
    if (influence_build_in_flight_ && influence_build_future_.valid()) {
        const auto status = influence_build_future_.wait_for(std::chrono::milliseconds(0));
        if (status == std::future_status::ready) {
            InfluenceBuildResult result = influence_build_future_.get();
            influence_build_in_flight_ = false;
            if (result.generation == influence_build_generation_ && has_influence_zone_signature_ &&
                result.signature == influence_zone_signature_) {
                ApplyInfluenceBuildResult(std::move(result));
            }
        }
    }

    if (!IsInfluenceZoneSystemEnabled()) {
        pending_influence_build_request_.reset();
        return;
    }

    if (!influence_build_in_flight_ && pending_influence_build_request_.has_value()) {
        StartPendingInfluenceFieldBuild();
    }
}

void GameApp::StartPendingInfluenceFieldBuild() {
    if (influence_build_in_flight_ || !pending_influence_build_request_.has_value()) {
        return;
    }
    InfluenceBuildRequest request = std::move(*pending_influence_build_request_);
    pending_influence_build_request_.reset();
    influence_build_future_ = std::async(std::launch::async, [request]() { return BuildInfluenceFields(request); });
    influence_build_in_flight_ = true;
}

void GameApp::ApplyInfluenceBuildResult(InfluenceBuildResult&& result) {
    auto clear_distance_texture = [](Texture2D* texture, bool* has_texture) {
        if (texture == nullptr || has_texture == nullptr || !*has_texture) {
            return;
        }
        UnloadTexture(*texture);
        *texture = {};
        *has_texture = false;
    };

    const float transition_t =
        std::clamp(influence_zone_transition_elapsed_seconds_ / std::max(0.0001f, Constants::kInfluenceZoneTransitionSeconds),
                   0.0f, 1.0f);
    auto apply_team_result = [&](InfluenceDistanceField&& rebuilt_field, InfluenceDistanceField* from_field,
                                 Texture2D* from_texture, bool* has_from, InfluenceDistanceField* to_field,
                                 Texture2D* to_texture, bool* has_to) {
        if (!has_influence_zone_signature_) {
            return;
        }

        if (!HasInfluenceDistanceField(*from_field) && !HasInfluenceDistanceField(*to_field)) {
            if (std::all_of(rebuilt_field.pixels.begin(), rebuilt_field.pixels.end(),
                            [](const Color& c) { return c.r == 0; })) {
                ClearInfluenceDistanceField(from_field);
                ClearInfluenceDistanceField(to_field);
                clear_distance_texture(from_texture, has_from);
                clear_distance_texture(to_texture, has_to);
                return;
            }
            BuildZeroInfluenceDistanceField(rebuilt_field.width, rebuilt_field.height, from_field);
            BuildInfluenceDistanceTexture(from_texture, has_from, from_field->pixels, from_field->width, from_field->height);
            *to_field = std::move(rebuilt_field);
            BuildInfluenceDistanceTexture(to_texture, has_to, to_field->pixels, to_field->width, to_field->height);
            return;
        }

        InfluenceDistanceField current_visible;
        if (HasInfluenceDistanceField(*from_field) && HasInfluenceDistanceField(*to_field)) {
            BlendInfluenceDistanceField(*from_field, *to_field, transition_t, &current_visible);
        } else if (HasInfluenceDistanceField(*to_field)) {
            CopyInfluenceDistanceField(*to_field, &current_visible);
        } else {
            CopyInfluenceDistanceField(*from_field, &current_visible);
        }

        *from_field = std::move(current_visible);
        BuildInfluenceDistanceTexture(from_texture, has_from, from_field->pixels, from_field->width, from_field->height);
        *to_field = std::move(rebuilt_field);
        BuildInfluenceDistanceTexture(to_texture, has_to, to_field->pixels, to_field->width, to_field->height);
    };

    apply_team_result(std::move(result.red_field), &influence_zone_distance_red_from_field_,
                      &influence_zone_distance_red_from_texture_, &has_influence_zone_distance_red_from_texture_,
                      &influence_zone_distance_red_to_field_, &influence_zone_distance_red_to_texture_,
                      &has_influence_zone_distance_red_to_texture_);
    apply_team_result(std::move(result.blue_field), &influence_zone_distance_blue_from_field_,
                      &influence_zone_distance_blue_from_texture_, &has_influence_zone_distance_blue_from_texture_,
                      &influence_zone_distance_blue_to_field_, &influence_zone_distance_blue_to_texture_,
                      &has_influence_zone_distance_blue_to_texture_);
    influence_zone_transition_elapsed_seconds_ = 0.0f;
}

void GameApp::UpdateMainMenu(float /*dt*/) {
    // Discovery keeps running so joinable hosts appear while user is in the menu.
}

void GameApp::LoadAudioAssets() {
    if (!audio_device_ready_) {
        return;
    }

    auto load_sfx = [&](LoadedSfx& clip, const char* path, float volume) {
        const std::string resolved = ResolveRuntimePath(path);
        if (!FileExists(resolved.c_str())) {
            clip.loaded = false;
            return;
        }
        clip.sound = LoadSound(resolved.c_str());
        clip.loaded = IsLoadedSound(clip.sound);
        if (clip.loaded) {
            SetSoundVolume(clip.sound, volume);
        }
    };
    auto load_sfx_with_fallback = [&](LoadedSfx& clip, const char* primary_path, const char* fallback_path, float volume) {
        const std::string resolved_primary = ResolveRuntimePath(primary_path);
        if (FileExists(resolved_primary.c_str())) {
            clip.sound = LoadSound(resolved_primary.c_str());
            clip.loaded = IsLoadedSound(clip.sound);
            if (clip.loaded) {
                SetSoundVolume(clip.sound, volume);
            }
            return;
        }
        load_sfx(clip, fallback_path, volume);
    };

    load_sfx(sfx_fireball_created_, Constants::kSfxFireballCreatedPath, Constants::kSfxVolumeFireballCreated);
    load_sfx(sfx_melee_attack_, Constants::kSfxMeleeAttackPath, Constants::kSfxVolumeMeleeAttack);
    load_sfx(sfx_create_rune_, Constants::kSfxCreateRunePath, Constants::kSfxVolumeCreateRune);
    load_sfx(sfx_volatile_cast_, Constants::kSfxVolatileCastPath, Constants::kSfxVolumeVolatileCast);
    load_sfx(sfx_explosion_, Constants::kSfxExplosionPath, Constants::kSfxVolumeExplosion);
    load_sfx(sfx_vase_breaking_, Constants::kSfxVaseBreakingPath, Constants::kSfxVolumeVaseBreaking);
    load_sfx(sfx_ice_wall_freeze_, Constants::kSfxIceWallFreezePath, Constants::kSfxVolumeIceWallFreeze);
    load_sfx(sfx_ice_wall_melt_, Constants::kSfxIceWallMeltPath, Constants::kSfxVolumeIceWallMelt);
    load_sfx(sfx_player_death_, Constants::kSfxPlayerDeathPath, Constants::kSfxVolumePlayerDeath);
    load_sfx(sfx_player_damaged_, Constants::kSfxPlayerDamagedPath, Constants::kSfxVolumePlayerDamaged);
    load_sfx(sfx_item_pickup_, Constants::kSfxItemPickupPath, Constants::kSfxVolumeItemPickup);
    load_sfx(sfx_drink_potion_, Constants::kSfxDrinkPotionPath, Constants::kSfxVolumeDrinkPotion);
    load_sfx(sfx_fire_spirit_launch_, Constants::kSfxFireSpiritLaunchPath, Constants::kSfxVolumeFireSpiritLaunch);
    load_sfx(sfx_fire_storm_cast_, Constants::kSfxFireStormCastPath, Constants::kSfxVolumeFireStormCast);
    load_sfx(sfx_fire_storm_impact_, Constants::kSfxFireStormImpactPath, Constants::kSfxVolumeFireStormImpact);
    load_sfx(sfx_static_upgrade_, Constants::kSfxStaticUpgradePath, Constants::kSfxVolumeStaticUpgrade);
    load_sfx(sfx_static_bolt_impact_, Constants::kSfxStaticBoltImpactPath, Constants::kSfxVolumeStaticBoltImpact);
    load_sfx(sfx_grappling_throw_, Constants::kSfxGrapplingThrowPath, Constants::kSfxVolumeGrapplingThrow);
    load_sfx(sfx_grappling_latch_, Constants::kSfxGrapplingLatchPath, Constants::kSfxVolumeGrapplingLatch);
    load_sfx(sfx_earth_rune_launch_, Constants::kSfxEarthRuneLaunchPath, Constants::kSfxVolumeEarthRuneLaunch);
    load_sfx(sfx_earth_rune_impact_, Constants::kSfxEarthRuneImpactPath, Constants::kSfxVolumeEarthRuneImpact);
    load_sfx(sfx_castle_level_up_, Constants::kSfxCastleLevelUpPath, Constants::kSfxVolumeCastleLevelUp);
    load_sfx(sfx_castle_equip_rune_, Constants::kSfxCastleEquipRunePath, Constants::kSfxVolumeCastleEquipRune);
    load_sfx(sfx_castle_unequip_rune_, Constants::kSfxCastleUnequipRunePath, Constants::kSfxVolumeCastleUnequipRune);
    for (size_t i = 0; i < sfx_ice_wave_cast_.size(); ++i) {
        load_sfx_with_fallback(sfx_ice_wave_cast_[i], Constants::kSfxIceWaveCastPaths[i],
                               Constants::kSfxIceWaveCastFallbackPaths[i], Constants::kSfxVolumeIceWaveCast);
    }
    for (size_t i = 0; i < sfx_ice_wave_impact_.size(); ++i) {
        load_sfx_with_fallback(sfx_ice_wave_impact_[i], Constants::kSfxIceWaveImpactPaths[i],
                               Constants::kSfxIceWaveImpactFallbackPaths[i], Constants::kSfxVolumeIceWaveImpact);
    }
    for (size_t i = 0; i < sfx_hammer_swing_.size(); ++i) {
        load_sfx(sfx_hammer_swing_[i], Constants::kSfxHammerSwingPaths[i], Constants::kSfxVolumeHammerSwing);
    }
    for (size_t i = 0; i < sfx_hammer_impact_.size(); ++i) {
        load_sfx(sfx_hammer_impact_[i], Constants::kSfxHammerImpactPaths[i], Constants::kSfxVolumeHammerImpact);
    }
    for (size_t i = 0; i < sfx_zone_damage_.size(); ++i) {
        load_sfx(sfx_zone_damage_[i], Constants::kSfxZoneDamagePaths[i], Constants::kSfxVolumeZoneDamage);
    }
    for (size_t i = 0; i < sfx_footstep_dirt_.size(); ++i) {
        load_sfx(sfx_footstep_dirt_[i], Constants::kSfxFootstepDirtPaths[i], Constants::kSfxVolumeFootstepDirt);
    }

    auto load_music = [&](Music& music, bool& loaded, const char* path) {
        const std::string resolved = ResolveRuntimePath(path);
        if (!FileExists(resolved.c_str())) {
            loaded = false;
            return;
        }
        music = LoadMusicStream(resolved.c_str());
        loaded = IsLoadedMusic(music);
        if (loaded) {
            music.looping = false;
            SetMusicVolume(music, 0.0f);
        }
    };

    load_music(fire_storm_ambient_, has_fire_storm_ambient_, Constants::kFireStormAmbientPath);
    if (has_fire_storm_ambient_) {
        SeekMusicStream(fire_storm_ambient_, Constants::kFireStormAmbientLoopStartSeconds);
    }
    {
        const std::string resolved = ResolveRuntimePath(Constants::kChargingLoopPath);
        if (FileExists(resolved.c_str())) {
            charging_loop_base_sound_ = LoadSound(resolved.c_str());
            has_charging_loop_base_sound_ = IsLoadedSound(charging_loop_base_sound_);
            if (has_charging_loop_base_sound_) {
                if (charging_loop_base_sound_.stream.sampleRate > 0) {
                    charging_loop_duration_seconds_ =
                        static_cast<float>(charging_loop_base_sound_.frameCount) /
                        static_cast<float>(charging_loop_base_sound_.stream.sampleRate);
                } else {
                    charging_loop_duration_seconds_ = 0.0f;
                }
                SetSoundVolume(charging_loop_base_sound_, 0.0f);
                for (size_t i = 0; i < charging_loop_instances_.size(); ++i) {
                    charging_loop_instances_[i] = LoadSoundAlias(charging_loop_base_sound_);
                    has_charging_loop_instances_[i] = IsSoundValid(charging_loop_instances_[i]);
                    if (has_charging_loop_instances_[i]) {
                        SetSoundVolume(charging_loop_instances_[i], 0.0f);
                    }
                }
            }
        }
    }
    {
        const std::string resolved = ResolveRuntimePath(Constants::kEmbersLoopPath);
        if (FileExists(resolved.c_str())) {
            embers_loop_base_sound_ = LoadSound(resolved.c_str());
            has_embers_loop_base_sound_ = IsLoadedSound(embers_loop_base_sound_);
            if (has_embers_loop_base_sound_) {
                if (embers_loop_base_sound_.stream.sampleRate > 0) {
                    embers_loop_duration_seconds_ =
                        static_cast<float>(embers_loop_base_sound_.frameCount) /
                        static_cast<float>(embers_loop_base_sound_.stream.sampleRate);
                } else {
                    embers_loop_duration_seconds_ = 0.0f;
                }
                SetSoundVolume(embers_loop_base_sound_, 0.0f);
                for (size_t i = 0; i < embers_loop_instances_.size(); ++i) {
                    embers_loop_instances_[i] = LoadSoundAlias(embers_loop_base_sound_);
                    has_embers_loop_instances_[i] = IsSoundValid(embers_loop_instances_[i]);
                    if (has_embers_loop_instances_[i]) {
                        SetSoundVolume(embers_loop_instances_[i], 0.0f);
                    }
                }
            }
        }
    }

    const std::string bgm_path = ResolveRuntimePath(Constants::kBgmForestDayPath);
    if (FileExists(bgm_path.c_str())) {
        bgm_forest_day_ = LoadMusicStream(bgm_path.c_str());
        has_bgm_forest_day_ = IsLoadedMusic(bgm_forest_day_);
        if (has_bgm_forest_day_) {
            SetMusicVolume(bgm_forest_day_, Constants::kBgmVolume);
        }
    }

    const std::string outside_game_bgm_path = ResolveRuntimePath(Constants::kBgmOutsideGamePath);
    if (FileExists(outside_game_bgm_path.c_str())) {
        bgm_outside_game_ = LoadMusicStream(outside_game_bgm_path.c_str());
        has_bgm_outside_game_ = IsLoadedMusic(bgm_outside_game_);
        if (has_bgm_outside_game_) {
            SetMusicVolume(bgm_outside_game_, Constants::kBgmVolume);
        }
    }
}

void GameApp::UnloadAudioAssets() {
    if (!audio_device_ready_) {
        return;
    }

    auto unload_sfx = [](LoadedSfx& clip) {
        if (!clip.loaded) {
            return;
        }
        UnloadSound(clip.sound);
        clip = LoadedSfx{};
    };

    unload_sfx(sfx_fireball_created_);
    unload_sfx(sfx_melee_attack_);
    unload_sfx(sfx_create_rune_);
    unload_sfx(sfx_volatile_cast_);
    unload_sfx(sfx_explosion_);
    unload_sfx(sfx_vase_breaking_);
    unload_sfx(sfx_ice_wall_freeze_);
    unload_sfx(sfx_ice_wall_melt_);
    unload_sfx(sfx_player_death_);
    unload_sfx(sfx_player_damaged_);
    unload_sfx(sfx_item_pickup_);
    unload_sfx(sfx_drink_potion_);
    unload_sfx(sfx_fire_spirit_launch_);
    unload_sfx(sfx_fire_storm_cast_);
    unload_sfx(sfx_fire_storm_impact_);
    unload_sfx(sfx_static_upgrade_);
    unload_sfx(sfx_static_bolt_impact_);
    unload_sfx(sfx_grappling_throw_);
    unload_sfx(sfx_grappling_latch_);
    unload_sfx(sfx_earth_rune_launch_);
    unload_sfx(sfx_earth_rune_impact_);
    unload_sfx(sfx_castle_level_up_);
    unload_sfx(sfx_castle_equip_rune_);
    unload_sfx(sfx_castle_unequip_rune_);
    for (auto& clip : sfx_ice_wave_cast_) {
        unload_sfx(clip);
    }
    for (auto& clip : sfx_ice_wave_impact_) {
        unload_sfx(clip);
    }
    for (auto& clip : sfx_hammer_swing_) {
        unload_sfx(clip);
    }
    for (auto& clip : sfx_hammer_impact_) {
        unload_sfx(clip);
    }
    for (auto& clip : sfx_zone_damage_) {
        unload_sfx(clip);
    }
    for (auto& clip : sfx_footstep_dirt_) {
        unload_sfx(clip);
    }

    if (has_fire_storm_ambient_) {
        StopMusicStream(fire_storm_ambient_);
        UnloadMusicStream(fire_storm_ambient_);
        has_fire_storm_ambient_ = false;
    }
    for (size_t i = 0; i < charging_loop_instances_.size(); ++i) {
        if (!has_charging_loop_instances_[i]) {
            continue;
        }
        StopSound(charging_loop_instances_[i]);
        UnloadSoundAlias(charging_loop_instances_[i]);
        has_charging_loop_instances_[i] = false;
    }
    if (has_charging_loop_base_sound_) {
        UnloadSound(charging_loop_base_sound_);
        has_charging_loop_base_sound_ = false;
    }
    for (size_t i = 0; i < embers_loop_instances_.size(); ++i) {
        if (!has_embers_loop_instances_[i]) {
            continue;
        }
        StopSound(embers_loop_instances_[i]);
        UnloadSoundAlias(embers_loop_instances_[i]);
        has_embers_loop_instances_[i] = false;
    }
    if (has_embers_loop_base_sound_) {
        UnloadSound(embers_loop_base_sound_);
        has_embers_loop_base_sound_ = false;
    }

    if (has_bgm_forest_day_) {
        StopMusicStream(bgm_forest_day_);
        UnloadMusicStream(bgm_forest_day_);
        has_bgm_forest_day_ = false;
    }
    if (has_bgm_outside_game_) {
        StopMusicStream(bgm_outside_game_);
        UnloadMusicStream(bgm_outside_game_);
        has_bgm_outside_game_ = false;
    }
    bgm_forest_day_active_last_frame_ = false;
    bgm_outside_game_active_last_frame_ = false;
    charging_loop_gain_ = 0.0f;
    charging_loop_primary_index_ = 0;
    charging_loop_secondary_index_ = 1;
    charging_loop_crossfade_active_ = false;
    charging_loop_crossfade_elapsed_seconds_ = 0.0f;
    charging_loop_elapsed_seconds_ = {0.0f, 0.0f};
    charging_loop_duration_seconds_ = 0.0f;
    embers_loop_gain_ = 0.0f;
    embers_loop_primary_index_ = 0;
    embers_loop_secondary_index_ = 1;
    embers_loop_crossfade_active_ = false;
    embers_loop_crossfade_elapsed_seconds_ = 0.0f;
    embers_loop_elapsed_seconds_ = {0.0f, 0.0f};
    embers_loop_duration_seconds_ = 0.0f;
}

void GameApp::UpdateAudioFrame() {
    if (!audio_device_ready_) {
        return;
    }
    UpdateLocalFootstepAudio();
    UpdateChargingLoopAudio();
    UpdateEmbersLoopAudio();
    const bool should_play_outside_game_bgm = app_screen_ != AppScreen::InMatch;
    const bool should_play_in_match_bgm = app_screen_ == AppScreen::InMatch;

    if (has_bgm_forest_day_) {
        if (should_play_in_match_bgm) {
            if (!bgm_forest_day_active_last_frame_) {
                SeekMusicStream(bgm_forest_day_, 0.0f);
            }
            if (!IsMusicStreamPlaying(bgm_forest_day_)) {
                PlayMusicStream(bgm_forest_day_);
            }
            SetMusicVolume(bgm_forest_day_, Constants::kBgmVolume);
            UpdateMusicStream(bgm_forest_day_);
        } else if (IsMusicStreamPlaying(bgm_forest_day_)) {
            StopMusicStream(bgm_forest_day_);
        }
    }
    if (has_bgm_outside_game_) {
        if (should_play_outside_game_bgm) {
            if (!bgm_outside_game_active_last_frame_) {
                SeekMusicStream(bgm_outside_game_, 0.0f);
            }
            if (!IsMusicStreamPlaying(bgm_outside_game_)) {
                PlayMusicStream(bgm_outside_game_);
            }
            SetMusicVolume(bgm_outside_game_, Constants::kBgmVolume);
            UpdateMusicStream(bgm_outside_game_);
        } else if (IsMusicStreamPlaying(bgm_outside_game_)) {
            StopMusicStream(bgm_outside_game_);
        }
    }
    bgm_forest_day_active_last_frame_ = should_play_in_match_bgm;
    bgm_outside_game_active_last_frame_ = should_play_outside_game_bgm;

    if (!has_fire_storm_ambient_) {
        return;
    }

    UpdateMusicStream(fire_storm_ambient_);

    const float dt = GetFrameTime();
    const bool should_play = HasVisibleIdleFireStormDummy();
    const float fade_speed = (Constants::kFireStormAmbientFadeSeconds > 0.0f)
                                 ? (1.0f / Constants::kFireStormAmbientFadeSeconds)
                                 : 1000.0f;
    const float target_gain = should_play ? 1.0f : 0.0f;
    if (fire_storm_ambient_gain_ < target_gain) {
        fire_storm_ambient_gain_ = std::min(target_gain, fire_storm_ambient_gain_ + dt * fade_speed);
    } else if (fire_storm_ambient_gain_ > target_gain) {
        fire_storm_ambient_gain_ = std::max(target_gain, fire_storm_ambient_gain_ - dt * fade_speed);
    }

    if (should_play && !IsMusicStreamPlaying(fire_storm_ambient_)) {
        SeekMusicStream(fire_storm_ambient_, Constants::kFireStormAmbientLoopStartSeconds);
        PlayMusicStream(fire_storm_ambient_);
    }

    if (IsMusicStreamPlaying(fire_storm_ambient_)) {
        const float played = GetMusicTimePlayed(fire_storm_ambient_);
        if (played >= Constants::kFireStormAmbientLoopEndSeconds) {
            SeekMusicStream(fire_storm_ambient_, Constants::kFireStormAmbientLoopStartSeconds);
        }
    }

    SetMusicVolume(fire_storm_ambient_, Constants::kFireStormAmbientVolume * fire_storm_ambient_gain_);

    if (fire_storm_ambient_gain_ <= 0.001f) {
        if (IsMusicStreamPlaying(fire_storm_ambient_)) {
            StopMusicStream(fire_storm_ambient_);
            SeekMusicStream(fire_storm_ambient_, Constants::kFireStormAmbientLoopStartSeconds);
        }
        SetMusicVolume(fire_storm_ambient_, 0.0f);
    }
}

void GameApp::UpdateChargingLoopAudio() {
    if (!has_charging_loop_instances_[0] && !has_charging_loop_instances_[1]) {
        return;
    }

    const float dt = GetFrameTime();
    for (size_t i = 0; i < charging_loop_instances_.size(); ++i) {
        if (has_charging_loop_instances_[i] && IsSoundPlaying(charging_loop_instances_[i])) {
            charging_loop_elapsed_seconds_[i] += dt;
        }
    }

    float target_gain = 0.0f;
    if (app_screen_ == AppScreen::InMatch && state_.map.cell_size > 0 && camera_.zoom > 0.0001f) {
        const float view_w = static_cast<float>(GetScreenWidth()) / camera_.zoom;
        const float view_h = static_cast<float>(GetScreenHeight()) / camera_.zoom;
        const float left = camera_.target.x - camera_.offset.x / camera_.zoom;
        const float top = camera_.target.y - camera_.offset.y / camera_.zoom;
        const float right = left + view_w;
        const float bottom = top + view_h;
        const Rectangle view_rect = {left, top, view_w, view_h};
        const float outside_fade_distance =
            std::max(static_cast<float>(state_.map.cell_size) * 1.5f,
                     std::min(view_w, view_h) * Constants::kChargingLoopOutsideFadeViewportFraction);

        bool found_inside = false;
        float best_inside_center_distance = std::numeric_limits<float>::max();
        float best_outside_edge_distance = std::numeric_limits<float>::max();

        const auto distance_to_view_edge = [&](Vector2 point) {
            const float dx = (point.x < view_rect.x)
                                 ? (view_rect.x - point.x)
                                 : ((point.x > view_rect.x + view_rect.width) ? (point.x - (view_rect.x + view_rect.width))
                                                                                : 0.0f);
            const float dy = (point.y < view_rect.y)
                                 ? (view_rect.y - point.y)
                                 : ((point.y > view_rect.y + view_rect.height)
                                        ? (point.y - (view_rect.y + view_rect.height))
                                        : 0.0f);
            return std::sqrt(dx * dx + dy * dy);
        };

        for (const Rune& rune : state_.runes) {
            if (!rune.active || !rune.castle_charging) {
                continue;
            }
            const CastleState* castle = FindCastleById(rune.castle_id);
            if (castle == nullptr) {
                continue;
            }
            const Vector2 rune_world = CellToWorldCenter(rune.cell);
            const Vector2 castle_port = GetCastleChargePortWorld(*castle);
            const Vector2 source_world = Vector2Lerp(rune_world, castle_port, 0.5f);
            const bool inside = source_world.x >= left && source_world.x <= right &&
                                source_world.y >= top && source_world.y <= bottom;
            if (inside) {
                const float center_distance = Vector2Distance(source_world, camera_.target);
                if (!found_inside || center_distance < best_inside_center_distance) {
                    found_inside = true;
                    best_inside_center_distance = center_distance;
                }
                continue;
            }

            const float edge_distance = distance_to_view_edge(source_world);
            if (edge_distance < best_outside_edge_distance) {
                best_outside_edge_distance = edge_distance;
            }
        }

        if (found_inside) {
            target_gain = 1.0f;
        } else if (best_outside_edge_distance < std::numeric_limits<float>::max() && outside_fade_distance > 0.0f) {
            target_gain = std::clamp(1.0f - best_outside_edge_distance / outside_fade_distance, 0.0f, 1.0f);
        }
    }

    const float fade_speed = (Constants::kChargingLoopFadeSeconds > 0.0f)
                                 ? (1.0f / Constants::kChargingLoopFadeSeconds)
                                 : 1000.0f;
    if (charging_loop_gain_ < target_gain) {
        charging_loop_gain_ = std::min(target_gain, charging_loop_gain_ + dt * fade_speed);
    } else if (charging_loop_gain_ > target_gain) {
        charging_loop_gain_ = std::max(target_gain, charging_loop_gain_ - dt * fade_speed);
    }

    const auto stop_instance = [&](int index) {
        if (index < 0 || index >= static_cast<int>(charging_loop_instances_.size()) ||
            !has_charging_loop_instances_[static_cast<size_t>(index)]) {
            return;
        }
        if (IsSoundPlaying(charging_loop_instances_[static_cast<size_t>(index)])) {
            StopSound(charging_loop_instances_[static_cast<size_t>(index)]);
        }
        charging_loop_elapsed_seconds_[static_cast<size_t>(index)] = 0.0f;
        SetSoundVolume(charging_loop_instances_[static_cast<size_t>(index)], 0.0f);
    };
    const auto start_instance = [&](int index) {
        if (index < 0 || index >= static_cast<int>(charging_loop_instances_.size()) ||
            !has_charging_loop_instances_[static_cast<size_t>(index)]) {
            return false;
        }
        StopSound(charging_loop_instances_[static_cast<size_t>(index)]);
        charging_loop_elapsed_seconds_[static_cast<size_t>(index)] = 0.0f;
        PlaySound(charging_loop_instances_[static_cast<size_t>(index)]);
        return true;
    };

    if (target_gain > 0.001f) {
        const bool primary_playing =
            has_charging_loop_instances_[static_cast<size_t>(charging_loop_primary_index_)] &&
            IsSoundPlaying(charging_loop_instances_[static_cast<size_t>(charging_loop_primary_index_)]);
        const bool secondary_playing =
            has_charging_loop_instances_[static_cast<size_t>(charging_loop_secondary_index_)] &&
            IsSoundPlaying(charging_loop_instances_[static_cast<size_t>(charging_loop_secondary_index_)]);
        if (!primary_playing && !secondary_playing) {
            charging_loop_primary_index_ = 0;
            charging_loop_secondary_index_ = 1;
            charging_loop_crossfade_active_ = false;
            charging_loop_crossfade_elapsed_seconds_ = 0.0f;
            start_instance(charging_loop_primary_index_);
        }
    }

    if (charging_loop_crossfade_active_) {
        charging_loop_crossfade_elapsed_seconds_ += dt;
        const float crossfade_t =
            (Constants::kChargingLoopCrossfadeSeconds > 0.0f)
                ? std::clamp(charging_loop_crossfade_elapsed_seconds_ / Constants::kChargingLoopCrossfadeSeconds, 0.0f, 1.0f)
                : 1.0f;
        const float primary_mix = 1.0f - crossfade_t;
        const float secondary_mix = crossfade_t;
        if (has_charging_loop_instances_[static_cast<size_t>(charging_loop_primary_index_)]) {
            SetSoundVolume(charging_loop_instances_[static_cast<size_t>(charging_loop_primary_index_)],
                           Constants::kChargingLoopVolume * charging_loop_gain_ * primary_mix);
        }
        if (has_charging_loop_instances_[static_cast<size_t>(charging_loop_secondary_index_)]) {
            SetSoundVolume(charging_loop_instances_[static_cast<size_t>(charging_loop_secondary_index_)],
                           Constants::kChargingLoopVolume * charging_loop_gain_ * secondary_mix);
        }
        if (crossfade_t >= 0.999f) {
            stop_instance(charging_loop_primary_index_);
            std::swap(charging_loop_primary_index_, charging_loop_secondary_index_);
            charging_loop_crossfade_active_ = false;
            charging_loop_crossfade_elapsed_seconds_ = 0.0f;
        }
    } else {
        if (has_charging_loop_instances_[static_cast<size_t>(charging_loop_primary_index_)]) {
            SetSoundVolume(charging_loop_instances_[static_cast<size_t>(charging_loop_primary_index_)],
                           Constants::kChargingLoopVolume * charging_loop_gain_);
        }
        if (has_charging_loop_instances_[static_cast<size_t>(charging_loop_secondary_index_)]) {
            SetSoundVolume(charging_loop_instances_[static_cast<size_t>(charging_loop_secondary_index_)], 0.0f);
        }

        if (charging_loop_gain_ > 0.001f &&
            has_charging_loop_instances_[static_cast<size_t>(charging_loop_primary_index_)]) {
            const float music_length = charging_loop_duration_seconds_;
            const float crossfade_window = std::max(0.02f, Constants::kChargingLoopCrossfadeSeconds);
            if (music_length > crossfade_window &&
                IsSoundPlaying(charging_loop_instances_[static_cast<size_t>(charging_loop_primary_index_)]) &&
                charging_loop_elapsed_seconds_[static_cast<size_t>(charging_loop_primary_index_)] >=
                    music_length - crossfade_window) {
                const int next_index = charging_loop_secondary_index_;
                if (has_charging_loop_instances_[static_cast<size_t>(next_index)] &&
                    !IsSoundPlaying(charging_loop_instances_[static_cast<size_t>(next_index)])) {
                    if (start_instance(next_index)) {
                        charging_loop_crossfade_active_ = true;
                        charging_loop_crossfade_elapsed_seconds_ = 0.0f;
                    }
                }
            }
        }
    }
    if (charging_loop_gain_ <= 0.001f) {
        stop_instance(0);
        stop_instance(1);
        charging_loop_primary_index_ = 0;
        charging_loop_secondary_index_ = 1;
        charging_loop_crossfade_active_ = false;
        charging_loop_crossfade_elapsed_seconds_ = 0.0f;
    }
}

void GameApp::UpdateEmbersLoopAudio() {
    if (!has_embers_loop_instances_[0] && !has_embers_loop_instances_[1]) {
        return;
    }

    const float dt = GetFrameTime();
    for (size_t i = 0; i < embers_loop_instances_.size(); ++i) {
        if (has_embers_loop_instances_[i] && IsSoundPlaying(embers_loop_instances_[i])) {
            embers_loop_elapsed_seconds_[i] += dt;
        }
    }

    const float target_gain = (app_screen_ == AppScreen::InMatch && HasVisibleEmbers()) ? 1.0f : 0.0f;
    const float fade_speed = (Constants::kEmbersLoopFadeSeconds > 0.0f)
                                 ? (1.0f / Constants::kEmbersLoopFadeSeconds)
                                 : 1000.0f;
    if (embers_loop_gain_ < target_gain) {
        embers_loop_gain_ = std::min(target_gain, embers_loop_gain_ + dt * fade_speed);
    } else if (embers_loop_gain_ > target_gain) {
        embers_loop_gain_ = std::max(target_gain, embers_loop_gain_ - dt * fade_speed);
    }

    const auto stop_instance = [&](int index) {
        if (index < 0 || index >= static_cast<int>(embers_loop_instances_.size()) ||
            !has_embers_loop_instances_[static_cast<size_t>(index)]) {
            return;
        }
        if (IsSoundPlaying(embers_loop_instances_[static_cast<size_t>(index)])) {
            StopSound(embers_loop_instances_[static_cast<size_t>(index)]);
        }
        embers_loop_elapsed_seconds_[static_cast<size_t>(index)] = 0.0f;
        SetSoundVolume(embers_loop_instances_[static_cast<size_t>(index)], 0.0f);
    };
    const auto start_instance = [&](int index) {
        if (index < 0 || index >= static_cast<int>(embers_loop_instances_.size()) ||
            !has_embers_loop_instances_[static_cast<size_t>(index)]) {
            return false;
        }
        StopSound(embers_loop_instances_[static_cast<size_t>(index)]);
        embers_loop_elapsed_seconds_[static_cast<size_t>(index)] = 0.0f;
        PlaySound(embers_loop_instances_[static_cast<size_t>(index)]);
        return true;
    };

    if (target_gain > 0.001f) {
        const bool primary_playing =
            has_embers_loop_instances_[static_cast<size_t>(embers_loop_primary_index_)] &&
            IsSoundPlaying(embers_loop_instances_[static_cast<size_t>(embers_loop_primary_index_)]);
        const bool secondary_playing =
            has_embers_loop_instances_[static_cast<size_t>(embers_loop_secondary_index_)] &&
            IsSoundPlaying(embers_loop_instances_[static_cast<size_t>(embers_loop_secondary_index_)]);
        if (!primary_playing && !secondary_playing) {
            embers_loop_primary_index_ = 0;
            embers_loop_secondary_index_ = 1;
            embers_loop_crossfade_active_ = false;
            embers_loop_crossfade_elapsed_seconds_ = 0.0f;
            start_instance(embers_loop_primary_index_);
        }
    }

    if (embers_loop_crossfade_active_) {
        embers_loop_crossfade_elapsed_seconds_ += dt;
        const float crossfade_t =
            (Constants::kEmbersLoopCrossfadeSeconds > 0.0f)
                ? std::clamp(embers_loop_crossfade_elapsed_seconds_ / Constants::kEmbersLoopCrossfadeSeconds, 0.0f, 1.0f)
                : 1.0f;
        const float primary_mix = 1.0f - crossfade_t;
        const float secondary_mix = crossfade_t;
        if (has_embers_loop_instances_[static_cast<size_t>(embers_loop_primary_index_)]) {
            SetSoundVolume(embers_loop_instances_[static_cast<size_t>(embers_loop_primary_index_)],
                           Constants::kEmbersLoopVolume * embers_loop_gain_ * primary_mix);
        }
        if (has_embers_loop_instances_[static_cast<size_t>(embers_loop_secondary_index_)]) {
            SetSoundVolume(embers_loop_instances_[static_cast<size_t>(embers_loop_secondary_index_)],
                           Constants::kEmbersLoopVolume * embers_loop_gain_ * secondary_mix);
        }
        if (crossfade_t >= 0.999f) {
            stop_instance(embers_loop_primary_index_);
            std::swap(embers_loop_primary_index_, embers_loop_secondary_index_);
            embers_loop_crossfade_active_ = false;
            embers_loop_crossfade_elapsed_seconds_ = 0.0f;
        }
    } else {
        if (has_embers_loop_instances_[static_cast<size_t>(embers_loop_primary_index_)]) {
            SetSoundVolume(embers_loop_instances_[static_cast<size_t>(embers_loop_primary_index_)],
                           Constants::kEmbersLoopVolume * embers_loop_gain_);
        }
        if (has_embers_loop_instances_[static_cast<size_t>(embers_loop_secondary_index_)]) {
            SetSoundVolume(embers_loop_instances_[static_cast<size_t>(embers_loop_secondary_index_)], 0.0f);
        }

        if (embers_loop_gain_ > 0.001f &&
            has_embers_loop_instances_[static_cast<size_t>(embers_loop_primary_index_)]) {
            const float loop_length = embers_loop_duration_seconds_;
            const float crossfade_window = std::max(0.02f, Constants::kEmbersLoopCrossfadeSeconds);
            if (loop_length > crossfade_window &&
                IsSoundPlaying(embers_loop_instances_[static_cast<size_t>(embers_loop_primary_index_)]) &&
                embers_loop_elapsed_seconds_[static_cast<size_t>(embers_loop_primary_index_)] >=
                    loop_length - crossfade_window) {
                const int next_index = embers_loop_secondary_index_;
                if (has_embers_loop_instances_[static_cast<size_t>(next_index)] &&
                    !IsSoundPlaying(embers_loop_instances_[static_cast<size_t>(next_index)])) {
                    if (start_instance(next_index)) {
                        embers_loop_crossfade_active_ = true;
                        embers_loop_crossfade_elapsed_seconds_ = 0.0f;
                    }
                }
            }
        }
    }

    if (embers_loop_gain_ <= 0.001f) {
        stop_instance(embers_loop_primary_index_);
        stop_instance(embers_loop_secondary_index_);
        embers_loop_primary_index_ = 0;
        embers_loop_secondary_index_ = 1;
        embers_loop_crossfade_active_ = false;
        embers_loop_crossfade_elapsed_seconds_ = 0.0f;
    }
}

void GameApp::UpdateLocalFootstepAudio() {
    if (state_.local_player_id < 0) {
        has_local_footstep_prev_pos_ = false;
        local_footstep_distance_accumulator_ = 0.0f;
        return;
    }

    const Player* local_player = FindPlayerById(state_.local_player_id);
    if (local_player == nullptr || !local_player->alive) {
        has_local_footstep_prev_pos_ = false;
        local_footstep_distance_accumulator_ = 0.0f;
        return;
    }

    const bool deliberate_movement = IsBindingDown(controls_bindings_.move_left) ||
                                     IsBindingDown(controls_bindings_.move_right) ||
                                     IsBindingDown(controls_bindings_.move_up) ||
                                     IsBindingDown(controls_bindings_.move_down);
    const bool blocked = HasStatusEffect(*local_player, StatusEffectType::Stunned) ||
                         IsPlayerBeingPulled(local_player->id);
    const float step_distance =
        static_cast<float>(std::max(1, state_.map.cell_size)) * (2.0f / 3.0f);

    if (!has_local_footstep_prev_pos_) {
        local_footstep_prev_pos_ = local_player->pos;
        has_local_footstep_prev_pos_ = true;
        local_footstep_distance_accumulator_ = 0.0f;
        return;
    }

    const float frame_distance = Vector2Distance(local_footstep_prev_pos_, local_player->pos);
    local_footstep_prev_pos_ = local_player->pos;

    if (!deliberate_movement || blocked) {
        local_footstep_distance_accumulator_ = 0.0f;
        return;
    }

    local_footstep_distance_accumulator_ += frame_distance;
    while (local_footstep_distance_accumulator_ >= step_distance) {
        local_footstep_distance_accumulator_ -= step_distance;
        std::uniform_int_distribution<int> clip_dist(0, static_cast<int>(sfx_footstep_dirt_.size()) - 1);
        LoadedSfx& clip = sfx_footstep_dirt_[static_cast<size_t>(clip_dist(visual_rng_))];
        PlaySfxIfVisible(clip.sound, clip.loaded, local_player->pos);
    }
}

Vector2 GameApp::GetRenderGrapplingHookHeadPosition(int hook_id, Vector2 fallback) const {
    auto it = render_grappling_hook_head_positions_.find(hook_id);
    if (it != render_grappling_hook_head_positions_.end()) {
        return it->second;
    }
    return fallback;
}

const GameApp::ActiveModularAttackVisual* GameApp::FindActiveModularAttackVisual(int player_id) const {
    const auto it = active_modular_attack_visuals_.find(player_id);
    return it != active_modular_attack_visuals_.end() ? &it->second : nullptr;
}

float GameApp::GetPlayerLockedMeleeAimRadians(const Player& player) const {
    if (const ActiveModularAttackVisual* active_visual = FindActiveModularAttackVisual(player.id);
        active_visual != nullptr && active_visual->uses_continuous_orientation) {
        return active_visual->locked_target_angle_radians;
    }
    return std::atan2(player.aim_dir.y, player.aim_dir.x);
}

float GameApp::GetPlayerAttackVisualRotationDegrees(const Player& player) const {
    const ActiveModularAttackVisual* active_visual = FindActiveModularAttackVisual(player.id);
    if (active_visual == nullptr || !active_visual->uses_continuous_orientation) {
        return 0.0f;
    }

    const float elapsed = active_visual->elapsed_seconds;
    const float impact_time = std::max(0.0f, active_visual->impact_time_seconds);
    const float recovery_start = std::max(impact_time, active_visual->recovery_start_time_seconds);
    const float attack_end = std::max(recovery_start, active_visual->attack_end_time_seconds);
    const float rotate_in_delay = impact_time * 0.66f;

    float current_angle = active_visual->locked_target_angle_radians;
    if (elapsed < impact_time && impact_time > 0.0f) {
        float t = 1.0f;
        if (impact_time > rotate_in_delay) {
            t = (elapsed - rotate_in_delay) / (impact_time - rotate_in_delay);
        }
        current_angle = LerpAngleRadiansShortest(active_visual->snapped_angle_radians,
                                                active_visual->locked_target_angle_radians, EaseSigmoid01(t));
    } else if (elapsed >= recovery_start && attack_end > recovery_start) {
        const float t = (elapsed - recovery_start) / (attack_end - recovery_start);
        current_angle = LerpAngleRadiansShortest(active_visual->locked_target_angle_radians,
                                                active_visual->snapped_angle_radians,
                                                EaseSigmoid01(t));
    }

    return NormalizeAngleRadians(current_angle - active_visual->snapped_angle_radians) * RAD2DEG;
}

bool GameApp::IsWorldPointInsideCameraView(Vector2 world_pos) const {
    if (camera_.zoom <= 0.0001f) {
        return false;
    }
    const float view_w = static_cast<float>(GetScreenWidth()) / camera_.zoom;
    const float view_h = static_cast<float>(GetScreenHeight()) / camera_.zoom;
    const float left = camera_.target.x - camera_.offset.x / camera_.zoom;
    const float top = camera_.target.y - camera_.offset.y / camera_.zoom;
    const float right = left + view_w;
    const float bottom = top + view_h;
    return world_pos.x >= left && world_pos.x <= right && world_pos.y >= top && world_pos.y <= bottom;
}

void GameApp::PlaySfxIfVisible(const Sound& sound, bool loaded, Vector2 world_pos) const {
    if (!audio_device_ready_ || !loaded) {
        return;
    }
    if (!IsWorldPointInsideCameraView(world_pos)) {
        return;
    }
    PlaySound(sound);
}

bool GameApp::ShouldPlayImmediateMeleeSwingSfx(const Player& player) const {
    const EquipmentItemDefinition* item =
        equipment_registry_.ResolveEquippedItem(player, EquipmentSlot::PrimaryWeapon);
    return item == nullptr || item->id != "hammer_item";
}

bool GameApp::HasVisibleIdleFireStormDummy() const {
    for (const auto& rune : state_.runes) {
        if (rune.rune_type != RuneType::FireStormDummy || rune.fire_storm_visual_state == FireStormRuneVisualState::Dying) {
            continue;
        }
        if (IsWorldPointInsideCameraView(CellToWorldCenter(rune.cell))) {
            return true;
        }
    }
    for (const auto& dummy : state_.fire_storm_dummies) {
        if (!dummy.alive || dummy.state != FireStormDummyState::Idle) {
            continue;
        }
        if (IsWorldPointInsideCameraView(CellToWorldCenter(dummy.cell))) {
            return true;
        }
    }
    return false;
}

bool GameApp::HasVisibleEmbers() const {
    for (const auto& modifier : state_.embers_tile_modifiers) {
        if (!modifier.alive) {
            continue;
        }
        if (IsWorldPointInsideCameraView(CellToWorldCenter(modifier.cell))) {
            return true;
        }
    }
    return false;
}

void GameApp::UpdateConnecting(float /*dt*/) {
    if (escape_pressed_this_update_) {
        ReturnToMainMenu();
        return;
    }

    if (network_manager_.IsHost()) {
        app_screen_ = AppScreen::Lobby;
        return;
    }

    if (const auto lobby_update = network_manager_.ConsumeLobbyState(); lobby_update.has_value()) {
        lobby_player_names_.clear();
        for (const auto& player : lobby_update->players) {
            lobby_player_names_.push_back(player.name);
        }
        lobby_mode_type_ = static_cast<MatchModeType>(lobby_update->mode_type);
        lobby_round_time_seconds_ = lobby_update->round_time_seconds;
        lobby_best_of_target_kills_ = lobby_update->best_of_target_kills;
        lobby_zone_enabled_ = lobby_update->zone_enabled;
        lobby_shrink_tiles_per_second_ = lobby_update->shrink_tiles_per_second;
        lobby_shrink_start_seconds_ = lobby_update->shrink_start_seconds;
        lobby_min_arena_radius_tiles_ = lobby_update->min_arena_radius_tiles;
        lobby_selected_map_key_ = lobby_update->selected_map_key;
        lobby_selected_map_label_ = lobby_update->selected_map_label;
        if ((lobby_update->preview_generation != applied_lobby_preview_generation_ ||
             lobby_update->selected_map_key != applied_lobby_preview_map_key_) &&
            !lobby_update->preview_png_bytes.empty()) {
            lobby_preview_png_bytes_ = lobby_update->preview_png_bytes;
            ApplyLobbyPreviewTextureFromPngBytes(lobby_preview_png_bytes_);
            applied_lobby_preview_generation_ = lobby_update->preview_generation;
            applied_lobby_preview_map_key_ = lobby_update->selected_map_key;
            lobby_preview_status_text_.clear();
        }
    }

    if (network_manager_.HasReceivedJoinAck() || network_manager_.HasReceivedLobbyState()) {
        app_screen_ = AppScreen::Lobby;
        return;
    }

    const ClientConnectionState state = network_manager_.GetClientConnectionState();
    const double now = GetTime();
    if (state == ClientConnectionState::Disconnected && (now - connect_attempt_start_seconds_) > 0.4) {
        network_manager_.Stop();
        app_screen_ = AppScreen::MainMenu;
        return;
    }

    constexpr double kConnectTimeoutSeconds = 10.0;
    if ((now - connect_attempt_start_seconds_) > kConnectTimeoutSeconds) {
        std::printf("[NET] Client connection timed out after %.1fs\n", kConnectTimeoutSeconds);
        network_manager_.Stop();
        app_screen_ = AppScreen::MainMenu;
    }
}

void GameApp::RefreshLobbyMapCatalog() {
    namespace fs = std::filesystem;
    const std::string previous_key = !lobby_selected_map_key_.empty()
                                         ? lobby_selected_map_key_
                                         : fs::path(resolved_host_selected_map_path_.empty() ? resolved_default_map_path_
                                                                                            : resolved_host_selected_map_path_)
                                               .filename()
                                               .string();
    lobby_map_catalog_.clear();

    const fs::path map_dir = fs::path(resolved_default_map_path_).parent_path();
    if (!map_dir.empty() && fs::exists(map_dir) && fs::is_directory(map_dir)) {
        for (const auto& entry : fs::directory_iterator(map_dir)) {
            LobbyMapEntry map_entry;
            if (entry.is_regular_file()) {
                const std::string extension = entry.path().extension().string();
                if (extension != ".png" && extension != ".PNG") {
                    continue;
                }
                map_entry.key = entry.path().filename().string();
                map_entry.label = entry.path().stem().string();
                map_entry.resolved_path = entry.path().string();
                lobby_map_catalog_.push_back(std::move(map_entry));
            } else if (entry.is_directory() && IsLayeredMapDirectoryPath(entry.path().string())) {
                map_entry.key = entry.path().filename().string();
                map_entry.label = entry.path().filename().string();
                map_entry.resolved_path = entry.path().string();
                lobby_map_catalog_.push_back(std::move(map_entry));
            }
        }
    }

    std::sort(lobby_map_catalog_.begin(), lobby_map_catalog_.end(),
              [](const LobbyMapEntry& a, const LobbyMapEntry& b) { return a.key < b.key; });

    if (lobby_map_catalog_.empty() && std::filesystem::exists(std::filesystem::path(resolved_default_map_path_))) {
        const fs::path default_path(resolved_default_map_path_);
        lobby_map_catalog_.push_back(
            {default_path.filename().string(), default_path.stem().string(), resolved_default_map_path_});
    }

    int selected_index = 0;
    for (size_t i = 0; i < lobby_map_catalog_.size(); ++i) {
        if (lobby_map_catalog_[i].key == previous_key) {
            selected_index = static_cast<int>(i);
            break;
        }
    }
    lobby_selected_map_index_ = std::clamp(selected_index, 0, std::max(0, static_cast<int>(lobby_map_catalog_.size()) - 1));
}

void GameApp::SetSelectedLobbyMapIndex(int index) {
    if (lobby_map_catalog_.empty()) {
        lobby_selected_map_index_ = 0;
        resolved_host_selected_map_path_ = resolved_default_map_path_;
        lobby_selected_map_key_.clear();
        lobby_selected_map_label_ = "(no maps)";
        return;
    }

    lobby_selected_map_index_ =
        std::clamp(index, 0, std::max(0, static_cast<int>(lobby_map_catalog_.size()) - 1));
    const LobbyMapEntry& entry = lobby_map_catalog_[static_cast<size_t>(lobby_selected_map_index_)];
    resolved_host_selected_map_path_ = entry.resolved_path;
    lobby_selected_map_key_ = entry.key;
    lobby_selected_map_label_ = entry.label;
    RebuildLobbyMapPreview();
}

std::string GameApp::BuildLobbyMapOptionsText() const {
    if (lobby_map_catalog_.empty()) {
        return "No maps";
    }
    std::string text;
    for (size_t i = 0; i < lobby_map_catalog_.size(); ++i) {
        if (i > 0) {
            text += ';';
        }
        text += lobby_map_catalog_[i].label;
    }
    return text;
}

std::string GameApp::GetSelectedLobbyMapLabel() const {
    return lobby_selected_map_label_.empty() ? "(no maps)" : lobby_selected_map_label_;
}

void GameApp::ClearLobbyMapPreviewTexture() {
    if (has_lobby_map_preview_texture_) {
        UnloadTexture(lobby_map_preview_texture_);
        lobby_map_preview_texture_ = {};
        has_lobby_map_preview_texture_ = false;
    }
}

void GameApp::ApplyLobbyPreviewTextureFromPngBytes(const std::vector<uint8_t>& png_bytes) {
    ClearLobbyMapPreviewTexture();
    if (png_bytes.empty()) {
        return;
    }

    Image image = LoadImageFromMemory(".png", png_bytes.data(), static_cast<int>(png_bytes.size()));
    if (image.data == nullptr) {
        return;
    }

    lobby_map_preview_texture_ = LoadTextureFromImage(image);
    has_lobby_map_preview_texture_ = (lobby_map_preview_texture_.id != 0);
    if (has_lobby_map_preview_texture_) {
        SetTextureFilter(lobby_map_preview_texture_, TEXTURE_FILTER_POINT);
    }
    UnloadImage(image);
}

bool GameApp::RebuildLobbyMapPreview() {
    namespace fs = std::filesystem;
    lobby_preview_png_bytes_.clear();
    lobby_preview_status_text_ = "Preview unavailable";
    ClearLobbyMapPreviewTexture();

    if (resolved_host_selected_map_path_.empty() || !fs::exists(fs::path(resolved_host_selected_map_path_))) {
        return false;
    }

    bool rendered_preview = false;
    GameState saved_state = std::move(state_);
    auto saved_altars = std::move(altars_);
    auto saved_pickup_blocks = std::move(dropped_item_pickup_blocks_);
    auto saved_loot_quota = std::move(loot_quota_remaining_);
    const float saved_render_time = render_time_seconds_;
    const Camera2D saved_camera = camera_;

    MapData preview_map;
    if (map_loader_.Load(resolved_host_selected_map_path_, &objects_database_, preview_map)) {
        state_ = GameState{};
        state_.map = std::move(preview_map);
        state_.next_entity_id = 1;
        render_time_seconds_ = 0.0f;
        RebuildMapObjectsFromSeeds();

        const int supersample = std::max(1, Constants::kLobbyMapPreviewSupersampleFactor);
        const int preview_render_width = Constants::kLobbyMapPreviewWidth * supersample;
        const int preview_render_height = Constants::kLobbyMapPreviewHeight * supersample;
        RenderTexture2D preview_target = LoadRenderTexture(preview_render_width, preview_render_height);
        if (preview_target.id != 0 && preview_target.texture.id != 0 && state_.map.cell_size > 0) {
            const float world_width = static_cast<float>(state_.map.width * state_.map.cell_size);
            const float world_height = static_cast<float>(state_.map.height * state_.map.cell_size);
            Camera2D preview_camera = {};
            preview_camera.offset = {static_cast<float>(preview_render_width) * 0.5f,
                                     static_cast<float>(preview_render_height) * 0.5f};
            preview_camera.target = {world_width * 0.5f, world_height * 0.5f};
            preview_camera.rotation = 0.0f;
            preview_camera.zoom =
                std::min(static_cast<float>(preview_render_width) / std::max(1.0f, world_width),
                         static_cast<float>(preview_render_height) / std::max(1.0f, world_height));
            camera_ = preview_camera;

            BeginTextureMode(preview_target);
            ClearBackground(Color{12, 14, 18, 255});
            BeginMode2D(camera_);
            RenderMap();
            RenderMapForeground();
            RenderGroundMapObjects();
            RenderNonTerrainDepthSorted(DepthSortedRenderPass::UnderInfluenceOverlay);
            RenderNonTerrainDepthSorted(DepthSortedRenderPass::OverInfluenceOverlay);
            EndMode2D();
            EndTextureMode();

            Image image = LoadImageFromTexture(preview_target.texture);
            if (image.data != nullptr) {
                ImageFlipVertical(&image);
                if (Constants::kLobbyMapPreviewGaussianBlurSize > 0) {
                    ImageBlurGaussian(&image, Constants::kLobbyMapPreviewGaussianBlurSize);
                }
                if (image.width != Constants::kLobbyMapPreviewWidth ||
                    image.height != Constants::kLobbyMapPreviewHeight) {
                    ImageResize(&image, Constants::kLobbyMapPreviewWidth, Constants::kLobbyMapPreviewHeight);
                }
                int png_size = 0;
                unsigned char* png_bytes = ExportImageToMemory(image, ".png", &png_size);
                if (png_bytes != nullptr && png_size > 0) {
                    lobby_preview_png_bytes_.assign(png_bytes, png_bytes + png_size);
                    UnloadFileData(png_bytes);
                    rendered_preview = true;
                }
                UnloadImage(image);
            }
            UnloadRenderTexture(preview_target);
        }
    }

    state_ = std::move(saved_state);
    altars_ = std::move(saved_altars);
    dropped_item_pickup_blocks_ = std::move(saved_pickup_blocks);
    loot_quota_remaining_ = std::move(saved_loot_quota);
    render_time_seconds_ = saved_render_time;
    camera_ = saved_camera;

    if (!rendered_preview && IsPngMapFilePath(resolved_host_selected_map_path_) &&
        !ReadFileBytes(resolved_host_selected_map_path_, &lobby_preview_png_bytes_)) {
        lobby_preview_png_bytes_.clear();
        lobby_preview_status_text_ = "Preview load failed";
        return false;
    }

    ApplyLobbyPreviewTextureFromPngBytes(lobby_preview_png_bytes_);
    lobby_preview_generation_ += 1;
    applied_lobby_preview_generation_ = lobby_preview_generation_;
    applied_lobby_preview_map_key_ = lobby_selected_map_key_;
    lobby_preview_status_text_ =
        rendered_preview ? "" : (IsPngMapFilePath(resolved_host_selected_map_path_) ? "PNG preview fallback" : "Preview unavailable");
    return has_lobby_map_preview_texture_;
}

bool GameApp::LoadMapOrFallback(const std::string& preferred_map_path) {
    if (!preferred_map_path.empty() && map_loader_.Load(preferred_map_path, &objects_database_, state_.map)) {
        resolved_map_path_ = preferred_map_path;
        return true;
    }
    if (map_loader_.Load(resolved_default_map_path_, &objects_database_, state_.map)) {
        resolved_map_path_ = resolved_default_map_path_;
        return false;
    }

    state_.map.width = 24;
    state_.map.height = 16;
    state_.map.cell_size = Constants::kRuneCellSize;
    state_.map.tiles.assign(static_cast<size_t>(state_.map.width * state_.map.height), TileType::Grass);
    state_.map.decorations.assign(static_cast<size_t>(state_.map.width * state_.map.height), "");
    state_.map.object_seeds.clear();
    state_.map.spawn_points = {{2, 2}, {state_.map.width - 3, state_.map.height - 3}};
    resolved_map_path_ = resolved_default_map_path_;
    return false;
}

void GameApp::PumpMapTransferMessages() {
    if (network_manager_.IsHost()) {
        return;
    }

    if (const auto begin = network_manager_.ConsumeMapTransferBegin(); begin.has_value()) {
        PendingMapTransfer transfer;
        transfer.transfer_id = begin->transfer_id;
        transfer.map_key = begin->map_key;
        transfer.map_filename = begin->map_filename;
        transfer.total_bytes = begin->total_bytes;
        transfer.chunk_count = begin->chunk_count;
        transfer.checksum = begin->checksum;
        transfer.chunks.resize(begin->chunk_count);
        transfer.received_chunks.assign(begin->chunk_count, false);
        pending_client_map_transfer_ = std::move(transfer);
        map_loading_status_text_ = TextFormat("Receiving map %s: 0%%", begin->map_filename.c_str());
    }

    for (MapTransferChunkMessage& chunk : network_manager_.ConsumeMapTransferChunks()) {
        if (!pending_client_map_transfer_.has_value() ||
            pending_client_map_transfer_->transfer_id != chunk.transfer_id ||
            chunk.chunk_index >= pending_client_map_transfer_->chunks.size()) {
            continue;
        }

        PendingMapTransfer& transfer = *pending_client_map_transfer_;
        if (!transfer.received_chunks[chunk.chunk_index]) {
            transfer.received_chunks[chunk.chunk_index] = true;
            transfer.received_bytes += chunk.bytes.size();
            transfer.chunks[chunk.chunk_index] = std::move(chunk.bytes);
            const float progress = transfer.total_bytes > 0
                                       ? std::clamp(static_cast<float>(transfer.received_bytes) /
                                                        static_cast<float>(transfer.total_bytes),
                                                    0.0f, 1.0f)
                                       : 0.0f;
            map_loading_status_text_ =
                TextFormat("Receiving map %s: %d%%", transfer.map_filename.c_str(), static_cast<int>(std::round(progress * 100.0f)));
        }
    }

    if (const auto complete = network_manager_.ConsumeMapTransferComplete(); complete.has_value()) {
        if (pending_client_map_transfer_.has_value() &&
            pending_client_map_transfer_->transfer_id == complete->transfer_id) {
            pending_client_map_transfer_->complete_received = true;
        }
    }
}

void GameApp::BeginClientMatchMapLoading(const MatchStartMessage& message) {
    expected_match_transfer_id_ = message.transfer_id;
    expected_match_map_key_ = message.map_key;
    map_loading_status_text_ = "Waiting for map transfer...";
    app_screen_ = AppScreen::LoadingMatchMap;
}

bool GameApp::FinalizeClientTransferredMap() {
    if (!pending_client_map_transfer_.has_value()) {
        return false;
    }

    const PendingMapTransfer transfer = *pending_client_map_transfer_;
    if (transfer.transfer_id != expected_match_transfer_id_ || transfer.map_key != expected_match_map_key_ ||
        !transfer.complete_received || transfer.received_bytes != transfer.total_bytes) {
        return false;
    }
    if (!std::all_of(transfer.received_chunks.begin(), transfer.received_chunks.end(),
                     [](bool received) { return received; })) {
        return false;
    }

    std::vector<uint8_t> file_bytes;
    file_bytes.reserve(transfer.total_bytes);
    for (const auto& chunk : transfer.chunks) {
        file_bytes.insert(file_bytes.end(), chunk.begin(), chunk.end());
    }
    if (ComputeByteChecksum(file_bytes) != transfer.checksum) {
        ReturnToMainMenu();
        main_menu_status_message_ = "Map transfer failed: checksum mismatch";
        main_menu_status_is_error_ = true;
        pending_client_map_transfer_.reset();
        return false;
    }

    if (!ReconstructTransferredMap(file_bytes, transfer.map_filename, transfer.transfer_id, &resolved_client_cached_map_path_)) {
        ReturnToMainMenu();
        main_menu_status_message_ = "Map transfer failed: could not reconstruct temp map";
        main_menu_status_is_error_ = true;
        pending_client_map_transfer_.reset();
        return false;
    }

    resolved_map_path_ = resolved_client_cached_map_path_;
    state_ = GameState{};
    if (!map_loader_.Load(resolved_map_path_, &objects_database_, state_.map)) {
        ReturnToMainMenu();
        main_menu_status_message_ = "Map transfer failed: received map could not be loaded";
        main_menu_status_is_error_ = true;
        pending_client_map_transfer_.reset();
        return false;
    }

    ClearInfluenceZoneVisuals();
    state_.match.match_running = true;
    state_.match.match_finished = false;
    state_.match.zone_enabled = lobby_zone_enabled_;
    state_.match.time_remaining = static_cast<float>(lobby_round_time_seconds_);
    state_.local_player_id = network_manager_.GetAssignedLocalPlayerId();
    pending_primary_pressed_ = false;
    pending_select_rune_slot_ = -1;
    pending_activate_item_slot_ = -1;
    pending_toggle_inventory_mode_ = false;
    pending_local_inventory_sync_.reset();
    pending_local_inventory_sync_dirty_ = false;
    local_inventory_ui_mode_ = InventoryUiMode::Closed;
    pending_open_initial_loadout_ui_ = true;
    in_game_menu_open_ = false;
    in_game_menu_page_ = InGameMenuPage::Home;
    render_player_positions_.clear();
    remote_position_samples_.clear();
    pending_local_prediction_inputs_.clear();
    pending_client_map_transfer_.reset();
    expected_match_transfer_id_ = 0;
    expected_match_map_key_.clear();
    app_screen_ = AppScreen::InMatch;
    return true;
}

bool GameApp::SendSelectedMapToClients(int transfer_id, const std::vector<uint8_t>& file_bytes) {
    const uint32_t chunk_bytes = static_cast<uint32_t>(std::max<size_t>(1, Constants::kMapTransferChunkBytes));
    const uint32_t chunk_count =
        static_cast<uint32_t>((file_bytes.size() + chunk_bytes - 1) / chunk_bytes);
    std::string map_filename = std::filesystem::path(resolved_host_selected_map_path_).filename().string();
    if (IsLayeredMapDirectoryPath(resolved_host_selected_map_path_)) {
        map_filename += ".mapbundle";
    }
    const uint32_t checksum = ComputeByteChecksum(file_bytes);

    network_manager_.BroadcastMapTransferBegin(
        MapTransferBeginMessage{transfer_id, lobby_selected_map_key_, map_filename, static_cast<uint32_t>(file_bytes.size()),
                                chunk_count, checksum});

    for (uint32_t chunk_index = 0; chunk_index < chunk_count; ++chunk_index) {
        const size_t offset = static_cast<size_t>(chunk_index) * chunk_bytes;
        const size_t length = std::min(static_cast<size_t>(chunk_bytes), file_bytes.size() - offset);
        MapTransferChunkMessage chunk;
        chunk.transfer_id = transfer_id;
        chunk.chunk_index = chunk_index;
        chunk.bytes.insert(chunk.bytes.end(), file_bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                           file_bytes.begin() + static_cast<std::ptrdiff_t>(offset + length));
        network_manager_.BroadcastMapTransferChunk(chunk);
    }

    network_manager_.BroadcastMapTransferComplete(MapTransferCompleteMessage{transfer_id, checksum});
    return true;
}

void GameApp::UpdateLoadingMatchMap(float /*dt*/) {
    if (escape_pressed_this_update_) {
        ReturnToMainMenu();
        return;
    }

    PumpMapTransferMessages();
    FinalizeClientTransferredMap();
}

void GameApp::UpdateLobby(float dt) {
    if (escape_pressed_this_update_) {
        ReturnToMainMenu();
        return;
    }

    PumpMapTransferMessages();

    if (network_manager_.IsHost()) {
        lobby_broadcast_accumulator_ += dt;

        lobby_player_names_.clear();
        lobby_player_names_.push_back(player_name_buffer_);
        known_player_names_[0] = player_name_buffer_;

        LobbyStateMessage lobby_state;
        lobby_state.host_can_start = true;
        lobby_state.allow_cheats = lobby_allow_cheats_;
        lobby_state.shrink_tiles_per_second = lobby_shrink_tiles_per_second_;
        lobby_state.min_arena_radius_tiles = lobby_min_arena_radius_tiles_;
        lobby_state.shrink_start_seconds = lobby_shrink_start_seconds_;
        lobby_state.zone_enabled = lobby_zone_enabled_;
        lobby_state.selected_map_key = lobby_selected_map_key_;
        lobby_state.selected_map_label = lobby_selected_map_label_;
        lobby_state.preview_generation = lobby_preview_generation_;
        lobby_state.preview_png_bytes = lobby_preview_png_bytes_;
        lobby_state.players.push_back({0, player_name_buffer_});

        for (const auto& remote : network_manager_.GetRemotePlayers()) {
            lobby_player_names_.push_back(remote.name);
            known_player_names_[remote.player_id] = remote.name;
            lobby_state.players.push_back({remote.player_id, remote.name});
        }

        if (lobby_broadcast_accumulator_ >= 0.2) {
            lobby_broadcast_accumulator_ = 0.0;
            lobby_state.mode_type = static_cast<int>(lobby_mode_type_);
            lobby_state.round_time_seconds = lobby_round_time_seconds_;
            lobby_state.best_of_target_kills = lobby_best_of_target_kills_;
            network_manager_.BroadcastLobbyState(lobby_state);
        }
    } else {
        const auto lobby_update = network_manager_.ConsumeLobbyState();
        if (lobby_update.has_value()) {
            lobby_player_names_.clear();
            for (const auto& player : lobby_update->players) {
                lobby_player_names_.push_back(player.name);
                known_player_names_[player.player_id] = player.name;
            }
            lobby_mode_type_ = static_cast<MatchModeType>(lobby_update->mode_type);
            lobby_round_time_seconds_ = lobby_update->round_time_seconds;
            lobby_best_of_target_kills_ = lobby_update->best_of_target_kills;
            lobby_zone_enabled_ = lobby_update->zone_enabled;
            lobby_allow_cheats_ = lobby_update->allow_cheats;
            lobby_shrink_tiles_per_second_ = lobby_update->shrink_tiles_per_second;
            lobby_shrink_start_seconds_ = lobby_update->shrink_start_seconds;
            lobby_min_arena_radius_tiles_ = lobby_update->min_arena_radius_tiles;
            lobby_selected_map_key_ = lobby_update->selected_map_key;
            lobby_selected_map_label_ = lobby_update->selected_map_label;
            if ((lobby_update->preview_generation != applied_lobby_preview_generation_ ||
                 lobby_update->selected_map_key != applied_lobby_preview_map_key_) &&
                !lobby_update->preview_png_bytes.empty()) {
                lobby_preview_png_bytes_ = lobby_update->preview_png_bytes;
                ApplyLobbyPreviewTextureFromPngBytes(lobby_preview_png_bytes_);
                applied_lobby_preview_generation_ = lobby_update->preview_generation;
                applied_lobby_preview_map_key_ = lobby_update->selected_map_key;
                lobby_preview_status_text_.clear();
            }
        }

        if (const auto match_start = network_manager_.ConsumeMatchStart(); match_start.has_value() && match_start->start) {
            BeginClientMatchMapLoading(*match_start);
        }
    }
}

void GameApp::UpdateMatch(float dt) {
    influence_zone_transition_elapsed_seconds_ =
        std::min(Constants::kInfluenceZoneTransitionSeconds, influence_zone_transition_elapsed_seconds_ + dt);
    DrainIncomingConsoleMessages();
    UpdateConsoleMessages(dt);
    UpdateChatInput();
    if (pending_open_initial_loadout_ui_ && state_.local_player_id >= 0) {
        OpenLocalInventoryUiForCurrentContext();
        pending_open_initial_loadout_ui_ = false;
    }

    if (escape_pressed_this_update_ && !chat_input_active_) {
        if (Player* local_player = FindPlayerById(state_.local_player_id);
            local_player != nullptr && local_player->inventory_mode) {
            CloseLocalInventoryUi();
        } else if (Player* local_player = FindPlayerById(state_.local_player_id);
                   local_player != nullptr && local_player->rune_placing_mode) {
            local_player->rune_placing_mode = false;
            if (local_player->action_state == PlayerActionState::RunePlacing) {
                local_player->action_state = PlayerActionState::Idle;
            }
        } else if (in_game_menu_open_) {
            if (in_game_menu_page_ == InGameMenuPage::Settings) {
                in_game_menu_page_ = InGameMenuPage::Home;
            } else {
                in_game_menu_open_ = false;
            }
        } else {
            CloseLocalInventoryUi();
            in_game_menu_open_ = true;
            in_game_menu_page_ = InGameMenuPage::Home;
        }
    }

    if (network_manager_.IsHost()) {
        HandleDisconnectedRemotePlayers();
        for (const auto& chat_submit : network_manager_.ConsumeHostChatSubmits()) {
            HandleHostChatSubmit(chat_submit);
        }
        const bool match_finished_before = state_.match.match_finished;
        SimulateHostGameplay(dt);
        MaybeBroadcastMatchCountdown();
        snapshot_accumulator_ += dt;
        const double snapshot_interval = static_cast<double>(Constants::kNetworkSnapshotIntervalSeconds);
        while (snapshot_accumulator_ >= snapshot_interval) {
            snapshot_accumulator_ -= snapshot_interval;
            network_manager_.BroadcastSnapshot(BuildHostSnapshot());
        }
        if (!match_finished_before && state_.match.match_finished) {
            network_manager_.BroadcastSnapshot(BuildHostSnapshot());
        }
        UpdateClientVisualSmoothing(dt);
    } else {
        ClientInputMessage local_input = BuildLocalInput(network_manager_.GetAssignedLocalPlayerId());
        local_input.local_dt = dt;
        ApplyClientLocalInputPreview(local_input, dt);

        ClientMoveMessage move_message;
        move_message.player_id = local_input.player_id;
        move_message.seq = local_input.seq;
        move_message.tick = local_input.tick;
        move_message.move_x = local_input.move_x;
        move_message.move_y = local_input.move_y;
        move_message.aim_x = local_input.aim_x;
        move_message.aim_y = local_input.aim_y;
        network_manager_.SendClientMove(move_message);

        if (local_input.primary_pressed || local_input.grappling_pressed ||
            local_input.request_rune_type != static_cast<int>(RuneType::None) || !local_input.request_item_id.empty() ||
            local_input.toggle_inventory_mode || pending_local_inventory_sync_dirty_ || pending_world_drop_request_) {
            ClientActionMessage action_message;
            action_message.player_id = local_input.player_id;
            action_message.seq = local_input.seq;
            action_message.primary_pressed = local_input.primary_pressed;
            action_message.grappling_pressed = local_input.grappling_pressed;
            action_message.request_rune_type = local_input.request_rune_type;
            action_message.request_item_id = local_input.request_item_id;
            action_message.toggle_inventory_mode = local_input.toggle_inventory_mode;
            if (pending_local_inventory_sync_dirty_ && pending_local_inventory_sync_.has_value()) {
                const Player& pending = *pending_local_inventory_sync_;
                action_message.has_inventory_layout_sync = true;
                action_message.selected_rune_slot = pending.selected_rune_slot;
                action_message.rune_slots.reserve(pending.rune_slots.size());
                for (const RuneType rune_type : pending.rune_slots) {
                    action_message.rune_slots.push_back(static_cast<int>(rune_type));
                }
                action_message.item_slots.assign(pending.item_slots.begin(), pending.item_slots.end());
                action_message.item_slot_counts.assign(pending.item_slot_counts.begin(), pending.item_slot_counts.end());
                action_message.item_slot_cooldown_remaining.assign(pending.item_slot_cooldown_remaining.begin(),
                                                                  pending.item_slot_cooldown_remaining.end());
                action_message.item_slot_cooldown_total.assign(pending.item_slot_cooldown_total.begin(),
                                                               pending.item_slot_cooldown_total.end());
                pending_local_inventory_sync_dirty_ = false;
            }
            if (pending_world_drop_request_) {
                action_message.request_world_drop = true;
                action_message.world_drop_slot_family = static_cast<int>(pending_world_drop_family_);
                action_message.world_drop_slot_index = pending_world_drop_slot_index_;
                action_message.world_drop_single_instance = pending_world_drop_single_instance_;
                pending_world_drop_request_ = false;
                pending_world_drop_slot_index_ = -1;
            }
            network_manager_.SendClientAction(action_message);
        }

        const auto snapshot = network_manager_.ConsumeLatestSnapshot();
        if (snapshot.has_value()) {
            ApplySnapshotToClientState(*snapshot);
            if (state_.match.match_finished) {
                winning_team_ = DetermineWinningTeam(state_.match);
                app_screen_ = AppScreen::PostMatch;
            }
        }
        UpdateGrapplingHooks(dt);
        UpdateFireSpirits(dt);
        UpdateFireWaveSegments(dt);
        UpdateEmbersTileModifiers(dt);
        UpdateClientVisualSmoothing(dt);
        UpdateDamagePopups(dt);
    }

    UpdateActiveModularAttackVisuals(dt);
    UpdateVolatileCastCasterFx(dt);
    UpdateDamageFlashVisuals(dt);
    UpdateProjectileEmitters();
    UpdateSnowParticleEmitters(dt);
    UpdateFireStormArcVisuals(dt);
    UpdateParticles(dt);
    UpdateHammerImpactEffects(dt);
    UpdateLightningEffects(dt);
    UpdateCompositeEffects(dt);
    UpdateFireStormCasts(dt);
    UpdateFireStormDummies(dt);
    camera_shake_time_remaining_ = std::max(0.0f, camera_shake_time_remaining_ - dt);
    UpdateCameraTarget();
}

void GameApp::UpdatePostMatch(float dt) {
    DrainIncomingConsoleMessages();
    UpdateConsoleMessages(dt);
    if (network_manager_.IsHost()) {
        HandleDisconnectedRemotePlayers();
        for (const auto& chat_submit : network_manager_.ConsumeHostChatSubmits()) {
            HandleHostChatSubmit(chat_submit);
        }
    }
    if (escape_pressed_this_update_) {
        ReturnToMainMenu();
        return;
    }
    const Rectangle leave_button = {static_cast<float>(GetScreenWidth() / 2 - 80), static_cast<float>(GetScreenHeight() - 92),
                                    160.0f, 42.0f};
    if ((enter_pressed_this_update_ || (CheckCollisionPointRec(GetMousePosition(), leave_button) &&
                                        IsMouseButtonPressed(MOUSE_LEFT_BUTTON)))) {
        ReturnToMainMenu();
    }
}

void GameApp::UpdateClientVisualSmoothing(float dt) {
    (void)dt;
    if (state_.players.empty()) {
        render_player_positions_.clear();
        render_grappling_hook_head_positions_.clear();
        return;
    }

    std::unordered_map<int, Vector2> updated_positions;
    updated_positions.reserve(state_.players.size());

    if (network_manager_.IsHost()) {
        for (const auto& player : state_.players) {
            updated_positions[player.id] = player.pos;
        }
        render_player_positions_.swap(updated_positions);
        render_grappling_hook_head_positions_.clear();
        return;
    }

    const double target_time = GetTime() - static_cast<double>(Constants::kRemoteInterpolationDelaySeconds);
    for (const auto& player : state_.players) {
        if (player.id == state_.local_player_id) {
            updated_positions[player.id] = player.pos;
            continue;
        }

        auto samples_it = remote_position_samples_.find(player.id);
        if (samples_it == remote_position_samples_.end() || samples_it->second.empty()) {
            updated_positions[player.id] = player.pos;
            continue;
        }

        auto& samples = samples_it->second;
        while (samples.size() > 2 && samples[1].time_seconds <= target_time) {
            samples.pop_front();
        }

        if (samples.size() >= 2 && samples.front().time_seconds <= target_time &&
            samples[1].time_seconds >= target_time) {
            const double t0 = samples.front().time_seconds;
            const double t1 = samples[1].time_seconds;
            const float alpha =
                static_cast<float>(std::clamp((target_time - t0) / std::max(0.00001, t1 - t0), 0.0, 1.0));
            updated_positions[player.id] = Vector2Lerp(samples.front().pos, samples[1].pos, alpha);
        } else {
            updated_positions[player.id] = samples.back().pos;
        }
    }
    render_player_positions_.swap(updated_positions);

    std::unordered_map<int, Vector2> updated_hook_positions;
    updated_hook_positions.reserve(state_.grappling_hooks.size());
    for (const auto& hook : state_.grappling_hooks) {
        auto samples_it = grappling_head_position_samples_.find(hook.id);
        if (samples_it == grappling_head_position_samples_.end() || samples_it->second.empty()) {
            updated_hook_positions[hook.id] = hook.head_pos;
            continue;
        }

        auto& samples = samples_it->second;
        while (samples.size() > 2 && samples[1].time_seconds <= target_time) {
            samples.pop_front();
        }

        if (samples.size() >= 2 && samples.front().time_seconds <= target_time &&
            samples[1].time_seconds >= target_time) {
            const double t0 = samples.front().time_seconds;
            const double t1 = samples[1].time_seconds;
            const float alpha =
                static_cast<float>(std::clamp((target_time - t0) / std::max(0.00001, t1 - t0), 0.0, 1.0));
            updated_hook_positions[hook.id] = Vector2Lerp(samples.front().pos, samples[1].pos, alpha);
        } else {
            updated_hook_positions[hook.id] = samples.back().pos;
        }
    }
    render_grappling_hook_head_positions_.swap(updated_hook_positions);
}

void GameApp::UpdateDamageFlashVisuals(float dt) {
    const float max_flash = Constants::kDamageFlashDurationSeconds;
    for (auto& [id, remaining] : player_damage_flash_remaining_) {
        remaining = std::max(0.0f, remaining - dt);
    }
    for (auto& [id, remaining] : object_damage_flash_remaining_) {
        remaining = std::max(0.0f, remaining - dt);
    }

    std::unordered_set<int> seen_player_ids;
    seen_player_ids.reserve(state_.players.size());
    for (const Player& player : state_.players) {
        seen_player_ids.insert(player.id);
        auto previous_it = previous_player_hp_.find(player.id);
        if (previous_it != previous_player_hp_.end() && player.hp < previous_it->second) {
            player_damage_flash_remaining_[player.id] = max_flash;
        }
        previous_player_hp_[player.id] = player.hp;
    }
    for (auto it = previous_player_hp_.begin(); it != previous_player_hp_.end();) {
        if (seen_player_ids.count(it->first) == 0) {
            it = previous_player_hp_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = player_damage_flash_remaining_.begin(); it != player_damage_flash_remaining_.end();) {
        if (seen_player_ids.count(it->first) == 0 || it->second <= 0.0f) {
            it = player_damage_flash_remaining_.erase(it);
        } else {
            ++it;
        }
    }

    std::unordered_set<int> seen_object_ids;
    seen_object_ids.reserve(state_.map_objects.size());
    for (const MapObjectInstance& object : state_.map_objects) {
        const ObjectPrototype* proto = FindObjectPrototype(object.prototype_id);
        if (proto == nullptr || (proto->type != ObjectType::Destructible && proto->type != ObjectType::Unit)) {
            continue;
        }
        seen_object_ids.insert(object.id);
        auto previous_it = previous_object_hp_.find(object.id);
        if (previous_it != previous_object_hp_.end() && object.hp < previous_it->second) {
            object_damage_flash_remaining_[object.id] = max_flash;
        }
        previous_object_hp_[object.id] = object.hp;
    }
    for (auto it = previous_object_hp_.begin(); it != previous_object_hp_.end();) {
        if (seen_object_ids.count(it->first) == 0) {
            it = previous_object_hp_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = object_damage_flash_remaining_.begin(); it != object_damage_flash_remaining_.end();) {
        if (seen_object_ids.count(it->first) == 0 || it->second <= 0.0f) {
            it = object_damage_flash_remaining_.erase(it);
        } else {
            ++it;
        }
    }
}

void GameApp::ApplyClientLocalInputPreview(const ClientInputMessage& input, float dt) {
    if (network_manager_.IsHost()) {
        return;
    }
    if (state_.local_player_id < 0) {
        return;
    }

    Player* local_player = FindPlayerById(state_.local_player_id);
    if (!local_player || !local_player->alive) {
        return;
    }
    const ActionIntent intent = BuildActionIntent(input);
    const bool is_stunned = HasStatusEffect(*local_player, StatusEffectType::Stunned);
    const bool is_pulled = IsPlayerBeingPulled(local_player->id);
    bool inventory_editing = local_player->inventory_mode;

    local_player->grappling_cooldown_remaining = std::max(0.0f, local_player->grappling_cooldown_remaining - dt);
    local_player->mana =
        std::clamp(local_player->mana + local_player->mana_regen_per_second * dt, 0.0f, local_player->max_mana);
    for (size_t i = 0; i < local_player->rune_cooldown_remaining.size(); ++i) {
        local_player->rune_cooldown_remaining[i] = std::max(0.0f, local_player->rune_cooldown_remaining[i] - dt);
    }

    if (!is_stunned && !is_pulled && intent.toggle_inventory_mode) {
        if (local_player->inventory_mode) {
            CloseLocalInventoryUi();
        } else {
            OpenLocalInventoryUiForCurrentContext();
        }
        inventory_editing = local_player->inventory_mode;
    }
    if (!is_stunned && !is_pulled && !inventory_editing && intent.request_rune_type != static_cast<int>(RuneType::None)) {
        const RuneType rune_type = static_cast<RuneType>(intent.request_rune_type);
        if (IsRuneAvailableToPlayer(*local_player, rune_type)) {
            const float mana_cost = GetRuneManaCost(rune_type);
            for (size_t i = 0; i < local_player->rune_slots.size(); ++i) {
                if (local_player->rune_slots[i] == rune_type) {
                    local_player->selected_rune_slot = static_cast<int>(i);
                    break;
                }
            }
            local_player->selected_rune_type = rune_type;
            local_player->rune_placing_mode = (rune_type != RuneType::None) &&
                                              (GetPlayerRuneCooldownRemaining(*local_player, rune_type) <= 0.0f) &&
                                              (local_player->mana >= mana_cost);
        } else {
            local_player->rune_placing_mode = false;
        }
    }

    Vector2 aim_vector = {intent.aim_world.x - local_player->pos.x, intent.aim_world.y - local_player->pos.y};
    if (local_player->melee_active_remaining <= 0.0f && Vector2LengthSqr(aim_vector) > 0.0001f) {
        local_player->aim_dir = Vector2Normalize(aim_vector);
        local_player->facing = AimToFacing(local_player->aim_dir);
    }

    if (!is_stunned && !is_pulled && !inventory_editing && intent.primary_pressed && local_player->rune_placing_mode) {
        local_player->rune_placing_mode = false;
    }
    const MobilityProfile* mobility_profile = GetEquippedMobility(*local_player);
    if (!is_stunned && !is_pulled && !inventory_editing && intent.mobility_pressed && !local_player->rune_placing_mode &&
        mobility_profile != nullptr && mobility_profile->kind == EquipmentActionKind::GrapplingHook &&
        local_player->grappling_cooldown_remaining <= 0.0f) {
        TryStartGrapplingHook(*local_player, intent.aim_world, false);
    }

    // Movement/facing prediction for client feel; authoritative state is reconciled on snapshots.
    Vector2 movement = intent.move;
    if (Vector2LengthSqr(movement) > 0.0001f) {
        movement = Vector2Normalize(movement);
    }

    const float movement_multiplier = GetPlayerMovementSpeedMultiplier(*local_player);
    const float base_acceleration = GetPlayerBaseAcceleration(*local_player);
    const float acceleration_multiplier = GetPlayerAccelerationMultiplier(*local_player);
    const Vector2 acceleration = (is_stunned || is_pulled || inventory_editing)
                                     ? Vector2{0.0f, 0.0f}
                                     : Vector2Scale(movement, base_acceleration * movement_multiplier *
                                                                  acceleration_multiplier);
    local_player->vel = Vector2Add(local_player->vel, Vector2Scale(acceleration, dt));
    const float damping = std::max(0.0f, 1.0f - Constants::kPlayerFriction * dt);
    local_player->vel = Vector2Scale(local_player->vel, damping);
    const float speed = Vector2Length(local_player->vel);
    if (speed > Constants::kPlayerMaxSpeed * movement_multiplier) {
        local_player->vel =
            Vector2Scale(Vector2Normalize(local_player->vel), Constants::kPlayerMaxSpeed * movement_multiplier);
    }
    local_player->pos = Vector2Add(local_player->pos, Vector2Scale(local_player->vel, dt));
    CollisionWorld::ResolvePlayerVsWorldLocal(state_.map, *local_player, !is_pulled);
    ResolvePlayerVsMapObjects(*local_player);
    ResolvePlayerVsIceWallsLocal(*local_player);

    if (is_stunned || is_pulled || inventory_editing) {
        local_player->action_state = PlayerActionState::Idle;
    } else if (local_player->melee_active_remaining > 0.0f) {
        local_player->action_state = PlayerActionState::Slashing;
    } else if (local_player->rune_placing_mode) {
        local_player->action_state = PlayerActionState::RunePlacing;
    } else if (Vector2LengthSqr(local_player->vel) > 8.0f) {
        local_player->action_state = PlayerActionState::Walking;
    } else {
        local_player->action_state = PlayerActionState::Idle;
    }

    pending_local_prediction_inputs_.push_back(input);
    while (pending_local_prediction_inputs_.size() > static_cast<size_t>(Constants::kMoveInputBufferSize)) {
        pending_local_prediction_inputs_.pop_front();
    }
}

void GameApp::Render() {
    BeginDrawing();
    ClearBackground(Color{16, 18, 22, 255});

    switch (app_screen_) {
        case AppScreen::MainMenu: {
            if (has_menu_background_texture_) {
                Rectangle src = {0.0f, 0.0f, static_cast<float>(menu_background_texture_.width),
                                 static_cast<float>(menu_background_texture_.height)};
                Rectangle dst = {0.0f, 0.0f, static_cast<float>(GetScreenWidth()), static_cast<float>(GetScreenHeight())};
                DrawTexturePro(menu_background_texture_, src, dst, {0.0f, 0.0f}, 0.0f, WHITE);
            }

            const MainMenuUiResult ui_result =
                DrawMainMenu(player_name_buffer_, sizeof(player_name_buffer_), join_ip_buffer_, sizeof(join_ip_buffer_),
                             discovery_service_->GetHosts(), config_manager_.GetConfigPath(), controls_bindings_,
                             controls_manager_.GetControlsPath(), show_network_debug_panel_,
                             settings_.auto_pick_replace_equipment,
                             settings_.hide_own_influence_zones, settings_.enable_influence_zone_system,
                             main_menu_status_message_, main_menu_status_is_error_);

            if (ui_result.settings_changed) {
                const bool influence_setting_changed =
                    settings_.enable_influence_zone_system != ui_result.enable_influence_zone_system;
                show_network_debug_panel_ = ui_result.show_network_debug_panel;
                settings_.auto_pick_replace_equipment = ui_result.auto_pick_replace_equipment;
                settings_.hide_own_influence_zones = ui_result.hide_own_influence_zones;
                settings_.enable_influence_zone_system = ui_result.enable_influence_zone_system;
                settings_.show_network_debug_panel = show_network_debug_panel_;
                if (influence_setting_changed) {
                    if (settings_.enable_influence_zone_system) {
                        RebuildInfluenceZones();
                    } else {
                        ClearInfluenceZoneVisuals();
                    }
                }
                config_manager_.Save(settings_);
            }

            if (ui_result.request_host) {
                StartAsHost();
            }
            if (ui_result.request_exit_app) {
                request_app_exit_ = true;
            }
            if (ui_result.request_join) {
                const int join_port =
                    ui_result.selected_host_port > 0 ? ui_result.selected_host_port : Constants::kDefaultPort;
                StartAsClient(ui_result.selected_host_ip, join_port);
            }
            if (ui_result.request_apply_controls) {
                controls_bindings_ = ui_result.controls_bindings;
                if (controls_manager_.Save(controls_bindings_)) {
                    main_menu_status_message_ =
                        TextFormat("Controls saved: %s", controls_manager_.GetControlsPath().c_str());
                    main_menu_status_is_error_ = false;
                } else {
                    main_menu_status_message_ = "Failed to save controls.json";
                    main_menu_status_is_error_ = true;
                }
            }
            break;
        }

        case AppScreen::Connecting: {
            const std::string lobby_mode_name =
                (lobby_mode_type_ == MatchModeType::BestOfKills) ? "Best Of" : most_kills_mode_.GetUiName();
            const LobbyUiResult lobby_ui =
            DrawLobby(lobby_player_names_, false, host_display_ip_, static_cast<int>(lobby_mode_type_),
                          lobby_round_time_seconds_, lobby_best_of_target_kills_, lobby_zone_enabled_, lobby_allow_cheats_,
                          lobby_shrink_tiles_per_second_, lobby_shrink_start_seconds_, lobby_min_arena_radius_tiles_,
                          lobby_mode_name, GetClientLobbyStatusText(), GetSelectedLobbyMapLabel(), BuildLobbyMapOptionsText(),
                          lobby_selected_map_index_, lobby_map_dropdown_edit_mode_, &lobby_map_preview_texture_,
                          has_lobby_map_preview_texture_, lobby_preview_status_text_);
            if (lobby_ui.request_leave) {
                ReturnToMainMenu();
            }
            break;
        }

        case AppScreen::Lobby: {
            const std::string lobby_mode_name =
                (lobby_mode_type_ == MatchModeType::BestOfKills) ? "Best Of" : most_kills_mode_.GetUiName();
            const LobbyUiResult lobby_ui = DrawLobby(lobby_player_names_, network_manager_.IsHost(), host_display_ip_,
                                                     static_cast<int>(lobby_mode_type_), lobby_round_time_seconds_,
                                                     lobby_best_of_target_kills_, lobby_zone_enabled_, lobby_allow_cheats_,
                                                     lobby_shrink_tiles_per_second_, lobby_shrink_start_seconds_,
                                                     lobby_min_arena_radius_tiles_,
                                                     lobby_mode_name,
                                                     network_manager_.IsHost() ? "hosting/listening"
                                                                              : GetClientLobbyStatusText(),
                                                     GetSelectedLobbyMapLabel(), BuildLobbyMapOptionsText(),
                                                     lobby_selected_map_index_, lobby_map_dropdown_edit_mode_,
                                                     &lobby_map_preview_texture_, has_lobby_map_preview_texture_,
                                                     lobby_preview_status_text_);
            if (lobby_ui.request_leave) {
                ReturnToMainMenu();
            }
            lobby_map_dropdown_edit_mode_ = lobby_ui.map_dropdown_edit_mode;
            bool mode_settings_changed = false;
            if (network_manager_.IsHost() && lobby_ui.selected_map_changed &&
                lobby_ui.selected_map_index != lobby_selected_map_index_) {
                SetSelectedLobbyMapIndex(lobby_ui.selected_map_index);
            }
            if (network_manager_.IsHost() && lobby_ui.request_toggle_mode_type) {
                lobby_mode_type_ = (lobby_mode_type_ == MatchModeType::MostKillsTimed) ? MatchModeType::BestOfKills
                                                                                       : MatchModeType::MostKillsTimed;
                mode_settings_changed = true;
            }
            if (network_manager_.IsHost() && lobby_ui.request_toggle_zone_enabled) {
                lobby_zone_enabled_ = !lobby_zone_enabled_;
                mode_settings_changed = true;
            }
            if (network_manager_.IsHost() && lobby_ui.request_toggle_allow_cheats) {
                lobby_allow_cheats_ = !lobby_allow_cheats_;
            }
            if (network_manager_.IsHost() && lobby_ui.request_decrease_round_time) {
                lobby_round_time_seconds_ =
                    std::max(Constants::kLobbyTimeStepSeconds,
                             lobby_round_time_seconds_ - Constants::kLobbyTimeStepSeconds);
                lobby_shrink_start_seconds_ = std::min(lobby_shrink_start_seconds_, static_cast<float>(lobby_round_time_seconds_));
                mode_settings_changed = true;
            }
            if (network_manager_.IsHost() && lobby_ui.request_increase_round_time) {
                lobby_round_time_seconds_ += Constants::kLobbyTimeStepSeconds;
                mode_settings_changed = true;
            }
            if (network_manager_.IsHost() && lobby_ui.request_decrease_best_of) {
                lobby_best_of_target_kills_ = std::max(1, lobby_best_of_target_kills_ - 2);
                if ((lobby_best_of_target_kills_ % 2) == 0) lobby_best_of_target_kills_ -= 1;
                mode_settings_changed = true;
            }
            if (network_manager_.IsHost() && lobby_ui.request_increase_best_of) {
                lobby_best_of_target_kills_ += 2;
                if ((lobby_best_of_target_kills_ % 2) == 0) lobby_best_of_target_kills_ += 1;
                mode_settings_changed = true;
            }
            if (network_manager_.IsHost() && lobby_ui.request_decrease_shrink_rate) {
                lobby_shrink_tiles_per_second_ = std::max(
                    0.0f, lobby_shrink_tiles_per_second_ - Constants::kShrinkTilesPerSecondStep);
                mode_settings_changed = true;
            }
            if (network_manager_.IsHost() && lobby_ui.request_increase_shrink_rate) {
                lobby_shrink_tiles_per_second_ =
                    std::min(Constants::kMaxShrinkTilesPerSecond,
                             lobby_shrink_tiles_per_second_ + Constants::kShrinkTilesPerSecondStep);
                mode_settings_changed = true;
            }
            if (network_manager_.IsHost() && lobby_ui.request_decrease_shrink_start) {
                lobby_shrink_start_seconds_ =
                    std::max(0.0f, lobby_shrink_start_seconds_ - Constants::kLobbyTimeStepSeconds);
                mode_settings_changed = true;
            }
            if (network_manager_.IsHost() && lobby_ui.request_increase_shrink_start) {
                lobby_shrink_start_seconds_ += Constants::kLobbyTimeStepSeconds;
                if (lobby_mode_type_ == MatchModeType::MostKillsTimed) {
                    lobby_shrink_start_seconds_ =
                        std::min(lobby_shrink_start_seconds_, static_cast<float>(lobby_round_time_seconds_));
                }
                mode_settings_changed = true;
            }
            if (network_manager_.IsHost() && lobby_ui.request_decrease_min_radius) {
                lobby_min_arena_radius_tiles_ = std::max(
                    0.0f, lobby_min_arena_radius_tiles_ - Constants::kMinArenaRadiusTilesStep);
                mode_settings_changed = true;
            }
            if (network_manager_.IsHost() && lobby_ui.request_increase_min_radius) {
                lobby_min_arena_radius_tiles_ = std::min(
                    Constants::kMaxArenaRadiusTiles, lobby_min_arena_radius_tiles_ + Constants::kMinArenaRadiusTilesStep);
                mode_settings_changed = true;
            }
            if (network_manager_.IsHost() && mode_settings_changed) {
                settings_.lobby_mode_type = static_cast<int>(lobby_mode_type_);
                settings_.lobby_match_length_seconds = lobby_round_time_seconds_;
                settings_.lobby_best_of_target_kills = lobby_best_of_target_kills_;
                settings_.lobby_zone_enabled = lobby_zone_enabled_;
                settings_.lobby_shrink_tiles_per_second = lobby_shrink_tiles_per_second_;
                settings_.lobby_shrink_start_seconds = lobby_shrink_start_seconds_;
                settings_.lobby_min_arena_radius_tiles = lobby_min_arena_radius_tiles_;
                config_manager_.Save(settings_);
            }
            if (lobby_ui.request_start_match && network_manager_.IsHost()) {
                StartMatchAsHost();
            }
            break;
        }

        case AppScreen::LoadingMatchMap:
            RenderMapLoadingScreen();
            break;

        case AppScreen::InMatch:
            RenderWorld();
            if (const Player* local_player = FindPlayerById(state_.local_player_id);
                local_player != nullptr && local_player->inventory_mode) {
                DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Color{0, 0, 0, 115});
                const char* text = "Inventory mode (Press tab to toggle off/on)";
                const int font_size = 20;
                const int text_width = MeasureText(text, font_size);
                DrawText(text, (GetScreenWidth() - text_width) / 2, GetScreenHeight() / 2 - font_size / 2, font_size,
                         RAYWHITE);
            }
            DrawMatchHud(state_, state_.local_player_id);
            RenderBottomHud();
            RenderConsoleLog();
            RenderChatInput();
            RenderFpsCounter();
            RenderNetworkDebugPanel();
            RenderInGameMenu();
            break;

        case AppScreen::PostMatch:
            DrawPostMatch(state_, winning_team_);
            RenderConsoleLog();
            break;
    }

    EndDrawing();
}

void GameApp::RenderMapLoadingScreen() const {
    const int panel_w = 560;
    const int panel_h = 360;
    const int panel_x = (GetScreenWidth() - panel_w) / 2;
    const int panel_y = (GetScreenHeight() - panel_h) / 2;

    DrawRectangle(panel_x, panel_y, panel_w, panel_h, Color{20, 23, 30, 255});
    DrawRectangleLines(panel_x, panel_y, panel_w, panel_h, Color{75, 82, 95, 255});
    DrawText("Loading Match Map", panel_x + 24, panel_y + 18, 30, RAYWHITE);
    DrawText(TextFormat("Map: %s", lobby_selected_map_label_.c_str()), panel_x + 24, panel_y + 64, 20,
             Color{196, 205, 228, 255});
    DrawText(map_loading_status_text_.c_str(), panel_x + 24, panel_y + 92, 18, Color{168, 220, 188, 255});

    const Rectangle preview_bounds = {static_cast<float>(panel_x + 24), static_cast<float>(panel_y + 126), 220.0f, 220.0f};
    DrawRectangleRec(preview_bounds, Color{28, 31, 39, 255});
    DrawRectangleLinesEx(preview_bounds, 1.0f, Color{75, 82, 95, 255});
    if (has_lobby_map_preview_texture_ && lobby_map_preview_texture_.id != 0) {
        const float texture_w = static_cast<float>(lobby_map_preview_texture_.width);
        const float texture_h = static_cast<float>(lobby_map_preview_texture_.height);
        const float scale =
            std::min(preview_bounds.width / std::max(1.0f, texture_w), preview_bounds.height / std::max(1.0f, texture_h));
        const Rectangle src = {0.0f, 0.0f, texture_w, texture_h};
        const Rectangle dst = {preview_bounds.x + (preview_bounds.width - texture_w * scale) * 0.5f,
                               preview_bounds.y + (preview_bounds.height - texture_h * scale) * 0.5f, texture_w * scale,
                               texture_h * scale};
        DrawTexturePro(lobby_map_preview_texture_, src, dst, {0.0f, 0.0f}, 0.0f, WHITE);
    }

    DrawText("Please wait until the transfer and local map load complete.", panel_x + 268, panel_y + 164, 18,
             Color{195, 200, 214, 255});
    DrawText("Press Esc to cancel and return to the menu.", panel_x + 268, panel_y + 196, 18,
             Color{195, 200, 214, 255});
}

void GameApp::StartAsHost() {
    std::printf("[UI] Host Game pressed\n");

    std::string saved_name(player_name_buffer_);
    if (!saved_name.empty()) {
        settings_.player_name = saved_name;
        config_manager_.Save(settings_);
    }

    network_manager_.SetLocalPlayerName(settings_.player_name);
    if (!network_manager_.StartHost(Constants::kDefaultPort)) {
        main_menu_status_message_ = "Host start failed: " + network_manager_.GetLastDebugMessage();
        main_menu_status_is_error_ = true;
        std::printf("[UI] %s\n", main_menu_status_message_.c_str());
        return;
    }

    main_menu_status_message_ = TextFormat("Host started on UDP %d", Constants::kDefaultPort);
    main_menu_status_is_error_ = false;

    discovery_service_->Stop();
    if (!discovery_service_->StartHostBroadcaster(settings_.player_name, Constants::kDefaultPort)) {
        std::printf("[UI] Warning: LAN discovery broadcaster failed to start\n");
    }

    host_display_ip_ = TextFormat("%s:%d", discovery_service_->GetHostLocalIp().c_str(), Constants::kDefaultPort);
    lobby_player_names_.clear();
    lobby_player_names_.push_back(settings_.player_name);
    render_player_positions_.clear();
    remote_position_samples_.clear();
    pending_local_prediction_inputs_.clear();
    player_attached_animations_.clear();
    storm_arc_visuals_.clear();
    fire_storm_cast_impact_played_.clear();
    fire_storm_cast_arcs_spawned_.clear();
    fire_storm_cast_conversion_sfx_triggered_.clear();
    known_player_names_.clear();
    known_player_names_[0] = settings_.player_name;
    lobby_allow_cheats_ = false;
    lobby_broadcast_accumulator_ = 0.0;
    snapshot_accumulator_ = 0.0;
    connect_attempt_start_seconds_ = 0.0;
    player_attached_animations_.clear();
    pending_local_inventory_sync_.reset();
    pending_local_inventory_sync_dirty_ = false;
    pending_client_map_transfer_.reset();
    expected_match_transfer_id_ = 0;
    expected_match_map_key_.clear();
    resolved_client_cached_map_path_.clear();
    RefreshLobbyMapCatalog();
    SetSelectedLobbyMapIndex(lobby_selected_map_index_);

    app_screen_ = AppScreen::Lobby;
}

void GameApp::StartAsClient(const std::string& ip, int port) {
    std::string saved_name(player_name_buffer_);
    if (!saved_name.empty()) {
        settings_.player_name = saved_name;
        config_manager_.Save(settings_);
    }

    network_manager_.SetLocalPlayerName(settings_.player_name);
    if (!network_manager_.StartClient()) {
        main_menu_status_message_ = "Client start failed: " + network_manager_.GetLastDebugMessage();
        main_menu_status_is_error_ = true;
        return;
    }

    if (!network_manager_.ConnectToHost(ip, port)) {
        main_menu_status_message_ = "Connect failed: " + network_manager_.GetLastDebugMessage();
        main_menu_status_is_error_ = true;
        network_manager_.Stop();
        return;
    }

    main_menu_status_message_.clear();
    main_menu_status_is_error_ = false;

    host_display_ip_ = TextFormat("%s:%d", ip.c_str(), port);
    lobby_player_names_.clear();
    render_player_positions_.clear();
    remote_position_samples_.clear();
    pending_local_prediction_inputs_.clear();
    lobby_broadcast_accumulator_ = 0.0;
    snapshot_accumulator_ = 0.0;
    connect_attempt_start_seconds_ = GetTime();
    player_attached_animations_.clear();
    console_entries_.clear();
    chat_input_active_ = false;
    chat_input_buffer_.clear();
    pending_local_inventory_sync_.reset();
    pending_local_inventory_sync_dirty_ = false;
    pending_client_map_transfer_.reset();
    expected_match_transfer_id_ = 0;
    expected_match_map_key_.clear();
    app_screen_ = AppScreen::Connecting;
}

void GameApp::ReturnToMainMenu() {
    network_manager_.Stop();
    discovery_service_->Stop();
    if (!discovery_service_->StartClientListener()) {
        main_menu_status_message_ = TextFormat("Discovery listener failed on UDP %d", Constants::kDiscoveryPort);
        main_menu_status_is_error_ = true;
    }

    state_ = GameState{};
    ClearInfluenceZoneVisuals();
    resolved_map_path_ = resolved_default_map_path_;
    resolved_client_cached_map_path_.clear();
    LoadMapOrFallback(resolved_map_path_);

    event_queue_.Clear();
    latest_remote_inputs_.clear();
    render_player_positions_.clear();
    remote_position_samples_.clear();
    pending_local_prediction_inputs_.clear();
    player_attached_animations_.clear();
    console_entries_.clear();
    chat_input_active_ = false;
    chat_input_buffer_.clear();
    known_player_names_.clear();
    pending_primary_pressed_ = false;
    pending_select_rune_slot_ = -1;
    pending_activate_item_slot_ = -1;
    pending_toggle_inventory_mode_ = false;
    pending_local_inventory_sync_.reset();
    pending_local_inventory_sync_dirty_ = false;
    pending_object_spawns_.clear();
    dropped_item_pickup_blocks_.clear();
    loot_quota_remaining_.clear();
    altars_.clear();
    lobby_broadcast_accumulator_ = 0.0;
    snapshot_accumulator_ = 0.0;
    connect_attempt_start_seconds_ = 0.0;
    pending_client_map_transfer_.reset();
    expected_match_transfer_id_ = 0;
    expected_match_map_key_.clear();
    winning_team_ = -1;
    lobby_allow_cheats_ = false;
    last_match_countdown_announcement_seconds_ = 11;
    host_server_tick_ = 0;
    local_input_seq_ = 0;
    main_menu_status_message_.clear();
    main_menu_status_is_error_ = false;
    lobby_shrink_tiles_per_second_ = settings_.lobby_shrink_tiles_per_second;
    lobby_mode_type_ = static_cast<MatchModeType>(settings_.lobby_mode_type);
    lobby_round_time_seconds_ = settings_.lobby_match_length_seconds;
    lobby_best_of_target_kills_ = settings_.lobby_best_of_target_kills;
    lobby_zone_enabled_ = settings_.lobby_zone_enabled;
    lobby_shrink_start_seconds_ = settings_.lobby_shrink_start_seconds;
    lobby_min_arena_radius_tiles_ = settings_.lobby_min_arena_radius_tiles;
    in_game_menu_open_ = false;
    in_game_menu_page_ = InGameMenuPage::Home;
    lobby_selected_map_key_.clear();
    lobby_selected_map_label_.clear();
    RefreshLobbyMapCatalog();
    SetSelectedLobbyMapIndex(lobby_selected_map_index_);
    app_screen_ = AppScreen::MainMenu;
}

void GameApp::StartMatchAsHost() {
    state_ = GameState{};
    if (!map_loader_.Load(resolved_host_selected_map_path_, &objects_database_, state_.map)) {
        lobby_preview_status_text_ = "Selected map failed to load";
        return;
    }
    resolved_map_path_ = resolved_host_selected_map_path_;

    const int transfer_id = next_map_transfer_id_++;
    std::vector<uint8_t> map_transfer_precheck;
    std::string transfer_filename;
    if (!BuildMapTransferPayload(resolved_host_selected_map_path_, &transfer_filename, &map_transfer_precheck)) {
        lobby_preview_status_text_ = "Selected map failed to transfer";
        return;
    }

    ClearInfluenceZoneVisuals();
    state_.match = MatchState{};
    state_.match.mode_type = lobby_mode_type_;
    state_.match.round_time_seconds = lobby_round_time_seconds_;
    state_.match.match_running = true;
    state_.match.match_finished = false;
    state_.match.time_remaining = static_cast<float>(lobby_round_time_seconds_);
    state_.match.best_of_target_kills = lobby_best_of_target_kills_;
    state_.match.zone_enabled = lobby_zone_enabled_;
    state_.match.shrink_tiles_per_second = lobby_shrink_tiles_per_second_;
    state_.match.shrink_start_seconds = lobby_shrink_start_seconds_;
    state_.match.arena_center_world = {static_cast<float>(state_.map.width * state_.map.cell_size) * 0.5f,
                                       static_cast<float>(state_.map.height * state_.map.cell_size) * 0.5f};
    const float map_diameter_tiles = static_cast<float>(std::max(state_.map.width, state_.map.height));
    const float map_radius_tiles = map_diameter_tiles * 0.5f;
    state_.match.min_arena_radius_tiles = std::clamp(lobby_min_arena_radius_tiles_, 0.0f, map_radius_tiles);
    state_.match.arena_radius_tiles = map_radius_tiles;
    state_.match.arena_radius_world = state_.match.arena_radius_tiles * static_cast<float>(state_.map.cell_size);
    state_.match.kill_timeline.clear();
    state_.match.kill_timeline.push_back({0.0f, 0, 0});

    state_.players.clear();
    state_.runes.clear();
    state_.projectiles.clear();
    state_.fire_spirits.clear();
    state_.explosions.clear();
    state_.lightning_effects.clear();
    state_.ice_walls.clear();
    state_.fire_wave_segments.clear();
    state_.embers_tile_modifiers.clear();
    state_.map_objects.clear();
    map_object_index_by_id_.clear();
    state_.particles.clear();
    state_.castles.clear();
    state_.next_entity_id = 1;
    state_.next_rune_placement_order = 1;
    latest_remote_inputs_.clear();
    render_player_positions_.clear();
    remote_position_samples_.clear();
    pending_local_prediction_inputs_.clear();
    player_attached_animations_.clear();
    fire_wave_damage_ledgers_.clear();
    fire_wave_ember_activation_ledgers_.clear();
    fire_flower_runtime_states_.clear();
    console_entries_.clear();
    chat_input_active_ = false;
    chat_input_buffer_.clear();
    last_match_countdown_announcement_seconds_ = 11;
    host_server_tick_ = 0;
    snapshot_accumulator_ = 0.0;
    pending_primary_pressed_ = false;
    pending_select_rune_slot_ = -1;
    pending_activate_item_slot_ = -1;
    pending_toggle_inventory_mode_ = false;
    pending_local_inventory_sync_.reset();
    pending_local_inventory_sync_dirty_ = false;
    local_inventory_ui_mode_ = InventoryUiMode::Closed;
    pending_open_initial_loadout_ui_ = false;
    pending_object_spawns_.clear();
    dropped_item_pickup_blocks_.clear();
    castle_charge_lightning_effect_ids_.clear();

    Player host_player;
    host_player.id = 0;
    host_player.name = settings_.player_name;
    host_player.team = Constants::kTeamRed;
    state_.players.push_back(host_player);

    for (const auto& remote : network_manager_.GetRemotePlayers()) {
        Player player;
        player.id = remote.player_id;
        player.name = remote.name;
        player.team = (static_cast<int>(state_.players.size()) % 2 == 0) ? Constants::kTeamRed : Constants::kTeamBlue;
        state_.players.push_back(player);
    }

    RebuildMapObjectsFromSeeds();
    InitializeCastlesFromSpawnPoints();

    for (size_t i = 0; i < state_.players.size(); ++i) {
        const CastleState* team_castle = FindCastleByTeam(state_.players[i].team);
        const GridCoord spawn = team_castle != nullptr ? team_castle->cell
                                                       : GridCoord{2 + static_cast<int>(i), 2 + static_cast<int>(i)};
        state_.players[i].spawn_cell = spawn;
        state_.players[i].pos = CellToWorldCenter(spawn);
        state_.players[i].alive = true;
        state_.players[i].awaiting_respawn = false;
        state_.players[i].respawn_remaining = 0.0f;
        state_.players[i].outside_zone_damage_accumulator = 0.0f;
        for (RuneType rune_type : GetAllCastableRuneTypes()) {
            SetPlayerRuneChargeCount(state_.players[i], rune_type, GetInitialRuneCharges(rune_type));
        }
        NormalizePlayerRuneLoadout(state_.players[i]);
        render_player_positions_[state_.players[i].id] = state_.players[i].pos;
    }

    loot_quota_remaining_ =
        loot_table_library_.BuildMatchQuotaPlan(static_cast<int>(state_.players.size())).guaranteed_counts;

    state_.local_player_id = 0;
    pending_open_initial_loadout_ui_ = true;

    network_manager_.BroadcastMatchStart(MatchStartMessage{true, transfer_id, lobby_selected_map_key_});
    if (!SendSelectedMapToClients(transfer_id, map_transfer_precheck)) {
        lobby_preview_status_text_ = "Selected map failed to transfer";
        return;
    }
    network_manager_.BroadcastSnapshot(BuildHostSnapshot());

    in_game_menu_open_ = false;
    in_game_menu_page_ = InGameMenuPage::Home;
    app_screen_ = AppScreen::InMatch;
}

void GameApp::ClearInfluenceZoneVisuals() {
    state_.influence_zones.clear();
    auto clear_texture = [](Texture2D* texture, bool* has_texture) {
        if (texture == nullptr || has_texture == nullptr || !*has_texture) {
            return;
        }
        UnloadTexture(*texture);
        *texture = {};
        *has_texture = false;
    };
    clear_texture(&influence_zone_distance_red_from_texture_, &has_influence_zone_distance_red_from_texture_);
    clear_texture(&influence_zone_distance_blue_from_texture_, &has_influence_zone_distance_blue_from_texture_);
    clear_texture(&influence_zone_distance_red_to_texture_, &has_influence_zone_distance_red_to_texture_);
    clear_texture(&influence_zone_distance_blue_to_texture_, &has_influence_zone_distance_blue_to_texture_);
    ClearInfluenceDistanceField(&influence_zone_distance_red_from_field_);
    ClearInfluenceDistanceField(&influence_zone_distance_blue_from_field_);
    ClearInfluenceDistanceField(&influence_zone_distance_red_to_field_);
    ClearInfluenceDistanceField(&influence_zone_distance_blue_to_field_);
    pending_influence_build_request_.reset();
    influence_build_generation_ += 1;
    influence_zone_transition_elapsed_seconds_ = Constants::kInfluenceZoneTransitionSeconds;
    influence_zone_signature_ = 0;
    has_influence_zone_signature_ = false;
}

void GameApp::AddConsoleMessage(const ConsoleMessage& message) {
    if (message.spans.empty()) {
        return;
    }
    console_entries_.push_back(ConsoleEntry{message, 0.0f});
}

void GameApp::BroadcastConsoleMessageToAll(const ConsoleMessage& message) {
    AddConsoleMessage(message);
    network_manager_.BroadcastConsoleMessage(ConsoleMessageNet{message});
}

void GameApp::SendConsoleMessageToPlayer(int player_id, const ConsoleMessage& message) {
    if (player_id == state_.local_player_id || (network_manager_.IsHost() && player_id == 0)) {
        AddConsoleMessage(message);
    }
    network_manager_.SendConsoleMessageToPlayer(player_id, ConsoleMessageNet{message});
}

void GameApp::SendConsoleMessageToTeam(int team, const ConsoleMessage& message) {
    if (const Player* local_player = FindPlayerById(state_.local_player_id);
        local_player != nullptr && local_player->team == team) {
        AddConsoleMessage(message);
    }
    for (const auto& player : state_.players) {
        if (player.id == 0 || player.team != team) {
            continue;
        }
        network_manager_.SendConsoleMessageToPlayer(player.id, ConsoleMessageNet{message});
    }
}

void GameApp::SendCheatCommandFeedback(int player_id, const std::string& text, Color color) {
    ConsoleMessage console;
    console.lifetime_seconds = 3.5f;
    console.spans.push_back(MakeConsoleSpan("[Cheats] ", color));
    console.spans.push_back(MakeConsoleSpan(text, RAYWHITE));
    SendConsoleMessageToPlayer(player_id, console);
}

std::string GameApp::ResolveSpawnCheatPrototypeId(const std::string& item_name) const {
    const std::string normalized = NormalizeSpawnCheatName(item_name);
    if (normalized.empty()) {
        return "";
    }

    if (FindObjectPrototype(normalized) != nullptr) {
        return normalized;
    }

    static const std::unordered_map<std::string, std::string> kAliases = {
        {"enemy", "training_dummy"},
        {"sword", "sword_item"},
        {"hammer", "hammer_item"},
        {"hook", "grappling_hook_item"},
        {"ghook", "grappling_hook_item"},
        {"grappling_hook", "grappling_hook_item"},
        {"grapple", "grappling_hook_item"},
        {"health", "health_small"},
        {"health_potion", "health_small"},
        {"health_potion_small", "health_small"},
        {"potion", "health_small"},
        {"hp_potion", "health_small"},
        {"mana", "mana_small"},
        {"mana_potion", "mana_small"},
        {"mana_potion_small", "mana_small"},
        {"mp_potion", "mana_small"},
        {"catalyst", "catalyst_charge_pickup"},
        {"catalyst_charge", "catalyst_charge_pickup"},
    };

    const auto it = kAliases.find(normalized);
    if (it != kAliases.end() && FindObjectPrototype(it->second) != nullptr) {
        return it->second;
    }
    return "";
}

bool GameApp::TryHandleCheatCommand(const ChatSubmitMessage& message, const Player& sender) {
    if (!lobby_allow_cheats_ || message.text.empty() || message.text[0] != '/') {
        return false;
    }

    if (message.text.rfind("/spawn", 0) != 0) {
        return false;
    }

    const std::string item_name =
        (message.text.size() > 6 && message.text[6] == ' ') ? message.text.substr(7) : std::string{};
    const std::string prototype_id = ResolveSpawnCheatPrototypeId(item_name);
    if (prototype_id.empty()) {
        SendCheatCommandFeedback(sender.id,
                                 item_name.empty() ? "Usage: /spawn <item_name>"
                                                   : TextFormat("Unknown spawn item '%s'", item_name.c_str()),
                                 Color{255, 140, 140, 255});
        return true;
    }
    const std::string normalized_item_name = NormalizeSpawnCheatName(item_name);
    if (normalized_item_name == "enemy") {
        PendingObjectSpawn spawn;
        spawn.prototype_id = prototype_id;
        spawn.cell = WorldToCell(sender.pos);
        spawn.owner_player_id = 1000000 + sender.id;
        spawn.owner_team = sender.team == Constants::kTeamRed ? Constants::kTeamBlue : Constants::kTeamRed;
        pending_object_spawns_.push_back(spawn);
    } else {
        pending_object_spawns_.push_back({prototype_id, WorldToCell(sender.pos)});
    }
    SendCheatCommandFeedback(sender.id,
                             TextFormat("Spawned %s", normalized_item_name == "enemy" ? "enemy" : prototype_id.c_str()),
                             Color{140, 255, 180, 255});
    return true;
}

void GameApp::DrainIncomingConsoleMessages() {
    for (const auto& message : network_manager_.ConsumeConsoleMessages()) {
        AddConsoleMessage(message.message);
    }
}

void GameApp::UpdateConsoleMessages(float dt) {
    for (auto& entry : console_entries_) {
        entry.age_seconds += dt;
    }
    console_entries_.erase(std::remove_if(console_entries_.begin(), console_entries_.end(),
                                          [](const ConsoleEntry& entry) {
                                              return entry.age_seconds >= entry.message.lifetime_seconds;
                                          }),
                           console_entries_.end());
}

std::string GameApp::GetPlayerDisplayName(int player_id) const {
    const auto known_name_it = known_player_names_.find(player_id);
    if (known_name_it != known_player_names_.end() && !known_name_it->second.empty()) {
        return known_name_it->second;
    }
    if (const Player* player = FindPlayerById(player_id); player != nullptr && !player->name.empty()) {
        return player->name;
    }
    return TextFormat("Player%d", player_id);
}

void GameApp::RecordKillTimelinePoint() {
    state_.match.kill_timeline.push_back(
        {state_.match.elapsed_seconds, state_.match.red_team_kills, state_.match.blue_team_kills});
}

void GameApp::HandleHostChatSubmit(const ChatSubmitMessage& message) {
    if (message.text.empty()) {
        return;
    }
    const Player* sender = FindPlayerById(message.sender_player_id);
    if (sender == nullptr) {
        return;
    }
    if (TryHandleCheatCommand(message, *sender)) {
        return;
    }

    ConsoleMessage console;
    console.lifetime_seconds = kConsoleLifetimeSeconds;
    if (message.channel == ChatChannel::Team) {
        console.spans.push_back(MakeConsoleSpan("[Team] ", Color{190, 220, 255, 255}));
    } else if (message.channel == ChatChannel::Private) {
        console.spans.push_back(MakeConsoleSpan("[Whisper] ", Color{255, 210, 160, 255}));
    }
    console.spans.push_back(MakeConsoleSpan(GetPlayerDisplayName(sender->id), TeamUiColor(sender->team)));
    console.spans.push_back(MakeConsoleSpan(": ", RAYWHITE));
    console.spans.push_back(MakeConsoleSpan(message.text, RAYWHITE));

    switch (message.channel) {
        case ChatChannel::Global:
            BroadcastConsoleMessageToAll(console);
            break;
        case ChatChannel::Team:
            SendConsoleMessageToTeam(sender->team, console);
            break;
        case ChatChannel::Private:
            SendConsoleMessageToPlayer(sender->id, console);
            if (message.target_player_id != sender->id) {
                SendConsoleMessageToPlayer(message.target_player_id, console);
            }
            break;
    }
}

void GameApp::HandleDisconnectedRemotePlayers() {
    for (const auto& remote : network_manager_.ConsumeDisconnectedRemotePlayers()) {
        int team = Constants::kTeamBlue;
        if (const Player* player = FindPlayerById(remote.player_id); player != nullptr) {
            team = player->team;
        }
        ConsoleMessage console;
        console.lifetime_seconds = kConsoleLifetimeSeconds;
        console.spans.push_back(MakeConsoleSpan(remote.name, TeamUiColor(team)));
        console.spans.push_back(MakeConsoleSpan(" has left the game", RAYWHITE));
        BroadcastConsoleMessageToAll(console);
    }
}

void GameApp::MaybeBroadcastMatchCountdown() {
    if (!network_manager_.IsHost() || state_.match.mode_type != MatchModeType::MostKillsTimed || state_.match.match_finished) {
        return;
    }
    const int seconds_remaining = static_cast<int>(std::ceil(state_.match.time_remaining));
    if (seconds_remaining > 10 || seconds_remaining < 1 || seconds_remaining >= last_match_countdown_announcement_seconds_) {
        return;
    }
    last_match_countdown_announcement_seconds_ = seconds_remaining;
    ConsoleMessage console;
    console.lifetime_seconds = 1.2f;
    console.spans.push_back(MakeConsoleSpan("Match ends in ", RAYWHITE));
    console.spans.push_back(MakeConsoleSpan(std::to_string(seconds_remaining), Color{255, 100, 100, 255}));
    console.spans.push_back(MakeConsoleSpan(" seconds!", RAYWHITE));
    BroadcastConsoleMessageToAll(console);
}

bool GameApp::TryOpenChatInput() {
    if (chat_input_active_ || in_game_menu_open_) {
        return false;
    }
    if (const Player* local_player = FindPlayerById(state_.local_player_id);
        local_player != nullptr && local_player->inventory_mode) {
        return false;
    }
    chat_input_active_ = true;
    chat_input_buffer_.clear();
    return true;
}

void GameApp::CancelChatInput() {
    chat_input_active_ = false;
    chat_input_buffer_.clear();
}

bool GameApp::TryBuildPrivateChatSubmit(const std::string& text, ChatSubmitMessage* out_message) const {
    if (out_message == nullptr) {
        return false;
    }
    const bool is_whisper = text.rfind("/w ", 0) == 0;
    const bool is_pm = text.rfind("/pm ", 0) == 0;
    if (!is_whisper && !is_pm) {
        return false;
    }
    const std::string payload = text.substr(is_whisper ? 3 : 4);
    const size_t split = payload.find(' ');
    if (split == std::string::npos || split == 0 || split + 1 >= payload.size()) {
        return false;
    }
    const std::string target_name = payload.substr(0, split);
    for (const auto& player : state_.players) {
        if (GetPlayerDisplayName(player.id) == target_name) {
            out_message->channel = ChatChannel::Private;
            out_message->target_player_id = player.id;
            out_message->text = payload.substr(split + 1);
            return true;
        }
    }
    return false;
}

void GameApp::SubmitChatInput(bool team_only) {
    const std::string text = chat_input_buffer_;
    CancelChatInput();
    if (text.empty()) {
        return;
    }

    ChatSubmitMessage message;
    message.sender_player_id = state_.local_player_id;
    message.channel = team_only ? ChatChannel::Team : ChatChannel::Global;
    message.target_player_id = -1;
    message.text = text;
    if (!team_only) {
        ChatSubmitMessage private_message = message;
        if (TryBuildPrivateChatSubmit(text, &private_message)) {
            message = std::move(private_message);
        }
    }

    if (network_manager_.IsHost()) {
        HandleHostChatSubmit(message);
    } else {
        network_manager_.SendChatSubmit(message);
    }
}

void GameApp::UpdateChatInput() {
    if (!chat_input_active_) {
        if (enter_pressed_this_update_) {
            TryOpenChatInput();
            enter_pressed_this_update_ = false;
        }
        return;
    }

    if (!text_input_this_update_.empty()) {
        chat_input_buffer_.append(text_input_this_update_);
        if (static_cast<int>(chat_input_buffer_.size()) > kChatMaxChars) {
            chat_input_buffer_.resize(kChatMaxChars);
        }
    }
    if (backspace_pressed_this_update_ && !chat_input_buffer_.empty()) {
        chat_input_buffer_.pop_back();
    }
    if (escape_pressed_this_update_) {
        CancelChatInput();
        escape_pressed_this_update_ = false;
        return;
    }
    if (enter_pressed_this_update_) {
        SubmitChatInput(enter_shift_down_this_update_);
        enter_pressed_this_update_ = false;
    }
}

void GameApp::ApplySnapshotToClientState(const ServerSnapshotMessage& snapshot) {
    struct PreviousPlayerState {
        int hp = 0;
        bool alive = false;
        PlayerActionState action_state = PlayerActionState::Idle;
    };
    altars_.clear();

    std::unordered_map<int, int> previous_hp;
    previous_hp.reserve(state_.players.size());
    std::unordered_map<int, PreviousPlayerState> previous_player_state;
    previous_player_state.reserve(state_.players.size());
    std::unordered_map<int, float> previous_player_frozen_remaining;
    previous_player_frozen_remaining.reserve(state_.players.size());
    std::unordered_map<int, std::array<std::string, 4>> previous_player_item_slots;
    previous_player_item_slots.reserve(state_.players.size());
    std::unordered_map<int, std::array<int, 4>> previous_player_item_counts;
    previous_player_item_counts.reserve(state_.players.size());
    for (const auto& player : state_.players) {
        previous_hp[player.id] = player.hp;
        previous_player_state[player.id] = {player.hp, player.alive, player.action_state};
        previous_player_frozen_remaining[player.id] = 0.0f;
        for (const auto& status : player.status_effects) {
            if (status.type == StatusEffectType::Frozen && status.remaining_seconds > 0.0f) {
                previous_player_frozen_remaining[player.id] =
                    std::max(previous_player_frozen_remaining[player.id], status.remaining_seconds);
            }
        }
        previous_player_item_slots[player.id] = player.item_slots;
        previous_player_item_counts[player.id] = player.item_slot_counts;
    }
    std::unordered_set<int> previous_dummy_ids;
    previous_dummy_ids.reserve(state_.fire_storm_dummies.size());
    for (const auto& dummy : state_.fire_storm_dummies) {
        previous_dummy_ids.insert(dummy.id);
    }
    std::unordered_set<int> previous_fire_storm_cast_ids;
    previous_fire_storm_cast_ids.reserve(state_.fire_storm_casts.size());
    for (const auto& cast : state_.fire_storm_casts) {
        previous_fire_storm_cast_ids.insert(cast.id);
    }
    std::unordered_set<int> previous_rune_ids;
    previous_rune_ids.reserve(state_.runes.size());
    std::unordered_map<int, bool> previous_rune_active;
    previous_rune_active.reserve(state_.runes.size());
    std::unordered_map<int, EarthRuneTrapState> previous_earth_rune_states;
    previous_earth_rune_states.reserve(state_.runes.size());
    std::unordered_map<int, bool> previous_earth_rune_roots_spawned;
    previous_earth_rune_roots_spawned.reserve(state_.runes.size());
    for (const auto& rune : state_.runes) {
        previous_rune_ids.insert(rune.id);
        previous_rune_active[rune.id] = rune.active;
        previous_earth_rune_states[rune.id] = rune.earth_trap_state;
        previous_earth_rune_roots_spawned[rune.id] = rune.earth_roots_spawned;
    }
    std::unordered_map<int, Vector2> previous_projectile_positions;
    previous_projectile_positions.reserve(state_.projectiles.size());
    std::unordered_map<int, Vector2> previous_projectile_velocities;
    previous_projectile_velocities.reserve(state_.projectiles.size());
    std::unordered_map<int, std::string> previous_projectile_animation_keys;
    previous_projectile_animation_keys.reserve(state_.projectiles.size());
    for (const auto& projectile : state_.projectiles) {
        previous_projectile_positions[projectile.id] = projectile.pos;
        previous_projectile_velocities[projectile.id] = projectile.vel;
        previous_projectile_animation_keys[projectile.id] = projectile.animation_key;
    }
    std::unordered_map<int, FireSpirit> previous_fire_spirits_by_id;
    previous_fire_spirits_by_id.reserve(state_.fire_spirits.size());
    for (const auto& spirit : state_.fire_spirits) {
        previous_fire_spirits_by_id.emplace(spirit.id, spirit);
    }
    std::unordered_set<int> previous_fire_wave_segment_ids;
    previous_fire_wave_segment_ids.reserve(state_.fire_wave_segments.size());
    for (const auto& segment : state_.fire_wave_segments) {
        previous_fire_wave_segment_ids.insert(segment.id);
    }
    std::unordered_map<int, IceWallState> previous_wall_states;
    previous_wall_states.reserve(state_.ice_walls.size());
    for (const auto& wall : state_.ice_walls) {
        previous_wall_states[wall.id] = wall.state;
    }
    std::unordered_map<int, MapObjectState> previous_object_states;
    previous_object_states.reserve(state_.map_objects.size());
    std::unordered_map<int, Vector2> previous_consumable_positions;
    previous_consumable_positions.reserve(state_.map_objects.size());
    std::unordered_map<int, std::string> previous_consumable_prototype_ids;
    previous_consumable_prototype_ids.reserve(state_.map_objects.size());
    for (const auto& object : state_.map_objects) {
        previous_object_states[object.id] = object.state;
        if (object.type == ObjectType::Consumable && object.alive) {
            previous_consumable_positions[object.id] = CellToWorldCenter(object.cell);
            previous_consumable_prototype_ids[object.id] = object.prototype_id;
        }
    }
    std::unordered_map<int, GrapplingHookPhase> previous_grappling_hook_phases;
    previous_grappling_hook_phases.reserve(state_.grappling_hooks.size());
    std::optional<GrapplingHook> previous_local_owned_hook;
    for (const auto& hook : state_.grappling_hooks) {
        previous_grappling_hook_phases[hook.id] = hook.phase;
        if (!network_manager_.IsHost() && state_.local_player_id >= 0 && hook.owner_player_id == state_.local_player_id &&
            hook.alive) {
            previous_local_owned_hook = hook;
        }
    }
    std::unordered_map<int, int> previous_castle_levels;
    previous_castle_levels.reserve(state_.castles.size());
    for (const auto& castle : state_.castles) {
        previous_castle_levels[castle.id] = castle.level;
    }

    Vector2 previous_local_pos = {0.0f, 0.0f};
    Vector2 previous_local_vel = {0.0f, 0.0f};
    bool has_previous_local = false;
    Player previous_local_ui_state;
    bool has_previous_local_ui_state = false;
    if (state_.local_player_id >= 0) {
        if (const Player* previous_local = FindPlayerById(state_.local_player_id)) {
            previous_local_pos = previous_local->pos;
            previous_local_vel = previous_local->vel;
            has_previous_local = true;
            previous_local_ui_state = *previous_local;
            has_previous_local_ui_state = true;
        }
    }

    state_.match.elapsed_seconds = snapshot.simulation_time_seconds;
    state_.match.time_remaining = snapshot.time_remaining;
    state_.match.zone_enabled = snapshot.zone_enabled;
    state_.match.shrink_tiles_per_second = snapshot.shrink_tiles_per_second;
    state_.match.min_arena_radius_tiles = snapshot.min_arena_radius_tiles;
    state_.match.arena_radius_tiles = snapshot.arena_radius_tiles;
    state_.match.arena_radius_world = snapshot.arena_radius_world;
    state_.match.arena_center_world = {snapshot.arena_center_world_x, snapshot.arena_center_world_y};
    state_.match.match_running = snapshot.match_running;
    state_.match.match_finished = snapshot.match_finished;
    state_.match.red_team_kills = snapshot.red_team_kills;
    state_.match.blue_team_kills = snapshot.blue_team_kills;
    state_.match.kill_timeline = snapshot.kill_timeline;

    const double now_seconds = GetTime();
    std::unordered_map<int, Vector2> updated_render_positions;
    state_.players.clear();
    for (const auto& player_snapshot : snapshot.players) {
        Player player;
        auto known_name_it = known_player_names_.find(player_snapshot.id);
        const std::string resolved_name =
            known_name_it != known_player_names_.end() ? known_name_it->second : TextFormat("Player%d", player_snapshot.id);
        SnapshotTranslation::ApplyPlayerSnapshot(&player, player_snapshot, resolved_name);
        if (has_previous_local_ui_state && player.id == state_.local_player_id) {
            player.inventory_mode = previous_local_ui_state.inventory_mode;
            if (PendingLocalInventorySyncMatches(player)) {
                pending_local_inventory_sync_.reset();
                pending_local_inventory_sync_dirty_ = false;
            }
            const bool preserve_local_inventory =
                previous_local_ui_state.ui_dragging_slot || pending_local_inventory_sync_.has_value();
            if (preserve_local_inventory) {
                player.rune_slots = previous_local_ui_state.rune_slots;
                player.selected_rune_slot = previous_local_ui_state.selected_rune_slot;
                player.selected_rune_type = previous_local_ui_state.selected_rune_type;
                player.item_slots = previous_local_ui_state.item_slots;
                player.item_slot_counts = previous_local_ui_state.item_slot_counts;
                player.item_slot_cooldown_remaining = previous_local_ui_state.item_slot_cooldown_remaining;
                player.item_slot_cooldown_total = previous_local_ui_state.item_slot_cooldown_total;
                player.ui_dragging_slot = previous_local_ui_state.ui_dragging_slot;
                player.ui_drag_source_family = previous_local_ui_state.ui_drag_source_family;
                player.ui_drag_source_index = previous_local_ui_state.ui_drag_source_index;
                player.ui_drag_rune_type = previous_local_ui_state.ui_drag_rune_type;
                player.ui_drag_item_id = previous_local_ui_state.ui_drag_item_id;
                player.ui_drag_item_count = previous_local_ui_state.ui_drag_item_count;
                player.ui_drag_item_cooldown_remaining = previous_local_ui_state.ui_drag_item_cooldown_remaining;
                player.ui_drag_item_cooldown_total = previous_local_ui_state.ui_drag_item_cooldown_total;
            }
        }
        state_.players.push_back(player);

        if (!network_manager_.IsHost() && player.id != state_.local_player_id) {
            auto& samples = remote_position_samples_[player.id];
            samples.push_back(RemotePositionSample{now_seconds, player.pos});
            while (samples.size() > 32) {
                samples.pop_front();
            }
            updated_render_positions[player.id] = player.pos;
        } else {
            updated_render_positions[player.id] = player.pos;
        }
    }
    render_player_positions_.swap(updated_render_positions);
    if (!network_manager_.IsHost()) {
        for (const auto& player : state_.players) {
            float frozen_remaining = 0.0f;
            for (const auto& status : player.status_effects) {
                if (status.type == StatusEffectType::Frozen && status.remaining_seconds > 0.0f) {
                    frozen_remaining = std::max(frozen_remaining, status.remaining_seconds);
                }
            }
            const float previous_remaining =
                previous_player_frozen_remaining.count(player.id) ? previous_player_frozen_remaining[player.id] : 0.0f;
            if (frozen_remaining > previous_remaining + 0.001f) {
                std::vector<size_t> loaded_indices;
                loaded_indices.reserve(sfx_ice_wave_impact_.size());
                for (size_t i = 0; i < sfx_ice_wave_impact_.size(); ++i) {
                    if (sfx_ice_wave_impact_[i].loaded) {
                        loaded_indices.push_back(i);
                    }
                }
                if (!loaded_indices.empty()) {
                    std::uniform_int_distribution<size_t> dist(0, loaded_indices.size() - 1);
                    const LoadedSfx& clip = sfx_ice_wave_impact_[loaded_indices[dist(rng_)]];
                    PlaySfxIfVisible(clip.sound, clip.loaded, player.pos);
                }
            }
        }
    }

    state_.runes.clear();
    for (const auto& rune_snapshot : snapshot.runes) {
        Rune rune;
        rune.id = rune_snapshot.id;
        rune.owner_player_id = rune_snapshot.owner_player_id;
        rune.owner_team = rune_snapshot.owner_team;
        rune.cell = {rune_snapshot.x, rune_snapshot.y};
        rune.rune_type = static_cast<RuneType>(rune_snapshot.rune_type);
        rune.placement_order = rune_snapshot.placement_order;
        rune.active = rune_snapshot.active;
        rune.volatile_cast = rune_snapshot.volatile_cast;
        rune.activation_total_seconds = rune_snapshot.activation_total_seconds;
        rune.activation_remaining_seconds = rune_snapshot.activation_remaining_seconds;
        rune.creates_influence_zone = rune_snapshot.creates_influence_zone;
        rune.earth_trap_state = static_cast<EarthRuneTrapState>(rune_snapshot.earth_trap_state);
        rune.earth_state_time = rune_snapshot.earth_state_time;
        rune.earth_state_duration = rune_snapshot.earth_state_duration;
        rune.earth_roots_spawned = rune_snapshot.earth_roots_spawned;
        rune.earth_roots_group_id = rune_snapshot.earth_roots_group_id;
        rune.fire_storm_original_owner_player_id = rune_snapshot.fire_storm_original_owner_player_id;
        rune.fire_storm_original_owner_team = rune_snapshot.fire_storm_original_owner_team;
        rune.fire_storm_original_rune_type = static_cast<RuneType>(rune_snapshot.fire_storm_original_rune_type);
        rune.fire_storm_temporary = rune_snapshot.fire_storm_temporary;
        rune.fire_storm_source_rune = rune_snapshot.fire_storm_source_rune;
        rune.fire_storm_remaining_seconds = rune_snapshot.fire_storm_remaining_seconds;
        rune.fire_storm_visual_state = static_cast<FireStormRuneVisualState>(rune_snapshot.fire_storm_visual_state);
        rune.fire_storm_visual_state_time = rune_snapshot.fire_storm_visual_state_time;
        rune.fire_storm_visual_state_duration = rune_snapshot.fire_storm_visual_state_duration;
        rune.fire_storm_revert_after_death = rune_snapshot.fire_storm_revert_after_death;
        rune.fire_storm_pending_removal = rune_snapshot.fire_storm_pending_removal;
        rune.castle_charging = rune_snapshot.castle_charging;
        rune.castle_id = rune_snapshot.castle_id;
        rune.castle_charge_elapsed_seconds = rune_snapshot.castle_charge_elapsed_seconds;
        state_.runes.push_back(rune);
    }
    RebuildInfluenceZones();
    if (!network_manager_.IsHost()) {
        for (const auto& rune : state_.runes) {
            const bool is_new_rune = previous_rune_ids.find(rune.id) == previous_rune_ids.end();
            if (is_new_rune && rune.volatile_cast && !rune.active && rune.activation_remaining_seconds > 0.0f) {
                SpawnVolatileCastCasterFx(rune.owner_player_id);
            }
            const auto previous_active_it = previous_rune_active.find(rune.id);
            const bool became_active = rune.active &&
                                       (previous_active_it == previous_rune_active.end() || !previous_active_it->second);
            if (!became_active) {
                continue;
            }
            Particle rune_cast_vfx;
            rune_cast_vfx.pos = CellToWorldCenter(rune.cell);
            rune_cast_vfx.vel = {0.0f, 0.0f};
            rune_cast_vfx.acc = {0.0f, 0.0f};
            rune_cast_vfx.drag = 0.0f;
            rune_cast_vfx.animation_key = rune.volatile_cast ? "rune_cast_volatile_effect" : "rune_cast_effect";
            rune_cast_vfx.facing = "default";
            rune_cast_vfx.age_seconds = 0.0f;
            rune_cast_vfx.max_cycles = 1;
            rune_cast_vfx.alive = true;
            state_.particles.push_back(rune_cast_vfx);
            PlaySfxIfVisible(sfx_create_rune_.sound, sfx_create_rune_.loaded, rune_cast_vfx.pos);

            Vector2 start = CellToWorldCenter(rune.cell);
            if (const Player* caster = FindPlayerById(rune.owner_player_id)) {
                start = caster->pos;
            }
            SpawnLightningEffect(start, CellToWorldCenter(rune.cell), 0.3f, rune.volatile_cast);
        }
        for (const auto& rune : state_.runes) {
            if (rune.rune_type != RuneType::Earth || !rune.active) {
                continue;
            }
            const auto previous_state_it = previous_earth_rune_states.find(rune.id);
            const EarthRuneTrapState previous_state =
                (previous_state_it != previous_earth_rune_states.end()) ? previous_state_it->second
                                                                        : EarthRuneTrapState::IdleRune;
            if (previous_state != EarthRuneTrapState::Slamming && rune.earth_trap_state == EarthRuneTrapState::Slamming) {
                PlaySfxIfVisible(sfx_earth_rune_launch_.sound, sfx_earth_rune_launch_.loaded, CellToWorldCenter(rune.cell));
            }
            const auto previous_roots_it = previous_earth_rune_roots_spawned.find(rune.id);
            const bool roots_spawned_before =
                (previous_roots_it != previous_earth_rune_roots_spawned.end()) ? previous_roots_it->second : false;
            if (!roots_spawned_before && rune.earth_roots_spawned) {
                const Vector2 impact_center = CellToWorldCenter(rune.cell);
                PlaySfxIfVisible(sfx_earth_rune_impact_.sound, sfx_earth_rune_impact_.loaded, impact_center);
                if (IsWorldPointInsideCameraView(impact_center)) {
                    camera_shake_time_remaining_ =
                        std::max(camera_shake_time_remaining_, Constants::kCameraShakeDurationSeconds);
                }
            }
        }
    }

    state_.projectiles.clear();
    for (const auto& projectile_snapshot : snapshot.projectiles) {
        Projectile projectile;
        projectile.id = projectile_snapshot.id;
        projectile.owner_player_id = projectile_snapshot.owner_player_id;
        projectile.owner_team = projectile_snapshot.owner_team;
        projectile.pos = {projectile_snapshot.pos_x, projectile_snapshot.pos_y};
        projectile.vel = {projectile_snapshot.vel_x, projectile_snapshot.vel_y};
        projectile.radius = projectile_snapshot.radius;
        projectile.damage = projectile_snapshot.damage;
        projectile.animation_key = projectile_snapshot.animation_key;
        projectile.emitter_enabled = projectile_snapshot.emitter_enabled;
        projectile.emitter_emit_every_frames = projectile_snapshot.emitter_emit_every_frames;
        projectile.emitter_frame_counter = projectile_snapshot.emitter_frame_counter;
        projectile.alive = projectile_snapshot.alive;
        state_.projectiles.push_back(projectile);
    }
    last_snapshot_simulation_time_seconds_ = snapshot.simulation_time_seconds;
    last_snapshot_received_local_time_seconds_ = GetTime();
    state_.fire_spirits.clear();
    for (const auto& spirit_snapshot : snapshot.fire_spirits) {
        FireSpirit spirit;
        spirit.id = spirit_snapshot.id;
        spirit.flower_object_id = spirit_snapshot.flower_object_id;
        spirit.owner_player_id = spirit_snapshot.owner_player_id;
        spirit.owner_team = spirit_snapshot.owner_team;
        spirit.state = static_cast<FireSpiritState>(spirit_snapshot.state);
        spirit.pos = {spirit_snapshot.pos_x, spirit_snapshot.pos_y};
        spirit.vel = {spirit_snapshot.vel_x, spirit_snapshot.vel_y};
        spirit.target_world = {spirit_snapshot.target_world_x, spirit_snapshot.target_world_y};
        spirit.spawn_order = spirit_snapshot.spawn_order;
        spirit.age_seconds = spirit_snapshot.age_seconds;
        spirit.launch_world = {spirit_snapshot.launch_world_x, spirit_snapshot.launch_world_y};
        spirit.impact_world = {spirit_snapshot.impact_world_x, spirit_snapshot.impact_world_y};
        spirit.launch_time_seconds = spirit_snapshot.launch_time_seconds;
        spirit.impact_time_seconds = spirit_snapshot.impact_time_seconds;
        spirit.travel_duration_seconds = spirit_snapshot.travel_duration_seconds;
        spirit.peak_height = spirit_snapshot.peak_height;
        spirit.projectile_animation_time = spirit_snapshot.projectile_animation_time;
        spirit.alive = spirit_snapshot.alive;
        if (const auto previous_it = previous_fire_spirits_by_id.find(spirit.id);
            previous_it != previous_fire_spirits_by_id.end()) {
            const bool previous_has_trail =
                previous_it->second.state == FireSpiritState::Projectile || previous_it->second.state == FireSpiritState::Dead;
            const bool current_has_trail =
                spirit.state == FireSpiritState::Projectile || spirit.state == FireSpiritState::Dead;
            if (previous_has_trail && current_has_trail) {
                spirit.trail_samples = previous_it->second.trail_samples;
                spirit.trail_distance_accumulator = previous_it->second.trail_distance_accumulator;
                spirit.spark_distance_accumulator = previous_it->second.spark_distance_accumulator;
                spirit.last_trail_sample_world = previous_it->second.last_trail_sample_world;
                spirit.has_last_trail_sample_world = previous_it->second.has_last_trail_sample_world;
                spirit.dead_trail_linger_remaining = previous_it->second.dead_trail_linger_remaining;
                if (Constants::kFireSpiritTrailEnableDeadLinger &&
                    previous_it->second.state != FireSpiritState::Dead && spirit.state == FireSpiritState::Dead) {
                    spirit.dead_trail_linger_remaining = Constants::kFireSpiritTrailDeadLingerSeconds;
                }
            }
        }
        state_.fire_spirits.push_back(std::move(spirit));
    }
    if (!network_manager_.IsHost()) {
        for (const auto& spirit : state_.fire_spirits) {
            const auto previous_it = previous_fire_spirits_by_id.find(spirit.id);
            if (previous_it == previous_fire_spirits_by_id.end()) {
                continue;
            }
            if (previous_it->second.state != FireSpiritState::Projectile && spirit.state == FireSpiritState::Projectile) {
                PlaySfxIfVisible(sfx_fire_spirit_launch_.sound, sfx_fire_spirit_launch_.loaded, spirit.launch_world);
            }
        }
    }
    state_.fire_wave_segments.clear();
    for (const auto& segment_snapshot : snapshot.fire_wave_segments) {
        FireWaveSegment segment;
        segment.id = segment_snapshot.id;
        segment.source_spirit_id = segment_snapshot.source_spirit_id;
        segment.owner_player_id = segment_snapshot.owner_player_id;
        segment.owner_team = segment_snapshot.owner_team;
        segment.segment_index = segment_snapshot.segment_index;
        segment.origin_world = {segment_snapshot.origin_world_x, segment_snapshot.origin_world_y};
        segment.direction_radians = segment_snapshot.direction_radians;
        segment.start_time_seconds = segment_snapshot.start_time_seconds;
        segment.duration_seconds = segment_snapshot.duration_seconds;
        segment.range_world = segment_snapshot.range_world;
        segment.alive = segment_snapshot.alive;
        state_.fire_wave_segments.push_back(segment);
        if (!network_manager_.IsHost() && segment.segment_index == 0 &&
            previous_fire_wave_segment_ids.find(segment.id) == previous_fire_wave_segment_ids.end()) {
            PlaySfxIfVisible(sfx_fire_storm_impact_.sound, sfx_fire_storm_impact_.loaded, segment.origin_world);
        }
    }
    state_.embers_tile_modifiers.clear();
    for (const auto& modifier_snapshot : snapshot.embers_tile_modifiers) {
        EmbersTileModifier modifier;
        modifier.id = modifier_snapshot.id;
        modifier.source_spirit_id = modifier_snapshot.source_spirit_id;
        modifier.owner_player_id = modifier_snapshot.owner_player_id;
        modifier.owner_team = modifier_snapshot.owner_team;
        modifier.cell = {modifier_snapshot.cell_x, modifier_snapshot.cell_y};
        modifier.remaining_seconds = modifier_snapshot.remaining_seconds;
        modifier.total_seconds = modifier_snapshot.total_seconds;
        modifier.alive = modifier_snapshot.alive;
        state_.embers_tile_modifiers.push_back(modifier);
    }
    if (!network_manager_.IsHost()) {
        std::unordered_set<int> current_projectile_ids;
        current_projectile_ids.reserve(state_.projectiles.size());
        bool played_ice_wave_cast = false;
        for (const auto& projectile : state_.projectiles) {
            current_projectile_ids.insert(projectile.id);
            if (previous_projectile_positions.find(projectile.id) == previous_projectile_positions.end() &&
                projectile.animation_key == "projectile_fire_bolt") {
                PlaySfxIfVisible(sfx_fireball_created_.sound, sfx_fireball_created_.loaded, projectile.pos);
            } else if (!played_ice_wave_cast &&
                       previous_projectile_positions.find(projectile.id) == previous_projectile_positions.end() &&
                       IsIceShardProjectile(projectile)) {
                std::vector<size_t> loaded_indices;
                loaded_indices.reserve(sfx_ice_wave_cast_.size());
                for (size_t i = 0; i < sfx_ice_wave_cast_.size(); ++i) {
                    if (sfx_ice_wave_cast_[i].loaded) {
                        loaded_indices.push_back(i);
                    }
                }
                if (!loaded_indices.empty()) {
                    std::uniform_int_distribution<size_t> dist(0, loaded_indices.size() - 1);
                    const LoadedSfx& clip = sfx_ice_wave_cast_[loaded_indices[dist(rng_)]];
                    PlaySfxIfVisible(clip.sound, clip.loaded, projectile.pos);
                }
                played_ice_wave_cast = true;
            }
            const auto previous_animation_it = previous_projectile_animation_keys.find(projectile.id);
            if (previous_animation_it != previous_projectile_animation_keys.end() &&
                previous_animation_it->second != projectile.animation_key && IsStaticFireBolt(projectile)) {
                Particle upgrade_vfx;
                upgrade_vfx.pos = projectile.pos;
                upgrade_vfx.vel = {0.0f, 0.0f};
                upgrade_vfx.acc = {0.0f, 0.0f};
                upgrade_vfx.drag = 0.0f;
                upgrade_vfx.size = 48.0f;
                upgrade_vfx.animation_key = "static_upgrade";
                upgrade_vfx.facing = "default";
                upgrade_vfx.age_seconds = 0.0f;
                upgrade_vfx.max_cycles = 1;
                upgrade_vfx.alive = true;
                state_.particles.push_back(upgrade_vfx);
                PlaySfxIfVisible(sfx_static_upgrade_.sound, sfx_static_upgrade_.loaded, projectile.pos);
            }
        }
        for (const auto& [projectile_id, projectile_pos] : previous_projectile_positions) {
            if (current_projectile_ids.find(projectile_id) != current_projectile_ids.end()) {
                continue;
            }
            const auto previous_animation_it = previous_projectile_animation_keys.find(projectile_id);
            if (previous_animation_it != previous_projectile_animation_keys.end() &&
                IsIceShardProjectileAnimation(previous_animation_it->second)) {
                const Vector2 travel_velocity =
                    previous_projectile_velocities.count(projectile_id) ? previous_projectile_velocities[projectile_id]
                                                                        : Vector2{1.0f, 0.0f};
                SpawnIceShardDeathParticle(projectile_pos, travel_velocity);
                continue;
            }
            const bool was_static = previous_animation_it != previous_projectile_animation_keys.end() &&
                                    previous_animation_it->second == "fire_storm_static_large";
            Particle explosion_vfx;
            explosion_vfx.pos = projectile_pos;
            explosion_vfx.vel = {0.0f, 0.0f};
            explosion_vfx.acc = {0.0f, 0.0f};
            explosion_vfx.drag = 0.0f;
            explosion_vfx.size = 32.0f;
            explosion_vfx.alpha = 255;
            explosion_vfx.animation_key = "explosion";
            explosion_vfx.facing = "default";
            explosion_vfx.age_seconds = 0.0f;
            explosion_vfx.max_cycles = 1;
            explosion_vfx.alive = true;
            state_.particles.push_back(explosion_vfx);
            const float half_w = static_cast<float>(GetScreenWidth()) / std::max(0.001f, (2.0f * camera_.zoom));
            const float half_h = static_cast<float>(GetScreenHeight()) / std::max(0.001f, (2.0f * camera_.zoom));
            const float pad = Constants::kFireBoltExplosionRadius;
            const bool in_camera_view =
                projectile_pos.x >= (camera_.target.x - half_w - pad) &&
                projectile_pos.x <= (camera_.target.x + half_w + pad) &&
                projectile_pos.y >= (camera_.target.y - half_h - pad) &&
                projectile_pos.y <= (camera_.target.y + half_h + pad);
            if (in_camera_view) {
                camera_shake_time_remaining_ =
                    std::max(camera_shake_time_remaining_, Constants::kCameraShakeDurationSeconds);
            }
            if (was_static) {
                PlaySfxIfVisible(sfx_static_bolt_impact_.sound, sfx_static_bolt_impact_.loaded, projectile_pos);
            } else {
                PlaySfxIfVisible(sfx_explosion_.sound, sfx_explosion_.loaded, projectile_pos);
            }
        }
    }

    state_.ice_walls.clear();
    for (const auto& wall_snapshot : snapshot.ice_walls) {
        IceWallPiece wall;
        wall.id = wall_snapshot.id;
        wall.owner_player_id = wall_snapshot.owner_player_id;
        wall.owner_team = wall_snapshot.owner_team;
        wall.cell = {wall_snapshot.cell_x, wall_snapshot.cell_y};
        wall.state = static_cast<IceWallState>(wall_snapshot.state);
        wall.state_time = wall_snapshot.state_time;
        wall.hp = wall_snapshot.hp;
        wall.alive = wall_snapshot.alive;
        state_.ice_walls.push_back(wall);
    }
    if (!network_manager_.IsHost()) {
        bool played_freeze = false;
        bool played_melt = false;
        for (const auto& wall : state_.ice_walls) {
            const auto previous_state_it = previous_wall_states.find(wall.id);
            const Vector2 center = CellToWorldCenter(wall.cell);
            if (previous_state_it == previous_wall_states.end()) {
                if (!played_freeze) {
                    PlaySfxIfVisible(sfx_ice_wall_freeze_.sound, sfx_ice_wall_freeze_.loaded, center);
                    played_freeze = true;
                }
                continue;
            }
            if (previous_state_it->second != IceWallState::Dying && wall.state == IceWallState::Dying) {
                if (!played_melt) {
                    PlaySfxIfVisible(sfx_ice_wall_melt_.sound, sfx_ice_wall_melt_.loaded, center);
                    played_melt = true;
                }
            }
        }
    }
    state_.map_objects.clear();
    map_object_index_by_id_.clear();
    for (const auto& object_snapshot : snapshot.map_objects) {
        MapObjectInstance object;
        object.id = object_snapshot.id;
        object.owner_player_id = object_snapshot.owner_player_id;
        object.owner_team = object_snapshot.owner_team;
        object.prototype_id = object_snapshot.prototype_id;
        object.cell = {object_snapshot.cell_x, object_snapshot.cell_y};
        object.type = static_cast<ObjectType>(object_snapshot.object_type);
        object.hp = object_snapshot.hp;
        object.state = static_cast<MapObjectState>(object_snapshot.state);
        object.state_time = object_snapshot.state_time;
        object.death_duration = object_snapshot.death_duration;
        object.collision_enabled = object_snapshot.collision_enabled;
        object.alive = object_snapshot.alive;
        if (const ObjectPrototype* proto = FindObjectPrototype(object.prototype_id)) {
            object.walkable = proto->walkable;
            object.stops_projectiles = proto->stops_projectiles;
        }
        state_.map_objects.push_back(object);
    }
    RebuildMapObjectIndexLookup();
    MarkMapObjectRenderCachesDirty();
    RebuildAltarsFromMapObjects();
    state_.castles.clear();
    for (const auto& castle_snapshot : snapshot.castles) {
        CastleState castle;
        castle.id = castle_snapshot.id;
        castle.team = castle_snapshot.team;
        castle.cell = {castle_snapshot.cell_x, castle_snapshot.cell_y};
        castle.map_object_id = castle_snapshot.map_object_id;
        castle.level = castle_snapshot.level;
        castle.total_energy = castle_snapshot.total_energy;
        castle.energy_into_current_level = castle_snapshot.energy_into_current_level;
        castle.energy_needed_for_next_level = castle_snapshot.energy_needed_for_next_level;
        castle.charge_port_offset_x = castle_snapshot.charge_port_offset_x;
        castle.charge_port_offset_y = castle_snapshot.charge_port_offset_y;
        state_.castles.push_back(castle);
    }
    if (!network_manager_.IsHost() && sfx_castle_level_up_.loaded) {
        for (const auto& castle : state_.castles) {
            const auto previous_level_it = previous_castle_levels.find(castle.id);
            if (previous_level_it != previous_castle_levels.end() && castle.level > previous_level_it->second) {
                PlaySound(sfx_castle_level_up_.sound);
            }
        }
    }
    if (!network_manager_.IsHost()) {
        std::unordered_set<int> current_object_ids;
        current_object_ids.reserve(state_.map_objects.size());
        for (const auto& object : state_.map_objects) {
            current_object_ids.insert(object.id);
            const auto previous_state_it = previous_object_states.find(object.id);
            if (previous_state_it == previous_object_states.end()) {
                if (object.prototype_id == "fire_flower" && object.state == MapObjectState::Spawning) {
                    PlaySfxIfVisible(sfx_earth_rune_launch_.sound, sfx_earth_rune_launch_.loaded, CellToWorldCenter(object.cell));
                }
                if (object.prototype_id == "catalyst_charge_pickup") {
                    const auto spawn_altar_charge_vfx = [&](const GridCoord& cell) {
                        Particle rune_cast_vfx;
                        rune_cast_vfx.pos = CellToWorldCenter(cell);
                        rune_cast_vfx.vel = {0.0f, 0.0f};
                        rune_cast_vfx.acc = {0.0f, 0.0f};
                        rune_cast_vfx.drag = 0.0f;
                        rune_cast_vfx.animation_key = "rune_cast_effect";
                        rune_cast_vfx.facing = "default";
                        rune_cast_vfx.age_seconds = 0.0f;
                        rune_cast_vfx.max_cycles = 2;
                        rune_cast_vfx.alive = true;
                        state_.particles.push_back(rune_cast_vfx);
                    };
                    const auto spawn_altar_charge_lightning = [&](const GridCoord& start_cell, const GridCoord& end_cell) {
                        SpawnLightningEffect(CellToWorldCenter(start_cell), CellToWorldCenter(end_cell), 0.3f, false);
                    };
                    spawn_altar_charge_vfx(object.cell);
                    PlaySfxIfVisible(sfx_static_upgrade_.sound, sfx_static_upgrade_.loaded, CellToWorldCenter(object.cell));
                    auto altar_it = std::find_if(altars_.begin(), altars_.end(), [&](const AltarInstance& altar) {
                        return altar.output_cell == object.cell;
                    });
                    if (altar_it != altars_.end()) {
                        for (const int slot_object_id : altar_it->slot_object_ids) {
                            if (const MapObjectInstance* slot_object = FindMapObjectById(slot_object_id)) {
                                spawn_altar_charge_vfx(slot_object->cell);
                                spawn_altar_charge_lightning(slot_object->cell, altar_it->output_cell);
                            }
                        }
                    }
                }
                continue;
            }
            if (object.prototype_id == "fire_flower" && previous_state_it->second == MapObjectState::Spawning &&
                object.state == MapObjectState::Active) {
                PlaySfxIfVisible(sfx_earth_rune_impact_.sound, sfx_earth_rune_impact_.loaded, CellToWorldCenter(object.cell));
            }
            if (previous_state_it->second != MapObjectState::Dying && object.state == MapObjectState::Dying) {
                PlaySfxIfVisible(sfx_vase_breaking_.sound, sfx_vase_breaking_.loaded, CellToWorldCenter(object.cell));
            }
        }
        for (const auto& [object_id, world_pos] : previous_consumable_positions) {
            if (current_object_ids.find(object_id) == current_object_ids.end()) {
                const auto proto_it = previous_consumable_prototype_ids.find(object_id);
                const bool is_charge_pickup =
                    proto_it != previous_consumable_prototype_ids.end() &&
                    GetRuneTypeForChargePickupPrototype(proto_it->second).has_value();
                if (is_charge_pickup) {
                    PlaySfxIfVisible(sfx_static_bolt_impact_.sound, sfx_static_bolt_impact_.loaded, world_pos);
                } else {
                    PlaySfxIfVisible(sfx_item_pickup_.sound, sfx_item_pickup_.loaded, world_pos);
                }
            }
        }
    }
    state_.fire_storm_dummies.clear();
    for (const auto& dummy_snapshot : snapshot.fire_storm_dummies) {
        FireStormDummy dummy;
        dummy.id = dummy_snapshot.id;
        dummy.owner_player_id = dummy_snapshot.owner_player_id;
        dummy.owner_team = dummy_snapshot.owner_team;
        dummy.cell = {dummy_snapshot.cell_x, dummy_snapshot.cell_y};
        dummy.state = static_cast<FireStormDummyState>(dummy_snapshot.state);
        dummy.state_time = dummy_snapshot.state_time;
        dummy.state_duration = dummy_snapshot.state_duration;
        dummy.idle_lifetime_remaining_seconds = dummy_snapshot.idle_lifetime_remaining_seconds;
        dummy.alive = dummy_snapshot.alive;
        state_.fire_storm_dummies.push_back(dummy);
    }
    state_.fire_storm_casts.clear();
    for (const auto& cast_snapshot : snapshot.fire_storm_casts) {
        FireStormCast cast;
        cast.id = cast_snapshot.id;
        cast.owner_player_id = cast_snapshot.owner_player_id;
        cast.owner_team = cast_snapshot.owner_team;
        cast.center_cell = {cast_snapshot.center_cell_x, cast_snapshot.center_cell_y};
        const size_t source_count = std::min(cast_snapshot.source_cell_x.size(), cast_snapshot.source_cell_y.size());
        cast.source_cells.reserve(source_count);
        for (size_t i = 0; i < source_count; ++i) {
            cast.source_cells.push_back({cast_snapshot.source_cell_x[i], cast_snapshot.source_cell_y[i]});
        }
        const size_t target_count = std::min(cast_snapshot.target_cell_x.size(), cast_snapshot.target_cell_y.size());
        cast.target_cells.reserve(target_count);
        for (size_t i = 0; i < target_count; ++i) {
            cast.target_cells.push_back({cast_snapshot.target_cell_x[i], cast_snapshot.target_cell_y[i]});
        }
        cast.elapsed_seconds = cast_snapshot.elapsed_seconds;
        cast.duration_seconds = cast_snapshot.duration_seconds;
        cast.alive = cast_snapshot.alive;
        state_.fire_storm_casts.push_back(cast);
    }
    if (!network_manager_.IsHost()) {
        for (const auto& cast : state_.fire_storm_casts) {
            if (previous_fire_storm_cast_ids.find(cast.id) != previous_fire_storm_cast_ids.end()) {
                continue;
            }
            PlaySfxIfVisible(sfx_fire_storm_cast_.sound, sfx_fire_storm_cast_.loaded,
                             CellToWorldCenter(cast.center_cell));
        }
    }
    state_.earth_roots_groups.clear();
    for (const auto& roots_snapshot : snapshot.earth_roots_groups) {
        EarthRootsGroup group;
        group.id = roots_snapshot.id;
        group.owner_player_id = roots_snapshot.owner_player_id;
        group.owner_team = roots_snapshot.owner_team;
        group.center_cell = {roots_snapshot.center_cell_x, roots_snapshot.center_cell_y};
        group.state = static_cast<EarthRootsGroupState>(roots_snapshot.state);
        group.state_time = roots_snapshot.state_time;
        group.state_duration = roots_snapshot.state_duration;
        group.idle_lifetime_remaining_seconds = roots_snapshot.idle_lifetime_remaining_seconds;
        group.active_for_gameplay = roots_snapshot.active_for_gameplay;
        group.alive = roots_snapshot.alive;
        state_.earth_roots_groups.push_back(group);
    }
    state_.grappling_hooks.clear();
    for (const auto& hook_snapshot : snapshot.grappling_hooks) {
        GrapplingHook hook;
        hook.id = hook_snapshot.id;
        hook.owner_player_id = hook_snapshot.owner_player_id;
        hook.owner_team = hook_snapshot.owner_team;
        hook.head_pos = {hook_snapshot.head_pos_x, hook_snapshot.head_pos_y};
        hook.target_pos = {hook_snapshot.target_pos_x, hook_snapshot.target_pos_y};
        hook.latch_point = {hook_snapshot.latch_point_x, hook_snapshot.latch_point_y};
        hook.pull_destination = {hook_snapshot.pull_destination_x, hook_snapshot.pull_destination_y};
        hook.phase = static_cast<GrapplingHookPhase>(hook_snapshot.phase);
        hook.latch_target_type = static_cast<GrapplingHookLatchTargetType>(hook_snapshot.latch_target_type);
        hook.latch_target_id = hook_snapshot.latch_target_id;
        hook.latch_cell = {hook_snapshot.latch_cell_x, hook_snapshot.latch_cell_y};
        hook.latched = hook_snapshot.latched;
        hook.animation_time = hook_snapshot.animation_time;
        hook.pull_elapsed_seconds = hook_snapshot.pull_elapsed_seconds;
        hook.max_pull_duration_seconds = hook_snapshot.max_pull_duration_seconds;
        hook.alive = hook_snapshot.alive;
        if (!network_manager_.IsHost() && previous_local_owned_hook.has_value() &&
            state_.local_player_id >= 0 && hook.owner_player_id == state_.local_player_id &&
            previous_local_owned_hook->alive &&
            previous_local_owned_hook->phase == hook.phase &&
            (hook.phase == GrapplingHookPhase::Firing || hook.phase == GrapplingHookPhase::Retracting)) {
            hook.head_pos = previous_local_owned_hook->head_pos;
            hook.animation_time = previous_local_owned_hook->animation_time;
        }
        state_.grappling_hooks.push_back(hook);
        if (!network_manager_.IsHost()) {
            auto& samples = grappling_head_position_samples_[hook.id];
            samples.push_back(RemotePositionSample{now_seconds, hook.head_pos});
            while (samples.size() > 32) {
                samples.pop_front();
            }
        }
    }
    if (!network_manager_.IsHost()) {
        std::unordered_set<int> current_hook_ids;
        current_hook_ids.reserve(state_.grappling_hooks.size());
        for (const auto& hook : state_.grappling_hooks) {
            current_hook_ids.insert(hook.id);
        }
        for (auto it = grappling_head_position_samples_.begin(); it != grappling_head_position_samples_.end();) {
            if (current_hook_ids.find(it->first) == current_hook_ids.end()) {
                it = grappling_head_position_samples_.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (!network_manager_.IsHost()) {
        for (const auto& hook : state_.grappling_hooks) {
            const auto previous_phase_it = previous_grappling_hook_phases.find(hook.id);
            if (previous_phase_it == previous_grappling_hook_phases.end()) {
                if (const Player* owner = FindPlayerById(hook.owner_player_id)) {
                    PlaySfxIfVisible(sfx_grappling_throw_.sound, sfx_grappling_throw_.loaded, owner->pos);
                }
                continue;
            }
            if (previous_phase_it->second != GrapplingHookPhase::Pulling &&
                hook.phase == GrapplingHookPhase::Pulling) {
                PlaySfxIfVisible(sfx_grappling_latch_.sound, sfx_grappling_latch_.loaded, hook.latch_point);
            }
        }
        for (const auto& dummy : state_.fire_storm_dummies) {
            if (previous_dummy_ids.find(dummy.id) != previous_dummy_ids.end()) {
                continue;
            }
            fire_storm_dummy_lightning_seconds_remaining_.erase(dummy.id);
            fire_storm_dummy_lightning_cooldown_seconds_remaining_.erase(dummy.id);
        }
    }
    state_.explosions.clear();

    if (!network_manager_.IsHost()) {
        for (const auto& player : state_.players) {
            auto previous_hp_it = previous_hp.find(player.id);
            if (previous_hp_it == previous_hp.end()) {
                continue;
            }
            if (previous_hp_it->second > player.hp) {
                const int damage = previous_hp_it->second - player.hp;
                SpawnDamagePopup(player.pos, damage, false);
                const int zone_tick_damage = static_cast<int>(
                    std::lround(Constants::kArenaUnsafeDamagePerSecond * Constants::kArenaUnsafeDamageTickSeconds));
                if (damage == zone_tick_damage && IsOutsideArena(player.pos)) {
                    std::vector<size_t> loaded_indices;
                    loaded_indices.reserve(sfx_zone_damage_.size());
                    for (size_t i = 0; i < sfx_zone_damage_.size(); ++i) {
                        if (sfx_zone_damage_[i].loaded) {
                            loaded_indices.push_back(i);
                        }
                    }
                    if (!loaded_indices.empty()) {
                        std::uniform_int_distribution<size_t> dist(0, loaded_indices.size() - 1);
                        const LoadedSfx& clip = sfx_zone_damage_[loaded_indices[dist(rng_)]];
                        PlaySfxIfVisible(clip.sound, clip.loaded, player.pos);
                    }
                } else {
                    PlaySfxIfVisible(sfx_player_damaged_.sound, sfx_player_damaged_.loaded, player.pos);
                }
            } else if (previous_hp_it->second < player.hp) {
                SpawnDamagePopup(player.pos, player.hp - previous_hp_it->second, true);
            }

            const auto previous_state_it = previous_player_state.find(player.id);
            if (previous_state_it != previous_player_state.end()) {
                if (previous_state_it->second.alive && !player.alive) {
                    PlaySfxIfVisible(sfx_player_death_.sound, sfx_player_death_.loaded, player.pos);
                }
                if (previous_state_it->second.action_state != PlayerActionState::Slashing &&
                    player.action_state == PlayerActionState::Slashing && ShouldPlayImmediateMeleeSwingSfx(player)) {
                    PlaySfxIfVisible(sfx_melee_attack_.sound, sfx_melee_attack_.loaded, player.pos);
                }
            }

            const auto previous_slots_it = previous_player_item_slots.find(player.id);
            const auto previous_counts_it = previous_player_item_counts.find(player.id);
            if (previous_slots_it != previous_player_item_slots.end() &&
                previous_counts_it != previous_player_item_counts.end()) {
                bool drank_small_potion = false;
                for (size_t i = 0; i < previous_counts_it->second.size() && i < player.item_slot_counts.size(); ++i) {
                    const int previous_count = previous_counts_it->second[i];
                    const int current_count = player.item_slot_counts[i];
                    const std::string& previous_item = previous_slots_it->second[i];
                    const std::string& current_item = player.item_slots[i];
                    const std::string& consumed_item = !previous_item.empty() ? previous_item : current_item;
                    if (previous_count > current_count &&
                        (consumed_item == "health_small" || consumed_item == "mana_small")) {
                        drank_small_potion = true;
                        break;
                    }
                }
                if (drank_small_potion) {
                    PlaySfxIfVisible(sfx_drink_potion_.sound, sfx_drink_potion_.loaded, player.pos);
                }
            }
        }
    }

    if (!network_manager_.IsHost() && state_.local_player_id >= 0) {
        Player* local_player = FindPlayerById(state_.local_player_id);
        if (local_player != nullptr) {
            while (!pending_local_prediction_inputs_.empty() &&
                   pending_local_prediction_inputs_.front().seq <= local_player->last_processed_move_seq) {
                pending_local_prediction_inputs_.pop_front();
            }

            Player replay = *local_player;
            for (const auto& pending_input : pending_local_prediction_inputs_) {
                Vector2 aim_vector = {pending_input.aim_x - replay.pos.x, pending_input.aim_y - replay.pos.y};
                if (replay.melee_active_remaining <= 0.0f && Vector2LengthSqr(aim_vector) > 0.0001f) {
                    replay.aim_dir = Vector2Normalize(aim_vector);
                    replay.facing = AimToFacing(replay.aim_dir);
                }

                Vector2 movement = {pending_input.move_x, pending_input.move_y};
                if (Vector2LengthSqr(movement) > 0.0001f) {
                    movement = Vector2Normalize(movement);
                }
                const float step_dt = pending_input.local_dt > 0.0f ? pending_input.local_dt
                                                                    : static_cast<float>(Constants::kFixedDt);
                const float movement_multiplier = GetPlayerMovementSpeedMultiplier(replay);
                const float base_acceleration = GetPlayerBaseAcceleration(replay);
                const float acceleration_multiplier = GetPlayerAccelerationMultiplier(replay);
                const Vector2 acceleration =
                    Vector2Scale(movement, base_acceleration * movement_multiplier * acceleration_multiplier);
                replay.vel = Vector2Add(replay.vel, Vector2Scale(acceleration, step_dt));
                const float damping = std::max(0.0f, 1.0f - Constants::kPlayerFriction * step_dt);
                replay.vel = Vector2Scale(replay.vel, damping);
                const float speed = Vector2Length(replay.vel);
                if (speed > Constants::kPlayerMaxSpeed * movement_multiplier) {
                    replay.vel =
                        Vector2Scale(Vector2Normalize(replay.vel), Constants::kPlayerMaxSpeed * movement_multiplier);
                }
                replay.pos = Vector2Add(replay.pos, Vector2Scale(replay.vel, step_dt));
                const bool replay_is_pulled = IsPlayerBeingPulled(replay.id);
                CollisionWorld::ResolvePlayerVsWorldLocal(state_.map, replay, !replay_is_pulled);
                ResolvePlayerVsMapObjects(replay);
                ResolvePlayerVsIceWallsLocal(replay);
            }

            const Vector2 base_pos = has_previous_local ? previous_local_pos : local_player->pos;
            const Vector2 base_vel = has_previous_local ? previous_local_vel : local_player->vel;
            const float error = Vector2Distance(base_pos, replay.pos);
            const bool grappling_active = std::any_of(
                state_.grappling_hooks.begin(), state_.grappling_hooks.end(),
                [&](const GrapplingHook& hook) { return hook.alive && hook.owner_player_id == local_player->id; });
            const float hard_snap_threshold = grappling_active ? Constants::kPredictionHardSnapThresholdGrapplingPx
                                                               : Constants::kPredictionHardSnapThresholdPx;
            const float reconcile_gain = grappling_active ? Constants::kPredictionReconcileGainGrappling
                                                          : Constants::kPredictionReconcileGain;
            const float telemetry_threshold =
                grappling_active ? Constants::kPredictionCorrectionTelemetryThresholdGrapplingPx
                                 : Constants::kPredictionCorrectionTelemetryThresholdPx;
            if (error > hard_snap_threshold) {
                local_player->pos = replay.pos;
                local_player->vel = replay.vel;
            } else {
                const float alpha =
                    std::clamp(reconcile_gain * Constants::kNetworkSnapshotIntervalSeconds, 0.0f, 1.0f);
                local_player->pos = Vector2Lerp(base_pos, replay.pos, alpha);
                local_player->vel = Vector2Lerp(base_vel, replay.vel, alpha);
            }
            local_player->aim_dir = replay.aim_dir;
            local_player->facing = replay.facing;

            if (error > telemetry_threshold) {
                network_manager_.AddReconciliationCorrection();
            }
            render_player_positions_[local_player->id] = local_player->pos;
        }
    }

    int max_order = 0;
    int max_entity_id = 0;
    for (const auto& rune : state_.runes) {
        max_order = std::max(max_order, rune.placement_order);
        max_entity_id = std::max(max_entity_id, rune.id);
    }
    for (const auto& projectile : state_.projectiles) {
        max_entity_id = std::max(max_entity_id, projectile.id);
    }
    for (const auto& spirit : state_.fire_spirits) {
        max_entity_id = std::max(max_entity_id, spirit.id);
    }
    for (const auto& segment : state_.fire_wave_segments) {
        max_entity_id = std::max(max_entity_id, segment.id);
    }
    for (const auto& modifier : state_.embers_tile_modifiers) {
        max_entity_id = std::max(max_entity_id, modifier.id);
    }
    for (const auto& wall : state_.ice_walls) {
        max_entity_id = std::max(max_entity_id, wall.id);
    }
    for (const auto& object : state_.map_objects) {
        max_entity_id = std::max(max_entity_id, object.id);
    }
    for (const auto& dummy : state_.fire_storm_dummies) {
        max_entity_id = std::max(max_entity_id, dummy.id);
    }
    for (const auto& cast : state_.fire_storm_casts) {
        max_entity_id = std::max(max_entity_id, cast.id);
    }
    for (const auto& hook : state_.grappling_hooks) {
        max_entity_id = std::max(max_entity_id, hook.id);
    }
    state_.next_rune_placement_order = max_order + 1;
    state_.next_entity_id = max_entity_id + 1;

    if (state_.local_player_id < 0) {
        state_.local_player_id = network_manager_.GetAssignedLocalPlayerId();
    }

    for (auto it = remote_position_samples_.begin(); it != remote_position_samples_.end();) {
        const bool exists = std::any_of(state_.players.begin(), state_.players.end(),
                                        [&](const Player& player) { return player.id == it->first; });
        if (!exists) {
            it = remote_position_samples_.erase(it);
        } else {
            ++it;
        }
    }
}

ServerSnapshotMessage GameApp::BuildHostSnapshot() {
    ServerSnapshotMessage snapshot;
    snapshot.server_tick = ++host_server_tick_;
    snapshot.snapshot_id = snapshot.server_tick;
    snapshot.base_snapshot_id = 0;
    snapshot.is_delta = false;
    snapshot.simulation_time_seconds = state_.match.elapsed_seconds;
    snapshot.time_remaining = state_.match.time_remaining;
    snapshot.zone_enabled = state_.match.zone_enabled;
    snapshot.shrink_tiles_per_second = state_.match.shrink_tiles_per_second;
    snapshot.min_arena_radius_tiles = state_.match.min_arena_radius_tiles;
    snapshot.arena_radius_tiles = state_.match.arena_radius_tiles;
    snapshot.arena_radius_world = state_.match.arena_radius_world;
    snapshot.arena_center_world_x = state_.match.arena_center_world.x;
    snapshot.arena_center_world_y = state_.match.arena_center_world.y;
    snapshot.match_running = state_.match.match_running;
    snapshot.match_finished = state_.match.match_finished;
    snapshot.red_team_kills = state_.match.red_team_kills;
    snapshot.blue_team_kills = state_.match.blue_team_kills;
    snapshot.kill_timeline = state_.match.kill_timeline;

    for (const auto& player : state_.players) {
        snapshot.players.push_back(SnapshotTranslation::BuildPlayerSnapshot(player));
    }

    for (const auto& rune : state_.runes) {
        snapshot.runes.push_back(SnapshotTranslation::BuildRuneSnapshot(rune));
    }

    for (const auto& projectile : state_.projectiles) {
        snapshot.projectiles.push_back(SnapshotTranslation::BuildProjectileSnapshot(projectile));
    }
    for (const auto& spirit : state_.fire_spirits) {
        snapshot.fire_spirits.push_back(SnapshotTranslation::BuildFireSpiritSnapshot(spirit));
    }
    for (const auto& segment : state_.fire_wave_segments) {
        snapshot.fire_wave_segments.push_back(SnapshotTranslation::BuildFireWaveSegmentSnapshot(segment));
    }
    for (const auto& modifier : state_.embers_tile_modifiers) {
        snapshot.embers_tile_modifiers.push_back(SnapshotTranslation::BuildEmbersTileModifierSnapshot(modifier));
    }

    for (const auto& wall : state_.ice_walls) {
        snapshot.ice_walls.push_back(SnapshotTranslation::BuildIceWallSnapshot(wall));
    }

    for (const auto& object : state_.map_objects) {
        snapshot.map_objects.push_back(SnapshotTranslation::BuildMapObjectSnapshot(object));
    }
    for (const auto& castle : state_.castles) {
        snapshot.castles.push_back(SnapshotTranslation::BuildCastleSnapshot(castle));
    }

    for (const auto& dummy : state_.fire_storm_dummies) {
        snapshot.fire_storm_dummies.push_back(SnapshotTranslation::BuildFireStormDummySnapshot(dummy));
    }
    for (const auto& cast : state_.fire_storm_casts) {
        snapshot.fire_storm_casts.push_back(SnapshotTranslation::BuildFireStormCastSnapshot(cast));
    }
    for (const auto& group : state_.earth_roots_groups) {
        snapshot.earth_roots_groups.push_back(SnapshotTranslation::BuildEarthRootsGroupSnapshot(group));
    }
    for (const auto& hook : state_.grappling_hooks) {
        snapshot.grappling_hooks.push_back(SnapshotTranslation::BuildGrapplingHookSnapshot(hook));
    }

    return snapshot;
}

ClientInputMessage GameApp::BuildLocalInput(int local_player_id) {
    ClientInputMessage input;
    input.player_id = local_player_id;
    input.tick = ++local_input_tick_;
    input.seq = ++local_input_seq_;

    input.move_x = 0.0f;
    input.move_y = 0.0f;
    if (IsBindingDown(controls_bindings_.move_left)) input.move_x -= 1.0f;
    if (IsBindingDown(controls_bindings_.move_right)) input.move_x += 1.0f;
    if (IsBindingDown(controls_bindings_.move_up)) input.move_y -= 1.0f;
    if (IsBindingDown(controls_bindings_.move_down)) input.move_y += 1.0f;

    Vector2 mouse_world = GetScreenToWorld2D(GetMousePosition(), camera_);
    input.aim_x = mouse_world.x;
    input.aim_y = mouse_world.y;

    if (app_screen_ == AppScreen::InMatch && in_game_menu_open_) {
        input.move_x = 0.0f;
        input.move_y = 0.0f;
        pending_primary_pressed_ = false;
        pending_grappling_pressed_ = false;
        pending_select_rune_slot_ = -1;
        pending_activate_item_slot_ = -1;
        pending_toggle_inventory_mode_ = false;
        pending_world_drop_request_ = false;
        return input;
    }

    if (app_screen_ == AppScreen::InMatch && chat_input_active_) {
        input.move_x = 0.0f;
        input.move_y = 0.0f;
        pending_primary_pressed_ = false;
        pending_grappling_pressed_ = false;
        pending_select_rune_slot_ = -1;
        pending_activate_item_slot_ = -1;
        pending_toggle_inventory_mode_ = false;
        pending_world_drop_request_ = false;
        return input;
    }

    if (app_screen_ == AppScreen::InMatch) {
        if (const Player* local_player = FindPlayerById(state_.local_player_id);
            local_player != nullptr && local_player->inventory_mode) {
            input.move_x = 0.0f;
            input.move_y = 0.0f;
            input.primary_pressed = false;
            input.grappling_pressed = false;
            input.request_rune_type = static_cast<int>(RuneType::None);
            input.request_item_id.clear();
            input.toggle_inventory_mode = pending_toggle_inventory_mode_;
            pending_primary_pressed_ = false;
            pending_grappling_pressed_ = false;
            pending_select_rune_slot_ = -1;
            pending_activate_item_slot_ = -1;
            pending_toggle_inventory_mode_ = false;
            // Let UpdateMatch package the queued world-drop into a ClientActionMessage.
            return input;
        }
    }

    // Use frame-latched edge inputs to avoid missing events when fixed-step updates
    // do not run on every render frame.
    input.primary_pressed = pending_primary_pressed_;
    input.grappling_pressed = pending_grappling_pressed_;
    if (state_.local_player_id >= 0) {
        if (const Player* local_player = FindPlayerById(state_.local_player_id)) {
            if (pending_select_rune_slot_ >= 0 &&
                pending_select_rune_slot_ < static_cast<int>(local_player->rune_slots.size())) {
                input.request_rune_type = static_cast<int>(local_player->rune_slots[pending_select_rune_slot_]);
            }
            if (pending_activate_item_slot_ >= 0 &&
                pending_activate_item_slot_ < static_cast<int>(local_player->item_slots.size())) {
                input.request_item_id = local_player->item_slots[pending_activate_item_slot_];
            }
        }
    }
    input.toggle_inventory_mode = pending_toggle_inventory_mode_;
    pending_primary_pressed_ = false;
    pending_grappling_pressed_ = false;
    pending_select_rune_slot_ = -1;
    pending_activate_item_slot_ = -1;
    pending_toggle_inventory_mode_ = false;
    return input;
}

void GameApp::SimulateHostGameplay(float dt) {
    const ClientInputMessage host_input = BuildLocalInput(0);
    latest_remote_inputs_[0] = host_input;

    for (const auto& move : network_manager_.ConsumeHostMoveInputs()) {
        ClientInputMessage& input = latest_remote_inputs_[move.player_id];
        input.player_id = move.player_id;
        input.seq = move.seq;
        input.tick = move.tick;
        input.move_x = move.move_x;
        input.move_y = move.move_y;
        input.aim_x = move.aim_x;
        input.aim_y = move.aim_y;
    }

    for (const auto& action : network_manager_.ConsumeHostActionInputs()) {
        ClientInputMessage& input = latest_remote_inputs_[action.player_id];
        input.player_id = action.player_id;
        input.seq = std::max(input.seq, action.seq);
        input.primary_pressed = input.primary_pressed || action.primary_pressed;
        input.grappling_pressed = input.grappling_pressed || action.grappling_pressed;
        if (action.request_rune_type != static_cast<int>(RuneType::None)) input.request_rune_type = action.request_rune_type;
        if (!action.request_item_id.empty()) input.request_item_id = action.request_item_id;
        input.toggle_inventory_mode = input.toggle_inventory_mode || action.toggle_inventory_mode;
        if (action.has_inventory_layout_sync) {
            if (Player* player = FindPlayerById(action.player_id); player != nullptr) {
                ApplyInventoryLayoutSync(*player, action);
            }
        }
        if (action.request_world_drop) {
            if (Player* player = FindPlayerById(action.player_id); player != nullptr) {
                TryDropDraggedSlotToWorld(*player, static_cast<SlotFamily>(action.world_drop_slot_family),
                                          action.world_drop_slot_index, action.world_drop_single_instance);
            }
        }
    }

    for (auto& player : state_.players) {
        auto input_it = latest_remote_inputs_.find(player.id);
        if (input_it == latest_remote_inputs_.end()) {
            continue;
        }
        SimulatePlayerFromInput(player, input_it->second, dt);
    }

    for (auto& [_, input] : latest_remote_inputs_) {
        input.primary_pressed = false;
        input.grappling_pressed = false;
        input.request_rune_type = static_cast<int>(RuneType::None);
        input.request_item_id.clear();
        input.toggle_inventory_mode = false;
    }

    ResolvePlayerCollisions();
    UpdateIceWalls(dt);
    UpdateRunes(dt);
    UpdateCastles(dt);
    UpdateAltars(dt);
    UpdateEarthRootsGroups(dt);
    UpdateArena(dt);

    for (auto& player : state_.players) {
        if (player.melee_active_remaining > 0.0f && player.alive) {
            HandleMeleeHit(player);
        }
    }

    UpdateProjectiles(dt);
    UpdateGrapplingHooks(dt);
    UpdateExplosions(dt);
    UpdateMapObjects(dt);
    UpdateFireSpirits(dt);
    UpdateFireWaveSegments(dt);
    UpdateEmbersTileModifiers(dt);
    UpdateRespawns(dt);
    UpdateDamagePopups(dt);
    most_kills_mode_.Update(state_, event_queue_, dt);
    HandleEventsHost();

    if (state_.match.match_finished) {
        winning_team_ = DetermineWinningTeam(state_.match);
        app_screen_ = AppScreen::PostMatch;
    }
}

void GameApp::SimulatePlayerFromInput(Player& player, const ClientInputMessage& input, float dt) {
    const ActionIntent intent = BuildActionIntent(input);
    player.last_processed_move_seq = intent.seq;

    if (!player.alive) {
        player.vel = {0.0f, 0.0f};
        player.melee_active_remaining = 0.0f;
        player.action_state = PlayerActionState::Idle;
        return;
    }

    player.last_input_tick = intent.tick;
    const bool is_stunned = HasStatusEffect(player, StatusEffectType::Stunned);
    const bool is_pulled = IsPlayerBeingPulled(player.id);
    bool inventory_editing = player.inventory_mode;

    if (!is_stunned && !is_pulled && intent.toggle_inventory_mode) {
        player.inventory_mode = !player.inventory_mode;
        if (!player.inventory_mode) {
            CancelInventoryDrag(player, true);
            if (player.id == state_.local_player_id) {
                local_inventory_ui_mode_ = InventoryUiMode::Closed;
            }
        } else {
            player.rune_placing_mode = false;
            if (player.action_state == PlayerActionState::RunePlacing) {
                player.action_state = PlayerActionState::Idle;
            }
            if (player.id == state_.local_player_id) {
                const CastleState* allied_castle = GetAlliedCastleForPlayer(player);
                local_inventory_ui_mode_ =
                    (allied_castle != nullptr && IsPlayerWithinCastleRange(player, *allied_castle))
                        ? InventoryUiMode::CastleLoadout
                        : InventoryUiMode::Inventory;
            }
        }
        inventory_editing = player.inventory_mode;
    }
    if (!is_stunned && !is_pulled && !inventory_editing && intent.request_rune_type != static_cast<int>(RuneType::None)) {
        const RuneType rune_type = static_cast<RuneType>(intent.request_rune_type);
        if (IsRuneAvailableToPlayer(player, rune_type)) {
            const float mana_cost = GetRuneManaCost(rune_type);
            for (size_t i = 0; i < player.rune_slots.size(); ++i) {
                if (player.rune_slots[i] == rune_type) {
                    player.selected_rune_slot = static_cast<int>(i);
                    break;
                }
            }
            player.selected_rune_type = rune_type;
            player.rune_placing_mode = (rune_type != RuneType::None) &&
                                       (GetPlayerRuneCooldownRemaining(player, rune_type) <= 0.0f) &&
                                       (player.mana >= mana_cost);
        } else {
            player.rune_placing_mode = false;
        }
    }
    Vector2 aim_vector = {intent.aim_world.x - player.pos.x, intent.aim_world.y - player.pos.y};
    if (player.melee_active_remaining <= 0.0f && Vector2LengthSqr(aim_vector) > 0.0001f) {
        player.aim_dir = Vector2Normalize(aim_vector);
        player.facing = AimToFacing(player.aim_dir);
    }

    player.melee_cooldown_remaining = std::max(0.0f, player.melee_cooldown_remaining - dt);
    player.melee_active_remaining = std::max(0.0f, player.melee_active_remaining - dt);
    player.grappling_cooldown_remaining = std::max(0.0f, player.grappling_cooldown_remaining - dt);
    player.mana = std::clamp(player.mana + player.mana_regen_per_second * dt, 0.0f, player.max_mana);
    UpdatePlayerStatusEffects(player, dt);
    for (size_t i = 0; i < player.rune_cooldown_remaining.size(); ++i) {
        player.rune_cooldown_remaining[i] = std::max(0.0f, player.rune_cooldown_remaining[i] - dt);
    }
    for (size_t i = 0; i < player.item_slot_cooldown_remaining.size(); ++i) {
        player.item_slot_cooldown_remaining[i] = std::max(0.0f, player.item_slot_cooldown_remaining[i] - dt);
    }

    if (!is_stunned && !is_pulled && !inventory_editing && !intent.request_item_id.empty()) {
        TryActivateItemById(player, intent.request_item_id);
    }

    const AttackProfile* attack_profile = GetEquippedPrimaryAttack(player);
    if (!is_stunned && !is_pulled && !inventory_editing && intent.primary_pressed) {
        if (player.rune_placing_mode) {
            if (TryPlaceRune(player, intent.aim_world)) {
                player.action_state = PlayerActionState::RunePlacing;
            }
        } else if (attack_profile != nullptr && attack_profile->kind == EquipmentActionKind::Melee &&
                   player.melee_cooldown_remaining <= 0.0f) {
            player.melee_cooldown_remaining = attack_profile->cooldown_seconds;
            player.melee_active_remaining = attack_profile->active_window_seconds;
            player.melee_hit_target_ids.clear();
            player.melee_hit_object_ids.clear();
            player.action_state = attack_profile->action_state;
            if (ShouldPlayImmediateMeleeSwingSfx(player)) {
                PlaySfxIfVisible(sfx_melee_attack_.sound, sfx_melee_attack_.loaded, player.pos);
            }
        }
    }

    const MobilityProfile* mobility_profile = GetEquippedMobility(player);
    if (!is_stunned && !is_pulled && !inventory_editing && intent.mobility_pressed && !player.rune_placing_mode &&
        mobility_profile != nullptr && mobility_profile->kind == EquipmentActionKind::GrapplingHook &&
        player.grappling_cooldown_remaining <= 0.0f) {
        TryStartGrapplingHook(player, intent.aim_world);
    }

    Vector2 movement = intent.move;
    if (Vector2LengthSqr(movement) > 0.0001f) {
        movement = Vector2Normalize(movement);
    }

    const float movement_multiplier = GetPlayerMovementSpeedMultiplier(player);
    const float base_acceleration = GetPlayerBaseAcceleration(player);
    const float acceleration_multiplier = GetPlayerAccelerationMultiplier(player);
    Vector2 acceleration = (is_stunned || is_pulled || inventory_editing)
                               ? Vector2{0.0f, 0.0f}
                               : Vector2Scale(movement, base_acceleration * movement_multiplier *
                                                            acceleration_multiplier);
    player.vel = Vector2Add(player.vel, Vector2Scale(acceleration, dt));

    const float damping = std::max(0.0f, 1.0f - Constants::kPlayerFriction * dt);
    player.vel = Vector2Scale(player.vel, damping);

    const float speed = Vector2Length(player.vel);
    if (speed > Constants::kPlayerMaxSpeed * movement_multiplier) {
        player.vel = Vector2Scale(Vector2Normalize(player.vel), Constants::kPlayerMaxSpeed * movement_multiplier);
    }

    player.pos = Vector2Add(player.pos, Vector2Scale(player.vel, dt));
    CollisionWorld::ResolvePlayerVsWorldLocal(state_.map, player, !is_pulled);
    ResolvePlayerVsMapObjects(player);
    ResolvePlayerVsIceWallsLocal(player);

    if (is_stunned || is_pulled || inventory_editing) {
        player.action_state = PlayerActionState::Idle;
    } else if (player.melee_active_remaining > 0.0f) {
        player.action_state = PlayerActionState::Slashing;
    } else if (player.rune_placing_mode) {
        player.action_state = PlayerActionState::RunePlacing;
    } else if (Vector2LengthSqr(player.vel) > 8.0f) {
        player.action_state = PlayerActionState::Walking;
    } else {
        player.action_state = PlayerActionState::Idle;
    }
}

void GameApp::UpdateArena(float dt) {
    if (!state_.match.match_running || state_.match.match_finished) {
        return;
    }
    if (state_.map.cell_size <= 0) {
        return;
    }
    if (!IsZoneEnabled()) {
        const float map_diameter_tiles = static_cast<float>(std::max(state_.map.width, state_.map.height));
        state_.match.arena_radius_tiles = map_diameter_tiles * 0.5f;
        state_.match.arena_radius_world = state_.match.arena_radius_tiles * static_cast<float>(state_.map.cell_size);
        for (auto& player : state_.players) {
            player.outside_zone_damage_accumulator = 0.0f;
        }
        return;
    }

    if (state_.match.elapsed_seconds >= state_.match.shrink_start_seconds) {
        state_.match.arena_radius_tiles = std::max(
            state_.match.min_arena_radius_tiles,
            state_.match.arena_radius_tiles - state_.match.shrink_tiles_per_second * dt);
    }
    state_.match.arena_radius_world = state_.match.arena_radius_tiles * static_cast<float>(state_.map.cell_size);

    for (auto& player : state_.players) {
        if (!player.alive) {
            continue;
        }

        if (!IsOutsideArena(player.pos)) {
            player.outside_zone_damage_accumulator = 0.0f;
            continue;
        }

        player.outside_zone_damage_accumulator += dt;
        const float tick_seconds = Constants::kArenaUnsafeDamageTickSeconds;
        const int zone_tick_damage =
            static_cast<int>(std::lround(Constants::kArenaUnsafeDamagePerSecond * tick_seconds));
        while (player.outside_zone_damage_accumulator >= tick_seconds) {
            player.outside_zone_damage_accumulator -= tick_seconds;
            ApplyDamageToPlayer(player, -1, zone_tick_damage, "outside_zone", false);
        }
    }
}

void GameApp::UpdateRespawns(float dt) {
    for (auto& player : state_.players) {
        if (!player.awaiting_respawn) {
            continue;
        }
        player.respawn_remaining = std::max(0.0f, player.respawn_remaining - dt);
        if (player.respawn_remaining > 0.0f) {
            continue;
        }

        player.awaiting_respawn = false;
        player.alive = true;
        player.hp = Constants::kMaxHp;
        player.vel = {0.0f, 0.0f};
        player.rune_placing_mode = false;
        player.melee_active_remaining = 0.0f;
        player.melee_cooldown_remaining = 0.0f;
        player.outside_zone_damage_accumulator = 0.0f;
        player.action_state = PlayerActionState::Idle;
        player.status_effects.clear();
        player.pos = ComputeRespawnPosition(player);
        CollisionWorld::ResolvePlayerVsWorld(state_.map, player);
        ResolvePlayerVsMapObjects(player);
        ResolvePlayerVsIceWalls(player);
        render_player_positions_[player.id] = player.pos;
    }
}

void GameApp::UpdatePlayerStatusEffects(Player& player, float dt) {
    if (!player.alive) {
        return;
    }

    std::unordered_map<int, int> pending_root_damage_by_source;
    struct PendingBurnDamage {
        int spirit_id = -1;
        int owner_player_id = -1;
        int damage = 0;
    };
    std::vector<PendingBurnDamage> pending_burn_damage;
    for (auto& status : player.status_effects) {
        if (status.remaining_seconds <= 0.0f) {
            continue;
        }
        const float applied_dt = std::min(dt, status.remaining_seconds);
        status.remaining_seconds = std::max(0.0f, status.remaining_seconds - dt);

        switch (status.type) {
            case StatusEffectType::Regeneration: {
                status.accumulated_magnitude += status.magnitude_per_second * applied_dt;
                const int whole_heal = static_cast<int>(std::floor(status.accumulated_magnitude + 0.0001f));
                if (whole_heal > 0) {
                    status.accumulated_magnitude -= static_cast<float>(whole_heal);
                    ApplyImmediateHeal(player, whole_heal);
                }
                break;
            }
            case StatusEffectType::ManaRegeneration: {
                status.accumulated_magnitude += status.magnitude_per_second * applied_dt;
                const int whole_mana = static_cast<int>(std::floor(status.accumulated_magnitude + 0.0001f));
                if (whole_mana > 0) {
                    status.accumulated_magnitude -= static_cast<float>(whole_mana);
                    ApplyImmediateManaRestore(player, whole_mana);
                }
                break;
            }
            case StatusEffectType::Stunned:
                break;
            case StatusEffectType::Frozen:
                status.movement_speed_multiplier = Constants::kFrozenMovementSpeedMultiplier;
                break;
            case StatusEffectType::Burning: {
                status.accumulated_magnitude += Constants::kBurningDamagePerSecond * applied_dt;
                const int whole_damage = static_cast<int>(std::floor(status.accumulated_magnitude + 0.0001f));
                if (whole_damage > 0) {
                    status.accumulated_magnitude -= static_cast<float>(whole_damage);
                    bool merged = false;
                    for (auto& pending : pending_burn_damage) {
                        if (pending.spirit_id == status.origin_source_id &&
                            pending.owner_player_id == status.source_owner_player_id) {
                            pending.damage += whole_damage;
                            merged = true;
                            break;
                        }
                    }
                    if (!merged) {
                        pending_burn_damage.push_back(
                            {status.origin_source_id, status.source_owner_player_id, whole_damage});
                    }
                }
                break;
            }
            case StatusEffectType::Rooted: {
                status.source_elapsed_seconds += applied_dt;
                const float burn_duration = std::max(0.001f, status.burn_duration_seconds);
                const float normalized = std::clamp(status.source_elapsed_seconds / burn_duration, 0.0f, 1.0f);
                status.progress = normalized;
                status.movement_speed_multiplier = 1.0f - SmoothRootSigmoid(normalized);
                status.accumulated_magnitude += Constants::kEarthRootedDamagePerSecond * applied_dt;
                const int whole_damage = static_cast<int>(std::floor(status.accumulated_magnitude + 0.0001f));
                if (whole_damage > 0) {
                    status.accumulated_magnitude -= static_cast<float>(whole_damage);
                    pending_root_damage_by_source[status.source_id] += whole_damage;
                }
                if (status.source_active) {
                    status.remaining_seconds =
                        std::max(status.remaining_seconds, Constants::kEarthRootedVisibleDurationSeconds);
                    status.total_seconds = std::max(status.total_seconds, Constants::kEarthRootedVisibleDurationSeconds);
                }
                break;
            }
            case StatusEffectType::RootedRecovery: {
                status.source_elapsed_seconds += applied_dt;
                const float recover_duration = std::max(0.001f, status.burn_duration_seconds);
                const float normalized = std::clamp(status.source_elapsed_seconds / recover_duration, 0.0f, 1.0f);
                const float base_progress = std::clamp(status.progress, 0.0f, 1.0f);
                const float active_progress = std::max(0.0f, base_progress * (1.0f - normalized));
                status.movement_speed_multiplier = 1.0f - SmoothRootSigmoid(active_progress);
                break;
            }
        }
    }

    if (!pending_burn_damage.empty() && player.alive) {
        for (const auto& pending : pending_burn_damage) {
            if (pending.damage <= 0) {
                continue;
            }
            ApplyDamageToPlayer(player, pending.owner_player_id, pending.damage, "burning",
                                pending.owner_player_id >= 0, player.pos);
            if (!player.alive) {
                break;
            }
        }
    }

    if (!pending_root_damage_by_source.empty() && player.alive) {
        for (const auto& [source_id, pending_root_damage] : pending_root_damage_by_source) {
            if (pending_root_damage <= 0) {
                continue;
            }

            const int root_source_id = source_id;
            int owner_player_id = -1;
            std::optional<Vector2> damage_world_pos = std::nullopt;
            const auto roots_it = std::find_if(state_.earth_roots_groups.begin(), state_.earth_roots_groups.end(),
                                               [&](const EarthRootsGroup& group) { return group.id == root_source_id; });
            if (roots_it != state_.earth_roots_groups.end()) {
                owner_player_id = roots_it->owner_player_id;
                damage_world_pos = CellToWorldCenter(roots_it->center_cell);
            }

            ApplyDamageToPlayer(player, owner_player_id, pending_root_damage, "earth_roots", owner_player_id >= 0,
                                damage_world_pos);
        }
    }

    if (HasStatusEffect(player, StatusEffectType::Stunned)) {
        player.rune_placing_mode = false;
        player.inventory_mode = false;
        player.melee_active_remaining = 0.0f;
        player.vel = {0.0f, 0.0f};
        player.action_state = PlayerActionState::Idle;
    }

    player.status_effects.erase(
        std::remove_if(player.status_effects.begin(), player.status_effects.end(),
                       [](const StatusEffectInstance& status) { return status.remaining_seconds <= 0.0f; }),
        player.status_effects.end());
}

float GameApp::GetPlayerMovementSpeedMultiplier(const Player& player) const {
    const int catalyst_charges = GetPlayerRuneChargeCount(player, RuneType::Catalyst);
    float multiplier = std::clamp(
        1.0f - static_cast<float>(catalyst_charges) * Constants::kCatalystChargeMovementPenaltyPerStack, 0.0f, 1.0f);
    for (const auto& status : player.status_effects) {
        if (status.remaining_seconds <= 0.0f) {
            continue;
        }
        if (status.type == StatusEffectType::Rooted || status.type == StatusEffectType::RootedRecovery ||
            status.type == StatusEffectType::Frozen) {
            multiplier = std::min(multiplier, std::clamp(status.movement_speed_multiplier, 0.0f, 1.0f));
        }
    }
    return multiplier;
}

float GameApp::GetPlayerRuneCastRangeWorld(const Player& player) const {
    const int catalyst_charges = GetPlayerRuneChargeCount(player, RuneType::Catalyst);
    const float range_tiles =
        std::max(0.0f, Constants::kRuneCastRangeTiles -
                           static_cast<float>(catalyst_charges) * Constants::kCatalystChargeRangePenaltyTilesPerStack);
    return range_tiles * static_cast<float>(Constants::kRuneCellSize);
}

float GameApp::GetPlayerGrapplingRangeWorld(const Player& player) const {
    const MobilityProfile* mobility_profile = GetEquippedMobility(player);
    if (mobility_profile == nullptr || mobility_profile->kind != EquipmentActionKind::GrapplingHook) {
        return 0.0f;
    }
    return std::max(0.0f, mobility_profile->range_tiles * static_cast<float>(state_.map.cell_size));
}

bool GameApp::CanPlayerStartGrapplingPreview(const Player& player) const {
    if (!player.alive || player.inventory_mode || player.rune_placing_mode || player.grappling_cooldown_remaining > 0.0f ||
        IsPlayerBeingPulled(player.id) || HasStatusEffect(player, StatusEffectType::Stunned)) {
        return false;
    }
    const MobilityProfile* mobility_profile = GetEquippedMobility(player);
    return mobility_profile != nullptr && mobility_profile->kind == EquipmentActionKind::GrapplingHook;
}

float GameApp::GetPlayerBaseAcceleration(const Player& player) const {
    float acceleration = Constants::kPlayerAcceleration;
    const EquipmentItemDefinition* item =
        equipment_registry_.ResolveEquippedItem(player, EquipmentSlot::PrimaryWeapon);
    if (item != nullptr && item->id == "hammer_item") {
        acceleration = std::max(0.0f, acceleration - Constants::kHammerAccelerationPenalty);
    }
    return acceleration;
}

float GameApp::GetPlayerAccelerationMultiplier(const Player& player) const {
    const AttackProfile* attack_profile = GetEquippedPrimaryAttack(player);
    const EquipmentItemDefinition* item =
        equipment_registry_.ResolveEquippedItem(player, EquipmentSlot::PrimaryWeapon);
    if (attack_profile == nullptr || attack_profile->kind != EquipmentActionKind::Melee || item == nullptr ||
        item->id != "hammer_item" || player.melee_active_remaining <= 0.0f) {
        return 1.0f;
    }

    const float attack_elapsed = attack_profile->active_window_seconds - player.melee_active_remaining;
    float impact_time_seconds = attack_profile->hit_start_seconds;
    const std::string attack_tag = ResolvePrimaryWeaponAttackModularTag(player);
    if (!attack_tag.empty()) {
        const float modular_impact_time =
            GetModularTagFrameStartSeconds(attack_tag, Constants::kHammerImpactEventFrame - 1);
        if (modular_impact_time > 0.0f) {
            impact_time_seconds = std::min(attack_profile->active_window_seconds, modular_impact_time);
        }
    }

    return attack_elapsed < impact_time_seconds ? Constants::kHammerAnticipationAccelerationMultiplier : 1.0f;
}

void GameApp::UpdateDamagePopups(float dt) {
    for (auto& popup : state_.damage_popups) {
        if (!popup.alive) {
            continue;
        }
        popup.age_seconds += dt;
        popup.pos.y -= popup.rise_per_second * dt;
        if (popup.age_seconds >= popup.lifetime_seconds) {
            popup.alive = false;
        }
    }

    state_.damage_popups.erase(
        std::remove_if(state_.damage_popups.begin(), state_.damage_popups.end(),
                       [](const DamagePopup& popup) { return !popup.alive; }),
        state_.damage_popups.end());
}

MapObjectInstance* GameApp::FindMapObjectById(int id) {
    const auto it = map_object_index_by_id_.find(id);
    if (it == map_object_index_by_id_.end() || it->second >= state_.map_objects.size()) {
        return nullptr;
    }
    return &state_.map_objects[it->second];
}

const MapObjectInstance* GameApp::FindMapObjectById(int id) const {
    const auto it = map_object_index_by_id_.find(id);
    if (it == map_object_index_by_id_.end() || it->second >= state_.map_objects.size()) {
        return nullptr;
    }
    return &state_.map_objects[it->second];
}

const ObjectPrototype* GameApp::FindObjectPrototype(const std::string& prototype_id) const {
    return objects_database_.FindById(prototype_id);
}

int GameApp::SpawnObjectInstanceAtCell(const std::string& prototype_id, const GridCoord& cell, int forced_id) {
    const ObjectPrototype* proto = FindObjectPrototype(prototype_id);
    if (proto == nullptr || proto->type == ObjectType::Terrain) {
        return -1;
    }
    if (!state_.map.IsInside(cell)) {
        return -1;
    }

    MapObjectInstance object;
    object.id = forced_id > 0 ? forced_id : state_.next_entity_id++;
    object.owner_player_id = -1;
    object.owner_team = 0;
    if (forced_id > 0) {
        state_.next_entity_id = std::max(state_.next_entity_id, forced_id + 1);
    }
    object.prototype_id = prototype_id;
    object.cell = cell;
    object.type = proto->type;
    object.walkable = proto->walkable;
    object.stops_projectiles = proto->stops_projectiles;
    object.collision_enabled = (!proto->walkable || proto->stops_projectiles);
    if (proto->type == ObjectType::Destructible) {
        object.hp = proto->destructible_hp;
    } else if (proto->type == ObjectType::Unit) {
        object.hp = proto->unit_hp;
    } else {
        object.hp = 0;
    }
    object.state = MapObjectState::Active;
    object.state_time = 0.0f;
    object.death_duration = 0.0f;
    object.alive = true;
    state_.map_objects.push_back(object);
    map_object_index_by_id_[object.id] = state_.map_objects.size() - 1;
    MarkMapObjectRenderCachesDirty();
    return object.id;
}

int GameApp::SpawnDroppedEquipmentItem(const std::string& prototype_id, const GridCoord& cell,
                                       std::optional<int> blocked_player_id) {
    const int reserved_object_id = state_.next_entity_id++;
    pending_object_spawns_.push_back({prototype_id, cell, blocked_player_id, reserved_object_id});
    return reserved_object_id;
}

void GameApp::RegisterPickupBlockForDroppedObject(int object_instance_id, int blocked_player_id, Vector2 origin_world,
                                                  float unlock_radius) {
    if (object_instance_id < 0 || blocked_player_id < 0) {
        return;
    }
    dropped_item_pickup_blocks_[object_instance_id] =
        DroppedItemPickupBlock{object_instance_id, blocked_player_id, origin_world, unlock_radius};
}

bool GameApp::CanPlayerPickUpObject(const Player& player, const MapObjectInstance& object) const {
    if (const auto block_it = dropped_item_pickup_blocks_.find(object.id); block_it != dropped_item_pickup_blocks_.end()) {
        if (block_it->second.blocked_player_id == player.id) {
            return false;
        }
    }

    const EquipmentItemDefinition* equipment_item = equipment_registry_.FindItem(object.prototype_id);
    if (equipment_item != nullptr && equipment_item->slot != EquipmentSlot::Inventory) {
        const size_t slot_index = equipment_item->slot == EquipmentSlot::PrimaryWeapon ? 0 : 1;
        if (!settings_.auto_pick_replace_equipment && slot_index < player.weapon_slots.size() &&
            !player.weapon_slots[slot_index].empty() && player.weapon_slots[slot_index] != object.prototype_id) {
            return false;
        }
        return Vector2Distance(player.pos, CellToWorldCenter(object.cell)) <= Constants::kDroppedEquipmentPickupRadius;
    }
    return true;
}

void GameApp::UpdateDroppedItemPickupBlocks() {
    for (auto it = dropped_item_pickup_blocks_.begin(); it != dropped_item_pickup_blocks_.end();) {
        const MapObjectInstance* object = FindMapObjectById(it->second.object_instance_id);
        const Player* player = FindPlayerById(it->second.blocked_player_id);
        if (object == nullptr || !object->alive || player == nullptr || !player->alive ||
            Vector2Distance(player->pos, it->second.origin_world) > it->second.unlock_radius) {
            it = dropped_item_pickup_blocks_.erase(it);
        } else {
            ++it;
        }
    }
}

void GameApp::RebuildMapObjectsFromSeeds() {
    state_.map_objects.clear();
    map_object_index_by_id_.clear();
    dropped_item_pickup_blocks_.clear();
    for (const auto& seed : state_.map.object_seeds) {
        SpawnObjectInstanceAtCell(seed.prototype_id, seed.cell);
    }
    RebuildStaticRenderCaches();
    RebuildAltarsFromMapObjects();
}

void GameApp::EnsureMapObjectRenderCaches() {
    if (!map_object_render_caches_dirty_) {
        return;
    }
    RebuildStaticRenderCaches();
}

void GameApp::MarkMapObjectRenderCachesDirty() { map_object_render_caches_dirty_ = true; }

void GameApp::RebuildMapObjectIndexLookup() {
    map_object_index_by_id_.clear();
    map_object_index_by_id_.reserve(state_.map_objects.size());
    for (size_t object_index = 0; object_index < state_.map_objects.size(); ++object_index) {
        map_object_index_by_id_[state_.map_objects[object_index].id] = object_index;
    }
}

void GameApp::RebuildStaticRenderCaches() {
    static_decoration_render_chunks_.clear();
    flat_map_object_indices_.clear();
    ysorted_map_object_indices_.clear();
    shadow_map_object_indices_.clear();
    static_decoration_chunk_cols_ = 0;
    static_decoration_chunk_rows_ = 0;
    map_object_render_caches_dirty_ = false;
    RebuildMapObjectIndexLookup();
    if (state_.map.width <= 0 || state_.map.height <= 0) {
        return;
    }

    static_decoration_chunk_cols_ =
        (state_.map.width + kStaticRenderChunkSizeTiles - 1) / kStaticRenderChunkSizeTiles;
    static_decoration_chunk_rows_ =
        (state_.map.height + kStaticRenderChunkSizeTiles - 1) / kStaticRenderChunkSizeTiles;
    static_decoration_render_chunks_.resize(static_cast<size_t>(static_decoration_chunk_cols_ * static_decoration_chunk_rows_));

    for (size_t i = 0; i < state_.map.decorations.size(); ++i) {
        if (state_.map.decorations[i].empty()) {
            continue;
        }
        const int cell_x = static_cast<int>(i % static_cast<size_t>(std::max(1, state_.map.width)));
        const int cell_y = static_cast<int>(i / static_cast<size_t>(std::max(1, state_.map.width)));
        const int chunk_x = cell_x / kStaticRenderChunkSizeTiles;
        const int chunk_y = cell_y / kStaticRenderChunkSizeTiles;
        const size_t chunk_index = static_cast<size_t>(chunk_y * static_decoration_chunk_cols_ + chunk_x);
        static_decoration_render_chunks_[chunk_index].push_back(
            {((static_cast<float>(cell_y) + 1.0f) * static_cast<float>(state_.map.cell_size)), i});
    }

    flat_map_object_indices_.reserve(state_.map_objects.size());
    ysorted_map_object_indices_.reserve(state_.map_objects.size());
    shadow_map_object_indices_.reserve(state_.map_objects.size());
    for (size_t object_index = 0; object_index < state_.map_objects.size(); ++object_index) {
        const auto& object = state_.map_objects[object_index];
        if (!object.alive) {
            continue;
        }
        const ObjectPrototype* proto = FindObjectPrototype(object.prototype_id);
        if (proto == nullptr) {
            continue;
        }
        if (IsFlatRenderedMapObjectPrototype(proto)) {
            flat_map_object_indices_.push_back(object_index);
        } else {
            ysorted_map_object_indices_.push_back(object_index);
        }
        if (proto->casts_shadow && !proto->shadow_animation.empty()) {
            shadow_map_object_indices_.push_back(object_index);
        }
    }
}

void GameApp::RebuildAltarsFromMapObjects() {
    altars_.clear();

    struct SlotAssignmentCandidate {
        int output_object_id = -1;
        int distance_sq = std::numeric_limits<int>::max();
    };
    std::unordered_map<int, SlotAssignmentCandidate> slot_assignments;

    for (const auto& object : state_.map_objects) {
        if (!object.alive || object.prototype_id != "altar_output_simple") {
            continue;
        }
        AltarInstance altar;
        altar.output_object_id = object.id;
        altar.output_cell = object.cell;
        altars_.push_back(altar);
    }

    for (const auto& slot : state_.map_objects) {
        if (!slot.alive || slot.prototype_id != "altar_rune_slot_generic") {
            continue;
        }

        for (const auto& altar : altars_) {
            const int dx = slot.cell.x - altar.output_cell.x;
            const int dy = slot.cell.y - altar.output_cell.y;
            if (std::abs(dx) > Constants::kAltarScanHalfExtentTiles ||
                std::abs(dy) > Constants::kAltarScanHalfExtentTiles) {
                continue;
            }

            const int distance_sq = dx * dx + dy * dy;
            auto& best = slot_assignments[slot.id];
            if (best.output_object_id < 0 || distance_sq < best.distance_sq ||
                (distance_sq == best.distance_sq && altar.output_object_id < best.output_object_id)) {
                best.output_object_id = altar.output_object_id;
                best.distance_sq = distance_sq;
            }
        }
    }

    for (auto& altar : altars_) {
        altar.slot_object_ids.clear();
        for (const auto& [slot_id, assignment] : slot_assignments) {
            if (assignment.output_object_id == altar.output_object_id) {
                altar.slot_object_ids.push_back(slot_id);
            }
        }
        std::sort(altar.slot_object_ids.begin(), altar.slot_object_ids.end());
        altar.required_slot_count = static_cast<int>(altar.slot_object_ids.size());
        altar.fulfilled_slot_rune_ids.clear();
    }
}

void GameApp::UpdateAltars(float /*dt*/) {
    if (!network_manager_.IsHost() || altars_.empty()) {
        return;
    }

    bool removed_runes = false;
    std::vector<int> consumed_rune_ids;

    for (auto& altar : altars_) {
        altar.fulfilled_slot_rune_ids.clear();
        if (altar.required_slot_count <= 0) {
            continue;
        }

        bool has_charge_present = false;
        for (const auto& object : state_.map_objects) {
            if (!object.alive || !(object.cell == altar.output_cell)) {
                continue;
            }
            if (object.prototype_id == "catalyst_charge_pickup") {
                has_charge_present = true;
                break;
            }
        }
        if (has_charge_present) {
            continue;
        }

        for (const int slot_object_id : altar.slot_object_ids) {
            const MapObjectInstance* slot_object = FindMapObjectById(slot_object_id);
            if (slot_object == nullptr || !slot_object->alive) {
                continue;
            }
            auto rune_it = std::find_if(state_.runes.begin(), state_.runes.end(), [&](const Rune& rune) {
                return rune.active && rune.cell == slot_object->cell;
            });
            if (rune_it == state_.runes.end()) {
                altar.fulfilled_slot_rune_ids.clear();
                break;
            }
            altar.fulfilled_slot_rune_ids.push_back(rune_it->id);
        }

        if (static_cast<int>(altar.fulfilled_slot_rune_ids.size()) != altar.required_slot_count) {
            altar.fulfilled_slot_rune_ids.clear();
            continue;
        }

        SpawnObjectInstanceAtCell("catalyst_charge_pickup", altar.output_cell);
        const auto spawn_altar_charge_vfx = [&](const GridCoord& cell) {
            Particle rune_cast_vfx;
            rune_cast_vfx.pos = CellToWorldCenter(cell);
            rune_cast_vfx.vel = {0.0f, 0.0f};
            rune_cast_vfx.acc = {0.0f, 0.0f};
            rune_cast_vfx.drag = 0.0f;
            rune_cast_vfx.animation_key = "rune_cast_effect";
            rune_cast_vfx.facing = "default";
            rune_cast_vfx.age_seconds = 0.0f;
            rune_cast_vfx.max_cycles = 2;
            rune_cast_vfx.alive = true;
            state_.particles.push_back(rune_cast_vfx);
        };
        const auto spawn_altar_charge_lightning = [&](const GridCoord& start_cell, const GridCoord& end_cell) {
            SpawnLightningEffect(CellToWorldCenter(start_cell), CellToWorldCenter(end_cell), 0.3f, false);
        };
        spawn_altar_charge_vfx(altar.output_cell);
        PlaySfxIfVisible(sfx_static_upgrade_.sound, sfx_static_upgrade_.loaded, CellToWorldCenter(altar.output_cell));
        for (const int slot_object_id : altar.slot_object_ids) {
            if (const MapObjectInstance* slot_object = FindMapObjectById(slot_object_id)) {
                spawn_altar_charge_vfx(slot_object->cell);
                spawn_altar_charge_lightning(slot_object->cell, altar.output_cell);
            }
        }
        consumed_rune_ids.insert(consumed_rune_ids.end(), altar.fulfilled_slot_rune_ids.begin(),
                                 altar.fulfilled_slot_rune_ids.end());
        altar.fulfilled_slot_rune_ids.clear();
    }

    if (!consumed_rune_ids.empty()) {
        std::sort(consumed_rune_ids.begin(), consumed_rune_ids.end());
        consumed_rune_ids.erase(std::unique(consumed_rune_ids.begin(), consumed_rune_ids.end()), consumed_rune_ids.end());
        state_.runes.erase(std::remove_if(state_.runes.begin(), state_.runes.end(),
                                          [&](const Rune& rune) {
                                              return std::binary_search(consumed_rune_ids.begin(), consumed_rune_ids.end(),
                                                                        rune.id);
                                          }),
                           state_.runes.end());
        removed_runes = true;
    }

    if (removed_runes) {
        RebuildInfluenceZones();
    }
}

void GameApp::ResolvePlayerVsMapObjects(Player& player) {
    if (!player.alive) {
        return;
    }

    for (const auto& object : state_.map_objects) {
        if (!object.alive || !object.collision_enabled || object.walkable) {
            continue;
        }

        const Rectangle aabb = GetMapObjectCollisionAabb(object, FindObjectPrototype(object.prototype_id));
        Vector2 normal = {0.0f, 0.0f};
        float penetration = 0.0f;
        if (!CollisionWorld::AabbVsAabb(GetPlayerCollisionRect(player), aabb, normal, penetration)) {
            continue;
        }

        player.pos = Vector2Add(player.pos, Vector2Scale(normal, penetration));
        const float velocity_dot = Vector2DotProduct(player.vel, normal);
        if (velocity_dot < 0.0f) {
            player.vel = Vector2Subtract(player.vel, Vector2Scale(normal, velocity_dot));
        }
    }
}

bool GameApp::ApplyObjectDamage(int object_instance_id, int amount, int /*source_player_id*/, const char* source,
                                std::optional<Vector2> damage_world_pos) {
    MapObjectInstance* object = FindMapObjectById(object_instance_id);
    if (object == nullptr || !object->alive || amount <= 0 || object->hp <= 0) {
        return false;
    }
    if (object->state == MapObjectState::Spawning) {
        return false;
    }

    const ObjectPrototype* proto = FindObjectPrototype(object->prototype_id);
    if (proto == nullptr || (proto->type != ObjectType::Destructible && proto->type != ObjectType::Unit)) {
        return false;
    }

    object->hp = std::max(0, object->hp - amount);
    const Vector2 object_world_pos = CellToWorldCenter(object->cell);
    SpawnDamagePopup(object_world_pos, amount, false);
    SpawnDamageHitParticles(object_world_pos, damage_world_pos);

    const bool is_melee_hit = (source != nullptr && std::strcmp(source, "melee") == 0);
    if (object->hp > 0) {
        const LoadedSfx& hit_sfx = is_melee_hit ? sfx_player_damaged_ : sfx_static_bolt_impact_;
        PlaySfxIfVisible(hit_sfx.sound, hit_sfx.loaded, object_world_pos);
        return true;
    }

    if (proto->type == ObjectType::Unit) {
        object->collision_enabled = false;
        object->alive = false;
        return true;
    }

    PlaySfxIfVisible(sfx_vase_breaking_.sound, sfx_vase_breaking_.loaded, object_world_pos);

    object->collision_enabled = false;

    const std::vector<LootSpawn> resolved_drops =
        loot_table_library_.ResolveDrops(object->prototype_id, proto->on_destroy_drops, &loot_quota_remaining_, rng_);
    for (const LootSpawn& drop : resolved_drops) {
        for (int i = 0; i < std::max(1, drop.count); ++i) {
            if (const EquipmentItemDefinition* dropped_item = equipment_registry_.FindItem(drop.object_id);
                dropped_item != nullptr && dropped_item->slot != EquipmentSlot::Inventory) {
                SpawnDroppedEquipmentItem(drop.object_id, object->cell);
            } else {
                pending_object_spawns_.push_back({drop.object_id, object->cell});
            }
        }
    }

    if (!proto->death_animation.empty()) {
        object->state = MapObjectState::Dying;
        object->state_time = 0.0f;
        object->death_duration = 0.4f;

        int frame_count = 0;
        float fps = 1.0f;
        const SpriteMetadataLoader* metadata = &sprite_metadata_;
        switch (proto->sprite_sheet) {
            case SpriteSheetType::Base32:
                metadata = &sprite_metadata_;
                break;
            case SpriteSheetType::Tall32x64:
                metadata = &sprite_metadata_tall_;
                break;
            case SpriteSheetType::Large128x128:
                metadata = &sprite_metadata_128x128_;
                break;
        }
        if (metadata->GetAnimationStats(proto->death_animation, "default", frame_count, fps) && frame_count > 0) {
            object->death_duration = static_cast<float>(frame_count) / std::max(0.001f, fps);
        }
    } else {
        object->alive = false;
    }

    return true;
}

bool GameApp::TryConsumeObject(int object_instance_id, int player_id) {
    MapObjectInstance* object = FindMapObjectById(object_instance_id);
    Player* player = FindPlayerById(player_id);
    if (object == nullptr || player == nullptr || !object->alive || !player->alive) {
        return false;
    }

    const ObjectPrototype* proto = FindObjectPrototype(object->prototype_id);
    if (proto == nullptr || proto->type != ObjectType::Consumable) {
        return false;
    }

    if (const std::optional<RuneType> charge_rune_type = GetRuneTypeForChargePickupPrototype(object->prototype_id);
        charge_rune_type.has_value()) {
        if (GetPlayerRuneChargeCount(*player, *charge_rune_type) >= GetMaxRuneCharges(*charge_rune_type)) {
            return false;
        }
        SetPlayerRuneChargeCount(*player, *charge_rune_type, GetPlayerRuneChargeCount(*player, *charge_rune_type) + 1);
        object->collision_enabled = false;
        object->alive = false;
        PlaySfxIfVisible(sfx_static_bolt_impact_.sound, sfx_static_bolt_impact_.loaded, CellToWorldCenter(object->cell));
        return true;
    }

    if (const EquipmentItemDefinition* equipment_item = equipment_registry_.FindItem(object->prototype_id);
        equipment_item != nullptr && equipment_item->slot != EquipmentSlot::Inventory) {
        if (!TryEquipItem(*player, object->prototype_id, object)) {
            return false;
        }
        object->collision_enabled = false;
        object->alive = false;
        PlaySfxIfVisible(sfx_item_pickup_.sound, sfx_item_pickup_.loaded, CellToWorldCenter(object->cell));
        return true;
    }

    int target_slot = -1;
    for (size_t i = 0; i < player->item_slots.size(); ++i) {
        if (player->item_slots[i] == object->prototype_id && player->item_slot_counts[i] > 0) {
            target_slot = static_cast<int>(i);
            break;
        }
    }
    if (target_slot < 0) {
        for (size_t i = 0; i < player->item_slots.size(); ++i) {
            if (player->item_slots[i].empty() || player->item_slot_counts[i] <= 0) {
                target_slot = static_cast<int>(i);
                break;
            }
        }
    }
    if (target_slot < 0) {
        return false;
    }

    player->item_slots[target_slot] = object->prototype_id;
    player->item_slot_counts[target_slot] += 1;
    object->collision_enabled = false;
    object->alive = false;
    PlaySfxIfVisible(sfx_item_pickup_.sound, sfx_item_pickup_.loaded, CellToWorldCenter(object->cell));
    return true;
}

bool GameApp::TryActivateItemSlot(Player& player, int slot_index) {
    if (slot_index < 0 || slot_index >= static_cast<int>(player.item_slots.size())) {
        return false;
    }
    if (player.item_slots[slot_index].empty() || player.item_slot_counts[slot_index] <= 0) {
        return false;
    }
    if (player.item_slot_cooldown_remaining[slot_index] > 0.0f) {
        return false;
    }

    const ObjectPrototype* proto = FindObjectPrototype(player.item_slots[slot_index]);
    if (proto == nullptr || proto->type != ObjectType::Consumable) {
        return false;
    }

    const std::string prototype_id = player.item_slots[slot_index];

    for (const EffectSpec& effect : proto->consumable_effects) {
        if (effect.type == EffectType::IncreaseCurrentHealth) {
            ApplyImmediateHeal(player, std::max(0, effect.amount));
        } else if (effect.type == EffectType::IncreaseCurrentMana) {
            ApplyImmediateManaRestore(player, std::max(0, effect.amount));
        } else if (effect.type == EffectType::SpawnObject) {
            const int min_count = std::max(1, effect.min_count);
            const int max_count = std::max(min_count, effect.max_count);
            std::uniform_int_distribution<int> count_dist(min_count, max_count);
            const int count = count_dist(rng_);
            for (int i = 0; i < count; ++i) {
                pending_object_spawns_.push_back({effect.object_id, WorldToCell(player.pos)});
            }
        }
    }

    if (prototype_id == "health_small") {
        AddRegenerationStatus(player, Constants::kPotionSmallRegenDurationSeconds, Constants::kPotionSmallRegenPerSecond);
    } else if (prototype_id == "mana_small") {
        AddManaRegenerationStatus(player, Constants::kPotionSmallManaRegenDurationSeconds,
                                  Constants::kPotionSmallManaRegenPerSecond);
    }

    PlaySfxIfVisible(sfx_drink_potion_.sound, sfx_drink_potion_.loaded, player.pos);
    player.item_slot_counts[slot_index] = std::max(0, player.item_slot_counts[slot_index] - 1);
    if (player.item_slot_counts[slot_index] == 0) {
        player.item_slots[slot_index].clear();
    }
    player.item_slot_cooldown_total[slot_index] = 0.25f;
    player.item_slot_cooldown_remaining[slot_index] = player.item_slot_cooldown_total[slot_index];
    return true;
}

bool GameApp::TryActivateItemById(Player& player, const std::string& prototype_id) {
    if (prototype_id.empty()) {
        return false;
    }
    for (int i = 0; i < static_cast<int>(player.item_slots.size()); ++i) {
        if (player.item_slots[i] == prototype_id && player.item_slot_counts[i] > 0) {
            return TryActivateItemSlot(player, i);
        }
    }
    return false;
}

void GameApp::ApplyImmediateHeal(Player& player, int amount) {
    if (amount <= 0 || !player.alive) {
        return;
    }
    const int before_hp = player.hp;
    player.hp = std::min(Constants::kMaxHp, player.hp + amount);
    const int healed = player.hp - before_hp;
    if (healed > 0) {
        SpawnDamagePopup(player.pos, healed, true);
    }
}

void GameApp::ApplyImmediateManaRestore(Player& player, int amount) {
    if (amount <= 0 || !player.alive) {
        return;
    }
    player.mana = std::min(player.max_mana, player.mana + static_cast<float>(amount));
}

void GameApp::CancelRegenerationStatuses(Player& player) {
    for (auto& status : player.status_effects) {
        if (status.type == StatusEffectType::Regeneration || status.type == StatusEffectType::ManaRegeneration) {
            status.remaining_seconds = 0.0f;
        }
    }
}

void GameApp::CancelInventoryDrag(Player& player, bool drop_to_world_if_unresolved) {
    if (!player.ui_dragging_slot) {
        return;
    }

    bool restored_to_slot = false;
    if (player.ui_drag_source_family == SlotFamily::Rune) {
        if (player.ui_drag_rune_type != RuneType::Catalyst && player.ui_drag_source_index >= 0 &&
            player.ui_drag_source_index < static_cast<int>(player.rune_slots.size()) &&
            player.rune_slots[player.ui_drag_source_index] == RuneType::None) {
            player.rune_slots[player.ui_drag_source_index] = player.ui_drag_rune_type;
            restored_to_slot = true;
        }
    } else if (player.ui_drag_source_family == SlotFamily::Item) {
        if (player.ui_drag_source_index >= 0 &&
            player.ui_drag_source_index < static_cast<int>(player.item_slots.size()) &&
            player.item_slots[player.ui_drag_source_index].empty() &&
            player.item_slot_counts[player.ui_drag_source_index] <= 0) {
            player.item_slots[player.ui_drag_source_index] = player.ui_drag_item_id;
            player.item_slot_counts[player.ui_drag_source_index] = player.ui_drag_item_count;
            player.item_slot_cooldown_remaining[player.ui_drag_source_index] = player.ui_drag_item_cooldown_remaining;
            player.item_slot_cooldown_total[player.ui_drag_source_index] = player.ui_drag_item_cooldown_total;
            restored_to_slot = true;
        }
    } else if (player.ui_drag_source_family == SlotFamily::Weapon) {
        if (player.ui_drag_source_index >= 0 &&
            player.ui_drag_source_index < static_cast<int>(player.weapon_slots.size()) &&
            player.weapon_slots[player.ui_drag_source_index].empty()) {
            player.weapon_slots[player.ui_drag_source_index] = player.ui_drag_item_id;
            restored_to_slot = true;
        }
    }

    if (!restored_to_slot && drop_to_world_if_unresolved) {
        TryDropDraggedSlotToWorld(player, player.ui_drag_source_family, player.ui_drag_source_index, false);
    }

    player.ui_drag_rune_type = RuneType::None;
    player.ui_drag_item_id.clear();
    player.ui_drag_item_count = 0;
    player.ui_drag_item_cooldown_remaining = 0.0f;
    player.ui_drag_item_cooldown_total = 0.0f;
    player.ui_dragging_slot = false;
    player.ui_drag_source_family = SlotFamily::Item;
    player.ui_drag_source_index = -1;
}

void GameApp::BeginInventoryDrag(Player& player, SlotFamily family, int slot_index) {
    CancelInventoryDrag(player);

    player.ui_drag_source_family = family;
    player.ui_drag_source_index = slot_index;
    player.ui_dragging_slot = true;

    if (family == SlotFamily::Rune) {
        player.ui_drag_rune_type = player.rune_slots[slot_index];
        if (player.ui_drag_rune_type != RuneType::Catalyst) {
            player.rune_slots[slot_index] = RuneType::None;
        }
        if (player.ui_drag_rune_type != RuneType::Catalyst && player.selected_rune_slot == slot_index) {
            player.selected_rune_type = RuneType::None;
        }
        return;
    }

    if (family == SlotFamily::Weapon) {
        player.ui_drag_item_id = player.weapon_slots[slot_index];
        player.ui_drag_item_count = 1;
        player.ui_drag_item_cooldown_remaining = 0.0f;
        player.ui_drag_item_cooldown_total = 0.0f;
        player.weapon_slots[slot_index].clear();
        return;
    }

    player.ui_drag_item_id = player.item_slots[slot_index];
    player.ui_drag_item_count = player.item_slot_counts[slot_index];
    player.ui_drag_item_cooldown_remaining = player.item_slot_cooldown_remaining[slot_index];
    player.ui_drag_item_cooldown_total = player.item_slot_cooldown_total[slot_index];
    player.item_slots[slot_index].clear();
    player.item_slot_counts[slot_index] = 0;
    player.item_slot_cooldown_remaining[slot_index] = 0.0f;
    player.item_slot_cooldown_total[slot_index] = 0.0f;
}

void GameApp::DropInventoryDrag(Player& player, SlotFamily family, int slot_index) {
    if (!player.ui_dragging_slot || player.ui_drag_source_family != family) {
        return;
    }

    if (slot_index == player.ui_drag_source_index) {
        CancelInventoryDrag(player);
        return;
    }

    if (family == SlotFamily::Rune) {
        if (player.ui_drag_rune_type == RuneType::Catalyst) {
            CancelInventoryDrag(player);
            return;
        }
        const RuneType target = player.rune_slots[slot_index];
        player.rune_slots[slot_index] = player.ui_drag_rune_type;
        if (player.ui_drag_source_index >= 0 &&
            player.ui_drag_source_index < static_cast<int>(player.rune_slots.size())) {
            player.rune_slots[player.ui_drag_source_index] = target;
        }
        if (player.selected_rune_slot == player.ui_drag_source_index) {
            player.selected_rune_slot = slot_index;
        } else if (player.selected_rune_slot == slot_index) {
            player.selected_rune_slot = player.ui_drag_source_index;
        }
        player.selected_rune_type = player.rune_slots[player.selected_rune_slot];
        player.ui_drag_rune_type = RuneType::None;
    } else if (family == SlotFamily::Item) {
        const std::string target_id = player.item_slots[slot_index];
        const int target_count = player.item_slot_counts[slot_index];
        const float target_remaining = player.item_slot_cooldown_remaining[slot_index];
        const float target_total = player.item_slot_cooldown_total[slot_index];

        player.item_slots[slot_index] = player.ui_drag_item_id;
        player.item_slot_counts[slot_index] = player.ui_drag_item_count;
        player.item_slot_cooldown_remaining[slot_index] = player.ui_drag_item_cooldown_remaining;
        player.item_slot_cooldown_total[slot_index] = player.ui_drag_item_cooldown_total;

        if (player.ui_drag_source_index >= 0 &&
            player.ui_drag_source_index < static_cast<int>(player.item_slots.size())) {
            player.item_slots[player.ui_drag_source_index] = target_id;
            player.item_slot_counts[player.ui_drag_source_index] = target_count;
            player.item_slot_cooldown_remaining[player.ui_drag_source_index] = target_remaining;
            player.item_slot_cooldown_total[player.ui_drag_source_index] = target_total;
        }

        player.ui_drag_item_id.clear();
        player.ui_drag_item_count = 0;
        player.ui_drag_item_cooldown_remaining = 0.0f;
        player.ui_drag_item_cooldown_total = 0.0f;
    } else if (family == SlotFamily::Weapon) {
        const std::string target_id = player.weapon_slots[slot_index];
        player.weapon_slots[slot_index] = player.ui_drag_item_id;
        if (player.ui_drag_source_index >= 0 &&
            player.ui_drag_source_index < static_cast<int>(player.weapon_slots.size())) {
            player.weapon_slots[player.ui_drag_source_index] = target_id;
        }
        player.ui_drag_item_id.clear();
        player.ui_drag_item_count = 0;
        player.ui_drag_item_cooldown_remaining = 0.0f;
        player.ui_drag_item_cooldown_total = 0.0f;
    }

    player.ui_dragging_slot = false;
    player.ui_drag_source_index = -1;
}

bool GameApp::TryDropDraggedSlotToWorld(Player& player, SlotFamily family, int slot_index, bool single_instance) {
    const GridCoord drop_cell = WorldToCell(player.pos);
    if (!state_.map.IsInside(drop_cell)) {
        return false;
    }

    const int drop_count = single_instance ? 1 : std::numeric_limits<int>::max();
    int spawned = 0;
    auto spawn_drop = [&](const std::string& prototype_id) {
        if (prototype_id.empty()) {
            return;
        }
        SpawnDroppedEquipmentItem(prototype_id, drop_cell, player.id);
        ++spawned;
    };

    if (family == SlotFamily::Item) {
        const bool use_drag_payload =
            player.ui_dragging_slot && player.ui_drag_source_family == SlotFamily::Item && player.ui_drag_source_index == slot_index;
        const std::string prototype_id = use_drag_payload ? player.ui_drag_item_id : player.item_slots[slot_index];
        int& count_ref = use_drag_payload ? player.ui_drag_item_count : player.item_slot_counts[slot_index];
        if (slot_index < 0 || slot_index >= static_cast<int>(player.item_slots.size()) ||
            prototype_id.empty() || count_ref <= 0) {
            return false;
        }
        const int actual_drop_count = std::clamp(drop_count, 1, count_ref);
        for (int i = 0; i < actual_drop_count; ++i) {
            spawn_drop(prototype_id);
        }
        count_ref -= actual_drop_count;
        if (use_drag_payload) {
            if (count_ref <= 0) {
                player.ui_drag_item_id.clear();
                player.ui_drag_item_count = 0;
                player.ui_drag_item_cooldown_remaining = 0.0f;
                player.ui_drag_item_cooldown_total = 0.0f;
            }
        } else if (player.item_slot_counts[slot_index] <= 0) {
            player.item_slots[slot_index].clear();
            player.item_slot_counts[slot_index] = 0;
            player.item_slot_cooldown_remaining[slot_index] = 0.0f;
            player.item_slot_cooldown_total[slot_index] = 0.0f;
        }
        return spawned > 0;
    }

    if (family == SlotFamily::Weapon) {
        const bool use_drag_payload =
            player.ui_dragging_slot && player.ui_drag_source_family == SlotFamily::Weapon && player.ui_drag_source_index == slot_index;
        const std::string prototype_id = use_drag_payload ? player.ui_drag_item_id : player.weapon_slots[slot_index];
        if (slot_index < 0 || slot_index >= static_cast<int>(player.weapon_slots.size()) || prototype_id.empty()) {
            return false;
        }
        spawn_drop(prototype_id);
        if (use_drag_payload) {
            player.ui_drag_item_id.clear();
            player.ui_drag_item_count = 0;
        } else {
            player.weapon_slots[slot_index].clear();
        }
        return spawned > 0;
    }

    if (family == SlotFamily::Rune) {
        const bool use_drag_payload =
            player.ui_dragging_slot && player.ui_drag_source_family == SlotFamily::Rune && player.ui_drag_source_index == slot_index;
        if (slot_index < 0 || slot_index >= static_cast<int>(player.rune_slots.size())) {
            return false;
        }
        const RuneType rune_type = use_drag_payload ? player.ui_drag_rune_type : player.rune_slots[slot_index];
        if (rune_type == RuneType::Catalyst) {
            const int current_charges = GetPlayerRuneChargeCount(player, RuneType::Catalyst);
            if (current_charges <= 0) {
                return false;
            }
            const int actual_drop_count = std::clamp(drop_count, 1, current_charges);
            for (int i = 0; i < actual_drop_count; ++i) {
                spawn_drop("catalyst_charge_pickup");
            }
            SetPlayerRuneChargeCount(player, RuneType::Catalyst, current_charges - actual_drop_count);
            if (use_drag_payload) {
                player.ui_drag_rune_type = RuneType::None;
            }
            return spawned > 0;
        }
    }

    return false;
}

void GameApp::QueueLocalInventorySync(const Player& player) {
    pending_local_inventory_sync_ = player;
    pending_local_inventory_sync_dirty_ = true;
}

void GameApp::ApplyInventoryLayoutSync(Player& player, const ClientActionMessage& action) {
    if (!action.has_inventory_layout_sync) {
        return;
    }
    if (action.rune_slots.size() != player.rune_slots.size() || action.item_slots.size() != player.item_slots.size() ||
        action.item_slot_counts.size() != player.item_slot_counts.size() ||
        action.item_slot_cooldown_remaining.size() != player.item_slot_cooldown_remaining.size() ||
        action.item_slot_cooldown_total.size() != player.item_slot_cooldown_total.size()) {
        return;
    }

    for (size_t i = 0; i < player.rune_slots.size(); ++i) {
        player.rune_slots[i] = static_cast<RuneType>(action.rune_slots[i]);
    }
    player.rune_slots[2] = RuneType::Catalyst;
    if (const CastleState* castle = GetAlliedCastleForPlayer(player)) {
        const int capacity = GetCastleLoadoutCapacity(castle->level);
        int equipped_count = 0;
        for (RuneType& rune_type : player.rune_slots) {
            if (!IsCastleEquippableRune(rune_type)) {
                continue;
            }
            if (castle->level < GetRuneRequiredCastleLevel(rune_type) || equipped_count >= capacity) {
                rune_type = RuneType::None;
                continue;
            }
            ++equipped_count;
        }
    }
    player.selected_rune_slot = std::clamp(action.selected_rune_slot, 0, static_cast<int>(player.rune_slots.size()) - 1);
    NormalizePlayerRuneLoadout(player);

    for (size_t i = 0; i < player.item_slots.size(); ++i) {
        player.item_slots[i] = action.item_slots[i];
        player.item_slot_counts[i] = action.item_slot_counts[i];
        player.item_slot_cooldown_remaining[i] = action.item_slot_cooldown_remaining[i];
        player.item_slot_cooldown_total[i] = action.item_slot_cooldown_total[i];
    }
}

bool GameApp::PendingLocalInventorySyncMatches(const Player& player) const {
    if (!pending_local_inventory_sync_.has_value()) {
        return false;
    }
    const Player& pending = *pending_local_inventory_sync_;
    return player.rune_slots == pending.rune_slots && player.selected_rune_slot == pending.selected_rune_slot &&
           player.item_slots == pending.item_slots && player.item_slot_counts == pending.item_slot_counts &&
           player.item_slot_cooldown_remaining == pending.item_slot_cooldown_remaining &&
           player.item_slot_cooldown_total == pending.item_slot_cooldown_total;
}

void GameApp::AddRegenerationStatus(Player& player, float duration_seconds, float amount_per_second) {
    StatusEffectInstance status;
    status.type = StatusEffectType::Regeneration;
    status.remaining_seconds = std::max(0.0f, duration_seconds);
    status.total_seconds = status.remaining_seconds;
    status.magnitude_per_second = std::max(0.0f, amount_per_second);
    status.accumulated_magnitude = 0.0f;
    status.visible = true;
    status.is_buff = true;
    status.composite_effect_id = "heal_effect";
    player.status_effects.push_back(status);
}

void GameApp::AddManaRegenerationStatus(Player& player, float duration_seconds, float amount_per_second) {
    StatusEffectInstance status;
    status.type = StatusEffectType::ManaRegeneration;
    status.remaining_seconds = std::max(0.0f, duration_seconds);
    status.total_seconds = status.remaining_seconds;
    status.magnitude_per_second = std::max(0.0f, amount_per_second);
    status.accumulated_magnitude = 0.0f;
    status.visible = true;
    status.is_buff = true;
    status.composite_effect_id = "mana_regen_effect";
    player.status_effects.push_back(status);
}

void GameApp::AddStunnedStatus(Player& player, float duration_seconds) {
    for (auto& status : player.status_effects) {
        if (status.type != StatusEffectType::Stunned) {
            continue;
        }
        status.total_seconds = std::max(status.total_seconds, duration_seconds);
        status.remaining_seconds = std::max(status.remaining_seconds, duration_seconds);
        return;
    }

    StatusEffectInstance status;
    status.type = StatusEffectType::Stunned;
    status.remaining_seconds = duration_seconds;
    status.total_seconds = duration_seconds;
    status.magnitude_per_second = 0.0f;
    status.accumulated_magnitude = 0.0f;
    status.visible = true;
    status.is_buff = false;
    status.composite_effect_id = "stunned_effect";
    player.status_effects.push_back(status);
}

void GameApp::AddFrozenStatus(Player& player, float duration_seconds) {
    const float clamped_duration = std::max(0.0f, duration_seconds);
    for (auto& status : player.status_effects) {
        if (status.type != StatusEffectType::Frozen) {
            continue;
        }
        status.total_seconds = std::max(status.total_seconds, clamped_duration);
        status.remaining_seconds = std::max(status.remaining_seconds, clamped_duration);
        status.movement_speed_multiplier = Constants::kFrozenMovementSpeedMultiplier;
        status.visible = true;
        status.is_buff = false;
        return;
    }

    StatusEffectInstance status;
    status.type = StatusEffectType::Frozen;
    status.remaining_seconds = clamped_duration;
    status.total_seconds = clamped_duration;
    status.visible = true;
    status.is_buff = false;
    status.movement_speed_multiplier = Constants::kFrozenMovementSpeedMultiplier;
    status.composite_effect_id = "ice_shard_projectile_idle";
    player.status_effects.push_back(status);
}

void GameApp::RefreshOrAddBurningStatus(Player& player, const EmbersTileModifier& modifier) {
    for (auto& status : player.status_effects) {
        if (status.type != StatusEffectType::Burning || status.origin_source_id != modifier.source_spirit_id) {
            continue;
        }
        status.source_id = modifier.id;
        status.source_owner_player_id = modifier.owner_player_id;
        status.remaining_seconds = std::max(status.remaining_seconds, Constants::kBurningStatusDurationSeconds);
        status.total_seconds = std::max(status.total_seconds, Constants::kBurningStatusDurationSeconds);
        return;
    }

    StatusEffectInstance status;
    status.type = StatusEffectType::Burning;
    status.remaining_seconds = Constants::kBurningStatusDurationSeconds;
    status.total_seconds = Constants::kBurningStatusDurationSeconds;
    status.visible = true;
    status.is_buff = false;
    status.source_id = modifier.id;
    status.origin_source_id = modifier.source_spirit_id;
    status.source_owner_player_id = modifier.owner_player_id;
    status.accumulated_magnitude = 0.0f;
    player.status_effects.push_back(status);
}

void GameApp::RefreshOrAddRootedStatus(Player& player, int source_id) {
    for (auto& status : player.status_effects) {
        if (status.type == StatusEffectType::RootedRecovery && status.source_id == source_id) {
            status.remaining_seconds = 0.0f;
        }
    }
    for (auto& status : player.status_effects) {
        if (status.type != StatusEffectType::Rooted || status.source_id != source_id) {
            continue;
        }
        status.visible = true;
        status.is_buff = false;
        status.source_active = true;
        status.remaining_seconds = std::max(status.remaining_seconds, Constants::kEarthRootedVisibleDurationSeconds);
        status.total_seconds = std::max(status.total_seconds, Constants::kEarthRootedVisibleDurationSeconds);
        return;
    }

    StatusEffectInstance status;
    status.type = StatusEffectType::Rooted;
    status.remaining_seconds = Constants::kEarthRootedVisibleDurationSeconds;
    status.total_seconds = Constants::kEarthRootedVisibleDurationSeconds;
    status.visible = true;
    status.is_buff = false;
    status.source_id = source_id;
    status.progress = 0.0f;
    status.source_elapsed_seconds = 0.0f;
    status.burn_duration_seconds = Constants::kEarthRootedBurnInSeconds;
    status.movement_speed_multiplier = 1.0f;
    status.source_active = true;
    status.accumulated_magnitude = 0.0f;
    player.status_effects.push_back(status);
}

void GameApp::UpdateMapObjects(float dt) {
    UpdateDroppedItemPickupBlocks();
    bool map_objects_changed = false;

    for (auto& object : state_.map_objects) {
        if (!object.alive) {
            continue;
        }

        if (object.state == MapObjectState::Spawning) {
            object.state_time += dt;
            const ObjectPrototype* proto = FindObjectPrototype(object.prototype_id);
            const float spawn_duration =
                (proto != nullptr) ? GetMapObjectAnimationDurationSeconds(*proto, proto->born_animation) : 0.0f;
            if (spawn_duration <= 0.0f || object.state_time >= spawn_duration) {
                object.state = MapObjectState::Active;
                object.state_time = 0.0f;
                object.collision_enabled = (!object.walkable || object.stops_projectiles);
                if (object.prototype_id == "fire_flower") {
                    PlaySfxIfVisible(sfx_earth_rune_impact_.sound, sfx_earth_rune_impact_.loaded,
                                     CellToWorldCenter(object.cell));
                }
            }
            continue;
        }

        if (object.state == MapObjectState::Dying) {
            object.state_time += dt;
            if (object.state_time >= object.death_duration) {
                object.alive = false;
            }
            continue;
        }

        if (object.type == ObjectType::Consumable) {
            const Rectangle aabb = GetMapObjectCollisionAabb(object, FindObjectPrototype(object.prototype_id));
            for (const auto& player : state_.players) {
                if (!player.alive) {
                    continue;
                }
                Vector2 normal = {0.0f, 0.0f};
                float penetration = 0.0f;
                if (!CollisionWorld::AabbVsAabb(GetPlayerCollisionRect(player), aabb, normal, penetration)) {
                    continue;
                }
                if (!CanPlayerPickUpObject(player, object)) {
                    continue;
                }
                TryConsumeObject(object.id, player.id);
                break;
            }
        }
    }

    for (const auto& pending : pending_object_spawns_) {
        const int object_id = SpawnObjectInstanceAtCell(pending.prototype_id, pending.cell, pending.reserved_object_id);
        if (object_id >= 0) {
            map_objects_changed = true;
            if (MapObjectInstance* object = FindMapObjectById(object_id); object != nullptr) {
                if (pending.owner_player_id.has_value()) {
                    object->owner_player_id = *pending.owner_player_id;
                }
                if (pending.owner_team.has_value()) {
                    object->owner_team = *pending.owner_team;
                }
            }
        }
        if (object_id >= 0 && pending.blocked_player_id.has_value()) {
            RegisterPickupBlockForDroppedObject(
                object_id, *pending.blocked_player_id, CellToWorldCenter(pending.cell),
                Constants::kDroppedEquipmentPickupRadius * Constants::kDroppedEquipmentPickupUnlockRadiusMultiplier);
        }
    }
    pending_object_spawns_.clear();

    const size_t object_count_before_erase = state_.map_objects.size();
    state_.map_objects.erase(
        std::remove_if(state_.map_objects.begin(), state_.map_objects.end(),
                       [](const MapObjectInstance& object) { return !object.alive; }),
        state_.map_objects.end());
    map_objects_changed = map_objects_changed || state_.map_objects.size() != object_count_before_erase;
    if (map_objects_changed) {
        RebuildMapObjectIndexLookup();
        MarkMapObjectRenderCachesDirty();
    }
}

void GameApp::UpdateFireSpirits(float dt) {
    const float simulation_time_seconds = GetFireSpiritSimTimeSeconds();

    if (network_manager_.IsHost()) {
        std::unordered_set<int> active_flower_ids;
        for (const auto& object : state_.map_objects) {
            if (!object.alive || object.prototype_id != "fire_flower") {
                continue;
            }
            active_flower_ids.insert(object.id);
        }

        for (auto it = fire_flower_runtime_states_.begin(); it != fire_flower_runtime_states_.end();) {
            if (active_flower_ids.find(it->first) != active_flower_ids.end()) {
                ++it;
                continue;
            }
            for (int spirit_id : it->second.owned_spirit_ids) {
                if (FireSpirit* spirit = FindFireSpiritById(spirit_id); spirit != nullptr) {
                    spirit->alive = false;
                    spirit->state = FireSpiritState::Dead;
                }
            }
            it = fire_flower_runtime_states_.erase(it);
        }

        std::unordered_map<int, size_t> spirit_index_by_id;
        spirit_index_by_id.reserve(state_.fire_spirits.size());
        SpatialHashGrid spirit_grid(Constants::kSpatialCellSize);
        for (size_t i = 0; i < state_.fire_spirits.size(); ++i) {
            FireSpirit& spirit = state_.fire_spirits[i];
            spirit_index_by_id[spirit.id] = i;
            if (!spirit.alive || spirit.state != FireSpiritState::Idle) {
                continue;
            }
            spirit_grid.Insert(spirit.id, spirit.pos, Constants::kFireSpiritRepulsionRadius);
        }

        for (const auto& object : state_.map_objects) {
            if (!object.alive || object.prototype_id != "fire_flower") {
                continue;
            }
            FireFlowerRuntimeState& runtime = fire_flower_runtime_states_[object.id];
            runtime.send_cooldown_remaining = std::max(0.0f, runtime.send_cooldown_remaining - dt);
            runtime.spawn_cooldown_remaining = std::max(0.0f, runtime.spawn_cooldown_remaining - dt);
            runtime.owned_spirit_ids.erase(
                std::remove_if(runtime.owned_spirit_ids.begin(), runtime.owned_spirit_ids.end(),
                               [&](int spirit_id) {
                                   const auto spirit_it = spirit_index_by_id.find(spirit_id);
                                   return spirit_it == spirit_index_by_id.end() ||
                                          !state_.fire_spirits[spirit_it->second].alive ||
                                          state_.fire_spirits[spirit_it->second].flower_object_id != object.id;
                               }),
                runtime.owned_spirit_ids.end());

            if (object.state == MapObjectState::Active &&
                runtime.spawn_cooldown_remaining <= 0.0f &&
                static_cast<int>(runtime.owned_spirit_ids.size()) < Constants::kFireSpiritMaxPerFlower) {
                FireSpirit spirit;
                spirit.id = state_.next_entity_id++;
                spirit.flower_object_id = object.id;
                spirit.owner_player_id = object.owner_player_id;
                spirit.owner_team = object.owner_team;
                spirit.state = FireSpiritState::Idle;
                spirit.pos = GetFireFlowerSpiritWorldPoint(object, Constants::kFireSpiritSpawnOffsetX,
                                                           Constants::kFireSpiritSpawnOffsetY);
                spirit.target_world = GetFireFlowerSpiritWorldPoint(object, Constants::kFireSpiritIdleAnchorOffsetX,
                                                                    Constants::kFireSpiritIdleAnchorOffsetY);
                spirit.spawn_order = spirit.id;
                spirit.alive = true;
                spirit.has_last_trail_sample_world = false;
                spirit.trail_samples.clear();
                state_.fire_spirits.push_back(spirit);
                spirit_index_by_id[spirit.id] = state_.fire_spirits.size() - 1;
                spirit_grid.Insert(spirit.id, spirit.pos, Constants::kFireSpiritRepulsionRadius);
                runtime.owned_spirit_ids.push_back(spirit.id);
                runtime.spawn_cooldown_remaining = Constants::kFireSpiritSpawnCooldownSeconds;
            }

            if (object.state != MapObjectState::Active || runtime.send_cooldown_remaining > 0.0f) {
                continue;
            }

            const Vector2 flower_world =
                GetFireFlowerSpiritWorldPoint(object, Constants::kFireSpiritIdleAnchorOffsetX,
                                              Constants::kFireSpiritIdleAnchorOffsetY);
            const float max_target_range = Constants::kFireSpiritTargetRangeTiles * static_cast<float>(state_.map.cell_size);
            float best_distance = max_target_range;
            Vector2 best_target_world = {};
            bool found_target = false;

            for (const auto& player : state_.players) {
                if (!player.alive || player.team == object.owner_team) {
                    continue;
                }
                const float distance = Vector2Distance(flower_world, player.pos);
                if (distance <= best_distance) {
                    best_distance = distance;
                    best_target_world = player.pos;
                    found_target = true;
                }
            }
            for (const auto& target_object : state_.map_objects) {
                if (!target_object.alive || target_object.hp <= 0 || target_object.type != ObjectType::Unit ||
                    target_object.state != MapObjectState::Active || target_object.owner_team == object.owner_team) {
                    continue;
                }
                const Vector2 target_world = CellToWorldCenter(target_object.cell);
                const float distance = Vector2Distance(flower_world, target_world);
                if (distance <= best_distance) {
                    best_distance = distance;
                    best_target_world = target_world;
                    found_target = true;
                }
            }
            if (!found_target) {
                continue;
            }

            FireSpirit* oldest_idle_spirit = nullptr;
            for (int spirit_id : runtime.owned_spirit_ids) {
                FireSpirit* spirit = FindFireSpiritById(spirit_id);
                if (spirit != nullptr && spirit->alive && spirit->state == FireSpiritState::Idle) {
                    oldest_idle_spirit = spirit;
                    break;
                }
            }
            if (oldest_idle_spirit == nullptr) {
                continue;
            }

            oldest_idle_spirit->state = FireSpiritState::Projectile;
            oldest_idle_spirit->launch_world = oldest_idle_spirit->pos;
            oldest_idle_spirit->impact_world = best_target_world;
            oldest_idle_spirit->target_world = best_target_world;
            oldest_idle_spirit->launch_time_seconds = simulation_time_seconds;
            const float travel_distance =
                Vector2Distance(oldest_idle_spirit->launch_world, oldest_idle_spirit->impact_world);
            oldest_idle_spirit->travel_duration_seconds =
                Constants::kFireSpiritTravelBaseSeconds + travel_distance * Constants::kFireSpiritTravelSecondsPerPixel;
            oldest_idle_spirit->impact_time_seconds =
                oldest_idle_spirit->launch_time_seconds + oldest_idle_spirit->travel_duration_seconds;
            oldest_idle_spirit->peak_height = Constants::kFireSpiritArcPeakHeight;
            oldest_idle_spirit->projectile_animation_time = 0.0f;
            oldest_idle_spirit->vel = {0.0f, 0.0f};
            oldest_idle_spirit->trail_samples.clear();
            oldest_idle_spirit->trail_distance_accumulator = 0.0f;
            oldest_idle_spirit->spark_distance_accumulator = 0.0f;
            oldest_idle_spirit->has_last_trail_sample_world = true;
            oldest_idle_spirit->last_trail_sample_world = oldest_idle_spirit->launch_world;
            const Vector2 initial_delta =
                Vector2Subtract(oldest_idle_spirit->impact_world, oldest_idle_spirit->launch_world);
            if (Vector2LengthSqr(initial_delta) > 0.0001f) {
                const Vector2 initial_dir = Vector2Normalize(initial_delta);
                const Vector2 back_sample =
                    Vector2Subtract(oldest_idle_spirit->launch_world,
                                    Vector2Scale(initial_dir, Constants::kFireSpiritTrailBasePointSpacing));
                oldest_idle_spirit->trail_samples.push_back({back_sample, 0.0f});
            }
            oldest_idle_spirit->trail_samples.push_back({oldest_idle_spirit->launch_world, 0.0f});
            runtime.send_cooldown_remaining = Constants::kFireSpiritSendCooldownSeconds;
            PlaySfxIfVisible(sfx_fire_spirit_launch_.sound, sfx_fire_spirit_launch_.loaded,
                             oldest_idle_spirit->launch_world);
        }

        for (auto& spirit : state_.fire_spirits) {
            if (!spirit.alive) {
                continue;
            }
            spirit.age_seconds += dt;
            if (spirit.state == FireSpiritState::Idle) {
                const MapObjectInstance* flower = FindMapObjectById(spirit.flower_object_id);
                if (flower == nullptr || !flower->alive) {
                    spirit.alive = false;
                    spirit.state = FireSpiritState::Dead;
                    continue;
                }
                spirit.target_world = GetFireFlowerSpiritWorldPoint(*flower, Constants::kFireSpiritIdleAnchorOffsetX,
                                                                    Constants::kFireSpiritIdleAnchorOffsetY);
                Vector2 acceleration = {0.0f, 0.0f};
                const Vector2 to_target = Vector2Subtract(spirit.target_world, spirit.pos);
                if (Vector2LengthSqr(to_target) > 0.0001f) {
                    const Vector2 target_dir = Vector2Normalize(to_target);
                    acceleration = Vector2Scale(target_dir, Constants::kFireSpiritIdleAcceleration);
                    acceleration = Vector2Add(
                        acceleration,
                        Vector2Scale(Vector2{-target_dir.y, target_dir.x}, Constants::kFireSpiritIdleOrbitForce));
                }

                for (int other_id : spirit_grid.Query(spirit.pos, Constants::kFireSpiritRepulsionRadius)) {
                    if (other_id == spirit.id) {
                        continue;
                    }
                    const auto other_it = spirit_index_by_id.find(other_id);
                    if (other_it == spirit_index_by_id.end()) {
                        continue;
                    }
                    const FireSpirit& other = state_.fire_spirits[other_it->second];
                    if (!other.alive || other.state != FireSpiritState::Idle) {
                        continue;
                    }
                    Vector2 away = Vector2Subtract(spirit.pos, other.pos);
                    float distance = Vector2Length(away);
                    if (distance <= 0.001f) {
                        away = {1.0f, 0.0f};
                        distance = 1.0f;
                    }
                    if (distance >= Constants::kFireSpiritRepulsionRadius) {
                        continue;
                    }
                    const float normalized = 1.0f - distance / Constants::kFireSpiritRepulsionRadius;
                    acceleration = Vector2Add(acceleration, Vector2Scale(Vector2Normalize(away),
                                                                         Constants::kFireSpiritRepulsionStrength *
                                                                             normalized));
                }

                spirit.vel = Vector2Add(spirit.vel, Vector2Scale(acceleration, dt));
                spirit.vel = Vector2Scale(spirit.vel, std::max(0.0f, 1.0f - Constants::kFireSpiritIdleDamping * dt));
                const float speed = Vector2Length(spirit.vel);
                if (speed > Constants::kFireSpiritIdleMaxSpeed) {
                    spirit.vel = Vector2Scale(Vector2Normalize(spirit.vel), Constants::kFireSpiritIdleMaxSpeed);
                }
                spirit.pos = Vector2Add(spirit.pos, Vector2Scale(spirit.vel, dt));
            } else if (spirit.state == FireSpiritState::Projectile) {
                float arc_height = 0.0f;
                const Vector2 render_world = EvaluateFireSpiritRenderWorld(spirit, simulation_time_seconds, &arc_height);
                spirit.pos = Vector2Lerp(
                    spirit.launch_world, spirit.impact_world,
                    std::clamp((simulation_time_seconds - spirit.launch_time_seconds) /
                                   std::max(0.001f, spirit.travel_duration_seconds),
                               0.0f, 1.0f));
                spirit.projectile_animation_time += dt;
                UpdateFireSpiritTrail(spirit, dt, simulation_time_seconds);
                if (simulation_time_seconds < spirit.impact_time_seconds) {
                    (void)render_world;
                    continue;
                }

                SpawnFireWaveSegments(spirit, simulation_time_seconds);
                PlaySfxIfVisible(sfx_fire_storm_impact_.sound, sfx_fire_storm_impact_.loaded, spirit.impact_world);
                spirit.state = FireSpiritState::Dead;
                if (Constants::kFireSpiritTrailEnableDeadLinger) {
                    spirit.dead_trail_linger_remaining = Constants::kFireSpiritTrailDeadLingerSeconds;
                    spirit.has_last_trail_sample_world = false;
                } else {
                    spirit.alive = false;
                }
            }
        }
    } else {
        for (auto& spirit : state_.fire_spirits) {
            if (!spirit.alive) {
                continue;
            }
            if (spirit.state == FireSpiritState::Projectile) {
                const float t =
                    std::clamp((simulation_time_seconds - spirit.launch_time_seconds) /
                                   std::max(0.001f, spirit.travel_duration_seconds),
                               0.0f, 1.0f);
                spirit.pos = Vector2Lerp(spirit.launch_world, spirit.impact_world, t);
                spirit.projectile_animation_time += dt;
                UpdateFireSpiritTrail(spirit, dt, simulation_time_seconds);
            }
        }
    }

    for (auto& spirit : state_.fire_spirits) {
        const bool dead_lingering =
            Constants::kFireSpiritTrailEnableDeadLinger && spirit.state == FireSpiritState::Dead && spirit.alive;
        if (dead_lingering) {
            spirit.dead_trail_linger_remaining = std::max(0.0f, spirit.dead_trail_linger_remaining - dt);
        }
        for (auto& sample : spirit.trail_samples) {
            sample.age_seconds += dt;
        }
        spirit.trail_samples.erase(
            std::remove_if(spirit.trail_samples.begin(), spirit.trail_samples.end(),
                           [](const FireSpiritTrailSample& sample) {
                               return sample.age_seconds >= Constants::kFireSpiritTrailLifetimeSeconds;
                               }),
            spirit.trail_samples.end());

        if (dead_lingering && spirit.dead_trail_linger_remaining <= 0.0f) {
            spirit.alive = false;
        }
    }

    state_.fire_spirits.erase(
        std::remove_if(state_.fire_spirits.begin(), state_.fire_spirits.end(),
                       [](const FireSpirit& spirit) { return !spirit.alive; }),
        state_.fire_spirits.end());
}

void GameApp::SpawnFireWaveSegments(const FireSpirit& spirit, float start_time_seconds) {
    fire_wave_damage_ledgers_[spirit.id];
    fire_wave_ember_activation_ledgers_[spirit.id];
    const float segment_angle_step = 360.0f / static_cast<float>(Constants::kFireWaveSegmentCount);
    for (int segment_index = 0; segment_index < Constants::kFireWaveSegmentCount; ++segment_index) {
        FireWaveSegment segment;
        segment.id = state_.next_entity_id++;
        segment.source_spirit_id = spirit.id;
        segment.owner_player_id = spirit.owner_player_id;
        segment.owner_team = spirit.owner_team;
        segment.segment_index = segment_index;
        segment.origin_world = spirit.impact_world;
        segment.direction_radians = (segment_angle_step * static_cast<float>(segment_index)) * DEG2RAD;
        segment.start_time_seconds = start_time_seconds;
        segment.duration_seconds = Constants::kFireWaveDurationSeconds;
        segment.range_world = Constants::kFireWaveRangeWorld;
        segment.alive = true;
        state_.fire_wave_segments.push_back(segment);
    }
}

void GameApp::ActivateEmbersTouchedByFireWaveSegment(const FireWaveSegment& segment, Vector2 center, float radius) {
    if (radius <= 0.001f) {
        return;
    }

    auto ledger_it = fire_wave_ember_activation_ledgers_.find(segment.source_spirit_id);
    if (ledger_it == fire_wave_ember_activation_ledgers_.end()) {
        ledger_it = fire_wave_ember_activation_ledgers_.emplace(segment.source_spirit_id, std::unordered_set<int64_t>{}).first;
    }
    std::unordered_set<int64_t>& activation_ledger = ledger_it->second;

    const int cell_size = std::max(1, state_.map.cell_size);
    const float embers_max_range_world = std::max(
        0.0f, segment.range_world - Constants::kEmbersSpawnRangeReductionTiles * static_cast<float>(cell_size));
    const GridCoord min_cell = WorldToCell({center.x - radius, center.y - radius});
    const GridCoord max_cell = WorldToCell({center.x + radius, center.y + radius});

    HitShapeDefinition hit_shape;
    hit_shape.type = HitShapeType::Circle;
    hit_shape.radius = radius;

    for (int cell_y = min_cell.y; cell_y <= max_cell.y; ++cell_y) {
        for (int cell_x = min_cell.x; cell_x <= max_cell.x; ++cell_x) {
            const GridCoord cell = {cell_x, cell_y};
            if (!state_.map.IsInside(cell) || !IsEmbersWalkableTile(state_.map.GetTile(cell))) {
                continue;
            }
            const Vector2 cell_center = CellToWorldCenter(cell);
            if (Vector2Distance(cell_center, segment.origin_world) > embers_max_range_world) {
                continue;
            }

            const int64_t cell_key = MakeCellKey(cell);
            if (activation_ledger.find(cell_key) != activation_ledger.end()) {
                continue;
            }

            const Rectangle tile_rect = {static_cast<float>(cell.x * cell_size), static_cast<float>(cell.y * cell_size),
                                         static_cast<float>(cell_size), static_cast<float>(cell_size)};
            Vector2 normal = {0.0f, 0.0f};
            float penetration = 0.0f;
            if (!HitShapeLibrary::OverlapsAabb(hit_shape, center, 0.0f, tile_rect, &normal, &penetration)) {
                continue;
            }

            activation_ledger.insert(cell_key);
            SpawnOrRefreshEmbersTileModifier(segment, cell);
        }
    }
}

void GameApp::SpawnOrRefreshEmbersTileModifier(const FireWaveSegment& segment, const GridCoord& cell) {
    for (auto& modifier : state_.embers_tile_modifiers) {
        if (!modifier.alive || !(modifier.cell == cell)) {
            continue;
        }
        modifier.source_spirit_id = segment.source_spirit_id;
        modifier.owner_player_id = segment.owner_player_id;
        modifier.owner_team = segment.owner_team;
        modifier.remaining_seconds = Constants::kEmbersModifierDurationSeconds;
        modifier.total_seconds = Constants::kEmbersModifierDurationSeconds;
        return;
    }

    EmbersTileModifier modifier;
    modifier.id = state_.next_entity_id++;
    modifier.source_spirit_id = segment.source_spirit_id;
    modifier.owner_player_id = segment.owner_player_id;
    modifier.owner_team = segment.owner_team;
    modifier.cell = cell;
    modifier.remaining_seconds = Constants::kEmbersModifierDurationSeconds;
    modifier.total_seconds = Constants::kEmbersModifierDurationSeconds;
    modifier.alive = true;
    state_.embers_tile_modifiers.push_back(modifier);
}

void GameApp::UpdateFireWaveSegments(float /*dt*/) {
    const float simulation_time_seconds = GetFireSpiritSimTimeSeconds();
    if (network_manager_.IsHost()) {
        for (auto& segment : state_.fire_wave_segments) {
            if (!segment.alive) {
                continue;
            }
            const float elapsed_seconds = GetFireWaveElapsedSeconds(segment, simulation_time_seconds);
            const float progress = GetFireWaveProgress(segment, simulation_time_seconds);
            const bool expired = elapsed_seconds > std::max(0.001f, segment.duration_seconds);

            auto ledger_it = fire_wave_damage_ledgers_.find(segment.source_spirit_id);
            if (ledger_it == fire_wave_damage_ledgers_.end()) {
                ledger_it = fire_wave_damage_ledgers_.emplace(segment.source_spirit_id, FireWaveDamageLedger{}).first;
            }
            FireWaveDamageLedger& ledger = ledger_it->second;
            const Vector2 center = GetFireWaveCenterWorld(segment, simulation_time_seconds);
            const float radius = GetFireWaveRadiusAtDistance(GetFireWaveTravelDistance(segment, simulation_time_seconds));
            if (radius <= 0.001f) {
                continue;
            }

            ActivateEmbersTouchedByFireWaveSegment(segment, center, radius);

            HitShapeDefinition hit_shape;
            hit_shape.type = HitShapeType::Circle;
            hit_shape.radius = radius;

            for (auto& player : state_.players) {
                if (!player.alive || player.team == segment.owner_team ||
                    ledger.damaged_player_ids.find(player.id) != ledger.damaged_player_ids.end()) {
                    continue;
                }
                Vector2 normal = {0.0f, 0.0f};
                float penetration = 0.0f;
                if (!HitShapeLibrary::OverlapsAabb(hit_shape, center, 0.0f, GetPlayerCollisionRect(player), &normal,
                                                   &penetration)) {
                    continue;
                }
                ledger.damaged_player_ids.insert(player.id);
                ApplyDamageToPlayer(player, segment.owner_player_id, Constants::kFireSpiritExplosionDamage,
                                    "fire_spirit_wave", true, center);
            }

            for (auto& object : state_.map_objects) {
                if (!object.alive || object.hp <= 0 || object.type != ObjectType::Unit ||
                    object.state != MapObjectState::Active || object.owner_team == segment.owner_team ||
                    ledger.damaged_object_ids.find(object.id) != ledger.damaged_object_ids.end()) {
                    continue;
                }
                Vector2 normal = {0.0f, 0.0f};
                float penetration = 0.0f;
                if (!HitShapeLibrary::OverlapsAabb(hit_shape, center, 0.0f,
                                                   GetMapObjectCollisionAabb(object, FindObjectPrototype(object.prototype_id)),
                                                   &normal, &penetration)) {
                    continue;
                }
                ledger.damaged_object_ids.insert(object.id);
                ApplyObjectDamage(object.id, Constants::kFireSpiritExplosionDamage, segment.owner_player_id,
                                  "fire_spirit_wave", center);
            }

            if (expired) {
                segment.alive = false;
            }
        }
    } else {
        for (auto& segment : state_.fire_wave_segments) {
            if (segment.alive &&
                GetFireWaveElapsedSeconds(segment, simulation_time_seconds) >
                    std::max(0.001f, segment.duration_seconds)) {
                segment.alive = false;
            }
        }
    }

    state_.fire_wave_segments.erase(
        std::remove_if(state_.fire_wave_segments.begin(), state_.fire_wave_segments.end(),
                       [](const FireWaveSegment& segment) { return !segment.alive; }),
        state_.fire_wave_segments.end());

    for (auto it = fire_wave_damage_ledgers_.begin(); it != fire_wave_damage_ledgers_.end();) {
        const bool has_active_segment = std::any_of(
            state_.fire_wave_segments.begin(), state_.fire_wave_segments.end(),
            [&](const FireWaveSegment& segment) { return segment.alive && segment.source_spirit_id == it->first; });
        if (!has_active_segment) {
            it = fire_wave_damage_ledgers_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = fire_wave_ember_activation_ledgers_.begin(); it != fire_wave_ember_activation_ledgers_.end();) {
        const bool has_active_segment = std::any_of(
            state_.fire_wave_segments.begin(), state_.fire_wave_segments.end(),
            [&](const FireWaveSegment& segment) { return segment.alive && segment.source_spirit_id == it->first; });
        if (!has_active_segment) {
            it = fire_wave_ember_activation_ledgers_.erase(it);
        } else {
            ++it;
        }
    }
}

void GameApp::UpdateEmbersTileModifiers(float dt) {
    if (network_manager_.IsHost()) {
        for (auto& modifier : state_.embers_tile_modifiers) {
            if (!modifier.alive) {
                continue;
            }
            modifier.remaining_seconds = std::max(0.0f, modifier.remaining_seconds - dt);
            if (modifier.remaining_seconds <= 0.0f) {
                modifier.alive = false;
                continue;
            }

            const Rectangle tile_rect = {static_cast<float>(modifier.cell.x * state_.map.cell_size),
                                         static_cast<float>(modifier.cell.y * state_.map.cell_size),
                                         static_cast<float>(state_.map.cell_size),
                                         static_cast<float>(state_.map.cell_size)};
            for (auto& player : state_.players) {
                if (!player.alive || player.team == modifier.owner_team) {
                    continue;
                }
                Vector2 normal = {0.0f, 0.0f};
                float penetration = 0.0f;
                if (CollisionWorld::AabbVsAabb(GetPlayerCollisionRect(player), tile_rect, normal, penetration)) {
                    RefreshOrAddBurningStatus(player, modifier);
                }
            }
        }
    }

    state_.embers_tile_modifiers.erase(
        std::remove_if(state_.embers_tile_modifiers.begin(), state_.embers_tile_modifiers.end(),
                       [](const EmbersTileModifier& modifier) { return !modifier.alive; }),
        state_.embers_tile_modifiers.end());
}

float GameApp::GetFireSpiritSimTimeSeconds() const {
    if (network_manager_.IsHost()) {
        return state_.match.elapsed_seconds;
    }
    return static_cast<float>(last_snapshot_simulation_time_seconds_ + (GetTime() - last_snapshot_received_local_time_seconds_));
}

Vector2 GameApp::GetFireFlowerSpiritWorldPoint(const MapObjectInstance& flower, int offset_x, int offset_y) const {
    const ObjectPrototype* proto = FindObjectPrototype(flower.prototype_id);
    if (proto == nullptr) {
        return CellToWorldCenter(flower.cell);
    }
    const Rectangle rect =
        GetMapObjectSpriteRect(flower, proto, static_cast<float>(GetSpriteSheetWidth(proto->sprite_sheet)),
                               static_cast<float>(GetSpriteSheetHeight(proto->sprite_sheet)), 1.0f, false);
    return {rect.x + static_cast<float>(offset_x), rect.y + static_cast<float>(offset_y)};
}

Vector2 GameApp::EvaluateFireSpiritRenderWorld(const FireSpirit& spirit, float simulation_time_seconds,
                                               float* out_arc_height) const {
    if (spirit.state != FireSpiritState::Projectile) {
        if (out_arc_height != nullptr) {
            *out_arc_height = 0.0f;
        }
        return spirit.pos;
    }
    const float t =
        std::clamp((simulation_time_seconds - spirit.launch_time_seconds) /
                       std::max(0.001f, spirit.travel_duration_seconds),
                   0.0f, 1.0f);
    Vector2 world = Vector2Lerp(spirit.launch_world, spirit.impact_world, t);
    const float arc_height = EvaluateFireSpiritArcHeight(t, spirit.peak_height);
    world.y -= arc_height;
    if (out_arc_height != nullptr) {
        *out_arc_height = arc_height;
    }
    return world;
}

void GameApp::UpdateFireSpiritTrail(FireSpirit& spirit, float dt, float simulation_time_seconds) {
    (void)dt;
    if (!spirit.alive || spirit.state == FireSpiritState::Idle) {
        spirit.trail_samples.clear();
        spirit.trail_distance_accumulator = 0.0f;
        spirit.spark_distance_accumulator = 0.0f;
        spirit.has_last_trail_sample_world = false;
        return;
    }
    if (spirit.state != FireSpiritState::Projectile) {
        return;
    }

    const Vector2 render_world = EvaluateFireSpiritRenderWorld(spirit, simulation_time_seconds);
    if (!spirit.has_last_trail_sample_world) {
        spirit.last_trail_sample_world = render_world;
        spirit.has_last_trail_sample_world = true;
        spirit.trail_samples.push_back({render_world, 0.0f});
        return;
    }

    const Vector2 delta = Vector2Subtract(render_world, spirit.last_trail_sample_world);
    const float distance = Vector2Length(delta);
    if (distance <= 0.001f) {
        return;
    }
    spirit.trail_distance_accumulator += distance;
    spirit.spark_distance_accumulator += distance;
    const Vector2 travel_dir = Vector2Normalize(delta);

    float spacing = Constants::kFireSpiritTrailBasePointSpacing;
    if (spirit.trail_samples.size() >= 2) {
        const Vector2 prev_delta = Vector2Subtract(spirit.trail_samples.back().world,
                                                   spirit.trail_samples[spirit.trail_samples.size() - 2].world);
        if (Vector2LengthSqr(prev_delta) > 0.0001f) {
            const Vector2 prev_dir = Vector2Normalize(prev_delta);
            const Vector2 dir = Vector2Normalize(delta);
            const float angle = std::acos(std::clamp(Vector2DotProduct(prev_dir, dir), -1.0f, 1.0f));
            const float angle_ratio = angle / PI;
            spacing /= (1.0f + angle_ratio * Constants::kFireSpiritTrailCurvatureSensitivity);
        }
    }
    spacing = std::max(2.0f, spacing);

    while (spirit.trail_distance_accumulator >= spacing) {
        spirit.trail_distance_accumulator -= spacing;
        const float t = 1.0f - (spirit.trail_distance_accumulator / distance);
        const Vector2 sample_world = Vector2Lerp(spirit.last_trail_sample_world, render_world, std::clamp(t, 0.0f, 1.0f));
        spirit.trail_samples.push_back({sample_world, 0.0f});
        if (static_cast<int>(spirit.trail_samples.size()) > Constants::kFireSpiritTrailMaxSamples) {
            spirit.trail_samples.erase(spirit.trail_samples.begin());
        }
    }
    const float spark_spacing = std::max(0.001f, Constants::kFireSpiritSparkSpacing);
    while (spirit.spark_distance_accumulator >= spark_spacing) {
        spirit.spark_distance_accumulator -= spark_spacing;
        const float t = 1.0f - (spirit.spark_distance_accumulator / distance);
        const Vector2 spark_world =
            Vector2Lerp(spirit.last_trail_sample_world, render_world, std::clamp(t, 0.0f, 1.0f));
        SpawnFireSpiritSparkParticle(spark_world, travel_dir);
    }
    spirit.last_trail_sample_world = render_world;
}

void GameApp::SpawnDamagePopup(Vector2 world_pos, int amount, bool is_heal) {
    if (amount <= 0) {
        return;
    }
    DamagePopup popup;
    popup.pos = world_pos;
    popup.amount = amount;
    popup.is_heal = is_heal;
    popup.age_seconds = 0.0f;
    popup.lifetime_seconds = Constants::kDamagePopupLifetimeSeconds;
    popup.rise_per_second = Constants::kDamagePopupRisePerSecond;
    popup.alive = true;
    state_.damage_popups.push_back(popup);
}

bool GameApp::ApplyDamageToPlayer(Player& target, int attacker_player_id, int damage, const char* source,
                                  bool count_kill_for_attacker, std::optional<Vector2> damage_world_pos) {
    if (!target.alive || damage <= 0) {
        return false;
    }

    CancelRegenerationStatuses(target);
    target.hp = std::max(0, target.hp - damage);
    event_queue_.Push(PlayerHitEvent{attacker_player_id, target.id, damage, source != nullptr ? source : "unknown"});
    SpawnDamagePopup(target.pos, damage, false);
    SpawnDamageHitParticles(target.pos, damage_world_pos);
    if (IsOutsideZoneDamageSource(source)) {
        std::vector<size_t> loaded_indices;
        loaded_indices.reserve(sfx_zone_damage_.size());
        for (size_t i = 0; i < sfx_zone_damage_.size(); ++i) {
            if (sfx_zone_damage_[i].loaded) {
                loaded_indices.push_back(i);
            }
        }
        if (!loaded_indices.empty()) {
            std::uniform_int_distribution<size_t> dist(0, loaded_indices.size() - 1);
            const LoadedSfx& clip = sfx_zone_damage_[loaded_indices[dist(rng_)]];
            PlaySfxIfVisible(clip.sound, clip.loaded, target.pos);
        }
    } else {
        PlaySfxIfVisible(sfx_player_damaged_.sound, sfx_player_damaged_.loaded, target.pos);
    }

    if (target.hp <= 0) {
        HandlePlayerDeath(target, attacker_player_id, count_kill_for_attacker);
    }
    return true;
}

void GameApp::HandlePlayerDeath(Player& victim, int killer_player_id, bool count_kill_for_attacker) {
    if (!victim.alive) {
        return;
    }

    victim.alive = false;
    victim.hp = 0;
    victim.awaiting_respawn = true;
    victim.respawn_remaining = Constants::kRespawnDelaySeconds;
    victim.rune_placing_mode = false;
    victim.melee_active_remaining = 0.0f;
    victim.melee_cooldown_remaining = 0.0f;
    victim.melee_hit_target_ids.clear();
    victim.melee_hit_object_ids.clear();
    victim.outside_zone_damage_accumulator = 0.0f;
    victim.action_state = PlayerActionState::Idle;
    victim.vel = {0.0f, 0.0f};
    victim.status_effects.clear();
    event_queue_.Push(PlayerDiedEvent{victim.id, killer_player_id});
    PlaySfxIfVisible(sfx_player_death_.sound, sfx_player_death_.loaded, victim.pos);

    if (!count_kill_for_attacker || killer_player_id < 0 || killer_player_id == victim.id) {
        return;
    }

    if (Player* killer = FindPlayerById(killer_player_id)) {
        killer->kills += 1;
        if (killer->team == Constants::kTeamRed) {
            state_.match.red_team_kills += 1;
        } else {
            state_.match.blue_team_kills += 1;
        }
        RecordKillTimelinePoint();

        ConsoleMessage console;
        console.lifetime_seconds = kConsoleLifetimeSeconds;
        console.spans.push_back(MakeConsoleSpan(GetPlayerDisplayName(killer->id), TeamUiColor(killer->team)));
        console.spans.push_back(MakeConsoleSpan(" killed ", RAYWHITE));
        console.spans.push_back(MakeConsoleSpan(GetPlayerDisplayName(victim.id), TeamUiColor(victim.team)));
        BroadcastConsoleMessageToAll(console);
    }
}

bool GameApp::IsOutsideArena(Vector2 world_pos) const {
    if (!IsZoneEnabled()) {
        return false;
    }
    if (state_.match.arena_radius_world <= 0.0f) {
        return false;
    }
    const Vector2 delta = Vector2Subtract(world_pos, state_.match.arena_center_world);
    return Vector2LengthSqr(delta) > state_.match.arena_radius_world * state_.match.arena_radius_world;
}

Vector2 GameApp::ClampToArenaWithBuffer(Vector2 world_pos, float buffer_tiles) const {
    if (!IsZoneEnabled()) {
        return world_pos;
    }
    const float buffer_world = std::max(0.0f, buffer_tiles) * static_cast<float>(state_.map.cell_size);
    const float allowed_radius = std::max(0.0f, state_.match.arena_radius_world + buffer_world);
    const Vector2 center = state_.match.arena_center_world;
    const Vector2 delta = Vector2Subtract(world_pos, center);
    const float distance = Vector2Length(delta);
    if (distance <= allowed_radius || distance <= 0.0001f) {
        return world_pos;
    }
    return Vector2Add(center, Vector2Scale(delta, allowed_radius / distance));
}

Vector2 GameApp::ComputeRespawnPosition(const Player& player) const {
    Vector2 respawn = CellToWorldCenter(player.spawn_cell);
    respawn = ClampToArenaWithBuffer(respawn, Constants::kArenaSpawnBufferTiles);

    const float map_width = static_cast<float>(state_.map.width * state_.map.cell_size);
    const float map_height = static_cast<float>(state_.map.height * state_.map.cell_size);
    const float half_width = Constants::kPlayerHitboxWidth * 0.5f;
    const float half_height = Constants::kPlayerHitboxHeight * 0.5f;
    respawn.x = std::clamp(respawn.x, half_width, std::max(half_width, map_width - half_width));
    respawn.y = std::clamp(respawn.y, half_height, std::max(half_height, map_height - half_height));
    return respawn;
}

void GameApp::UpdateIceWalls(float dt) {
    int death_frame_count = 0;
    float death_fps = 1.0f;
    float death_duration = 0.5f;
    if (sprite_metadata_.GetAnimationStats("ice_wall_death", "default", death_frame_count, death_fps)) {
        death_duration = static_cast<float>(death_frame_count) / std::max(0.001f, death_fps);
    }

    for (auto& wall : state_.ice_walls) {
        if (!wall.alive) {
            continue;
        }

        wall.state_time += dt;
        switch (wall.state) {
            case IceWallState::Materializing:
                if (wall.state_time >= Constants::kIceWallMaterializeSeconds) {
                    wall.state = IceWallState::Active;
                    wall.state_time = 0.0f;
                    PushPlayersOutOfIceWall(wall.cell);
                }
                break;
            case IceWallState::Active: {
                const float previous_hp = wall.hp;
                wall.hp = std::max(0.0f, wall.hp - Constants::kIceWallHpDecayPerSecond * dt);
                if (wall.hp <= 0.0f) {
                    wall.state = IceWallState::Dying;
                    wall.state_time = GetIceWallDeathAnimationSampleTime(sprite_metadata_, previous_hp);
                    PlaySfxIfVisible(sfx_ice_wall_melt_.sound, sfx_ice_wall_melt_.loaded, CellToWorldCenter(wall.cell));
                }
                break;
            }
            case IceWallState::Dying:
                if (wall.state_time >= death_duration) {
                    wall.alive = false;
                }
                break;
        }
    }

    state_.ice_walls.erase(
        std::remove_if(state_.ice_walls.begin(), state_.ice_walls.end(),
                       [](const IceWallPiece& wall) { return !wall.alive; }),
        state_.ice_walls.end());
}

void GameApp::UpdateRunes(float dt) {
    std::vector<RunePlacedEvent> activated_runes;
    std::vector<int> expired_rune_ids;
    bool influence_changed = false;
    for (auto& rune : state_.runes) {
        if (rune.rune_type == RuneType::FireStormDummy || rune.fire_storm_original_rune_type != RuneType::None) {
            if (rune.fire_storm_visual_state == FireStormRuneVisualState::Born) {
                rune.fire_storm_visual_state_time += dt;
                if (rune.fire_storm_visual_state_time >= rune.fire_storm_visual_state_duration) {
                    rune.fire_storm_visual_state = FireStormRuneVisualState::Idle;
                    rune.fire_storm_visual_state_time = 0.0f;
                    rune.fire_storm_visual_state_duration = 0.0f;
                }
            } else if (rune.fire_storm_visual_state == FireStormRuneVisualState::Dying) {
                rune.fire_storm_visual_state_time += dt;
                if (rune.fire_storm_visual_state_time >= rune.fire_storm_visual_state_duration) {
                    FinalizeFireStormRuneDeath(rune);
                    influence_changed = true;
                }
                continue;
            }
            if (rune.fire_storm_temporary && rune.active) {
                rune.fire_storm_remaining_seconds = std::max(0.0f, rune.fire_storm_remaining_seconds - dt);
                if (rune.fire_storm_remaining_seconds <= 0.0f) {
                    BeginFireStormRuneRevert(rune, true);
                    influence_changed = true;
                    continue;
                }
            }

            float& lightning_seconds = fire_storm_dummy_lightning_seconds_remaining_[rune.id];
            float& lightning_cooldown_seconds = fire_storm_dummy_lightning_cooldown_seconds_remaining_[rune.id];
            if (rune.fire_storm_visual_state != FireStormRuneVisualState::Idle || !rune.active) {
                lightning_seconds = 0.0f;
                lightning_cooldown_seconds = 0.0f;
            } else {
                int lightning_frame_count = 0;
                float lightning_fps = 8.0f;
                if (!(sprite_metadata_.GetAnimationStats("fire_storm_top_lightning", "default", lightning_frame_count, lightning_fps) &&
                      lightning_frame_count > 0 && lightning_fps > 0.001f)) {
                    lightning_frame_count = Constants::kFireStormDummyLightningMaxFrames;
                    lightning_fps = 8.0f;
                }
                const float frame_seconds = 1.0f / std::max(0.001f, lightning_fps);
                if (lightning_seconds > 0.0f) {
                    lightning_seconds = std::max(0.0f, lightning_seconds - dt);
                    if (lightning_seconds <= 0.0f) {
                        lightning_cooldown_seconds =
                            static_cast<float>(Constants::kFireStormDummyLightningCooldownFrames) * frame_seconds;
                    }
                } else if (lightning_cooldown_seconds > 0.0f) {
                    lightning_cooldown_seconds = std::max(0.0f, lightning_cooldown_seconds - dt);
                } else {
                    std::uniform_real_distribution<float> chance_dist(0.0f, 1.0f);
                    if (chance_dist(visual_rng_) < Constants::kFireStormDummyLightningChancePerFrame) {
                        std::uniform_int_distribution<int> frame_dist(Constants::kFireStormDummyLightningMinFrames,
                                                                      Constants::kFireStormDummyLightningMaxFrames);
                        lightning_seconds = static_cast<float>(frame_dist(visual_rng_)) * frame_seconds;
                    }
                }
            }
        }

        if (!rune.active) {
            rune.activation_remaining_seconds = std::max(0.0f, rune.activation_remaining_seconds - dt);
            if (rune.activation_remaining_seconds > 0.0f) {
                continue;
            }

            rune.active = true;
            if (rune.creates_influence_zone) {
                influence_changed = true;
            }
            activated_runes.push_back(
                RunePlacedEvent{rune.owner_player_id, rune.owner_team, rune.cell, rune.rune_type, rune.placement_order});
            continue;
        }

        if (rune.rune_type != RuneType::Earth) {
            continue;
        }

        switch (rune.earth_trap_state) {
            case EarthRuneTrapState::IdleRune: {
                bool enemy_in_range = false;
                const Vector2 rune_center = CellToWorldCenter(rune.cell);
                const float trigger_radius =
                    Constants::kEarthRuneTrapRangeTiles * static_cast<float>(state_.map.cell_size);
                for (const auto& player : state_.players) {
                    if (!player.alive || player.team == rune.owner_team) {
                        continue;
                    }
                    if (Vector2Distance(player.pos, rune_center) <= trigger_radius) {
                        enemy_in_range = true;
                        break;
                    }
                }
                if (!enemy_in_range) {
                    for (const auto& object : state_.map_objects) {
                        if (!object.alive || object.hp <= 0 || object.type != ObjectType::Unit ||
                            object.state != MapObjectState::Active || object.owner_player_id < 0 ||
                            object.owner_team == rune.owner_team) {
                            continue;
                        }
                        if (Vector2Distance(CellToWorldCenter(object.cell), rune_center) <= trigger_radius) {
                            enemy_in_range = true;
                            break;
                        }
                    }
                }
                if (!enemy_in_range) {
                    break;
                }
                rune.earth_trap_state = EarthRuneTrapState::Slamming;
                rune.earth_state_time = 0.0f;
                int frame_count = 0;
                float fps = 1.0f;
                rune.earth_state_duration = Constants::kEarthRuneSlamFallbackSeconds;
                if (sprite_metadata_.GetAnimationStats("earth_rune_slam", "default", frame_count, fps) && frame_count > 0) {
                    rune.earth_state_duration =
                        (static_cast<float>(frame_count) / std::max(0.001f, fps)) * Constants::kEarthRuneSlamSlowdown;
                }
                rune.earth_roots_spawned = false;
                influence_changed = influence_changed || rune.creates_influence_zone;
                rune.creates_influence_zone = false;
                PlaySfxIfVisible(sfx_earth_rune_launch_.sound, sfx_earth_rune_launch_.loaded, CellToWorldCenter(rune.cell));
                break;
            }
            case EarthRuneTrapState::Slamming: {
                rune.earth_state_time += dt;
                if (!rune.earth_roots_spawned &&
                    rune.earth_state_time >= rune.earth_state_duration * Constants::kEarthRuneSlamSpawnRootsProgress) {
                    rune.earth_roots_group_id = SpawnEarthRootsGroup(rune.owner_player_id, rune.owner_team, rune.cell);
                    rune.earth_roots_spawned = true;
                    const Vector2 impact_center = CellToWorldCenter(rune.cell);
                    PlaySfxIfVisible(sfx_earth_rune_impact_.sound, sfx_earth_rune_impact_.loaded, impact_center);
                    if (IsWorldPointInsideCameraView(impact_center)) {
                        camera_shake_time_remaining_ =
                            std::max(camera_shake_time_remaining_, Constants::kCameraShakeDurationSeconds);
                    }
                }
                if (rune.earth_state_time >= rune.earth_state_duration) {
                    rune.earth_trap_state = EarthRuneTrapState::RootedIdle;
                    rune.earth_state_time = 0.0f;
                    rune.earth_state_duration = Constants::kEarthRuneTrapPersistSeconds;
                }
                break;
            }
            case EarthRuneTrapState::RootedIdle:
                rune.earth_state_time += dt;
                if (rune.earth_state_time >= rune.earth_state_duration) {
                    rune.earth_trap_state = EarthRuneTrapState::RootedDeath;
                    rune.earth_state_time = 0.0f;
                    int frame_count = 0;
                    float fps = 1.0f;
                    rune.earth_state_duration = Constants::kEarthRuneRootedDeathFallbackSeconds;
                    if (sprite_metadata_.GetAnimationStats("earth_rune_rooted_death", "default", frame_count, fps) &&
                        frame_count > 0) {
                        rune.earth_state_duration = static_cast<float>(frame_count) / std::max(0.001f, fps);
                    }
                }
                break;
            case EarthRuneTrapState::RootedDeath:
                rune.earth_state_time += dt;
                if (rune.earth_state_time >= rune.earth_state_duration) {
                    expired_rune_ids.push_back(rune.id);
                }
                break;
        }
    }

    if (!expired_rune_ids.empty()) {
        state_.runes.erase(
            std::remove_if(state_.runes.begin(), state_.runes.end(),
                           [&](const Rune& rune) {
                               return std::find(expired_rune_ids.begin(), expired_rune_ids.end(), rune.id) !=
                                      expired_rune_ids.end();
                           }),
            state_.runes.end());
    }

    if (!activated_runes.empty() || !expired_rune_ids.empty() || influence_changed) {
        RebuildInfluenceZones();
    }
    if (!activated_runes.empty()) {
        for (const RunePlacedEvent& event : activated_runes) {
            event_queue_.Push(event);
        }
    }
}

void GameApp::UpdateCastles(float dt) {
    std::unordered_map<int, int> previous_castle_levels;
    previous_castle_levels.reserve(state_.castles.size());
    for (const auto& castle : state_.castles) {
        previous_castle_levels[castle.id] = castle.level;
    }

    for (auto& castle : state_.castles) {
        RefreshCastleDerivedState(castle);
    }

    const float charge_radius_world = Constants::kCastleInteractionRangeTiles * static_cast<float>(state_.map.cell_size);
    const float energy_per_second = Constants::kCastleChargeEnergyPerRune /
                                    std::max(0.001f, Constants::kCastleChargeDurationSeconds);

    for (auto& rune : state_.runes) {
        if (!rune.active || rune.rune_type != RuneType::Catalyst) {
            rune.castle_charging = false;
            rune.castle_id = -1;
            rune.castle_charge_elapsed_seconds = 0.0f;
            continue;
        }

        CastleState* bound_castle = FindCastleById(rune.castle_id);
        if (!rune.castle_charging || bound_castle == nullptr) {
            rune.castle_charging = false;
            rune.castle_id = -1;
            rune.castle_charge_elapsed_seconds = 0.0f;
            const Vector2 rune_world = CellToWorldCenter(rune.cell);
            for (auto& castle : state_.castles) {
                if (Vector2Distance(rune_world, CellToWorldCenter(castle.cell)) <= charge_radius_world) {
                    rune.castle_charging = true;
                    rune.castle_id = castle.id;
                    bound_castle = &castle;
                    break;
                }
            }
        }

        if (!rune.castle_charging || bound_castle == nullptr) {
            continue;
        }

        rune.castle_charge_elapsed_seconds += dt;
        bound_castle->total_energy += energy_per_second * dt;
        RefreshCastleDerivedState(*bound_castle);
        const auto previous_level_it = previous_castle_levels.find(bound_castle->id);
        const int previous_level =
            previous_level_it != previous_castle_levels.end() ? previous_level_it->second : bound_castle->level;
        if (bound_castle->level > previous_level) {
            if (sfx_castle_level_up_.loaded) {
                PlaySound(sfx_castle_level_up_.sound);
            }
            ConsoleMessage console;
            console.lifetime_seconds = kConsoleLifetimeSeconds;
            console.spans.push_back(MakeConsoleSpan(TeamDisplayName(bound_castle->team), TeamUiColor(bound_castle->team)));
            console.spans.push_back(MakeConsoleSpan("'s castle has reached level ", RAYWHITE));
            console.spans.push_back(MakeConsoleSpan(std::to_string(bound_castle->level), TeamUiColor(bound_castle->team)));
            console.spans.push_back(MakeConsoleSpan(".", RAYWHITE));
            BroadcastConsoleMessageToAll(console);
            previous_castle_levels[bound_castle->id] = bound_castle->level;
        }

        if (rune.castle_charge_elapsed_seconds >= Constants::kCastleChargeDurationSeconds) {
            rune.active = false;
            rune.activation_total_seconds = 0.0f;
            rune.activation_remaining_seconds = 0.0f;
            rune.creates_influence_zone = false;
            rune.castle_charging = false;
            rune.castle_id = -1;
            rune.castle_charge_elapsed_seconds = 0.0f;
        }
    }
}

int GameApp::SpawnEarthRootsGroup(int owner_player_id, int owner_team, const GridCoord& center_cell) {
    EarthRootsGroup group;
    group.id = state_.next_entity_id++;
    group.owner_player_id = owner_player_id;
    group.owner_team = owner_team;
    group.center_cell = center_cell;
    group.state = EarthRootsGroupState::Born;
    group.state_time = 0.0f;
    int frame_count = 0;
    float fps = 1.0f;
    group.state_duration = 0.5f;
    if (sprite_metadata_.GetAnimationStats("roots_born_back", "default", frame_count, fps) && frame_count > 0) {
        group.state_duration = static_cast<float>(frame_count) / std::max(0.001f, fps);
    }
    group.idle_lifetime_remaining_seconds = Constants::kEarthRuneTrapPersistSeconds;
    group.active_for_gameplay = true;
    group.alive = true;
    state_.earth_roots_groups.push_back(group);
    return group.id;
}

void GameApp::UpdateEarthRootsGroups(float dt) {
    for (auto& player : state_.players) {
        for (auto& status : player.status_effects) {
            if (status.type == StatusEffectType::Rooted) {
                status.source_active = false;
            }
        }
    }

    for (auto& group : state_.earth_roots_groups) {
        if (!group.alive) {
            continue;
        }

        if (group.active_for_gameplay) {
            for (auto& player : state_.players) {
                if (!player.alive || player.team == group.owner_team) {
                    continue;
                }
                const GridCoord cell = WorldToCell(player.pos);
                if (std::abs(cell.x - group.center_cell.x) <= 1 && std::abs(cell.y - group.center_cell.y) <= 1) {
                    RefreshOrAddRootedStatus(player, group.id);
                }
            }

            for (auto& object : state_.map_objects) {
                if (!object.alive || object.hp <= 0 || object.type != ObjectType::Unit ||
                    object.state != MapObjectState::Active || object.owner_player_id < 0 ||
                    object.owner_team == group.owner_team) {
                    continue;
                }
                if (std::abs(object.cell.x - group.center_cell.x) <= 1 &&
                    std::abs(object.cell.y - group.center_cell.y) <= 1) {
                    float& accumulated_damage = rooted_unit_damage_accumulators_[object.id];
                    accumulated_damage += Constants::kEarthRootedDamagePerSecond * dt;
                    const int whole_damage = static_cast<int>(std::floor(accumulated_damage + 0.0001f));
                    if (whole_damage > 0) {
                        accumulated_damage -= static_cast<float>(whole_damage);
                        ApplyObjectDamage(object.id, whole_damage, group.owner_player_id, "earth_roots",
                                          CellToWorldCenter(group.center_cell));
                    }
                }
            }
        }

        group.state_time += dt;
        switch (group.state) {
            case EarthRootsGroupState::Born:
                if (group.state_time >= group.state_duration) {
                    group.state = EarthRootsGroupState::Idle;
                    group.state_time = 0.0f;
                    group.state_duration = 0.0f;
                }
                break;
            case EarthRootsGroupState::Idle:
                group.idle_lifetime_remaining_seconds = std::max(0.0f, group.idle_lifetime_remaining_seconds - dt);
                if (group.idle_lifetime_remaining_seconds <= 0.0f) {
                    group.state = EarthRootsGroupState::Dying;
                    group.state_time = 0.0f;
                    group.active_for_gameplay = false;
                    int frame_count = 0;
                    float fps = 1.0f;
                    group.state_duration = 0.5f;
                    if (sprite_metadata_.GetAnimationStats("roots_death_back", "default", frame_count, fps) && frame_count > 0) {
                        group.state_duration = static_cast<float>(frame_count) / std::max(0.001f, fps);
                    }
                }
                break;
            case EarthRootsGroupState::Dying:
                if (group.state_time >= group.state_duration) {
                    group.alive = false;
                }
                break;
        }
    }

    for (auto& player : state_.players) {
        std::vector<StatusEffectInstance> recovery_to_add;
        for (auto& status : player.status_effects) {
            if (status.type != StatusEffectType::Rooted || status.remaining_seconds <= 0.0f || status.source_active) {
                continue;
            }
            StatusEffectInstance recovery;
            recovery.type = StatusEffectType::RootedRecovery;
            recovery.remaining_seconds = Constants::kEarthRootedRecoverSeconds;
            recovery.total_seconds = Constants::kEarthRootedRecoverSeconds;
            recovery.visible = false;
            recovery.is_buff = false;
            recovery.source_id = status.source_id;
            recovery.progress = status.progress;
            recovery.source_elapsed_seconds = 0.0f;
            recovery.burn_duration_seconds = Constants::kEarthRootedRecoverSeconds;
            recovery.movement_speed_multiplier = status.movement_speed_multiplier;
            recovery.source_active = false;
            recovery_to_add.push_back(recovery);
            status.remaining_seconds = 0.0f;
        }
        for (auto& recovery : recovery_to_add) {
            bool replaced = false;
            for (auto& existing : player.status_effects) {
                if (existing.type == StatusEffectType::RootedRecovery && existing.source_id == recovery.source_id) {
                    existing = recovery;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                player.status_effects.push_back(recovery);
            }
        }
    }

    state_.earth_roots_groups.erase(
        std::remove_if(state_.earth_roots_groups.begin(), state_.earth_roots_groups.end(),
                       [](const EarthRootsGroup& group) { return !group.alive; }),
        state_.earth_roots_groups.end());

    for (auto it = rooted_unit_damage_accumulators_.begin(); it != rooted_unit_damage_accumulators_.end();) {
        const MapObjectInstance* object = FindMapObjectById(it->first);
        if (object == nullptr || !object->alive || object->type != ObjectType::Unit || object->hp <= 0 ||
            object->state != MapObjectState::Active) {
            it = rooted_unit_damage_accumulators_.erase(it);
        } else {
            ++it;
        }
    }
}

void GameApp::RebuildInfluenceZones() {
    if (!IsInfluenceZoneSystemEnabled()) {
        ClearInfluenceZoneVisuals();
        return;
    }

    std::vector<InfluenceZoneCell> rebuilt_zones;
    for (const auto& rune : state_.runes) {
        if (!rune.active || !rune.creates_influence_zone) {
            continue;
        }
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const GridCoord cell = {rune.cell.x + dx, rune.cell.y + dy};
                if (!state_.map.IsInside(cell)) {
                    continue;
                }
                auto it = std::find_if(rebuilt_zones.begin(), rebuilt_zones.end(),
                                       [&](const InfluenceZoneCell& zone) { return zone.cell == cell; });
                if (it != rebuilt_zones.end()) {
                    it->team = rune.owner_team;
                    it->source_rune_id = rune.id;
                } else {
                    rebuilt_zones.push_back(InfluenceZoneCell{rune.id, rune.owner_team, cell});
                }
            }
        }
    }

    for (const auto& rune : state_.runes) {
        if (!rune.active || !rune.volatile_cast) {
            continue;
        }
        rebuilt_zones.erase(
            std::remove_if(rebuilt_zones.begin(), rebuilt_zones.end(),
                           [&](const InfluenceZoneCell& zone) {
                               return zone.cell == rune.cell && zone.team != rune.owner_team;
                           }),
            rebuilt_zones.end());
    }

    state_.influence_zones = rebuilt_zones;

    const uint64_t new_signature = ComputeInfluenceZoneSignature(state_.influence_zones);
    if (has_influence_zone_signature_ && influence_zone_signature_ == new_signature) {
        return;
    }
    if (!has_influence_zone_signature_ && state_.influence_zones.empty()) {
        return;
    }
    influence_zone_signature_ = new_signature;
    has_influence_zone_signature_ = true;

    InfluenceBuildRequest request;
    request.signature = new_signature;
    request.generation = influence_build_generation_;
    request.map_width = state_.map.width;
    request.map_height = state_.map.height;
    request.cell_size = state_.map.cell_size;
    request.samples_per_tile = kInfluenceDistanceSamplesPerTile;
    request.zones = state_.influence_zones;
    pending_influence_build_request_ = std::move(request);

    if (!influence_build_in_flight_) {
        StartPendingInfluenceFieldBuild();
    }
}

void GameApp::ResolvePlayerVsIceWalls(Player& player) {
    if (!player.alive) {
        return;
    }

    const int cell_size = state_.map.cell_size;
    std::unordered_set<int64_t> visited_components;
    for (const auto& wall : state_.ice_walls) {
        if (!wall.alive || wall.state != IceWallState::Active) {
            continue;
        }
        const int64_t key = MakeGridKey(wall.cell);
        if (visited_components.count(key)) {
            continue;
        }

        const std::vector<GridCoord> component = CollectConnectedActiveIceWallCells(state_.ice_walls, wall.cell);
        for (const GridCoord& cell : component) {
            visited_components.insert(MakeGridKey(cell));
        }
        if (!IntersectsTileComponent(GetPlayerCollisionRect(player), cell_size, component)) {
            continue;
        }

        Vector2 resolved = player.pos;
        if (!CollisionWorld::FindClosestBoundaryExitForTileComponent(GetPlayerCollisionRect(player), cell_size, component,
                                                                     resolved)) {
            continue;
        }

        const Vector2 new_pos = MoveTowardResolvedPosition(player.pos, resolved);
        const Vector2 push = Vector2Subtract(new_pos, player.pos);
        player.pos = new_pos;
        if (Vector2LengthSqr(push) <= 0.0001f) {
            continue;
        }
        const Vector2 normal = Vector2Normalize(push);
        const float velocity_dot = Vector2DotProduct(player.vel, normal);
        if (velocity_dot < 0.0f) {
            player.vel = Vector2Subtract(player.vel, Vector2Scale(normal, velocity_dot));
        }
    }
}

void GameApp::ResolvePlayerVsIceWallsLocal(Player& player) {
    if (!player.alive) {
        return;
    }

    const int cell_size = state_.map.cell_size;
    for (const auto& wall : state_.ice_walls) {
        if (!wall.alive || wall.state != IceWallState::Active) {
            continue;
        }

        const Rectangle aabb = {static_cast<float>(wall.cell.x * cell_size), static_cast<float>(wall.cell.y * cell_size),
                                static_cast<float>(cell_size), static_cast<float>(cell_size)};
        Vector2 normal = {0.0f, 0.0f};
        float penetration = 0.0f;
        if (!CollisionWorld::AabbVsAabb(GetPlayerCollisionRect(player), aabb, normal, penetration)) {
            continue;
        }

        player.pos = Vector2Add(player.pos, Vector2Scale(normal, penetration));
        const float velocity_dot = Vector2DotProduct(player.vel, normal);
        if (velocity_dot < 0.0f) {
            player.vel = Vector2Subtract(player.vel, Vector2Scale(normal, velocity_dot));
        }
    }
}

void GameApp::PushPlayersOutOfIceWall(const GridCoord& cell) {
    const int cell_size = state_.map.cell_size;
    const std::vector<GridCoord> component = CollectConnectedActiveIceWallCells(state_.ice_walls, cell);
    if (component.empty()) {
        return;
    }

    for (auto& player : state_.players) {
        if (!player.alive) {
            continue;
        }

        if (!IntersectsTileComponent(GetPlayerCollisionRect(player), cell_size, component)) {
            continue;
        }

        Vector2 resolved = player.pos;
        if (!CollisionWorld::FindClosestBoundaryExitForTileComponent(GetPlayerCollisionRect(player), cell_size, component,
                                                                     resolved)) {
            continue;
        }

        const Vector2 new_pos = MoveTowardResolvedPosition(player.pos, resolved);
        const Vector2 push_dir = Vector2Subtract(new_pos, player.pos);
        player.pos = new_pos;
        if (Vector2LengthSqr(push_dir) > 0.0001f) {
            const Vector2 normal = Vector2Normalize(push_dir);
            const float velocity_dot = Vector2DotProduct(player.vel, normal);
            if (velocity_dot < 0.0f) {
                player.vel = Vector2Subtract(player.vel, Vector2Scale(normal, velocity_dot));
            }
        }
        CollisionWorld::ResolvePlayerVsWorld(state_.map, player);
    }
}

void GameApp::UpdateProjectiles(float dt) {
    for (auto& projectile : state_.projectiles) {
        if (!projectile.alive) {
            continue;
        }

        if (projectile.lifetime_remaining > 0.0f) {
            projectile.lifetime_remaining = std::max(0.0f, projectile.lifetime_remaining - dt);
        }

        if (projectile.upgrade_pause_remaining > 0.0f) {
            projectile.upgrade_pause_remaining = std::max(0.0f, projectile.upgrade_pause_remaining - dt);
            if (projectile.upgrade_pause_remaining > 0.0f) {
                continue;
            }
            projectile.vel = projectile.resume_vel;
        }

        projectile.vel = Vector2Add(projectile.vel, Vector2Scale(projectile.acc, dt));
        const float drag_factor = std::max(0.0f, 1.0f - projectile.drag * dt);
        projectile.vel = Vector2Scale(projectile.vel, drag_factor);
        projectile.pos = Vector2Add(projectile.pos, Vector2Scale(projectile.vel, dt));

        if (!IsStaticFireBolt(projectile)) {
            for (const auto& dummy : state_.fire_storm_dummies) {
                if (!dummy.alive || dummy.owner_team != projectile.owner_team) {
                    continue;
                }
                const Rectangle cell_aabb = {static_cast<float>(dummy.cell.x * state_.map.cell_size),
                                             static_cast<float>(dummy.cell.y * state_.map.cell_size),
                                             static_cast<float>(state_.map.cell_size),
                                             static_cast<float>(state_.map.cell_size)};
                Vector2 normal = {0.0f, 0.0f};
                float penetration = 0.0f;
                if (!CollisionWorld::CircleVsAabb(projectile.pos, projectile.radius, cell_aabb, normal, penetration)) {
                    continue;
                }

                if (projectile.animation_key == "projectile_fire_bolt") {
                    ConfigureLargeStaticFireBolt(projectile, true);

                    Particle upgrade_vfx;
                    upgrade_vfx.pos = projectile.pos;
                    upgrade_vfx.vel = {0.0f, 0.0f};
                    upgrade_vfx.acc = {0.0f, 0.0f};
                    upgrade_vfx.drag = 0.0f;
                    upgrade_vfx.size = 48.0f;
                    upgrade_vfx.animation_key = "static_upgrade";
                    upgrade_vfx.facing = "default";
                    upgrade_vfx.age_seconds = 0.0f;
                    upgrade_vfx.max_cycles = 1;
                    upgrade_vfx.alive = true;
                    state_.particles.push_back(upgrade_vfx);
                    PlaySfxIfVisible(sfx_static_upgrade_.sound, sfx_static_upgrade_.loaded, projectile.pos);
                }
                break;
            }
        }

        bool destroy_projectile = false;
        std::optional<int> excluded_target_id;

        const GridCoord cell = WorldToCell(projectile.pos);
        if (!state_.map.IsInside(cell)) {
            destroy_projectile = true;
        } else if (projectile.lifetime_remaining == 0.0f) {
            destroy_projectile = true;
        }

        if (!destroy_projectile) {
            for (auto& object : state_.map_objects) {
                if (!object.alive || !object.collision_enabled || !object.stops_projectiles) {
                    continue;
                }

                const Rectangle aabb =
                    GetMapObjectCollisionAabb(object, FindObjectPrototype(object.prototype_id));
                Vector2 normal = {0.0f, 0.0f};
                float penetration = 0.0f;
                if (!CollisionWorld::CircleVsAabb(projectile.pos, projectile.radius, aabb, normal, penetration)) {
                    continue;
                }

                ApplyObjectDamage(object.id, projectile.damage, projectile.owner_player_id, "projectile", projectile.pos);
                destroy_projectile = true;
                break;
            }
        }

        if (!destroy_projectile) {
            const int wall_cell_size = state_.map.cell_size;
            for (auto& wall : state_.ice_walls) {
                if (!wall.alive || wall.state != IceWallState::Active) {
                    continue;
                }

                const Rectangle aabb = {static_cast<float>(wall.cell.x * wall_cell_size),
                                        static_cast<float>(wall.cell.y * wall_cell_size),
                                        static_cast<float>(wall_cell_size), static_cast<float>(wall_cell_size)};
                Vector2 normal = {0.0f, 0.0f};
                float penetration = 0.0f;
                if (CollisionWorld::CircleVsAabb(projectile.pos, projectile.radius, aabb, normal, penetration)) {
                    if (IsIceShardProjectile(projectile)) {
                        constexpr float kIceWallHealPropagationFactor = 0.75f;
                        const auto component_distances = BuildIceWallComponentDistances(state_.ice_walls, wall.cell);
                        for (auto& healed_wall : state_.ice_walls) {
                            if (!healed_wall.alive || healed_wall.state != IceWallState::Active) {
                                continue;
                            }
                            const auto distance_it = component_distances.find(MakeGridKey(healed_wall.cell));
                            if (distance_it == component_distances.end()) {
                                continue;
                            }

                            const float propagated_heal =
                                static_cast<float>(projectile.damage) *
                                std::pow(kIceWallHealPropagationFactor, static_cast<float>(distance_it->second));
                            if (propagated_heal <= 0.0f) {
                                continue;
                            }

                            const float before_hp = healed_wall.hp;
                            healed_wall.hp = std::min(Constants::kIceWallMaxHp, healed_wall.hp + propagated_heal);
                            const int healed = static_cast<int>(std::round(healed_wall.hp - before_hp));
                            if (healed > 0) {
                                SpawnDamagePopup(CellToWorldCenter(healed_wall.cell), healed, true);
                                SpawnIceWallHealSnowBurst(healed_wall, healed);
                            }
                        }
                    } else {
                        const float previous_hp = wall.hp;
                        wall.hp = std::max(0.0f, wall.hp - static_cast<float>(projectile.damage));
                        if (wall.hp <= 0.0f) {
                            wall.state = IceWallState::Dying;
                            wall.state_time = GetIceWallDeathAnimationSampleTime(sprite_metadata_, previous_hp);
                        }
                    }
                    destroy_projectile = true;
                    break;
                }
            }
        }

        if (!destroy_projectile) {
            HitShapeDefinition projectile_hit_shape;
            projectile_hit_shape.type = HitShapeType::Circle;
            projectile_hit_shape.radius = projectile.radius;
            for (auto& target : state_.players) {
                if (!target.alive || target.team == projectile.owner_team || target.id == projectile.owner_player_id) {
                    continue;
                }

                Vector2 normal = {0.0f, 0.0f};
                float penetration = 0.0f;
                if (!HitShapeLibrary::OverlapsAabb(projectile_hit_shape, projectile.pos, 0.0f, GetPlayerCollisionRect(target),
                                                   &normal, &penetration)) {
                    continue;
                }

                destroy_projectile = true;
                excluded_target_id = target.id;
                ApplyDamageToPlayer(target, projectile.owner_player_id, projectile.damage,
                                    IsIceShardProjectile(projectile) ? "ice_wave" : "fire_bolt", true, projectile.pos);
                if (IsStaticFireBolt(projectile)) {
                    AddStunnedStatus(target, Constants::kStaticFireBoltStunSeconds);
                } else if (IsIceShardProjectile(projectile)) {
                    AddFrozenStatus(target, Constants::kFrozenStatusDurationSeconds);
                    std::vector<size_t> loaded_indices;
                    loaded_indices.reserve(sfx_ice_wave_impact_.size());
                    for (size_t i = 0; i < sfx_ice_wave_impact_.size(); ++i) {
                        if (sfx_ice_wave_impact_[i].loaded) {
                            loaded_indices.push_back(i);
                        }
                    }
                    if (!loaded_indices.empty()) {
                        std::uniform_int_distribution<size_t> dist(0, loaded_indices.size() - 1);
                        const LoadedSfx& clip = sfx_ice_wave_impact_[loaded_indices[dist(rng_)]];
                        PlaySfxIfVisible(clip.sound, clip.loaded, projectile.pos);
                    }
                }

                break;
            }
        }

        if (destroy_projectile) {
            if (IsIceShardProjectile(projectile)) {
                SpawnIceShardDeathParticle(projectile.pos, projectile.vel);
            } else {
                SpawnProjectileExplosion(projectile, excluded_target_id);
            }
            projectile.alive = false;
        }
    }

    state_.projectiles.erase(std::remove_if(state_.projectiles.begin(), state_.projectiles.end(),
                                            [](const Projectile& projectile) { return !projectile.alive; }),
                             state_.projectiles.end());
}

void GameApp::SpawnIceShardDeathParticle(Vector2 position, Vector2 travel_velocity) {
    Particle particle;
    particle.pos = position;
    particle.vel = {0.0f, 0.0f};
    particle.acc = {0.0f, 0.0f};
    particle.drag = 0.0f;
    particle.size = 32.0f;
    particle.alpha = 255;
    particle.animation_key = "ice_shard_projectile_death";
    particle.facing = "default";
    particle.rotation_degrees = AimToDegrees(travel_velocity);
    particle.age_seconds = 0.0f;
    particle.max_cycles = 1;
    particle.alive = true;
    state_.particles.push_back(particle);
}

void GameApp::SpawnProjectileExplosion(const Projectile& projectile, std::optional<int> excluded_target_id) {
    Explosion explosion;
    explosion.owner_player_id = projectile.owner_player_id;
    explosion.owner_team = projectile.owner_team;
    explosion.pos = projectile.pos;
    explosion.damage = projectile.damage;
    explosion.radius = Constants::kFireBoltExplosionRadius;
    explosion.duration_seconds = Constants::kFireBoltExplosionFallbackDuration;

    if (excluded_target_id.has_value()) {
        explosion.excluded_target_ids.push_back(*excluded_target_id);
    }

    int frame_count = 0;
    float fps = 0.0f;
    if (sprite_metadata_.GetAnimationStats("explosion", "default", frame_count, fps)) {
        explosion.duration_seconds = static_cast<float>(frame_count) / std::max(0.001f, fps);
    }

    state_.explosions.push_back(explosion);

    const float half_w = static_cast<float>(GetScreenWidth()) / std::max(0.001f, (2.0f * camera_.zoom));
    const float half_h = static_cast<float>(GetScreenHeight()) / std::max(0.001f, (2.0f * camera_.zoom));
    const float pad = explosion.radius;
    const bool in_camera_view =
        projectile.pos.x >= (camera_.target.x - half_w - pad) && projectile.pos.x <= (camera_.target.x + half_w + pad) &&
        projectile.pos.y >= (camera_.target.y - half_h - pad) && projectile.pos.y <= (camera_.target.y + half_h + pad);
    if (in_camera_view) {
        camera_shake_time_remaining_ = std::max(camera_shake_time_remaining_, Constants::kCameraShakeDurationSeconds);
    }
    if (IsStaticFireBolt(projectile)) {
        PlaySfxIfVisible(sfx_static_bolt_impact_.sound, sfx_static_bolt_impact_.loaded, projectile.pos);
    } else {
        PlaySfxIfVisible(sfx_explosion_.sound, sfx_explosion_.loaded, projectile.pos);
    }

    Particle explosion_vfx;
    explosion_vfx.pos = projectile.pos;
    explosion_vfx.vel = {0.0f, 0.0f};
    explosion_vfx.acc = {0.0f, 0.0f};
    explosion_vfx.drag = 0.0f;
    explosion_vfx.size = 32.0f;
    explosion_vfx.alpha = 255;
    explosion_vfx.animation_key = "explosion";
    explosion_vfx.facing = "default";
    explosion_vfx.age_seconds = 0.0f;
    explosion_vfx.max_cycles = 1;
    explosion_vfx.alive = true;
    state_.particles.push_back(explosion_vfx);
}

void GameApp::UpdateExplosions(float dt) {
    auto has_player_id = [](const std::vector<int>& ids, int player_id) {
        return std::find(ids.begin(), ids.end(), player_id) != ids.end();
    };

    for (auto& explosion : state_.explosions) {
        if (!explosion.alive) {
            continue;
        }

        explosion.age_seconds += dt;
        HitShapeDefinition explosion_hit_shape;
        explosion_hit_shape.type = HitShapeType::Circle;
        explosion_hit_shape.radius = explosion.radius;

        for (auto& target : state_.players) {
            if (!target.alive || target.team == explosion.owner_team || target.id == explosion.owner_player_id) {
                continue;
            }
            if (has_player_id(explosion.excluded_target_ids, target.id) ||
                has_player_id(explosion.already_hit_target_ids, target.id)) {
                continue;
            }

            Vector2 normal = {0.0f, 0.0f};
            float penetration = 0.0f;
            if (!HitShapeLibrary::OverlapsAabb(explosion_hit_shape, explosion.pos, 0.0f, GetPlayerCollisionRect(target),
                                               &normal, &penetration)) {
                continue;
            }

            explosion.already_hit_target_ids.push_back(target.id);
            ApplyDamageToPlayer(target, explosion.owner_player_id, explosion.damage, "fire_bolt_explosion", true,
                                explosion.pos);
        }

        if (explosion.age_seconds >= explosion.duration_seconds) {
            explosion.alive = false;
        }
    }

    state_.explosions.erase(
        std::remove_if(state_.explosions.begin(), state_.explosions.end(),
                       [](const Explosion& explosion) { return !explosion.alive; }),
        state_.explosions.end());
}

void GameApp::SpawnLightningEffect(Vector2 start, Vector2 end, float idle_duration_seconds, bool volatile_variant,
                                   const char* born_animation_key, const char* idle_animation_key,
                                   const char* death_animation_key) {
    const float half_tile = 0.5f * static_cast<float>(std::max(1, state_.map.cell_size));
    const Vector2 half_tile_offset = {0.0f, half_tile};

    LightningEffect effect;
    effect.id = state_.next_entity_id++;
    effect.start = Vector2Add(start, half_tile_offset);
    effect.end = Vector2Add(end, half_tile_offset);
    effect.idle_duration = std::max(0.0f, idle_duration_seconds);
    effect.volatile_variant = volatile_variant;
    effect.born_animation_key = born_animation_key != nullptr ? born_animation_key : "";
    effect.idle_animation_key =
        idle_animation_key != nullptr
            ? idle_animation_key
            : (volatile_variant ? "lightning_volatile_magic_idle" : "lightning_magic_idle");
    effect.death_animation_key =
        death_animation_key != nullptr
            ? death_animation_key
            : (volatile_variant ? "lightning_volatile_magic_death" : "lightning_magic_death");
    effect.elapsed = 0.0f;
    effect.born_elapsed = 0.0f;
    effect.born_duration = 0.0f;
    effect.birthing = false;
    effect.dying = false;
    effect.death_elapsed = 0.0f;
    effect.death_duration = 0.25f;
    effect.alive = true;

    const Vector2 delta = Vector2Subtract(end, start);
    const float distance = Vector2Length(delta);
    const float target_len = static_cast<float>(std::max(1, state_.map.cell_size));
    effect.segment_count = std::max(1, static_cast<int>(std::round(distance / std::max(1.0f, target_len))));

    int idle_frame_count = 0;
    float idle_fps = 1.0f;
    float cycle_seconds = 0.25f;
    if (!effect.born_animation_key.empty()) {
        int born_frame_count = 0;
        float born_fps = 1.0f;
        if (sprite_metadata_.GetAnimationStats(effect.born_animation_key, "default", born_frame_count, born_fps) &&
            born_frame_count > 0) {
            effect.born_duration = static_cast<float>(born_frame_count) / std::max(0.001f, born_fps);
            effect.birthing = true;
        }
    }

    if (sprite_metadata_.GetAnimationStats(effect.idle_animation_key, "default", idle_frame_count, idle_fps) &&
        idle_frame_count > 0) {
        cycle_seconds = static_cast<float>(idle_frame_count) / std::max(0.001f, idle_fps);
    }

    effect.segment_phase_offsets_seconds.resize(static_cast<size_t>(effect.segment_count), 0.0f);
    std::uniform_real_distribution<float> phase_dist(0.0f, std::max(0.0f, cycle_seconds - 0.0001f));
    for (float& phase : effect.segment_phase_offsets_seconds) {
        phase = phase_dist(rng_);
    }

    int death_frame_count = 0;
    float death_fps = 1.0f;
    if (sprite_metadata_.GetAnimationStats(effect.death_animation_key, "default", death_frame_count, death_fps) &&
        death_frame_count > 0) {
        effect.death_duration = static_cast<float>(death_frame_count) / std::max(0.001f, death_fps);
    }

    state_.lightning_effects.push_back(effect);
}

void GameApp::UpdateLightningEffects(float dt) {
    for (auto& effect : state_.lightning_effects) {
        if (!effect.alive) {
            continue;
        }
        if (effect.birthing) {
            effect.born_elapsed += dt;
            if (effect.born_elapsed >= effect.born_duration) {
                effect.birthing = false;
                effect.born_elapsed = effect.born_duration;
            }
        } else if (!effect.dying) {
            effect.elapsed += dt;
            if (effect.elapsed >= effect.idle_duration) {
                effect.dying = true;
                effect.death_elapsed = 0.0f;
            }
        } else {
            effect.death_elapsed += dt;
            if (effect.death_elapsed >= effect.death_duration) {
                effect.alive = false;
            }
        }
    }

    state_.lightning_effects.erase(
        std::remove_if(state_.lightning_effects.begin(), state_.lightning_effects.end(),
                       [](const LightningEffect& effect) { return !effect.alive; }),
        state_.lightning_effects.end());

    const auto has_active_lightning_effect = [&](int effect_id) {
        return std::any_of(state_.lightning_effects.begin(), state_.lightning_effects.end(),
                           [&](const LightningEffect& effect) { return effect.alive && effect.id == effect_id; });
    };
    const auto find_active_lightning_effect = [&](int effect_id) -> LightningEffect* {
        auto it = std::find_if(state_.lightning_effects.begin(), state_.lightning_effects.end(),
                               [&](const LightningEffect& effect) { return effect.alive && effect.id == effect_id; });
        return it == state_.lightning_effects.end() ? nullptr : &(*it);
    };

    std::unordered_set<int> active_charging_rune_ids;
    for (const Rune& rune : state_.runes) {
        if (!rune.active || !rune.castle_charging) {
            continue;
        }
        const CastleState* castle = FindCastleById(rune.castle_id);
        if (castle == nullptr) {
            continue;
        }
        active_charging_rune_ids.insert(rune.id);
        const auto active_effect_it = castle_charge_lightning_effect_ids_.find(rune.id);
        const bool has_active_effect =
            active_effect_it != castle_charge_lightning_effect_ids_.end() &&
            has_active_lightning_effect(active_effect_it->second);
        if (!has_active_effect) {
            SpawnLightningEffect(CellToWorldCenter(rune.cell), GetCastleChargePortWorld(*castle),
                                 999999.0f, false, "charging_lightning_born", "charging_lightning_idle",
                                 "charging_lightning_death");
            if (!state_.lightning_effects.empty()) {
                LightningEffect& effect = state_.lightning_effects.back();
                castle_charge_lightning_effect_ids_[rune.id] = effect.id;
                effect.has_sort_y_override = true;
                float sort_y = GetCastleChargePortWorld(*castle).y;
                if (const MapObjectInstance* castle_object = FindMapObjectById(castle->map_object_id)) {
                    if (const ObjectPrototype* proto = FindObjectPrototype(castle_object->prototype_id)) {
                        const Rectangle aabb = GetMapObjectCollisionAabb(*castle_object, proto);
                        sort_y = aabb.y + aabb.height + 1.0f;
                    }
                }
                effect.sort_y_override = sort_y;
            }
        } else if (LightningEffect* effect = find_active_lightning_effect(active_effect_it->second)) {
            effect->start = Vector2Add(CellToWorldCenter(rune.cell), {0.0f, 0.5f * static_cast<float>(std::max(1, state_.map.cell_size))});
            effect->end = Vector2Add(GetCastleChargePortWorld(*castle), {0.0f, 0.5f * static_cast<float>(std::max(1, state_.map.cell_size))});
        }
    }
    for (auto it = castle_charge_lightning_effect_ids_.begin(); it != castle_charge_lightning_effect_ids_.end();) {
        LightningEffect* effect = find_active_lightning_effect(it->second);
        if (active_charging_rune_ids.count(it->first) == 0) {
            if (effect != nullptr && !effect->dying) {
                effect->birthing = false;
                effect->dying = true;
                effect->death_elapsed = 0.0f;
            }
            it = castle_charge_lightning_effect_ids_.erase(it);
        } else if (effect == nullptr) {
            it = castle_charge_lightning_effect_ids_.erase(it);
        } else {
            ++it;
        }
    }
}

int GameApp::SpawnCompositeEffect(const std::string& effect_id, Vector2 origin_world) {
    const CompositeEffectDefinition* definition = composite_effects_loader_.FindById(effect_id);
    if (definition == nullptr) {
        return -1;
    }

    const auto get_metadata = [&](CompositeEffectSheet sheet) -> const SpriteMetadataLoader* {
        switch (sheet) {
            case CompositeEffectSheet::Base32:
                return &sprite_metadata_;
            case CompositeEffectSheet::Tall32x64:
                return &sprite_metadata_tall_;
            case CompositeEffectSheet::Large96x96:
                return &sprite_metadata_96x96_;
        }
        return &sprite_metadata_;
    };

    float duration_seconds = 0.0f;
    for (const auto& part : definition->parts) {
        int frame_count = 0;
        float fps = 1.0f;
        const SpriteMetadataLoader* metadata = get_metadata(part.sheet);
        if (metadata != nullptr && metadata->GetAnimationStats(part.animation, "default", frame_count, fps) &&
            frame_count > 0) {
            duration_seconds = std::max(duration_seconds, static_cast<float>(frame_count) / std::max(0.001f, fps));
        }
    }

    CompositeEffectInstance instance;
    instance.id = state_.next_entity_id++;
    instance.effect_id = effect_id;
    instance.origin_world = origin_world;
    instance.duration_seconds = duration_seconds;
    state_.composite_effects.push_back(instance);
    return instance.id;
}

bool GameApp::IsPlayerBeingPulled(int player_id) const {
    return std::any_of(state_.grappling_hooks.begin(), state_.grappling_hooks.end(),
                       [&](const GrapplingHook& hook) {
                           return hook.alive && hook.owner_player_id == player_id &&
                                  hook.phase == GrapplingHookPhase::Pulling;
                       });
}

bool GameApp::TryStartGrapplingHook(Player& player, Vector2 target_world, bool play_audio) {
    const MobilityProfile* mobility_profile = GetEquippedMobility(player);
    if (!player.alive || player.grappling_cooldown_remaining > 0.0f || IsPlayerBeingPulled(player.id) ||
        mobility_profile == nullptr || mobility_profile->kind != EquipmentActionKind::GrapplingHook) {
        return false;
    }

    Vector2 delta = Vector2Subtract(target_world, player.pos);
    if (Vector2LengthSqr(delta) < 0.0001f) {
        delta = {1.0f, 0.0f};
    }
    const Vector2 direction = Vector2Normalize(delta);
    const float max_distance = mobility_profile->range_tiles * static_cast<float>(state_.map.cell_size);
    const Vector2 clamped_target = Vector2Add(player.pos, Vector2Scale(direction, max_distance));
    const Vector2 collision_offset = {0.0f, -0.5f * static_cast<float>(state_.map.cell_size)};

    GrapplingHook hook;
    hook.id = network_manager_.IsHost() ? state_.next_entity_id++ : next_predicted_entity_id_--;
    hook.owner_player_id = player.id;
    hook.owner_team = player.team;
    hook.head_pos = player.pos;
    hook.target_pos = clamped_target;
    hook.latch_point = clamped_target;
    hook.pull_destination = player.pos;
    hook.phase = GrapplingHookPhase::Firing;
    hook.latch_target_type = GrapplingHookLatchTargetType::None;
    hook.latch_target_id = -1;
    hook.latch_cell = WorldToCell(clamped_target);
    hook.latched = false;
    hook.animation_time = 0.0f;
    hook.pull_elapsed_seconds = 0.0f;
    hook.max_pull_duration_seconds = max_distance / std::max(1.0f, Constants::kGrapplingHookPullSpeed);
    hook.alive = true;

    const float step = std::max(1.0f, Constants::kGrapplingHookCollisionProbeStep);
    const float distance = Vector2Distance(player.pos, clamped_target);
    const int samples = std::max(1, static_cast<int>(std::ceil(distance / step)));
    for (int i = 1; i <= samples; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(samples);
        const Vector2 sample = Vector2Lerp(player.pos, clamped_target, t);
        const Vector2 collision_sample = Vector2Add(sample, collision_offset);
        bool found = false;

        for (const auto& wall : state_.ice_walls) {
            if (!wall.alive || wall.state == IceWallState::Dying) {
                continue;
            }
            const Rectangle aabb = {static_cast<float>(wall.cell.x * state_.map.cell_size),
                                    static_cast<float>(wall.cell.y * state_.map.cell_size),
                                    static_cast<float>(state_.map.cell_size),
                                    static_cast<float>(state_.map.cell_size)};
            if (CheckCollisionPointRec(collision_sample, aabb)) {
                hook.target_pos = sample;
                hook.latch_point = sample;
                hook.pull_destination =
                    Vector2Subtract(sample, Vector2Scale(direction, GetPlayerCollisionSupportDistance(direction)));
                hook.phase = GrapplingHookPhase::Firing;
                hook.latch_target_type = GrapplingHookLatchTargetType::IceWall;
                hook.latch_target_id = wall.id;
                hook.latch_cell = wall.cell;
                hook.latched = true;
                hook.pull_elapsed_seconds = 0.0f;
                found = true;
                break;
            }
        }
        if (found) {
            break;
        }

        for (const auto& object : state_.map_objects) {
            if (!object.alive || !object.stops_projectiles || !object.collision_enabled) {
                continue;
            }
            const Rectangle aabb = GetMapObjectCollisionAabb(object, FindObjectPrototype(object.prototype_id));
            if (CheckCollisionPointRec(collision_sample, aabb)) {
                hook.target_pos = sample;
                hook.latch_point = sample;
                hook.pull_destination =
                    Vector2Subtract(sample, Vector2Scale(direction, GetPlayerCollisionSupportDistance(direction)));
                hook.phase = GrapplingHookPhase::Firing;
                hook.latch_target_type = GrapplingHookLatchTargetType::MapObject;
                hook.latch_target_id = object.id;
                hook.latch_cell = object.cell;
                hook.latched = true;
                hook.pull_elapsed_seconds = 0.0f;
                found = true;
                break;
            }
        }
        if (found) {
            break;
        }
    }

    player.grappling_cooldown_remaining = mobility_profile->cooldown_seconds;
    player.grappling_cooldown_total = mobility_profile->cooldown_seconds;
    player.rune_placing_mode = false;
    player.inventory_mode = false;
    state_.grappling_hooks.push_back(hook);
    if (play_audio) {
        PlaySfxIfVisible(sfx_grappling_throw_.sound, sfx_grappling_throw_.loaded, player.pos);
    }
    return true;
}

void GameApp::UpdateGrapplingHooks(float dt) {
    for (auto& hook : state_.grappling_hooks) {
        if (!hook.alive) {
            continue;
        }
        if (!network_manager_.IsHost() && hook.owner_player_id != state_.local_player_id) {
            continue;
        }
        hook.animation_time += dt;

        Player* owner = FindPlayerById(hook.owner_player_id);
        if (owner == nullptr || !owner->alive) {
            hook.alive = false;
            continue;
        }

        if (hook.phase == GrapplingHookPhase::Firing) {
            const Vector2 to_target = Vector2Subtract(hook.target_pos, hook.head_pos);
            const float distance = Vector2Length(to_target);
            const float step = Constants::kGrapplingHookFireSpeed * dt;
            if (distance <= step || distance <= 0.0001f) {
                hook.head_pos = hook.target_pos;
                if (hook.latched) {
                    hook.phase = GrapplingHookPhase::Pulling;
                    hook.pull_elapsed_seconds = 0.0f;
                    if (hook.id >= 0) {
                        PlaySfxIfVisible(sfx_grappling_latch_.sound, sfx_grappling_latch_.loaded, hook.latch_point);
                    }
                } else {
                    hook.phase = GrapplingHookPhase::Retracting;
                }
            } else {
                hook.head_pos = Vector2Add(hook.head_pos, Vector2Scale(Vector2Normalize(to_target), step));
            }
            continue;
        }

        if (hook.phase == GrapplingHookPhase::Pulling) {
            hook.pull_elapsed_seconds += dt;
            bool latch_valid = true;
            if (hook.latch_target_type == GrapplingHookLatchTargetType::IceWall) {
                latch_valid = std::any_of(state_.ice_walls.begin(), state_.ice_walls.end(),
                                          [&](const IceWallPiece& wall) {
                                              return wall.id == hook.latch_target_id && wall.alive &&
                                                     wall.state != IceWallState::Dying;
                                          });
            } else if (hook.latch_target_type == GrapplingHookLatchTargetType::MapObject) {
                latch_valid = std::any_of(state_.map_objects.begin(), state_.map_objects.end(),
                                          [&](const MapObjectInstance& object) {
                                              return object.id == hook.latch_target_id && object.alive &&
                                                     object.collision_enabled && object.stops_projectiles;
                                          });
            }
            if (!latch_valid) {
                hook.alive = false;
                continue;
            }

            if (hook.pull_elapsed_seconds >= hook.max_pull_duration_seconds) {
                owner->vel = {0.0f, 0.0f};
                hook.alive = false;
                continue;
            }

            owner->vel = {0.0f, 0.0f};
            owner->rune_placing_mode = false;
            owner->inventory_mode = false;
            owner->melee_active_remaining = 0.0f;

            const Vector2 to_destination = Vector2Subtract(hook.pull_destination, owner->pos);
            const float distance = Vector2Length(to_destination);
            const float step = Constants::kGrapplingHookPullSpeed * dt;
            if (distance <= Constants::kGrapplingHookArrivalEpsilon || distance <= step) {
                owner->pos = hook.pull_destination;
                owner->vel = {0.0f, 0.0f};
                CollisionWorld::ResolvePlayerVsWorld(state_.map, *owner);
                ResolvePlayerVsMapObjects(*owner);
                ResolvePlayerVsIceWalls(*owner);
                hook.alive = false;
            } else {
                owner->pos = Vector2Add(owner->pos, Vector2Scale(Vector2Normalize(to_destination), step));
                CollisionWorld::ResolvePlayerVsWorld(state_.map, *owner, false);
                ResolvePlayerVsMapObjects(*owner);
                ResolvePlayerVsIceWalls(*owner);
            }
            hook.head_pos = hook.latch_point;
            continue;
        }

        if (hook.phase == GrapplingHookPhase::Retracting) {
            const Vector2 retract_target =
                Vector2Add(owner->pos, {0.0f, 0.5f * static_cast<float>(state_.map.cell_size)});
            const Vector2 to_owner = Vector2Subtract(retract_target, hook.head_pos);
            const float distance = Vector2Length(to_owner);
            const float step = Constants::kGrapplingHookFireSpeed * Constants::kGrapplingHookRetractSpeedMultiplier * dt;
            if (distance <= step || distance <= 0.0001f) {
                hook.head_pos = retract_target;
                hook.alive = false;
            } else {
                hook.head_pos = Vector2Add(hook.head_pos, Vector2Scale(Vector2Normalize(to_owner), step));
            }
        }
    }

    state_.grappling_hooks.erase(
        std::remove_if(state_.grappling_hooks.begin(), state_.grappling_hooks.end(),
                       [](const GrapplingHook& hook) { return !hook.alive; }),
        state_.grappling_hooks.end());
}

void GameApp::UpdateCompositeEffects(float dt) {
    for (auto& effect : state_.composite_effects) {
        if (!effect.alive) {
            continue;
        }
        effect.age_seconds += dt;
        if (effect.duration_seconds > 0.0f && effect.age_seconds >= effect.duration_seconds) {
            effect.alive = false;
        }
    }

    state_.composite_effects.erase(
        std::remove_if(state_.composite_effects.begin(), state_.composite_effects.end(),
                       [](const CompositeEffectInstance& effect) { return !effect.alive; }),
        state_.composite_effects.end());
}

float GameApp::GetCompositeEffectDurationSeconds(const std::string& effect_id) const {
    const CompositeEffectDefinition* definition = composite_effects_loader_.FindById(effect_id);
    if (definition == nullptr) {
        return 0.0f;
    }

    const auto get_metadata = [&](CompositeEffectSheet sheet) -> const SpriteMetadataLoader* {
        switch (sheet) {
            case CompositeEffectSheet::Base32:
                return &sprite_metadata_;
            case CompositeEffectSheet::Tall32x64:
                return &sprite_metadata_tall_;
            case CompositeEffectSheet::Large96x96:
                return &sprite_metadata_96x96_;
        }
        return &sprite_metadata_;
    };

    float duration_seconds = 0.0f;
    for (const auto& part : definition->parts) {
        int frame_count = 0;
        float fps = 1.0f;
        const SpriteMetadataLoader* metadata = get_metadata(part.sheet);
        if (metadata != nullptr && metadata->GetAnimationStats(part.animation, "default", frame_count, fps) &&
            frame_count > 0) {
            duration_seconds = std::max(duration_seconds, static_cast<float>(frame_count) / std::max(0.001f, fps));
        }
    }
    return duration_seconds;
}

void GameApp::UpdateFireStormCasts(float dt) {
    for (auto& cast : state_.fire_storm_casts) {
        if (!cast.alive) {
            continue;
        }
        cast.elapsed_seconds += dt;
        if (!fire_storm_cast_impact_played_[cast.id] &&
            cast.elapsed_seconds >= Constants::kFireStormImpactDelaySeconds) {
            const Vector2 impact_center = CellToWorldCenter(cast.center_cell);
            PlaySfxIfVisible(sfx_fire_storm_impact_.sound, sfx_fire_storm_impact_.loaded, impact_center);
            if (IsWorldPointInsideCameraView(impact_center)) {
                camera_shake_time_remaining_ =
                    std::max(camera_shake_time_remaining_, Constants::kCameraShakeDurationSeconds);
            }
            fire_storm_cast_impact_played_[cast.id] = true;
        }

        if (!fire_storm_cast_arcs_spawned_[cast.id] && cast.elapsed_seconds >= Constants::kFireStormImpactDelaySeconds) {
            SpawnStormArcVisuals(cast);
            fire_storm_cast_arcs_spawned_[cast.id] = true;
        }

        const bool conversion_triggered_now =
            !fire_storm_cast_conversion_sfx_triggered_[cast.id] &&
            cast.elapsed_seconds >= GetFireStormConversionTriggerSeconds();
        if (conversion_triggered_now) {
            bool any_target_visible = false;
            for (const GridCoord& target : cast.target_cells) {
                const Vector2 world_pos = CellToWorldCenter(target);
                Particle death_particle;
                death_particle.pos = world_pos;
                death_particle.size = Constants::kFireStormStormProjectileSize;
                death_particle.alpha = 255;
                death_particle.animation_key = "storm_particle_death";
                death_particle.facing = "default";
                death_particle.max_cycles = 1;
                death_particle.alive = true;
                state_.particles.push_back(death_particle);
                PlaySfxIfVisible(sfx_fire_storm_impact_.sound, sfx_fire_storm_impact_.loaded, world_pos);
                any_target_visible = any_target_visible || IsWorldPointInsideCameraView(world_pos);
            }
            if (any_target_visible) {
                camera_shake_time_remaining_ =
                    std::max(camera_shake_time_remaining_, Constants::kCameraShakeDurationSeconds);
            }
            fire_storm_cast_conversion_sfx_triggered_[cast.id] = true;
        }

        if (network_manager_.IsHost() && conversion_triggered_now) {
            for (const GridCoord& target : cast.target_cells) {
                auto rune_it = std::find_if(state_.runes.begin(), state_.runes.end(), [&](const Rune& rune) {
                    return rune.cell == target && rune.active && rune.rune_type == RuneType::Fire;
                });
                if (rune_it != state_.runes.end()) {
                    ConvertRuneToFireStorm(*rune_it, cast.owner_player_id, cast.owner_team, true, false);
                }
            }
            for (const GridCoord& source_cell : cast.source_cells) {
                auto rune_it = std::find_if(state_.runes.begin(), state_.runes.end(), [&](const Rune& rune) {
                    return rune.cell == source_cell && rune.rune_type == RuneType::FireStormDummy &&
                           rune.fire_storm_source_rune;
                });
                if (rune_it == state_.runes.end()) {
                    SpawnFireStormConvertedRuneAtCell(cast.owner_player_id, cast.owner_team, source_cell, true);
                } else {
                    ConvertRuneToFireStorm(*rune_it, cast.owner_player_id, cast.owner_team, true, true);
                }
            }
            RebuildInfluenceZones();
        }

        if (cast.elapsed_seconds >= cast.duration_seconds) {
            cast.alive = false;
        }
    }

    const auto prune_cast_flag_map = [&](auto& map) {
        for (auto it = map.begin(); it != map.end();) {
            const bool still_alive = std::any_of(state_.fire_storm_casts.begin(), state_.fire_storm_casts.end(),
                                                 [&](const FireStormCast& cast) { return cast.id == it->first && cast.alive; });
            if (!still_alive) {
                it = map.erase(it);
            } else {
                ++it;
            }
        }
    };

    prune_cast_flag_map(fire_storm_cast_impact_played_);
    prune_cast_flag_map(fire_storm_cast_arcs_spawned_);
    prune_cast_flag_map(fire_storm_cast_conversion_sfx_triggered_);

    state_.fire_storm_casts.erase(
        std::remove_if(state_.fire_storm_casts.begin(), state_.fire_storm_casts.end(),
                       [](const FireStormCast& cast) { return !cast.alive; }),
        state_.fire_storm_casts.end());
}

void GameApp::UpdateFireStormDummies(float dt) {
    std::uniform_real_distribution<float> chance_dist(0.0f, 1.0f);
    std::uniform_int_distribution<int> frame_dist(Constants::kFireStormDummyLightningMinFrames,
                                                  Constants::kFireStormDummyLightningMaxFrames);
    int lightning_frame_count = 0;
    float lightning_fps = 8.0f;
    if (sprite_metadata_.GetAnimationStats("fire_storm_top_lightning", "default", lightning_frame_count, lightning_fps) &&
        lightning_frame_count > 0 && lightning_fps > 0.001f) {
        // use animation fps directly
    } else {
        lightning_fps = 8.0f;
    }
    const float frame_seconds = 1.0f / std::max(0.001f, lightning_fps);

    std::unordered_set<int> live_ids;
    for (const auto& rune : state_.runes) {
        const bool fire_storm_rune =
            rune.rune_type == RuneType::FireStormDummy || rune.fire_storm_original_rune_type != RuneType::None;
        if (!fire_storm_rune) {
            continue;
        }
        live_ids.insert(rune.id);
    }
    for (auto& dummy : state_.fire_storm_dummies) {
        if (!dummy.alive) {
            continue;
        }
        live_ids.insert(dummy.id);
        dummy.state_time += dt;
        if (dummy.state == FireStormDummyState::Born && dummy.state_time >= dummy.state_duration) {
            dummy.state = FireStormDummyState::Idle;
            dummy.state_time = 0.0f;
            dummy.state_duration = 0.0f;
        } else if (dummy.state == FireStormDummyState::Idle && dummy.idle_lifetime_remaining_seconds > 0.0f) {
            dummy.idle_lifetime_remaining_seconds = std::max(0.0f, dummy.idle_lifetime_remaining_seconds - dt);
            if (dummy.idle_lifetime_remaining_seconds <= 0.0f) {
                dummy.state = FireStormDummyState::Dying;
                dummy.state_time = 0.0f;
                dummy.state_duration = GetCompositeEffectDurationSeconds("fire_storm_death");
            }
        } else if (dummy.state == FireStormDummyState::Dying && dummy.state_time >= dummy.state_duration) {
            dummy.alive = false;
        }

        float& lightning_seconds = fire_storm_dummy_lightning_seconds_remaining_[dummy.id];
        float& lightning_cooldown_seconds = fire_storm_dummy_lightning_cooldown_seconds_remaining_[dummy.id];
        if (dummy.state != FireStormDummyState::Idle || !dummy.alive) {
            lightning_seconds = 0.0f;
            lightning_cooldown_seconds = 0.0f;
            continue;
        }
        if (lightning_seconds > 0.0f) {
            lightning_seconds = std::max(0.0f, lightning_seconds - dt);
            if (lightning_seconds <= 0.0f) {
                lightning_cooldown_seconds =
                    static_cast<float>(Constants::kFireStormDummyLightningCooldownFrames) * frame_seconds;
            }
        } else if (lightning_cooldown_seconds > 0.0f) {
            lightning_cooldown_seconds = std::max(0.0f, lightning_cooldown_seconds - dt);
        } else if (chance_dist(visual_rng_) < Constants::kFireStormDummyLightningChancePerFrame) {
            lightning_seconds = static_cast<float>(frame_dist(visual_rng_)) * frame_seconds;
        }
    }

    state_.fire_storm_dummies.erase(
        std::remove_if(state_.fire_storm_dummies.begin(), state_.fire_storm_dummies.end(),
                       [](const FireStormDummy& dummy) { return !dummy.alive; }),
        state_.fire_storm_dummies.end());

    for (auto it = fire_storm_dummy_lightning_seconds_remaining_.begin();
         it != fire_storm_dummy_lightning_seconds_remaining_.end();) {
        if (live_ids.find(it->first) == live_ids.end()) {
            it = fire_storm_dummy_lightning_seconds_remaining_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = fire_storm_dummy_lightning_cooldown_seconds_remaining_.begin();
         it != fire_storm_dummy_lightning_cooldown_seconds_remaining_.end();) {
        if (live_ids.find(it->first) == live_ids.end()) {
            it = fire_storm_dummy_lightning_cooldown_seconds_remaining_.erase(it);
        } else {
            ++it;
        }
    }
}

void GameApp::UpdateProjectileEmitters() {
    for (auto& projectile : state_.projectiles) {
        smoke_emitter_.UpdateAndEmit(projectile, state_.particles);
    }
}

void GameApp::UpdateSnowParticleEmitters(float dt) {
    if (dt <= 0.0f) {
        return;
    }

    std::poisson_distribution<int> shard_spawn_dist(Constants::kIceWaveShardSnowSpawnRatePerSecond * dt);
    for (const auto& projectile : state_.projectiles) {
        if (!projectile.alive || !IsIceShardProjectile(projectile)) {
            continue;
        }

        const int spawn_count = shard_spawn_dist(visual_rng_);
        for (int i = 0; i < spawn_count; ++i) {
            SpawnIceWaveShardSnowParticle(projectile);
        }
    }

    std::poisson_distribution<int> frozen_spawn_dist(Constants::kFrozenSnowSpawnRatePerSecond * dt);
    for (const auto& player : state_.players) {
        if (!player.alive || !HasStatusEffect(player, StatusEffectType::Frozen)) {
            continue;
        }

        const int spawn_count = frozen_spawn_dist(visual_rng_);
        for (int i = 0; i < spawn_count; ++i) {
            SpawnFrozenStatusSnowParticle(player);
        }
    }
}

void GameApp::SpawnIceWaveShardSnowParticle(const Projectile& projectile) {
    std::uniform_real_distribution<float> z_dist(Constants::kIceWaveShardSnowSpawnZMin,
                                                 Constants::kIceWaveShardSnowSpawnZMax);
    std::uniform_real_distribution<float> scale_dist(Constants::kIceWaveShardSnowScaleMin,
                                                     Constants::kIceWaveShardSnowScaleMax);
    std::uniform_real_distribution<float> phase_dist(0.0f, 2.0f * PI);

    Particle particle;
    particle.pos = projectile.pos;
    particle.vel = {0.0f, 0.0f};
    particle.acc = {0.0f, 0.0f};
    particle.drag = 0.0f;
    particle.size = Constants::kSnowParticleBaseSize * scale_dist(visual_rng_);
    particle.alpha = 255;
    particle.animation_key = "snow_particle_born";
    particle.idle_animation_key = "snow_particle_idle";
    particle.death_animation_key = "snow_particle_death";
    particle.facing = "default";
    particle.age_seconds = 0.0f;
    particle.max_cycles = 1;
    particle.use_visual_z = true;
    particle.visual_z = z_dist(visual_rng_);
    particle.visual_z_velocity = Constants::kSnowParticleFallSpeed;
    particle.sway_amplitude = Constants::kSnowParticleSwayAmplitude;
    particle.sway_frequency_hz = Constants::kSnowParticleSwayFrequencyHz;
    particle.sway_phase = phase_dist(visual_rng_);
    particle.alive = true;
    state_.particles.push_back(particle);
}

void GameApp::SpawnFrozenStatusSnowParticle(const Player& player) {
    std::uniform_real_distribution<float> angle_dist(0.0f, 2.0f * PI);
    std::normal_distribution<float> radius_offset_dist(0.0f, Constants::kFrozenSnowSpawnRadiusStdDev);
    std::uniform_real_distribution<float> z_dist(Constants::kFrozenSnowSpawnZMin, Constants::kFrozenSnowSpawnZMax);
    std::uniform_real_distribution<float> phase_dist(0.0f, 2.0f * PI);

    const float angle = angle_dist(visual_rng_);
    const float radius = std::max(0.0f, Constants::kFrozenSnowSpawnRadiusBase + radius_offset_dist(visual_rng_));

    Particle particle;
    particle.pos = {player.pos.x + std::cos(angle) * radius, player.pos.y + std::sin(angle) * radius};
    particle.vel = {0.0f, 0.0f};
    particle.acc = {0.0f, 0.0f};
    particle.drag = 0.0f;
    particle.size = Constants::kSnowParticleBaseSize;
    particle.alpha = 255;
    particle.animation_key = "snow_particle_born";
    particle.idle_animation_key = "snow_particle_idle";
    particle.death_animation_key = "snow_particle_death";
    particle.facing = "default";
    particle.age_seconds = 0.0f;
    particle.max_cycles = 1;
    particle.use_visual_z = true;
    particle.visual_z = z_dist(visual_rng_);
    particle.visual_z_velocity = Constants::kSnowParticleFallSpeed;
    particle.sway_amplitude = Constants::kSnowParticleSwayAmplitude;
    particle.sway_frequency_hz = Constants::kSnowParticleSwayFrequencyHz;
    particle.sway_phase = phase_dist(visual_rng_);
    particle.alive = true;
    state_.particles.push_back(particle);
}

void GameApp::SpawnIceWallHealSnowBurst(const IceWallPiece& wall, int healed_amount) {
    std::uniform_real_distribution<float> angle_dist(0.0f, 2.0f * PI);
    std::uniform_real_distribution<float> radius_dist(0.0f, 12.0f);
    std::uniform_real_distribution<float> z_dist(Constants::kFrozenSnowSpawnZMin, Constants::kFrozenSnowSpawnZMax);
    std::uniform_real_distribution<float> phase_dist(0.0f, 2.0f * PI);
    std::uniform_real_distribution<float> scale_dist(0.85f, 1.20f);

    const Vector2 center = CellToWorldCenter(wall.cell);
    const int particle_count = std::clamp(2 + healed_amount / 4, 2, 8);
    for (int i = 0; i < particle_count; ++i) {
        const float angle = angle_dist(visual_rng_);
        const float radius = radius_dist(visual_rng_);

        Particle particle;
        particle.pos = {center.x + std::cos(angle) * radius, center.y + std::sin(angle) * radius};
        particle.vel = {0.0f, 0.0f};
        particle.acc = {0.0f, 0.0f};
        particle.drag = 0.0f;
        particle.size = Constants::kSnowParticleBaseSize * scale_dist(visual_rng_);
        particle.alpha = 255;
        particle.animation_key = "snow_particle_born";
        particle.idle_animation_key = "snow_particle_idle";
        particle.death_animation_key = "snow_particle_death";
        particle.facing = "default";
        particle.age_seconds = 0.0f;
        particle.max_cycles = 1;
        particle.use_visual_z = true;
        particle.visual_z = z_dist(visual_rng_);
        particle.visual_z_velocity = Constants::kSnowParticleFallSpeed;
        particle.sway_amplitude = Constants::kSnowParticleSwayAmplitude;
        particle.sway_frequency_hz = Constants::kSnowParticleSwayFrequencyHz;
        particle.sway_phase = phase_dist(visual_rng_);
        particle.alive = true;
        state_.particles.push_back(particle);
    }
}

void GameApp::SpawnStormArcSparkParticle(Vector2 world_pos, float visual_z) {
    std::uniform_real_distribution<float> size_dist(Constants::kFireStormSparkSizeMin, Constants::kFireStormSparkSizeMax);
    std::uniform_real_distribution<float> phase_dist(0.0f, 2.0f * PI);

    Particle particle;
    particle.pos = world_pos;
    particle.size = size_dist(visual_rng_);
    particle.alpha = 255;
    particle.animation_key = "spark_particle_born";
    particle.idle_animation_key = "spark_particle_idle";
    particle.death_animation_key = "spark_particle_death";
    particle.facing = "default";
    particle.max_cycles = 1;
    particle.use_visual_z = true;
    particle.visual_z = std::max(0.0f, visual_z);
    particle.visual_z_velocity = Constants::kFireStormSparkFallSpeed;
    particle.sway_amplitude = Constants::kFireStormSparkSwayAmplitude;
    particle.sway_frequency_hz = Constants::kFireStormSparkSwayFrequencyHz;
    particle.sway_phase = phase_dist(visual_rng_);
    particle.alive = true;
    state_.particles.push_back(particle);
}

void GameApp::SpawnFireSpiritSparkParticle(Vector2 world_pos, Vector2 travel_dir) {
    if (!sprite_metadata_.IsLoaded() || !sprite_metadata_.HasAnimation("spark_particle_born")) {
        return;
    }

    Vector2 direction = travel_dir;
    if (Vector2LengthSqr(direction) <= 0.0001f) {
        direction = {1.0f, 0.0f};
    } else {
        direction = Vector2Normalize(direction);
    }
    std::normal_distribution<float> angle_noise_dist(0.0f, Constants::kFireSpiritSparkDirectionNoiseStdDevRadians);
    direction = Vector2Rotate(direction, angle_noise_dist(visual_rng_));

    Particle particle;
    particle.pos = world_pos;
    particle.vel = Vector2Scale(Vector2Negate(direction), Constants::kFireSpiritSparkSpeed);
    particle.acc = {0.0f, 0.0f};
    particle.drag = Constants::kFireSpiritSparkDrag;
    particle.velocity_decay = 0.0f;
    particle.size = Constants::kFireSpiritSparkSize;
    particle.alpha = 255;
    particle.animation_key = "spark_particle_born";
    particle.death_animation_key = "spark_particle_death";
    particle.facing = "default";
    particle.age_seconds = 0.0f;
    particle.max_cycles = 1;
    particle.use_visual_z = false;
    particle.render_on_top = true;
    particle.alive = true;
    state_.particles.push_back(particle);
}

void GameApp::UpdateFireStormArcVisuals(float dt) {
    if (dt <= 0.0f) {
        return;
    }

    for (auto& arc : storm_arc_visuals_) {
        if (!arc.alive) {
            continue;
        }

        arc.elapsed_seconds += dt;
        if (arc.elapsed_seconds >= arc.next_spark_emit_seconds) {
            const float t = std::clamp(arc.elapsed_seconds / std::max(0.001f, arc.duration_seconds), 0.0f, 1.0f);
            const Vector2 world_pos = Vector2Lerp(arc.start_world, arc.end_world, t);
            SpawnStormArcSparkParticle(world_pos, EvaluateStormArcHeight(t));
            const float interval = 1.0f / std::max(0.001f, Constants::kFireStormSparkSpawnRatePerSecond);
            std::uniform_real_distribution<float> jitter_dist(0.6f, 1.4f);
            arc.next_spark_emit_seconds += interval * jitter_dist(visual_rng_);
        }

        if (arc.elapsed_seconds >= arc.duration_seconds) {
            arc.alive = false;
        }
    }

    storm_arc_visuals_.erase(std::remove_if(storm_arc_visuals_.begin(), storm_arc_visuals_.end(),
                                            [](const StormArcVisual& arc) { return !arc.alive; }),
                             storm_arc_visuals_.end());
}

void GameApp::SpawnDamageHitParticles(Vector2 target_pos, std::optional<Vector2> damage_world_pos) {
    Vector2 damage_dir = {0.0f, -1.0f};
    if (damage_world_pos.has_value()) {
        Vector2 delta = Vector2Subtract(target_pos, *damage_world_pos);
        if (Vector2LengthSqr(delta) > 0.0001f) {
            damage_dir = Vector2Normalize(delta);
        }
    }

    std::uniform_real_distribution<float> spread_dist(-Constants::kDamageHitParticleSpreadRadians,
                                                      Constants::kDamageHitParticleSpreadRadians);
    std::uniform_real_distribution<float> speed_dist(Constants::kDamageHitParticleBaseSpeed -
                                                         Constants::kDamageHitParticleSpeedJitter,
                                                     Constants::kDamageHitParticleBaseSpeed +
                                                         Constants::kDamageHitParticleSpeedJitter);
    std::uniform_real_distribution<float> size_dist(Constants::kDamageHitParticleBaseSize -
                                                        Constants::kDamageHitParticleSizeJitter,
                                                    Constants::kDamageHitParticleBaseSize +
                                                        Constants::kDamageHitParticleSizeJitter);
    std::uniform_real_distribution<float> pos_jitter_dist(-4.0f, 4.0f);

    const float base_angle = std::atan2(damage_dir.y, damage_dir.x);
    for (int i = 0; i < Constants::kDamageHitParticlesPerBurst; ++i) {
        const float angle = base_angle + spread_dist(visual_rng_);
        const float speed = std::max(0.0f, speed_dist(visual_rng_));
        Particle particle;
        particle.pos = {target_pos.x + pos_jitter_dist(visual_rng_), target_pos.y + pos_jitter_dist(visual_rng_)};
        particle.vel = {std::cos(angle) * speed, std::sin(angle) * speed};
        particle.acc = {0.0f, 0.0f};
        particle.drag = 0.0f;
        particle.velocity_decay = Constants::kDamageHitParticleVelocityDecay;
        particle.size = std::max(6.0f, size_dist(visual_rng_));
        particle.size_decay = Constants::kDamageHitParticleSizeDecay;
        particle.lifetime_seconds = Constants::kDamageHitParticleLifetimeSeconds;
        particle.alpha = 255;
        particle.animation_key = "particle_hit_general";
        particle.facing = "default";
        particle.age_seconds = 0.0f;
        particle.max_cycles = 1;
        particle.alive = true;
        state_.particles.push_back(particle);
    }
}

void GameApp::UpdateParticles(float dt) {
    for (auto& particle : state_.particles) {
        if (!particle.alive) {
            continue;
        }

        particle.vel = Vector2Add(particle.vel, Vector2Scale(particle.acc, dt));
        if (particle.velocity_decay > 0.0f) {
            particle.vel = Vector2Scale(particle.vel, std::exp(-particle.velocity_decay * dt));
        }
        const float drag_factor = std::max(0.0f, 1.0f - particle.drag * dt);
        particle.vel = Vector2Scale(particle.vel, drag_factor);
        particle.pos = Vector2Add(particle.pos, Vector2Scale(particle.vel, dt));
        if (particle.size_decay > 0.0f) {
            particle.size = std::max(0.0f, particle.size * std::exp(-particle.size_decay * dt));
        }
        particle.age_seconds += dt;

        if (particle.use_visual_z &&
            (IsSnowParticleAnimation(particle.animation_key) || IsSparkParticleAnimation(particle.animation_key))) {
            if (particle.animation_key != particle.death_animation_key) {
                particle.visual_z = std::max(0.0f, particle.visual_z - particle.visual_z_velocity * dt);
            }

            const float animation_seconds =
                GetAnimationDurationSeconds(sprite_metadata_, particle.animation_key, particle.facing, particle.max_cycles, 0.0f);
            const bool in_born_animation =
                particle.animation_key == "snow_particle_born" || particle.animation_key == "spark_particle_born";
            if (in_born_animation) {
                if (animation_seconds > 0.0f && particle.age_seconds >= animation_seconds) {
                    particle.animation_key = particle.idle_animation_key;
                    particle.age_seconds = 0.0f;
                }
            } else if (particle.animation_key == particle.idle_animation_key) {
                if (particle.visual_z <= 0.0f) {
                    if (!particle.death_animation_key.empty()) {
                        particle.animation_key = particle.death_animation_key;
                        particle.age_seconds = 0.0f;
                    } else {
                        particle.alive = false;
                    }
                }
            } else if (particle.animation_key == particle.death_animation_key) {
                if (animation_seconds <= 0.0f || particle.age_seconds >= animation_seconds) {
                    particle.alive = false;
                }
            }
            continue;
        }

        if (!particle.use_visual_z && IsSparkParticleAnimation(particle.animation_key)) {
            const float animation_seconds =
                GetAnimationDurationSeconds(sprite_metadata_, particle.animation_key, particle.facing, particle.max_cycles, 0.0f);
            if (particle.animation_key == "spark_particle_born") {
                if (animation_seconds <= 0.0f || particle.age_seconds >= animation_seconds) {
                    if (!particle.death_animation_key.empty()) {
                        particle.animation_key = particle.death_animation_key;
                        particle.age_seconds = 0.0f;
                    } else {
                        particle.alive = false;
                    }
                }
            } else if (particle.animation_key == particle.death_animation_key) {
                if (animation_seconds <= 0.0f || particle.age_seconds >= animation_seconds) {
                    particle.alive = false;
                }
            }
            continue;
        }

        int frame_count = 0;
        float fps = 1.0f;
        float lifetime_seconds = particle.lifetime_seconds > 0.0f
                                     ? particle.lifetime_seconds
                                     : (0.33f * static_cast<float>(std::max(1, particle.max_cycles)));
        if (particle.lifetime_seconds <= 0.0f &&
            sprite_metadata_.GetAnimationStats(particle.animation_key, particle.facing, frame_count, fps)) {
            const float cycle_seconds = static_cast<float>(frame_count) / std::max(0.001f, fps);
            lifetime_seconds = cycle_seconds * static_cast<float>(std::max(1, particle.max_cycles));
        }
        if (particle.age_seconds >= lifetime_seconds) {
            particle.alive = false;
        }
    }

    state_.particles.erase(std::remove_if(state_.particles.begin(), state_.particles.end(),
                                          [](const Particle& particle) { return !particle.alive; }),
                           state_.particles.end());
}

void GameApp::UpdateHammerImpactEffects(float dt) {
    for (auto& effect : hammer_impact_effects_) {
        if (!effect.alive) {
            continue;
        }
        effect.age_seconds += dt;
        if (effect.age_seconds >= effect.duration_seconds) {
            effect.alive = false;
        }
    }

    hammer_impact_effects_.erase(
        std::remove_if(hammer_impact_effects_.begin(), hammer_impact_effects_.end(),
                       [](const HammerImpactEffect& effect) { return !effect.alive; }),
        hammer_impact_effects_.end());
}

void GameApp::SpawnHammerImpactEffect(Vector2 world_pos) {
    if (!sprite_metadata_128x128_.IsLoaded() || !sprite_metadata_128x128_.HasAnimation("hammer_impact")) {
        return;
    }

    int frame_count = 0;
    float fps = 1.0f;
    if (!sprite_metadata_128x128_.GetAnimationStats("hammer_impact", "default", frame_count, fps) || frame_count <= 0) {
        return;
    }

    HammerImpactEffect effect;
    effect.world_pos = world_pos;
    effect.duration_seconds = static_cast<float>(frame_count) / std::max(0.001f, fps);
    hammer_impact_effects_.push_back(effect);
}

void GameApp::ResolvePlayerCollisions() {
    for (size_t i = 0; i < state_.players.size(); ++i) {
        for (size_t j = i + 1; j < state_.players.size(); ++j) {
            Player& a = state_.players[i];
            Player& b = state_.players[j];
            if (!a.alive || !b.alive) {
                continue;
            }

            Vector2 normal = {0.0f, 0.0f};
            float penetration = 0.0f;
            if (!CollisionWorld::AabbVsAabb(GetPlayerCollisionRect(a), GetPlayerCollisionRect(b), normal, penetration)) {
                continue;
            }

            // AabbVsAabb returns a normal that pushes the first box out of the second.
            const Vector2 push_dir_a = normal;
            const Vector2 push_dir_b = Vector2Negate(normal);
            a.pos = Vector2Add(a.pos, Vector2Scale(push_dir_a, penetration * 0.5f));
            b.pos = Vector2Add(b.pos, Vector2Scale(push_dir_b, penetration * 0.5f));

            const float a_dot = Vector2DotProduct(a.vel, push_dir_a);
            if (a_dot < 0.0f) {
                a.vel = Vector2Subtract(a.vel, Vector2Scale(push_dir_a, a_dot));
            }

            const float b_dot = Vector2DotProduct(b.vel, push_dir_b);
            if (b_dot < 0.0f) {
                b.vel = Vector2Subtract(b.vel, Vector2Scale(push_dir_b, b_dot));
            }
        }
    }

    for (auto& player : state_.players) {
        ResolvePlayerVsMapObjects(player);
        ResolvePlayerVsIceWalls(player);
    }
}

void GameApp::HandleMeleeHit(Player& attacker) {
    const AttackProfile* attack_profile = GetEquippedPrimaryAttack(attacker);
    if (attack_profile == nullptr || attack_profile->kind != EquipmentActionKind::Melee) {
        return;
    }

    const float attack_elapsed = attack_profile->active_window_seconds - attacker.melee_active_remaining;
    if (attack_elapsed < attack_profile->hit_start_seconds || attack_elapsed > attack_profile->hit_end_seconds) {
        return;
    }

    const HitShapeDefinition* hit_shape = hit_shape_library_.FindById(attack_profile->hit_shape_id);
    if (hit_shape == nullptr) {
        return;
    }
    const float rotation = GetPlayerLockedMeleeAimRadians(attacker);
    const Vector2 melee_origin = GetPlayerMeleeRotationOrigin(attacker);

    for (auto& target : state_.players) {
        if (!target.alive || target.team == attacker.team || target.id == attacker.id) {
            continue;
        }
        if (std::find(attacker.melee_hit_target_ids.begin(), attacker.melee_hit_target_ids.end(), target.id) !=
            attacker.melee_hit_target_ids.end()) {
            continue;
        }

        Vector2 normal = {0.0f, 0.0f};
        float penetration = 0.0f;
        if (!HitShapeLibrary::OverlapsAabb(*hit_shape, melee_origin, rotation, GetPlayerCollisionRect(target), &normal,
                                           &penetration)) {
            continue;
        }

        ApplyDamageToPlayer(target, attacker.id, attack_profile->damage, "melee", true, attacker.pos);
        attacker.melee_hit_target_ids.push_back(target.id);
    }

    for (auto& object : state_.map_objects) {
        if (!object.alive || object.hp <= 0) {
            continue;
        }
        if (std::find(attacker.melee_hit_object_ids.begin(), attacker.melee_hit_object_ids.end(), object.id) !=
            attacker.melee_hit_object_ids.end()) {
            continue;
        }

        const Rectangle aabb = GetMapObjectCollisionAabb(object, FindObjectPrototype(object.prototype_id));
        Vector2 normal = {0.0f, 0.0f};
        float penetration = 0.0f;
        if (!HitShapeLibrary::OverlapsAabb(*hit_shape, melee_origin, rotation, aabb, &normal, &penetration)) {
            continue;
        }

        if (ApplyObjectDamage(object.id, attack_profile->damage, attacker.id, "melee", attacker.pos)) {
            attacker.melee_hit_object_ids.push_back(object.id);
        }
    }

    for (auto& dummy : state_.fire_storm_dummies) {
        if (!dummy.alive || dummy.state == FireStormDummyState::Dying) {
            continue;
        }
        const Rectangle aabb = {static_cast<float>(dummy.cell.x * state_.map.cell_size),
                                static_cast<float>(dummy.cell.y * state_.map.cell_size),
                                static_cast<float>(state_.map.cell_size), static_cast<float>(state_.map.cell_size)};
        Vector2 normal = {0.0f, 0.0f};
        float penetration = 0.0f;
        if (!HitShapeLibrary::OverlapsAabb(*hit_shape, melee_origin, rotation, aabb, &normal, &penetration)) {
            continue;
        }

        dummy.state = FireStormDummyState::Dying;
        dummy.state_time = 0.0f;
        dummy.state_duration = GetCompositeEffectDurationSeconds("fire_storm_death");
        fire_storm_dummy_lightning_seconds_remaining_[dummy.id] = 0.0f;
    }
}

void GameApp::HandleEventsHost() {
    size_t index = 0;
    while (index < event_queue_.GetEvents().size()) {
        const Event event = event_queue_.GetEvents()[index];
        if (std::holds_alternative<RunePlacedEvent>(event)) {
            CheckSpellPatterns(std::get<RunePlacedEvent>(event));
        } else if (std::holds_alternative<MatchEndedEvent>(event)) {
            winning_team_ = std::get<MatchEndedEvent>(event).winning_team;
            if (state_.match.kill_timeline.empty() ||
                std::fabs(state_.match.kill_timeline.back().elapsed_seconds - state_.match.elapsed_seconds) > 0.001f) {
                RecordKillTimelinePoint();
            }
        }
        ++index;
    }

    event_queue_.Clear();

    const size_t before_rune_count = state_.runes.size();
    state_.runes.erase(std::remove_if(state_.runes.begin(), state_.runes.end(),
                                      [](const Rune& rune) {
                                          if (rune.fire_storm_pending_removal) {
                                              return true;
                                          }
                                          return !rune.active && rune.activation_remaining_seconds <= 0.0f &&
                                                 rune.activation_total_seconds <= 0.0f &&
                                                 rune.fire_storm_visual_state != FireStormRuneVisualState::Dying;
                                      }),
                       state_.runes.end());
    if (state_.runes.size() != before_rune_count) {
        RebuildInfluenceZones();
    }
}

bool GameApp::TryPlaceRune(Player& player, Vector2 world_mouse) {
    if (player.selected_rune_type == RuneType::None || !IsRuneAvailableToPlayer(player, player.selected_rune_type)) {
        return false;
    }

    const GridCoord cell = WorldToCell(world_mouse);
    const Vector2 cell_center = CellToWorldCenter(cell);
    if (Vector2Distance(player.pos, cell_center) > GetPlayerRuneCastRangeWorld(player)) {
        return false;
    }
    if (!IsTileRunePlaceable(cell) || IsCellOccupiedByRune(cell) || IsCellOccupiedByFireStormDummy(cell)) {
        return false;
    }
    if (player.selected_rune_type == RuneType::FireStormDummy) {
        return TryPlaceFireStormDummy(player, cell);
    }

    bool volatile_cast = false;
    float mana_cost = GetRunePlacementManaCost(player, player.selected_rune_type, cell, &volatile_cast);
    float activation_seconds = GetRuneActivationSeconds(player.selected_rune_type);
    if (volatile_cast) {
        activation_seconds *= Constants::kRuneVolatileActivationMultiplier;
    }
    if (player.mana < mana_cost) {
        ConsoleMessage console;
        console.lifetime_seconds = kConsoleLifetimeSeconds;
        console.spans.push_back(MakeConsoleSpan("Not enough mana", Color{255, 140, 140, 255}));
        if (network_manager_.IsHost() && player.id != 0) {
            network_manager_.SendConsoleMessageToPlayer(player.id, ConsoleMessageNet{console});
        } else if (player.id == state_.local_player_id) {
            AddConsoleMessage(console);
        }
        return false;
    }
    if (GetPlayerRuneCooldownRemaining(player, player.selected_rune_type) > 0.0f) {
        return false;
    }
    if (RuneUsesCharges(player.selected_rune_type) && GetPlayerRuneChargeCount(player, player.selected_rune_type) <= 0) {
        return false;
    }

    Rune rune;
    rune.id = state_.next_entity_id++;
    rune.owner_player_id = player.id;
    rune.owner_team = player.team;
    rune.cell = cell;
    rune.rune_type = player.selected_rune_type;
    rune.placement_order = state_.next_rune_placement_order++;
    rune.active = false;
    rune.volatile_cast = volatile_cast;
    rune.activation_total_seconds = activation_seconds;
    rune.activation_remaining_seconds = activation_seconds;
    rune.creates_influence_zone = !volatile_cast;
    state_.runes.push_back(rune);

    Particle rune_cast_vfx;
    rune_cast_vfx.pos = cell_center;
    rune_cast_vfx.vel = {0.0f, 0.0f};
    rune_cast_vfx.acc = {0.0f, 0.0f};
    rune_cast_vfx.drag = 0.0f;
    rune_cast_vfx.animation_key = volatile_cast ? "rune_cast_volatile_effect" : "rune_cast_effect";
    rune_cast_vfx.facing = "default";
    rune_cast_vfx.age_seconds = 0.0f;
    rune_cast_vfx.max_cycles = 1;
    rune_cast_vfx.alive = true;
    state_.particles.push_back(rune_cast_vfx);

    SpawnLightningEffect(player.pos, cell_center, 0.3f, volatile_cast);
    PlaySfxIfVisible(sfx_create_rune_.sound, sfx_create_rune_.loaded, cell_center);
    if (volatile_cast) {
        SpawnVolatileCastCasterFx(player.id);
    }

    player.mana = std::max(0.0f, player.mana - mana_cost);
    if (RuneUsesCharges(player.selected_rune_type)) {
        SetPlayerRuneChargeCount(player, player.selected_rune_type,
                                 GetPlayerRuneChargeCount(player, player.selected_rune_type) - 1);
    }
    ApplyRuneCooldownsAfterCast(player, player.selected_rune_type);
    player.rune_placing_mode = false;
    return true;
}

bool GameApp::IsVolatileCastCellForPlayer(const Player& player, const GridCoord& cell) const {
    if (!IsInfluenceZoneSystemEnabled()) {
        return false;
    }
    return std::any_of(state_.influence_zones.begin(), state_.influence_zones.end(), [&](const InfluenceZoneCell& influence) {
        return influence.cell == cell && influence.team != player.team;
    });
}

float GameApp::GetRunePlacementManaCost(const Player& player, RuneType rune_type, const GridCoord& cell,
                                        bool* out_volatile_cast) const {
    const bool volatile_cast = IsVolatileCastCellForPlayer(player, cell);
    if (out_volatile_cast != nullptr) {
        *out_volatile_cast = volatile_cast;
    }

    float mana_cost = GetRuneManaCost(rune_type);
    if (volatile_cast) {
        mana_cost *= Constants::kRuneVolatileManaMultiplier;
    }
    return mana_cost;
}

void GameApp::SpawnVolatileCastCasterFx(int player_id) {
    const Player* player = FindPlayerById(player_id);
    if (player == nullptr || !player->alive) {
        return;
    }

    SpawnPlayerAttachedAnimation(player_id, "volatile_cast_effect", 0.0f, 0.0f);
    PlaySfxIfVisible(sfx_volatile_cast_.sound, sfx_volatile_cast_.loaded, player->pos);
}

void GameApp::SpawnPlayerAttachedAnimation(int player_id, const std::string& animation_key, float offset_x,
                                           float offset_y) {
    if (player_id < 0 || animation_key.empty() || !sprite_metadata_.IsLoaded() ||
        !sprite_metadata_.HasAnimation(animation_key)) {
        return;
    }

    player_attached_animations_.erase(
        std::remove_if(player_attached_animations_.begin(), player_attached_animations_.end(),
                       [&](const PlayerAttachedAnimation& animation) {
                           return animation.player_id == player_id && animation.animation_key == animation_key;
                       }),
        player_attached_animations_.end());

    PlayerAttachedAnimation animation;
    animation.player_id = player_id;
    animation.animation_key = animation_key;
    animation.offset_x = offset_x;
    animation.offset_y = offset_y;
    animation.duration_seconds = GetAnimationDurationSeconds(sprite_metadata_, animation_key, "default", 1, 0.25f);
    player_attached_animations_.push_back(std::move(animation));
}

void GameApp::UpdateVolatileCastCasterFx(float dt) {
    for (auto it = player_attached_animations_.begin(); it != player_attached_animations_.end();) {
        it->age_seconds += dt;
        if (it->age_seconds >= it->duration_seconds || FindPlayerById(it->player_id) == nullptr) {
            it = player_attached_animations_.erase(it);
        } else {
            ++it;
        }
    }
}

bool GameApp::TryPlaceFireStormDummy(Player& player, const GridCoord& cell) {
    SpawnFireStormDummyAtCell(player.id, player.team, cell, -1.0f);

    ApplyRuneCooldownsAfterCast(player, player.selected_rune_type);
    player.rune_placing_mode = false;
    return true;
}

void GameApp::ConvertRuneToFireStorm(Rune& rune, int caster_player_id, int caster_team, bool temporary, bool source_rune) {
    if (rune.fire_storm_original_rune_type == RuneType::None) {
        rune.fire_storm_original_owner_player_id = rune.owner_player_id;
        rune.fire_storm_original_owner_team = rune.owner_team;
        rune.fire_storm_original_rune_type = rune.rune_type;
    }
    rune.owner_player_id = caster_player_id;
    rune.owner_team = caster_team;
    rune.rune_type = RuneType::FireStormDummy;
    rune.active = true;
    rune.creates_influence_zone = true;
    rune.fire_storm_temporary = temporary;
    rune.fire_storm_source_rune = source_rune;
    rune.fire_storm_remaining_seconds = Constants::kFireStormLifetimeSeconds;
    rune.fire_storm_visual_state = FireStormRuneVisualState::Born;
    rune.fire_storm_visual_state_time = 0.0f;
    rune.fire_storm_visual_state_duration = GetCompositeEffectDurationSeconds("fire_storm_born");
    rune.fire_storm_revert_after_death = false;
    rune.fire_storm_pending_removal = false;
}

void GameApp::BeginFireStormRuneRevert(Rune& rune, bool restore_after_death) {
    rune.active = false;
    rune.creates_influence_zone = false;
    rune.fire_storm_visual_state = FireStormRuneVisualState::Dying;
    rune.fire_storm_visual_state_time = 0.0f;
    rune.fire_storm_visual_state_duration = GetCompositeEffectDurationSeconds("fire_storm_death");
    rune.fire_storm_revert_after_death = restore_after_death;
}

void GameApp::FinalizeFireStormRuneDeath(Rune& rune) {
    if (rune.fire_storm_revert_after_death) {
        rune.rune_type = rune.fire_storm_original_rune_type == RuneType::None ? RuneType::Fire : rune.fire_storm_original_rune_type;
        if (!rune.fire_storm_source_rune) {
            rune.owner_player_id = rune.fire_storm_original_owner_player_id;
            rune.owner_team = rune.fire_storm_original_owner_team;
        }
        rune.active = true;
        rune.creates_influence_zone = true;
        rune.fire_storm_original_owner_player_id = -1;
        rune.fire_storm_original_owner_team = 0;
        rune.fire_storm_original_rune_type = RuneType::None;
        rune.fire_storm_temporary = false;
        rune.fire_storm_source_rune = false;
        rune.fire_storm_remaining_seconds = 0.0f;
        rune.fire_storm_visual_state = FireStormRuneVisualState::None;
        rune.fire_storm_visual_state_time = 0.0f;
        rune.fire_storm_visual_state_duration = 0.0f;
        rune.fire_storm_revert_after_death = false;
        rune.fire_storm_pending_removal = false;
        return;
    }
    rune.fire_storm_pending_removal = true;
}

void GameApp::SpawnFireStormConvertedRuneAtCell(int owner_player_id, int owner_team, const GridCoord& cell, bool source_rune) {
    Rune rune;
    rune.id = state_.next_entity_id++;
    rune.owner_player_id = owner_player_id;
    rune.owner_team = owner_team;
    rune.cell = cell;
    rune.rune_type = RuneType::Fire;
    rune.placement_order = state_.next_rune_placement_order++;
    rune.active = true;
    rune.creates_influence_zone = true;
    ConvertRuneToFireStorm(rune, owner_player_id, owner_team, true, source_rune);
    state_.runes.push_back(rune);
}

void GameApp::SpawnStormArcVisuals(const FireStormCast& cast) {
    if (cast.target_cells.empty()) {
        return;
    }
    std::uniform_real_distribution<float> offset_dist(0.0f, 1.0f / std::max(0.001f, Constants::kFireStormSparkSpawnRatePerSecond));
    const Vector2 start_world = CellToWorldCenter(cast.center_cell);
    for (const GridCoord& cell : cast.target_cells) {
        Particle born_particle;
        born_particle.pos = start_world;
        born_particle.size = Constants::kFireStormStormProjectileSize;
        born_particle.alpha = 255;
        born_particle.animation_key = "storm_particle_born";
        born_particle.facing = "default";
        born_particle.max_cycles = 1;
        born_particle.alive = true;
        state_.particles.push_back(born_particle);

        const Vector2 end_world = CellToWorldCenter(cell);
        const Vector2 delta = Vector2Subtract(end_world, start_world);
        StormArcVisual visual;
        visual.cast_id = cast.id;
        visual.start_world = start_world;
        visual.end_world = end_world;
        visual.duration_seconds = Constants::kFireStormStormProjectileTravelSeconds;
        visual.peak_height = Constants::kFireStormStormArcPeakHeight;
        visual.rotation_degrees = AimToDegrees(delta);
        visual.next_spark_emit_seconds = offset_dist(visual_rng_);
        visual.alive = true;
        storm_arc_visuals_.push_back(visual);
    }
}

void GameApp::SpawnFireStormDummyAtCell(int owner_player_id, int owner_team, const GridCoord& cell,
                                        float idle_lifetime_seconds) {
    FireStormDummy dummy;
    dummy.id = state_.next_entity_id++;
    dummy.owner_player_id = owner_player_id;
    dummy.owner_team = owner_team;
    dummy.cell = cell;
    dummy.state = FireStormDummyState::Born;
    dummy.state_time = 0.0f;
    dummy.state_duration = GetCompositeEffectDurationSeconds("fire_storm_born");
    dummy.idle_lifetime_remaining_seconds = idle_lifetime_seconds;
    dummy.alive = true;
    state_.fire_storm_dummies.push_back(dummy);
    fire_storm_dummy_lightning_seconds_remaining_[dummy.id] = 0.0f;
    fire_storm_dummy_lightning_cooldown_seconds_remaining_[dummy.id] = 0.0f;
}

void GameApp::CheckSpellPatterns(const RunePlacedEvent& event) {
    struct PendingSpellMatch {
        std::string spell_name;
        SpellDirection direction = SpellDirection::Right;
        GridCoord cast_origin;
        std::vector<GridCoord> matched_cells;
        std::vector<int> matched_fire_storm_rune_ids;
        std::vector<int> matched_fire_storm_dummy_ids;
        bool static_fire_bolt = false;
    };

    const auto& patterns = spell_patterns_.GetPatterns();
    std::vector<PendingSpellMatch> pending_matches;
    for (const auto& pattern : patterns) {
        for (const auto& directional : pattern.directional_patterns) {
            for (const auto& variant : directional.variants) {
                const int rows = static_cast<int>(variant.size());
                if (rows <= 0) {
                    continue;
                }
                const int cols = static_cast<int>(variant[0].size());
                if (cols <= 0) {
                    continue;
                }

                std::vector<GridCoord> anchor_cells;
                for (int y = 0; y < rows; ++y) {
                    for (int x = 0; x < cols; ++x) {
                        const auto info = spell_patterns_.GetSymbolInfo(variant[y][x]);
                        if (!info.has_value()) {
                            continue;
                        }
                        const PlacementConstraint constraint =
                            pattern.order_relevant ? info->placement_constraint : PlacementConstraint::Any;
                        if (constraint == PlacementConstraint::Latest &&
                            info->rune_type == event.rune_type) {
                            anchor_cells.push_back({x, y});
                        }
                    }
                }
                if (anchor_cells.empty()) {
                    for (int y = 0; y < rows; ++y) {
                        for (int x = 0; x < cols; ++x) {
                            anchor_cells.push_back({x, y});
                        }
                    }
                }

                for (const GridCoord& anchor : anchor_cells) {
                    const GridCoord top_left = {event.cell.x - anchor.x, event.cell.y - anchor.y};

                    std::vector<GridCoord> matched_cells;
                    std::vector<int> matched_fire_storm_rune_ids;
                    std::vector<int> matched_fire_storm_dummy_ids;
                    bool is_match = true;
                    for (int y = 0; y < rows && is_match; ++y) {
                        for (int x = 0; x < cols && is_match; ++x) {
                            const std::string& symbol = variant[y][x];
                            if (symbol.empty()) {
                                continue;
                            }
                            const auto info = spell_patterns_.GetSymbolInfo(symbol);
                            if (!info.has_value()) {
                                is_match = false;
                                break;
                            }

                            const GridCoord cell = {top_left.x + x, top_left.y + y};
                            if (!state_.map.IsInside(cell)) {
                                is_match = false;
                                break;
                            }

                            bool cell_matched = false;
                            if (info->rune_type == RuneType::FireStormDummy) {
                                const auto storm_rune_it =
                                    std::find_if(state_.runes.begin(), state_.runes.end(),
                                                 [&](const Rune& rune) {
                                                     return rune.cell == cell &&
                                                            rune.rune_type == RuneType::FireStormDummy &&
                                                            rune.fire_storm_visual_state != FireStormRuneVisualState::Dying &&
                                                            (rune.active || rune.fire_storm_visual_state == FireStormRuneVisualState::Born);
                                                 });
                                if (storm_rune_it != state_.runes.end()) {
                                    cell_matched = true;
                                    matched_fire_storm_rune_ids.push_back(storm_rune_it->id);
                                } else {
                                    const auto dummy_it =
                                        std::find_if(state_.fire_storm_dummies.begin(), state_.fire_storm_dummies.end(),
                                                     [&](const FireStormDummy& dummy) {
                                                         return dummy.alive &&
                                                                dummy.state != FireStormDummyState::Dying &&
                                                                dummy.cell == cell;
                                                     });
                                    if (dummy_it != state_.fire_storm_dummies.end()) {
                                        cell_matched = true;
                                        matched_fire_storm_dummy_ids.push_back(dummy_it->id);
                                    }
                                }
                            } else {
                                const auto rune_it = std::find_if(state_.runes.begin(), state_.runes.end(),
                                                                  [&](const Rune& rune) {
                                                                      if (!rune.active || !(rune.cell == cell) ||
                                                                          rune.rune_type != info->rune_type ||
                                                                          rune.castle_charging) {
                                                                          return false;
                                                                      }

                                                                      const PlacementConstraint constraint =
                                                                          pattern.order_relevant
                                                                              ? info->placement_constraint
                                                                              : PlacementConstraint::Any;
                                                                      if (constraint == PlacementConstraint::Latest) {
                                                                          return rune.placement_order ==
                                                                                 event.placement_order;
                                                                      }
                                                                      if (constraint == PlacementConstraint::Old) {
                                                                          return rune.placement_order <
                                                                                 event.placement_order;
                                                                      }
                                                                      return true;
                                                                  });
                                cell_matched = rune_it != state_.runes.end();
                            }

                            if (!cell_matched) {
                                is_match = false;
                                break;
                            }
                            matched_cells.push_back(cell);
                        }
                    }

                    if (!is_match) {
                        continue;
                    }

                    std::sort(matched_cells.begin(), matched_cells.end(),
                              [](const GridCoord& a, const GridCoord& b) {
                                  return (a.y == b.y) ? (a.x < b.x) : (a.y < b.y);
                              });

                    GridCoord cast_origin = event.cell;
                    const bool static_fire_bolt = !matched_fire_storm_rune_ids.empty() || !matched_fire_storm_dummy_ids.empty();
                    if (pattern.spell_name == "fire_storm" || pattern.spell_name == "fire_flower") {
                        for (int y = 0; y < rows; ++y) {
                            for (int x = 0; x < cols; ++x) {
                                const auto info = spell_patterns_.GetSymbolInfo(variant[y][x]);
                                if (info.has_value() && info->rune_type == RuneType::Catalyst) {
                                    cast_origin = {top_left.x + x, top_left.y + y};
                                    break;
                                }
                            }
                        }
                    }
                    if (pattern.spell_name == "ice_wall" && !matched_cells.empty()) {
                        std::vector<GridCoord> ordered = matched_cells;
                        if (IsHorizontalDirection(directional.direction)) {
                            std::sort(ordered.begin(), ordered.end(),
                                      [](const GridCoord& a, const GridCoord& b) {
                                          return (a.x == b.x) ? (a.y < b.y) : (a.x < b.x);
                                      });
                        } else {
                            std::sort(ordered.begin(), ordered.end(),
                                      [](const GridCoord& a, const GridCoord& b) {
                                          return (a.y == b.y) ? (a.x < b.x) : (a.y < b.y);
                                      });
                        }
                        cast_origin = ordered[ordered.size() / 2];
                    }

                    const bool duplicate = std::any_of(
                        pending_matches.begin(), pending_matches.end(),
                        [&](const PendingSpellMatch& match) {
                            return match.spell_name == pattern.spell_name && match.direction == directional.direction &&
                                   match.cast_origin == cast_origin && match.matched_cells == matched_cells &&
                                   match.matched_fire_storm_rune_ids == matched_fire_storm_rune_ids &&
                                   match.matched_fire_storm_dummy_ids == matched_fire_storm_dummy_ids &&
                                   match.static_fire_bolt == static_fire_bolt;
                        });
                    if (!duplicate) {
                        pending_matches.push_back(PendingSpellMatch{pattern.spell_name,
                                                                    directional.direction,
                                                                    cast_origin,
                                                                    matched_cells,
                                                                    matched_fire_storm_rune_ids,
                                                                    matched_fire_storm_dummy_ids,
                                                                    static_fire_bolt});
                    }
                }
            }
        }
    }

    if (pending_matches.empty()) {
        return;
    }

    std::vector<GridCoord> consumed_cells;
    std::vector<int> consumed_fire_storm_rune_ids;
    std::vector<int> consumed_fire_storm_dummy_ids;
    for (const auto& match : pending_matches) {
        event_queue_.Push(RunePatternCompletedEvent{match.spell_name, match.direction, match.cast_origin, match.matched_cells});

        const SpellRuntimeMatch runtime_match = {
            match.spell_name,
            event.player_id,
            event.team,
            match.direction,
            match.cast_origin,
            match.matched_cells,
            match.static_fire_bolt,
        };

        const bool cast_succeeded = CastRuntimeSpell(runtime_match);
        if (cast_succeeded && match.spell_name == "fire_bolt") {
            PlaySfxIfVisible(sfx_fireball_created_.sound, sfx_fireball_created_.loaded, CellToWorldCenter(match.cast_origin));
        } else if (cast_succeeded && match.spell_name == "ice_wave") {
            std::vector<size_t> loaded_indices;
            loaded_indices.reserve(sfx_ice_wave_cast_.size());
            for (size_t i = 0; i < sfx_ice_wave_cast_.size(); ++i) {
                if (sfx_ice_wave_cast_[i].loaded) {
                    loaded_indices.push_back(i);
                }
            }
            if (!loaded_indices.empty()) {
                std::uniform_int_distribution<size_t> dist(0, loaded_indices.size() - 1);
                const LoadedSfx& clip = sfx_ice_wave_cast_[loaded_indices[dist(rng_)]];
                PlaySfxIfVisible(clip.sound, clip.loaded, CellToWorldCenter(match.cast_origin));
            }
        } else if (cast_succeeded && match.spell_name == "ice_wall") {
            PlaySfxIfVisible(sfx_ice_wall_freeze_.sound, sfx_ice_wall_freeze_.loaded, CellToWorldCenter(match.cast_origin));
        } else if (cast_succeeded && match.spell_name == "fire_storm") {
            PlaySfxIfVisible(sfx_fire_storm_cast_.sound, sfx_fire_storm_cast_.loaded, CellToWorldCenter(match.cast_origin));
        } else if (cast_succeeded && match.spell_name == "fire_flower") {
            PlaySfxIfVisible(sfx_earth_rune_launch_.sound, sfx_earth_rune_launch_.loaded, CellToWorldCenter(match.cast_origin));
        }

        for (const auto& matched_cell : match.matched_cells) {
            const bool already_consumed =
                std::any_of(consumed_cells.begin(), consumed_cells.end(), [&](const GridCoord& cell) { return cell == matched_cell; });
            if (!already_consumed) {
                consumed_cells.push_back(matched_cell);
            }
        }
        for (int dummy_id : match.matched_fire_storm_dummy_ids) {
            const bool already_consumed =
                std::find(consumed_fire_storm_dummy_ids.begin(), consumed_fire_storm_dummy_ids.end(), dummy_id) !=
                consumed_fire_storm_dummy_ids.end();
            if (!already_consumed) {
                consumed_fire_storm_dummy_ids.push_back(dummy_id);
            }
        }
        for (int rune_id : match.matched_fire_storm_rune_ids) {
            const bool already_consumed =
                std::find(consumed_fire_storm_rune_ids.begin(), consumed_fire_storm_rune_ids.end(), rune_id) !=
                consumed_fire_storm_rune_ids.end();
            if (!already_consumed) {
                consumed_fire_storm_rune_ids.push_back(rune_id);
            }
        }
    }

    for (auto& rune : state_.runes) {
        if (std::find(consumed_fire_storm_rune_ids.begin(), consumed_fire_storm_rune_ids.end(), rune.id) !=
            consumed_fire_storm_rune_ids.end()) {
            BeginFireStormRuneRevert(rune, false);
            continue;
        }
        const bool should_consume =
            rune.active && std::any_of(consumed_cells.begin(), consumed_cells.end(),
                                       [&](const GridCoord& cell) { return rune.cell == cell; });
        if (!should_consume) {
            continue;
        }
        rune.active = false;
        rune.activation_total_seconds = 0.0f;
        rune.activation_remaining_seconds = 0.0f;
    }

    for (auto& dummy : state_.fire_storm_dummies) {
        const bool should_consume =
            dummy.alive &&
            std::find(consumed_fire_storm_dummy_ids.begin(), consumed_fire_storm_dummy_ids.end(), dummy.id) !=
                consumed_fire_storm_dummy_ids.end();
        if (!should_consume) {
            continue;
        }
        dummy.state = FireStormDummyState::Dying;
        dummy.state_time = 0.0f;
        dummy.state_duration = GetCompositeEffectDurationSeconds("fire_storm_death");
        fire_storm_dummy_lightning_seconds_remaining_[dummy.id] = 0.0f;
    }
}

bool GameApp::IsTileRunePlaceable(const GridCoord& cell) const {
    if (!state_.map.IsInside(cell)) {
        return false;
    }

    const TileType tile = state_.map.GetTile(cell);
    if (!(tile == TileType::Grass || tile == TileType::SpawnPoint || tile == TileType::StoneTiles)) {
        return false;
    }

    for (const auto& object : state_.map_objects) {
        const ObjectPrototype* proto = FindObjectPrototype(object.prototype_id);
        if (BlocksRunePlacementAtCell(object, cell, proto)) {
            return false;
        }
    }
    return true;
}

bool GameApp::IsCellOccupiedByRune(const GridCoord& cell) const {
    return std::any_of(state_.runes.begin(), state_.runes.end(), [&](const Rune& rune) { return rune.cell == cell; });
}

bool GameApp::IsCellOccupiedByFireStormDummy(const GridCoord& cell) const {
    return std::any_of(state_.fire_storm_dummies.begin(), state_.fire_storm_dummies.end(),
                       [&](const FireStormDummy& dummy) { return dummy.alive && dummy.cell == cell; });
}

GridCoord GameApp::WorldToCell(Vector2 world) const {
    return {static_cast<int>(std::floor(world.x / state_.map.cell_size)),
            static_cast<int>(std::floor(world.y / state_.map.cell_size))};
}

Vector2 GameApp::CellToWorldCenter(const GridCoord& cell) const { return state_.map.CellCenterWorld(cell); }

Rectangle GameApp::GetCameraWorldCullRect(float padding_world) const {
    const Vector2 top_left =
        GetScreenToWorld2D({-padding_world * camera_.zoom, -padding_world * camera_.zoom}, camera_);
    const Vector2 bottom_right = GetScreenToWorld2D(
        {static_cast<float>(GetScreenWidth()) + padding_world * camera_.zoom,
         static_cast<float>(GetScreenHeight()) + padding_world * camera_.zoom},
        camera_);
    return {std::min(top_left.x, bottom_right.x), std::min(top_left.y, bottom_right.y),
            std::fabs(bottom_right.x - top_left.x), std::fabs(bottom_right.y - top_left.y)};
}

void GameApp::GetVisibleCellBounds(int padding_cells, int* out_min_x, int* out_min_y, int* out_max_x,
                                   int* out_max_y) const {
    if (out_min_x == nullptr || out_min_y == nullptr || out_max_x == nullptr || out_max_y == nullptr ||
        state_.map.width <= 0 || state_.map.height <= 0 || state_.map.cell_size <= 0) {
        return;
    }
    const float padding_world = static_cast<float>(std::max(0, padding_cells * state_.map.cell_size));
    const Rectangle world_rect = GetCameraWorldCullRect(padding_world);
    *out_min_x = std::clamp(static_cast<int>(std::floor(world_rect.x / static_cast<float>(state_.map.cell_size))), 0,
                            state_.map.width - 1);
    *out_min_y = std::clamp(static_cast<int>(std::floor(world_rect.y / static_cast<float>(state_.map.cell_size))), 0,
                            state_.map.height - 1);
    *out_max_x = std::clamp(
        static_cast<int>(std::floor((world_rect.x + world_rect.width) / static_cast<float>(state_.map.cell_size))), 0,
        state_.map.width - 1);
    *out_max_y = std::clamp(
        static_cast<int>(std::floor((world_rect.y + world_rect.height) / static_cast<float>(state_.map.cell_size))), 0,
        state_.map.height - 1);
}

bool GameApp::IsWorldRectVisible(const Rectangle& world_rect, float padding_world) const {
    const Rectangle view = GetCameraWorldCullRect(padding_world);
    return world_rect.x + world_rect.width >= view.x && world_rect.x <= view.x + view.width &&
           world_rect.y + world_rect.height >= view.y && world_rect.y <= view.y + view.height;
}

bool GameApp::IsCastleEquippableRune(RuneType rune_type) const {
    return rune_type == RuneType::Fire || rune_type == RuneType::Water || rune_type == RuneType::Earth;
}

int GameApp::GetRuneRequiredCastleLevel(RuneType rune_type) const {
    switch (rune_type) {
        case RuneType::Fire:
        case RuneType::Water:
            return 1;
        case RuneType::Earth:
            return 2;
        case RuneType::Catalyst:
        case RuneType::FireStormDummy:
        case RuneType::None:
            return 0;
    }
    return 0;
}

int GameApp::GetCastleLoadoutCapacity(int castle_level) const {
    if (castle_level <= 1) {
        return 1;
    }
    if (castle_level == 2) {
        return 2;
    }
    return 3;
}

float GameApp::GetCastleEnergyRequirementForLevel(int castle_level) const {
    if (castle_level <= 0) {
        return 0.0f;
    }
    if (castle_level_energy_requirements_.empty()) {
        return Constants::kCastleChargeEnergyPerRune;
    }
    const size_t index = static_cast<size_t>(std::max(0, castle_level - 1));
    if (index < castle_level_energy_requirements_.size()) {
        return castle_level_energy_requirements_[index];
    }
    return castle_level_energy_requirements_.back();
}

int GameApp::ComputeCastleLevel(float total_energy, float* out_energy_into_current_level,
                                float* out_energy_needed_for_next_level) const {
    float remaining_energy = std::max(0.0f, total_energy);
    int level = 1;
    float requirement = std::max(0.001f, GetCastleEnergyRequirementForLevel(level));
    while (remaining_energy + 0.0001f >= requirement) {
        remaining_energy -= requirement;
        ++level;
        requirement = std::max(0.001f, GetCastleEnergyRequirementForLevel(level));
    }
    if (out_energy_into_current_level != nullptr) {
        *out_energy_into_current_level = remaining_energy;
    }
    if (out_energy_needed_for_next_level != nullptr) {
        *out_energy_needed_for_next_level = requirement;
    }
    return std::max(1, level);
}

bool GameApp::IsRuneAvailableToPlayer(const Player& player, RuneType rune_type) const {
    if (rune_type == RuneType::None) {
        return false;
    }
    if (rune_type == RuneType::Catalyst || rune_type == RuneType::FireStormDummy) {
        return true;
    }
    return std::find(player.rune_slots.begin(), player.rune_slots.end(), rune_type) != player.rune_slots.end();
}

const CastleState* GameApp::FindCastleById(int id) const {
    auto it = std::find_if(state_.castles.begin(), state_.castles.end(),
                           [&](const CastleState& castle) { return castle.id == id; });
    return it == state_.castles.end() ? nullptr : &(*it);
}

CastleState* GameApp::FindCastleById(int id) {
    auto it = std::find_if(state_.castles.begin(), state_.castles.end(),
                           [&](const CastleState& castle) { return castle.id == id; });
    return it == state_.castles.end() ? nullptr : &(*it);
}

const CastleState* GameApp::FindCastleByTeam(int team) const {
    auto it = std::find_if(state_.castles.begin(), state_.castles.end(),
                           [&](const CastleState& castle) { return castle.team == team; });
    return it == state_.castles.end() ? nullptr : &(*it);
}

CastleState* GameApp::FindCastleByTeam(int team) {
    auto it = std::find_if(state_.castles.begin(), state_.castles.end(),
                           [&](const CastleState& castle) { return castle.team == team; });
    return it == state_.castles.end() ? nullptr : &(*it);
}

const CastleState* GameApp::GetAlliedCastleForPlayer(const Player& player) const {
    return FindCastleByTeam(player.team);
}

bool GameApp::IsPlayerWithinCastleRange(const Player& player, const CastleState& castle) const {
    const float range_world = Constants::kCastleInteractionRangeTiles * static_cast<float>(state_.map.cell_size);
    return Vector2Distance(player.pos, CellToWorldCenter(castle.cell)) <= range_world;
}

Vector2 GameApp::GetCastleChargePortWorld(const CastleState& castle) const {
    Vector2 port = CellToWorldCenter(castle.cell);
    port.x += static_cast<float>(castle.charge_port_offset_x);
    port.y += static_cast<float>(castle.charge_port_offset_y);
    return port;
}

void GameApp::RefreshCastleDerivedState(CastleState& castle) {
    castle.total_energy = std::max(0.0f, castle.total_energy);
    castle.level = ComputeCastleLevel(castle.total_energy, &castle.energy_into_current_level,
                                      &castle.energy_needed_for_next_level);
}

void GameApp::NormalizePlayerRuneLoadout(Player& player) const {
    std::array<RuneType, 4> normalized = {RuneType::None, RuneType::None, RuneType::Catalyst, RuneType::None};
    int insert_index = 0;
    for (RuneType rune_type : player.rune_slots) {
        if (!IsCastleEquippableRune(rune_type)) {
            continue;
        }
        while (insert_index == 2) {
            ++insert_index;
        }
        if (insert_index >= static_cast<int>(normalized.size())) {
            break;
        }
        normalized[static_cast<size_t>(insert_index)] = rune_type;
        ++insert_index;
    }
    player.rune_slots = normalized;
    if (player.selected_rune_slot < 0 || player.selected_rune_slot >= static_cast<int>(player.rune_slots.size())) {
        player.selected_rune_slot = 2;
    }
    if (!IsCastleManagedRuneSlot(player.selected_rune_slot) &&
        player.rune_slots[static_cast<size_t>(player.selected_rune_slot)] != RuneType::Catalyst) {
        player.selected_rune_slot = 2;
    }
    player.selected_rune_type = player.rune_slots[static_cast<size_t>(player.selected_rune_slot)];
    player.rune_placing_mode = !player.inventory_mode && player.selected_rune_type != RuneType::None &&
                               GetPlayerRuneCooldownRemaining(player, player.selected_rune_type) <= 0.0f &&
                               player.mana >= GetRuneManaCost(player.selected_rune_type);
}

bool GameApp::EquipRuneToCastleLoadout(Player& player, RuneType rune_type, int castle_level) {
    if (!IsCastleEquippableRune(rune_type) || castle_level < GetRuneRequiredCastleLevel(rune_type)) {
        return false;
    }
    if (std::find(player.rune_slots.begin(), player.rune_slots.end(), rune_type) != player.rune_slots.end()) {
        return false;
    }
    int equipped_count = 0;
    for (RuneType slot_rune : player.rune_slots) {
        if (IsCastleEquippableRune(slot_rune)) {
            ++equipped_count;
        }
    }
    if (equipped_count >= GetCastleLoadoutCapacity(castle_level)) {
        return false;
    }
    for (int slot_index : {0, 1, 3}) {
        if (player.rune_slots[static_cast<size_t>(slot_index)] == RuneType::None) {
            player.rune_slots[static_cast<size_t>(slot_index)] = rune_type;
            NormalizePlayerRuneLoadout(player);
            return true;
        }
    }
    return false;
}

bool GameApp::UnequipRuneFromCastleLoadout(Player& player, RuneType rune_type) {
    bool removed = false;
    for (RuneType& slot_rune : player.rune_slots) {
        if (slot_rune == rune_type && IsCastleEquippableRune(slot_rune)) {
            slot_rune = RuneType::None;
            removed = true;
        }
    }
    if (removed) {
        NormalizePlayerRuneLoadout(player);
    }
    return removed;
}

bool GameApp::IsCastleManagedRuneSlot(int slot_index) const {
    return slot_index >= 0 && slot_index < Constants::kHudRuneSlotCount && slot_index != 2;
}

bool GameApp::IsCastleCharging(int castle_id) const {
    return std::any_of(state_.runes.begin(), state_.runes.end(), [&](const Rune& rune) {
        return rune.active && rune.castle_charging && rune.castle_id == castle_id;
    });
}

void GameApp::OpenLocalInventoryUiForCurrentContext() {
    Player* local_player = FindPlayerById(state_.local_player_id);
    if (local_player == nullptr) {
        return;
    }
    local_player->inventory_mode = true;
    local_player->rune_placing_mode = false;
    if (local_player->action_state == PlayerActionState::RunePlacing) {
        local_player->action_state = PlayerActionState::Idle;
    }
    const CastleState* allied_castle = GetAlliedCastleForPlayer(*local_player);
    local_inventory_ui_mode_ =
        (allied_castle != nullptr && IsPlayerWithinCastleRange(*local_player, *allied_castle))
            ? InventoryUiMode::CastleLoadout
            : InventoryUiMode::Inventory;
}

void GameApp::CloseLocalInventoryUi() {
    if (Player* local_player = FindPlayerById(state_.local_player_id)) {
        local_player->inventory_mode = false;
        CancelInventoryDrag(*local_player, true);
    }
    local_inventory_ui_mode_ = InventoryUiMode::Closed;
}

void GameApp::InitializeCastlesFromSpawnPoints() {
    state_.castles.clear();
    std::vector<GridCoord> castle_cells = state_.map.spawn_points;
    if (castle_cells.size() < 2) {
        castle_cells = {{2, 2}, {state_.map.width - 3, state_.map.height - 3}};
    } else if (castle_cells.size() > 2) {
        castle_cells.resize(2);
    }
    state_.map.spawn_points = castle_cells;

    for (size_t i = 0; i < castle_cells.size(); ++i) {
        const int team = (i == 0) ? Constants::kTeamBlue : Constants::kTeamRed;
        const int object_id = SpawnObjectInstanceAtCell("castle", castle_cells[i]);
        MapObjectInstance* object = FindMapObjectById(object_id);
        if (object != nullptr) {
            object->owner_team = team;
        }
        CastleState castle;
        castle.id = state_.next_entity_id++;
        castle.team = team;
        castle.cell = castle_cells[i];
        castle.map_object_id = object_id;
        if (const ObjectPrototype* proto = FindObjectPrototype("castle")) {
            castle.charge_port_offset_x = proto->charge_port_offset_x;
            castle.charge_port_offset_y = proto->charge_port_offset_y;
        }
        RefreshCastleDerivedState(castle);
        state_.castles.push_back(castle);
    }
}

Player* GameApp::FindPlayerById(int id) {
    auto it = std::find_if(state_.players.begin(), state_.players.end(),
                           [&](const Player& player) { return player.id == id; });
    return it == state_.players.end() ? nullptr : &(*it);
}

const Player* GameApp::FindPlayerById(int id) const {
    auto it = std::find_if(state_.players.begin(), state_.players.end(),
                           [&](const Player& player) { return player.id == id; });
    return it == state_.players.end() ? nullptr : &(*it);
}

FireSpirit* GameApp::FindFireSpiritById(int id) {
    auto it = std::find_if(state_.fire_spirits.begin(), state_.fire_spirits.end(),
                           [&](const FireSpirit& spirit) { return spirit.id == id; });
    return it == state_.fire_spirits.end() ? nullptr : &(*it);
}

const FireSpirit* GameApp::FindFireSpiritById(int id) const {
    auto it = std::find_if(state_.fire_spirits.begin(), state_.fire_spirits.end(),
                           [&](const FireSpirit& spirit) { return spirit.id == id; });
    return it == state_.fire_spirits.end() ? nullptr : &(*it);
}

void GameApp::RenderWorld() {
    UpdateCameraTarget();
    EnsureMapObjectRenderCaches();
    UpdateObjectShadowLayer();
    EnsureWorldLayerRenderTarget();
    if (Constants::kFireSpiritTrailEnableLowResRender) {
        EnsureFireSpiritTrailRenderTarget();
        RenderFireSpiritTrailTarget();
    }
    if (!has_world_layer_target_) {
        return;
    }

    BeginTextureMode(world_layer_target_);
    ClearBackground(BLANK);

    BeginMode2D(camera_);
    RenderMap();
    EndMode2D();

    BeginMode2D(camera_);
    RenderMapForeground();
    EndMode2D();

    BeginMode2D(camera_);
    RenderGroundMapObjects();
    EndMode2D();

    DrawObjectShadowLayer();

    BeginMode2D(camera_);
    RenderNonTerrainDepthSorted(DepthSortedRenderPass::UnderInfluenceOverlay);
    RenderInfluenceZoneOverlay();
    RenderInfluenceZoneAnimatedTiles();
    EndMode2D();

    BeginMode2D(camera_);
    RenderNonTerrainDepthSorted(DepthSortedRenderPass::OverInfluenceOverlay);
    RenderTopLayerParticles();
    RenderLocalGrapplingPreview();
    RenderMeleeAttacks();
    RenderDamagePopups();
    EndMode2D();

    if (Constants::kFireSpiritTrailEnableLowResRender) {
        DrawFireSpiritTrailLayer();
    }

    EndTextureMode();
    DrawWorldLayerWithZonePostProcess();
    RenderZoneFillOverlay();
    RenderZoneBorderOverlay();
    RenderMapBoundsFadeOverlay();

    BeginMode2D(camera_);
    RenderRunePlacementOverlay();
    RenderPlayerOverlays();
    RenderDebugCollisionOverlay();
    EndMode2D();
}

void GameApp::EnsureFireSpiritTrailRenderTarget() {
    if (!Constants::kFireSpiritTrailEnableLowResRender) {
        if (has_fire_spirit_trail_target_) {
            UnloadRenderTexture(fire_spirit_trail_target_);
            fire_spirit_trail_target_ = {};
            has_fire_spirit_trail_target_ = false;
        }
        if (has_fire_spirit_trail_lowres_target_) {
            UnloadRenderTexture(fire_spirit_trail_lowres_target_);
            fire_spirit_trail_lowres_target_ = {};
            has_fire_spirit_trail_lowres_target_ = false;
        }
        return;
    }

    const int scale = std::max(1, Constants::kFireSpiritTrailRenderScale);
    const int full_width = std::max(1, GetScreenWidth());
    const int full_height = std::max(1, GetScreenHeight());
    const int lowres_width = std::max(1, GetScreenWidth() / scale);
    const int lowres_height = std::max(1, GetScreenHeight() / scale);
    const bool full_size_ok = has_fire_spirit_trail_target_ && fire_spirit_trail_target_.texture.width == full_width &&
                              fire_spirit_trail_target_.texture.height == full_height;
    const bool lowres_size_ok = has_fire_spirit_trail_lowres_target_ &&
                                fire_spirit_trail_lowres_target_.texture.width == lowres_width &&
                                fire_spirit_trail_lowres_target_.texture.height == lowres_height;
    if (full_size_ok && lowres_size_ok) {
        return;
    }

    if (has_fire_spirit_trail_target_) {
        UnloadRenderTexture(fire_spirit_trail_target_);
        fire_spirit_trail_target_ = {};
        has_fire_spirit_trail_target_ = false;
    }
    if (has_fire_spirit_trail_lowres_target_) {
        UnloadRenderTexture(fire_spirit_trail_lowres_target_);
        fire_spirit_trail_lowres_target_ = {};
        has_fire_spirit_trail_lowres_target_ = false;
    }

    fire_spirit_trail_target_ = LoadRenderTexture(full_width, full_height);
    has_fire_spirit_trail_target_ = (fire_spirit_trail_target_.id != 0);
    if (has_fire_spirit_trail_target_) {
        SetTextureFilter(fire_spirit_trail_target_.texture, TEXTURE_FILTER_BILINEAR);
    }

    fire_spirit_trail_lowres_target_ = LoadRenderTexture(lowres_width, lowres_height);
    has_fire_spirit_trail_lowres_target_ = (fire_spirit_trail_lowres_target_.id != 0);
    if (has_fire_spirit_trail_lowres_target_) {
        // The final upscale should stay blocky after the downsample pass.
        SetTextureFilter(fire_spirit_trail_lowres_target_.texture, TEXTURE_FILTER_POINT);
    }
}

void GameApp::RenderFireSpiritTrailGeometry(bool debug_trail_geometry) {
    if (!sprite_metadata_.IsLoaded()) {
        return;
    }

    const Texture2D texture = sprite_metadata_.GetTexture();
    const auto shared_trail_frame_time = [&]() {
        switch (Constants::kFireSpiritTrailAnimationMode) {
            case Constants::TrailAnimationMode::Static:
            case Constants::TrailAnimationMode::WidthDriven:
                return 0.0f;
            case Constants::TrailAnimationMode::PerSegmentAnimated:
            case Constants::TrailAnimationMode::GlobalAnimated:
            default:
                return render_time_seconds_;
        }
    }();
    Rectangle src = {};
    float u0 = 0.0f;
    float v_top = 0.0f;
    float u1 = 0.0f;
    float v_bottom = 0.0f;
    if (!debug_trail_geometry) {
        if (!sprite_metadata_.HasAnimation("fire_trail")) {
            return;
        }
        src = sprite_metadata_.GetFrame("fire_trail", "default", shared_trail_frame_time);
        u0 = src.x / static_cast<float>(texture.width);
        v_top = src.y / static_cast<float>(texture.height);
        u1 = (src.x + src.width) / static_cast<float>(texture.width);
        v_bottom = (src.y + src.height) / static_cast<float>(texture.height);
        rlDisableBackfaceCulling();
        rlSetTexture(texture.id);
        rlBegin(RL_TRIANGLES);
    } else {
        rlDisableBackfaceCulling();
    }
    const auto emit_debug_triangle = [&](Vector2 p0, Vector2 p1, Vector2 p2, Color color) {
        DrawTriangle(p0, p1, p2, color);
    };
    const auto emit_textured_triangle = [&](Vector2 p0, float tu0, float tv0, Vector2 p1, float tu1, float tv1,
                                            Vector2 p2, float tu2, float tv2) {
        rlTexCoord2f(tu0, tv0);
        rlVertex2f(p0.x, p0.y);
        rlTexCoord2f(tu1, tv1);
        rlVertex2f(p1.x, p1.y);
        rlTexCoord2f(tu2, tv2);
        rlVertex2f(p2.x, p2.y);
    };
    const auto sample_width = [&](const FireSpiritTrailSample& sample) {
        return Constants::kFireSpiritTrailHalfWidth *
               std::clamp(1.0f - sample.age_seconds / Constants::kFireSpiritTrailLifetimeSeconds, 0.0f, 1.0f);
    };
    int trail_frame_count = 0;
    float trail_fps_unused = 0.0f;
    const bool has_trail_frame_stats =
        sprite_metadata_.GetAnimationStats("fire_trail", "default", trail_frame_count, trail_fps_unused) &&
        trail_frame_count > 0;
    const auto compute_trail_direction = [&](const FireSpirit& spirit, size_t sample_index) {
        const Vector2 sample_world = spirit.trail_samples[sample_index].world;
        Vector2 tangent = {0.0f, 0.0f};
        if (sample_index > 0) {
            const Vector2 prev = Vector2Subtract(sample_world, spirit.trail_samples[sample_index - 1].world);
            if (Vector2LengthSqr(prev) > 0.0001f) {
                tangent = Vector2Add(tangent, Vector2Normalize(prev));
            }
        }
        if (sample_index + 1 < spirit.trail_samples.size()) {
            const Vector2 next = Vector2Subtract(spirit.trail_samples[sample_index + 1].world, sample_world);
            if (Vector2LengthSqr(next) > 0.0001f) {
                tangent = Vector2Add(tangent, Vector2Normalize(next));
            }
        }
        if (Vector2LengthSqr(tangent) <= 0.0001f) {
            if (sample_index > 0) {
                tangent = Vector2Subtract(sample_world, spirit.trail_samples[sample_index - 1].world);
            } else if (sample_index + 1 < spirit.trail_samples.size()) {
                tangent = Vector2Subtract(spirit.trail_samples[sample_index + 1].world, sample_world);
            }
        }
        if (Vector2LengthSqr(tangent) <= 0.0001f) {
            return Vector2{1.0f, 0.0f};
        }
        return Vector2Normalize(tangent);
    };

    for (const FireSpirit& spirit : state_.fire_spirits) {
        const bool render_dead_trail =
            Constants::kFireSpiritTrailEnableDeadLinger && spirit.state == FireSpiritState::Dead &&
            !spirit.trail_samples.empty();
        if (!spirit.alive || spirit.trail_samples.size() < 2 ||
            (spirit.state != FireSpiritState::Projectile && !render_dead_trail)) {
            continue;
        }

        std::vector<Vector2> trail_normals;
        trail_normals.reserve(spirit.trail_samples.size());
        for (size_t sample_index = 0; sample_index < spirit.trail_samples.size(); ++sample_index) {
            const Vector2 tangent = compute_trail_direction(spirit, sample_index);
            Vector2 normal = {-tangent.y, tangent.x};
            if (!trail_normals.empty() && Vector2DotProduct(normal, trail_normals.back()) < 0.0f) {
                normal = Vector2Negate(normal);
            }
            trail_normals.push_back(normal);
        }

        for (size_t j = 1; j < spirit.trail_samples.size(); ++j) {
            const FireSpiritTrailSample& a = spirit.trail_samples[j - 1];
            const FireSpiritTrailSample& b = spirit.trail_samples[j];
            float width_a = sample_width(a);
            float width_b = sample_width(b);
            if (width_a <= 0.01f && width_b <= 0.01f) {
                continue;
            }

            const Vector2 v0 = Vector2Add(a.world, Vector2Scale(trail_normals[j - 1], width_a));
            const Vector2 v1 = Vector2Subtract(a.world, Vector2Scale(trail_normals[j - 1], width_a));
            const Vector2 v2 = Vector2Subtract(b.world, Vector2Scale(trail_normals[j], width_b));
            const Vector2 v3 = Vector2Add(b.world, Vector2Scale(trail_normals[j], width_b));

            if (debug_trail_geometry) {
                const Color fill = (spirit.owner_team == 1) ? Color{255, 96, 48, 255} : Color{255, 220, 80, 255};
                const Color outline = fill;
                emit_debug_triangle(v3, v2, v1, fill);
                emit_debug_triangle(v3, v1, v0, fill);
                DrawLineEx(v0, v1, 1.0f, outline);
                DrawLineEx(v1, v2, 1.0f, outline);
                DrawLineEx(v2, v3, 1.0f, outline);
                DrawLineEx(v3, v0, 1.0f, outline);
            } else {
                float segment_u0 = u0;
                float segment_v_top = v_top;
                float segment_u1 = u1;
                float segment_v_bottom = v_bottom;
                if (Constants::kFireSpiritTrailAnimationMode == Constants::TrailAnimationMode::PerSegmentAnimated ||
                    Constants::kFireSpiritTrailAnimationMode == Constants::TrailAnimationMode::WidthDriven) {
                    Rectangle segment_src = {};
                    if (Constants::kFireSpiritTrailAnimationMode == Constants::TrailAnimationMode::PerSegmentAnimated) {
                        segment_src = sprite_metadata_.GetFrame("fire_trail", "default", b.age_seconds);
                    } else if (has_trail_frame_stats) {
                        const float average_width_ratio =
                            std::clamp((width_a + width_b) / (2.0f * Constants::kFireSpiritTrailHalfWidth), 0.0f, 1.0f);
                        const float progress = 1.0f - average_width_ratio;
                        const int frame_index = std::clamp(
                            static_cast<int>(progress * static_cast<float>(trail_frame_count)), 0, trail_frame_count - 1);
                        segment_src = sprite_metadata_.GetFrameByIndex("fire_trail", "default", frame_index);
                    } else {
                        segment_src = sprite_metadata_.GetFrame("fire_trail", "default", 0.0f);
                    }
                    segment_u0 = segment_src.x / static_cast<float>(texture.width);
                    segment_v_top = segment_src.y / static_cast<float>(texture.height);
                    segment_u1 = (segment_src.x + segment_src.width) / static_cast<float>(texture.width);
                    segment_v_bottom = (segment_src.y + segment_src.height) / static_cast<float>(texture.height);
                }
                rlColor4ub(255, 255, 255, 255);
                emit_textured_triangle(v3, segment_u0, segment_v_top, v2, segment_u0, segment_v_bottom, v1,
                                       segment_u1, segment_v_bottom);
                emit_textured_triangle(v3, segment_u0, segment_v_top, v1, segment_u1, segment_v_bottom, v0,
                                       segment_u1, segment_v_top);
            }
        }
    }
    if (!debug_trail_geometry) {
        rlEnd();
        rlSetTexture(0);
    }
    rlEnableBackfaceCulling();
}

void GameApp::RenderFireSpiritTrailTarget() {
    if (!Constants::kFireSpiritTrailEnableLowResRender || !has_fire_spirit_trail_target_ ||
        !has_fire_spirit_trail_lowres_target_) {
        return;
    }

    BeginTextureMode(fire_spirit_trail_target_);
    ClearBackground(BLANK);
    BeginMode2D(camera_);
    RenderFireSpiritTrailGeometry(show_network_debug_panel_);
    EndMode2D();
    EndTextureMode();

    BeginTextureMode(fire_spirit_trail_lowres_target_);
    ClearBackground(BLANK);
    const Rectangle full_src = {0.0f, 0.0f, static_cast<float>(fire_spirit_trail_target_.texture.width),
                                -static_cast<float>(fire_spirit_trail_target_.texture.height)};
    const Rectangle lowres_dst = {0.0f, 0.0f, static_cast<float>(fire_spirit_trail_lowres_target_.texture.width),
                                  static_cast<float>(fire_spirit_trail_lowres_target_.texture.height)};
    // Render textures already contain alpha-applied RGB. Composite them as premultiplied-alpha,
    // otherwise semi-transparent trail texels darken as if sampled over black on each RT hop.
    rlSetBlendFactorsSeparate(RL_ONE, RL_ONE_MINUS_SRC_ALPHA, RL_ONE, RL_ONE_MINUS_SRC_ALPHA, RL_FUNC_ADD,
                              RL_FUNC_ADD);
    BeginBlendMode(BLEND_CUSTOM_SEPARATE);
    DrawTexturePro(fire_spirit_trail_target_.texture, full_src, lowres_dst, {0.0f, 0.0f}, 0.0f, WHITE);
    EndBlendMode();
    EndTextureMode();
}

void GameApp::DrawFireSpiritTrailLayer() {
    if (!Constants::kFireSpiritTrailEnableLowResRender || !has_fire_spirit_trail_lowres_target_) {
        return;
    }

    const Rectangle src = {0.0f, 0.0f, static_cast<float>(fire_spirit_trail_lowres_target_.texture.width),
                           -static_cast<float>(fire_spirit_trail_lowres_target_.texture.height)};
    const Rectangle dst = {0.0f, 0.0f, static_cast<float>(GetScreenWidth()), static_cast<float>(GetScreenHeight())};
    rlSetBlendFactorsSeparate(RL_ONE, RL_ONE_MINUS_SRC_ALPHA, RL_ONE, RL_ONE_MINUS_SRC_ALPHA, RL_FUNC_ADD,
                              RL_FUNC_ADD);
    BeginBlendMode(BLEND_CUSTOM_SEPARATE);
    DrawTexturePro(fire_spirit_trail_lowres_target_.texture, src, dst, {0.0f, 0.0f}, 0.0f, WHITE);
    EndBlendMode();
}

void GameApp::RenderParticleInstance(const Particle& particle) {
    const bool has_texture = sprite_metadata_.IsLoaded();
    const Texture2D texture = sprite_metadata_.GetTexture();
    const Vector2 render_center = GetParticleRenderCenter(particle, GetTime());
    if (has_texture && sprite_metadata_.HasAnimation(particle.animation_key)) {
        const Rectangle src =
            InsetSourceRect(sprite_metadata_.GetFrame(particle.animation_key, particle.facing, particle.age_seconds),
                            Constants::kAtlasSampleInsetPixels);
        const float particle_size =
            particle.size > 0.0f ? particle.size : (particle.animation_key == "static_upgrade" ? 48.0f : 32.0f);
        const bool rotated_particle = std::fabs(particle.rotation_degrees) > 0.001f;
        Rectangle dst = rotated_particle
                            ? Rectangle{render_center.x, render_center.y, particle_size, particle_size}
                            : Rectangle{render_center.x - particle_size * 0.5f, render_center.y - particle_size * 0.5f,
                                        particle_size, particle_size};
        dst = SnapRect(dst);
        const Vector2 origin =
            rotated_particle ? Vector2{particle_size * 0.5f, particle_size * 0.5f} : Vector2{0.0f, 0.0f};
        DrawTexturePro(texture, src, dst, origin, particle.rotation_degrees, Color{255, 255, 255, particle.alpha});
    } else {
        DrawCircleV(render_center, 4.0f, Color{190, 190, 190, 160});
    }
}

void GameApp::RenderTopLayerParticles() {
    for (const auto& particle : state_.particles) {
        if (!particle.alive || !particle.render_on_top) {
            continue;
        }
        RenderParticleInstance(particle);
    }
}

void GameApp::RenderLocalGrapplingPreview() {
    if (!grappling_preview_armed_ || app_screen_ != AppScreen::InMatch || !IsBindingDown(controls_bindings_.grappling_hook_action) ||
        !sprite_metadata_.IsLoaded()) {
        return;
    }
    const Player* local_player = FindPlayerById(state_.local_player_id);
    if (local_player == nullptr || !CanPlayerStartGrapplingPreview(*local_player)) {
        return;
    }
    if (!sprite_metadata_.HasAnimation("spark_particle_born") || !sprite_metadata_.HasAnimation("spark_particle_idle")) {
        return;
    }

    const float radius = GetPlayerGrapplingRangeWorld(*local_player);
    if (radius <= 1.0f) {
        return;
    }

    const float circumference = 2.0f * PI * radius;
    const int particle_count =
        std::max(16, static_cast<int>(std::round(circumference / Constants::kGrapplingPreviewParticleSpacingWorld)));
    if (particle_count <= 0) {
        return;
    }

    const float born_duration =
        GetAnimationDurationSeconds(sprite_metadata_, "spark_particle_born", "default", 1, 0.15f);
    const float idle_duration =
        GetAnimationDurationSeconds(sprite_metadata_, "spark_particle_idle", "default", 1, 0.20f);
    const float hold_elapsed = std::max(0.0f, render_time_seconds_ - grappling_preview_started_time_);
    const float particle_scale = Constants::kGrapplingPreviewParticleScale;

    for (int i = 0; i < particle_count; ++i) {
        const float angle = (2.0f * PI * static_cast<float>(i)) / static_cast<float>(particle_count);
        const Vector2 center = {
            local_player->pos.x + std::cos(angle) * radius,
            local_player->pos.y + std::sin(angle) * radius,
        };
        const float phase_offset = born_duration * (static_cast<float>(i) / static_cast<float>(particle_count));
        const float particle_elapsed = std::max(0.0f, hold_elapsed - phase_offset);

        const char* animation = "spark_particle_idle";
        float animation_time = 0.0f;
        if (particle_elapsed < born_duration) {
            animation = "spark_particle_born";
            animation_time = particle_elapsed;
        } else {
            animation = "spark_particle_idle";
            animation_time = std::fmod(std::max(0.0f, particle_elapsed - born_duration), std::max(0.05f, idle_duration));
        }

        const Rectangle src = InsetSourceRect(sprite_metadata_.GetFrame(animation, "default", animation_time),
                                              Constants::kAtlasSampleInsetPixels);
        const float dst_w = static_cast<float>(sprite_metadata_.GetCellWidth()) * particle_scale;
        const float dst_h = static_cast<float>(sprite_metadata_.GetCellHeight()) * particle_scale;
        Rectangle dst = {center.x - dst_w * 0.5f, center.y - dst_h * 0.5f, dst_w, dst_h};
        DrawTexturePro(sprite_metadata_.GetTexture(), src, SnapRect(dst), {0.0f, 0.0f}, 0.0f,
                       Color{255, 255, 255, 225});
    }
}

void GameApp::EnsureWorldLayerRenderTarget() {
    const int width = GetScreenWidth();
    const int height = GetScreenHeight();
    if (width <= 0 || height <= 0) {
        return;
    }
    if (has_world_layer_target_ && world_layer_target_.texture.width == width &&
        world_layer_target_.texture.height == height) {
        return;
    }
    if (has_world_layer_target_) {
        UnloadRenderTexture(world_layer_target_);
        world_layer_target_ = {};
        has_world_layer_target_ = false;
    }

    world_layer_target_ = LoadRenderTexture(width, height);
    has_world_layer_target_ = (world_layer_target_.id != 0);
    if (has_world_layer_target_) {
        SetTextureFilter(world_layer_target_.texture, TEXTURE_FILTER_POINT);
    }
}

void GameApp::EnsureShadowLayerRenderTarget() {
    const int width = GetScreenWidth();
    const int height = GetScreenHeight();
    if (width <= 0 || height <= 0) {
        return;
    }
    if (has_shadow_layer_target_ && shadow_layer_target_.texture.width == width &&
        shadow_layer_target_.texture.height == height) {
        return;
    }
    if (has_shadow_layer_target_) {
        UnloadRenderTexture(shadow_layer_target_);
        shadow_layer_target_ = {};
        has_shadow_layer_target_ = false;
    }

    shadow_layer_target_ = LoadRenderTexture(width, height);
    has_shadow_layer_target_ = (shadow_layer_target_.id != 0);
    if (has_shadow_layer_target_) {
        SetTextureFilter(shadow_layer_target_.texture, TEXTURE_FILTER_POINT);
    }
}

void GameApp::RenderGroundMapObjects() {
    const bool has_texture = sprite_metadata_.IsLoaded();
    const Texture2D texture = sprite_metadata_.GetTexture();
    const bool has_tall_texture = sprite_metadata_tall_.IsLoaded();
    const Texture2D tall_texture = sprite_metadata_tall_.GetTexture();
    const bool has_huge_texture = sprite_metadata_128x128_.IsLoaded();
    const Texture2D huge_texture = sprite_metadata_128x128_.GetTexture();
    // Flat decorations are the densest object class, so keep this pass on direct vector indices
    // and reuse one wind shader instance with deterministic world-space phase when possible.
    bool wind_shader_active = false;
    bool wind_shader_uniforms_initialized = false;
    Rectangle wind_shader_last_src = {};
    float wind_shader_last_strength = 0.0f;
    float wind_shader_last_speed = 0.0f;
    float wind_shader_last_gradient_start = 0.0f;
    const auto stop_wind_shader = [&]() {
        if (wind_shader_active) {
            EndShaderMode();
            wind_shader_active = false;
        }
    };
    const auto ensure_wind_shader = [&]() {
        if (wind_shader_active || !has_tree_wind_shader_) {
            return;
        }
        const float screen_height = static_cast<float>(GetScreenHeight());
        const float camera_target[2] = {camera_.target.x, camera_.target.y};
        const float camera_offset[2] = {camera_.offset.x, camera_.offset.y};
        const float camera_zoom = camera_.zoom;
        SetShaderValue(tree_wind_shader_, tree_wind_screen_height_loc_, &screen_height, SHADER_UNIFORM_FLOAT);
        SetShaderValueV(tree_wind_shader_, tree_wind_camera_target_loc_, camera_target, SHADER_UNIFORM_VEC2, 1);
        SetShaderValueV(tree_wind_shader_, tree_wind_camera_offset_loc_, camera_offset, SHADER_UNIFORM_VEC2, 1);
        SetShaderValue(tree_wind_shader_, tree_wind_camera_zoom_loc_, &camera_zoom, SHADER_UNIFORM_FLOAT);
        BeginShaderMode(tree_wind_shader_);
        wind_shader_active = true;
        wind_shader_uniforms_initialized = false;
    };
    const auto configure_wind_shader = [&](Rectangle src, const ObjectPrototype& proto) {
        ensure_wind_shader();
        const float sway_strength = proto.wind_strength_pixels;
        const float sway_speed = Constants::kTreeWindSpeed * proto.wind_speed_multiplier;
        const float gradient_start = proto.wind_gradient_start;
        if (wind_shader_uniforms_initialized && wind_shader_last_src.x == src.x && wind_shader_last_src.y == src.y &&
            wind_shader_last_src.width == src.width && wind_shader_last_src.height == src.height &&
            wind_shader_last_strength == sway_strength && wind_shader_last_speed == sway_speed &&
            wind_shader_last_gradient_start == gradient_start) {
            return;
        }

        const float frame_rect[4] = {src.x, src.y, src.width, src.height};
        const float time_seconds = render_time_seconds_;
        SetShaderValueV(tree_wind_shader_, tree_wind_frame_rect_loc_, frame_rect, SHADER_UNIFORM_VEC4, 1);
        SetShaderValue(tree_wind_shader_, tree_wind_time_loc_, &time_seconds, SHADER_UNIFORM_FLOAT);
        SetShaderValue(tree_wind_shader_, tree_wind_sway_strength_loc_, &sway_strength, SHADER_UNIFORM_FLOAT);
        SetShaderValue(tree_wind_shader_, tree_wind_sway_speed_loc_, &sway_speed, SHADER_UNIFORM_FLOAT);
        SetShaderValue(tree_wind_shader_, tree_wind_gradient_start_loc_, &gradient_start, SHADER_UNIFORM_FLOAT);
        wind_shader_last_src = src;
        wind_shader_last_strength = sway_strength;
        wind_shader_last_speed = sway_speed;
        wind_shader_last_gradient_start = gradient_start;
        wind_shader_uniforms_initialized = true;
    };
    const auto draw_object = [&](const MapObjectInstance& object, const ObjectPrototype* proto) {
        if (proto == nullptr || !IsFlatRenderedMapObjectPrototype(proto)) {
            return;
        }

        const SpriteMetadataLoader* metadata = &sprite_metadata_;
        const Texture2D* draw_texture = has_texture ? &texture : nullptr;
        switch (proto->sprite_sheet) {
            case SpriteSheetType::Base32:
                metadata = &sprite_metadata_;
                draw_texture = has_texture ? &texture : nullptr;
                break;
            case SpriteSheetType::Tall32x64:
                metadata = &sprite_metadata_tall_;
                draw_texture = has_tall_texture ? &tall_texture : nullptr;
                break;
            case SpriteSheetType::Large128x128:
                metadata = &sprite_metadata_128x128_;
                draw_texture = has_huge_texture ? &huge_texture : nullptr;
                break;
        }

        const float base_dst_w =
            static_cast<float>(metadata->GetCellWidth() > 0 ? metadata->GetCellWidth() : state_.map.cell_size);
        const float base_dst_h =
            static_cast<float>(metadata->GetCellHeight() > 0 ? metadata->GetCellHeight() : state_.map.cell_size);
        const float visual_scale =
            proto->type == ObjectType::Consumable ? Constants::kDroppedItemVisualScale : 1.0f;
        const Rectangle dst = GetMapObjectSpriteRect(object, proto, base_dst_w, base_dst_h, visual_scale);
        if (!IsWorldRectVisible(dst, static_cast<float>(state_.map.cell_size))) {
            return;
        }
        float anim_time = render_time_seconds_;
        const std::string animation = ResolveMapObjectAnimation(object, *proto, *metadata, &anim_time);

        if (draw_texture != nullptr && metadata->IsLoaded() && metadata->HasAnimation(animation)) {
            const Rectangle src =
                InsetSourceRect(metadata->GetFrame(animation, "default", anim_time), Constants::kAtlasSampleInsetPixels);
            const float flash_amount = GetObjectDamageFlashAmount(object);
            const bool use_damage_flash = flash_amount > 0.0f && has_damage_flash_shader_;
            const bool use_wind_batch = !use_damage_flash && proto->wind_strength_pixels > 0.0f && has_tree_wind_shader_;
            if (use_damage_flash) {
                stop_wind_shader();
                SetShaderValue(damage_flash_shader_, damage_flash_amount_loc_, &flash_amount, SHADER_UNIFORM_FLOAT);
                BeginShaderMode(damage_flash_shader_);
            }
            if (use_wind_batch) {
                configure_wind_shader(src, *proto);
                DrawTexturePro(*draw_texture, src, dst, {0, 0}, 0.0f, WHITE);
            } else {
                stop_wind_shader();
                DrawWindAnimatedMapObject(object, *proto, *draw_texture, src, dst);
            }
            if (use_damage_flash) {
                EndShaderMode();
            }
        } else {
            stop_wind_shader();
            DrawRectangleRec(dst, Color{180, 120, 80, 220});
            DrawRectangleLinesEx(dst, 1.0f, Color{30, 20, 12, 255});
        }
    };

    if (has_texture && sprite_metadata_.HasAnimation("embers_idle")) {
        for (const auto& modifier : state_.embers_tile_modifiers) {
            if (!modifier.alive || !state_.map.IsInside(modifier.cell)) {
                continue;
            }
            const Vector2 center = CellToWorldCenter(modifier.cell);
            const Rectangle dst = {center.x - 16.0f, center.y - 16.0f, 32.0f, 32.0f};
            if (!IsWorldRectVisible(dst, static_cast<float>(state_.map.cell_size))) {
                continue;
            }
            const Rectangle src =
                InsetSourceRect(sprite_metadata_.GetFrame("embers_idle", "default", render_time_seconds_),
                                Constants::kAtlasSampleInsetPixels);
            DrawTexturePro(texture, src, SnapRect(dst), {0, 0}, 0.0f, WHITE);
        }
    }

    for (size_t object_index : flat_map_object_indices_) {
        if (object_index >= state_.map_objects.size()) {
            continue;
        }
        const MapObjectInstance& object = state_.map_objects[object_index];
        if (!object.alive) {
            continue;
        }
        draw_object(object, FindObjectPrototype(object.prototype_id));
    }
    stop_wind_shader();
}

void GameApp::UpdateObjectShadowLayer() {
    EnsureShadowLayerRenderTarget();
    if (!has_shadow_layer_target_) {
        return;
    }

    BeginTextureMode(shadow_layer_target_);
    ClearBackground(Color{0, 0, 0, 0});
    BeginMode2D(camera_);

    const bool has_texture = sprite_metadata_.IsLoaded();
    const Texture2D texture = sprite_metadata_.GetTexture();
    const bool has_tall_texture = sprite_metadata_tall_.IsLoaded();
    const Texture2D tall_texture = sprite_metadata_tall_.GetTexture();
    const bool has_huge_texture = sprite_metadata_128x128_.IsLoaded();
    const Texture2D huge_texture = sprite_metadata_128x128_.GetTexture();
    const auto draw_shadow = [&](const MapObjectInstance& object, const ObjectPrototype* proto) {
        if (!object.alive || object.state == MapObjectState::Dying || proto == nullptr || proto->shadow_animation.empty()) {
            return;
        }
        if (!proto->casts_shadow) {
            return;
        }
        if (RenderModularTreeShadow(object, *proto)) {
            return;
        }

        const SpriteMetadataLoader* metadata = &sprite_metadata_;
        const Texture2D* draw_texture = has_texture ? &texture : nullptr;
        switch (proto->shadow_sheet) {
            case SpriteSheetType::Base32:
                metadata = &sprite_metadata_;
                draw_texture = has_texture ? &texture : nullptr;
                break;
            case SpriteSheetType::Tall32x64:
                metadata = &sprite_metadata_tall_;
                draw_texture = has_tall_texture ? &tall_texture : nullptr;
                break;
            case SpriteSheetType::Large128x128:
                metadata = &sprite_metadata_128x128_;
                draw_texture = has_huge_texture ? &huge_texture : nullptr;
                break;
        }
        if (draw_texture == nullptr || !metadata->IsLoaded() || !metadata->HasAnimation(proto->shadow_animation)) {
            return;
        }

        int object_cell_width = state_.map.cell_size;
        switch (proto->sprite_sheet) {
            case SpriteSheetType::Base32:
                object_cell_width = sprite_metadata_.GetCellWidth() > 0 ? sprite_metadata_.GetCellWidth() : state_.map.cell_size;
                break;
            case SpriteSheetType::Tall32x64:
                object_cell_width =
                    sprite_metadata_tall_.GetCellWidth() > 0 ? sprite_metadata_tall_.GetCellWidth() : state_.map.cell_size;
                break;
            case SpriteSheetType::Large128x128:
                object_cell_width = sprite_metadata_128x128_.GetCellWidth() > 0 ? sprite_metadata_128x128_.GetCellWidth()
                                                                                : state_.map.cell_size;
                break;
        }

        const float base_dst_w =
            static_cast<float>(metadata->GetCellWidth() > 0 ? metadata->GetCellWidth() : state_.map.cell_size);
        const float base_dst_h =
            static_cast<float>(metadata->GetCellHeight() > 0 ? metadata->GetCellHeight() : state_.map.cell_size);
        const float visual_scale =
            proto->type == ObjectType::Consumable ? Constants::kDroppedItemVisualScale : 1.0f;
        const float dst_w = base_dst_w * visual_scale;
        const float dst_h = base_dst_h * visual_scale;
        Rectangle dst = {static_cast<float>(object.cell.x * state_.map.cell_size) +
                             (static_cast<float>(object_cell_width) - base_dst_w) * 0.5f,
                         static_cast<float>(object.cell.y * state_.map.cell_size) -
                             (base_dst_h - static_cast<float>(state_.map.cell_size)),
                         dst_w, dst_h};
        dst.x += (base_dst_w - dst_w) * 0.5f;
        dst.y += (base_dst_h - dst_h) * 0.5f;
        dst = SnapRect(dst);
        if (!IsWorldRectVisible(dst, static_cast<float>(state_.map.cell_size))) {
            return;
        }

        const Rectangle src = InsetSourceRect(
            metadata->GetFrame(proto->shadow_animation, "default", render_time_seconds_),
            Constants::kAtlasSampleInsetPixels);
        DrawTexturePro(*draw_texture, src, dst, {0, 0}, 0.0f, WHITE);
    };

    for (size_t object_index : shadow_map_object_indices_) {
        if (object_index >= state_.map_objects.size()) {
            continue;
        }
        const MapObjectInstance& object = state_.map_objects[object_index];
        draw_shadow(object, FindObjectPrototype(object.prototype_id));
    }

    if (modular_player_asset_.HasLayer("shadow")) {
        const Texture2D* player_shadow_texture = modular_player_asset_.GetLayerTexture("shadow");
        if (player_shadow_texture != nullptr) {
            for (const auto& player : state_.players) {
                if (!player.alive) {
                    continue;
                }
                const std::string tag = ResolvePlayerModularTag(player);
                if (!modular_player_asset_.HasTag("shadow", tag)) {
                    continue;
                }
                const Vector2 draw_pos = GetRenderPlayerPosition(player.id);
                const Rectangle dst = GetPlayerSpriteRect(draw_pos);
                const float modular_time = ResolvePlayerModularTime(player);
                const Rectangle src =
                    InsetSourceRect(modular_player_asset_.GetFrame("shadow", tag, modular_time),
                                    Constants::kAtlasSampleInsetPixels);
                DrawTexturePro(*player_shadow_texture, src, dst, {0, 0}, 0.0f, WHITE);
            }
        }
    }

    EndMode2D();
    EndTextureMode();
}

void GameApp::DrawObjectShadowLayer() {
    if (!has_shadow_layer_target_) {
        return;
    }
    const Rectangle src = {0.0f, 0.0f, static_cast<float>(shadow_layer_target_.texture.width),
                           -static_cast<float>(shadow_layer_target_.texture.height)};
    const Rectangle dst = {0.0f, 0.0f, static_cast<float>(GetScreenWidth()), static_cast<float>(GetScreenHeight())};
    DrawTexturePro(shadow_layer_target_.texture, src, dst, {0.0f, 0.0f}, 0.0f,
                   Color{255, 255, 255, Constants::kGlobalShadowAlpha});
}

bool GameApp::IsZoneEnabled() const { return state_.match.zone_enabled; }

void GameApp::DrawWorldLayerWithZonePostProcess() {
    if (!has_world_layer_target_) {
        return;
    }

    const Rectangle src = {0.0f, 0.0f, static_cast<float>(world_layer_target_.texture.width),
                           -static_cast<float>(world_layer_target_.texture.height)};
    const Rectangle dst = {0.0f, 0.0f, static_cast<float>(GetScreenWidth()), static_cast<float>(GetScreenHeight())};
    if (!IsZoneEnabled() || !has_zone_post_process_shader_) {
        DrawTexturePro(world_layer_target_.texture, src, dst, {0.0f, 0.0f}, 0.0f, WHITE);
        return;
    }

    const float screen_height = static_cast<float>(GetScreenHeight());
    const float camera_target[2] = {camera_.target.x, camera_.target.y};
    const float camera_offset[2] = {camera_.offset.x, camera_.offset.y};
    const float camera_zoom = camera_.zoom;
    const float zone_center[2] = {state_.match.arena_center_world.x, state_.match.arena_center_world.y};
    const float zone_radius = state_.match.arena_radius_world;

    SetShaderValue(zone_post_process_shader_, zone_post_process_screen_height_loc_, &screen_height, SHADER_UNIFORM_FLOAT);
    SetShaderValueV(zone_post_process_shader_, zone_post_process_camera_target_loc_, camera_target,
                    SHADER_UNIFORM_VEC2, 1);
    SetShaderValueV(zone_post_process_shader_, zone_post_process_camera_offset_loc_, camera_offset,
                    SHADER_UNIFORM_VEC2, 1);
    SetShaderValue(zone_post_process_shader_, zone_post_process_camera_zoom_loc_, &camera_zoom, SHADER_UNIFORM_FLOAT);
    SetShaderValueV(zone_post_process_shader_, zone_post_process_zone_center_loc_, zone_center, SHADER_UNIFORM_VEC2, 1);
    SetShaderValue(zone_post_process_shader_, zone_post_process_zone_radius_loc_, &zone_radius, SHADER_UNIFORM_FLOAT);

    BeginShaderMode(zone_post_process_shader_);
    DrawTexturePro(world_layer_target_.texture, src, dst, {0.0f, 0.0f}, 0.0f, WHITE);
    EndShaderMode();
}

void GameApp::RenderZoneFillOverlay() {
    if (!IsZoneEnabled()) {
        return;
    }
    if (!has_zone_fill_overlay_shader_ || !sprite_metadata_.IsLoaded() || !sprite_metadata_.HasAnimation("zone_fill")) {
        return;
    }

    const Texture2D texture = sprite_metadata_.GetTexture();
    const Rectangle frame_rect = sprite_metadata_.GetFrame("zone_fill", "default", render_time_seconds_);
    const Rectangle src = {0.0f, 0.0f, static_cast<float>(texture.width), static_cast<float>(texture.height)};
    const Rectangle dst = {0.0f, 0.0f, static_cast<float>(GetScreenWidth()), static_cast<float>(GetScreenHeight())};
    const float screen_height = static_cast<float>(GetScreenHeight());
    const float camera_target[2] = {camera_.target.x, camera_.target.y};
    const float camera_offset[2] = {camera_.offset.x, camera_.offset.y};
    const float camera_zoom = camera_.zoom;
    const float zone_center[2] = {state_.match.arena_center_world.x, state_.match.arena_center_world.y};
    const float zone_radius = state_.match.arena_radius_world;
    const float zone_fill_rect_px[4] = {frame_rect.x, frame_rect.y, frame_rect.width, frame_rect.height};

    SetShaderValue(zone_fill_overlay_shader_, zone_fill_overlay_screen_height_loc_, &screen_height, SHADER_UNIFORM_FLOAT);
    SetShaderValueV(zone_fill_overlay_shader_, zone_fill_overlay_camera_target_loc_, camera_target, SHADER_UNIFORM_VEC2, 1);
    SetShaderValueV(zone_fill_overlay_shader_, zone_fill_overlay_camera_offset_loc_, camera_offset, SHADER_UNIFORM_VEC2, 1);
    SetShaderValue(zone_fill_overlay_shader_, zone_fill_overlay_camera_zoom_loc_, &camera_zoom, SHADER_UNIFORM_FLOAT);
    SetShaderValueV(zone_fill_overlay_shader_, zone_fill_overlay_zone_center_loc_, zone_center, SHADER_UNIFORM_VEC2, 1);
    SetShaderValue(zone_fill_overlay_shader_, zone_fill_overlay_zone_radius_loc_, &zone_radius, SHADER_UNIFORM_FLOAT);
    SetShaderValueV(zone_fill_overlay_shader_, zone_fill_overlay_zone_fill_rect_loc_, zone_fill_rect_px,
                    SHADER_UNIFORM_VEC4, 1);

    BeginShaderMode(zone_fill_overlay_shader_);
    DrawTexturePro(texture, src, dst, {0.0f, 0.0f}, 0.0f, WHITE);
    EndShaderMode();
}

void GameApp::RenderZoneBorderOverlay() {
    if (!IsZoneEnabled()) {
        return;
    }
    if (!has_zone_border_overlay_shader_) {
        return;
    }

    const float screen_height = static_cast<float>(GetScreenHeight());
    const float camera_target[2] = {camera_.target.x, camera_.target.y};
    const float camera_offset[2] = {camera_.offset.x, camera_.offset.y};
    const float camera_zoom = camera_.zoom;
    const float zone_center[2] = {state_.match.arena_center_world.x, state_.match.arena_center_world.y};
    const float zone_radius = state_.match.arena_radius_world;

    SetShaderValue(zone_border_overlay_shader_, zone_border_overlay_screen_height_loc_, &screen_height,
                   SHADER_UNIFORM_FLOAT);
    SetShaderValueV(zone_border_overlay_shader_, zone_border_overlay_camera_target_loc_, camera_target,
                    SHADER_UNIFORM_VEC2, 1);
    SetShaderValueV(zone_border_overlay_shader_, zone_border_overlay_camera_offset_loc_, camera_offset,
                    SHADER_UNIFORM_VEC2, 1);
    SetShaderValue(zone_border_overlay_shader_, zone_border_overlay_camera_zoom_loc_, &camera_zoom,
                   SHADER_UNIFORM_FLOAT);
    SetShaderValueV(zone_border_overlay_shader_, zone_border_overlay_zone_center_loc_, zone_center,
                    SHADER_UNIFORM_VEC2, 1);
    SetShaderValue(zone_border_overlay_shader_, zone_border_overlay_zone_radius_loc_, &zone_radius,
                   SHADER_UNIFORM_FLOAT);

    BeginShaderMode(zone_border_overlay_shader_);
    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), WHITE);
    EndShaderMode();
}

void GameApp::RenderInfluenceZoneOverlay() {
    if (!IsInfluenceZoneSystemEnabled() || !has_influence_zone_overlay_shader_ || state_.map.cell_size <= 0 ||
        state_.map.width <= 0 || state_.map.height <= 0) {
        return;
    }

    const float screen_height = static_cast<float>(GetScreenHeight());
    const float camera_target[2] = {camera_.target.x, camera_.target.y};
    const float camera_offset[2] = {camera_.offset.x, camera_.offset.y};
    const float camera_zoom = camera_.zoom;
    const float map_size_world[2] = {static_cast<float>(state_.map.width * state_.map.cell_size),
                                     static_cast<float>(state_.map.height * state_.map.cell_size)};
    const float signed_distance_range[2] = {-6.0f, 22.0f};
    const float pattern_frame = std::floor(render_time_seconds_ * 10.0f);
    const float blend_t = std::clamp(influence_zone_transition_elapsed_seconds_ /
                                         std::max(0.0001f, Constants::kInfluenceZoneTransitionSeconds),
                                     0.0f, 1.0f);

    // The influence overlay is drawn into the intermediate world render target.
    // Preserve destination alpha while applying normal source-over RGB blending,
    // otherwise semi-transparent overlay pixels can leave the RT partially transparent
    // and later compositing exposes black under discarded pixels in tall shaders.
    rlSetBlendFactorsSeparate(RL_SRC_ALPHA, RL_ONE_MINUS_SRC_ALPHA, RL_ONE, RL_ONE_MINUS_SRC_ALPHA, RL_FUNC_ADD,
                              RL_FUNC_ADD);
    BeginBlendMode(BLEND_CUSTOM_SEPARATE);

    SetShaderValue(influence_zone_overlay_shader_, influence_zone_overlay_screen_height_loc_, &screen_height,
                   SHADER_UNIFORM_FLOAT);
    SetShaderValueV(influence_zone_overlay_shader_, influence_zone_overlay_camera_target_loc_, camera_target,
                    SHADER_UNIFORM_VEC2, 1);
    SetShaderValueV(influence_zone_overlay_shader_, influence_zone_overlay_camera_offset_loc_, camera_offset,
                    SHADER_UNIFORM_VEC2, 1);
    SetShaderValue(influence_zone_overlay_shader_, influence_zone_overlay_camera_zoom_loc_, &camera_zoom,
                   SHADER_UNIFORM_FLOAT);
    SetShaderValueV(influence_zone_overlay_shader_, influence_zone_overlay_map_size_world_loc_, map_size_world,
                    SHADER_UNIFORM_VEC2, 1);
    SetShaderValueV(influence_zone_overlay_shader_, influence_zone_overlay_signed_distance_range_loc_,
                    signed_distance_range, SHADER_UNIFORM_VEC2, 1);
    SetShaderValue(influence_zone_overlay_shader_, influence_zone_overlay_pattern_frame_loc_, &pattern_frame,
                   SHADER_UNIFORM_FLOAT);

    const auto draw_team_field = [&](const Texture2D& from_texture, bool has_from, const Texture2D& to_texture,
                                     bool has_to, const float tint[4], float pattern_phase) {
        if (!has_from && !has_to) {
            return;
        }

        const Texture2D& draw_texture = has_from ? from_texture : to_texture;
        const Texture2D& target_texture = has_to ? to_texture : draw_texture;
        const float draw_blend_t = (has_from && has_to) ? blend_t : 1.0f;
        const Rectangle src = {0.0f, 0.0f, static_cast<float>(draw_texture.width), static_cast<float>(draw_texture.height)};
        // This draw happens under BeginMode2D(camera_), so the destination rectangle is in world space.
        // Using screen-space dimensions clips the overlay to the first visible screen-width chunk of the map.
        const Rectangle dst = {0.0f, 0.0f, map_size_world[0], map_size_world[1]};
        rlDrawRenderBatchActive();
        BeginShaderMode(influence_zone_overlay_shader_);
        SetShaderValueV(influence_zone_overlay_shader_, influence_zone_overlay_tint_loc_, tint, SHADER_UNIFORM_VEC4, 1);
        SetShaderValue(influence_zone_overlay_shader_, influence_zone_overlay_pattern_phase_loc_, &pattern_phase,
                       SHADER_UNIFORM_FLOAT);
        SetShaderValueTexture(influence_zone_overlay_shader_, influence_zone_overlay_to_distance_texture_loc_, target_texture);
        SetShaderValue(influence_zone_overlay_shader_, influence_zone_overlay_blend_t_loc_, &draw_blend_t,
                       SHADER_UNIFORM_FLOAT);
        DrawTexturePro(draw_texture, src, dst, {0.0f, 0.0f}, 0.0f, WHITE);
        EndShaderMode();
    };

    const float red_tint[4] = {1.0f, 100.0f / 255.0f, 100.0f / 255.0f, 1.0f};
    const float blue_tint[4] = {120.0f / 255.0f, 150.0f / 255.0f, 1.0f, 1.0f};
    if (ShouldRenderInfluenceTeam(Constants::kTeamRed)) {
        draw_team_field(influence_zone_distance_red_from_texture_, has_influence_zone_distance_red_from_texture_,
                        influence_zone_distance_red_to_texture_, has_influence_zone_distance_red_to_texture_, red_tint, 0.0f);
    }
    if (ShouldRenderInfluenceTeam(Constants::kTeamBlue)) {
        draw_team_field(influence_zone_distance_blue_from_texture_, has_influence_zone_distance_blue_from_texture_,
                        influence_zone_distance_blue_to_texture_, has_influence_zone_distance_blue_to_texture_, blue_tint, 1.0f);
    }

    EndBlendMode();
}

void GameApp::RenderInfluenceZoneAnimatedTiles() {
    if (!IsInfluenceZoneSystemEnabled() || state_.influence_zones.empty()) {
        return;
    }

    const bool has_texture = sprite_metadata_.IsLoaded();
    const Texture2D texture = sprite_metadata_.GetTexture();
    for (const auto& zone : state_.influence_zones) {
        if (!ShouldRenderInfluenceTeam(zone.team)) {
            continue;
        }
        if (!state_.map.IsInside(zone.cell)) {
            continue;
        }
        const Rectangle dst = SnapRect(
            {static_cast<float>(zone.cell.x * state_.map.cell_size), static_cast<float>(zone.cell.y * state_.map.cell_size),
             static_cast<float>(state_.map.cell_size), static_cast<float>(state_.map.cell_size)});
        const char* animation = zone.team == Constants::kTeamRed ? "influence_zone_red" : "influence_zone_blue";
        if (has_texture && sprite_metadata_.HasAnimation(animation)) {
            const Rectangle src =
                InsetSourceRect(sprite_metadata_.GetFrame(animation, "default", render_time_seconds_),
                                Constants::kAtlasSampleInsetPixels);
            DrawTexturePro(texture, src, dst, {0, 0}, 0.0f, Color{255, 255, 255, 32});
        } else {
            const Color tint =
                zone.team == Constants::kTeamRed ? Color{255, 100, 100, 96} : Color{120, 150, 255, 96};
            DrawRectangleRec(dst, tint);
        }
    }
}

bool GameApp::ShouldRenderInfluenceTeam(int team) const {
    if (!settings_.hide_own_influence_zones) {
        return true;
    }
    if (state_.local_player_id < 0) {
        return true;
    }
    const Player* local_player = FindPlayerById(state_.local_player_id);
    if (local_player == nullptr) {
        return true;
    }
    return local_player->team != team;
}

bool GameApp::IsInfluenceZoneSystemEnabled() const { return settings_.enable_influence_zone_system; }

void GameApp::RenderMapBoundsFadeOverlay() {
    if (!has_map_bounds_fade_shader_ || state_.map.cell_size <= 0) {
        return;
    }

    const float screen_height = static_cast<float>(GetScreenHeight());
    const float camera_target[2] = {camera_.target.x, camera_.target.y};
    const float camera_offset[2] = {camera_.offset.x, camera_.offset.y};
    const float camera_zoom = camera_.zoom;
    const float cell = static_cast<float>(state_.map.cell_size);
    const float dual_grid_offset = cell * 0.5f;
    const float map_width_world = static_cast<float>(state_.map.width * state_.map.cell_size);
    const float map_height_world = static_cast<float>(state_.map.height * state_.map.cell_size);
    const float fade_rect_min[2] = {dual_grid_offset, dual_grid_offset};
    const float fade_rect_max[2] = {std::max(dual_grid_offset, map_width_world + dual_grid_offset),
                                    std::max(dual_grid_offset, map_height_world + dual_grid_offset)};

    SetShaderValue(map_bounds_fade_shader_, map_bounds_fade_screen_height_loc_, &screen_height, SHADER_UNIFORM_FLOAT);
    SetShaderValueV(map_bounds_fade_shader_, map_bounds_fade_camera_target_loc_, camera_target, SHADER_UNIFORM_VEC2, 1);
    SetShaderValueV(map_bounds_fade_shader_, map_bounds_fade_camera_offset_loc_, camera_offset, SHADER_UNIFORM_VEC2, 1);
    SetShaderValue(map_bounds_fade_shader_, map_bounds_fade_camera_zoom_loc_, &camera_zoom, SHADER_UNIFORM_FLOAT);
    SetShaderValueV(map_bounds_fade_shader_, map_bounds_fade_fade_rect_min_loc_, fade_rect_min, SHADER_UNIFORM_VEC2, 1);
    SetShaderValueV(map_bounds_fade_shader_, map_bounds_fade_fade_rect_max_loc_, fade_rect_max, SHADER_UNIFORM_VEC2, 1);

    BeginShaderMode(map_bounds_fade_shader_);
    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), WHITE);
    EndShaderMode();
}

void GameApp::RenderConsoleLog() const {
    struct ConsoleRenderLine {
        std::vector<ConsoleTextSpanMessage> spans;
    };

    std::vector<ConsoleRenderLine> lines;
    lines.reserve(console_entries_.size() * 2);
    for (const auto& entry : console_entries_) {
        ConsoleRenderLine current_line;
        int chars_in_line = 0;
        auto flush_line = [&]() {
            if (!current_line.spans.empty()) {
                lines.push_back(std::move(current_line));
                current_line = ConsoleRenderLine{};
                chars_in_line = 0;
            }
        };
        for (const auto& span : entry.message.spans) {
            std::string chunk;
            for (char ch : span.text) {
                if (ch == '\n' || chars_in_line >= kConsoleCharsPerLine) {
                    if (!chunk.empty()) {
                        current_line.spans.push_back({chunk, span.r, span.g, span.b, span.a});
                        chunk.clear();
                    }
                    flush_line();
                    if (ch == '\n') {
                        continue;
                    }
                }
                chunk.push_back(ch);
                chars_in_line += 1;
            }
            if (!chunk.empty()) {
                current_line.spans.push_back({chunk, span.r, span.g, span.b, span.a});
            }
        }
        flush_line();
    }

    if (lines.empty()) {
        return;
    }
    if (static_cast<int>(lines.size()) > kConsoleMaxVisibleLines) {
        lines.erase(lines.begin(), lines.end() - kConsoleMaxVisibleLines);
    }

    const float scale = Constants::kHudScale;
    const float panel_height = (Constants::kHudPanelHeight - 28.0f) * scale;
    const float hud_y = static_cast<float>(GetScreenHeight()) - panel_height - 4.0f;
    const int font_size = 18;
    const int line_height = 20;
    const int x = 16;
    const int y = static_cast<int>(hud_y) - 12 - static_cast<int>(lines.size()) * line_height;

    for (size_t i = 0; i < lines.size(); ++i) {
        int line_x = x;
        const int line_y = y + static_cast<int>(i) * line_height;
        for (const auto& span : lines[i].spans) {
            if (span.text.empty()) {
                continue;
            }
            DrawText(span.text.c_str(), line_x + 1, line_y + 1, font_size, Color{0, 0, 0, 180});
            DrawText(span.text.c_str(), line_x, line_y, font_size, SpanColor(span));
            line_x += MeasureText(span.text.c_str(), font_size);
        }
    }
}

void GameApp::RenderChatInput() const {
    if (!chat_input_active_) {
        return;
    }
    const float scale = Constants::kHudScale;
    const float panel_height = (Constants::kHudPanelHeight - 28.0f) * scale;
    const float hud_y = static_cast<float>(GetScreenHeight()) - panel_height - 4.0f;
    const Rectangle box = {12.0f, hud_y - 42.0f, 480.0f, 28.0f};
    DrawRectangleRec(box, Color{16, 18, 24, 230});
    DrawRectangleLinesEx(box, 1.0f, Color{88, 96, 112, 255});
    DrawText("> ", static_cast<int>(box.x + 8.0f), static_cast<int>(box.y + 6.0f), 18, Color{220, 220, 220, 255});
    DrawText(chat_input_buffer_.c_str(), static_cast<int>(box.x + 26.0f), static_cast<int>(box.y + 6.0f), 18,
             RAYWHITE);
}

void GameApp::RenderNonTerrainDepthSorted(DepthSortedRenderPass pass) {
    enum class RenderKind {
        LegacyDecoration,
        MapObject,
        CompositeEffectPart,
        FireStormDummyPart,
        EarthRootsPart,
        StatusEffectPart,
        Rune,
        IceWall,
        LightningSegment,
        GrapplingHookSegment,
        Projectile,
        FireWaveSegment,
        FireSpiritIdle,
        FireSpiritTrail,
        FireSpiritProjectile,
        FireStormArc,
        Particle,
        HammerImpact,
        Player,
        PlayerAttachedAnimation,
    };

    struct RenderItem {
        RenderKind kind = RenderKind::Player;
        float sort_y = 0.0f;
        size_t index = 0;
        size_t aux_index = 0;
        size_t sub_index = 0;
    };

    const auto kind_priority = [](RenderKind kind) {
        switch (kind) {
            case RenderKind::LegacyDecoration:
                return 0;
            case RenderKind::MapObject:
                return 1;
            case RenderKind::CompositeEffectPart:
                return 2;
            case RenderKind::FireStormDummyPart:
                return 3;
            case RenderKind::EarthRootsPart:
                return 4;
            case RenderKind::StatusEffectPart:
                return 5;
            case RenderKind::Rune:
                return 6;
            case RenderKind::IceWall:
                return 7;
            case RenderKind::LightningSegment:
                return 8;
            case RenderKind::GrapplingHookSegment:
                return 9;
            case RenderKind::Projectile:
                return 10;
            case RenderKind::FireWaveSegment:
                return 11;
            case RenderKind::FireSpiritIdle:
                return 12;
            case RenderKind::FireSpiritTrail:
                return 13;
            case RenderKind::FireSpiritProjectile:
                return 14;
            case RenderKind::FireStormArc:
                return 15;
            case RenderKind::Particle:
                return 16;
            case RenderKind::HammerImpact:
                return 17;
            case RenderKind::Player:
                return 18;
            case RenderKind::PlayerAttachedAnimation:
                return 19;
            default:
                return 20;
        }
    };

    const auto pass_matches = [&](const RenderItem& item) {
        switch (pass) {
            case DepthSortedRenderPass::UnderInfluenceOverlay:
                return item.kind == RenderKind::CompositeEffectPart || item.kind == RenderKind::FireStormDummyPart ||
                       item.kind == RenderKind::Rune || item.kind == RenderKind::IceWall;
            case DepthSortedRenderPass::OverInfluenceOverlay:
                return !(item.kind == RenderKind::CompositeEffectPart || item.kind == RenderKind::FireStormDummyPart ||
                         item.kind == RenderKind::Rune || item.kind == RenderKind::IceWall);
        }
        return true;
    };

    const Rectangle visible_world =
        GetCameraWorldCullRect(static_cast<float>(std::max(1, state_.map.cell_size) * 3));
    const auto point_visible = [&](Vector2 world_pos) {
        return world_pos.x >= visible_world.x && world_pos.x <= visible_world.x + visible_world.width &&
               world_pos.y >= visible_world.y && world_pos.y <= visible_world.y + visible_world.height;
    };

    std::vector<RenderItem> items;
    size_t lightning_segment_total = 0;
    for (const auto& effect : state_.lightning_effects) {
        if (effect.alive) {
            lightning_segment_total += static_cast<size_t>(std::max(1, effect.segment_count));
        }
    }
    size_t grappling_segment_total = 0;
    for (const auto& hook : state_.grappling_hooks) {
        if (!hook.alive) {
            continue;
        }
        const Player* owner = FindPlayerById(hook.owner_player_id);
        if (owner == nullptr || !owner->alive) {
            continue;
        }
        const Vector2 start = Vector2Add(GetRenderPlayerPosition(owner->id),
                                         {0.0f, 0.5f * static_cast<float>(state_.map.cell_size)});
        const Vector2 end =
            (hook.phase == GrapplingHookPhase::Pulling)
                ? hook.latch_point
                : ((!network_manager_.IsHost() && hook.owner_player_id == state_.local_player_id)
                       ? hook.head_pos
                       : GetRenderGrapplingHookHeadPosition(hook.id, hook.head_pos));
        const float distance = Vector2Distance(start, end);
        grappling_segment_total += static_cast<size_t>(std::max(2, static_cast<int>(std::floor(
            distance / std::max(1.0f, static_cast<float>(state_.map.cell_size))))));
    }
    items.reserve(state_.players.size() + state_.runes.size() + state_.projectiles.size() + state_.fire_wave_segments.size() +
                  storm_arc_visuals_.size() + state_.particles.size() + hammer_impact_effects_.size() +
                  state_.ice_walls.size() + state_.map_objects.size() +
                  lightning_segment_total + grappling_segment_total + state_.earth_roots_groups.size() * 17);

    if (static_decoration_chunk_cols_ > 0 && static_decoration_chunk_rows_ > 0) {
        const int chunk_world = kStaticRenderChunkSizeTiles * std::max(1, state_.map.cell_size);
        const int min_chunk_x =
            std::clamp(static_cast<int>(std::floor(visible_world.x / static_cast<float>(chunk_world))), 0,
                       static_decoration_chunk_cols_ - 1);
        const int min_chunk_y =
            std::clamp(static_cast<int>(std::floor(visible_world.y / static_cast<float>(chunk_world))), 0,
                       static_decoration_chunk_rows_ - 1);
        const int max_chunk_x =
            std::clamp(static_cast<int>(std::floor((visible_world.x + visible_world.width) / static_cast<float>(chunk_world))), 0,
                       static_decoration_chunk_cols_ - 1);
        const int max_chunk_y =
            std::clamp(static_cast<int>(std::floor((visible_world.y + visible_world.height) / static_cast<float>(chunk_world))), 0,
                       static_decoration_chunk_rows_ - 1);
        for (int chunk_y = min_chunk_y; chunk_y <= max_chunk_y; ++chunk_y) {
            for (int chunk_x = min_chunk_x; chunk_x <= max_chunk_x; ++chunk_x) {
                const auto& chunk = static_decoration_render_chunks_[static_cast<size_t>(
                    chunk_y * static_decoration_chunk_cols_ + chunk_x)];
                for (const StaticDecorationRenderItem& item : chunk) {
                    items.push_back({RenderKind::LegacyDecoration, item.sort_y, item.decoration_index});
                }
            }
        }
    }

    for (size_t object_index : ysorted_map_object_indices_) {
        if (object_index >= state_.map_objects.size()) {
            continue;
        }
        const MapObjectInstance& object = state_.map_objects[object_index];
        const ObjectPrototype* proto = FindObjectPrototype(object.prototype_id);
        if (!object.alive || proto == nullptr || IsFlatRenderedMapObjectPrototype(proto)) {
            continue;
        }
        const Rectangle world_rect =
            GetMapObjectSpriteRect(object, proto, static_cast<float>(GetSpriteSheetWidth(proto->sprite_sheet)),
                                   static_cast<float>(GetSpriteSheetHeight(proto->sprite_sheet)), 1.0f, false);
        if (!IsWorldRectVisible(world_rect, static_cast<float>(state_.map.cell_size))) {
            continue;
        }
        float sort_y = (static_cast<float>(object.cell.y) + 1.0f) * static_cast<float>(state_.map.cell_size);
        if (proto->has_collision_box_override) {
            const Rectangle aabb = GetMapObjectCollisionAabb(object, proto);
            sort_y = aabb.y + aabb.height;
        }
        if (object.prototype_id == "vase_1" || object.prototype_id == "vase_2" || object.prototype_id == "vase_3" ||
            object.prototype_id == "vase_4") {
            sort_y -= 4.0f;
        }
        items.push_back({RenderKind::MapObject, sort_y, object_index});
    }

    for (size_t i = 0; i < state_.composite_effects.size(); ++i) {
        const CompositeEffectInstance& effect = state_.composite_effects[i];
        if (!effect.alive) {
            continue;
        }
        const CompositeEffectDefinition* definition = composite_effects_loader_.FindById(effect.effect_id);
        if (definition == nullptr) {
            continue;
        }
        for (size_t j = 0; j < definition->parts.size(); ++j) {
            const auto& part = definition->parts[j];
            if (part.layer != CompositeEffectLayer::YSorted) {
                continue;
            }
            const float cell = static_cast<float>(state_.map.cell_size);
            const float sort_y = effect.origin_world.y + 0.5f * cell + part.sort_anchor_y_tiles * cell;
            items.push_back({RenderKind::CompositeEffectPart, sort_y, i, 0, j});
        }
    }

    for (size_t i = 0; i < state_.fire_storm_dummies.size(); ++i) {
        const FireStormDummy& dummy = state_.fire_storm_dummies[i];
        if (!dummy.alive) {
            continue;
        }
        const char* effect_id = dummy.state == FireStormDummyState::Born
                                    ? "fire_storm_born"
                                    : (dummy.state == FireStormDummyState::Dying ? "fire_storm_death" : "fire_storm_idle");
        const CompositeEffectDefinition* definition = composite_effects_loader_.FindById(effect_id);
        if (definition == nullptr) {
            continue;
        }
        const Vector2 origin = CellToWorldCenter(dummy.cell);
        if (!point_visible(origin)) {
            continue;
        }
        for (size_t j = 0; j < definition->parts.size(); ++j) {
            const auto& part = definition->parts[j];
            if (part.layer != CompositeEffectLayer::YSorted) {
                continue;
            }
            const float cell = static_cast<float>(state_.map.cell_size);
            const float sort_y = origin.y + 0.5f * cell + part.sort_anchor_y_tiles * cell;
            items.push_back({RenderKind::FireStormDummyPart, sort_y, i, 0, j});
        }

        if (dummy.state == FireStormDummyState::Idle) {
            auto lightning_it = fire_storm_dummy_lightning_seconds_remaining_.find(dummy.id);
            if (lightning_it != fire_storm_dummy_lightning_seconds_remaining_.end() && lightning_it->second > 0.0f) {
                const CompositeEffectDefinition* lightning_definition = composite_effects_loader_.FindById("fire_storm_lightning");
                if (lightning_definition != nullptr) {
                    for (size_t j = 0; j < lightning_definition->parts.size(); ++j) {
                        const auto& part = lightning_definition->parts[j];
                        if (part.layer != CompositeEffectLayer::YSorted) {
                            continue;
                        }
                        const float cell = static_cast<float>(state_.map.cell_size);
                        const float sort_y = origin.y + 0.5f * cell + part.sort_anchor_y_tiles * cell;
                        items.push_back({RenderKind::FireStormDummyPart, sort_y, i, 1, j});
                    }
                }
            }
        }
    }

    for (size_t i = 0; i < state_.earth_roots_groups.size(); ++i) {
        const EarthRootsGroup& group = state_.earth_roots_groups[i];
        if (!group.alive) {
            continue;
        }
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const size_t tile_index = static_cast<size_t>((dy + 1) * 3 + (dx + 1));
                const GridCoord cell = {group.center_cell.x + dx, group.center_cell.y + dy};
                const Vector2 center = CellToWorldCenter(cell);
                if (!point_visible(center)) {
                    continue;
                }
                items.push_back({RenderKind::EarthRootsPart, center.y - 4.0f, i, tile_index, 0});
                if (!(dx == 0 && dy == 0)) {
                    items.push_back({RenderKind::EarthRootsPart, center.y + 8.0f, i, tile_index, 1});
                }
            }
        }
    }

    for (size_t i = 0; i < state_.players.size(); ++i) {
        const Player& player = state_.players[i];
        if (!player.alive) {
            continue;
        }
        const Vector2 origin = GetRenderPlayerPosition(player.id);
        if (!point_visible(origin)) {
            continue;
        }
        for (size_t j = 0; j < player.status_effects.size(); ++j) {
            const StatusEffectInstance& status = player.status_effects[j];
            if (status.composite_effect_id.empty()) {
                continue;
            }
            const CompositeEffectDefinition* definition = composite_effects_loader_.FindById(status.composite_effect_id);
            if (definition == nullptr) {
                continue;
            }
            for (size_t k = 0; k < definition->parts.size(); ++k) {
                const auto& part = definition->parts[k];
                if (part.layer != CompositeEffectLayer::YSorted) {
                    continue;
                }
                const float cell = static_cast<float>(state_.map.cell_size);
                const float sort_y = origin.y + 0.5f * cell + part.sort_anchor_y_tiles * cell;
                items.push_back({RenderKind::StatusEffectPart, sort_y, i, j, k});
            }
        }
    }

    for (size_t i = 0; i < state_.runes.size(); ++i) {
        if (!state_.runes[i].active && state_.runes[i].activation_remaining_seconds <= 0.0f &&
            state_.runes[i].fire_storm_visual_state != FireStormRuneVisualState::Dying) {
            continue;
        }
        const Vector2 center = CellToWorldCenter(state_.runes[i].cell);
        if (!point_visible(center)) {
            continue;
        }
        items.push_back({RenderKind::Rune, center.y, i});
    }

    for (size_t i = 0; i < state_.ice_walls.size(); ++i) {
        if (!state_.ice_walls[i].alive) {
            continue;
        }
        if (!point_visible(CellToWorldCenter(state_.ice_walls[i].cell))) {
            continue;
        }
        const float base_y = (static_cast<float>(state_.ice_walls[i].cell.y) + 1.0f) * static_cast<float>(state_.map.cell_size);
        items.push_back({RenderKind::IceWall, base_y, i});
    }

    for (size_t i = 0; i < state_.lightning_effects.size(); ++i) {
        const LightningEffect& effect = state_.lightning_effects[i];
        if (!effect.alive) {
            continue;
        }
        const Vector2 delta = Vector2Subtract(effect.end, effect.start);
        const int segments = std::max(1, effect.segment_count);
        for (int seg = 0; seg < segments; ++seg) {
            const float t0 = static_cast<float>(seg) / static_cast<float>(segments);
            const float t1 = static_cast<float>(seg + 1) / static_cast<float>(segments);
            const Vector2 p0 = Vector2Add(effect.start, Vector2Scale(delta, t0));
            const Vector2 p1 = Vector2Add(effect.start, Vector2Scale(delta, t1));
            const float sort_y = effect.has_sort_y_override ? effect.sort_y_override : (p0.y + p1.y) * 0.5f;
            items.push_back({RenderKind::LightningSegment, sort_y, i, 0, static_cast<size_t>(seg)});
        }
    }

    for (size_t i = 0; i < state_.grappling_hooks.size(); ++i) {
        const GrapplingHook& hook = state_.grappling_hooks[i];
        if (!hook.alive) {
            continue;
        }
        const Player* owner = FindPlayerById(hook.owner_player_id);
        if (owner == nullptr || !owner->alive) {
            continue;
        }
        const Vector2 start = GetRenderPlayerPosition(owner->id);
        const Vector2 end =
            (hook.phase == GrapplingHookPhase::Pulling)
                ? hook.latch_point
                : ((!network_manager_.IsHost() && hook.owner_player_id == state_.local_player_id)
                       ? hook.head_pos
                       : GetRenderGrapplingHookHeadPosition(hook.id, hook.head_pos));
        if (!point_visible(start) && !point_visible(end)) {
            continue;
        }
        const float distance = Vector2Distance(start, end);
        const int segments = std::max(
            2, static_cast<int>(std::floor(distance / std::max(1.0f, static_cast<float>(state_.map.cell_size)))));
        for (int seg = 0; seg < segments; ++seg) {
            const float t = (segments <= 1) ? 0.0f : (static_cast<float>(seg) / static_cast<float>(segments - 1));
            const Vector2 p = Vector2Lerp(start, end, t);
            items.push_back({RenderKind::GrapplingHookSegment, p.y, i, static_cast<size_t>(segments),
                             static_cast<size_t>(seg)});
        }
    }

    for (size_t i = 0; i < state_.projectiles.size(); ++i) {
        if (!state_.projectiles[i].alive) {
            continue;
        }
        if (!point_visible(state_.projectiles[i].pos)) {
            continue;
        }
        items.push_back({RenderKind::Projectile, state_.projectiles[i].pos.y, i});
    }

    const float fire_wave_sim_time = GetFireSpiritSimTimeSeconds();
    for (size_t i = 0; i < state_.fire_wave_segments.size(); ++i) {
        const FireWaveSegment& segment = state_.fire_wave_segments[i];
        if (!segment.alive) {
            continue;
        }
        const Vector2 center = GetFireWaveCenterWorld(segment, fire_wave_sim_time);
        if (!point_visible(center) && !point_visible(segment.origin_world)) {
            continue;
        }
        items.push_back({RenderKind::FireWaveSegment, center.y, i});
    }

    for (size_t i = 0; i < storm_arc_visuals_.size(); ++i) {
        if (!storm_arc_visuals_[i].alive) {
            continue;
        }
        const Vector2 mid = Vector2Lerp(storm_arc_visuals_[i].start_world, storm_arc_visuals_[i].end_world, 0.5f);
        if (!point_visible(mid)) {
            continue;
        }
        items.push_back({RenderKind::FireStormArc, mid.y, i});
    }

    for (size_t i = 0; i < state_.particles.size(); ++i) {
        if (!state_.particles[i].alive) {
            continue;
        }
        if (state_.particles[i].render_on_top) {
            continue;
        }
        if (!point_visible(state_.particles[i].pos)) {
            continue;
        }
        items.push_back({RenderKind::Particle, state_.particles[i].pos.y, i});
    }

    for (size_t i = 0; i < hammer_impact_effects_.size(); ++i) {
        if (!hammer_impact_effects_[i].alive) {
            continue;
        }
        if (!point_visible(hammer_impact_effects_[i].world_pos)) {
            continue;
        }
        items.push_back({RenderKind::HammerImpact, hammer_impact_effects_[i].world_pos.y, i});
    }

    for (size_t i = 0; i < state_.players.size(); ++i) {
        const Player& player = state_.players[i];
        const Vector2 draw_pos = GetRenderPlayerPosition(player.id);
        if (!point_visible(draw_pos)) {
            continue;
        }
        items.push_back({RenderKind::Player, draw_pos.y + 16.0f, i});
    }

    for (size_t i = 0; i < player_attached_animations_.size(); ++i) {
        const PlayerAttachedAnimation& animation = player_attached_animations_[i];
        const Player* player = FindPlayerById(animation.player_id);
        if (player == nullptr || !player->alive) {
            continue;
        }
        const Vector2 draw_pos = GetRenderPlayerPosition(player->id);
        if (!point_visible(draw_pos)) {
            continue;
        }
        items.push_back({RenderKind::PlayerAttachedAnimation, draw_pos.y + 16.0f, i});
    }

    std::sort(items.begin(), items.end(), [&](const RenderItem& a, const RenderItem& b) {
        const float dy = a.sort_y - b.sort_y;
        if (std::fabs(dy) > 0.001f) {
            return dy < 0.0f;
        }
        return kind_priority(a.kind) < kind_priority(b.kind);
    });

    const bool has_texture = sprite_metadata_.IsLoaded();
    const Texture2D texture = sprite_metadata_.GetTexture();
    const bool has_tall_texture = sprite_metadata_tall_.IsLoaded();
    const Texture2D tall_texture = sprite_metadata_tall_.GetTexture();
    const bool has_large_texture = sprite_metadata_96x96_.IsLoaded();
    const Texture2D large_texture = sprite_metadata_96x96_.GetTexture();
    const bool has_huge_texture = sprite_metadata_128x128_.IsLoaded();
    const Texture2D huge_texture = sprite_metadata_128x128_.GetTexture();

    const auto get_metadata = [&](CompositeEffectSheet sheet) -> const SpriteMetadataLoader* {
        switch (sheet) {
            case CompositeEffectSheet::Base32:
                return &sprite_metadata_;
            case CompositeEffectSheet::Tall32x64:
                return &sprite_metadata_tall_;
            case CompositeEffectSheet::Large96x96:
                return &sprite_metadata_96x96_;
        }
        return &sprite_metadata_;
    };
    const auto get_texture = [&](CompositeEffectSheet sheet) -> const Texture2D* {
        switch (sheet) {
            case CompositeEffectSheet::Base32:
                return has_texture ? &texture : nullptr;
            case CompositeEffectSheet::Tall32x64:
                return has_tall_texture ? &tall_texture : nullptr;
            case CompositeEffectSheet::Large96x96:
                return has_large_texture ? &large_texture : nullptr;
        }
        return nullptr;
    };

    for (const RenderItem& item : items) {
        if (!pass_matches(item)) {
            continue;
        }
        switch (item.kind) {
            case RenderKind::LegacyDecoration: {
                if (!has_tall_texture || state_.map.width <= 0) {
                    break;
                }
                const std::string& animation_name = state_.map.decorations[item.index];
                if (animation_name.empty() || !sprite_metadata_tall_.HasAnimation(animation_name)) {
                    break;
                }
                const int cell_x = static_cast<int>(item.index % static_cast<size_t>(state_.map.width));
                const int cell_y = static_cast<int>(item.index / static_cast<size_t>(state_.map.width));

                const float dst_w = static_cast<float>(sprite_metadata_tall_.GetCellWidth());
                const float dst_h = static_cast<float>(sprite_metadata_tall_.GetCellHeight());
                Rectangle dst = {static_cast<float>(cell_x * state_.map.cell_size),
                                 static_cast<float>(cell_y * state_.map.cell_size) -
                                     (dst_h - static_cast<float>(state_.map.cell_size)),
                                 dst_w, dst_h};
                dst = SnapRect(dst);

                const Rectangle src = InsetSourceRect(
                    sprite_metadata_tall_.GetFrame(animation_name, "default", render_time_seconds_),
                    Constants::kAtlasSampleInsetPixels);
                DrawTexturePro(tall_texture, src, dst, {0, 0}, 0.0f, WHITE);
                break;
            }
            case RenderKind::MapObject: {
                if (item.index >= state_.map_objects.size()) {
                    break;
                }
                const MapObjectInstance& object = state_.map_objects[item.index];
                const ObjectPrototype* proto = FindObjectPrototype(object.prototype_id);
                if (proto == nullptr) {
                    break;
                }
                if (RenderModularTreeObject(object, *proto, item.sort_y)) {
                    break;
                }

                const SpriteMetadataLoader* metadata = &sprite_metadata_;
                const Texture2D* draw_texture = has_texture ? &texture : nullptr;
                switch (proto->sprite_sheet) {
                    case SpriteSheetType::Base32:
                        metadata = &sprite_metadata_;
                        draw_texture = has_texture ? &texture : nullptr;
                        break;
                    case SpriteSheetType::Tall32x64:
                        metadata = &sprite_metadata_tall_;
                        draw_texture = has_tall_texture ? &tall_texture : nullptr;
                        break;
                    case SpriteSheetType::Large128x128:
                        metadata = &sprite_metadata_128x128_;
                        draw_texture = has_huge_texture ? &huge_texture : nullptr;
                        break;
                }
                if (object.prototype_id == "castle" && object.owner_team >= 0 &&
                    object.owner_team < static_cast<int>(sprite_sheet_128x128_team_variants_.size())) {
                    const Texture2D& variant =
                        sprite_sheet_128x128_team_variants_[static_cast<size_t>(object.owner_team)];
                    if (variant.id != 0) {
                        draw_texture = &variant;
                    }
                }
                const float base_dst_w =
                    static_cast<float>(metadata->GetCellWidth() > 0 ? metadata->GetCellWidth() : state_.map.cell_size);
                const float base_dst_h =
                    static_cast<float>(metadata->GetCellHeight() > 0 ? metadata->GetCellHeight() : state_.map.cell_size);
                const float visual_scale =
                    proto->type == ObjectType::Consumable ? Constants::kDroppedItemVisualScale : 1.0f;
                const Rectangle dst = GetMapObjectSpriteRect(object, proto, base_dst_w, base_dst_h, visual_scale);
                float anim_time = render_time_seconds_;
                const std::string animation = ResolveMapObjectAnimation(object, *proto, *metadata, &anim_time);

                if (draw_texture != nullptr && metadata->IsLoaded() && metadata->HasAnimation(animation)) {
                    const Rectangle src =
                        InsetSourceRect(metadata->GetFrame(animation, "default", anim_time), Constants::kAtlasSampleInsetPixels);
                    const float flash_amount = GetObjectDamageFlashAmount(object);
                    const bool use_damage_flash =
                        flash_amount > 0.0f && has_damage_flash_shader_ && !proto->masked_occluder;
                    if (!proto->masked_occluder || !DrawMaskedOccluder(dst, *draw_texture, src, item.sort_y)) {
                        if (use_damage_flash) {
                            SetShaderValue(damage_flash_shader_, damage_flash_amount_loc_, &flash_amount,
                                           SHADER_UNIFORM_FLOAT);
                            BeginShaderMode(damage_flash_shader_);
                        }
                        DrawWindAnimatedMapObject(object, *proto, *draw_texture, src, dst);
                        if (use_damage_flash) {
                            EndShaderMode();
                        }
                    }
                } else {
                    DrawRectangleRec(dst, Color{180, 120, 80, 220});
                    DrawRectangleLinesEx(dst, 1.0f, Color{30, 20, 12, 255});
                }
                break;
            }
            case RenderKind::CompositeEffectPart: {
                if (item.index >= state_.composite_effects.size()) {
                    break;
                }
                const CompositeEffectInstance& effect = state_.composite_effects[item.index];
                const CompositeEffectDefinition* definition = composite_effects_loader_.FindById(effect.effect_id);
                if (!effect.alive || definition == nullptr || item.sub_index >= definition->parts.size()) {
                    break;
                }
                const auto& part = definition->parts[item.sub_index];
                const SpriteMetadataLoader* metadata = get_metadata(part.sheet);
                const Texture2D* draw_texture = get_texture(part.sheet);
                if (metadata == nullptr || draw_texture == nullptr || !metadata->HasAnimation(part.animation)) {
                    break;
                }

                const float cell = static_cast<float>(state_.map.cell_size);
                const Vector2 draw_pos = {
                    effect.origin_world.x + part.offset_x_tiles * cell,
                    effect.origin_world.y + 0.5f * cell + part.offset_y_tiles * cell,
                };
                const Rectangle src =
                    InsetSourceRect(metadata->GetFrame(part.animation, "default", effect.age_seconds),
                                    Constants::kAtlasSampleInsetPixels);
                const float dst_w = static_cast<float>(metadata->GetCellWidth());
                const float dst_h = static_cast<float>(metadata->GetCellHeight());
                Rectangle dst = {draw_pos.x - dst_w * 0.5f, draw_pos.y - dst_h, dst_w, dst_h};
                DrawTexturePro(*draw_texture, src, SnapRect(dst), {0, 0}, 0.0f, WHITE);
                break;
            }
            case RenderKind::FireStormDummyPart: {
                if (item.index >= state_.fire_storm_dummies.size()) {
                    break;
                }
                const FireStormDummy& dummy = state_.fire_storm_dummies[item.index];
                if (!dummy.alive) {
                    break;
                }
                const char* effect_id = nullptr;
                if (item.aux_index == 1) {
                    effect_id = "fire_storm_lightning";
                } else {
                    effect_id = dummy.state == FireStormDummyState::Born
                                    ? "fire_storm_born"
                                    : (dummy.state == FireStormDummyState::Dying ? "fire_storm_death" : "fire_storm_idle");
                }
                const CompositeEffectDefinition* definition = composite_effects_loader_.FindById(effect_id);
                if (definition == nullptr || item.sub_index >= definition->parts.size()) {
                    break;
                }
                const auto& part = definition->parts[item.sub_index];
                if (!has_texture || !sprite_metadata_.HasAnimation(part.animation)) {
                    break;
                }
                const float cell = static_cast<float>(state_.map.cell_size);
                const Vector2 origin = CellToWorldCenter(dummy.cell);
                const Vector2 draw_pos = {
                    origin.x + part.offset_x_tiles * cell,
                    origin.y + 0.5f * cell + part.offset_y_tiles * cell,
                };
                const Rectangle src = InsetSourceRect(
                    sprite_metadata_.GetFrame(part.animation, "default", dummy.state_time),
                    Constants::kAtlasSampleInsetPixels);
                Rectangle dst = {draw_pos.x - 16.0f, draw_pos.y - 32.0f, 32.0f, 32.0f};
                DrawTexturePro(texture, src, SnapRect(dst), {0, 0}, 0.0f, WHITE);
                break;
            }
            case RenderKind::GrapplingHookSegment: {
                if (item.index >= state_.grappling_hooks.size() || !has_texture) {
                    break;
                }
                const GrapplingHook& hook = state_.grappling_hooks[item.index];
                const Player* owner = FindPlayerById(hook.owner_player_id);
                if (!hook.alive || owner == nullptr || !owner->alive) {
                    break;
                }

                const Vector2 start = Vector2Add(GetRenderPlayerPosition(owner->id),
                                                 {0.0f, 0.5f * static_cast<float>(state_.map.cell_size)});
                const Vector2 end =
                    (hook.phase == GrapplingHookPhase::Pulling)
                        ? hook.latch_point
                        : ((!network_manager_.IsHost() && hook.owner_player_id == state_.local_player_id)
                               ? hook.head_pos
                               : GetRenderGrapplingHookHeadPosition(hook.id, hook.head_pos));
                const Vector2 delta = Vector2Subtract(end, start);
                const float total_len = Vector2Length(delta);
                if (total_len <= 0.001f) {
                    break;
                }

                const int segments = std::max(2, static_cast<int>(item.aux_index));
                const int seg = static_cast<int>(item.sub_index);
                const float rotation = static_cast<float>(std::atan2(delta.y, delta.x) * RAD2DEG);
                const bool use_latched_animation =
                    hook.phase == GrapplingHookPhase::Pulling || hook.phase == GrapplingHookPhase::Retracting;
                const char* animation = nullptr;
                if (use_latched_animation) {
                    animation = "grappling_hook_latched";
                } else if (seg == segments - 1) {
                    animation = "grappling_hook_head";
                } else {
                    animation = "grappling_hook";
                }
                if (!sprite_metadata_.HasAnimation(animation)) {
                    break;
                }
                const Rectangle src = InsetSourceRect(
                    sprite_metadata_.GetFrame(animation, "default", hook.animation_time),
                    Constants::kAtlasSampleInsetPixels);
                const float seg_h = static_cast<float>(std::max(1, state_.map.cell_size));
                const float anchor_y = seg_h * Constants::kLightningAnchorYRatio;

                const float t0 = static_cast<float>(seg) / static_cast<float>(segments);
                const float t1 = static_cast<float>(seg + 1) / static_cast<float>(segments);
                const Vector2 p0 = Vector2Add(start, Vector2Scale(delta, t0));
                const Vector2 p1 = Vector2Add(start, Vector2Scale(delta, t1));
                const Vector2 seg_vec = Vector2Subtract(p1, p0);
                const float seg_len = std::max(1.0f, Vector2Length(seg_vec));
                Rectangle dst = {p0.x, p0.y - anchor_y, seg_len + 1.0f, seg_h};
                DrawTexturePro(texture, src, dst, {0.0f, anchor_y}, rotation, WHITE);
                break;
            }
            case RenderKind::StatusEffectPart: {
                if (item.index >= state_.players.size()) {
                    break;
                }
                const Player& player = state_.players[item.index];
                if (!player.alive || item.aux_index >= player.status_effects.size()) {
                    break;
                }
                const StatusEffectInstance& status = player.status_effects[item.aux_index];
                const CompositeEffectDefinition* definition = composite_effects_loader_.FindById(status.composite_effect_id);
                if (definition == nullptr || item.sub_index >= definition->parts.size()) {
                    break;
                }
                const auto& part = definition->parts[item.sub_index];
                if (!has_texture || !sprite_metadata_.HasAnimation(part.animation)) {
                    break;
                }
                const float cell = static_cast<float>(state_.map.cell_size);
                const Vector2 origin = GetRenderPlayerPosition(player.id);
                const Vector2 draw_pos = {
                    origin.x + part.offset_x_tiles * cell,
                    origin.y + 0.5f * cell + part.offset_y_tiles * cell,
                };
                const float animation_time = status.total_seconds - status.remaining_seconds;
                const Rectangle src = InsetSourceRect(
                    sprite_metadata_.GetFrame(part.animation, "default", animation_time),
                    Constants::kAtlasSampleInsetPixels);
                Rectangle dst = {draw_pos.x - 16.0f, draw_pos.y - 32.0f, 32.0f, 32.0f};
                DrawTexturePro(texture, src, SnapRect(dst), {0, 0}, 0.0f, WHITE);
                break;
            }
            case RenderKind::EarthRootsPart: {
                if (item.index >= state_.earth_roots_groups.size() || !has_texture) {
                    break;
                }
                const EarthRootsGroup& group = state_.earth_roots_groups[item.index];
                const int tile_index = static_cast<int>(item.aux_index);
                const int dx = tile_index % 3 - 1;
                const int dy = tile_index / 3 - 1;
                const GridCoord cell = {group.center_cell.x + dx, group.center_cell.y + dy};
                const Vector2 center = CellToWorldCenter(cell);
                const bool front = item.sub_index == 1;
                const char* animation_name = nullptr;
                float animation_time = 0.0f;
                switch (group.state) {
                    case EarthRootsGroupState::Born:
                        animation_name = front ? "roots_born_front" : "roots_born_back";
                        animation_time = group.state_time;
                        break;
                    case EarthRootsGroupState::Idle:
                        animation_name = front ? "roots_idle_front" : "roots_idle_back";
                        animation_time = 0.0f;
                        break;
                    case EarthRootsGroupState::Dying:
                        animation_name = front ? "roots_death_front" : "roots_death_back";
                        animation_time = group.state_time;
                        break;
                }
                if (animation_name == nullptr || !sprite_metadata_.HasAnimation(animation_name)) {
                    break;
                }
                const Rectangle src = InsetSourceRect(
                    sprite_metadata_.GetFrame(animation_name, "default", animation_time),
                    Constants::kAtlasSampleInsetPixels);
                Rectangle dst = {center.x - 16.0f, center.y - 16.0f, 32.0f, 32.0f};
                DrawTexturePro(texture, src, SnapRect(dst), {0, 0}, 0.0f, WHITE);
                break;
            }
            case RenderKind::Rune: {
                const Rune& rune = state_.runes[item.index];
                const Vector2 center = CellToWorldCenter(rune.cell);
                const bool fire_storm_rune =
                    rune.rune_type == RuneType::FireStormDummy || rune.fire_storm_original_rune_type != RuneType::None;
                const char* animation_name = fire_storm_rune
                                                 ? GetFireStormRuneAnimationKey(rune)
                                                 : (rune.active
                                                        ? (rune.rune_type == RuneType::Earth ? GetEarthRuneAnimationKey(rune)
                                                                                             : GetRuneSpriteAnimationKey(rune.rune_type))
                                                        : GetRuneBornSpriteAnimationKey(rune.rune_type));
                const float animation_time = fire_storm_rune
                                                 ? ((rune.fire_storm_visual_state == FireStormRuneVisualState::Idle ||
                                                     rune.fire_storm_visual_state == FireStormRuneVisualState::None)
                                                        ? (render_time_seconds_ + rune.placement_order * 0.05f)
                                                        : rune.fire_storm_visual_state_time)
                                                 : (rune.active
                                                        ? (rune.rune_type == RuneType::Earth
                                                               ? ((rune.earth_trap_state == EarthRuneTrapState::Slamming)
                                                                      ? (rune.earth_state_time / Constants::kEarthRuneSlamSlowdown)
                                                                      : rune.earth_state_time)
                                                               : (render_time_seconds_ + rune.placement_order * 0.05f))
                                                        : (rune.activation_total_seconds - rune.activation_remaining_seconds));

                const char* fallback_animation = rune.active ? "" : "rune_born";
                if (has_texture &&
                    (sprite_metadata_.HasAnimation(animation_name) ||
                     (fallback_animation[0] != '\0' && sprite_metadata_.HasAnimation(fallback_animation)))) {
                    if (fire_storm_rune) {
                        const char* background_animation = GetFireStormRuneBackgroundAnimationKey(rune);
                        if (sprite_metadata_.HasAnimation(background_animation)) {
                            const Rectangle bg_src = InsetSourceRect(
                                sprite_metadata_.GetFrame(background_animation, "default", animation_time),
                                Constants::kAtlasSampleInsetPixels);
                            Rectangle bg_dst = {center.x - 16.0f, center.y - 16.0f, 32.0f, 32.0f};
                            DrawTexturePro(texture, bg_src, SnapRect(bg_dst), {0, 0}, 0.0f, WHITE);
                        }
                        const char* ground_overlay_animation = GetFireStormRuneGroundOverlayAnimationKey(rune);
                        if (sprite_metadata_.HasAnimation(ground_overlay_animation)) {
                            const Rectangle ground_overlay_src = InsetSourceRect(
                                sprite_metadata_.GetFrame(ground_overlay_animation, "default", animation_time),
                                Constants::kAtlasSampleInsetPixels);
                            Rectangle ground_overlay_dst = {center.x - 16.0f, center.y - 16.0f, 32.0f, 32.0f};
                            DrawTexturePro(texture, ground_overlay_src, SnapRect(ground_overlay_dst), {0, 0}, 0.0f, WHITE);
                        }
                    }
                    if (!sprite_metadata_.HasAnimation(animation_name) && fallback_animation[0] != '\0') {
                        animation_name = fallback_animation;
                    }
                    const Rectangle src = InsetSourceRect(
                        sprite_metadata_.GetFrame(animation_name, "default", animation_time),
                        Constants::kAtlasSampleInsetPixels);
                    Rectangle dst = {center.x - 16.0f, center.y - 16.0f, 32.0f, 32.0f};
                    dst = SnapRect(dst);
                    DrawTexturePro(texture, src, dst, {0, 0}, 0.0f, WHITE);
                    if (fire_storm_rune) {
                        const char* top_animation = GetFireStormRuneTopAnimationKey(rune);
                        if (sprite_metadata_.HasAnimation(top_animation)) {
                            const Rectangle top_src = InsetSourceRect(
                                sprite_metadata_.GetFrame(top_animation, "default", animation_time),
                                Constants::kAtlasSampleInsetPixels);
                            DrawTexturePro(texture, top_src, dst, {0, 0}, 0.0f, WHITE);
                        }
                        const auto lightning_it = fire_storm_dummy_lightning_seconds_remaining_.find(rune.id);
                        if (rune.fire_storm_visual_state == FireStormRuneVisualState::Idle &&
                            lightning_it != fire_storm_dummy_lightning_seconds_remaining_.end() &&
                            lightning_it->second > 0.0f) {
                            if (sprite_metadata_.HasAnimation("fire_storm_bottom_lightning")) {
                                const Rectangle lightning_bottom_src = InsetSourceRect(
                                    sprite_metadata_.GetFrame("fire_storm_bottom_lightning", "default", animation_time),
                                    Constants::kAtlasSampleInsetPixels);
                                DrawTexturePro(texture, lightning_bottom_src, dst, {0, 0}, 0.0f, WHITE);
                            }
                            if (sprite_metadata_.HasAnimation("fire_storm_top_lightning")) {
                                const Rectangle lightning_top_src = InsetSourceRect(
                                    sprite_metadata_.GetFrame("fire_storm_top_lightning", "default", animation_time),
                                    Constants::kAtlasSampleInsetPixels);
                                DrawTexturePro(texture, lightning_top_src, dst, {0, 0}, 0.0f, WHITE);
                            }
                        }
                    }
                } else {
                    DrawCircleV(center, 8.0f, GetRuneFallbackColor(rune.rune_type));
                    DrawCircleLinesV(center, 8.0f, Color{20, 24, 32, 255});
                }
                break;
            }
            case RenderKind::IceWall: {
                const IceWallPiece& wall = state_.ice_walls[item.index];
                const Vector2 center = CellToWorldCenter(wall.cell);
                Rectangle dst = {center.x - 16.0f, center.y - 16.0f, 32.0f, 32.0f};
                dst = SnapRect(dst);

                const char* animation_name = "ice_wall_idle";
                float animation_time = render_time_seconds_;
                if (wall.state == IceWallState::Materializing) {
                    animation_name = "ice_wall_born";
                    animation_time = wall.state_time;
                } else if (wall.state == IceWallState::Active) {
                    animation_name = "ice_wall_death";
                    animation_time = GetIceWallDeathAnimationSampleTime(sprite_metadata_, wall.hp);
                } else if (wall.state == IceWallState::Dying) {
                    animation_name = "ice_wall_death";
                    animation_time = wall.state_time;
                }

                if (has_texture && sprite_metadata_.HasAnimation(animation_name)) {
                    const Rectangle src =
                        InsetSourceRect(sprite_metadata_.GetFrame(animation_name, "default", animation_time),
                                        Constants::kAtlasSampleInsetPixels);
                    DrawTexturePro(texture, src, dst, {0, 0}, 0.0f, WHITE);
                } else {
                    const Color fallback =
                        (wall.state == IceWallState::Dying) ? Color{140, 180, 230, 180} : Color{180, 220, 255, 255};
                    DrawRectangleRec(dst, fallback);
                    DrawRectangleLinesEx(dst, 1.0f, Color{60, 95, 140, 255});
                }
                break;
            }
            case RenderKind::LightningSegment: {
                if (item.index >= state_.lightning_effects.size()) {
                    break;
                }
                const LightningEffect& effect = state_.lightning_effects[item.index];
                if (!effect.alive) {
                    break;
                }
                const std::string& animation_key = effect.birthing   ? effect.born_animation_key
                                                   : effect.dying   ? effect.death_animation_key
                                                                    : effect.idle_animation_key;
                if (!has_texture || animation_key.empty() || !sprite_metadata_.HasAnimation(animation_key)) {
                    break;
                }

                const Vector2 delta = Vector2Subtract(effect.end, effect.start);
                const float total_len = Vector2Length(delta);
                if (total_len <= 0.001f) {
                    break;
                }
                const int segments = std::max(1, effect.segment_count);
                const size_t seg = std::min(item.sub_index, static_cast<size_t>(segments - 1));
                const float t0 = static_cast<float>(seg) / static_cast<float>(segments);
                const float t1 = static_cast<float>(seg + 1) / static_cast<float>(segments);
                const Vector2 p0 = Vector2Add(effect.start, Vector2Scale(delta, t0));
                const Vector2 p1 = Vector2Add(effect.start, Vector2Scale(delta, t1));
                const Vector2 seg_vec = Vector2Subtract(p1, p0);
                const float seg_len = std::max(1.0f, Vector2Length(seg_vec));
                const float rotation = static_cast<float>(std::atan2(delta.y, delta.x) * RAD2DEG);

                float anim_time = effect.birthing ? effect.born_elapsed : (effect.dying ? effect.death_elapsed : effect.elapsed);
                if (!effect.birthing && !effect.dying && seg < effect.segment_phase_offsets_seconds.size()) {
                    anim_time += effect.segment_phase_offsets_seconds[seg];
                }

                const Rectangle src =
                    InsetSourceRect(sprite_metadata_.GetFrame(animation_key, "default", anim_time),
                                    Constants::kAtlasSampleInsetPixels);
                const float seg_h = static_cast<float>(std::max(1, state_.map.cell_size));
                const float anchor_y = seg_h * Constants::kLightningAnchorYRatio;
                Rectangle dst = {p0.x, p0.y - anchor_y, seg_len, seg_h};
                DrawTexturePro(texture, src, dst, {0.0f, anchor_y}, rotation, WHITE);
                break;
            }
            case RenderKind::Projectile: {
                const Projectile& projectile = state_.projectiles[item.index];
                const bool use_large_static = projectile.animation_key == "fire_storm_static_large";
                const SpriteMetadataLoader* projectile_metadata =
                    use_large_static ? (has_large_texture ? &sprite_metadata_96x96_ : nullptr)
                                     : (has_texture ? &sprite_metadata_ : nullptr);
                const Texture2D* projectile_texture =
                    use_large_static ? (has_large_texture ? &large_texture : nullptr) : (has_texture ? &texture : nullptr);
                if (projectile_metadata != nullptr && projectile_texture != nullptr &&
                    projectile_metadata->HasAnimation(projectile.animation_key)) {
                    const Rectangle src = InsetSourceRect(
                        projectile_metadata->GetFrame(projectile.animation_key, "default", render_time_seconds_),
                        Constants::kAtlasSampleInsetPixels);
                    const float dst_w = use_large_static ? static_cast<float>(projectile_metadata->GetCellWidth())
                                                         : (IsIceShardProjectile(projectile)
                                                                ? static_cast<float>(projectile_metadata->GetCellWidth()) *
                                                                      Constants::kIceWaveShardScale
                                                                : projectile.radius * 8.0f);
                    const float dst_h = use_large_static ? static_cast<float>(projectile_metadata->GetCellHeight())
                                                         : (IsIceShardProjectile(projectile)
                                                                ? static_cast<float>(projectile_metadata->GetCellHeight()) *
                                                                      Constants::kIceWaveShardScale
                                                                : projectile.radius * 8.0f);
                    const Color tint =
                        (IsStaticFireBolt(projectile) || IsIceShardProjectile(projectile))
                            ? WHITE
                            : (projectile.owner_team == Constants::kTeamRed ? Color{255, 215, 215, 255}
                                                                            : Color{215, 230, 255, 255});
                    const Vector2 rotation_vel =
                        use_large_static && projectile.upgrade_pause_remaining > 0.0f ? projectile.resume_vel : projectile.vel;
                    const bool oriented_projectile = use_large_static || IsIceShardProjectile(projectile);
                    Rectangle dst = oriented_projectile
                                        ? Rectangle{projectile.pos.x, projectile.pos.y, dst_w, dst_h}
                                        : Rectangle{projectile.pos.x - dst_w * 0.5f, projectile.pos.y - dst_h * 0.5f,
                                                    dst_w, dst_h};
                    dst = SnapRect(dst);
                    const float rotation = oriented_projectile ? AimToDegrees(rotation_vel) : 0.0f;
                    const Vector2 origin =
                        oriented_projectile ? Vector2{dst_w * 0.5f, dst_h * 0.5f} : Vector2{0.0f, 0.0f};
                    DrawTexturePro(*projectile_texture, src, dst, origin, rotation, tint);
                } else {
                    DrawCircleV(projectile.pos, projectile.radius,
                                projectile.owner_team == Constants::kTeamRed ? Color{255, 92, 48, 255}
                                                                             : Color{80, 180, 255, 255});
                }
                break;
            }
            case RenderKind::FireWaveSegment: {
                if (item.index >= state_.fire_wave_segments.size() || !has_huge_texture ||
                    !sprite_metadata_128x128_.HasAnimation("fire_wave")) {
                    break;
                }
                const FireWaveSegment& segment = state_.fire_wave_segments[item.index];
                if (!segment.alive) {
                    break;
                }
                const float progress = GetFireWaveProgress(segment, GetFireSpiritSimTimeSeconds());
                Rectangle src = sprite_metadata_128x128_.GetFrame("fire_wave", "default", progress);
                int frame_count = 0;
                float fps = 1.0f;
                if (sprite_metadata_128x128_.GetAnimationStats("fire_wave", "default", frame_count, fps) &&
                    frame_count > 0) {
                    const int frame_index =
                        std::clamp(static_cast<int>(progress * static_cast<float>(frame_count)), 0, frame_count - 1);
                    src = sprite_metadata_128x128_.GetFrameByIndex("fire_wave", "default", frame_index);
                }
                src = InsetSourceRect(src, Constants::kAtlasSampleInsetPixels);
                const float dst_w = static_cast<float>(sprite_metadata_128x128_.GetCellWidth());
                const float dst_h = static_cast<float>(sprite_metadata_128x128_.GetCellHeight());
                Rectangle dst = {segment.origin_world.x, segment.origin_world.y, dst_w, dst_h};
                DrawTexturePro(huge_texture, src, SnapRect(dst), {0.0f, dst_h * 0.5f},
                               segment.direction_radians * RAD2DEG, WHITE);
                break;
            }
            case RenderKind::FireSpiritIdle: {
                if (item.index >= state_.fire_spirits.size() || !has_texture) {
                    break;
                }
                const FireSpirit& spirit = state_.fire_spirits[item.index];
                if (!spirit.alive || spirit.state != FireSpiritState::Idle ||
                    !sprite_metadata_.HasAnimation("fire_spirit_idle")) {
                    break;
                }
                const Rectangle src =
                    InsetSourceRect(sprite_metadata_.GetFrame("fire_spirit_idle", "default", render_time_seconds_),
                                    Constants::kAtlasSampleInsetPixels);
                Rectangle dst = {spirit.pos.x - 16.0f, spirit.pos.y - 16.0f, 32.0f, 32.0f};
                DrawTexturePro(texture, src, SnapRect(dst), {0, 0}, 0.0f, WHITE);
                break;
            }
            case RenderKind::FireSpiritTrail: {
                if (item.index >= state_.fire_spirits.size() || !has_texture) {
                    break;
                }
                const FireSpirit& spirit = state_.fire_spirits[item.index];
                if (!spirit.alive || spirit.state != FireSpiritState::Projectile ||
                    item.sub_index >= spirit.trail_samples.size() || item.sub_index == 0 ||
                    !sprite_metadata_.HasAnimation("fire_trail")) {
                    break;
                }
                const FireSpiritTrailSample& a = spirit.trail_samples[item.sub_index - 1];
                const FireSpiritTrailSample& b = spirit.trail_samples[item.sub_index];
                Vector2 dir = Vector2Subtract(b.world, a.world);
                const float len = Vector2Length(dir);
                if (len <= 0.001f) {
                    break;
                }
                dir = Vector2Scale(dir, 1.0f / len);
                const Vector2 normal = {-dir.y, dir.x};
                const float width_a = Constants::kFireSpiritTrailHalfWidth *
                                      std::clamp(1.0f - a.age_seconds / Constants::kFireSpiritTrailLifetimeSeconds,
                                                 0.0f, 1.0f);
                const float width_b = Constants::kFireSpiritTrailHalfWidth *
                                      std::clamp(1.0f - b.age_seconds / Constants::kFireSpiritTrailLifetimeSeconds,
                                                 0.0f, 1.0f);
                if (width_a <= 0.01f && width_b <= 0.01f) {
                    break;
                }
                const Vector2 v0 = Vector2Add(a.world, Vector2Scale(normal, width_a));
                const Vector2 v1 = Vector2Subtract(a.world, Vector2Scale(normal, width_a));
                const Vector2 v2 = Vector2Subtract(b.world, Vector2Scale(normal, width_b));
                const Vector2 v3 = Vector2Add(b.world, Vector2Scale(normal, width_b));
                float trail_frame_time = render_time_seconds_;
                switch (Constants::kFireSpiritTrailAnimationMode) {
                    case Constants::TrailAnimationMode::Static:
                        trail_frame_time = 0.0f;
                        break;
                    case Constants::TrailAnimationMode::PerSegmentAnimated:
                        trail_frame_time = b.age_seconds;
                        break;
                    case Constants::TrailAnimationMode::WidthDriven:
                        trail_frame_time = 0.0f;
                        break;
                    case Constants::TrailAnimationMode::GlobalAnimated:
                    default:
                        trail_frame_time = render_time_seconds_;
                        break;
                }
                Rectangle src = {};
                if (Constants::kFireSpiritTrailAnimationMode == Constants::TrailAnimationMode::WidthDriven) {
                    int trail_frame_count = 0;
                    float trail_fps_unused = 0.0f;
                    if (sprite_metadata_.GetAnimationStats("fire_trail", "default", trail_frame_count, trail_fps_unused) &&
                        trail_frame_count > 0) {
                        const float average_width_ratio =
                            std::clamp((width_a + width_b) / (2.0f * Constants::kFireSpiritTrailHalfWidth), 0.0f, 1.0f);
                        const float progress = 1.0f - average_width_ratio;
                        const int frame_index = std::clamp(
                            static_cast<int>(progress * static_cast<float>(trail_frame_count)), 0, trail_frame_count - 1);
                        src = InsetSourceRect(sprite_metadata_.GetFrameByIndex("fire_trail", "default", frame_index),
                                              Constants::kAtlasSampleInsetPixels);
                    } else {
                        src = InsetSourceRect(sprite_metadata_.GetFrame("fire_trail", "default", 0.0f),
                                              Constants::kAtlasSampleInsetPixels);
                    }
                } else {
                    src = InsetSourceRect(sprite_metadata_.GetFrame("fire_trail", "default", trail_frame_time),
                                          Constants::kAtlasSampleInsetPixels);
                }
                const float u0 = src.x / static_cast<float>(texture.width);
                const float v_top = src.y / static_cast<float>(texture.height);
                const float u1 = (src.x + src.width) / static_cast<float>(texture.width);
                const float v_bottom = (src.y + src.height) / static_cast<float>(texture.height);
                rlSetTexture(texture.id);
                rlBegin(RL_QUADS);
                rlColor4ub(255, 255, 255, 255);
                rlTexCoord2f(u0, v_top);
                rlVertex2f(v0.x, v0.y);
                rlTexCoord2f(u0, v_bottom);
                rlVertex2f(v1.x, v1.y);
                rlTexCoord2f(u1, v_bottom);
                rlVertex2f(v2.x, v2.y);
                rlTexCoord2f(u1, v_top);
                rlVertex2f(v3.x, v3.y);
                rlEnd();
                rlSetTexture(0);
                break;
            }
            case RenderKind::FireSpiritProjectile: {
                if (item.index >= state_.fire_spirits.size() || !has_texture) {
                    break;
                }
                const FireSpirit& spirit = state_.fire_spirits[item.index];
                if (!spirit.alive || spirit.state != FireSpiritState::Projectile ||
                    !sprite_metadata_.HasAnimation("fire_spirit_projectile")) {
                    break;
                }
                const Vector2 render_world = EvaluateFireSpiritRenderWorld(spirit, GetFireSpiritSimTimeSeconds());
                Vector2 travel_dir = Vector2Subtract(spirit.impact_world, spirit.launch_world);
                if (Vector2LengthSqr(travel_dir) <= 0.0001f) {
                    travel_dir = {1.0f, 0.0f};
                }
                const Rectangle src =
                    InsetSourceRect(sprite_metadata_.GetFrame("fire_spirit_projectile", "default",
                                                              spirit.projectile_animation_time),
                                    Constants::kAtlasSampleInsetPixels);
                Rectangle dst = {render_world.x, render_world.y, 32.0f, 32.0f};
                DrawTexturePro(texture, src, SnapRect(dst), {16.0f, 16.0f}, AimToDegrees(travel_dir), WHITE);
                break;
            }
            case RenderKind::FireStormArc: {
                const StormArcVisual& arc = storm_arc_visuals_[item.index];
                if (!arc.alive || !has_texture || !sprite_metadata_.HasAnimation("storm_particle_idle")) {
                    break;
                }
                const float t = std::clamp(arc.elapsed_seconds / std::max(0.001f, arc.duration_seconds), 0.0f, 1.0f);
                Vector2 world_pos = Vector2Lerp(arc.start_world, arc.end_world, t);
                world_pos.y -= EvaluateStormArcHeight(t);
                const Rectangle src = InsetSourceRect(
                    sprite_metadata_.GetFrame("storm_particle_idle", "default", arc.elapsed_seconds),
                    Constants::kAtlasSampleInsetPixels);
                const float size = Constants::kFireStormStormProjectileSize;
                Rectangle dst = {world_pos.x, world_pos.y, size, size};
                DrawTexturePro(texture, src, SnapRect(dst), {size * 0.5f, size * 0.5f}, arc.rotation_degrees, WHITE);
                break;
            }
            case RenderKind::Particle: {
                const Particle& particle = state_.particles[item.index];
                RenderParticleInstance(particle);
                break;
            }
            case RenderKind::HammerImpact: {
                if (item.index >= hammer_impact_effects_.size() || !has_huge_texture ||
                    !sprite_metadata_128x128_.HasAnimation("hammer_impact")) {
                    break;
                }
                const HammerImpactEffect& effect = hammer_impact_effects_[item.index];
                if (!effect.alive) {
                    break;
                }
                const Rectangle src =
                    InsetSourceRect(sprite_metadata_128x128_.GetFrame("hammer_impact", "default", effect.age_seconds),
                                    Constants::kAtlasSampleInsetPixels);
                const float dst_w = static_cast<float>(sprite_metadata_128x128_.GetCellWidth());
                const float dst_h = static_cast<float>(sprite_metadata_128x128_.GetCellHeight());
                Rectangle dst = {effect.world_pos.x - dst_w * 0.5f, effect.world_pos.y - dst_h * 0.5f, dst_w, dst_h};
                DrawTexturePro(huge_texture, src, SnapRect(dst), {0, 0}, 0.0f, WHITE);
                break;
            }
            case RenderKind::Player: {
                const Player& player = state_.players[item.index];
                const Vector2 draw_pos = GetRenderPlayerPosition(player.id);
                if (!player.alive) {
                    DrawRectangleRec(GetPlayerCollisionRect(draw_pos), Color{70, 70, 70, 180});
                    break;
                }

                if (modular_player_asset_.HasTag("main", ResolvePlayerModularTag(player))) {
                    RenderPlayerModularLayers(player, draw_pos);
                } else {
                    DrawRectangleRec(GetPlayerCollisionRect(draw_pos), BLUE);
                }
                break;
            }
            case RenderKind::PlayerAttachedAnimation: {
                if (item.index >= player_attached_animations_.size()) {
                    break;
                }
                const PlayerAttachedAnimation& animation = player_attached_animations_[item.index];
                const Player* player = FindPlayerById(animation.player_id);
                if (player == nullptr || !player->alive) {
                    break;
                }
                RenderPlayerAttachedAnimation(animation.animation_key, animation.age_seconds, animation.offset_x,
                                              animation.offset_y, GetRenderPlayerPosition(player->id));
                break;
            }
        }
    }

    if (pass == DepthSortedRenderPass::OverInfluenceOverlay && has_texture) {
        const float fire_spirit_sim_time = GetFireSpiritSimTimeSeconds();
        if (!Constants::kFireSpiritTrailEnableLowResRender) {
            RenderFireSpiritTrailGeometry(show_network_debug_panel_);
        }
        for (const auto& spirit : state_.fire_spirits) {
            if (!spirit.alive) {
                continue;
            }
            const bool render_dead_trail =
                Constants::kFireSpiritTrailEnableDeadLinger && spirit.state == FireSpiritState::Dead &&
                !spirit.trail_samples.empty();
            if (spirit.state == FireSpiritState::Idle) {
                if (!point_visible(spirit.pos) || !sprite_metadata_.HasAnimation("fire_spirit_idle")) {
                    continue;
                }
                const Rectangle src =
                    InsetSourceRect(sprite_metadata_.GetFrame("fire_spirit_idle", "default", render_time_seconds_),
                                    Constants::kAtlasSampleInsetPixels);
                Rectangle dst = {spirit.pos.x - 16.0f, spirit.pos.y - 16.0f, 32.0f, 32.0f};
                DrawTexturePro(texture, src, SnapRect(dst), {0, 0}, 0.0f, WHITE);
                continue;
            }
            if (spirit.state != FireSpiritState::Projectile && !render_dead_trail) {
                continue;
            }

            const Vector2 render_world = EvaluateFireSpiritRenderWorld(spirit, fire_spirit_sim_time);
            if (!point_visible(render_world)) {
                continue;
            }

            if (render_dead_trail || !sprite_metadata_.HasAnimation("fire_spirit_projectile")) {
                continue;
            }
            const Rectangle src =
                InsetSourceRect(sprite_metadata_.GetFrame("fire_spirit_projectile", "default",
                                                          spirit.projectile_animation_time),
                                Constants::kAtlasSampleInsetPixels);
            Vector2 travel_dir = Vector2Subtract(spirit.impact_world, spirit.launch_world);
            if (Vector2LengthSqr(travel_dir) <= 0.0001f) {
                travel_dir = {1.0f, 0.0f};
            }
            Rectangle dst = {render_world.x, render_world.y, 32.0f, 32.0f};
            DrawTexturePro(texture, src, SnapRect(dst), {16.0f, 16.0f}, AimToDegrees(travel_dir), WHITE);
        }
    }

}

void GameApp::RenderMap() {
    if (state_.map.width <= 0 || state_.map.height <= 0) {
        return;
    }
    int min_x = 0;
    int min_y = 0;
    int max_x = state_.map.width - 1;
    int max_y = state_.map.height - 1;
    GetVisibleCellBounds(1, &min_x, &min_y, &max_x, &max_y);

    const bool has_texture = sprite_metadata_.IsLoaded();
    const Texture2D texture = sprite_metadata_.GetTexture();
    const bool has_grass_dual_grid = has_texture && sprite_metadata_.HasDualGridAnimation("tile_grass");
    const bool has_water_animation = has_texture && sprite_metadata_.HasAnimation("tile_water");
    const bool has_stone_dual_grid = has_texture && sprite_metadata_.HasDualGridAnimation("stone_tiles");
    const bool has_stone_animation = has_texture && sprite_metadata_.HasAnimation("stone_tiles");
    if (has_water_gradient_shader_) {
        const float screen_height = static_cast<float>(GetScreenHeight());
        const float gradient_start[4] = {
            static_cast<float>(Constants::kWaterOverlayStartR) / 255.0f,
            static_cast<float>(Constants::kWaterOverlayStartG) / 255.0f,
            static_cast<float>(Constants::kWaterOverlayStartB) / 255.0f,
            static_cast<float>(Constants::kWaterOverlayStartA) / 255.0f,
        };
        const float gradient_end[4] = {
            static_cast<float>(Constants::kWaterOverlayEndR) / 255.0f,
            static_cast<float>(Constants::kWaterOverlayEndG) / 255.0f,
            static_cast<float>(Constants::kWaterOverlayEndB) / 255.0f,
            static_cast<float>(Constants::kWaterOverlayEndA) / 255.0f,
        };
        SetShaderValue(water_gradient_shader_, water_gradient_screen_height_loc_, &screen_height, SHADER_UNIFORM_FLOAT);
        SetShaderValueV(water_gradient_shader_, water_gradient_start_loc_, gradient_start, SHADER_UNIFORM_VEC4, 1);
        SetShaderValueV(water_gradient_shader_, water_gradient_end_loc_, gradient_end, SHADER_UNIFORM_VEC4, 1);
        BeginShaderMode(water_gradient_shader_);
    }
    const auto is_grass_family = [](TileType tile) {
        return tile == TileType::Grass || tile == TileType::SpawnPoint;
    };

    const auto sample_grass_for_dual_grid = [&](int x, int y) {
        if (state_.map.width <= 0 || state_.map.height <= 0) {
            return false;
        }
        x = std::clamp(x, 0, state_.map.width - 1);
        y = std::clamp(y, 0, state_.map.height - 1);
        return is_grass_family(state_.map.tiles[static_cast<size_t>(y * state_.map.width + x)]);
    };
    const auto sample_stone_for_dual_grid = [&](int x, int y) {
        if (state_.map.width <= 0 || state_.map.height <= 0) {
            return false;
        }
        x = std::clamp(x, 0, state_.map.width - 1);
        y = std::clamp(y, 0, state_.map.height - 1);
        return state_.map.tiles[static_cast<size_t>(y * state_.map.width + x)] == TileType::StoneTiles;
    };

    const auto compute_grass_dual_grid_mask = [&](int x, int y) {
        int mask = 0;
        if (sample_grass_for_dual_grid(x, y)) {
            mask |= 1;  // TL
        }
        if (sample_grass_for_dual_grid(x + 1, y)) {
            mask |= 2;  // TR
        }
        if (sample_grass_for_dual_grid(x + 1, y + 1)) {
            mask |= 4;  // BR
        }
        if (sample_grass_for_dual_grid(x, y + 1)) {
            mask |= 8;  // BL
        }
        return mask;
    };
    const auto compute_stone_dual_grid_mask = [&](int x, int y) {
        int mask = 0;
        if (sample_stone_for_dual_grid(x, y)) {
            mask |= 1;  // TL
        }
        if (sample_stone_for_dual_grid(x + 1, y)) {
            mask |= 2;  // TR
        }
        if (sample_stone_for_dual_grid(x + 1, y + 1)) {
            mask |= 4;  // BR
        }
        if (sample_stone_for_dual_grid(x, y + 1)) {
            mask |= 8;  // BL
        }
        return mask;
    };
    const auto sample_tile_clamped = [&](int x, int y) {
        x = std::clamp(x, 0, state_.map.width - 1);
        y = std::clamp(y, 0, state_.map.height - 1);
        return state_.map.tiles[static_cast<size_t>(y * state_.map.width + x)];
    };
    const auto compute_dual_grid_base_tile = [&](int x, int y) {
        bool has_water = false;
        bool has_stone = false;
        for (const GridCoord corner : {GridCoord{x, y}, GridCoord{x + 1, y}, GridCoord{x + 1, y + 1}, GridCoord{x, y + 1}}) {
            const TileType corner_tile = sample_tile_clamped(corner.x, corner.y);
            has_water = has_water || corner_tile == TileType::Water;
            has_stone = has_stone || corner_tile == TileType::StoneTiles;
        }
        if (has_stone && !has_water) {
            return TileType::StoneTiles;
        }
        return TileType::Water;
    };

    const auto compute_grass_bitmask = [&](int x, int y) {
        int mask = 0;
        const GridCoord north = {x, y - 1};
        const GridCoord west = {x - 1, y};
        const GridCoord east = {x + 1, y};
        const GridCoord south = {x, y + 1};
        const GridCoord north_west = {x - 1, y - 1};
        const GridCoord north_east = {x + 1, y - 1};
        const GridCoord south_west = {x - 1, y + 1};
        const GridCoord south_east = {x + 1, y + 1};

        const bool has_n = state_.map.IsInside(north) && is_grass_family(state_.map.GetTile(north));
        const bool has_w = state_.map.IsInside(west) && is_grass_family(state_.map.GetTile(west));
        const bool has_e = state_.map.IsInside(east) && is_grass_family(state_.map.GetTile(east));
        const bool has_s = state_.map.IsInside(south) && is_grass_family(state_.map.GetTile(south));

        // 8-bit layout:
        // NW=1, N=2, NE=4, W=8, E=16, SW=32, S=64, SE=128
        if (has_n) {
            mask |= 2;
        }
        if (has_w) {
            mask |= 8;
        }
        if (has_e) {
            mask |= 16;
        }
        if (has_s) {
            mask |= 64;
        }

        // Corner bits only count when both adjacent cardinals are present.
        if (has_n && has_w && state_.map.IsInside(north_west) && is_grass_family(state_.map.GetTile(north_west))) {
            mask |= 1;
        }
        if (has_n && has_e && state_.map.IsInside(north_east) && is_grass_family(state_.map.GetTile(north_east))) {
            mask |= 4;
        }
        if (has_s && has_w && state_.map.IsInside(south_west) && is_grass_family(state_.map.GetTile(south_west))) {
            mask |= 32;
        }
        if (has_s && has_e && state_.map.IsInside(south_east) && is_grass_family(state_.map.GetTile(south_east))) {
            mask |= 128;
        }

        return mask;
    };

    for (int y = min_y; y <= max_y; ++y) {
        for (int x = min_x; x <= max_x; ++x) {
            const TileType tile = state_.map.tiles[static_cast<size_t>(y * state_.map.width + x)];
            const Rectangle dst = SnapRect(
                {static_cast<float>(x * state_.map.cell_size), static_cast<float>(y * state_.map.cell_size),
                 static_cast<float>(state_.map.cell_size), static_cast<float>(state_.map.cell_size)});

            if (has_texture && has_grass_dual_grid) {
                const float half_cell = static_cast<float>(state_.map.cell_size) * 0.5f;
                const Rectangle terrain_dst =
                    SnapRect({dst.x + half_cell, dst.y + half_cell, dst.width, dst.height});

                const TileType base_tile = compute_dual_grid_base_tile(x, y);
                const int mask = (base_tile == TileType::StoneTiles) ? compute_stone_dual_grid_mask(x, y)
                                                                     : compute_grass_dual_grid_mask(x, y);
                Rectangle src = {};
                if (base_tile == TileType::StoneTiles && has_stone_dual_grid &&
                    sprite_metadata_.GetDualGridFrame("stone_tiles", mask, render_time_seconds_,
                                                      SpriteFrameLayer::Background, src)) {
                    DrawTexturePro(texture, InsetSourceRect(src, Constants::kAtlasSampleInsetPixels), terrain_dst, {0, 0},
                                   0.0f, WHITE);
                } else if (sprite_metadata_.GetDualGridFrame("tile_grass", mask, render_time_seconds_,
                                                             SpriteFrameLayer::Water, src)) {
                    DrawTexturePro(texture, InsetSourceRect(src, Constants::kAtlasSampleInsetPixels), terrain_dst, {0, 0},
                                   0.0f, WHITE);
                } else if (tile == TileType::StoneTiles && has_stone_animation) {
                    const Rectangle stone_src = InsetSourceRect(
                        sprite_metadata_.GetFrame("stone_tiles", "default", render_time_seconds_),
                        Constants::kAtlasSampleInsetPixels);
                    DrawTexturePro(texture, stone_src, dst, {0, 0}, 0.0f, WHITE);
                } else if (has_water_animation) {
                    const Rectangle water_src = InsetSourceRect(
                        sprite_metadata_.GetFrame("tile_water", "default", render_time_seconds_),
                        Constants::kAtlasSampleInsetPixels);
                    DrawTexturePro(texture, water_src, terrain_dst, {0, 0}, 0.0f, WHITE);
                } else if (tile == TileType::StoneTiles) {
                    DrawRectangleRec(terrain_dst, Color{124, 120, 112, 255});
                } else {
                    DrawRectangleRec(terrain_dst, Color{26, 96, 152, 255});
                }
            } else if (has_texture && tile == TileType::Water) {
                const Rectangle src = InsetSourceRect(
                    sprite_metadata_.GetFrame("tile_water", "default", render_time_seconds_),
                    Constants::kAtlasSampleInsetPixels);
                DrawTexturePro(texture, src, dst, {0, 0}, 0.0f, WHITE);
            } else if (has_texture && tile == TileType::StoneTiles && has_stone_animation) {
                const Rectangle src = InsetSourceRect(
                    sprite_metadata_.GetFrame("stone_tiles", "default", render_time_seconds_),
                    Constants::kAtlasSampleInsetPixels);
                DrawTexturePro(texture, src, dst, {0, 0}, 0.0f, WHITE);
            } else {
                if (tile == TileType::Water) {
                    DrawRectangleRec(dst, Color{26, 96, 152, 255});
                } else if (tile == TileType::StoneTiles) {
                    DrawRectangleRec(dst, Color{124, 120, 112, 255});
                }
            }
        }
    }

    if (has_water_gradient_shader_) {
        EndShaderMode();
    }

}

void GameApp::RenderMapForeground() {
    if (state_.map.width <= 0 || state_.map.height <= 0) {
        return;
    }
    int min_x = 0;
    int min_y = 0;
    int max_x = state_.map.width - 1;
    int max_y = state_.map.height - 1;
    GetVisibleCellBounds(2, &min_x, &min_y, &max_x, &max_y);

    const bool has_texture = sprite_metadata_.IsLoaded();
    const Texture2D texture = sprite_metadata_.GetTexture();
    const bool has_tall_texture = sprite_metadata_tall_.IsLoaded();
    const Texture2D tall_texture = sprite_metadata_tall_.GetTexture();
    const bool has_large_texture = sprite_metadata_96x96_.IsLoaded();
    const Texture2D large_texture = sprite_metadata_96x96_.GetTexture();
    const auto get_metadata = [&](CompositeEffectSheet sheet) -> const SpriteMetadataLoader* {
        switch (sheet) {
            case CompositeEffectSheet::Base32:
                return &sprite_metadata_;
            case CompositeEffectSheet::Tall32x64:
                return &sprite_metadata_tall_;
            case CompositeEffectSheet::Large96x96:
                return &sprite_metadata_96x96_;
        }
        return &sprite_metadata_;
    };
    const auto get_texture = [&](CompositeEffectSheet sheet) -> const Texture2D* {
        switch (sheet) {
            case CompositeEffectSheet::Base32:
                return has_texture ? &texture : nullptr;
            case CompositeEffectSheet::Tall32x64:
                return has_tall_texture ? &tall_texture : nullptr;
            case CompositeEffectSheet::Large96x96:
                return has_large_texture ? &large_texture : nullptr;
        }
        return nullptr;
    };
    const bool has_grass_dual_grid = has_texture && sprite_metadata_.HasDualGridAnimation("tile_grass");
    const bool has_terrain_foam_dual_grid = has_texture && sprite_metadata_.HasDualGridAnimation("terrain_foam");
    const bool has_grass_bitmask = has_texture && sprite_metadata_.HasBitmaskAnimation("tile_grass");
    const bool has_stone_dual_grid = has_texture && sprite_metadata_.HasDualGridAnimation("stone_tiles");
    const auto is_grass_family = [](TileType tile) {
        return tile == TileType::Grass || tile == TileType::SpawnPoint;
    };
    const auto sample_grass_for_dual_grid = [&](int x, int y) {
        if (state_.map.width <= 0 || state_.map.height <= 0) {
            return false;
        }
        x = std::clamp(x, 0, state_.map.width - 1);
        y = std::clamp(y, 0, state_.map.height - 1);
        return is_grass_family(state_.map.tiles[static_cast<size_t>(y * state_.map.width + x)]);
    };
    const auto sample_stone_for_dual_grid = [&](int x, int y) {
        if (state_.map.width <= 0 || state_.map.height <= 0) {
            return false;
        }
        x = std::clamp(x, 0, state_.map.width - 1);
        y = std::clamp(y, 0, state_.map.height - 1);
        return state_.map.tiles[static_cast<size_t>(y * state_.map.width + x)] == TileType::StoneTiles;
    };
    const auto compute_grass_dual_grid_mask = [&](int x, int y) {
        int mask = 0;
        if (sample_grass_for_dual_grid(x, y)) {
            mask |= 1;
        }
        if (sample_grass_for_dual_grid(x + 1, y)) {
            mask |= 2;
        }
        if (sample_grass_for_dual_grid(x + 1, y + 1)) {
            mask |= 4;
        }
        if (sample_grass_for_dual_grid(x, y + 1)) {
            mask |= 8;
        }
        return mask;
    };
    const auto compute_stone_dual_grid_mask = [&](int x, int y) {
        int mask = 0;
        if (sample_stone_for_dual_grid(x, y)) {
            mask |= 1;
        }
        if (sample_stone_for_dual_grid(x + 1, y)) {
            mask |= 2;
        }
        if (sample_stone_for_dual_grid(x + 1, y + 1)) {
            mask |= 4;
        }
        if (sample_stone_for_dual_grid(x, y + 1)) {
            mask |= 8;
        }
        return mask;
    };
    const auto sample_tile_clamped = [&](int x, int y) {
        x = std::clamp(x, 0, state_.map.width - 1);
        y = std::clamp(y, 0, state_.map.height - 1);
        return state_.map.tiles[static_cast<size_t>(y * state_.map.width + x)];
    };
    const auto compute_dual_grid_base_tile = [&](int x, int y) {
        bool has_water = false;
        bool has_stone = false;
        for (const GridCoord corner : {GridCoord{x, y}, GridCoord{x + 1, y}, GridCoord{x + 1, y + 1}, GridCoord{x, y + 1}}) {
            const TileType corner_tile = sample_tile_clamped(corner.x, corner.y);
            has_water = has_water || corner_tile == TileType::Water;
            has_stone = has_stone || corner_tile == TileType::StoneTiles;
        }
        if (has_stone && !has_water) {
            return TileType::StoneTiles;
        }
        return TileType::Water;
    };
    const auto compute_grass_bitmask = [&](int x, int y) {
        int mask = 0;
        const GridCoord north = {x, y - 1};
        const GridCoord west = {x - 1, y};
        const GridCoord east = {x + 1, y};
        const GridCoord south = {x, y + 1};
        const GridCoord north_west = {x - 1, y - 1};
        const GridCoord north_east = {x + 1, y - 1};
        const GridCoord south_west = {x - 1, y + 1};
        const GridCoord south_east = {x + 1, y + 1};

        const bool has_n = state_.map.IsInside(north) && is_grass_family(state_.map.GetTile(north));
        const bool has_w = state_.map.IsInside(west) && is_grass_family(state_.map.GetTile(west));
        const bool has_e = state_.map.IsInside(east) && is_grass_family(state_.map.GetTile(east));
        const bool has_s = state_.map.IsInside(south) && is_grass_family(state_.map.GetTile(south));

        if (has_n) mask |= 2;
        if (has_w) mask |= 8;
        if (has_e) mask |= 16;
        if (has_s) mask |= 64;
        if (has_n && has_w && state_.map.IsInside(north_west) && is_grass_family(state_.map.GetTile(north_west))) mask |= 1;
        if (has_n && has_e && state_.map.IsInside(north_east) && is_grass_family(state_.map.GetTile(north_east))) mask |= 4;
        if (has_s && has_w && state_.map.IsInside(south_west) && is_grass_family(state_.map.GetTile(south_west))) mask |= 32;
        if (has_s && has_e && state_.map.IsInside(south_east) && is_grass_family(state_.map.GetTile(south_east))) mask |= 128;
        return mask;
    };

    for (int y = min_y; y <= max_y; ++y) {
        for (int x = min_x; x <= max_x; ++x) {
            const TileType tile = state_.map.tiles[static_cast<size_t>(y * state_.map.width + x)];
            const Rectangle dst = SnapRect(
                {static_cast<float>(x * state_.map.cell_size), static_cast<float>(y * state_.map.cell_size),
                 static_cast<float>(state_.map.cell_size), static_cast<float>(state_.map.cell_size)});
            if (has_texture && has_grass_dual_grid) {
                const float half_cell = static_cast<float>(state_.map.cell_size) * 0.5f;
                const Rectangle terrain_dst =
                    SnapRect({dst.x + half_cell, dst.y + half_cell, dst.width, dst.height});
                const TileType base_tile = compute_dual_grid_base_tile(x, y);
                const bool use_stone_transition = base_tile == TileType::StoneTiles && has_stone_dual_grid;
                const int mask = use_stone_transition ? compute_stone_dual_grid_mask(x, y)
                                                      : compute_grass_dual_grid_mask(x, y);
                const bool has_land =
                    !use_stone_transition && sprite_metadata_.HasDualGridLayer("tile_grass", mask, SpriteFrameLayer::Land);

                Rectangle src = {};
                if (use_stone_transition &&
                    sprite_metadata_.GetDualGridFrame("stone_tiles", mask, render_time_seconds_,
                                                      SpriteFrameLayer::Foreground, src)) {
                    DrawTexturePro(texture, InsetSourceRect(src, Constants::kAtlasSampleInsetPixels), terrain_dst, {0, 0},
                                   0.0f, WHITE);
                }
                if (has_land &&
                    sprite_metadata_.GetDualGridFrame("tile_grass", mask, render_time_seconds_, SpriteFrameLayer::Land,
                                                      src)) {
                    DrawTexturePro(texture, InsetSourceRect(src, Constants::kAtlasSampleInsetPixels), terrain_dst, {0, 0},
                                   0.0f, WHITE);
                }
                if (!use_stone_transition && !has_land &&
                    sprite_metadata_.GetDualGridFrame("tile_grass", mask, render_time_seconds_, SpriteFrameLayer::Single,
                                                      src)) {
                    DrawTexturePro(texture, InsetSourceRect(src, Constants::kAtlasSampleInsetPixels), terrain_dst, {0, 0},
                                   0.0f, WHITE);
                }
                if (tile == TileType::SpawnPoint) {
                    DrawCircleV({dst.x + dst.width * 0.5f, dst.y + dst.height * 0.5f}, 3.0f,
                                Color{240, 240, 240, 180});
                }
            } else if (has_texture && (tile == TileType::Grass || tile == TileType::SpawnPoint)) {
                if (has_grass_bitmask) {
                    const int mask = compute_grass_bitmask(x, y);
                    const bool has_background =
                        sprite_metadata_.HasBitmaskLayer("tile_grass", mask, SpriteFrameLayer::Background);
                    const bool has_foreground =
                        sprite_metadata_.HasBitmaskLayer("tile_grass", mask, SpriteFrameLayer::Foreground);

                    Rectangle src = {};
                    if (has_background &&
                        sprite_metadata_.GetBitmaskFrame("tile_grass", mask, render_time_seconds_,
                                                         SpriteFrameLayer::Background, src)) {
                        DrawTexturePro(texture, InsetSourceRect(src, Constants::kAtlasSampleInsetPixels), dst, {0, 0},
                                       0.0f, WHITE);
                    }
                    if (has_foreground &&
                        sprite_metadata_.GetBitmaskFrame("tile_grass", mask, render_time_seconds_,
                                                         SpriteFrameLayer::Foreground, src)) {
                        DrawTexturePro(texture, InsetSourceRect(src, Constants::kAtlasSampleInsetPixels), dst, {0, 0},
                                       0.0f, WHITE);
                    }
                    if (!has_background && !has_foreground &&
                        sprite_metadata_.GetBitmaskFrame("tile_grass", mask, render_time_seconds_,
                                                         SpriteFrameLayer::Single, src)) {
                        DrawTexturePro(texture, InsetSourceRect(src, Constants::kAtlasSampleInsetPixels), dst, {0, 0},
                                       0.0f, WHITE);
                    }
                } else {
                    const Rectangle src = InsetSourceRect(
                        sprite_metadata_.GetFrame("tile_grass", "default", render_time_seconds_),
                        Constants::kAtlasSampleInsetPixels);
                    DrawTexturePro(texture, src, dst, {0, 0}, 0.0f, WHITE);
                }
                if (tile == TileType::SpawnPoint) {
                    DrawCircleV({dst.x + dst.width * 0.5f, dst.y + dst.height * 0.5f}, 3.0f,
                                Color{240, 240, 240, 180});
                }
            } else {
                if (!has_texture && (tile == TileType::Grass || tile == TileType::SpawnPoint)) {
                    DrawRectangleRec(dst, Color{34, 120, 34, 255});
                    if (tile == TileType::SpawnPoint) {
                        DrawCircleV({dst.x + dst.width * 0.5f, dst.y + dst.height * 0.5f}, 3.0f,
                                    Color{240, 240, 240, 180});
                }
            }
        }
    }

    if (has_texture && has_grass_dual_grid && has_terrain_foam_dual_grid) {
        // The foam is drawn into an intermediate world render target. Preserve the destination alpha
        // while doing standard source-over RGB blending, otherwise the partially transparent foam
        // leaves the world RT semi-transparent and the final RT->screen composite darkens against black.
        rlSetBlendFactorsSeparate(RL_SRC_ALPHA, RL_ONE_MINUS_SRC_ALPHA, RL_ONE, RL_ONE_MINUS_SRC_ALPHA,
                                  RL_FUNC_ADD, RL_FUNC_ADD);
        BeginBlendMode(BLEND_CUSTOM_SEPARATE);

        for (int y = min_y; y <= max_y; ++y) {
            for (int x = min_x; x <= max_x; ++x) {
                const Rectangle dst = SnapRect(
                    {static_cast<float>(x * state_.map.cell_size), static_cast<float>(y * state_.map.cell_size),
                     static_cast<float>(state_.map.cell_size), static_cast<float>(state_.map.cell_size)});
                const float half_cell = static_cast<float>(state_.map.cell_size) * 0.5f;
                const Rectangle terrain_dst =
                    SnapRect({dst.x + half_cell, dst.y + half_cell, dst.width, dst.height});
                const int mask = compute_grass_dual_grid_mask(x, y);
                const TileType base_tile = compute_dual_grid_base_tile(x, y);
                if (base_tile != TileType::Water) {
                    continue;
                }
                if (mask <= 0 || mask >= 15) {
                    continue;
                }

                Rectangle src = {};
                if (!sprite_metadata_.GetDualGridFrame("terrain_foam", mask, render_time_seconds_, SpriteFrameLayer::Single,
                                                       src)) {
                    continue;
                }
                DrawTexturePro(texture, InsetSourceRect(src, Constants::kAtlasSampleInsetPixels), terrain_dst, {0, 0},
                               0.0f, Color{255, 255, 255, 10}
);
            }
        }
        EndBlendMode();
    }
}

    for (const auto& effect : state_.composite_effects) {
        if (!effect.alive) {
            continue;
        }
        const CompositeEffectDefinition* definition = composite_effects_loader_.FindById(effect.effect_id);
        if (definition == nullptr) {
            continue;
        }
        for (const auto& part : definition->parts) {
            if (part.layer != CompositeEffectLayer::Ground) {
                continue;
            }
            const SpriteMetadataLoader* metadata = get_metadata(part.sheet);
            const Texture2D* draw_texture = get_texture(part.sheet);
            if (metadata == nullptr || draw_texture == nullptr || !metadata->HasAnimation(part.animation)) {
                continue;
            }

            const Vector2 part_center = {
                effect.origin_world.x + part.offset_x_tiles * static_cast<float>(state_.map.cell_size),
                effect.origin_world.y + 0.5f * static_cast<float>(state_.map.cell_size) +
                    part.offset_y_tiles * static_cast<float>(state_.map.cell_size),
            };
            const Rectangle src =
                InsetSourceRect(metadata->GetFrame(part.animation, "default", effect.age_seconds),
                                Constants::kAtlasSampleInsetPixels);
            const float dst_w = static_cast<float>(metadata->GetCellWidth());
            const float dst_h = static_cast<float>(metadata->GetCellHeight());
            Rectangle dst = {part_center.x - dst_w * 0.5f, part_center.y - dst_h * 0.5f, dst_w, dst_h};
            DrawTexturePro(*draw_texture, src, SnapRect(dst), {0, 0}, 0.0f, WHITE);
        }
    }

    const CompositeEffectDefinition* fire_storm_cast_definition = composite_effects_loader_.FindById("fire_storm_cast");
    if (fire_storm_cast_definition != nullptr) {
        for (const auto& cast : state_.fire_storm_casts) {
            if (!cast.alive) {
                continue;
            }
            const Vector2 origin = CellToWorldCenter(cast.center_cell);
            for (const auto& part : fire_storm_cast_definition->parts) {
                if (part.layer != CompositeEffectLayer::Ground) {
                    continue;
                }
                const SpriteMetadataLoader* metadata = get_metadata(part.sheet);
                const Texture2D* draw_texture = get_texture(part.sheet);
                if (metadata == nullptr || draw_texture == nullptr || !metadata->HasAnimation(part.animation)) {
                    continue;
                }
                const Vector2 part_center = {
                    origin.x + part.offset_x_tiles * static_cast<float>(state_.map.cell_size),
                    origin.y + 0.5f * static_cast<float>(state_.map.cell_size) +
                        part.offset_y_tiles * static_cast<float>(state_.map.cell_size),
                };
                const Rectangle src = InsetSourceRect(
                    metadata->GetFrame(part.animation, "default", cast.elapsed_seconds),
                    Constants::kAtlasSampleInsetPixels);
                const float dst_w = static_cast<float>(metadata->GetCellWidth());
                const float dst_h = static_cast<float>(metadata->GetCellHeight());
                Rectangle dst = {part_center.x - dst_w * 0.5f, part_center.y - dst_h * 0.5f, dst_w, dst_h};
                DrawTexturePro(*draw_texture, src, SnapRect(dst), {0, 0}, 0.0f, WHITE);
            }
        }
    }

    for (const auto& dummy : state_.fire_storm_dummies) {
        if (!dummy.alive) {
            continue;
        }
        const char* effect_id = dummy.state == FireStormDummyState::Born
                                    ? "fire_storm_born"
                                    : (dummy.state == FireStormDummyState::Dying ? "fire_storm_death" : "fire_storm_idle");
        const CompositeEffectDefinition* definition = composite_effects_loader_.FindById(effect_id);
        if (definition == nullptr) {
            continue;
        }
        for (const auto& part : definition->parts) {
            if (part.layer != CompositeEffectLayer::Ground) {
                continue;
            }
            const SpriteMetadataLoader* metadata = get_metadata(part.sheet);
            const Texture2D* draw_texture = get_texture(part.sheet);
            if (metadata == nullptr || draw_texture == nullptr || !metadata->HasAnimation(part.animation)) {
                continue;
            }
            const Vector2 origin = CellToWorldCenter(dummy.cell);
            const Vector2 part_center = {
                origin.x + part.offset_x_tiles * static_cast<float>(state_.map.cell_size),
                origin.y + 0.5f * static_cast<float>(state_.map.cell_size) +
                    part.offset_y_tiles * static_cast<float>(state_.map.cell_size),
            };
            const Rectangle src = InsetSourceRect(
                metadata->GetFrame(part.animation, "default", dummy.state_time),
                Constants::kAtlasSampleInsetPixels);
            const float dst_w = static_cast<float>(metadata->GetCellWidth());
            const float dst_h = static_cast<float>(metadata->GetCellHeight());
            Rectangle dst = {part_center.x - dst_w * 0.5f, part_center.y - dst_h * 0.5f, dst_w, dst_h};
            DrawTexturePro(*draw_texture, src, SnapRect(dst), {0, 0}, 0.0f, WHITE);
        }
    }

    for (const auto& player : state_.players) {
        if (!player.alive) {
            continue;
        }
        const Vector2 origin = GetRenderPlayerPosition(player.id);
        for (const auto& status : player.status_effects) {
            if (status.composite_effect_id.empty()) {
                continue;
            }
            const CompositeEffectDefinition* definition = composite_effects_loader_.FindById(status.composite_effect_id);
            if (definition == nullptr) {
                continue;
            }
            for (const auto& part : definition->parts) {
                if (part.layer != CompositeEffectLayer::Ground) {
                    continue;
                }
                const SpriteMetadataLoader* metadata = get_metadata(part.sheet);
                const Texture2D* draw_texture = get_texture(part.sheet);
                if (metadata == nullptr || draw_texture == nullptr || !metadata->HasAnimation(part.animation)) {
                    continue;
                }
                const Vector2 part_center = {
                    origin.x + part.offset_x_tiles * static_cast<float>(state_.map.cell_size),
                    origin.y + 0.5f * static_cast<float>(state_.map.cell_size) +
                        part.offset_y_tiles * static_cast<float>(state_.map.cell_size),
                };
                const float animation_time = status.total_seconds - status.remaining_seconds;
                const Rectangle src = InsetSourceRect(
                    metadata->GetFrame(part.animation, "default", animation_time),
                    Constants::kAtlasSampleInsetPixels);
                const float dst_w = static_cast<float>(metadata->GetCellWidth());
                const float dst_h = static_cast<float>(metadata->GetCellHeight());
                Rectangle dst = {part_center.x - dst_w * 0.5f, part_center.y - dst_h * 0.5f, dst_w, dst_h};
                DrawTexturePro(*draw_texture, src, SnapRect(dst), {0, 0}, 0.0f, WHITE);
            }
        }
    }
}

void GameApp::RenderRunes() {
    const bool has_texture = sprite_metadata_.IsLoaded();
    const Texture2D texture = sprite_metadata_.GetTexture();

    for (const auto& rune : state_.runes) {
        if (!rune.active) {
            continue;
        }

        const Vector2 center = CellToWorldCenter(rune.cell);
        const char* animation_name = GetRuneSpriteAnimationKey(rune.rune_type);

        if (has_texture && sprite_metadata_.HasAnimation(animation_name)) {
            const Rectangle src = InsetSourceRect(
                sprite_metadata_.GetFrame(animation_name, "default", render_time_seconds_ + rune.placement_order * 0.05f),
                Constants::kAtlasSampleInsetPixels);
            Rectangle dst = {center.x - 16.0f, center.y - 16.0f, 32.0f, 32.0f};
            dst = SnapRect(dst);
            DrawTexturePro(texture, src, dst, {0, 0}, 0.0f, WHITE);
        } else {
            DrawCircleV(center, 8.0f, GetRuneFallbackColor(rune.rune_type));
            DrawCircleLinesV(center, 8.0f, Color{20, 24, 32, 255});
        }
    }
}

void GameApp::RenderIceWalls() {
    const bool has_texture = sprite_metadata_.IsLoaded();
    const Texture2D texture = sprite_metadata_.GetTexture();

    for (const auto& wall : state_.ice_walls) {
        if (!wall.alive) {
            continue;
        }

        const Vector2 center = CellToWorldCenter(wall.cell);
        Rectangle dst = {center.x - 16.0f, center.y - 16.0f, 32.0f, 32.0f};
        dst = SnapRect(dst);

        const char* animation_name = "ice_wall_idle";
        float animation_time = render_time_seconds_;
        if (wall.state == IceWallState::Materializing) {
            animation_name = "ice_wall_born";
            animation_time = wall.state_time;
        } else if (wall.state == IceWallState::Active) {
            animation_name = "ice_wall_death";
            animation_time = GetIceWallDeathAnimationSampleTime(sprite_metadata_, wall.hp);
        } else if (wall.state == IceWallState::Dying) {
            animation_name = "ice_wall_death";
            animation_time = wall.state_time;
        }

        if (has_texture && sprite_metadata_.HasAnimation(animation_name)) {
            const Rectangle src = InsetSourceRect(sprite_metadata_.GetFrame(animation_name, "default", animation_time),
                                                  Constants::kAtlasSampleInsetPixels);
            DrawTexturePro(texture, src, dst, {0, 0}, 0.0f, WHITE);
        } else {
            const Color fallback =
                (wall.state == IceWallState::Dying) ? Color{140, 180, 230, 180} : Color{180, 220, 255, 255};
            DrawRectangleRec(dst, fallback);
            DrawRectangleLinesEx(dst, 1.0f, Color{60, 95, 140, 255});
        }
    }
}

void GameApp::RenderPlayers() {
    for (const auto& player : state_.players) {
        const Vector2 draw_pos = GetRenderPlayerPosition(player.id);

        if (!player.alive) {
            constexpr const char* kDeathTag = "wiz_death";
            const Texture2D* texture = modular_player_asset_.GetLayerTexture("main", player.team);
            if (player.awaiting_respawn && texture != nullptr && modular_player_asset_.HasTag("main", kDeathTag)) {
                const float death_duration =
                    std::max(0.001f, modular_player_asset_.GetTagDurationSeconds("main", kDeathTag));
                const float death_elapsed =
                    std::clamp(Constants::kRespawnDelaySeconds - player.respawn_remaining, 0.0f,
                               std::max(0.0f, death_duration - 0.0001f));
                const Rectangle src =
                    InsetSourceRect(modular_player_asset_.GetFrame("main", kDeathTag, death_elapsed),
                                    Constants::kAtlasSampleInsetPixels);
                const Rectangle dst = GetPlayerSpriteRect(draw_pos, "main");
                DrawTexturePro(*texture, src, dst, {0, 0}, 0.0f, WHITE);
            } else {
                DrawRectangleRec(GetPlayerCollisionRect(draw_pos), Color{70, 70, 70, 180});
            }
            continue;
        }

        const Rectangle dst = GetPlayerSpriteRect(draw_pos);

        if (modular_player_asset_.HasTag("main", ResolvePlayerModularTag(player))) {
            RenderPlayerModularLayers(player, draw_pos);
        } else {
            DrawRectangleRec(GetPlayerCollisionRect(draw_pos), BLUE);
        }
    }
}

void GameApp::RenderPlayerOverlays() {
    struct OverlayItem {
        float sort_y = 0.0f;
        const Player* player = nullptr;
    };

    std::vector<OverlayItem> overlays;
    overlays.reserve(state_.players.size());
    for (const auto& player : state_.players) {
        if (!player.alive) {
            continue;
        }
        const Vector2 draw_pos = GetRenderPlayerPosition(player.id);
        overlays.push_back({draw_pos.y + 16.0f, &player});
    }

    std::sort(overlays.begin(), overlays.end(), [](const OverlayItem& a, const OverlayItem& b) {
        return a.sort_y < b.sort_y;
    });

    for (const OverlayItem& overlay : overlays) {
        const Player& player = *overlay.player;
        const Vector2 draw_pos = GetRenderPlayerPosition(player.id);
        RenderPlayerOverheadUi(player, draw_pos);
    }
}

void GameApp::RenderOverheadResourceBar(const Rectangle& bar, float value, float max_value, Color fill_color) const {
    const Color bar_missing = {static_cast<unsigned char>(Constants::kPlayerHealthBarMissingR),
                               static_cast<unsigned char>(Constants::kPlayerHealthBarMissingG),
                               static_cast<unsigned char>(Constants::kPlayerHealthBarMissingB), 255};
    const Rectangle snapped_bar = SnapRect(bar);
    DrawRectangleRec(snapped_bar, bar_missing);

    const float ratio = (max_value > 0.0f) ? std::clamp(value / max_value, 0.0f, 1.0f) : 0.0f;
    if (ratio > 0.0f) {
        Rectangle fill = snapped_bar;
        fill.width = std::roundf(fill.width * ratio);
        if (fill.width > 0.0f) {
            DrawRectangleRec(fill, fill_color);
        }
    }
    DrawRectangleLinesEx(snapped_bar, 1.0f, BLACK);
}

void GameApp::RenderPlayerOverheadUi(const Player& player, Vector2 draw_pos) {
    const Rectangle sprite_rect = GetPlayerSpriteRect(draw_pos);
    const Rectangle health_bar = {draw_pos.x - Constants::kPlayerHealthBarWidth * 0.5f,
                                  sprite_rect.y - Constants::kPlayerHealthBarOffsetY + 20.0f,
                                  Constants::kPlayerHealthBarWidth, Constants::kPlayerHealthBarHeight};
    const Rectangle mana_bar = {health_bar.x,
                                health_bar.y + health_bar.height + Constants::kPlayerResourceBarGap,
                                health_bar.width,
                                health_bar.height};

    RenderOverheadResourceBar(health_bar, static_cast<float>(player.hp), static_cast<float>(Constants::kMaxHp),
                              TeamUiColor(player.team));
    RenderOverheadResourceBar(mana_bar, player.mana, player.max_mana,
                              Color{static_cast<unsigned char>(Constants::kHudManaBarR),
                                    static_cast<unsigned char>(Constants::kHudManaBarG),
                                    static_cast<unsigned char>(Constants::kHudManaBarB), 255});

    float name_anchor_y = health_bar.y;
    const int catalyst_charges = GetPlayerRuneChargeCount(player, RuneType::Catalyst);
    if (catalyst_charges > 0 && sprite_metadata_.IsLoaded() && sprite_metadata_.HasAnimation("catalyst_overhead_indicator")) {
        const Texture2D texture = sprite_metadata_.GetTexture();
        Rectangle src = InsetSourceRect(
            sprite_metadata_.GetFrame("catalyst_overhead_indicator", "default", render_time_seconds_),
            Constants::kAtlasSampleInsetPixels);
        src.x += 12.0f;
        src.y += 12.0f;
        src.width = 10.0f;
        src.height = 10.0f;
        const float icon_size = src.width;
        const float icon_gap = Constants::kCatalystOverheadIconGap;
        const float row_width =
            static_cast<float>(catalyst_charges) * icon_size + static_cast<float>(catalyst_charges - 1) * icon_gap;
        const float row_left = health_bar.x + (health_bar.width - row_width) * 0.5f;
        const float row_y = health_bar.y - Constants::kCatalystOverheadIconRowGap - icon_size;
        name_anchor_y = row_y;
        for (int i = 0; i < catalyst_charges; ++i) {
            Rectangle icon_dst = {row_left + static_cast<float>(i) * (icon_size + icon_gap), row_y, icon_size, icon_size};
            DrawTexturePro(texture, src, SnapRect(icon_dst), {0, 0}, 0.0f, WHITE);
        }
    }

    if (player.id != state_.local_player_id && !player.name.empty()) {
        const int name_font_size = 8;
        const int name_width = MeasureText(player.name.c_str(), name_font_size);
        const int name_x = static_cast<int>(health_bar.x + (health_bar.width - static_cast<float>(name_width)) * 0.5f);
        const int name_y = static_cast<int>(name_anchor_y - static_cast<float>(name_font_size) - 2.0f);
        DrawText(player.name.c_str(), name_x, name_y, name_font_size, WHITE);
    }
}

void GameApp::RenderDebugCollisionOverlay() {
    if (!show_network_debug_panel_) {
        return;
    }

    const Color player_color = Color{80, 180, 255, 220};
    const Color player_outline = Color{210, 240, 255, 220};
    const Color object_color = Color{255, 190, 90, 220};
    const Color wall_color = Color{140, 255, 170, 220};
    const Color projectile_color = Color{255, 110, 110, 220};
    const Color explosion_color = Color{255, 60, 60, 220};
    const Color melee_color = Color{255, 120, 220, 220};
    const Color grapple_color = Color{180, 255, 255, 220};

    for (const auto& player : state_.players) {
        if (!player.alive) {
            continue;
        }

        DrawRectangleLinesEx(GetPlayerCollisionRect(player), 1.0f, player_color);
        DrawCircleV(player.pos, 1.5f, player_outline);

        const AttackProfile* attack_profile = GetEquippedPrimaryAttack(player);
        const HitShapeDefinition* hit_shape =
            attack_profile != nullptr ? hit_shape_library_.FindById(attack_profile->hit_shape_id) : nullptr;
        if (player.melee_active_remaining > 0.0f && attack_profile != nullptr && hit_shape != nullptr) {
            const float rotation = GetPlayerLockedMeleeAimRadians(player);
            const Vector2 melee_origin = GetPlayerMeleeRotationOrigin(player);
            if (hit_shape->type == HitShapeType::Circle) {
                const Vector2 center =
                    Vector2Add(melee_origin, Vector2Rotate(hit_shape->center, rotation));
                DrawCircleLinesV(center, hit_shape->radius, melee_color);
            } else {
                const std::vector<Vector2> points = HitShapeLibrary::BuildWorldPoints(*hit_shape, melee_origin, rotation);
                for (size_t i = 0; i < points.size(); ++i) {
                    const Vector2 a = points[i];
                    const Vector2 b = points[(i + 1) % points.size()];
                    DrawLineV(a, b, melee_color);
                }
            }
        }
    }

    for (const auto& object : state_.map_objects) {
        if (!object.alive || !object.collision_enabled) {
            continue;
        }

        const ObjectPrototype* proto = FindObjectPrototype(object.prototype_id);
        const Rectangle aabb = GetMapObjectCollisionAabb(object, proto);
        DrawRectangleLinesEx(aabb, 1.0f, object_color);
    }

    for (const auto& wall : state_.ice_walls) {
        if (!wall.alive || wall.state == IceWallState::Dying) {
            continue;
        }

        const Rectangle aabb = {static_cast<float>(wall.cell.x * state_.map.cell_size),
                                static_cast<float>(wall.cell.y * state_.map.cell_size),
                                static_cast<float>(state_.map.cell_size),
                                static_cast<float>(state_.map.cell_size)};
        DrawRectangleLinesEx(aabb, 1.0f, wall_color);
    }

    for (const auto& projectile : state_.projectiles) {
        if (!projectile.alive) {
            continue;
        }

        DrawCircleLinesV(projectile.pos, projectile.radius, projectile_color);
        DrawCircleV(projectile.pos, 2.0f, projectile_color);
        const Rectangle projectile_aabb = {projectile.pos.x - projectile.radius, projectile.pos.y - projectile.radius,
                                           projectile.radius * 2.0f, projectile.radius * 2.0f};
        DrawRectangleLinesEx(projectile_aabb, 1.0f, projectile_color);
    }

    for (const auto& explosion : state_.explosions) {
        if (!explosion.alive) {
            continue;
        }

        DrawCircleLinesV(explosion.pos, explosion.radius, explosion_color);
    }

    for (const auto& hook : state_.grappling_hooks) {
        if (!hook.alive) {
            continue;
        }

        DrawLineEx(hook.head_pos, hook.target_pos, 1.0f, grapple_color);
        DrawCircleLinesV(hook.head_pos, 4.0f, grapple_color);
        if (hook.latched) {
            DrawCircleLinesV(hook.latch_point, 5.0f, grapple_color);
        }
    }
}

void GameApp::RenderMeleeAttacks() {
    if (!sprite_metadata_.IsLoaded()) {
        return;
    }

    const Texture2D texture = sprite_metadata_.GetTexture();
    for (const auto& player : state_.players) {
        if (!player.alive || player.melee_active_remaining <= 0.0f) {
            continue;
        }

        const AttackProfile* attack_profile = GetEquippedPrimaryAttack(player);
        if (attack_profile == nullptr || attack_profile->kind != EquipmentActionKind::Melee ||
            !sprite_metadata_.HasAnimation(attack_profile->attack_animation)) {
            continue;
        }
        if (!ResolvePrimaryWeaponAttackModularTag(player).empty()) {
            continue;
        }

        const float attack_elapsed = attack_profile->active_window_seconds - player.melee_active_remaining;
        const Vector2 draw_pos = GetRenderPlayerPosition(player.id);
        const float rotation = GetPlayerLockedMeleeAimRadians(player) * RAD2DEG;

        const Rectangle src =
            InsetSourceRect(sprite_metadata_.GetFrame(attack_profile->attack_animation, "default", attack_elapsed),
                            Constants::kAtlasSampleInsetPixels);

        const float visual_size = 32.0f * Constants::kMeleeAttackVisualScale;
        Rectangle dst = {draw_pos.x, draw_pos.y, visual_size, visual_size};
        dst = SnapRect(dst);

        // Rotate around player centroid and keep slash offset one cell to the east at 0 degrees.
        const Vector2 origin = {visual_size * 0.5f - attack_profile->range, visual_size * 0.5f};
        DrawTexturePro(texture, src, dst, origin, rotation, WHITE);
    }
}

void GameApp::RenderProjectiles() {
    const bool has_texture = sprite_metadata_.IsLoaded();
    const Texture2D texture = sprite_metadata_.GetTexture();
    const bool has_large_texture = sprite_metadata_96x96_.IsLoaded();
    const Texture2D large_texture = sprite_metadata_96x96_.GetTexture();

    for (const auto& projectile : state_.projectiles) {
        if (!projectile.alive) {
            continue;
        }

        const bool use_large_static = projectile.animation_key == "fire_storm_static_large";
        const SpriteMetadataLoader* projectile_metadata =
            use_large_static ? (has_large_texture ? &sprite_metadata_96x96_ : nullptr)
                             : (has_texture ? &sprite_metadata_ : nullptr);
        const Texture2D* projectile_texture =
            use_large_static ? (has_large_texture ? &large_texture : nullptr) : (has_texture ? &texture : nullptr);
        if (projectile_metadata != nullptr && projectile_texture != nullptr &&
            projectile_metadata->HasAnimation(projectile.animation_key)) {
            const Rectangle src = InsetSourceRect(
                projectile_metadata->GetFrame(projectile.animation_key, "default", render_time_seconds_),
                Constants::kAtlasSampleInsetPixels);
            const float dst_w = use_large_static ? static_cast<float>(projectile_metadata->GetCellWidth())
                                                 : (IsIceShardProjectile(projectile)
                                                        ? static_cast<float>(projectile_metadata->GetCellWidth()) *
                                                              Constants::kIceWaveShardScale
                                                        : projectile.radius * 8.0f);
            const float dst_h = use_large_static ? static_cast<float>(projectile_metadata->GetCellHeight())
                                                 : (IsIceShardProjectile(projectile)
                                                        ? static_cast<float>(projectile_metadata->GetCellHeight()) *
                                                              Constants::kIceWaveShardScale
                                                        : projectile.radius * 8.0f);
            const Color tint =
                (IsStaticFireBolt(projectile) || IsIceShardProjectile(projectile))
                    ? WHITE
                    : (projectile.owner_team == Constants::kTeamRed ? Color{255, 215, 215, 255}
                                                                    : Color{215, 230, 255, 255});
            const Vector2 rotation_vel =
                use_large_static && projectile.upgrade_pause_remaining > 0.0f ? projectile.resume_vel : projectile.vel;
            const bool oriented_projectile = use_large_static || IsIceShardProjectile(projectile);
            Rectangle dst = oriented_projectile
                                ? Rectangle{projectile.pos.x, projectile.pos.y, dst_w, dst_h}
                                : Rectangle{projectile.pos.x - dst_w * 0.5f, projectile.pos.y - dst_h * 0.5f, dst_w,
                                            dst_h};
            dst = SnapRect(dst);
            const float rotation = oriented_projectile ? AimToDegrees(rotation_vel) : 0.0f;
            const Vector2 origin = oriented_projectile ? Vector2{dst_w * 0.5f, dst_h * 0.5f} : Vector2{0.0f, 0.0f};
            DrawTexturePro(*projectile_texture, src, dst, origin, rotation, tint);
        } else {
            DrawCircleV(projectile.pos, projectile.radius,
                        projectile.owner_team == Constants::kTeamRed ? Color{255, 92, 48, 255}
                                                                     : Color{80, 180, 255, 255});
        }
    }
}

void GameApp::RenderParticles() {
    const bool has_texture = sprite_metadata_.IsLoaded();
    const Texture2D texture = sprite_metadata_.GetTexture();

    for (const auto& particle : state_.particles) {
        if (!particle.alive) {
            continue;
        }

        const Vector2 render_center = GetParticleRenderCenter(particle, GetTime());
        if (has_texture && sprite_metadata_.HasAnimation(particle.animation_key)) {
            const Rectangle src =
                InsetSourceRect(sprite_metadata_.GetFrame(particle.animation_key, particle.facing, particle.age_seconds),
                                Constants::kAtlasSampleInsetPixels);
            const float particle_size =
                particle.size > 0.0f ? particle.size : (particle.animation_key == "static_upgrade" ? 48.0f : 32.0f);
            const bool rotated_particle = std::fabs(particle.rotation_degrees) > 0.001f;
            Rectangle dst = rotated_particle
                                ? Rectangle{render_center.x, render_center.y, particle_size, particle_size}
                                : Rectangle{render_center.x - particle_size * 0.5f, render_center.y - particle_size * 0.5f,
                                            particle_size, particle_size};
            dst = SnapRect(dst);
            const Vector2 origin =
                rotated_particle ? Vector2{particle_size * 0.5f, particle_size * 0.5f} : Vector2{0.0f, 0.0f};
            DrawTexturePro(texture, src, dst, origin, particle.rotation_degrees,
                           Color{255, 255, 255, particle.alpha});
        } else {
            DrawCircleV(render_center, 4.0f, Color{190, 190, 190, 160});
        }
    }
}

void GameApp::RenderDamagePopups() {
    for (const auto& popup : state_.damage_popups) {
        if (!popup.alive) {
            continue;
        }

        const float ratio = std::clamp(1.0f - popup.age_seconds / std::max(0.001f, popup.lifetime_seconds), 0.0f, 1.0f);
        const unsigned char alpha = static_cast<unsigned char>(255.0f * ratio);
        const char* text = popup.is_heal ? TextFormat("+%d", popup.amount) : TextFormat("-%d", popup.amount);
        const int font_size = Constants::kDamagePopupFontSize;
        const int text_w = MeasureText(text, font_size);
        const Vector2 draw_pos = {std::roundf(popup.pos.x - static_cast<float>(text_w) * 0.5f), std::roundf(popup.pos.y)};
        const Color tint = popup.is_heal ? Color{90, 230, 120, alpha} : Color{255, 50, 50, alpha};
        DrawText(text, static_cast<int>(draw_pos.x), static_cast<int>(draw_pos.y), font_size,
                 tint);
    }
}

void GameApp::RenderRunePlacementOverlay() {
    const Player* local_player = FindPlayerById(state_.local_player_id);
    if (!local_player || !local_player->rune_placing_mode) {
        return;
    }

    const GridCoord mouse_cell = WorldToCell(GetScreenToWorld2D(GetMousePosition(), camera_));

    for (int y = mouse_cell.y - Constants::kPlacementPreviewRadiusCells;
         y <= mouse_cell.y + Constants::kPlacementPreviewRadiusCells; ++y) {
        for (int x = mouse_cell.x - Constants::kPlacementPreviewRadiusCells;
             x <= mouse_cell.x + Constants::kPlacementPreviewRadiusCells; ++x) {
            const GridCoord cell = {x, y};
            if (!state_.map.IsInside(cell)) {
                continue;
            }
            if (!IsTileRunePlaceable(cell)) {
                continue;
            }
            if (Vector2Distance(local_player->pos, CellToWorldCenter(cell)) > GetPlayerRuneCastRangeWorld(*local_player)) {
                continue;
            }

            const Rectangle dst = {static_cast<float>(x * state_.map.cell_size), static_cast<float>(y * state_.map.cell_size),
                                   static_cast<float>(state_.map.cell_size), static_cast<float>(state_.map.cell_size)};

            if (sprite_metadata_.IsLoaded() && sprite_metadata_.HasAnimation("grid_overlay")) {
                const Rectangle src =
                    InsetSourceRect(sprite_metadata_.GetFrame("grid_overlay", "default", render_time_seconds_),
                                    Constants::kAtlasSampleInsetPixels);
                DrawTexturePro(sprite_metadata_.GetTexture(), src, SnapRect(dst), {0, 0}, 0.0f,
                               Color{255, 255, 255, 128});
            } else {
                DrawRectangleLinesEx(dst, 1.0f, Color{190, 205, 255, 130});
            }
        }
    }

    if (!state_.map.IsInside(mouse_cell)) {
        return;
    }

    const Vector2 center = CellToWorldCenter(mouse_cell);
    const bool in_range = Vector2Distance(local_player->pos, center) <= GetPlayerRuneCastRangeWorld(*local_player);
    const bool valid = in_range && !IsCellOccupiedByRune(mouse_cell) && !IsCellOccupiedByFireStormDummy(mouse_cell) &&
                       IsTileRunePlaceable(mouse_cell);
    const char* animation_name = GetRuneSpriteAnimationKey(local_player->selected_rune_type);
    if (sprite_metadata_.IsLoaded() && sprite_metadata_.HasAnimation(animation_name)) {
        const Rectangle src =
            InsetSourceRect(sprite_metadata_.GetFrame(animation_name, "default", render_time_seconds_),
                            Constants::kAtlasSampleInsetPixels);
        Rectangle dst = {center.x - 16.0f, center.y - 16.0f, 32.0f, 32.0f};
        dst = SnapRect(dst);
        const Color tint = valid ? Color{255, 255, 255, 160} : Color{255, 96, 96, 160};
        DrawTexturePro(sprite_metadata_.GetTexture(), src, dst, {0, 0}, 0.0f, tint);
    } else {
        Color preview = GetRuneFallbackColor(local_player->selected_rune_type);
        preview.a = 160;
        if (!valid) {
            preview = Color{220, 60, 60, 160};
        }
        DrawCircleV(center, 10.0f, preview);
        DrawCircleLinesV(center, 10.0f, Color{20, 26, 32, 255});
    }
}

void GameApp::RenderBottomHud() {
    Player* local_player = FindPlayerById(state_.local_player_id);
    if (!local_player) {
        return;
    }
    const CastleState* allied_castle = GetAlliedCastleForPlayer(*local_player);
    const bool near_allied_castle = allied_castle != nullptr && IsPlayerWithinCastleRange(*local_player, *allied_castle);
    const bool loadout_mode = local_player->inventory_mode && local_inventory_ui_mode_ == InventoryUiMode::CastleLoadout;
    const float scale = Constants::kHudScale;
    const float panel_width = Constants::kHudPanelWidth * scale;
    const float panel_height = (Constants::kHudPanelHeight - 28.0f) * scale;
    const float hud_x = (static_cast<float>(GetScreenWidth()) - panel_width) * 0.5f;
    const float hud_y = static_cast<float>(GetScreenHeight()) - panel_height - 4.0f;
    const Rectangle panel = {hud_x, hud_y, panel_width, panel_height};
    DrawRectangleRec(panel, Color{static_cast<unsigned char>(Constants::kHudBgR),
                                  static_cast<unsigned char>(Constants::kHudBgG),
                                  static_cast<unsigned char>(Constants::kHudBgB),
                                  static_cast<unsigned char>(Constants::kHudBgA)});
    DrawRectangleLinesEx(panel, std::max(1.0f, scale), Color{70, 82, 104, 255});

    auto draw_bar = [&](float x, float y, float width, float height, float value, float max_value, Color fill_color,
                        const char* label) {
        const Rectangle bg = {x, y, width, height};
        DrawRectangleRec(bg, Color{66, 72, 82, 255});
        const float ratio = (max_value > 0.0f) ? std::clamp(value / max_value, 0.0f, 1.0f) : 0.0f;
        Rectangle fill = bg;
        fill.width = std::roundf(fill.width * ratio);
        if (fill.width > 0.0f) {
            DrawRectangleRec(fill, fill_color);
        }
        DrawRectangleLinesEx(bg, std::max(1.0f, scale * 0.6f), Color{16, 18, 24, 255});
        const int font_size = static_cast<int>(std::roundf(8.0f * scale));
        const int text_width = MeasureText(label, font_size);
        const int text_x = static_cast<int>(x + (width - static_cast<float>(text_width)) * 0.5f);
        const int text_y = static_cast<int>(y + (height - static_cast<float>(font_size)) * 0.5f);
        DrawText(label, text_x, text_y, font_size, BLACK);
    };

    draw_bar(hud_x + 12.0f * scale, hud_y + 6.0f * scale, panel_width - 24.0f * scale,
             Constants::kHudBarHeight * scale,
             static_cast<float>(local_player->hp), static_cast<float>(Constants::kMaxHp),
             Color{static_cast<unsigned char>(Constants::kPlayerHealthBarFillR),
                   static_cast<unsigned char>(Constants::kPlayerHealthBarFillG),
                   static_cast<unsigned char>(Constants::kPlayerHealthBarFillB), 255},
             TextFormat("%d / %d", local_player->hp, Constants::kMaxHp));
    draw_bar(hud_x + 12.0f * scale, hud_y + 18.0f * scale, panel_width - 24.0f * scale,
             Constants::kHudBarHeight * scale,
             local_player->mana, local_player->max_mana,
             Color{static_cast<unsigned char>(Constants::kHudManaBarR),
                   static_cast<unsigned char>(Constants::kHudManaBarG),
                   static_cast<unsigned char>(Constants::kHudManaBarB), 255},
             TextFormat("%d / %d", static_cast<int>(std::roundf(local_player->mana)),
                        static_cast<int>(std::roundf(local_player->max_mana))));

    const std::vector<const StatusEffectInstance*> active_statuses = [&]() {
        std::vector<const StatusEffectInstance*> result;
        result.reserve(local_player->status_effects.size());
        const StatusEffectInstance* rooted_dominant = nullptr;
        for (const auto& status : local_player->status_effects) {
            if (status.remaining_seconds <= 0.0f || !status.visible) {
                continue;
            }
            if (status.type == StatusEffectType::Rooted) {
                if (rooted_dominant == nullptr ||
                    status.movement_speed_multiplier < rooted_dominant->movement_speed_multiplier - 0.0001f ||
                    (std::fabs(status.movement_speed_multiplier - rooted_dominant->movement_speed_multiplier) <= 0.0001f &&
                     status.remaining_seconds > rooted_dominant->remaining_seconds)) {
                    rooted_dominant = &status;
                }
                continue;
            }
            result.push_back(&status);
        }
        if (rooted_dominant != nullptr) {
            result.push_back(rooted_dominant);
        }
        return result;
    }();
    if (!active_statuses.empty()) {
        const float icon_diameter = 21.0f * scale;
        const float icon_spacing = 5.0f * scale;
        const float total_width =
            static_cast<float>(active_statuses.size()) * icon_diameter +
            static_cast<float>(std::max<int>(0, static_cast<int>(active_statuses.size()) - 1)) * icon_spacing;
        const float start_x = hud_x + (panel_width - total_width) * 0.5f;
        const float center_y = hud_y - 13.0f * scale;
        const Color badge_fill = Color{28, 32, 38, 235};
        const Color buff_color = Color{92, 214, 112, 255};
        const Color debuff_color = Color{224, 84, 84, 255};
        for (size_t i = 0; i < active_statuses.size(); ++i) {
            const StatusEffectInstance& status = *active_statuses[i];
            const StatusEffectUiSpec spec = GetStatusEffectUiSpec(status.type);
            const Vector2 center = {start_x + icon_diameter * 0.5f + static_cast<float>(i) * (icon_diameter + icon_spacing),
                                    center_y};
            const float radius = icon_diameter * 0.5f;
            DrawCircleV(center, radius, badge_fill);

            if (sprite_metadata_.IsLoaded() && spec.animation != nullptr && sprite_metadata_.HasAnimation(spec.animation)) {
                const float animation_time = render_time_seconds_;
                const Rectangle src =
                    InsetSourceRect(sprite_metadata_.GetFrame(spec.animation, "default", animation_time),
                                    Constants::kAtlasSampleInsetPixels);
                const float icon_size = icon_diameter * 1.44f;
                Rectangle icon_dst = {center.x - icon_size * 0.5f, center.y - icon_size * 0.5f, icon_size, icon_size};
                DrawTexturePro(sprite_metadata_.GetTexture(), src, SnapRect(icon_dst), {0, 0}, 0.0f, WHITE);
            } else {
                DrawCircleV(center, radius * 0.45f, Color{180, 184, 196, 255});
            }

            const float ratio =
                (status.total_seconds > 0.0f) ? std::clamp(status.remaining_seconds / status.total_seconds, 0.0f, 1.0f) : 0.0f;
            if (ratio > 0.0f) {
                const float start_angle = -90.0f;
                const float end_angle = start_angle + 360.0f * ratio;
                const Color ring_color = status.is_buff ? buff_color : debuff_color;
                DrawRing(center, radius - 2.5f * scale, radius, start_angle, end_angle, 48, ring_color);
            }
            DrawCircleLinesV(center, radius, BLACK);
        }
    }

    const Vector2 mouse = GetMousePosition();
    const float slot_row_height = Constants::kHudSlotSize * scale;
    const float slot_y = hud_y + panel_height - slot_row_height - 4.0f * scale;
    const float slot_spacing = 4.0f * scale;
    const int total_slots = Constants::kHudTotalSlotCount;
    const float volatile_low_mana_pulse =
        0.25f + 0.25f * std::sin(render_time_seconds_ * (2.0f * PI * Constants::kHudVolatileLowManaBlinkHz));
    bool show_inventory_edit_hint = false;
    bool drag_drop_completed = false;
    bool inventory_slot_clicked = false;
    for (int i = 0; i < total_slots; ++i) {
        const float slot_x = hud_x + 12.0f * scale + static_cast<float>(i) * (Constants::kHudSlotSize * scale + slot_spacing);
        const Rectangle slot = {slot_x, slot_y, Constants::kHudSlotSize * scale, Constants::kHudSlotSize * scale};
        const bool is_rune_slot = i < Constants::kHudRuneSlotCount;
        const bool is_item_slot = i >= Constants::kHudRuneSlotCount &&
                                  i < Constants::kHudRuneSlotCount + Constants::kHudItemSlotCount;
        const bool is_weapon_slot = i >= Constants::kHudRuneSlotCount + Constants::kHudItemSlotCount;
        const int slot_index = is_rune_slot ? i : (is_item_slot ? (i - Constants::kHudRuneSlotCount)
                                                                 : (i - Constants::kHudRuneSlotCount - Constants::kHudItemSlotCount));
        const bool hovered = CheckCollisionPointRec(mouse, slot);
        const int slot_font_size = static_cast<int>(std::roundf(8.0f * scale));
        const int binding_code = is_rune_slot
                                     ? (slot_index == 0   ? controls_bindings_.select_rune_slot_1
                                        : slot_index == 1 ? controls_bindings_.select_rune_slot_2
                                        : slot_index == 2 ? controls_bindings_.select_rune_slot_3
                                                          : controls_bindings_.select_rune_slot_4)
                                     : (is_item_slot
                                            ? (slot_index == 0   ? controls_bindings_.activate_item_slot_1
                                               : slot_index == 1 ? controls_bindings_.activate_item_slot_2
                                               : slot_index == 2 ? controls_bindings_.activate_item_slot_3
                                                                 : controls_bindings_.activate_item_slot_4)
                                            : (slot_index == 0 ? controls_bindings_.primary_action
                                                               : controls_bindings_.grappling_hook_action));
        const std::string slot_label_text = BindingToString(binding_code);
        const char* slot_label = slot_label_text.c_str();
        const int slot_label_width = MeasureText(slot_label, slot_font_size);
        DrawText(slot_label,
                 static_cast<int>(slot_x + (slot.width - static_cast<float>(slot_label_width)) * 0.5f),
                 static_cast<int>(slot_y + 3.0f * scale),
                 slot_font_size,
                 hovered ? Color{210, 214, 222, 255} : Color{180, 184, 196, 255});

        if (hovered && !local_player->inventory_mode) {
            show_inventory_edit_hint = true;
        }

        if (local_player->inventory_mode && hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            if (is_rune_slot && !IsCastleManagedRuneSlot(slot_index) && local_player->rune_slots[slot_index] != RuneType::Catalyst) {
                continue;
            }
            inventory_slot_clicked = true;
            if (!local_player->ui_dragging_slot) {
                const bool has_content =
                    is_rune_slot ? (local_player->rune_slots[slot_index] != RuneType::None)
                                 : (is_item_slot ? (!local_player->item_slots[slot_index].empty() &&
                                                    local_player->item_slot_counts[slot_index] > 0)
                                                 : !local_player->weapon_slots[slot_index].empty());
                if (has_content) {
                    BeginInventoryDrag(*local_player,
                                      is_rune_slot ? SlotFamily::Rune : (is_item_slot ? SlotFamily::Item : SlotFamily::Weapon),
                                      slot_index);
                }
            } else if (local_player->ui_drag_source_family ==
                       (is_rune_slot ? SlotFamily::Rune : (is_item_slot ? SlotFamily::Item : SlotFamily::Weapon))) {
                if (local_player->ui_drag_source_family == SlotFamily::Rune &&
                    local_player->ui_drag_rune_type != RuneType::Catalyst &&
                    (!IsCastleManagedRuneSlot(local_player->ui_drag_source_index) || !IsCastleManagedRuneSlot(slot_index))) {
                    continue;
                }
                DropInventoryDrag(*local_player,
                                  is_rune_slot ? SlotFamily::Rune : (is_item_slot ? SlotFamily::Item : SlotFamily::Weapon),
                                  slot_index);
                drag_drop_completed = true;
                if (!network_manager_.IsHost()) {
                    QueueLocalInventorySync(*local_player);
                }
            }
        }

        if (is_rune_slot) {
            const RuneType rune_type = local_player->rune_slots[slot_index];
            const char* animation = GetRuneSpriteAnimationKey(rune_type);
            if (rune_type != RuneType::None && sprite_metadata_.IsLoaded() && sprite_metadata_.HasAnimation(animation)) {
                const Rectangle src =
                    InsetSourceRect(sprite_metadata_.GetFrame(animation, "default", render_time_seconds_),
                                    Constants::kAtlasSampleInsetPixels);
                const float normal_mana_cost = GetRuneManaCost(rune_type);
                const float volatile_mana_cost = normal_mana_cost * Constants::kRuneVolatileManaMultiplier;
                const bool low_mana = local_player->mana < normal_mana_cost;
                const bool missing_charge = RuneUsesCharges(rune_type) && GetPlayerRuneChargeCount(*local_player, rune_type) <= 0;
                const bool volatile_low_mana =
                    !low_mana && !missing_charge && local_player->mana < volatile_mana_cost;
                const Color low_mana_tint = {
                    static_cast<unsigned char>(Constants::kHudLowManaTintR),
                    static_cast<unsigned char>(Constants::kHudLowManaTintG),
                    static_cast<unsigned char>(Constants::kHudLowManaTintB),
                    static_cast<unsigned char>(255.0f * Constants::kHudLowManaBrightness),
                };
                const auto blend_channel = [](unsigned char from, unsigned char to, float t) {
                    return static_cast<unsigned char>(std::roundf(static_cast<float>(from) +
                                                                  (static_cast<float>(to) - static_cast<float>(from)) * t));
                };
                const Color tint = (low_mana || missing_charge)
                                       ? low_mana_tint
                                       : (volatile_low_mana
                                              ? Color{blend_channel(WHITE.r, low_mana_tint.r, volatile_low_mana_pulse),
                                                      blend_channel(WHITE.g, low_mana_tint.g, volatile_low_mana_pulse),
                                                      blend_channel(WHITE.b, low_mana_tint.b, volatile_low_mana_pulse),
                                                      blend_channel(WHITE.a, low_mana_tint.a, volatile_low_mana_pulse)}
                                              : WHITE);
                const float base_icon_size = std::min(slot.width - 8.0f * scale, slot.height - 18.0f * scale);
                const float icon_size =
                    std::min({slot.width - 2.0f * scale, slot.height - 10.0f * scale, base_icon_size * 1.2f});
                Rectangle icon_dst = {slot.x + (slot.width - icon_size) * 0.5f, slot.y + 9.0f * scale, icon_size,
                                      icon_size};
                DrawTexturePro(sprite_metadata_.GetTexture(), src, SnapRect(icon_dst), {0, 0}, 0.0f, tint);
            }

            const float charge_y = slot.y + slot.height - 12.0f * scale;
            if (RuneUsesCharges(rune_type)) {
                const int charge_count = GetPlayerRuneChargeCount(*local_player, rune_type);
                const int charge_font_size = static_cast<int>(std::roundf(9.0f * scale));
                const char* charge_text = TextFormat("%d", charge_count);
                DrawText(charge_text,
                         static_cast<int>(slot.x + slot.width - 14.0f * scale),
                         static_cast<int>(charge_y),
                         charge_font_size,
                         WHITE);
            }

            const float remaining = GetPlayerRuneCooldownRemaining(*local_player, rune_type);
            const float total = std::max(0.001f, GetPlayerRuneCooldownTotal(*local_player, rune_type));
            if (remaining > 0.0f) {
                const float ratio = std::clamp(remaining / total, 0.0f, 1.0f);
                DrawCircleSector({slot.x + slot.width * 0.5f, slot.y + slot.height * 0.5f}, slot.width * 0.45f, -90.0f,
                                 -90.0f + 360.0f * ratio, 32, Color{0, 0, 0, 150});
            }
            if (local_player->selected_rune_slot == slot_index) {
                const float underline_y = slot.y + slot.height - 2.0f * scale;
                DrawLineEx({slot.x + 4.0f * scale, underline_y}, {slot.x + slot.width - 4.0f * scale, underline_y},
                           std::max(2.0f, scale * 0.9f), Color{220, 228, 242, 255});
            }
        } else if (is_item_slot) {
            if (!local_player->item_slots[slot_index].empty()) {
                const ObjectPrototype* proto = FindObjectPrototype(local_player->item_slots[slot_index]);
                if (proto != nullptr && sprite_metadata_.IsLoaded() && sprite_metadata_.HasAnimation(proto->idle_animation)) {
                    const Rectangle src =
                        InsetSourceRect(sprite_metadata_.GetFrame(proto->idle_animation, "default", render_time_seconds_),
                                        Constants::kAtlasSampleInsetPixels);
                    const float base_icon_size = std::min(slot.width - 8.0f * scale, slot.height - 18.0f * scale);
                    const float icon_size =
                        std::min({slot.width - 2.0f * scale, slot.height - 10.0f * scale, base_icon_size * 1.2f});
                    Rectangle icon_dst = {slot.x + (slot.width - icon_size) * 0.5f, slot.y + 9.0f * scale, icon_size,
                                          icon_size};
                    DrawTexturePro(sprite_metadata_.GetTexture(), src, SnapRect(icon_dst), {0, 0}, 0.0f, WHITE);
                }
                DrawText(TextFormat("%d", local_player->item_slot_counts[slot_index]),
                         static_cast<int>(slot.x + slot.width - 12.0f * scale),
                         static_cast<int>(slot.y + slot.height - 12.0f * scale),
                         static_cast<int>(std::roundf(8.0f * scale)), WHITE);
            }
            const float remaining = local_player->item_slot_cooldown_remaining[slot_index];
            const float total = std::max(0.001f, local_player->item_slot_cooldown_total[slot_index]);
            if (remaining > 0.0f) {
                const float ratio = std::clamp(remaining / total, 0.0f, 1.0f);
                DrawCircleSector({slot.x + slot.width * 0.5f, slot.y + slot.height * 0.5f}, slot.width * 0.45f, -90.0f,
                                 -90.0f + 360.0f * ratio, 32, Color{0, 0, 0, 150});
            }
        } else if (is_weapon_slot) {
            const EquipmentSlot equipment_slot =
                slot_index == 0 ? EquipmentSlot::PrimaryWeapon : EquipmentSlot::Mobility;
            const EquipmentItemDefinition* equipped_item =
                equipment_registry_.ResolveEquippedItem(*local_player, equipment_slot);
            const std::string animation =
                equipped_item != nullptr && !equipped_item->hud_icon_animation.empty()
                    ? equipped_item->hud_icon_animation
                    : (equipped_item != nullptr ? equipped_item->id : std::string{});
            if (!animation.empty() && sprite_metadata_.IsLoaded() && sprite_metadata_.HasAnimation(animation)) {
                const Rectangle src =
                    InsetSourceRect(sprite_metadata_.GetFrame(animation, "default", render_time_seconds_),
                                    Constants::kAtlasSampleInsetPixels);
                const float base_icon_size = std::min(slot.width - 8.0f * scale, slot.height - 18.0f * scale);
                const float icon_size =
                    std::min({slot.width - 2.0f * scale, slot.height - 10.0f * scale, base_icon_size * 1.2f});
                Rectangle icon_dst = {slot.x + (slot.width - icon_size) * 0.5f, slot.y + 9.0f * scale, icon_size,
                                      icon_size};
                DrawTexturePro(sprite_metadata_.GetTexture(), src, SnapRect(icon_dst), {0, 0}, 0.0f, WHITE);
            }
            const AttackProfile* attack_profile = GetEquippedPrimaryAttack(*local_player);
            const float remaining = slot_index == 0 ? local_player->melee_cooldown_remaining
                                                    : local_player->grappling_cooldown_remaining;
            const float total = std::max(0.001f, slot_index == 0
                                                      ? (attack_profile != nullptr ? attack_profile->cooldown_seconds
                                                                                   : Constants::kMeleeCooldownSeconds)
                                                      : local_player->grappling_cooldown_total);
            if (remaining > 0.0f) {
                const float ratio = std::clamp(remaining / total, 0.0f, 1.0f);
                DrawCircleSector({slot.x + slot.width * 0.5f, slot.y + slot.height * 0.5f}, slot.width * 0.45f, -90.0f,
                                 -90.0f + 360.0f * ratio, 32, Color{0, 0, 0, 150});
            }
        }

        if (i == Constants::kHudRuneSlotCount - 1 || i == Constants::kHudRuneSlotCount + Constants::kHudItemSlotCount - 1) {
            const float divider_x = slot.x + slot.width + slot_spacing * 0.5f;
            DrawLineEx({divider_x, slot_y + 6.0f * scale},
                       {divider_x, slot_y + slot_row_height - 6.0f * scale},
                       std::max(1.0f, scale * 0.75f), Color{90, 100, 120, 255});
        }
    }

    const Rectangle hud_panel_rect = {hud_x, hud_y, panel_width, panel_height};
    if (local_player->inventory_mode && local_player->ui_dragging_slot && !drag_drop_completed && !inventory_slot_clicked) {
        const bool left_click = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
        const bool right_click = IsMouseButtonPressed(MOUSE_BUTTON_RIGHT);
        if ((left_click || right_click) && !CheckCollisionPointRec(mouse, hud_panel_rect)) {
            const bool single_instance = right_click;
            bool changed = false;
            if (network_manager_.IsHost()) {
                changed = TryDropDraggedSlotToWorld(*local_player, local_player->ui_drag_source_family,
                                                   local_player->ui_drag_source_index, single_instance);
            } else {
                pending_world_drop_request_ = true;
                pending_world_drop_family_ = local_player->ui_drag_source_family;
                pending_world_drop_slot_index_ = local_player->ui_drag_source_index;
                pending_world_drop_single_instance_ = single_instance;
                changed = true;
            }
            CancelInventoryDrag(*local_player);
        } else if (left_click) {
            CancelInventoryDrag(*local_player);
        }
    }

    if (show_inventory_edit_hint && !local_player->inventory_mode) {
        const char* hint_text = near_allied_castle ? "Press Tab for loadout" : "Press Tab to edit";
        const int hint_font_size = static_cast<int>(std::roundf(9.0f * scale));
        const int hint_padding_x = static_cast<int>(std::roundf(8.0f * scale));
        const int hint_padding_y = static_cast<int>(std::roundf(6.0f * scale));
        const int hint_width = MeasureText(hint_text, hint_font_size) + hint_padding_x * 2;
        const int hint_height = hint_font_size + hint_padding_y * 2;
        const float hint_x = mouse.x + 10.0f;
        const float hint_y = mouse.y + 10.0f;
        const Rectangle bubble = {hint_x, hint_y, static_cast<float>(hint_width), static_cast<float>(hint_height)};
        DrawRectangleRec(bubble, Color{24, 28, 34, 235});
        DrawRectangleLinesEx(bubble, 1.0f, Color{80, 92, 112, 255});
        DrawText(hint_text, static_cast<int>(hint_x) + hint_padding_x, static_cast<int>(hint_y) + hint_padding_y,
                 hint_font_size, RAYWHITE);
    }

    if (local_player->inventory_mode && local_player->ui_dragging_slot) {
        const float base_icon_size =
            std::min(Constants::kHudSlotSize * scale - 8.0f * scale, Constants::kHudSlotSize * scale - 18.0f * scale);
        const float icon_size =
            std::min({Constants::kHudSlotSize * scale - 2.0f * scale,
                      Constants::kHudSlotSize * scale - 10.0f * scale,
                      base_icon_size * 1.2f});
        const Rectangle drag_dst = {mouse.x + 10.0f, mouse.y + 10.0f, icon_size, icon_size};
        if (local_player->ui_drag_source_family == SlotFamily::Rune) {
            const char* animation = GetRuneSpriteAnimationKey(local_player->ui_drag_rune_type);
            if (local_player->ui_drag_rune_type != RuneType::None && sprite_metadata_.IsLoaded() &&
                sprite_metadata_.HasAnimation(animation)) {
                const Rectangle src =
                    InsetSourceRect(sprite_metadata_.GetFrame(animation, "default", render_time_seconds_),
                                    Constants::kAtlasSampleInsetPixels);
                DrawTexturePro(sprite_metadata_.GetTexture(), src, SnapRect(drag_dst), {0, 0}, 0.0f,
                               Color{255, 255, 255, 230});
            }
        } else if (!local_player->ui_drag_item_id.empty()) {
            if (const ObjectPrototype* proto = FindObjectPrototype(local_player->ui_drag_item_id);
                proto != nullptr && sprite_metadata_.IsLoaded() && sprite_metadata_.HasAnimation(proto->idle_animation)) {
                const Rectangle src =
                    InsetSourceRect(sprite_metadata_.GetFrame(proto->idle_animation, "default", render_time_seconds_),
                                    Constants::kAtlasSampleInsetPixels);
                DrawTexturePro(sprite_metadata_.GetTexture(), src, SnapRect(drag_dst), {0, 0}, 0.0f,
                               Color{255, 255, 255, 230});
                DrawText(TextFormat("%d", local_player->ui_drag_item_count),
                         static_cast<int>(drag_dst.x + drag_dst.width - 12.0f * scale),
                         static_cast<int>(drag_dst.y + drag_dst.height - 12.0f * scale),
                         static_cast<int>(std::roundf(8.0f * scale)), WHITE);
            }
        }
    }

    if (loadout_mode && allied_castle != nullptr) {
        const float screen_w = static_cast<float>(GetScreenWidth());
        const float top_division_top = 78.0f;
        const float top_division_bottom = hud_y - 18.0f;
        const float panel_x = screen_w * 0.5f + 14.0f;
        const float panel_y = top_division_top;
        const float panel_w = std::min(534.0f, std::max(450.0f, screen_w - panel_x - 14.0f));
        const float panel_h = std::max(180.0f, top_division_bottom - panel_y);
        const Rectangle loadout_panel = {panel_x, panel_y, panel_w, panel_h};
        DrawRectangleRec(loadout_panel, Color{24, 28, 34, 236});
        DrawRectangleLinesEx(loadout_panel, 1.0f, Color{80, 92, 112, 255});
        DrawText(TextFormat("Castle level: %d", allied_castle->level), static_cast<int>(panel_x + 16.0f),
                 static_cast<int>(panel_y + 14.0f), 22, RAYWHITE);
        DrawLineEx({panel_x + 14.0f, panel_y + 44.0f}, {panel_x + panel_w - 14.0f, panel_y + 44.0f}, 1.0f,
                   Color{72, 82, 96, 255});

        int equipped_count = 0;
        for (RuneType rune_type : local_player->rune_slots) {
            if (IsCastleEquippableRune(rune_type)) {
                ++equipped_count;
            }
        }
        DrawText(TextFormat("Equipped: %d / %d", equipped_count, GetCastleLoadoutCapacity(allied_castle->level)),
                 static_cast<int>(panel_x + 16.0f), static_cast<int>(panel_y + 52.0f), 16, Color{188, 196, 212, 255});

        struct LoadoutRuneRow {
            RuneType rune_type = RuneType::None;
            const char* label = "";
        };
        const std::array<LoadoutRuneRow, 3> rows = {{
            {RuneType::Fire, "Fire"},
            {RuneType::Water, "Water"},
            {RuneType::Earth, "Earth"},
        }};
        float row_y = panel_y + 84.0f;
        for (const LoadoutRuneRow& row : rows) {
            const bool equipped =
                std::find(local_player->rune_slots.begin(), local_player->rune_slots.end(), row.rune_type) !=
                local_player->rune_slots.end();
            const int required_level = GetRuneRequiredCastleLevel(row.rune_type);
            const bool level_unlocked = allied_castle->level >= required_level;
            const bool has_capacity = equipped || equipped_count < GetCastleLoadoutCapacity(allied_castle->level);
            const bool can_press_equip = level_unlocked && !equipped && has_capacity;
            const Rectangle row_rect = {panel_x + 10.0f, row_y + 2.0f, panel_w - 20.0f, 32.0f};
            if (equipped) {
                DrawRectangleRec(row_rect, Color{58, 150, 76, 84});
                DrawRectangleLinesEx(row_rect, 1.0f, Color{104, 208, 120, 220});
            }
            const char* rune_animation = GetRuneSpriteAnimationKey(row.rune_type);
            const float icon_size = 18.0f;
            const float icon_x = panel_x + 16.0f;
            const float icon_y = row_y + 7.0f;
            if (sprite_metadata_.IsLoaded() && sprite_metadata_.HasAnimation(rune_animation)) {
                const Rectangle icon_src =
                    InsetSourceRect(sprite_metadata_.GetFrame(rune_animation, "default", render_time_seconds_),
                                    Constants::kAtlasSampleInsetPixels);
                const Rectangle icon_dst = {icon_x, icon_y, icon_size, icon_size};
                DrawTexturePro(sprite_metadata_.GetTexture(), icon_src, SnapRect(icon_dst), {0, 0}, 0.0f, WHITE);
            }
            DrawText(row.label, static_cast<int>(panel_x + 40.0f), static_cast<int>(row_y + 8.0f), 20, RAYWHITE);
            if (!level_unlocked) {
                DrawText(TextFormat("Requires castle level %d", required_level), static_cast<int>(panel_x + 128.0f),
                         static_cast<int>(row_y + 10.0f), 16, Color{186, 120, 120, 255});
            } else if (!equipped && !has_capacity) {
                DrawText("Capacity full", static_cast<int>(panel_x + 128.0f), static_cast<int>(row_y + 10.0f), 16,
                         Color{198, 188, 132, 255});
            }

            const Rectangle button = {panel_x + panel_w - 118.0f, row_y + 2.0f, 100.0f, 30.0f};
            bool pressed = false;
            if (!level_unlocked || (!equipped && !has_capacity)) {
                GuiDisable();
                pressed = GuiButton(button, equipped ? "Unequip" : "Equip");
                GuiEnable();
            } else {
                pressed = GuiButton(button, equipped ? "Unequip" : "Equip");
            }
            if (can_press_equip) {
                const float pulse = 0.5f + 0.5f * std::sin(render_time_seconds_ * 2.0f * PI * 1.6f);
                const unsigned char fill_alpha = static_cast<unsigned char>(std::roundf(42.0f + 78.0f * pulse));
                const unsigned char border_alpha = static_cast<unsigned char>(std::roundf(120.0f + 100.0f * pulse));
                const Rectangle fill_rect = {button.x + 1.0f, button.y + 1.0f, button.width - 2.0f, button.height - 2.0f};
                DrawRectangleRec(fill_rect, Color{224, 198, 72, fill_alpha});
                DrawRectangleLinesEx(fill_rect, 1.0f, Color{250, 228, 110, border_alpha});
                const char* label = "Equip";
                const int font_size = 10;
                const int text_width = MeasureText(label, font_size);
                DrawText(label,
                         static_cast<int>(button.x + (button.width - static_cast<float>(text_width)) * 0.5f),
                         static_cast<int>(button.y + (button.height - static_cast<float>(font_size)) * 0.5f - 1.0f),
                         font_size, Color{221, 228, 245, 255});
            }
            if (pressed) {
                const bool changed = equipped ? UnequipRuneFromCastleLoadout(*local_player, row.rune_type)
                                              : EquipRuneToCastleLoadout(*local_player, row.rune_type, allied_castle->level);
                if (changed) {
                    if (equipped) {
                        if (sfx_castle_unequip_rune_.loaded) {
                            PlaySound(sfx_castle_unequip_rune_.sound);
                        }
                    } else if (sfx_castle_equip_rune_.loaded) {
                        PlaySound(sfx_castle_equip_rune_.sound);
                    }
                }
                if (changed && !network_manager_.IsHost()) {
                    QueueLocalInventorySync(*local_player);
                }
                if (changed) {
                    equipped_count += equipped ? -1 : 1;
                }
            }
            row_y += 40.0f;
        }
    }
}

void GameApp::RenderFpsCounter() {
    if (!show_network_debug_panel_) {
        return;
    }

    const int fps = GetFPS();
    const int font_size = 18;
    const std::string text = TextFormat("FPS: %d", fps);
    const int text_width = MeasureText(text.c_str(), font_size);
    const float padding = 12.0f;
    const float box_width = static_cast<float>(text_width) + 16.0f;
    const float box_height = 28.0f;
    const float x = static_cast<float>(GetScreenWidth()) - box_width - padding;
    const float y = padding;
    const Rectangle panel = {x, y, box_width, box_height};

    DrawRectangleRec(panel, Color{20, 24, 30, 220});
    DrawRectangleLinesEx(panel, 1.0f, Color{70, 82, 104, 255});
    DrawText(text.c_str(), static_cast<int>(x + 8.0f), static_cast<int>(y + 6.0f), font_size, RAYWHITE);
}

void GameApp::RenderInGameMenu() {
    if (app_screen_ != AppScreen::InMatch || !in_game_menu_open_) {
        return;
    }

    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Color{0, 0, 0, 145});

    const float width = 320.0f;
    const float height = 260.0f;
    const float x = (static_cast<float>(GetScreenWidth()) - width) * 0.5f;
    const float y = (static_cast<float>(GetScreenHeight()) - height) * 0.5f;
    const Rectangle panel = {x, y, width, height};
    DrawRectangleRec(panel, Color{25, 28, 34, 245});
    DrawRectangleLinesEx(panel, 1.0f, Color{68, 75, 88, 255});

    if (in_game_menu_page_ == InGameMenuPage::Home) {
        DrawText("Game Menu", static_cast<int>(x + 20.0f), static_cast<int>(y + 18.0f), 28, RAYWHITE);

        if (GuiButton({x + 50.0f, y + 70.0f, 220.0f, 40.0f}, "Return To Game")) {
            in_game_menu_open_ = false;
        }
        if (GuiButton({x + 50.0f, y + 124.0f, 220.0f, 40.0f}, "Settings")) {
            in_game_menu_page_ = InGameMenuPage::Settings;
        }
        if (GuiButton({x + 50.0f, y + 178.0f, 220.0f, 40.0f}, "Leave Game")) {
            ReturnToMainMenu();
        }
        return;
    }

    DrawText("Settings", static_cast<int>(x + 20.0f), static_cast<int>(y + 18.0f), 28, RAYWHITE);
    bool updated_debug_panel = show_network_debug_panel_;
    GuiCheckBox({x + 20.0f, y + 82.0f, 24.0f, 24.0f}, "Show Network Debug Panel", &updated_debug_panel);
    if (updated_debug_panel != show_network_debug_panel_) {
        show_network_debug_panel_ = updated_debug_panel;
        settings_.show_network_debug_panel = show_network_debug_panel_;
        config_manager_.Save(settings_);
    }
    bool updated_hide_own_influence_zones = settings_.hide_own_influence_zones;
    bool updated_auto_pick_replace_equipment = settings_.auto_pick_replace_equipment;
    GuiCheckBox({x + 20.0f, y + 116.0f, 24.0f, 24.0f}, "Auto Replace Equipped Weapons",
                &updated_auto_pick_replace_equipment);
    if (updated_auto_pick_replace_equipment != settings_.auto_pick_replace_equipment) {
        settings_.auto_pick_replace_equipment = updated_auto_pick_replace_equipment;
        config_manager_.Save(settings_);
    }
    GuiCheckBox({x + 20.0f, y + 150.0f, 24.0f, 24.0f}, "Hide Own Influence Zones", &updated_hide_own_influence_zones);
    if (updated_hide_own_influence_zones != settings_.hide_own_influence_zones) {
        settings_.hide_own_influence_zones = updated_hide_own_influence_zones;
        config_manager_.Save(settings_);
    }
    bool updated_enable_influence_zone_system = settings_.enable_influence_zone_system;
    GuiCheckBox({x + 20.0f, y + 184.0f, 24.0f, 24.0f}, "Enable Influence Zone System",
                &updated_enable_influence_zone_system);
    if (updated_enable_influence_zone_system != settings_.enable_influence_zone_system) {
        settings_.enable_influence_zone_system = updated_enable_influence_zone_system;
        if (settings_.enable_influence_zone_system) {
            RebuildInfluenceZones();
        } else {
            ClearInfluenceZoneVisuals();
        }
        config_manager_.Save(settings_);
    }
    if (GuiButton({x + 20.0f, y + height - 52.0f, 120.0f, 32.0f}, "Back")) {
        in_game_menu_page_ = InGameMenuPage::Home;
    }
}

void GameApp::RenderNetworkDebugPanel() {
    if (!show_network_debug_panel_) {
        return;
    }

    const NetTelemetry& t = network_manager_.GetTelemetry();

    const float x = 12.0f;
    const float y = 12.0f;
    const float width = 408.0f;
    const float height = network_debug_panel_minimized_ ? 34.0f : 224.0f;
    const Rectangle panel = {x, y, width, height};

    DrawRectangleRec(panel, Color{20, 24, 30, 220});
    DrawRectangleLinesEx(panel, 1.0f, Color{70, 82, 104, 255});
    DrawText("Net Debug", static_cast<int>(x + 10.0f), static_cast<int>(y + 8.0f), 14, RAYWHITE);

    if (GuiButton({x + width - 28.0f, y + 6.0f, 20.0f, 20.0f}, network_debug_panel_minimized_ ? "+" : "-")) {
        network_debug_panel_minimized_ = !network_debug_panel_minimized_;
    }

    if (network_debug_panel_minimized_) {
        return;
    }

    const char* role = network_manager_.IsHost() ? "Host" : "Client";
    const std::string status_text = network_manager_.IsHost() ? "authoritative" : GetClientLobbyStatusText();

    int text_y = static_cast<int>(y + 34.0f);
    const int line_h = 16;
    DrawText(TextFormat("Role: %s  |  Status: %s", role, status_text.c_str()), static_cast<int>(x + 10.0f), text_y, 13,
             RAYWHITE);
    text_y += line_h;
    DrawText(TextFormat("Up: %.1f KB/s (%0.1f pkt/s)  Down: %.1f KB/s (%0.1f pkt/s)", t.bytes_per_sec_up / 1024.0f,
                        t.packets_per_sec_up, t.bytes_per_sec_down / 1024.0f, t.packets_per_sec_down),
             static_cast<int>(x + 10.0f), text_y, 13, Color{184, 210, 255, 255});
    text_y += line_h;
    DrawText(TextFormat("Snapshots tx/rx: %llu / %llu  avg bytes tx/rx: %.1f / %.1f",
                        static_cast<unsigned long long>(t.snapshots_sent_total),
                        static_cast<unsigned long long>(t.snapshots_received_total), t.average_snapshot_bytes_sent,
                        t.average_snapshot_bytes_received),
             static_cast<int>(x + 10.0f), text_y, 13, Color{184, 210, 255, 255});
    text_y += line_h;
    DrawText(TextFormat("Keyframe tx/rx: %llu / %llu  Delta tx/rx: %llu / %llu",
                        static_cast<unsigned long long>(t.keyframe_snapshots_sent_total),
                        static_cast<unsigned long long>(t.keyframe_snapshots_received_total),
                        static_cast<unsigned long long>(t.delta_snapshots_sent_total),
                        static_cast<unsigned long long>(t.delta_snapshots_received_total)),
             static_cast<int>(x + 10.0f), text_y, 13, Color{190, 226, 180, 255});
    text_y += line_h;
    DrawText(TextFormat("Dropped snapshots: %llu  Missing-base deltas: %llu",
                        static_cast<unsigned long long>(t.dropped_snapshots_total),
                        static_cast<unsigned long long>(t.dropped_delta_missing_base_total)),
             static_cast<int>(x + 10.0f), text_y, 13,
             (t.dropped_snapshots_total > 0 || t.dropped_delta_missing_base_total > 0) ? Color{255, 175, 130, 255}
                                                                                         : Color{186, 226, 186, 255});
    text_y += line_h;
    DrawText(TextFormat("Reconcile corrections: %.2f/s (total %llu)", t.reconciliation_corrections_per_sec,
                        static_cast<unsigned long long>(t.reconciliation_corrections_total)),
             static_cast<int>(x + 10.0f), text_y, 13, Color{235, 220, 185, 255});
    text_y += line_h;

    const char* hint_1 = "Tune: lower interpolation delay first (100ms -> 75 -> 50 on LAN).";
    const char* hint_2 = "If missing-base deltas rise, shorten keyframe interval.";
    if (t.reconciliation_corrections_per_sec > 8.0f) {
        hint_2 = "High reconcile rate: increase hard snap threshold or lower reconcile gain.";
    } else if (t.dropped_delta_missing_base_total == 0 && t.dropped_snapshots_total == 0) {
        hint_2 = "If still stuttery, raise snapshot rate or reduce render load.";
    }
    DrawText(hint_1, static_cast<int>(x + 10.0f), text_y, 12, Color{210, 214, 220, 255});
    text_y += 14;
    DrawText(hint_2, static_cast<int>(x + 10.0f), text_y, 12, Color{210, 214, 220, 255});
}

void GameApp::UpdateCameraTarget() {
    const Player* local_player = FindPlayerById(state_.local_player_id);
    if (!local_player && !state_.players.empty()) {
        local_player = &state_.players.front();
    }

    if (local_player) {
        const Vector2 render_pos = GetRenderPlayerPosition(local_player->id);
        camera_.target = {std::roundf(render_pos.x), std::roundf(render_pos.y)};
    }

    Vector2 base_offset = {std::roundf(static_cast<float>(GetScreenWidth()) * 0.5f),
                           std::roundf(static_cast<float>(GetScreenHeight()) * 0.5f)};

    if (camera_shake_time_remaining_ > 0.0f) {
        const float shake_alpha = std::clamp(camera_shake_time_remaining_ / Constants::kCameraShakeDurationSeconds, 0.0f,
                                             1.0f);
        const float amplitude = Constants::kCameraShakePixels * shake_alpha;
        std::uniform_real_distribution<float> shake_dist(-amplitude, amplitude);
        base_offset.x += shake_dist(rng_);
        base_offset.y += shake_dist(rng_);
    }

    camera_.offset = base_offset;
}

Vector2 GameApp::GetRenderPlayerPosition(int player_id) const {
    auto it = render_player_positions_.find(player_id);
    if (it != render_player_positions_.end()) {
        return it->second;
    }
    if (const Player* player = FindPlayerById(player_id)) {
        return player->pos;
    }
    return {0.0f, 0.0f};
}

std::string GameApp::GetClientLobbyStatusText() const {
    switch (network_manager_.GetClientConnectionState()) {
        case ClientConnectionState::ConnectingTransport:
            return "connecting transport...";
        case ClientConnectionState::WaitingJoinAck:
            return "connected; waiting join_ack...";
        case ClientConnectionState::WaitingLobbyState:
            return "join_ack received; waiting lobby_state...";
        case ClientConnectionState::ReadyInLobby:
            return "connected";
        case ClientConnectionState::Disconnected:
            return "disconnected";
        case ClientConnectionState::Idle:
        default:
            return "idle";
    }
}

FacingDirection GameApp::AimToFacing(Vector2 aim) {
    if (Vector2LengthSqr(aim) <= 0.0001f) {
        return FacingDirection::Bottom;
    }

    const float angle = std::atan2(aim.y, aim.x);
    const float octant = std::round(angle / (PI / 4.0f));
    const int index = (static_cast<int>(octant) + 8) % 8;
    switch (index) {
        case 0:
            return FacingDirection::Right;
        case 1:
            return FacingDirection::BottomRight;
        case 2:
            return FacingDirection::Bottom;
        case 3:
            return FacingDirection::BottomLeft;
        case 4:
            return FacingDirection::Left;
        case 5:
            return FacingDirection::TopLeft;
        case 6:
            return FacingDirection::Top;
        case 7:
        default:
            return FacingDirection::TopRight;
    }
}

const char* GameApp::FacingToSpriteFacing(FacingDirection facing) {
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
        default:
            return "s";
    }
}

bool GameApp::PlayerHasEquippedWeapon(const Player& player, const char* weapon_id) const {
    if (weapon_id == nullptr || weapon_id[0] == '\0') {
        return false;
    }
    return std::find(player.weapon_slots.begin(), player.weapon_slots.end(), weapon_id) != player.weapon_slots.end();
}

const AttackProfile* GameApp::GetEquippedPrimaryAttack(const Player& player) const {
    return equipment_registry_.ResolvePrimaryAttack(player);
}

const MobilityProfile* GameApp::GetEquippedMobility(const Player& player) const {
    return equipment_registry_.ResolveMobility(player);
}

void GameApp::UpdateActiveModularAttackVisuals(float dt) {
    const auto play_random_loaded_sfx = [&](const auto& clips, Vector2 world_pos) {
        std::vector<size_t> loaded_indices;
        loaded_indices.reserve(clips.size());
        for (size_t i = 0; i < clips.size(); ++i) {
            if (clips[i].loaded) {
                loaded_indices.push_back(i);
            }
        }
        if (loaded_indices.empty()) {
            return;
        }
        std::uniform_int_distribution<size_t> dist(0, loaded_indices.size() - 1);
        const LoadedSfx& clip = clips[loaded_indices[dist(rng_)]];
        PlaySfxIfVisible(clip.sound, clip.loaded, world_pos);
    };

    std::unordered_map<int, PlayerActionState> updated_action_states;
    updated_action_states.reserve(state_.players.size());

    for (const auto& player : state_.players) {
        updated_action_states[player.id] = player.action_state;

        if (!player.alive) {
            active_modular_attack_visuals_.erase(player.id);
            continue;
        }

        if (auto active_it = active_modular_attack_visuals_.find(player.id);
            active_it != active_modular_attack_visuals_.end()) {
            const float previous_elapsed = active_it->second.elapsed_seconds;
            active_it->second.elapsed_seconds =
                std::min(active_it->second.elapsed_seconds + dt, active_it->second.duration_seconds);
            if (active_it->second.weapon_item_id == "hammer_item") {
                const float swing_time =
                    GetModularTagFrameStartSeconds(active_it->second.tag, Constants::kHammerSwingEventFrame - 1);
                if (!active_it->second.swing_event_played && active_it->second.elapsed_seconds >= swing_time &&
                    previous_elapsed < swing_time) {
                    play_random_loaded_sfx(sfx_hammer_swing_, player.pos);
                    active_it->second.swing_event_played = true;
                }

                const float impact_time =
                    GetModularTagFrameStartSeconds(active_it->second.tag, Constants::kHammerImpactEventFrame - 1);
                if (!active_it->second.impact_event_played && active_it->second.elapsed_seconds >= impact_time &&
                    previous_elapsed < impact_time) {
                    const Vector2 impact_world = ResolveHammerImpactWorldPosition(player);
                    play_random_loaded_sfx(sfx_hammer_impact_, impact_world);
                    SpawnHammerImpactEffect(impact_world);
                    camera_shake_time_remaining_ = std::max(camera_shake_time_remaining_,
                                                            Constants::kHammerImpactCameraShakeDurationSeconds);
                    active_it->second.impact_event_played = true;
                }
            }
            if (active_it->second.elapsed_seconds >= active_it->second.duration_seconds) {
                active_modular_attack_visuals_.erase(active_it);
            }
        }

        const auto previous_it = previous_player_action_states_.find(player.id);
        const PlayerActionState previous_state =
            previous_it != previous_player_action_states_.end() ? previous_it->second : PlayerActionState::Idle;
        const bool slash_started =
            player.action_state == PlayerActionState::Slashing && previous_state != PlayerActionState::Slashing;
        if (!slash_started) {
            continue;
        }

        const AttackProfile* attack_profile = GetEquippedPrimaryAttack(player);
        if (attack_profile == nullptr || attack_profile->kind != EquipmentActionKind::Melee) {
            continue;
        }
        const EquipmentItemDefinition* item =
            equipment_registry_.ResolveEquippedItem(player, EquipmentSlot::PrimaryWeapon);
        const bool use_continuous_orientation =
            item != nullptr && (item->id == "sword_item" || item->id == "hammer_item");
        const std::string attack_tag = ResolvePrimaryWeaponAttackModularTag(player);
        if (attack_tag.empty() && !use_continuous_orientation) {
            continue;
        }

        float duration_seconds = 0.0f;
        if (!attack_tag.empty()) {
            duration_seconds = modular_player_asset_.GetTagDurationSeconds("main", attack_tag);
            for (const std::string& layer_name : GetEquippedModularLayers(player)) {
                duration_seconds =
                    std::max(duration_seconds, modular_player_asset_.GetTagDurationSeconds(layer_name, attack_tag));
            }
            duration_seconds = std::max(duration_seconds, modular_player_asset_.GetTagDurationSeconds("fx", attack_tag));
        }
        duration_seconds = std::max(duration_seconds, attack_profile->active_window_seconds);
        if (duration_seconds <= 0.0f && !use_continuous_orientation) {
            continue;
        }

        ActiveModularAttackVisual visual;
        visual.tag = attack_tag;
        visual.weapon_item_id = item != nullptr ? item->id : "";
        visual.duration_seconds = duration_seconds;
        visual.attack_end_time_seconds = duration_seconds;
        visual.uses_continuous_orientation = use_continuous_orientation;
        if (use_continuous_orientation) {
            constexpr float kDiag = 0.70710678f;
            Vector2 locked_aim_dir = player.aim_dir;
            if (Vector2LengthSqr(locked_aim_dir) <= 0.0001f) {
                locked_aim_dir = {1.0f, 0.0f};
            } else {
                locked_aim_dir = Vector2Normalize(locked_aim_dir);
            }
            Vector2 snapped_dir = player.aim_dir;
            switch (player.facing) {
                case FacingDirection::Top:
                    snapped_dir = {0.0f, -1.0f};
                    break;
                case FacingDirection::TopRight:
                    snapped_dir = {kDiag, -kDiag};
                    break;
                case FacingDirection::Right:
                    snapped_dir = {1.0f, 0.0f};
                    break;
                case FacingDirection::BottomRight:
                    snapped_dir = {kDiag, kDiag};
                    break;
                case FacingDirection::Bottom:
                    snapped_dir = {0.0f, 1.0f};
                    break;
                case FacingDirection::BottomLeft:
                    snapped_dir = {-kDiag, kDiag};
                    break;
                case FacingDirection::Left:
                    snapped_dir = {-1.0f, 0.0f};
                    break;
                case FacingDirection::TopLeft:
                    snapped_dir = {-kDiag, -kDiag};
                    break;
            }
            visual.snapped_angle_radians = std::atan2(snapped_dir.y, snapped_dir.x);
            visual.locked_target_angle_radians = std::atan2(locked_aim_dir.y, locked_aim_dir.x);
            visual.impact_time_seconds = attack_profile->hit_start_seconds;
            visual.recovery_start_time_seconds = attack_profile->hit_end_seconds;
            if (visual.weapon_item_id == "hammer_item" && !attack_tag.empty()) {
                visual.impact_time_seconds = std::min(
                    visual.attack_end_time_seconds,
                    GetModularTagFrameStartSeconds(attack_tag, Constants::kHammerImpactEventFrame - 1));
            }
            visual.impact_time_seconds =
                std::clamp(visual.impact_time_seconds, 0.0f, visual.attack_end_time_seconds);
            visual.recovery_start_time_seconds =
                std::clamp(std::max(visual.impact_time_seconds, visual.recovery_start_time_seconds), 0.0f,
                           visual.attack_end_time_seconds);
        }
        if (visual.weapon_item_id == "hammer_item") {
            visual.swing_event_played = false;
            visual.impact_event_played = false;
        } else {
            visual.swing_event_played = true;
            visual.impact_event_played = true;
        }
        active_modular_attack_visuals_[player.id] = visual;
    }

    for (auto it = active_modular_attack_visuals_.begin(); it != active_modular_attack_visuals_.end();) {
        if (updated_action_states.find(it->first) == updated_action_states.end()) {
            it = active_modular_attack_visuals_.erase(it);
        } else {
            ++it;
        }
    }

    previous_player_action_states_.swap(updated_action_states);
}

float GameApp::GetModularTagFrameStartSeconds(const std::string& tag_name, int frame_index,
                                              std::string* out_layer_name) const {
    if (out_layer_name != nullptr) {
        out_layer_name->clear();
    }

    static constexpr std::array<const char*, 5> kCandidateLayers = {"main", "hammer", "sword", "ghook", "fx"};
    for (const char* layer_name : kCandidateLayers) {
        if (!modular_player_asset_.HasTag(layer_name, tag_name)) {
            continue;
        }
        if (out_layer_name != nullptr) {
            *out_layer_name = layer_name;
        }
        return modular_player_asset_.GetTagFrameStartSeconds(layer_name, tag_name, frame_index);
    }
    return 0.0f;
}

std::vector<std::string> GameApp::GetEquippedModularLayers(const Player& player) const {
    return equipment_registry_.CollectVisibleLayers(player);
}

std::string GameApp::ResolveSwordAttackModularTag(const Player& player) const {
    if (!PlayerHasEquippedWeapon(player, "sword_item")) {
        return "";
    }

    const std::string facing_specific_tag = ModularCharacterAsset::BuildTag("sword_attack", player.facing);
    if (modular_player_asset_.HasTag("main", facing_specific_tag)) {
        return facing_specific_tag;
    }

    const std::string east_fallback_tag = ModularCharacterAsset::BuildTag("sword_attack", FacingDirection::Right);
    if (modular_player_asset_.HasTag("main", east_fallback_tag)) {
        return east_fallback_tag;
    }

    return "";
}

std::string GameApp::ResolvePrimaryWeaponAttackModularTag(const Player& player) const {
    const EquipmentItemDefinition* item =
        equipment_registry_.ResolveEquippedItem(player, EquipmentSlot::PrimaryWeapon);
    if (item == nullptr) {
        return "";
    }
    if (item->id == "sword_item") {
        return ResolveSwordAttackModularTag(player);
    }
    if (item->id != "hammer_item") {
        return "";
    }

    const auto has_tag_any_layer = [&](const std::string& tag) {
        if (modular_player_asset_.HasTag("main", tag) || modular_player_asset_.HasTag("hammer", tag) ||
            modular_player_asset_.HasTag("fx", tag)) {
            return true;
        }
        return false;
    };

    std::vector<std::string> candidates;
    candidates.push_back(ModularCharacterAsset::BuildTag("hammer_attack", player.facing));
    if (player.facing == FacingDirection::Left) {
        candidates.push_back("wizhammer_attack_w");
    }
    candidates.push_back(ModularCharacterAsset::BuildTag("hammer_attack", FacingDirection::TopLeft));
    candidates.push_back(ModularCharacterAsset::BuildTag("hammer_attack", FacingDirection::TopRight));
    candidates.push_back("wizhammer_attack_w");

    for (const std::string& candidate : candidates) {
        if (!candidate.empty() && has_tag_any_layer(candidate)) {
            return candidate;
        }
    }
    return "";
}

Vector2 GameApp::ResolveHammerImpactWorldPosition(const Player& player) const {
    const float rotation = GetPlayerLockedMeleeAimRadians(player);
    Vector2 aim_dir = {std::cos(rotation), std::sin(rotation)};
    return Vector2Add(player.pos, Vector2Scale(aim_dir, Constants::kHammerImpactOffsetPixels));
}

Rectangle GameApp::GetPlayerCollisionRect(const Player& player) const {
    return GetPlayerCollisionRect(player.pos);
}

Rectangle GameApp::GetPlayerCollisionRect(Vector2 center) const {
    center.y += Constants::kPlayerHitboxOffsetY;
    return MakeCenteredRect(center, Constants::kPlayerHitboxWidth, Constants::kPlayerHitboxHeight);
}

float GameApp::GetPlayerCollisionSupportDistance(Vector2 direction) const {
    if (Vector2LengthSqr(direction) <= 0.0001f) {
        direction = {1.0f, 0.0f};
    } else {
        direction = Vector2Normalize(direction);
    }
    const float half_width = Constants::kPlayerHitboxWidth * 0.5f;
    const float half_height = Constants::kPlayerHitboxHeight * 0.5f;
    return std::fabs(direction.x) * half_width + std::fabs(direction.y) * half_height;
}

Vector2 GameApp::GetPlayerMeleeRotationOrigin(const Player& player) const {
    const Rectangle collision = GetPlayerCollisionRect(player);
    return {collision.x + collision.width * 0.5f, collision.y + collision.height * 0.5f - 4.0f};
}

bool GameApp::TryEquipItem(Player& player, const std::string& item_id, const MapObjectInstance* source_object) {
    const EquipmentItemDefinition* item = equipment_registry_.FindItem(item_id);
    if (item == nullptr || item->slot == EquipmentSlot::Inventory) {
        return false;
    }
    const size_t slot_index = item->slot == EquipmentSlot::PrimaryWeapon ? 0 : 1;
    if (slot_index >= player.weapon_slots.size()) {
        return false;
    }
    if (player.weapon_slots[slot_index] == item_id) {
        return false;
    }
    if (!settings_.auto_pick_replace_equipment && !player.weapon_slots[slot_index].empty() &&
        player.weapon_slots[slot_index] != item_id) {
        return false;
    }

    if (!player.weapon_slots[slot_index].empty() && player.weapon_slots[slot_index] != item_id &&
        source_object != nullptr) {
        SpawnDroppedEquipmentItem(player.weapon_slots[slot_index], source_object->cell, player.id);
    }

    player.weapon_slots[slot_index] = item_id;
    return true;
}

void GameApp::RegisterSpellRuntimes() {
    spell_runtime_registry_.Register("fire_bolt",
                                     [](const SpellRuntimeMatch& match, const SpellRuntimeContext& context) {
                                         if (context.state == nullptr || context.event_queue == nullptr) {
                                             return false;
                                         }
                                         FireBoltSpell spell(context.state->map.CellCenterWorld(match.cast_origin),
                                                             match.caster_player_id, match.direction,
                                                             match.static_fire_bolt);
                                         spell.Cast(*context.state, *context.event_queue);
                                         return true;
                                     });
    spell_runtime_registry_.Register("ice_wall",
                                     [](const SpellRuntimeMatch& match, const SpellRuntimeContext& context) {
                                         if (context.state == nullptr || context.event_queue == nullptr) {
                                             return false;
                                         }
                                         IceWallSpell spell(match.cast_origin, match.caster_player_id, match.direction);
                                         spell.Cast(*context.state, *context.event_queue);
                                         return true;
                                     });
    spell_runtime_registry_.Register("ice_wave",
                                     [](const SpellRuntimeMatch& match, const SpellRuntimeContext& context) {
                                         if (context.state == nullptr || context.event_queue == nullptr) {
                                             return false;
                                         }
                                         Vector2 cast_position = context.state->map.CellCenterWorld(match.cast_origin);
                                         if (!match.matched_cells.empty()) {
                                             Vector2 accumulated = {0.0f, 0.0f};
                                             for (const GridCoord& cell : match.matched_cells) {
                                                 const Vector2 center = context.state->map.CellCenterWorld(cell);
                                                 accumulated.x += center.x;
                                                 accumulated.y += center.y;
                                             }
                                             const float inv_count = 1.0f / static_cast<float>(match.matched_cells.size());
                                             cast_position = {accumulated.x * inv_count, accumulated.y * inv_count};
                                         }
                                         IceWaveSpell spell(cast_position,
                                                            match.caster_player_id, match.direction);
                                         spell.Cast(*context.state, *context.event_queue);
                                         return true;
                                     });
    spell_runtime_registry_.Register("fire_storm",
                                     [](const SpellRuntimeMatch& match, const SpellRuntimeContext& context) {
                                         if (context.state == nullptr) {
                                             return false;
                                         }
                                         FireStormCast cast;
                                         cast.id = context.state->next_entity_id++;
                                         cast.owner_player_id = match.caster_player_id;
                                         cast.owner_team = match.caster_team;
                                         cast.center_cell = match.cast_origin;
                                         for (const GridCoord& cell : match.matched_cells) {
                                             const auto rune_it =
                                                 std::find_if(context.state->runes.begin(), context.state->runes.end(),
                                                              [&](const Rune& rune) {
                                                                  return rune.cell == cell && rune.active &&
                                                                         rune.rune_type == RuneType::Fire;
                                                              });
                                             if (rune_it != context.state->runes.end()) {
                                                 cast.source_cells.push_back(cell);
                                             }
                                         }
                                         const float radius_world =
                                             Constants::kFireStormConversionRadiusTiles *
                                             static_cast<float>(context.state->map.cell_size);
                                         const Vector2 center_world = context.state->map.CellCenterWorld(match.cast_origin);
                                         for (const Rune& rune : context.state->runes) {
                                             if (!rune.active || rune.rune_type != RuneType::Fire) {
                                                 continue;
                                             }
                                             if (Vector2Distance(context.state->map.CellCenterWorld(rune.cell), center_world) >
                                                 radius_world) {
                                                 continue;
                                             }
                                             cast.target_cells.push_back(rune.cell);
                                         }
                                         for (const GridCoord& source_cell : cast.source_cells) {
                                             if (std::find(cast.target_cells.begin(), cast.target_cells.end(), source_cell) ==
                                                 cast.target_cells.end()) {
                                                 cast.target_cells.push_back(source_cell);
                                             }
                                         }
                                         cast.elapsed_seconds = 0.0f;
                                         cast.duration_seconds =
                                             context.get_effect_duration_seconds != nullptr
                                                 ? context.get_effect_duration_seconds("fire_storm_cast")
                                                 : 0.0f;
                                         cast.duration_seconds = std::max(
                                             cast.duration_seconds,
                                             GetFireStormConversionTriggerSeconds() + 0.1f);
                                         cast.alive = true;
                                         context.state->fire_storm_casts.push_back(cast);
                                         return true;
                                     });
    spell_runtime_registry_.Register("fire_flower",
                                     [](const SpellRuntimeMatch& match, const SpellRuntimeContext& context) {
                                         if (context.state == nullptr || context.event_queue == nullptr) {
                                             return false;
                                         }
                                         FireFlowerSpell spell(match.cast_origin, match.caster_player_id, match.direction);
                                         spell.Cast(*context.state, *context.event_queue);
                                         return true;
                                     });
}

bool GameApp::CastRuntimeSpell(const SpellRuntimeMatch& match) {
    const size_t map_object_count_before = state_.map_objects.size();
    const SpellRuntimeContext context = {
        &state_,
        &event_queue_,
        [&](const std::string& effect_id) { return GetCompositeEffectDurationSeconds(effect_id); },
    };
    const bool cast_succeeded = spell_runtime_registry_.Cast(match, context);
    if (state_.map_objects.size() != map_object_count_before) {
        // Some spells construct map objects directly instead of going through SpawnObjectInstanceAtCell().
        // Keep the shared id lookup and render buckets in sync with those spell-side mutations.
        RebuildMapObjectIndexLookup();
        MarkMapObjectRenderCachesDirty();
    }
    return cast_succeeded;
}

std::string GameApp::ResolvePlayerModularAnimationName(const Player& player) const {
    const bool is_moving = Vector2LengthSqr(player.vel) > 8.0f;
    if (player.action_state == PlayerActionState::RunePlacing) {
        return is_moving ? "walk" : "idle";
    }
    return is_moving ? "walk" : "idle";
}

std::string GameApp::ResolvePlayerModularTag(const Player& player) const {
    if (const auto active_it = active_modular_attack_visuals_.find(player.id);
        active_it != active_modular_attack_visuals_.end() && !active_it->second.tag.empty()) {
        return active_it->second.tag;
    }

    if (player.action_state == PlayerActionState::Slashing) {
        const std::string attack_tag = ResolvePrimaryWeaponAttackModularTag(player);
        if (!attack_tag.empty()) {
            return attack_tag;
        }
    }

    return ModularCharacterAsset::BuildTag(ResolvePlayerModularAnimationName(player).c_str(), player.facing);
}

float GameApp::ResolvePlayerModularTime(const Player& player) const {
    if (const auto active_it = active_modular_attack_visuals_.find(player.id);
        active_it != active_modular_attack_visuals_.end()) {
        return active_it->second.elapsed_seconds;
    }
    return render_time_seconds_;
}

float GameApp::GetPlayerDamageFlashAmount(const Player& player) const {
    const auto it = player_damage_flash_remaining_.find(player.id);
    if (it == player_damage_flash_remaining_.end()) {
        return 0.0f;
    }
    return ComputeDamageFlashAmount(it->second);
}

float GameApp::GetObjectDamageFlashAmount(const MapObjectInstance& object) const {
    const auto it = object_damage_flash_remaining_.find(object.id);
    if (it == object_damage_flash_remaining_.end()) {
        return 0.0f;
    }
    return ComputeDamageFlashAmount(it->second);
}

Rectangle GameApp::GetPlayerSpriteRect(Vector2 draw_pos, const std::string& layer_name) const {
    const float frame_width = static_cast<float>(modular_player_asset_.GetFrameWidth(layer_name));
    const float frame_height = static_cast<float>(modular_player_asset_.GetFrameHeight(layer_name));
    const float draw_width = (frame_width > 0.0f ? frame_width : 32.0f) * Constants::kPlayerVisualScale;
    const float draw_height = (frame_height > 0.0f ? frame_height : 32.0f) * Constants::kPlayerVisualScale;
    return SnapRect({draw_pos.x - draw_width * 0.5f, draw_pos.y - draw_height * 0.5f, draw_width, draw_height});
}

void GameApp::RenderPlayerModularLayers(const Player& player, Vector2 draw_pos) const {
    const std::string tag = ResolvePlayerModularTag(player);
    const float modular_time = ResolvePlayerModularTime(player);
    const float rotation = GetPlayerAttackVisualRotationDegrees(player);
    const float flash_amount = GetPlayerDamageFlashAmount(player);
    const bool use_damage_flash = flash_amount > 0.0f && has_damage_flash_shader_;

    if (use_damage_flash) {
        SetShaderValue(damage_flash_shader_, damage_flash_amount_loc_, &flash_amount, SHADER_UNIFORM_FLOAT);
        BeginShaderMode(damage_flash_shader_);
    }

    const auto draw_layer = [&](const char* layer_name) {
        if (!modular_player_asset_.HasTag(layer_name, tag)) {
            return;
        }
        const int team_index = std::strcmp(layer_name, "main") == 0 ? player.team : -1;
        const Texture2D* texture = modular_player_asset_.GetLayerTexture(layer_name, team_index);
        if (texture == nullptr) {
            return;
        }
        const Rectangle src =
            InsetSourceRect(modular_player_asset_.GetFrame(layer_name, tag, modular_time),
                            Constants::kAtlasSampleInsetPixels);
        Rectangle dst = GetPlayerSpriteRect(draw_pos, layer_name);
        const Vector2 origin = {draw_pos.x - dst.x, draw_pos.y - dst.y};
        dst.x += origin.x;
        dst.y += origin.y;
        DrawTexturePro(*texture, src, dst, origin, rotation, WHITE);
    };

    draw_layer("main");
    for (const std::string& layer_name : GetEquippedModularLayers(player)) {
        if (layer_name == "main" || layer_name == "shadow") {
            continue;
        }
        draw_layer(layer_name.c_str());
    }
    draw_layer("fx");

    if (use_damage_flash) {
        EndShaderMode();
    }
}

void GameApp::RenderPlayerAttachedAnimation(const std::string& animation_key, float age_seconds, float offset_x,
                                            float offset_y, Vector2 draw_pos) const {
    if (!sprite_metadata_.IsLoaded() || !sprite_metadata_.HasAnimation(animation_key)) {
        return;
    }

    const Texture2D texture = sprite_metadata_.GetTexture();
    const Rectangle src = InsetSourceRect(sprite_metadata_.GetFrame(animation_key, "default", age_seconds),
                                          Constants::kAtlasSampleInsetPixels);
    const float frame_width = static_cast<float>(sprite_metadata_.GetCellWidth() > 0 ? sprite_metadata_.GetCellWidth() : 32);
    const float frame_height =
        static_cast<float>(sprite_metadata_.GetCellHeight() > 0 ? sprite_metadata_.GetCellHeight() : 32);
    const Rectangle dst = SnapRect(
        {draw_pos.x - frame_width * 0.5f + offset_x, draw_pos.y - frame_height * 0.5f + offset_y, frame_width, frame_height});
    DrawTexturePro(texture, src, dst, {0, 0}, 0.0f, WHITE);
}
