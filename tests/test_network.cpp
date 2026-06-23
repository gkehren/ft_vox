#include <Network/Server.hpp>
#include <Network/Client.hpp>
#include <iostream>
#include <thread>
#include <chrono>
#include <cassert>
#include <vector>

void sendSpoofedPosition(unsigned short port, uint32_t playerId, float x, float y, float z)
{
	boost::asio::io_context io;
	boost::asio::ip::udp::socket sock(io, boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), 0));
	boost::asio::ip::udp::endpoint serverEP(boost::asio::ip::make_address("127.0.0.1"), port);

	ByteBuffer payloadBuf;
	payloadBuf.writeUInt32(playerId);
	payloadBuf.writeFloat(x);
	payloadBuf.writeFloat(y);
	payloadBuf.writeFloat(z);

	ByteBuffer msgBuf;
	msgBuf.writeUInt8(MessageType::PLAYER_POSITION);
	msgBuf.writeUInt32(0); // Sequence number
	msgBuf.getBytes().insert(msgBuf.getBytes().end(), payloadBuf.getBytes().begin(), payloadBuf.getBytes().end());

	sock.send_to(boost::asio::buffer(msgBuf.getBytes()), serverEP);
}

void sendSpoofedAck(unsigned short port, uint32_t playerId)
{
	boost::asio::io_context io;
	boost::asio::ip::udp::socket sock(io, boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), 0));
	boost::asio::ip::udp::endpoint serverEP(boost::asio::ip::make_address("127.0.0.1"), port);

	ByteBuffer payloadBuf;
	payloadBuf.writeUInt32(playerId);

	ByteBuffer msgBuf;
	msgBuf.writeUInt8(MessageType::ACK);
	msgBuf.writeUInt32(0); // Sequence number
	msgBuf.getBytes().insert(msgBuf.getBytes().end(), payloadBuf.getBytes().begin(), payloadBuf.getBytes().end());

	sock.send_to(boost::asio::buffer(msgBuf.getBytes()), serverEP);
}

int main()
{
	std::cout << "[TEST] Starting Network Server/Client Handshake Test..." << std::endl;

	const unsigned short test_port = 54321;
	const uint32_t test_seed = 424242;

	// Start Server
	Server server(test_port, test_seed);
	server.start();
	assert(server.isRunning());
	std::cout << "[TEST] Server started on port " << test_port << std::endl;

	// Start Client 1
	Client client1;
	client1.connect("127.0.0.1", test_port);
	std::cout << "[TEST] Client 1 connecting..." << std::endl;

	// Start Client 2
	Client client2;
	client2.connect("127.0.0.1", test_port);
	std::cout << "[TEST] Client 2 connecting..." << std::endl;

	// Wait up to 3 seconds for handshakes
	int attempts = 0;
	while (attempts < 30)
	{
		if (client1.getPlayerId() != 0 && client2.getPlayerId() != 0 &&
			client1.getWorldSeed() == test_seed && client2.getWorldSeed() == test_seed)
		{
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		attempts++;
	}

	std::cout << "[TEST] Handshake finished. Client 1 ID: " << client1.getPlayerId()
			  << ", Client 2 ID: " << client2.getPlayerId() << std::endl;

	assert(client1.getPlayerId() != 0);
	assert(client2.getPlayerId() != 0);
	assert(server.getClientCount() == 2);
	std::cout << "[TEST] Mutual handshakes successful!" << std::endl;

	// Client 1 sends legitimate position update
	std::cout << "[TEST] Sending legitimate position update for Client 1..." << std::endl;
	client1.sendPlayerPosition(11.0f, 12.0f, 13.0f);

	// Wait for position to propagate to Client 2
	std::this_thread::sleep_for(std::chrono::milliseconds(200));

	{
		std::lock_guard<std::mutex> lock(client2.playerMutex);
		assert(client2.playerPositions.count(client1.getPlayerId()) > 0);
		assert(client2.playerPositions[client1.getPlayerId()].x == 11.0f);
	}
	std::cout << "[TEST] Legit update propagated successfully!" << std::endl;

	// Test 1: Spoofed update for non-existent player (ID 999)
	std::cout << "[TEST] Sending spoofed update for unregistered player ID 999..." << std::endl;
	sendSpoofedPosition(test_port, 999, 100.0f, 200.0f, 300.0f);
	std::this_thread::sleep_for(std::chrono::milliseconds(200));

	// Server client count should remain 2 (not add 999)
	assert(server.getClientCount() == 2);
	std::cout << "[TEST] Spoofed update for unregistered player ignored successfully!" << std::endl;

	// Test 2: Spoofed update for Client 1 (ID 1) from spoofing socket
	std::cout << "[TEST] Sending spoofed update for Client 1 from unauthorized socket..." << std::endl;
	sendSpoofedPosition(test_port, client1.getPlayerId(), 999.0f, 999.0f, 999.0f);
	std::this_thread::sleep_for(std::chrono::milliseconds(200));

	// Client 2's knowledge of Client 1's position must NOT be 999.0f (should still be 11.0f)
	{
		std::lock_guard<std::mutex> lock(client2.playerMutex);
		assert(client2.playerPositions[client1.getPlayerId()].x == 11.0f);
	}
	std::cout << "[TEST] Spoofed update from unauthorized socket rejected successfully!" << std::endl;

	// Test 3: Send spoofed ACK for Client 1 from unauthorized socket
	std::cout << "[TEST] Sending spoofed ACK for Client 1..." << std::endl;
	sendSpoofedAck(test_port, client1.getPlayerId());
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	std::cout << "[TEST] Spoofed ACK sent (should be logged as suspicious by server)." << std::endl;

	// Test 4: Verify Server Tick Architecture and Sequence Numbers
	std::cout << "[TEST] Verifying WORLD_STATE tick architecture and sequence numbers..." << std::endl;
	uint32_t initialPacketCount = client1.worldStatePacketCount.load();
	uint32_t initialSeqNum = client1.lastWorldStateSequenceNumber.load();

	// Wait for approximately 5 ticks (5 * 50ms = 250ms)
	std::this_thread::sleep_for(std::chrono::milliseconds(250));

	uint32_t finalPacketCount = client1.worldStatePacketCount.load();
	uint32_t finalSeqNum = client1.lastWorldStateSequenceNumber.load();

	assert(finalPacketCount > initialPacketCount);
	assert(finalSeqNum > initialSeqNum);
	std::cout << "[TEST] Tick architecture verified! Sequence number properly increments from "
			  << initialSeqNum << " to " << finalSeqNum << "." << std::endl;

	// Cleanup
	client1.disconnect();
	client2.disconnect();
	server.stop();

	std::cout << "[TEST] All network and spoofing tests passed successfully!" << std::endl;
	return 0;
}
