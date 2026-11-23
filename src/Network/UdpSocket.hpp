#pragma once

#include <Network/WireProtocol.hpp>

#include <string_view>

namespace net::udp
{

struct Address
{
	uint32_t ipv4{};
	uint16_t port{};
	bool operator==(const Address &) const = default;
};

Address parseAddress(std::string_view ipv4, uint16_t port);
uint64_t randomId();
wire::PlayerKey randomPlayerKey();

struct Received
{
	Address from;
	std::array<uint8_t, wire::kDatagramBytes + 1> bytes;
	// Oversize (including OS truncation) is represented by bytes.size().
	size_t size{};
};

// Single IO owner. Socket calls never block or retain application buffers.
class Socket
{
public:
	explicit Socket(Address bindAddress);
	~Socket();
	Socket(const Socket &) = delete;
	Socket &operator=(const Socket &) = delete;
	void swap(Socket &other) noexcept;

	Address address() const { return m_address; }
	bool receive(Received &out);
	bool send(Address to, std::span<const uint8_t> bytes);

private:
	uintptr_t m_handle{};
	Address m_address;
};

} // namespace net::udp
