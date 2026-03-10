#pragma once

#include <array>
#include <string_view>

namespace Constants {

constexpr int kWindowWidth = 1280;
constexpr int kWindowHeight = 720;
constexpr int kDefaultPort = 7777;
constexpr int kDiscoveryPort = 7778;

constexpr double kFixedDt = 1.0 / 60.0;
constexpr float kNetworkSnapshotIntervalSeconds = 1.0f / 20.0f;
constexpr float kCameraZoom = 3.0f;

constexpr float kPlayerRadius = 12.0f;
constexpr float kPlayerAcceleration = 900.0f;
constexpr float kPlayerFriction = 8.0f;
constexpr float kPlayerMaxSpeed = 220.0f;

constexpr float kProjectileRadius = 4.0f;
constexpr float kProjectileSpeed = 420.0f;
constexpr int kProjectileDamage = 34;
constexpr int kProjectileSmokeEmitEveryFrames = 2;
constexpr int kProjectileSmokeParticlesPerBurst = 5;
constexpr float kProjectileSmokeBackVelocityFactor = -0.08f;
constexpr float kSmokeEmitterOffsetStdDev = 4.0f;
constexpr float kSmokeEmitterVelocityJitterStdDev = 10.0f;

constexpr float kMeleeCooldownSeconds = 0.45f;
constexpr float kMeleeActiveWindowSeconds = 0.12f;
constexpr float kMeleeRange = 20.0f;
constexpr float kMeleeHitRadius = 14.0f;
constexpr int kMeleeDamage = 24;
constexpr float kRunePlaceCooldownSeconds = 1.0f;

constexpr int kMatchDurationSeconds = 120;
constexpr int kMaxHp = 100;

constexpr int kRuneCellSize = 32;
constexpr int kPlacementPreviewRadiusCells = 3;
constexpr float kRunePreviewAlpha = 0.6f;

constexpr int kSpatialCellSize = 64;
constexpr float kAtlasSampleInsetPixels = 0.35f;

constexpr int kTeamRed = 0;
constexpr int kTeamBlue = 1;

constexpr std::array<std::string_view, 1> kPlaceableTiles = {"tile_grass"};

constexpr const char* kDefaultMapPath = "assets/maps/default.png";
constexpr const char* kTileMappingPath = "assets/maps/tiles_mapping.json";
constexpr const char* kSpriteMetadataPath = "assets/sprite_sheet.json";
constexpr const char* kSpellPatternPath = "assets/spell_patterns.json";

constexpr const char* kConfigFolderName = "RuneArena";
constexpr const char* kConfigFileName = "config.json";

}  // namespace Constants
