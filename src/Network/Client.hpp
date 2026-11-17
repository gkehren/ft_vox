#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <boost/asio.hpp>

#include "common.hpp"

struct VoxelEditEvent {
	uint32_t playerId;
	int32_t x, y, z;
	uint8_t type;
};

class Client
{
public:
	// Receive buffer sized for a full WORLD_STATE snapshot at the server's
	// player cap (review §4.11): 1024 bytes silently truncated snapshots
	// above ~60 players.
	static constexpr std::size_t kRecvBufferBytes = 4096;

	Client();
	~Client();

	/// Resolves `host` (names like "localhost" work, not only numeric
	/// addresses — review M18) and starts the io thread. On failure the
	/// client stays disconnected and `outError` carries the reason; no
	/// partially-connected state is left behind.
	bool connect(const std::string &host, unsigned short port, std::string &outError);
	void disconnect();
	bool isConnected() const;

	uint32_t getWorldSeed() const;
	uint32_t getPlayerId() const;
	void sendPlayerPosition(float x, float y, float z);
	void sendVoxelEdit(int32_t x, int32_t y, int32_t z, uint8_t type);
	std::vector<VoxelEditEvent> getPendingVoxelEdits();

private:
	void run();
	void stop();
	void sendMessage(const Message &message);
	void receive();
	void handleReceive(const boost::system::error_code &error, std::size_t bytesTransferred);
	void handleMessage(const std::vector<uint8_t> &data);

	void requestSeed();
	void sendAck(uint32_t playerId);

	std::atomic<bool> connected;

	boost::asio::io_context ioContext;
	boost::asio::ip::udp::socket socket;
	boost::asio::ip::udp::endpoint serverEndpoint;

	std::atomic<uint32_t> worldSeed;

	// Bounded handshake (review M20): runs on the io thread; the old
	// condvar wait was dead code because it would have blocked the very
	// thread that receives SEND_SEED.
	boost::asio::steady_timer handshakeTimer;

	std::atomic<uint32_t> sequenceNumber;

	std::array<uint8_t, kRecvBufferBytes> recvBuffer;

	// Written by the io thread (AUTHENTICATION handler), read by caller
	// threads (getPlayerId, sendVoxelEdit, disconnect): atomic like
	// worldSeed/sequenceNumber (review C2 — the plain uint32_t was a data
	// race / UB).
	std::atomic<uint32_t> playerId;

public: // public temporarily for ImGui and testing
	std::unordered_map<uint32_t, PlayerPosition> playerPositions;
	std::mutex playerMutex;

	std::atomic<uint32_t> worldStatePacketCount{0};
	std::atomic<uint32_t> lastWorldStateSequenceNumber{0};

private:
	std::vector<VoxelEditEvent> pendingVoxelEdits;
	std::mutex voxelEditMutex;

	// Declared last: joined before the socket/ioContext members above are
	// destroyed.
	std::jthread clientThread;
};
