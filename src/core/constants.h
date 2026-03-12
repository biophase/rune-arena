#pragma once

#include <array>
#include <string_view>

namespace Constants {

constexpr int kWindowWidth = 1280;
constexpr int kWindowHeight = 720;
constexpr int kDefaultPort = 7967;
constexpr int kDiscoveryPort = 7968;

constexpr double kFixedDt = 1.0 / 60.0;
constexpr float kNetworkSnapshotIntervalSeconds = 1.0f / 40.0f;
constexpr int kNetworkSnapshotKeyframeIntervalTicks = 18;
constexpr int kNetworkSnapshotHistorySize = 128;
constexpr float kCameraZoom = 3.0f;
constexpr float kClientVisualSmoothing = 16.0f;
constexpr float kRemoteInterpolationDelaySeconds = 0.08f; // reduce to improve feel
constexpr float kPredictionHardSnapThresholdPx = 32.0f;
constexpr float kPredictionReconcileGain = 8.0f;
constexpr int kMoveInputBufferSize = 256;
constexpr float kDefaultShrinkTilesPerSecond = 1.0f;
constexpr float kShrinkTilesPerSecondStep = 0.25f;
constexpr float kMaxShrinkTilesPerSecond = 6.0f;
constexpr float kDefaultMinArenaRadiusTiles = 6.0f;
constexpr float kMinArenaRadiusTilesStep = 1.0f;
constexpr float kMaxArenaRadiusTiles = 256.0f;

constexpr float kPlayerRadius = 12.0f;
constexpr float kPlayerAcceleration = 900.0f;
constexpr float kPlayerFriction = 8.0f;
constexpr float kPlayerMaxSpeed = 220.0f;

constexpr float kProjectileRadius = 4.0f;
constexpr float kProjectileSpeed = 420.0f;
constexpr int kProjectileDamage = 34;
constexpr float kFireBoltExplosionRadius = 20.0f;
constexpr float kFireBoltExplosionFallbackDuration = 0.5f;
constexpr float kIceWallMaterializeSeconds = 0.3f;
constexpr int kIceWallLengthCells = 5;
constexpr float kIceWallMaxHp = 100.0f;
constexpr float kIceWallHpDecayPerSecond = 10.0f;
constexpr int kProjectileSmokeEmitEveryFrames = 2;
constexpr int kProjectileSmokeParticlesPerBurst = 5;
constexpr float kProjectileSmokeBackVelocityFactor = -0.08f;
constexpr float kSmokeEmitterOffsetStdDev = 4.0f;
constexpr float kSmokeEmitterVelocityJitterStdDev = 10.0f;

constexpr float kMeleeCooldownSeconds = 0.3f;
constexpr float kMeleeActiveWindowSeconds = 0.3f;
constexpr float kMeleeHitStartSeconds = 0.1f;
constexpr float kMeleeHitEndSeconds = 0.3f;
constexpr float kMeleeRange = 32.0f;
constexpr float kMeleeHitRadius = 14.0f;
constexpr int kMeleeDamage = 24;
constexpr float kMeleeAttackVisualScale = 2.0f;
constexpr float kRunePlaceCooldownSeconds = 0.5f;

constexpr int kMatchDurationSeconds = 120;
constexpr int kMaxHp = 100;
constexpr float kRespawnDelaySeconds = 3.0f;
constexpr float kArenaUnsafeDamagePerSecond = 5.0f;
constexpr float kArenaSpawnBufferTiles = 3.0f;
constexpr float kOutsideZoneTileBrightness = 0.5f;
constexpr float kPlayerHealthBarWidth = 32.0f;
constexpr float kPlayerHealthBarHeight = 4.0f;
constexpr float kPlayerHealthBarOffsetY = 11.0f;
constexpr int kPlayerHealthTextFontSize = 6;
constexpr int kPlayerHealthBarFillR = 189;
constexpr int kPlayerHealthBarFillG = 217;
constexpr int kPlayerHealthBarFillB = 65;
constexpr int kPlayerHealthBarMissingR = 200;
constexpr int kPlayerHealthBarMissingG = 204;
constexpr int kPlayerHealthBarMissingB = 182;
constexpr float kRuneCooldownBarWidth = 128.0f;
constexpr float kRuneCooldownBarHeight = 8.0f;
constexpr int kRuneCooldownTextFontSize = 14;

constexpr float kDamagePopupLifetimeSeconds = 0.75f;
constexpr float kDamagePopupRisePerSecond = 20.0f;
constexpr int kDamagePopupFontSize = 11;
constexpr float kCameraShakeDurationSeconds = 0.12f;
constexpr float kCameraShakePixels = 7.0f;
// Vertical anchor inside the lightning sprite used for start/end alignment.
// 0.5 keeps the beam centerline on the world-space start/end points.
constexpr float kLightningAnchorYRatio = 0.5f;

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
constexpr const char* kSpriteMetadataTallPath = "assets/sprite_sheet_32x64.json";
constexpr const char* kSpellPatternPath = "assets/spell_patterns.json";
constexpr const char* kObjectsConfigPath = "assets/objects.json";
constexpr const char* kMenuBackgroundPath = "assets/menu_background.png";
constexpr const char* kSfxFireballCreatedPath = "assets/sfx/ogg/SFX/Spells/Fireball 1.ogg";
constexpr const char* kSfxMeleeAttackPath = "assets/sfx/ogg/SFX/Attacks/Sword Attacks Hits and Blocks/Sword Attack 1.ogg";
constexpr const char* kSfxCreateRunePath = "assets/sfx/ogg/SFX/Torch/Light Torch 1.ogg";
constexpr const char* kSfxExplosionPath = "assets/sfx/ogg/SFX/Spells/Spell Impact 1.ogg";
constexpr const char* kSfxVaseBreakingPath = "assets/sfx/ogg/SFX/Torch/Torch Impact 2.ogg";
constexpr const char* kSfxIceWallFreezePath = "assets/sfx/ogg/SFX/Spells/Ice Freeze 1.ogg";
constexpr const char* kSfxIceWallMeltPath = "assets/sfx/ogg/SFX/Spells/Ice Wall 2.ogg";
constexpr const char* kSfxPlayerDeathPath = "assets/sfx/ogg/SFX/Spells/Spell Impact 3.ogg";
constexpr const char* kSfxPlayerDamagedPath = "assets/sfx/ogg/SFX/Attacks/Sword Attacks Hits and Blocks/Sword Impact Hit 1.ogg";
constexpr const char* kSfxDrinkPotionPath = "assets/sfx/ogg/SFX/Spells/Waterspray 1.ogg";
constexpr const char* kBgmForestDayPath = "assets/sfx/ogg/BGS Loops/Forest Day/Forest Day.ogg";

constexpr float kBgmVolume = 0.35f;
constexpr float kSfxVolumeFireballCreated = 0.70f;
constexpr float kSfxVolumeMeleeAttack = 0.60f;
constexpr float kSfxVolumeCreateRune = 0.70f;
constexpr float kSfxVolumeExplosion = 0.80f;
constexpr float kSfxVolumeVaseBreaking = 0.80f;
constexpr float kSfxVolumeIceWallFreeze = 0.80f;
constexpr float kSfxVolumeIceWallMelt = 0.75f;
constexpr float kSfxVolumePlayerDeath = 0.85f;
constexpr float kSfxVolumePlayerDamaged = 0.60f;
constexpr float kSfxVolumeDrinkPotion = 0.65f;

constexpr const char* kConfigFolderName = "RuneArena";
constexpr const char* kConfigFileName = "config.json";
constexpr const char* kControlsFileName = "controls.json";

}  // namespace Constants
