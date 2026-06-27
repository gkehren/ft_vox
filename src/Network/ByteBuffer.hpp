#pragma once

#include <cstdint>
#include <vector>
#include <cstring>
#include <stdexcept>
#include <boost/asio.hpp>

class ByteBuffer
{
public:
	ByteBuffer() = default;
	explicit ByteBuffer(const std::vector<uint8_t> &data) : buffer(data), readOffset(0) {}
	explicit ByteBuffer(std::vector<uint8_t> &&data) : buffer(std::move(data)), readOffset(0) {}

	const std::vector<uint8_t> &getBytes() const { return buffer; }
	std::vector<uint8_t> &getBytes() { return buffer; }
	void reserve(size_t size) { buffer.reserve(size); }

	// Write primitives
	void writeUInt8(uint8_t value)
	{
		buffer.push_back(value);
	}

	void writeUInt32(uint32_t value)
	{
		uint32_t netValue = htonl(value);
		const uint8_t *ptr = reinterpret_cast<const uint8_t *>(&netValue);
		buffer.insert(buffer.end(), ptr, ptr + sizeof(uint32_t));
	}

	void writeFloat(float value)
	{
		uint32_t temp;
		std::memcpy(&temp, &value, sizeof(float));
		writeUInt32(temp);
	}

	// Read primitives
	uint8_t readUInt8()
	{
		if (readOffset + sizeof(uint8_t) > buffer.size())
		{
			throw std::out_of_range("ByteBuffer read out of bounds");
		}
		return buffer[readOffset++];
	}

	uint32_t readUInt32()
	{
		if (readOffset + sizeof(uint32_t) > buffer.size())
		{
			throw std::out_of_range("ByteBuffer read out of bounds");
		}
		uint32_t netValue;
		std::memcpy(&netValue, &buffer[readOffset], sizeof(uint32_t));
		readOffset += sizeof(uint32_t);
		return ntohl(netValue);
	}

	float readFloat()
	{
		uint32_t temp = readUInt32();
		float value;
		std::memcpy(&value, &temp, sizeof(float));
		return value;
	}

	bool hasMore(size_t size) const
	{
		return readOffset + size <= buffer.size();
	}

	size_t getReadOffset() const { return readOffset; }
	size_t size() const { return buffer.size(); }

private:
	std::vector<uint8_t> buffer;
	size_t readOffset = 0;
};
