#pragma once

#include <cstdint>
#include <vector>

enum MessageType : uint8_t
{
	REQUEST_SEED = 1,
	SEND_SEED = 2,
	ACK = 3
};

struct Message
{
	uint8_t type;
	uint32_t sequenceNumber;
	std::vector<uint8_t> payload;
};