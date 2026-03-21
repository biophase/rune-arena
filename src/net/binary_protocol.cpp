#include "net/binary_protocol.h"

#include <algorithm>
#include <cstring>

namespace binary {
namespace {

constexpr uint32_t kMagic = 0x524E4152;  // RNAR
constexpr uint16_t kVersion = 2;
constexpr size_t kHeaderSize = 12;

class BufferWriter {
  public:
    void WriteU8(uint8_t value) { bytes_.push_back(value); }
    void WriteBool(bool value) { WriteU8(value ? 1u : 0u); }
    void WriteU16(uint16_t value) {
        bytes_.push_back(static_cast<uint8_t>(value & 0xFFu));
        bytes_.push_back(static_cast<uint8_t>((value >> 8u) & 0xFFu));
    }
    void WriteU32(uint32_t value) {
        bytes_.push_back(static_cast<uint8_t>(value & 0xFFu));
        bytes_.push_back(static_cast<uint8_t>((value >> 8u) & 0xFFu));
        bytes_.push_back(static_cast<uint8_t>((value >> 16u) & 0xFFu));
        bytes_.push_back(static_cast<uint8_t>((value >> 24u) & 0xFFu));
    }
    void WriteI32(int32_t value) { WriteU32(static_cast<uint32_t>(value)); }
    void WriteF32(float value) {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(uint32_t));
        WriteU32(bits);
    }
    void WriteString(const std::string& value) {
        const auto size = static_cast<uint16_t>(std::min<size_t>(value.size(), 65535));
        WriteU16(size);
        bytes_.insert(bytes_.end(), value.begin(), value.begin() + size);
    }

    const std::vector<uint8_t>& Bytes() const { return bytes_; }

  private:
    std::vector<uint8_t> bytes_;
};

class BufferReader {
  public:
    BufferReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    bool ReadU8(uint8_t& out) {
        if (offset_ + 1 > size_) return false;
        out = data_[offset_++];
        return true;
    }
    bool ReadBool(bool& out) {
        uint8_t v = 0;
        if (!ReadU8(v)) return false;
        out = (v != 0u);
        return true;
    }
    bool ReadU16(uint16_t& out) {
        if (offset_ + 2 > size_) return false;
        out = static_cast<uint16_t>(data_[offset_]) | static_cast<uint16_t>(data_[offset_ + 1] << 8u);
        offset_ += 2;
        return true;
    }
    bool ReadU32(uint32_t& out) {
        if (offset_ + 4 > size_) return false;
        out = static_cast<uint32_t>(data_[offset_]) | static_cast<uint32_t>(data_[offset_ + 1] << 8u) |
              static_cast<uint32_t>(data_[offset_ + 2] << 16u) | static_cast<uint32_t>(data_[offset_ + 3] << 24u);
        offset_ += 4;
        return true;
    }
    bool ReadI32(int32_t& out) {
        uint32_t value = 0;
        if (!ReadU32(value)) return false;
        out = static_cast<int32_t>(value);
        return true;
    }
    bool ReadF32(float& out) {
        uint32_t bits = 0;
        if (!ReadU32(bits)) return false;
        std::memcpy(&out, &bits, sizeof(float));
        return true;
    }
    bool ReadString(std::string& out) {
        uint16_t size = 0;
        if (!ReadU16(size)) return false;
        if (offset_ + size > this->size_) return false;
        out.assign(reinterpret_cast<const char*>(data_ + offset_), size);
        offset_ += size;
        return true;
    }

    bool End() const { return offset_ == size_; }

  private:
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    size_t offset_ = 0;
};

std::vector<uint8_t> MakePacket(PacketType type, const std::vector<uint8_t>& payload) {
    BufferWriter writer;
    writer.WriteU32(kMagic);
    writer.WriteU16(kVersion);
    writer.WriteU8(static_cast<uint8_t>(type));
    writer.WriteU8(0);  // flags
    writer.WriteU32(static_cast<uint32_t>(payload.size()));
    std::vector<uint8_t> packet = writer.Bytes();
    packet.insert(packet.end(), payload.begin(), payload.end());
    return packet;
}

}  // namespace

std::vector<uint8_t> EncodeJoinPacket(const std::string& name) {
    BufferWriter payload;
    payload.WriteString(name);
    return MakePacket(PacketType::Join, payload.Bytes());
}

std::optional<std::string> DecodeJoinPayload(const uint8_t* payload, size_t payload_size) {
    BufferReader reader(payload, payload_size);
    std::string out;
    if (!reader.ReadString(out) || !reader.End()) return std::nullopt;
    return out;
}

std::vector<uint8_t> EncodeJoinAckPacket(int player_id) {
    BufferWriter payload;
    payload.WriteI32(player_id);
    return MakePacket(PacketType::JoinAck, payload.Bytes());
}

std::optional<int> DecodeJoinAckPayload(const uint8_t* payload, size_t payload_size) {
    BufferReader reader(payload, payload_size);
    int32_t player_id = -1;
    if (!reader.ReadI32(player_id) || !reader.End()) return std::nullopt;
    return static_cast<int>(player_id);
}

std::vector<uint8_t> EncodeClientMovePacket(const ClientMoveMessage& message) {
    BufferWriter payload;
    payload.WriteI32(message.player_id);
    payload.WriteI32(message.seq);
    payload.WriteI32(message.tick);
    payload.WriteI32(message.last_received_snapshot_id);
    payload.WriteF32(message.move_x);
    payload.WriteF32(message.move_y);
    payload.WriteF32(message.aim_x);
    payload.WriteF32(message.aim_y);
    return MakePacket(PacketType::ClientMove, payload.Bytes());
}

std::optional<ClientMoveMessage> DecodeClientMovePayload(const uint8_t* payload, size_t payload_size) {
    BufferReader reader(payload, payload_size);
    ClientMoveMessage out;
    if (!reader.ReadI32(out.player_id) || !reader.ReadI32(out.seq) || !reader.ReadI32(out.tick) ||
        !reader.ReadI32(out.last_received_snapshot_id) || !reader.ReadF32(out.move_x) ||
        !reader.ReadF32(out.move_y) || !reader.ReadF32(out.aim_x) || !reader.ReadF32(out.aim_y) || !reader.End()) {
        return std::nullopt;
    }
    return out;
}

std::vector<uint8_t> EncodeClientActionPacket(const ClientActionMessage& message) {
    BufferWriter payload;
    payload.WriteI32(message.player_id);
    payload.WriteI32(message.seq);
    payload.WriteI32(message.last_received_snapshot_id);
    payload.WriteBool(message.primary_pressed);
    payload.WriteBool(message.grappling_pressed);
    payload.WriteI32(message.request_rune_type);
    payload.WriteString(message.request_item_id);
    payload.WriteBool(message.toggle_inventory_mode);
    return MakePacket(PacketType::ClientAction, payload.Bytes());
}

std::optional<ClientActionMessage> DecodeClientActionPayload(const uint8_t* payload, size_t payload_size) {
    BufferReader reader(payload, payload_size);
    ClientActionMessage out;
    if (!reader.ReadI32(out.player_id) || !reader.ReadI32(out.seq) || !reader.ReadI32(out.last_received_snapshot_id) ||
        !reader.ReadBool(out.primary_pressed) || !reader.ReadBool(out.grappling_pressed) ||
        !reader.ReadI32(out.request_rune_type) ||
        !reader.ReadString(out.request_item_id) || !reader.ReadBool(out.toggle_inventory_mode) || !reader.End()) {
        return std::nullopt;
    }
    return out;
}

std::vector<uint8_t> EncodeSnapshotPacket(const ServerSnapshotMessage& message) {
    BufferWriter payload;
    payload.WriteI32(message.server_tick);
    payload.WriteI32(message.snapshot_id);
    payload.WriteI32(message.base_snapshot_id);
    payload.WriteBool(message.is_delta);
    payload.WriteF32(message.time_remaining);
    payload.WriteF32(message.shrink_tiles_per_second);
    payload.WriteF32(message.min_arena_radius_tiles);
    payload.WriteF32(message.arena_radius_tiles);
    payload.WriteF32(message.arena_radius_world);
    payload.WriteF32(message.arena_center_world_x);
    payload.WriteF32(message.arena_center_world_y);
    payload.WriteBool(message.match_running);
    payload.WriteBool(message.match_finished);
    payload.WriteI32(message.red_team_kills);
    payload.WriteI32(message.blue_team_kills);

    payload.WriteU16(static_cast<uint16_t>(std::min<size_t>(message.players.size(), 65535)));
    for (size_t i = 0; i < message.players.size() && i < 65535; ++i) {
        const auto& player = message.players[i];
        payload.WriteI32(player.id);
        payload.WriteI32(player.team);
        payload.WriteF32(player.pos_x);
        payload.WriteF32(player.pos_y);
        payload.WriteF32(player.vel_x);
        payload.WriteF32(player.vel_y);
        payload.WriteF32(player.aim_dir_x);
        payload.WriteF32(player.aim_dir_y);
        payload.WriteI32(player.hp);
        payload.WriteI32(player.kills);
        payload.WriteBool(player.alive);
        payload.WriteI32(player.facing);
        payload.WriteI32(player.action_state);
        payload.WriteF32(player.melee_active_remaining);
        payload.WriteBool(player.rune_placing_mode);
        payload.WriteI32(player.selected_rune_type);
        payload.WriteF32(player.rune_place_cooldown_remaining);
        payload.WriteF32(player.mana);
        payload.WriteF32(player.max_mana);
        payload.WriteF32(player.grappling_cooldown_remaining);
        payload.WriteF32(player.grappling_cooldown_total);
        payload.WriteU16(static_cast<uint16_t>(std::min<size_t>(player.rune_cooldown_remaining.size(), 65535)));
        for (size_t j = 0; j < player.rune_cooldown_remaining.size() && j < 65535; ++j) {
            payload.WriteF32(player.rune_cooldown_remaining[j]);
        }
        payload.WriteU16(static_cast<uint16_t>(std::min<size_t>(player.rune_cooldown_total.size(), 65535)));
        for (size_t j = 0; j < player.rune_cooldown_total.size() && j < 65535; ++j) {
            payload.WriteF32(player.rune_cooldown_total[j]);
        }
        payload.WriteU16(static_cast<uint16_t>(std::min<size_t>(player.status_effects.size(), 65535)));
        for (size_t j = 0; j < player.status_effects.size() && j < 65535; ++j) {
            payload.WriteI32(player.status_effects[j].type);
            payload.WriteF32(player.status_effects[j].remaining_seconds);
            payload.WriteF32(player.status_effects[j].total_seconds);
            payload.WriteF32(player.status_effects[j].magnitude_per_second);
            payload.WriteString(player.status_effects[j].composite_effect_id);
        }
        payload.WriteU16(static_cast<uint16_t>(std::min<size_t>(player.item_slots.size(), 65535)));
        for (size_t j = 0; j < player.item_slots.size() && j < 65535; ++j) {
            payload.WriteString(player.item_slots[j]);
        }
        payload.WriteU16(static_cast<uint16_t>(std::min<size_t>(player.item_slot_counts.size(), 65535)));
        for (size_t j = 0; j < player.item_slot_counts.size() && j < 65535; ++j) {
            payload.WriteI32(player.item_slot_counts[j]);
        }
        payload.WriteU16(static_cast<uint16_t>(std::min<size_t>(player.item_slot_cooldown_remaining.size(), 65535)));
        for (size_t j = 0; j < player.item_slot_cooldown_remaining.size() && j < 65535; ++j) {
            payload.WriteF32(player.item_slot_cooldown_remaining[j]);
        }
        payload.WriteU16(static_cast<uint16_t>(std::min<size_t>(player.item_slot_cooldown_total.size(), 65535)));
        for (size_t j = 0; j < player.item_slot_cooldown_total.size() && j < 65535; ++j) {
            payload.WriteF32(player.item_slot_cooldown_total[j]);
        }
        payload.WriteBool(player.awaiting_respawn);
        payload.WriteF32(player.respawn_remaining);
        payload.WriteI32(player.last_processed_move_seq);
    }

    payload.WriteU16(static_cast<uint16_t>(std::min<size_t>(message.runes.size(), 65535)));
    for (size_t i = 0; i < message.runes.size() && i < 65535; ++i) {
        const auto& rune = message.runes[i];
        payload.WriteI32(rune.id);
        payload.WriteI32(rune.owner_player_id);
        payload.WriteI32(rune.owner_team);
        payload.WriteI32(rune.x);
        payload.WriteI32(rune.y);
        payload.WriteI32(rune.rune_type);
        payload.WriteI32(rune.placement_order);
        payload.WriteBool(rune.active);
        payload.WriteBool(rune.volatile_cast);
        payload.WriteF32(rune.activation_total_seconds);
        payload.WriteF32(rune.activation_remaining_seconds);
        payload.WriteBool(rune.creates_influence_zone);
    }

    payload.WriteU16(static_cast<uint16_t>(std::min<size_t>(message.projectiles.size(), 65535)));
    for (size_t i = 0; i < message.projectiles.size() && i < 65535; ++i) {
        const auto& projectile = message.projectiles[i];
        payload.WriteI32(projectile.id);
        payload.WriteI32(projectile.owner_player_id);
        payload.WriteI32(projectile.owner_team);
        payload.WriteF32(projectile.pos_x);
        payload.WriteF32(projectile.pos_y);
        payload.WriteF32(projectile.vel_x);
        payload.WriteF32(projectile.vel_y);
        payload.WriteF32(projectile.radius);
        payload.WriteI32(projectile.damage);
        payload.WriteString(projectile.animation_key);
        payload.WriteBool(projectile.emitter_enabled);
        payload.WriteI32(projectile.emitter_emit_every_frames);
        payload.WriteI32(projectile.emitter_frame_counter);
        payload.WriteBool(projectile.alive);
    }

    payload.WriteU16(static_cast<uint16_t>(std::min<size_t>(message.ice_walls.size(), 65535)));
    for (size_t i = 0; i < message.ice_walls.size() && i < 65535; ++i) {
        const auto& wall = message.ice_walls[i];
        payload.WriteI32(wall.id);
        payload.WriteI32(wall.owner_player_id);
        payload.WriteI32(wall.owner_team);
        payload.WriteI32(wall.cell_x);
        payload.WriteI32(wall.cell_y);
        payload.WriteI32(wall.state);
        payload.WriteF32(wall.state_time);
        payload.WriteF32(wall.hp);
        payload.WriteBool(wall.alive);
    }

    payload.WriteU16(static_cast<uint16_t>(std::min<size_t>(message.map_objects.size(), 65535)));
    for (size_t i = 0; i < message.map_objects.size() && i < 65535; ++i) {
        const auto& object = message.map_objects[i];
        payload.WriteI32(object.id);
        payload.WriteString(object.prototype_id);
        payload.WriteI32(object.cell_x);
        payload.WriteI32(object.cell_y);
        payload.WriteI32(object.object_type);
        payload.WriteI32(object.hp);
        payload.WriteI32(object.state);
        payload.WriteF32(object.state_time);
        payload.WriteF32(object.death_duration);
        payload.WriteBool(object.collision_enabled);
        payload.WriteBool(object.alive);
    }

    payload.WriteU16(static_cast<uint16_t>(std::min<size_t>(message.fire_storm_dummies.size(), 65535)));
    for (size_t i = 0; i < message.fire_storm_dummies.size() && i < 65535; ++i) {
        const auto& dummy = message.fire_storm_dummies[i];
        payload.WriteI32(dummy.id);
        payload.WriteI32(dummy.owner_player_id);
        payload.WriteI32(dummy.owner_team);
        payload.WriteI32(dummy.cell_x);
        payload.WriteI32(dummy.cell_y);
        payload.WriteI32(dummy.state);
        payload.WriteF32(dummy.state_time);
        payload.WriteF32(dummy.state_duration);
        payload.WriteF32(dummy.idle_lifetime_remaining_seconds);
        payload.WriteBool(dummy.alive);
    }

    payload.WriteU16(static_cast<uint16_t>(std::min<size_t>(message.fire_storm_casts.size(), 65535)));
    for (size_t i = 0; i < message.fire_storm_casts.size() && i < 65535; ++i) {
        const auto& cast = message.fire_storm_casts[i];
        payload.WriteI32(cast.id);
        payload.WriteI32(cast.owner_player_id);
        payload.WriteI32(cast.owner_team);
        payload.WriteI32(cast.center_cell_x);
        payload.WriteI32(cast.center_cell_y);
        payload.WriteF32(cast.elapsed_seconds);
        payload.WriteF32(cast.duration_seconds);
        payload.WriteBool(cast.alive);
    }

    payload.WriteU16(static_cast<uint16_t>(std::min<size_t>(message.grappling_hooks.size(), 65535)));
    for (size_t i = 0; i < message.grappling_hooks.size() && i < 65535; ++i) {
        const auto& hook = message.grappling_hooks[i];
        payload.WriteI32(hook.id);
        payload.WriteI32(hook.owner_player_id);
        payload.WriteI32(hook.owner_team);
        payload.WriteF32(hook.head_pos_x);
        payload.WriteF32(hook.head_pos_y);
        payload.WriteF32(hook.target_pos_x);
        payload.WriteF32(hook.target_pos_y);
        payload.WriteF32(hook.latch_point_x);
        payload.WriteF32(hook.latch_point_y);
        payload.WriteF32(hook.pull_destination_x);
        payload.WriteF32(hook.pull_destination_y);
        payload.WriteI32(hook.phase);
        payload.WriteI32(hook.latch_target_type);
        payload.WriteI32(hook.latch_target_id);
        payload.WriteI32(hook.latch_cell_x);
        payload.WriteI32(hook.latch_cell_y);
        payload.WriteBool(hook.latched);
        payload.WriteF32(hook.animation_time);
        payload.WriteF32(hook.pull_elapsed_seconds);
        payload.WriteF32(hook.max_pull_duration_seconds);
        payload.WriteBool(hook.alive);
    }

    payload.WriteU16(static_cast<uint16_t>(std::min<size_t>(message.removed_player_ids.size(), 65535)));
    for (size_t i = 0; i < message.removed_player_ids.size() && i < 65535; ++i) {
        payload.WriteI32(message.removed_player_ids[i]);
    }
    payload.WriteU16(static_cast<uint16_t>(std::min<size_t>(message.removed_rune_ids.size(), 65535)));
    for (size_t i = 0; i < message.removed_rune_ids.size() && i < 65535; ++i) {
        payload.WriteI32(message.removed_rune_ids[i]);
    }
    payload.WriteU16(static_cast<uint16_t>(std::min<size_t>(message.removed_projectile_ids.size(), 65535)));
    for (size_t i = 0; i < message.removed_projectile_ids.size() && i < 65535; ++i) {
        payload.WriteI32(message.removed_projectile_ids[i]);
    }
    payload.WriteU16(static_cast<uint16_t>(std::min<size_t>(message.removed_ice_wall_ids.size(), 65535)));
    for (size_t i = 0; i < message.removed_ice_wall_ids.size() && i < 65535; ++i) {
        payload.WriteI32(message.removed_ice_wall_ids[i]);
    }
    payload.WriteU16(static_cast<uint16_t>(std::min<size_t>(message.removed_map_object_ids.size(), 65535)));
    for (size_t i = 0; i < message.removed_map_object_ids.size() && i < 65535; ++i) {
        payload.WriteI32(message.removed_map_object_ids[i]);
    }
    payload.WriteU16(static_cast<uint16_t>(std::min<size_t>(message.removed_fire_storm_dummy_ids.size(), 65535)));
    for (size_t i = 0; i < message.removed_fire_storm_dummy_ids.size() && i < 65535; ++i) {
        payload.WriteI32(message.removed_fire_storm_dummy_ids[i]);
    }
    payload.WriteU16(static_cast<uint16_t>(std::min<size_t>(message.removed_fire_storm_cast_ids.size(), 65535)));
    for (size_t i = 0; i < message.removed_fire_storm_cast_ids.size() && i < 65535; ++i) {
        payload.WriteI32(message.removed_fire_storm_cast_ids[i]);
    }
    payload.WriteU16(static_cast<uint16_t>(std::min<size_t>(message.removed_grappling_hook_ids.size(), 65535)));
    for (size_t i = 0; i < message.removed_grappling_hook_ids.size() && i < 65535; ++i) {
        payload.WriteI32(message.removed_grappling_hook_ids[i]);
    }

    return MakePacket(PacketType::Snapshot, payload.Bytes());
}

std::optional<ServerSnapshotMessage> DecodeSnapshotPayload(const uint8_t* payload_data, size_t payload_size) {
    BufferReader reader(payload_data, payload_size);
    ServerSnapshotMessage out;
    if (!reader.ReadI32(out.server_tick) || !reader.ReadI32(out.snapshot_id) ||
        !reader.ReadI32(out.base_snapshot_id) || !reader.ReadBool(out.is_delta) ||
        !reader.ReadF32(out.time_remaining) || !reader.ReadF32(out.shrink_tiles_per_second) ||
        !reader.ReadF32(out.min_arena_radius_tiles) || !reader.ReadF32(out.arena_radius_tiles) ||
        !reader.ReadF32(out.arena_radius_world) || !reader.ReadF32(out.arena_center_world_x) ||
        !reader.ReadF32(out.arena_center_world_y) || !reader.ReadBool(out.match_running) ||
        !reader.ReadBool(out.match_finished) || !reader.ReadI32(out.red_team_kills) ||
        !reader.ReadI32(out.blue_team_kills)) {
        return std::nullopt;
    }

    uint16_t player_count = 0;
    if (!reader.ReadU16(player_count)) return std::nullopt;
    out.players.reserve(player_count);
    for (uint16_t i = 0; i < player_count; ++i) {
        PlayerSnapshot player;
        if (!reader.ReadI32(player.id) || !reader.ReadI32(player.team) || !reader.ReadF32(player.pos_x) ||
            !reader.ReadF32(player.pos_y) || !reader.ReadF32(player.vel_x) || !reader.ReadF32(player.vel_y) ||
            !reader.ReadF32(player.aim_dir_x) || !reader.ReadF32(player.aim_dir_y) || !reader.ReadI32(player.hp) ||
            !reader.ReadI32(player.kills) || !reader.ReadBool(player.alive) || !reader.ReadI32(player.facing) ||
            !reader.ReadI32(player.action_state) || !reader.ReadF32(player.melee_active_remaining) ||
            !reader.ReadBool(player.rune_placing_mode) || !reader.ReadI32(player.selected_rune_type) ||
            !reader.ReadF32(player.rune_place_cooldown_remaining) ||
            !reader.ReadF32(player.mana) || !reader.ReadF32(player.max_mana) ||
            !reader.ReadF32(player.grappling_cooldown_remaining) || !reader.ReadF32(player.grappling_cooldown_total)) {
            return std::nullopt;
        }

        uint16_t count = 0;
        if (!reader.ReadU16(count)) return std::nullopt;
        player.rune_cooldown_remaining.reserve(count);
        for (uint16_t j = 0; j < count; ++j) {
            float value = 0.0f;
            if (!reader.ReadF32(value)) return std::nullopt;
            player.rune_cooldown_remaining.push_back(value);
        }
        if (!reader.ReadU16(count)) return std::nullopt;
        player.rune_cooldown_total.reserve(count);
        for (uint16_t j = 0; j < count; ++j) {
            float value = 0.0f;
            if (!reader.ReadF32(value)) return std::nullopt;
            player.rune_cooldown_total.push_back(value);
        }
        if (!reader.ReadU16(count)) return std::nullopt;
        player.status_effects.reserve(count);
        for (uint16_t j = 0; j < count; ++j) {
            PlayerSnapshot::StatusEffectSnapshot status;
            if (!reader.ReadI32(status.type) || !reader.ReadF32(status.remaining_seconds) ||
                !reader.ReadF32(status.total_seconds) || !reader.ReadF32(status.magnitude_per_second) ||
                !reader.ReadString(status.composite_effect_id)) {
                return std::nullopt;
            }
            player.status_effects.push_back(status);
        }
        if (!reader.ReadU16(count)) return std::nullopt;
        player.item_slots.reserve(count);
        for (uint16_t j = 0; j < count; ++j) {
            std::string value;
            if (!reader.ReadString(value)) return std::nullopt;
            player.item_slots.push_back(value);
        }
        if (!reader.ReadU16(count)) return std::nullopt;
        player.item_slot_counts.reserve(count);
        for (uint16_t j = 0; j < count; ++j) {
            int32_t value = 0;
            if (!reader.ReadI32(value)) return std::nullopt;
            player.item_slot_counts.push_back(value);
        }
        if (!reader.ReadU16(count)) return std::nullopt;
        player.item_slot_cooldown_remaining.reserve(count);
        for (uint16_t j = 0; j < count; ++j) {
            float value = 0.0f;
            if (!reader.ReadF32(value)) return std::nullopt;
            player.item_slot_cooldown_remaining.push_back(value);
        }
        if (!reader.ReadU16(count)) return std::nullopt;
        player.item_slot_cooldown_total.reserve(count);
        for (uint16_t j = 0; j < count; ++j) {
            float value = 0.0f;
            if (!reader.ReadF32(value)) return std::nullopt;
            player.item_slot_cooldown_total.push_back(value);
        }
        if (!reader.ReadBool(player.awaiting_respawn) || !reader.ReadF32(player.respawn_remaining) ||
            !reader.ReadI32(player.last_processed_move_seq)) {
            return std::nullopt;
        }
        out.players.push_back(player);
    }

    uint16_t rune_count = 0;
    if (!reader.ReadU16(rune_count)) return std::nullopt;
    out.runes.reserve(rune_count);
    for (uint16_t i = 0; i < rune_count; ++i) {
        RuneSnapshot rune;
        if (!reader.ReadI32(rune.id) || !reader.ReadI32(rune.owner_player_id) || !reader.ReadI32(rune.owner_team) ||
            !reader.ReadI32(rune.x) || !reader.ReadI32(rune.y) || !reader.ReadI32(rune.rune_type) ||
            !reader.ReadI32(rune.placement_order) || !reader.ReadBool(rune.active) ||
            !reader.ReadBool(rune.volatile_cast) || !reader.ReadF32(rune.activation_total_seconds) ||
            !reader.ReadF32(rune.activation_remaining_seconds) || !reader.ReadBool(rune.creates_influence_zone)) {
            return std::nullopt;
        }
        out.runes.push_back(rune);
    }

    uint16_t projectile_count = 0;
    if (!reader.ReadU16(projectile_count)) return std::nullopt;
    out.projectiles.reserve(projectile_count);
    for (uint16_t i = 0; i < projectile_count; ++i) {
        ProjectileSnapshot projectile;
        if (!reader.ReadI32(projectile.id) || !reader.ReadI32(projectile.owner_player_id) ||
            !reader.ReadI32(projectile.owner_team) || !reader.ReadF32(projectile.pos_x) ||
            !reader.ReadF32(projectile.pos_y) || !reader.ReadF32(projectile.vel_x) ||
            !reader.ReadF32(projectile.vel_y) || !reader.ReadF32(projectile.radius) ||
            !reader.ReadI32(projectile.damage) || !reader.ReadString(projectile.animation_key) ||
            !reader.ReadBool(projectile.emitter_enabled) || !reader.ReadI32(projectile.emitter_emit_every_frames) ||
            !reader.ReadI32(projectile.emitter_frame_counter) || !reader.ReadBool(projectile.alive)) {
            return std::nullopt;
        }
        out.projectiles.push_back(projectile);
    }

    uint16_t wall_count = 0;
    if (!reader.ReadU16(wall_count)) return std::nullopt;
    out.ice_walls.reserve(wall_count);
    for (uint16_t i = 0; i < wall_count; ++i) {
        IceWallSnapshot wall;
        if (!reader.ReadI32(wall.id) || !reader.ReadI32(wall.owner_player_id) || !reader.ReadI32(wall.owner_team) ||
            !reader.ReadI32(wall.cell_x) || !reader.ReadI32(wall.cell_y) || !reader.ReadI32(wall.state) ||
            !reader.ReadF32(wall.state_time) || !reader.ReadF32(wall.hp) || !reader.ReadBool(wall.alive)) {
            return std::nullopt;
        }
        out.ice_walls.push_back(wall);
    }

    uint16_t object_count = 0;
    if (!reader.ReadU16(object_count)) return std::nullopt;
    out.map_objects.reserve(object_count);
    for (uint16_t i = 0; i < object_count; ++i) {
        MapObjectSnapshot object;
        if (!reader.ReadI32(object.id) || !reader.ReadString(object.prototype_id) || !reader.ReadI32(object.cell_x) ||
            !reader.ReadI32(object.cell_y) || !reader.ReadI32(object.object_type) || !reader.ReadI32(object.hp) ||
            !reader.ReadI32(object.state) || !reader.ReadF32(object.state_time) ||
            !reader.ReadF32(object.death_duration) || !reader.ReadBool(object.collision_enabled) ||
            !reader.ReadBool(object.alive)) {
            return std::nullopt;
        }
        out.map_objects.push_back(object);
    }

    uint16_t dummy_count = 0;
    if (!reader.ReadU16(dummy_count)) return std::nullopt;
    out.fire_storm_dummies.reserve(dummy_count);
    for (uint16_t i = 0; i < dummy_count; ++i) {
        FireStormDummySnapshot dummy;
        if (!reader.ReadI32(dummy.id) || !reader.ReadI32(dummy.owner_player_id) || !reader.ReadI32(dummy.owner_team) ||
            !reader.ReadI32(dummy.cell_x) || !reader.ReadI32(dummy.cell_y) || !reader.ReadI32(dummy.state) ||
            !reader.ReadF32(dummy.state_time) || !reader.ReadF32(dummy.state_duration) ||
            !reader.ReadF32(dummy.idle_lifetime_remaining_seconds) ||
            !reader.ReadBool(dummy.alive)) {
            return std::nullopt;
        }
        out.fire_storm_dummies.push_back(dummy);
    }

    uint16_t cast_count = 0;
    if (!reader.ReadU16(cast_count)) return std::nullopt;
    out.fire_storm_casts.reserve(cast_count);
    for (uint16_t i = 0; i < cast_count; ++i) {
        FireStormCastSnapshot cast;
        if (!reader.ReadI32(cast.id) || !reader.ReadI32(cast.owner_player_id) || !reader.ReadI32(cast.owner_team) ||
            !reader.ReadI32(cast.center_cell_x) || !reader.ReadI32(cast.center_cell_y) ||
            !reader.ReadF32(cast.elapsed_seconds) || !reader.ReadF32(cast.duration_seconds) ||
            !reader.ReadBool(cast.alive)) {
            return std::nullopt;
        }
        out.fire_storm_casts.push_back(cast);
    }

    uint16_t hook_count = 0;
    if (!reader.ReadU16(hook_count)) return std::nullopt;
    out.grappling_hooks.reserve(hook_count);
    for (uint16_t i = 0; i < hook_count; ++i) {
        GrapplingHookSnapshot hook;
        if (!reader.ReadI32(hook.id) || !reader.ReadI32(hook.owner_player_id) || !reader.ReadI32(hook.owner_team) ||
            !reader.ReadF32(hook.head_pos_x) || !reader.ReadF32(hook.head_pos_y) ||
            !reader.ReadF32(hook.target_pos_x) || !reader.ReadF32(hook.target_pos_y) ||
            !reader.ReadF32(hook.latch_point_x) || !reader.ReadF32(hook.latch_point_y) ||
            !reader.ReadF32(hook.pull_destination_x) || !reader.ReadF32(hook.pull_destination_y) ||
            !reader.ReadI32(hook.phase) || !reader.ReadI32(hook.latch_target_type) ||
            !reader.ReadI32(hook.latch_target_id) || !reader.ReadI32(hook.latch_cell_x) ||
            !reader.ReadI32(hook.latch_cell_y) || !reader.ReadBool(hook.latched) ||
            !reader.ReadF32(hook.animation_time) || !reader.ReadF32(hook.pull_elapsed_seconds) ||
            !reader.ReadF32(hook.max_pull_duration_seconds) || !reader.ReadBool(hook.alive)) {
            return std::nullopt;
        }
        out.grappling_hooks.push_back(hook);
    }

    uint16_t removed_player_count = 0;
    if (!reader.ReadU16(removed_player_count)) return std::nullopt;
    out.removed_player_ids.reserve(removed_player_count);
    for (uint16_t i = 0; i < removed_player_count; ++i) {
        int32_t id = 0;
        if (!reader.ReadI32(id)) return std::nullopt;
        out.removed_player_ids.push_back(id);
    }

    uint16_t removed_rune_count = 0;
    if (!reader.ReadU16(removed_rune_count)) return std::nullopt;
    out.removed_rune_ids.reserve(removed_rune_count);
    for (uint16_t i = 0; i < removed_rune_count; ++i) {
        int32_t id = 0;
        if (!reader.ReadI32(id)) return std::nullopt;
        out.removed_rune_ids.push_back(id);
    }

    uint16_t removed_projectile_count = 0;
    if (!reader.ReadU16(removed_projectile_count)) return std::nullopt;
    out.removed_projectile_ids.reserve(removed_projectile_count);
    for (uint16_t i = 0; i < removed_projectile_count; ++i) {
        int32_t id = 0;
        if (!reader.ReadI32(id)) return std::nullopt;
        out.removed_projectile_ids.push_back(id);
    }

    uint16_t removed_wall_count = 0;
    if (!reader.ReadU16(removed_wall_count)) return std::nullopt;
    out.removed_ice_wall_ids.reserve(removed_wall_count);
    for (uint16_t i = 0; i < removed_wall_count; ++i) {
        int32_t id = 0;
        if (!reader.ReadI32(id)) return std::nullopt;
        out.removed_ice_wall_ids.push_back(id);
    }

    uint16_t removed_object_count = 0;
    if (!reader.ReadU16(removed_object_count)) return std::nullopt;
    out.removed_map_object_ids.reserve(removed_object_count);
    for (uint16_t i = 0; i < removed_object_count; ++i) {
        int32_t id = 0;
        if (!reader.ReadI32(id)) return std::nullopt;
        out.removed_map_object_ids.push_back(id);
    }

    uint16_t removed_dummy_count = 0;
    if (!reader.ReadU16(removed_dummy_count)) return std::nullopt;
    out.removed_fire_storm_dummy_ids.reserve(removed_dummy_count);
    for (uint16_t i = 0; i < removed_dummy_count; ++i) {
        int32_t id = 0;
        if (!reader.ReadI32(id)) return std::nullopt;
        out.removed_fire_storm_dummy_ids.push_back(id);
    }

    uint16_t removed_cast_count = 0;
    if (!reader.ReadU16(removed_cast_count)) return std::nullopt;
    out.removed_fire_storm_cast_ids.reserve(removed_cast_count);
    for (uint16_t i = 0; i < removed_cast_count; ++i) {
        int32_t id = 0;
        if (!reader.ReadI32(id)) return std::nullopt;
        out.removed_fire_storm_cast_ids.push_back(id);
    }

    uint16_t removed_hook_count = 0;
    if (!reader.ReadU16(removed_hook_count)) return std::nullopt;
    out.removed_grappling_hook_ids.reserve(removed_hook_count);
    for (uint16_t i = 0; i < removed_hook_count; ++i) {
        int32_t id = 0;
        if (!reader.ReadI32(id)) return std::nullopt;
        out.removed_grappling_hook_ids.push_back(id);
    }

    if (!reader.End()) return std::nullopt;
    return out;
}

std::vector<uint8_t> EncodeLobbyStatePacket(const LobbyStateMessage& message) {
    BufferWriter payload;
    payload.WriteBool(message.host_can_start);
    payload.WriteI32(message.mode_type);
    payload.WriteI32(message.round_time_seconds);
    payload.WriteI32(message.best_of_target_kills);
    payload.WriteF32(message.shrink_tiles_per_second);
    payload.WriteF32(message.shrink_start_seconds);
    payload.WriteF32(message.min_arena_radius_tiles);
    payload.WriteU16(static_cast<uint16_t>(std::min<size_t>(message.players.size(), 65535)));
    for (size_t i = 0; i < message.players.size() && i < 65535; ++i) {
        payload.WriteI32(message.players[i].player_id);
        payload.WriteString(message.players[i].name);
    }
    return MakePacket(PacketType::LobbyState, payload.Bytes());
}

std::optional<LobbyStateMessage> DecodeLobbyStatePayload(const uint8_t* payload_data, size_t payload_size) {
    BufferReader reader(payload_data, payload_size);
    LobbyStateMessage out;
    if (!reader.ReadBool(out.host_can_start) || !reader.ReadI32(out.mode_type) ||
        !reader.ReadI32(out.round_time_seconds) || !reader.ReadI32(out.best_of_target_kills) ||
        !reader.ReadF32(out.shrink_tiles_per_second) || !reader.ReadF32(out.shrink_start_seconds) ||
        !reader.ReadF32(out.min_arena_radius_tiles)) {
        return std::nullopt;
    }
    uint16_t player_count = 0;
    if (!reader.ReadU16(player_count)) return std::nullopt;
    out.players.reserve(player_count);
    for (uint16_t i = 0; i < player_count; ++i) {
        LobbyPlayerInfo info;
        if (!reader.ReadI32(info.player_id) || !reader.ReadString(info.name)) {
            return std::nullopt;
        }
        out.players.push_back(info);
    }
    if (!reader.End()) return std::nullopt;
    return out;
}

std::vector<uint8_t> EncodeMatchStartPacket(const MatchStartMessage& message) {
    BufferWriter payload;
    payload.WriteBool(message.start);
    return MakePacket(PacketType::MatchStart, payload.Bytes());
}

std::optional<MatchStartMessage> DecodeMatchStartPayload(const uint8_t* payload_data, size_t payload_size) {
    BufferReader reader(payload_data, payload_size);
    MatchStartMessage out;
    if (!reader.ReadBool(out.start) || !reader.End()) return std::nullopt;
    return out;
}

bool DecodePacketHeader(const uint8_t* data, size_t data_size, DecodedPacketHeader& out_header) {
    if (data == nullptr || data_size < kHeaderSize) {
        return false;
    }

    BufferReader reader(data, data_size);
    uint32_t magic = 0;
    uint16_t version = 0;
    uint8_t type = 0;
    uint8_t flags = 0;
    uint32_t payload_size = 0;
    if (!reader.ReadU32(magic) || !reader.ReadU16(version) || !reader.ReadU8(type) || !reader.ReadU8(flags) ||
        !reader.ReadU32(payload_size)) {
        return false;
    }
    (void)flags;

    if (magic != kMagic) {
        return false;
    }
    if (kHeaderSize + payload_size > data_size) {
        return false;
    }

    out_header.type = static_cast<PacketType>(type);
    out_header.payload = data + kHeaderSize;
    out_header.payload_size = payload_size;
    out_header.version_ok = (version == kVersion);
    return true;
}

}  // namespace binary
