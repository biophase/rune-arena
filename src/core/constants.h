#pragma once

#include <array>
#include <string_view>

namespace Constants {

constexpr int kWindowWidth = 1280;
constexpr int kWindowHeight = 720;
constexpr int kDefaultPort = 7967;
constexpr int kDiscoveryPort = 7968;
constexpr int kLobbyMapPreviewWidth = 192;
constexpr int kLobbyMapPreviewHeight = 192;
constexpr int kLobbyMapPreviewSupersampleFactor = 4;
constexpr int kLobbyMapPreviewGaussianBlurSize = 2;
constexpr size_t kMapTransferChunkBytes = 24 * 1024;

constexpr double kFixedDt = 1.0 / 60.0;
constexpr float kNetworkSnapshotIntervalSeconds = 1.0f / 40.0f;
constexpr int kNetworkSnapshotKeyframeIntervalTicks = 18;
constexpr int kNetworkSnapshotHistorySize = 128;
constexpr float kCameraZoom = 2.f;
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

constexpr float kPlayerHitboxWidth = 15.0f;
constexpr float kPlayerHitboxHeight = 15.0f;
constexpr float kPlayerHitboxOffsetY = 7.0f;
constexpr float kTerrainCollisionOffsetX = 0.0f;
constexpr float kTerrainCollisionOffsetY = -12.0f;
constexpr float kPlayerAcceleration = 300.0f;
constexpr float kPlayerFriction = 3.5f;
constexpr float kPlayerMaxSpeed = 100.0f;
constexpr float kCollisionResolveBlendFactor = 0.6f;
constexpr float kCollisionResolveMinStep = 2.0f;
constexpr float kCollisionResolveSnapDistance = 3.0f;

constexpr float kFireBoltCollisionRadius = 6.5f;
constexpr float kFireBoltScale = 1.4f;
constexpr float kStaticFireBoltScaleMultiplier = 1.2f;
constexpr float kStaticFireBoltUpgradePauseSeconds = 0.15f;
constexpr float kStaticFireBoltSpeedMultiplier = 1.8f;
constexpr float kProjectileSpeed = 420.0f;
constexpr int kProjectileDamage = 34;
constexpr float kFireBoltExplosionRadius = 20.0f;
constexpr float kFireBoltExplosionFallbackDuration = 0.5f;
constexpr int kIceWaveShardCount = 5;
constexpr float kIceWaveFanAngleDegrees = 35.0f;
constexpr float kIceWaveRangeTiles = 6.0f;
constexpr float kIceWaveShardSpeed = 420.0f;
constexpr float kIceWaveShardCollisionRadius = 4.0f;
constexpr float kIceWaveShardScale = 1.0f;
constexpr int kIceWaveShardDamage = 6;
constexpr float kFrozenStatusDurationSeconds = 4.0f;
constexpr float kFrozenMovementSpeedMultiplier = 0.75f;
constexpr float kSnowParticleBaseSize = 32.0f;
constexpr float kSnowParticleFallSpeed = 48.0f;
constexpr float kSnowParticleSwayAmplitude = 6.0f;
constexpr float kSnowParticleSwayFrequencyHz = 1.75f;
constexpr float kIceWaveShardSnowSpawnRatePerSecond = 12.0f;
constexpr float kIceWaveShardSnowSpawnZMin = 16.0f;
constexpr float kIceWaveShardSnowSpawnZMax = 19.2f;
constexpr float kIceWaveShardSnowScaleMin = 0.4f;
constexpr float kIceWaveShardSnowScaleMax = 0.6f;
constexpr float kFrozenSnowSpawnRatePerSecond = 7.0f;
constexpr float kFrozenSnowSpawnRadiusBase = 16.0f;
constexpr float kFrozenSnowSpawnRadiusStdDev = 5.0f;
constexpr float kFrozenSnowSpawnZMin = 16.0f;
constexpr float kFrozenSnowSpawnZMax = 32.0f;
constexpr float kIceWallMaterializeSeconds = 0.3f;
constexpr int kIceWallLengthCells = 5;
constexpr float kIceWallMaxHp = 100.0f;
constexpr float kIceWallHpDecayPerSecond = 10.0f;
constexpr int kProjectileSmokeEmitEveryFrames = 2;
constexpr int kProjectileSmokeParticlesPerBurst = 5;
constexpr float kProjectileSmokeBackVelocityFactor = -0.08f;
constexpr float kSmokeEmitterOffsetStdDev = 4.0f;
constexpr float kSmokeEmitterVelocityJitterStdDev = 10.0f;
constexpr int kDamageHitParticlesPerBurst = 6;
constexpr float kDamageHitParticleBaseSpeed = 240.0f;
constexpr float kDamageHitParticleSpeedJitter = 60.0f;
constexpr float kDamageHitParticleSpreadRadians = 0.55f;
constexpr float kDamageHitParticleLifetimeSeconds = 0.32f;
constexpr float kDamageHitParticleVelocityDecay = 7.5f;
constexpr float kDamageHitParticleSizeDecay = 4.0f;
constexpr float kDamageHitParticleBaseSize = 24.0f;
constexpr float kDamageHitParticleSizeJitter = 5.0f;
constexpr float kHammerImpactOffsetPixels = 64.0f;
constexpr float kHammerImpactRadiusPixels = 64.0f;
constexpr float kHammerAccelerationPenalty = 80.0f;
constexpr float kHammerAnticipationAccelerationMultiplier = 0.1f;
constexpr int kHammerSwingEventFrame = 6;
constexpr int kHammerImpactEventFrame = 9;

constexpr float kMeleeCooldownSeconds = 0.5f;
constexpr float kMeleeActiveWindowSeconds = 0.25f;
constexpr float kMeleeHitStartSeconds = 0.1f;
constexpr float kMeleeHitEndSeconds = 0.25f;
constexpr float kMeleeRange = 32.0f;
constexpr float kMeleeHitRadius = 14.0f;
constexpr int kMeleeDamage = 12;
constexpr float kMeleeAttackVisualScale = 1.8f;
constexpr float kGrapplingHookRangeTiles = 12.0f;
constexpr float kGrapplingHookCooldownSeconds = 10.0f;
constexpr float kGrapplingHookFireSpeed = 300.0f;
constexpr float kGrapplingHookPullSpeed = 200.0f;
constexpr float kGrapplingHookRetractSpeedMultiplier = 1.3f;
constexpr float kGrapplingHookCollisionProbeStep = 4.0f;
constexpr float kGrapplingHookArrivalEpsilon = 12.0f;
constexpr float kRuneMinCrossCooldownSeconds = 0.5f;
constexpr float kRuneManaCostFire = 6.0f;
constexpr float kRuneManaCostWater = 4.0f;
constexpr float kRuneManaCostCatalyst = 2.0f;
constexpr float kRuneManaCostEarth = 7.0f;
constexpr float kRuneManaCostFireStormDummy = 0.0f;
constexpr float kDefaultManaRegenPerSecond = 1.0f;
constexpr float kDefaultPlayerMaxMana = 50.0f;
constexpr float kRuneCooldownFireSeconds = 1.3f;
constexpr float kRuneCooldownWaterSeconds = 0.9f;
constexpr float kRuneCooldownCatalystSeconds = 3.0f;
constexpr float kRuneCooldownEarthSeconds = 6.0f;
constexpr float kRuneCooldownFireStormDummySeconds = 0.5f;
constexpr float kRuneActivationFireSeconds = 0.2f;
constexpr float kRuneActivationWaterSeconds = 0.1f;
constexpr float kRuneActivationCatalystSeconds = 0.2f;
constexpr float kRuneActivationEarthSeconds = 0.2f;
constexpr float kRuneActivationFireStormDummySeconds = 0.0f;
constexpr float kEarthRuneSlamSpawnRootsProgress = 0.7f;
constexpr float kEarthRuneSlamSlowdown = 1.5f;
constexpr float kEarthRuneTrapPersistSeconds = 4.0f;
constexpr float kEarthRootedBurnInSeconds = 1.0f;
constexpr float kEarthRootedRecoverSeconds = 0.2f;
constexpr float kEarthRootedVisibleDurationSeconds = 3.0f;
constexpr float kEarthRootedDamagePerSecond = 2.0f;
constexpr float kEarthRuneTrapRangeTiles = 1.8f;
constexpr float kEarthRuneSlamFallbackSeconds = 1.0f;
constexpr float kEarthRuneRootedDeathFallbackSeconds = 0.5f;
constexpr int kPotionSmallImmediateHeal = 14;
constexpr float kPotionSmallRegenPerSecond = 6.0f;
constexpr float kPotionSmallRegenDurationSeconds = 6.0f;
constexpr int kPotionSmallImmediateMana = 5;
constexpr float kPotionSmallManaRegenPerSecond = 2.5f;
constexpr float kPotionSmallManaRegenDurationSeconds = 6.0f;
constexpr float kStaticFireBoltStunSeconds = 2.5f;
constexpr float kFireStormDummyLightningChancePerFrame = 0.04f;
constexpr int kFireStormDummyLightningMinFrames = 3;
constexpr int kFireStormDummyLightningMaxFrames = 8;
constexpr int kFireStormDummyLightningCooldownFrames = 8;
constexpr float kFireStormLifetimeSeconds = 30.0f;
constexpr float kFireStormConversionRadiusTiles = 16.0f;
constexpr float kFireStormStormProjectileTravelSeconds = 1.0f;
constexpr float kFireStormStormArcPeakHeight = 128.0f;
constexpr float kFireStormSparkSpawnRatePerSecond = 42.0f;
constexpr float kFireStormSparkFallSpeed = 160.0f;
constexpr float kFireStormSparkSwayAmplitude = 1.75f;
constexpr float kFireStormSparkSwayFrequencyHz = 1.4f;
constexpr float kFireStormStormProjectileSize = 40.0f;
constexpr float kFireStormSparkSizeMin = 10.0f;
constexpr float kFireStormSparkSizeMax = 16.0f;
constexpr float kRuneVolatileManaMultiplier = 3.0f;
constexpr float kRuneVolatileActivationMultiplier = 2.0f;
constexpr float kHudVolatileLowManaBlinkHz = 4.0f / 3.0f;
constexpr float kRuneCastRangeTiles = 12.0f;
constexpr int kAltarScanHalfExtentTiles = 8;
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
constexpr float kInfluenceZoneTransitionSeconds = 1.0f;
constexpr int kDefaultMatchDurationSeconds = 300;
constexpr int kDefaultBestOfTargetKills = 5;
constexpr float kDefaultShrinkStartSeconds = 150.0f;
constexpr int kLobbyTimeStepSeconds = 30;

constexpr int kMatchDurationSeconds = 120;
constexpr int kMaxHp = 100;
constexpr float kRespawnDelaySeconds = 3.0f;
constexpr float kArenaUnsafeDamagePerSecond = 5.0f;
constexpr float kArenaUnsafeDamageTickSeconds = 3.0f;
constexpr float kArenaSpawnBufferTiles = 3.0f;
constexpr float kOutsideZoneTileBrightness = 0.5f;
constexpr unsigned char kGlobalShadowAlpha = 50;
constexpr float kPlayerHealthBarWidth = 32.0f;
constexpr float kPlayerHealthBarHeight = 4.0f;
constexpr float kPlayerHealthBarOffsetY = -20.0f;
constexpr int kMainMenuTitleFontSize = 120;
constexpr float kPlayerVisualScale = 1.0f;
constexpr float kDroppedItemVisualScale = 1.0f;
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
constexpr float kDamageFlashDurationSeconds = 0.24f;
constexpr float kCameraShakeDurationSeconds = 0.12f;
constexpr float kCameraShakePixels = 7.0f;
constexpr float kHammerImpactCameraShakeDurationSeconds = 0.12f;
constexpr float kTreeWindSwayStrengthPixels = 3.0f;
constexpr float kTreeWindSpeed = 0.45f;
constexpr float kTreeWindGradientStart = 0.3f;
constexpr int kOccluderRevealMaxCircles = 8;
constexpr float kOccluderRevealPlayerRadiusWorld = 15.0f;
constexpr float kOccluderRevealItemRadiusWorld = 9.0f;
constexpr float kOccluderRevealFalloffWorld = 6.0f;
constexpr float kOccluderRevealInsideAlpha = 0.22f;
constexpr float kDroppedEquipmentPickupRadius = 24.0f;
constexpr float kDroppedEquipmentPickupUnlockRadiusMultiplier = 1.5f;
constexpr int kWaterOverlayStartR = 45;
constexpr int kWaterOverlayStartG = 112;
constexpr int kWaterOverlayStartB = 130;
constexpr int kWaterOverlayStartA = 180;
constexpr int kWaterOverlayEndR = 45;
constexpr int kWaterOverlayEndG = 112;
constexpr int kWaterOverlayEndB = 130;
constexpr int kWaterOverlayEndA = 0;
// Vertical anchor inside the lightning sprite used for start/end alignment.
// 0.5 keeps the beam centerline on the world-space start/end points.
constexpr float kLightningAnchorYRatio = 0.5f;

constexpr int kRuneCellSize = 32;
constexpr float kRuneCastRangeWorld = kRuneCastRangeTiles * static_cast<float>(kRuneCellSize);
constexpr int kPlacementPreviewRadiusCells = 3;
constexpr float kRunePreviewAlpha = 0.6f;

constexpr int kSpatialCellSize = 64;
constexpr float kAtlasSampleInsetPixels = 0.35f;

constexpr int kTeamBlue = 0;
constexpr int kTeamRed = 1;
constexpr int kTeamCount = 2;

constexpr std::array<std::string_view, 1> kPlaceableTiles = {"tile_grass"};

constexpr const char* kDefaultMapPath = "assets/maps/default.png";
constexpr const char* kSpriteMetadataPath = "assets/sprite_sheet.json";
constexpr const char* kSpriteMetadataTallPath = "assets/sprite_sheet_32x64.json";
constexpr const char* kSpriteMetadata96x96Path = "assets/sprite_sheet_96x96.json";
constexpr const char* kSpriteMetadata128x128Path = "assets/sprite_sheet_128x128.json";
constexpr const char* kModularPlayerMainMetadataPath = "assets/128x128_modular/exports/wizzard_128x128-main.json";
constexpr const char* kPlayerColorsMapPath = "assets/player-colors-map.png";
constexpr const char* kModularPlayerShadowMetadataPath =
    "assets/128x128_modular/exports/wizzard_128x128-shadow.json";
constexpr const char* kModularPlayerSwordMetadataPath =
    "assets/128x128_modular/exports/wizzard_128x128-sword.json";
constexpr const char* kModularPlayerHammerMetadataPath =
    "assets/128x128_modular/exports/wizzard_128x128-hammer.json";
constexpr const char* kModularPlayerGhookMetadataPath =
    "assets/128x128_modular/exports/wizzard_128x128-ghook.json";
constexpr const char* kModularPlayerFxMetadataPath = "assets/128x128_modular/exports/wizzard_128x128-fx.json";
constexpr const char* kModularTreeCanopyBackgroundMetadataPath =
    "assets/128x128_tree_modular/exports/128x128_tree_modular-canopy_background.json";
constexpr const char* kModularTreeTrunkMetadataPath =
    "assets/128x128_tree_modular/exports/128x128_tree_modular-trunk.json";
constexpr const char* kModularTreeCanopyForegroundMetadataPath =
    "assets/128x128_tree_modular/exports/128x128_tree_modular-canopy_foreground.json";
constexpr const char* kModularTreeShadowMetadataPath =
    "assets/128x128_tree_modular/exports/128x128_tree_modular-shadow.json";
constexpr const char* kModularTreeOutlineMaskMetadataPath =
    "assets/128x128_tree_modular/exports/128x128_tree_modular-layer_2.json";
constexpr const char* kModularTreeAssetMetadataPath = "assets/128x128_tree_modular/metadata.json";
constexpr const char* kSpellPatternPath = "assets/spell_patterns.json";
constexpr const char* kEquipmentProfilesPath = "assets/equipment_profiles.json";
constexpr const char* kHitShapesPath = "assets/hit_shapes.json";
constexpr const char* kLootTablesPath = "assets/loot_tables.json";
constexpr const char* kObjectsConfigPath = "assets/objects.json";
constexpr const char* kCompositeEffectsPath = "assets/composite_effects.json";
constexpr const char* kMenuBackgroundPath = "assets/menu_background.png";
constexpr const char* kOccluderRevealShaderPath = "assets/shaders/occluder_reveal.fs";
constexpr const char* kWaterGradientShaderPath = "assets/shaders/water_gradient.fs";
constexpr const char* kZonePostProcessShaderPath = "assets/shaders/zone_desaturate.fs";
constexpr const char* kZoneFillOverlayShaderPath = "assets/shaders/zone_fill_overlay.fs";
constexpr const char* kZoneBorderOverlayShaderPath = "assets/shaders/zone_border_overlay.fs";
constexpr const char* kMapBoundsFadeShaderPath = "assets/shaders/map_bounds_fade.fs";
constexpr const char* kInfluenceZoneOverlayShaderPath = "assets/shaders/influence_zone_overlay.fs";
constexpr const char* kDamageFlashShaderPath = "assets/shaders/damage_flash.fs";
constexpr const char* kTreeCompositeShaderPath = "assets/shaders/tree_composite.fs";
constexpr const char* kTreeWindShaderPath = "assets/shaders/tree_wind.fs";
constexpr const char* kSfxFireballCreatedPath = "assets/sfx/ogg/SFX/Spells/Fireball 1.ogg";
constexpr const char* kSfxMeleeAttackPath = "assets/sfx/ogg/SFX/Attacks/Sword Attacks Hits and Blocks/Sword Attack 1.ogg";
constexpr const char* kSfxCreateRunePath = "assets/sfx/ogg/SFX/Torch/Light Torch 1.ogg";
constexpr const char* kSfxVolatileCastPath = "assets/sfx/energy-zap.wav";
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
constexpr std::array<const char*, 2> kSfxHammerSwingPaths = {
    "assets/sfx/ogg/SFX/Spells/Rock Meteor Throw 1.ogg",
    "assets/sfx/ogg/SFX/Spells/Rock Meteor Throw 2.ogg",
};
constexpr std::array<const char*, 3> kSfxHammerImpactPaths = {
    "assets/sfx/ogg/SFX/Spells/Spell Impact 1.ogg",
    "assets/sfx/ogg/SFX/Spells/Spell Impact 2.ogg",
    "assets/sfx/ogg/SFX/Spells/Spell Impact 3.ogg",
};
constexpr std::array<const char*, 3> kSfxZoneDamagePaths = {
    "assets/sfx/Arcane_AttackF1.wav",
    "assets/sfx/Arcane_AttackF2.wav",
    "assets/sfx/Arcane_AttackF3.wav",
};
constexpr std::array<const char*, 3> kSfxIceWaveCastPaths = {
    "assets/sfx/Ice_AttackF1.wav",
    "assets/sfx/Ice_AttackF2.wav",
    "assets/sfx/Ice_AttackF3.wav",
};
constexpr std::array<const char*, 3> kSfxIceWaveCastFallbackPaths = {
    "assets/sfx/Arcane_AttackF1.wav",
    "assets/sfx/Arcane_AttackF2.wav",
    "assets/sfx/Arcane_AttackF3.wav",
};
constexpr std::array<const char*, 3> kSfxIceWaveImpactPaths = {
    "assets/sfx/Ice_ImpactF1.wav",
    "assets/sfx/Ice_ImpactF2.wav",
    "assets/sfx/Ice_ImpactF3.wav",
};
constexpr std::array<const char*, 3> kSfxIceWaveImpactFallbackPaths = {
    "assets/sfx/ogg/SFX/Spells/Ice Freeze 1.ogg",
    "assets/sfx/ogg/SFX/Spells/Ice Freeze 1.ogg",
    "assets/sfx/ogg/SFX/Spells/Ice Freeze 1.ogg",
};
constexpr std::array<const char*, 5> kSfxFootstepDirtPaths = {
    "assets/sfx/ogg/SFX/Footsteps/Dirt/Dirt Walk 1.ogg",
    "assets/sfx/ogg/SFX/Footsteps/Dirt/Dirt Walk 2.ogg",
    "assets/sfx/ogg/SFX/Footsteps/Dirt/Dirt Walk 3.ogg",
    "assets/sfx/ogg/SFX/Footsteps/Dirt/Dirt Walk 4.ogg",
    "assets/sfx/ogg/SFX/Footsteps/Dirt/Dirt Walk 5.ogg",
};
constexpr const char* kFireStormAmbientPath = "assets/sfx/EM_FIRE_HOLD_4s.ogg";
constexpr const char* kBgmForestDayPath = "assets/sfx/ogg/BGS Loops/Forest Day/Forest Day.ogg";
constexpr const char* kBgmOutsideGamePath = "assets/sfx/ogg/BGS Loops/Cave/Cave Storm.ogg";

constexpr float kBgmVolume = 0.35f;
constexpr float kSfxVolumeFireballCreated = 0.70f;
constexpr float kSfxVolumeMeleeAttack = 0.60f;
constexpr float kSfxVolumeCreateRune = 0.70f;
constexpr float kSfxVolumeVolatileCast = 0.76f;
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
constexpr float kSfxVolumeZoneDamage = 0.60f;
constexpr float kSfxVolumeStaticBoltImpact = 0.78f;
constexpr float kSfxVolumeGrapplingThrow = 0.68f;
constexpr float kSfxVolumeGrapplingLatch = 0.72f;
constexpr float kSfxVolumeEarthRuneLaunch = 0.74f;
constexpr float kSfxVolumeEarthRuneImpact = 0.78f;
constexpr float kSfxVolumeIceWaveCast = 0.72f;
constexpr float kSfxVolumeIceWaveImpact = 0.72f;
constexpr float kSfxVolumeHammerSwing = 0.72f;
constexpr float kSfxVolumeHammerImpact = 0.80f;
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
