#include "spells/ice_wall_spell.h"

#include <algorithm>

#include "core/constants.h"
#include "game/ice_wall.h"

namespace {

bool IsHorizontalDirection(SpellDirection direction) {
    return direction == SpellDirection::Horizontal || direction == SpellDirection::Left ||
           direction == SpellDirection::Right;
}

}  // namespace

std::string IceWallSpell::GetName() const { return "ice_wall"; }

void IceWallSpell::Cast(GameState& state, EventQueue& event_queue) {
    int caster_team = 0;
    for (const auto& player : state.players) {
        if (player.id == caster_player_id_) {
            caster_team = player.team;
            break;
        }
    }

    const bool horizontal = IsHorizontalDirection(direction_);
    const int half_len = Constants::kIceWallLengthCells / 2;

    for (int offset = -half_len; offset <= half_len; ++offset) {
        GridCoord cell = cast_cell_;
        if (horizontal) {
            cell.x += offset;
        } else {
            cell.y += offset;
        }

        if (!state.map.IsInside(cell)) {
            continue;
        }

        const bool already_has_wall = std::any_of(state.ice_walls.begin(), state.ice_walls.end(),
                                                  [&](const IceWallPiece& piece) {
                                                      return piece.alive && piece.cell == cell;
                                                  });
        if (already_has_wall) {
            continue;
        }

        IceWallPiece piece;
        piece.id = state.next_entity_id++;
        piece.cell = cell;
        piece.owner_player_id = caster_player_id_;
        piece.owner_team = caster_team;
        piece.state = IceWallState::Materializing;
        piece.state_time = 0.0f;
        piece.hp = Constants::kIceWallMaxHp;
        piece.alive = true;
        state.ice_walls.push_back(piece);
    }

    event_queue.Push(
        SpellCastEvent{caster_player_id_, GetName(), state.map.CellCenterWorld(cast_cell_), direction_});
}
