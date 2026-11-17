#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <boost/asio.hpp>

#include "common.hpp"

class Server
{
public:
	/// Player-table cap (review M19): forged endpoints can no longer grow
	/// the table without bound and exhaust the id space.
	static constexpr std::size_t kMaxPlayers = 64;
	/// Inactive registrations are purged after this long (review M19):
	/// ghost players no longer broadcast forever in WORLD_STATE.
	static constexpr auto kInactivityTimeout = std::chrono::seconds(30);
	/// Receive buffer sized for a full WORLD_STATE snapshot at the cap.
	static constexpr std::size_t kRecvBufferBytes = 4096;

	Server(unsigned short port, uint32_t worldSeed);
	~Server();

	void start();
	void stop();
	bool isRunning() const;

	/// The port the socket actually bound to. Pass 0 to the constructor to
	/// let the OS pick a free (ephemeral) port — the test suite does this so
	/// a WinNAT excluded port range can never fail the bind. Cached at bind
	/// time: stays valid after stop() (review §4.11: local_endpoint() used
	/// to throw on the closed socket).
	unsigned short port() const;

	/// Number of REGISTERED players (endpoint-validated), not only those
	/// who already sent a position.
	size_t getClientCount() const;

private:
	void run();
	void receive();
	void tick();
	void handleReceive(const boost::asio::ip::udp::endpoint &sender, const boost::system::error_code &error, std::size_t bytesTransferred);

	void handleMessage(const boost::asio::ip::udp::endpoint &sender, const std::vector<uint8_t> &data);
	void sendMessage(const boost::asio::ip::udp::endpoint &endpoint, const Message &message);
	void broadcastWorldState();
	void purgeInactivePlayers();

	std::thread serverThread;
	std::atomic<bool> running;

	boost::asio::io_context ioContext;
	boost::asio::ip::udp::socket socket;
	boost::asio::steady_timer tickTimer;

	uint32_t worldSeed;
	unsigned short boundPort{0};

	std::array<uint8_t, kRecvBufferBytes> recvBuffer;
	boost::asio::ip::udp::endpoint senderEndpoint;

	std::unordered_map<uint32_t, PlayerPosition> playerPositions;
	std::unordered_map<uint32_t, boost::asio::ip::udp::endpoint> playerEndpoints;
	// Endpoint dedup (review M19): a re-handshake from the same endpoint
	// reuses its player id instead of allocating a ghost. std::map because
	// udp::endpoint has operator< but no std::hash.
	std::map<boost::asio::ip::udp::endpoint, uint32_t> endpointPlayers;
	std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> lastActivity;
	mutable std::mutex playerMutex;

	std::unordered_map<uint32_t, uint32_t> playerSequenceNumbers;
	uint32_t nextPlayerId;
};
