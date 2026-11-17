#pragma once

#include <cstdint>
#include <vector>
#include "ByteBuffer.hpp"

// Wire protocol version (review §4.11): every packet starts with this byte
// and a mismatched peer is rejected instead of misparsed — the same
// versioned-contract discipline as the headless digest (FTVHDIG2). Bump on
// ANY layout change; the module is test-only, so client and server move
// together.
inline constexpr uint8_t kProtocolVersion = 1;

// Scoped message tags (review §5.1): the ordinals are the wire values and
// stay append-only.
enum class MessageType : uint8_t
{
	REQUEST_SEED = 1,
	SEND_SEED = 2,
	PLAYER_POSITION = 3,
	AUTHENTICATION = 4,
	ACK = 5,
	WORLD_STATE = 6,
	DISCONNECT = 7,
	VOXEL_EDIT = 8
};

struct Message
{
	MessageType type{MessageType::REQUEST_SEED};
	uint32_t sequenceNumber{0};
	std::vector<uint8_t> payload;
};

struct PlayerPosition
{
	uint32_t playerId{0};
	float x{0.f}, y{0.f}, z{0.f};
};

// Single serialization point (review §4.11): [version u8][type u8]
// [sequence u32][payload]. Client and server share it so the framing cannot
// drift between the two sides.
inline std::vector<uint8_t> serializeMessage(const Message &message)
{
	ByteBuffer buf;
	buf.reserve(sizeof(uint8_t) * 2 + sizeof(uint32_t) + message.payload.size());
	buf.writeUInt8(kProtocolVersion);
	buf.writeUInt8(static_cast<uint8_t>(message.type));
	buf.writeUInt32(message.sequenceNumber);
	buf.getBytes().insert(buf.getBytes().end(), message.payload.begin(), message.payload.end());
	return std::move(buf.getBytes());
}

// Parses the framed header; false on truncation or a foreign protocol
// version. On success `out.payload` holds the remaining bytes.
inline bool parseMessage(const std::vector<uint8_t> &data, Message &out)
{
	ByteBuffer buf(data);
	if (!buf.hasMore(sizeof(uint8_t) * 2 + sizeof(uint32_t)))
		return false;
	if (buf.readUInt8() != kProtocolVersion)
		return false;
	out.type = static_cast<MessageType>(buf.readUInt8());
	out.sequenceNumber = buf.readUInt32();
	out.payload.assign(data.begin() + static_cast<std::ptrdiff_t>(buf.getReadOffset()), data.end());
	return true;
}
