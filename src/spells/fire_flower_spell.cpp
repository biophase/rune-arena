#include "spells/fire_flower_spell.h"

#include "game/map_object.h"

std::string FireFlowerSpell::GetName() const { return "fire_flower"; }

void FireFlowerSpell::Cast(GameState& state, EventQueue& event_queue) {
    int caster_team = 0;
    for (const auto& player : state.players) {
        if (player.id == caster_player_id_) {
            caster_team = player.team;
            break;
        }
    }

    MapObjectInstance flower;
    flower.id = state.next_entity_id++;
    flower.owner_player_id = caster_player_id_;
    flower.owner_team = caster_team;
    flower.prototype_id = "fire_flower";
    flower.cell = cast_cell_;
    flower.type = ObjectType::Unit;
    flower.walkable = false;
    flower.stops_projectiles = true;
    flower.collision_enabled = false;
    flower.hp = 100;
    flower.state = MapObjectState::Spawning;
    flower.state_time = 0.0f;
    flower.death_duration = 0.0f;
    flower.alive = true;
    state.map_objects.push_back(flower);

    event_queue.Push(
        SpellCastEvent{caster_player_id_, GetName(), state.map.CellCenterWorld(cast_cell_), direction_});
}
