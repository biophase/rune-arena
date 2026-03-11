#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "net/network_messages.h"

namespace binary {

enum class PacketType : uint8_t {
    Join = 1,
    JoinAck = 2,
    ClientMove = 3,
    ClientAction = 4,
    Snapshot = 5,
    LobbyState = 6,
    MatchStart = 7,
};

struct DecodedPacketHeader {
    PacketType type = PacketType::Join;
    const uint8_t* payload = nullptr;
    size_t payload_size = 0;
    bool version_ok = false;
};

std::vector<uint8_t> EncodeJoinPacket(const std::string& name);
std::optional<std::string> DecodeJoinPayload(const uint8_t* payload, size_t payload_size);

std::vector<uint8_t> EncodeJoinAckPacket(int player_id);
std::optional<int> DecodeJoinAckPayload(const uint8_t* payload, size_t payload_size);

std::vector<uint8_t> EncodeClientMovePacket(const ClientMoveMessage& message);
std::optional<ClientMoveMessage> DecodeClientMovePayload(const uint8_t* payload, size_t payload_size);

std::vector<uint8_t> EncodeClientActionPacket(const ClientActionMessage& message);
std::optional<ClientActionMessage> DecodeClientActionPayload(const uint8_t* payload, size_t payload_size);

std::vector<uint8_t> EncodeSnapshotPacket(const ServerSnapshotMessage& message);
std::optional<ServerSnapshotMessage> DecodeSnapshotPayload(const uint8_t* payload, size_t payload_size);

std::vector<uint8_t> EncodeLobbyStatePacket(const LobbyStateMessage& message);
std::optional<LobbyStateMessage> DecodeLobbyStatePayload(const uint8_t* payload, size_t payload_size);

std::vector<uint8_t> EncodeMatchStartPacket(const MatchStartMessage& message);
std::optional<MatchStartMessage> DecodeMatchStartPayload(const uint8_t* payload, size_t payload_size);

bool DecodePacketHeader(const uint8_t* data, size_t data_size, DecodedPacketHeader& out_header);

}  // namespace binary
