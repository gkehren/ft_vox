#include "UdpSocket.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <bcrypt.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace net::udp
{
namespace
{
#ifdef _WIN32
using Handle = SOCKET;
using AddressSize = int;
constexpr Handle kInvalidHandle = INVALID_SOCKET;
int socketError() { return WSAGetLastError(); }
void closeSocket(Handle handle) { closesocket(handle); }
bool transient(int error)
{
	return error == WSAEWOULDBLOCK || error == WSAEINTR || error == WSAECONNRESET ||
		error == WSAENETRESET || error == WSAENOBUFS || error == WSAEHOSTUNREACH ||
		error == WSAENETUNREACH || error == WSAECONNREFUSED || error == WSAEADDRNOTAVAIL;
}
void initializeSockets()
{
	struct Runtime
	{
		Runtime()
		{
			WSADATA data{};
			const int error = WSAStartup(MAKEWORD(2, 2), &data);
			if (error != 0)
				throw std::system_error(error, std::system_category(), "WSAStartup");
		}
		~Runtime() { WSACleanup(); }
	};
	static const Runtime runtime;
}
#else
using Handle = int;
using AddressSize = socklen_t;
constexpr Handle kInvalidHandle = -1;
int socketError() { return errno; }
void closeSocket(Handle handle) { close(handle); }
bool transient(int error)
{
	return error == EAGAIN || error == EWOULDBLOCK || error == EINTR || error == ENOBUFS ||
		error == EHOSTUNREACH || error == ENETUNREACH || error == ECONNREFUSED || error == EADDRNOTAVAIL;
}
void initializeSockets() {}
#endif

[[noreturn]] void fail(int error, const char *operation)
{
	throw std::system_error(error, std::system_category(), operation);
}

sockaddr_in native(Address address)
{
	sockaddr_in result{};
	result.sin_family = AF_INET;
	result.sin_port = htons(address.port);
	result.sin_addr.s_addr = htonl(address.ipv4);
	return result;
}

Address portable(const sockaddr_in &address)
{
	return {ntohl(address.sin_addr.s_addr), ntohs(address.sin_port)};
}
} // namespace

Address parseAddress(std::string_view ipv4, uint16_t port)
{
	initializeSockets();
	in_addr address{};
	if (ipv4.find('\0') != std::string_view::npos ||
	    inet_pton(AF_INET, std::string(ipv4).c_str(), &address) != 1)
		throw std::invalid_argument("expected a numeric IPv4 address");
	return {ntohl(address.s_addr), port};
}

void Socket::swap(Socket &other) noexcept
{
	std::swap(m_handle, other.m_handle);
	std::swap(m_address, other.m_address);
}

namespace
{
void randomBytes(std::span<uint8_t> bytes)
{
#ifdef _WIN32
	if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
	                    BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
		throw std::runtime_error("BCryptGenRandom failed");
#else
	auto *source = std::fopen("/dev/urandom", "rb");
	if (!source)
		fail(errno, "open /dev/urandom");
	const auto count = std::fread(bytes.data(), 1, bytes.size(), source);
	std::fclose(source);
	if (count != bytes.size())
		throw std::runtime_error("short read from /dev/urandom");
#endif
}
}

uint64_t randomId()
{
	uint64_t value{};
	do
	{
		randomBytes({reinterpret_cast<uint8_t *>(&value), sizeof(value)});
	}
	while (value == 0);
	return value;
}

wire::PlayerKey randomPlayerKey()
{
	wire::PlayerKey key;
	do
	{
		randomBytes(key);
	}
	while (key == wire::PlayerKey{});
	return key;
}

Socket::Socket(Address bindAddress)
{
	initializeSockets();
	const auto handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (handle == kInvalidHandle)
		fail(socketError(), "create UDP socket");
	try
	{
#ifdef _WIN32
		u_long nonblocking = 1;
		if (ioctlsocket(handle, FIONBIO, &nonblocking) != 0)
			fail(socketError(), "set UDP nonblocking");
		const int exclusive = 1;
		if (setsockopt(handle, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
		               reinterpret_cast<const char *>(&exclusive), sizeof(exclusive)) != 0)
			fail(socketError(), "set UDP exclusive bind");
#else
		if (fcntl(handle, F_SETFL, O_NONBLOCK) == -1 || fcntl(handle, F_SETFD, FD_CLOEXEC) == -1)
			fail(socketError(), "set UDP socket flags");
#endif
		auto address = native(bindAddress);
		if (bind(handle, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0)
			fail(socketError(), "bind UDP socket");
		AddressSize size = sizeof(address);
		if (getsockname(handle, reinterpret_cast<sockaddr *>(&address), &size) != 0)
			fail(socketError(), "query bound UDP address");
		m_address = portable(address);
		m_handle = static_cast<uintptr_t>(handle);
	}
	catch (...)
	{
		closeSocket(handle);
		throw;
	}
}

Socket::~Socket()
{
	closeSocket(static_cast<Handle>(m_handle));
}

bool Socket::receive(Received &out)
{
	sockaddr_in from{};
	AddressSize length = sizeof(from);
	const auto size = recvfrom(static_cast<Handle>(m_handle), reinterpret_cast<char *>(out.bytes.data()),
	                           static_cast<int>(out.bytes.size()), 0, reinterpret_cast<sockaddr *>(&from), &length);
	if (size < 0)
	{
		const int error = socketError();
#ifdef _WIN32
		if (error == WSAEMSGSIZE)
		{
			out.from = portable(from);
			out.size = out.bytes.size();
			return true;
		}
#endif
		if (transient(error))
			return false;
		fail(error, "receive UDP datagram");
	}
	out.from = portable(from);
	out.size = static_cast<size_t>(size);
	return true;
}

bool Socket::send(Address to, std::span<const uint8_t> bytes)
{
	const auto address = native(to);
	const auto size = sendto(static_cast<Handle>(m_handle), reinterpret_cast<const char *>(bytes.data()),
	                         static_cast<int>(bytes.size()), 0,
	                         reinterpret_cast<const sockaddr *>(&address), sizeof(address));
	if (size < 0)
	{
		const int error = socketError();
		if (transient(error)
#ifdef _WIN32
		    || error == WSAEACCES || error == WSAEHOSTDOWN || error == WSAEMSGSIZE
#else
		    || error == EACCES || error == EPERM || error == EHOSTDOWN || error == EMSGSIZE
#endif
		)
			return false;
		fail(error, "send UDP datagram");
	}
	return static_cast<size_t>(size) == bytes.size();
}

} // namespace net::udp
