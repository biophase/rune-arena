#pragma once

#include <string>

#include "game/player.h"
#include "game/projectile.h"
#include "game/rune.h"
#include "game/ice_wall.h"
#include "game/map_object.h"
#include "game/fire_storm_dummy.h"
#include "game/fire_storm_cast.h"
#include "game/earth_roots_group.h"
#include "game/castle.h"
#include "game/grappling_hook.h"
#include "net/network_messages.h"

namespace SnapshotTranslation {

PlayerSnapshot BuildPlayerSnapshot(const Player& player);
void ApplyPlayerSnapshot(Player* player, const PlayerSnapshot& snapshot, const std::string& resolved_name);

RuneSnapshot BuildRuneSnapshot(const Rune& rune);
CastleSnapshot BuildCastleSnapshot(const CastleState& castle);
ProjectileSnapshot BuildProjectileSnapshot(const Projectile& projectile);
IceWallSnapshot BuildIceWallSnapshot(const IceWallPiece& wall);
MapObjectSnapshot BuildMapObjectSnapshot(const MapObjectInstance& object);
FireStormDummySnapshot BuildFireStormDummySnapshot(const FireStormDummy& dummy);
FireStormCastSnapshot BuildFireStormCastSnapshot(const FireStormCast& cast);
EarthRootsGroupSnapshot BuildEarthRootsGroupSnapshot(const EarthRootsGroup& group);
GrapplingHookSnapshot BuildGrapplingHookSnapshot(const GrapplingHook& hook);

}  // namespace SnapshotTranslation
