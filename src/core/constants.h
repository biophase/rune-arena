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
constexpr float kCameraZoom = 2.0f;
constexpr float kClientVisualSmoothing = 16.0f;
constexpr float kRemoteInterpolationDelaySeconds = 0.08f; // reduce to improve feel
constexpr float kPredictionHardSnapThresholdPx = 32.0f;
constexpr float kPredictionReconcileGain = 8.0f;
constexpr float kPredictionCorrectionTelemetryThresholdPx = 1.0f;
constexpr float kPredictionHardSnapThresholdGrapplingPx = 128.0f;
constexpr float kPredictionReconcileGainGrappling = 2.0f;
constexpr float kPredictionCorrectionTelemetryThresholdGrapplingPx = 8.0f;
constexpr int kMoveInputBufferSize = 256;
constexpr float kDefaultShrinkTilesPerSecond = 1.0f;
constexpr float kShrinkTilesPerSecondStep = 0.25f;
constexpr float kMaxShrinkTilesPerSecond = 6.0f;
constexpr float kDefaultMinArenaRadiusTiles = 6.0f;
constexpr float kMinArenaRadiusTilesStep = 1.0f;
constexpr float kMaxArenaRadiusTiles = 256.0f;

constexpr float kPlayerRadius = 15.0f;
constexpr float kPlayerAcceleration = 600.0f;
constexpr float kPlayerFriction = 8.0f;
constexpr float kPlayerMaxSpeed = 160.0f;
constexpr float kCollisionResolveBlendFactor = 0.6f;
constexpr float kCollisionResolveMinStep = 2.0f;
constexpr float kCollisionResolveSnapDistance = 3.0f;

constexpr float kProjectileRadius = 4.0f;
constexpr float kFireBoltScale = 1.4f;
constexpr float kStaticFireBoltScaleMultiplier = 1.2f;
constexpr float kStaticFireBoltUpgradePauseSeconds = 0.15f;
constexpr float kStaticFireBoltSpeedMultiplier = 1.8f;
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

constexpr float kMeleeCooldownSeconds = 0.5f;
constexpr float kMeleeActiveWindowSeconds = 0.25f;
constexpr float kMeleeHitStartSeconds = 0.1f;
constexpr float kMeleeHitEndSeconds = 0.25f;
constexpr float kMeleeRange = 32.0f;
constexpr float kMeleeHitRadius = 14.0f;
constexpr int kMeleeDamage = 12;
constexpr float kMeleeAttackVisualScale = 1.8f;
constexpr float kGrapplingHookRangeTiles = 8.0f;
constexpr float kGrapplingHookCooldownSeconds = 1.5f;
constexpr float kGrapplingHookFireSpeed = 420.0f;
constexpr float kGrapplingHookPullSpeed = 345.0f;
constexpr float kGrapplingHookRetractSpeedMultiplier = 1.3f;
constexpr float kGrapplingHookCollisionProbeStep = 4.0f;
constexpr float kGrapplingHookArrivalEpsilon = 12.0f;
constexpr float kRunePlaceCooldownSeconds = 0.5f;
constexpr float kRuneMinCrossCooldownSeconds = 0.5f;
constexpr float kRuneManaCostFire = 6.0f;
constexpr float kRuneManaCostWater = 4.0f;
constexpr float kRuneManaCostCatalyst = 2.0f;
constexpr float kRuneManaCostEarth = 7.0f;
constexpr float kRuneManaCostFireStormDummy = 0.0f;
constexpr float kDefaultManaRegenPerSecond = 1.0f;
constexpr float kDefaultPlayerMaxMana = 50.0f;
constexpr float kRuneCooldownFireSeconds = 1.3f;
constexpr float kRuneCooldownWaterSeconds = 1.1f;
constexpr float kRuneCooldownCatalystSeconds = 0.5f;
constexpr float kRuneCooldownEarthSeconds = 1.5f;
constexpr float kRuneCooldownFireStormDummySeconds = 0.5f;
constexpr float kRuneActivationFireSeconds = 0.2f;
constexpr float kRuneActivationWaterSeconds = 0.2f;
constexpr float kRuneActivationCatalystSeconds = 0.2f;
constexpr float kRuneActivationEarthSeconds = 0.2f;
constexpr float kRuneActivationFireStormDummySeconds = 0.0f;
constexpr float kEarthRuneSlamSpawnRootsProgress = 0.7f;
constexpr float kEarthRuneSlamSlowdown = 1.5f;
constexpr float kEarthRuneTrapPersistSeconds = 4.0f;
constexpr float kEarthRootedBurnInSeconds = 2.0f;
constexpr float kEarthRootedRecoverSeconds = 0.2f;
constexpr float kEarthRootedVisibleDurationSeconds = 3.0f;
constexpr float kEarthRootedDamagePerSecond = 2.0f;
constexpr float kEarthRuneTrapRangeTiles = 1.0f;
constexpr float kEarthRuneSlamFallbackSeconds = 1.0f;
constexpr float kEarthRuneRootedDeathFallbackSeconds = 0.5f;
constexpr int kPotionSmallImmediateHeal = 14;
constexpr float kPotionSmallRegenPerSecond = 6.0f;
constexpr float kPotionSmallRegenDurationSeconds = 6.0f;
constexpr float kStaticFireBoltStunSeconds = 2.5f;
constexpr float kFireStormDummyLightningChancePerFrame = 0.04f;
constexpr int kFireStormDummyLightningMinFrames = 3;
constexpr int kFireStormDummyLightningMaxFrames = 8;
constexpr int kFireStormDummyLightningCooldownFrames = 8;
constexpr float kFireStormLifetimeSeconds = 30.0f;
constexpr float kRuneVolatileManaMultiplier = 3.0f;
constexpr float kRuneVolatileActivationMultiplier = 2.0f;
constexpr float kRuneCastRangeTiles = 12.0f;
constexpr int kHudRuneSlotCount = 4;
constexpr int kHudItemSlotCount = 4;
constexpr int kHudWeaponSlotCount = 2;
constexpr int kHudTotalSlotCount = 10;
constexpr float kHudScale = 2.0f;
constexpr float kHudPanelWidth = 480.0f;
constexpr float kHudPanelHeight = 112.0f;
constexpr float kHudSlotSize = 42.0f;
constexpr float kHudSlotGap = 4.0f;
constexpr float kHudBarWidth = 180.0f;
constexpr float kHudBarHeight = 10.0f;
constexpr int kHudBgR = 55;
constexpr int kHudBgG = 58;
constexpr int kHudBgB = 64;
constexpr int kHudBgA = 220;
constexpr int kHudManaBarR = 119;
constexpr int kHudManaBarG = 146;
constexpr int kHudManaBarB = 168;
constexpr int kHudLowManaTintR = 120;
constexpr int kHudLowManaTintG = 170;
constexpr int kHudLowManaTintB = 255;
constexpr float kHudLowManaBrightness = 0.55f;
constexpr float kInventoryDimAlpha = 0.45f;
constexpr float kInfluenceZoneAlpha = 0.6f;
constexpr int kDefaultMatchDurationSeconds = 300;
constexpr int kDefaultBestOfTargetKills = 5;
constexpr float kDefaultShrinkStartSeconds = 150.0f;
constexpr int kLobbyTimeStepSeconds = 30;

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
constexpr int kPlayerHealthBarMissingR = 92;
constexpr int kPlayerHealthBarMissingG = 92;
constexpr int kPlayerHealthBarMissingB = 92;
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
constexpr float kRuneCastRangeWorld = kRuneCastRangeTiles * static_cast<float>(kRuneCellSize);
constexpr int kPlacementPreviewRadiusCells = 3;
constexpr float kRunePreviewAlpha = 0.6f;

constexpr int kSpatialCellSize = 64;
constexpr float kAtlasSampleInsetPixels = 0.35f;

constexpr int kTeamRed = 0;
constexpr int kTeamBlue = 1;

constexpr std::array<std::string_view, 1> kPlaceableTiles = {"tile_grass"};

constexpr const char* kDefaultMapPath = "assets/maps/default.png";
constexpr const char* kSpriteMetadataPath = "assets/sprite_sheet.json";
constexpr const char* kSpriteMetadataTallPath = "assets/sprite_sheet_32x64.json";
constexpr const char* kSpriteMetadata96x96Path = "assets/sprite_sheet_96x96.json";
constexpr const char* kSpellPatternPath = "assets/spell_patterns.json";
constexpr const char* kObjectsConfigPath = "assets/objects.json";
constexpr const char* kCompositeEffectsPath = "assets/composite_effects.json";
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
constexpr const char* kSfxItemPickupPath = "assets/sfx/ogg/SFX/Doors Gates and Chests/Lock Unlock.ogg";
constexpr const char* kSfxDrinkPotionPath = "assets/sfx/ogg/SFX/Spells/Waterspray 1.ogg";
constexpr const char* kSfxFireStormCastPath = "assets/sfx/EM_LIGHT_LAUNCH_01.ogg";
constexpr const char* kSfxFireStormImpactPath = "assets/sfx/EM_FIRE_IMPACT_01.ogg";
constexpr const char* kSfxStaticUpgradePath = "assets/sfx/EM_LIGHT_CAST_02_S.ogg";
constexpr const char* kSfxStaticBoltImpactPath = "assets/sfx/EM_LIGHT_IMPACT_01.ogg";
constexpr const char* kSfxGrapplingThrowPath =
    "assets/sfx/ogg/SFX/Attacks/Bow Attacks Hits and Blocks/Bow Attack 1.ogg";
constexpr const char* kSfxGrapplingLatchPath =
    "assets/sfx/ogg/SFX/Attacks/Bow Attacks Hits and Blocks/Bow Impact Hit 1.ogg";
constexpr const char* kSfxEarthRuneLaunchPath = "assets/sfx/EM_EARTH_LAUNCH_01.ogg";
constexpr const char* kSfxEarthRuneImpactPath = "assets/sfx/EM_EARTH_IMPACT_01.ogg";
constexpr std::array<const char*, 5> kSfxFootstepDirtPaths = {
    "assets/sfx/ogg/SFX/Footsteps/Dirt/Dirt Walk 1.ogg",
    "assets/sfx/ogg/SFX/Footsteps/Dirt/Dirt Walk 2.ogg",
    "assets/sfx/ogg/SFX/Footsteps/Dirt/Dirt Walk 3.ogg",
    "assets/sfx/ogg/SFX/Footsteps/Dirt/Dirt Walk 4.ogg",
    "assets/sfx/ogg/SFX/Footsteps/Dirt/Dirt Walk 5.ogg",
};
constexpr const char* kFireStormAmbientPath = "assets/sfx/EM_FIRE_HOLD_4s.ogg";
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
constexpr float kSfxVolumeItemPickup = 0.55f;
constexpr float kSfxVolumeDrinkPotion = 0.65f;
constexpr float kSfxVolumeFireStormCast = 0.75f;
constexpr float kSfxVolumeFireStormImpact = 0.72f;
constexpr float kSfxVolumeStaticUpgrade = 0.72f;
constexpr float kSfxVolumeStaticBoltImpact = 0.78f;
constexpr float kSfxVolumeGrapplingThrow = 0.68f;
constexpr float kSfxVolumeGrapplingLatch = 0.72f;
constexpr float kSfxVolumeEarthRuneLaunch = 0.74f;
constexpr float kSfxVolumeEarthRuneImpact = 0.78f;
constexpr float kSfxVolumeFootstepDirt = 0.50f;
constexpr float kFireStormAmbientVolume = 0.42f;
constexpr float kFireStormAmbientLoopStartSeconds = 0.20f;
constexpr float kFireStormAmbientLoopEndSeconds = 3.10f;
constexpr float kFireStormAmbientFadeSeconds = 0.20f;
constexpr float kFireStormImpactDelaySeconds = 0.9f;

constexpr const char* kConfigFolderName = "RuneArena";
constexpr const char* kConfigFileName = "config.json";
constexpr const char* kControlsFileName = "controls.json";

}  // namespace Constants
