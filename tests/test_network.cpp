// Network prototype integration test (review M22 rewrite).
//
// The old version was the repo's only assert()-based test: under NDEBUG
// (Release) every assertion vanished and the suite became a chain of
// sleep_for calls that always passed. It now uses the shared counted CHECK
// harness (TestMain.hpp — a vacuous pass is a failure) and waits on
// DEADLINES instead of fixed sleeps, so it is neither flaky nor silent.
//
// Covers: handshake (seed + player id), endpoint-validated position
// propagation, spoofing rejection (unregistered id, foreign endpoint,
// foreign ACK), WORLD_STATE tick + sequence numbers, endpoint dedup on
// re-handshake (review M19), and DISCONNECT cleanup.

#include <Network/Server.hpp>
#include <Network/Client.hpp>
#include "TestMain.hpp"

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace
{
// Deadline wait (review M22): poll a predicate until it holds or the budget
// is exhausted — no fixed sleeps that are either too short (flaky) or too
// long (slow).
template <class Pred>
bool waitFor(Pred pred, std::chrono::milliseconds budget)
{
	const auto deadline = std::chrono::steady_clock::now() + budget;
	while (std::chrono::steady_clock::now() < deadline)
	{
		if (pred())
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return pred();
}

// Raw packet sender for the spoofing scenarios: shares the wire framing
// (version byte included) with the real client via serializeMessage.
void sendRaw(unsigned short port, const Message &message)
{
	boost::asio::io_context io;
	boost::asio::ip::udp::socket sock(io, boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), 0));
	const boost::asio::ip::udp::endpoint serverEP(boost::asio::ip::make_address("127.0.0.1"), port);
	const auto bytes = serializeMessage(message);
	sock.send_to(boost::asio::buffer(bytes), serverEP);
}

void sendSpoofedPosition(unsigned short port, uint32_t playerId, float x, float y, float z)
{
	ByteBuffer payload;
	payload.writeUInt32(playerId);
	payload.writeFloat(x);
	payload.writeFloat(y);
	payload.writeFloat(z);
	Message m;
	m.type = MessageType::PLAYER_POSITION;
	m.payload = std::move(payload.getBytes());
	sendRaw(port, m);
}

void sendSpoofedAck(unsigned short port, uint32_t playerId)
{
	ByteBuffer payload;
	payload.writeUInt32(playerId);
	Message m;
	m.type = MessageType::ACK;
	m.payload = std::move(payload.getBytes());
	sendRaw(port, m);
}

// One raw REQUEST_SEED from a fixed local socket, collecting the
// AUTHENTICATION id the server answers (used by the dedup scenario).
// Scans past SEND_SEED and any WORLD_STATE broadcasts interleaved on the
// same endpoint until the AUTHENTICATION datagram arrives.
uint32_t requestSeedFrom(boost::asio::ip::udp::socket &sock,
						 const boost::asio::ip::udp::endpoint &serverEP)
{
	Message req;
	req.type = MessageType::REQUEST_SEED;
	const auto bytes = serializeMessage(req);
	sock.send_to(boost::asio::buffer(bytes), serverEP);

	std::array<uint8_t, 4096> buf{};
	boost::asio::ip::udp::endpoint from;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (std::chrono::steady_clock::now() < deadline)
	{
		boost::system::error_code recvEc;
		const std::size_t n = sock.receive_from(boost::asio::buffer(buf), from, 0, recvEc);
		if (recvEc == boost::asio::error::would_block)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			continue;
		}
		if (recvEc || n == 0)
			return 0;
		const std::vector<uint8_t> data(buf.begin(), buf.begin() + n);
		Message msg;
		if (!parseMessage(data, msg))
			continue;
		if (msg.type == MessageType::AUTHENTICATION)
		{
			ByteBuffer p(msg.payload);
			return p.hasMore(4) ? p.readUInt32() : 0;
		}
		// SEND_SEED / WORLD_STATE: keep scanning.
	}
	return 0;
}
} // namespace

int main()
{
	std::cout << "[TEST] Network Server/Client integration (deadline-based)\n";
	const uint32_t test_seed = 424242;

	try
	{
		// Ephemeral port: hard-coded ports collide with WinNAT excluded
		// ranges on Windows runners.
		Server server(0, test_seed);
		server.start();
		CHECK(server.isRunning(), "server running after start");
		const unsigned short test_port = server.port();
		CHECK(test_port != 0, "server bound a port");
		std::cout << "[TEST] Server on port " << test_port << "\n";

		Client client1;
		Client client2;
		std::string err;
		const bool c1 = client1.connect("127.0.0.1", test_port, err);
		CHECK(c1, "client1 connect failed");
		const bool c2 = client2.connect("127.0.0.1", test_port, err);
		CHECK(c2, "client2 connect failed");

		// Handshake: both clients learn the seed AND a non-zero player id.
		const bool handshook = waitFor([&] {
			return client1.getPlayerId() != 0 && client2.getPlayerId() != 0 &&
				   client1.getWorldSeed() == test_seed && client2.getWorldSeed() == test_seed;
		}, std::chrono::seconds(5));
		CHECK(handshook, "both handshakes complete within deadline");
		CHECK(client1.getPlayerId() != client2.getPlayerId(), "distinct player ids");
		CHECK(server.getClientCount() == 2, "server registered exactly 2 players");

		// Legitimate position update propagates through WORLD_STATE.
		client1.sendPlayerPosition(11.0f, 12.0f, 13.0f);
		const bool propagated = waitFor([&] {
			std::lock_guard<std::mutex> lock(client2.playerMutex);
			const auto it = client2.playerPositions.find(client1.getPlayerId());
			return it != client2.playerPositions.end() && it->second.x == 11.0f;
		}, std::chrono::seconds(5));
		CHECK(propagated, "legit position propagated to client2");

		// Spoof 1: unregistered player id — the table must not grow.
		sendSpoofedPosition(test_port, 999, 100.0f, 200.0f, 300.0f);
		// Spoof 2: client1's id from a FOREIGN endpoint — must be rejected,
		// client2 keeps seeing the legitimate position.
		sendSpoofedPosition(test_port, client1.getPlayerId(), 999.0f, 999.0f, 999.0f);
		sendSpoofedAck(test_port, client1.getPlayerId());
		// Give the server a few ticks to process (and wrongly accept) the
		// spoofed packets before asserting the invariants.
		const uint32_t seqBefore = client1.lastWorldStateSequenceNumber.load();
		const bool ticksPassed = waitFor([&] {
			return client1.lastWorldStateSequenceNumber.load() > seqBefore + 2;
		}, std::chrono::seconds(5));
		CHECK(ticksPassed, "WORLD_STATE ticks advance sequence numbers");
		CHECK(server.getClientCount() == 2, "spoofed id did not register");
		{
			std::lock_guard<std::mutex> lock(client2.playerMutex);
			const auto it = client2.playerPositions.find(client1.getPlayerId());
			CHECK(it != client2.playerPositions.end() && it->second.x == 11.0f,
				  "spoofed endpoint did not overwrite client1 position");
		}

		// Endpoint dedup (review M19): a re-handshake from the SAME endpoint
		// reuses its player id instead of allocating a ghost.
		{
			boost::asio::io_context io;
			boost::asio::ip::udp::socket sock(io,
				boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), 0));
			sock.non_blocking(true);
			const boost::asio::ip::udp::endpoint serverEP(
				boost::asio::ip::make_address("127.0.0.1"), test_port);
			const uint32_t firstId = requestSeedFrom(sock, serverEP);
			const uint32_t secondId = requestSeedFrom(sock, serverEP);
			CHECK(firstId != 0, "raw handshake answered");
			CHECK(firstId == secondId, "re-handshake reuses the endpoint's player id");
			CHECK(server.getClientCount() == 3, "dedup added exactly one registration");
			// Clean the raw registration up again via DISCONNECT.
			ByteBuffer payload;
			payload.writeUInt32(firstId);
			Message disc;
			disc.type = MessageType::DISCONNECT;
			disc.payload = std::move(payload.getBytes());
			const auto bytes = serializeMessage(disc);
			sock.send_to(boost::asio::buffer(bytes), serverEP);
			const bool cleaned = waitFor([&] { return server.getClientCount() == 2; },
										 std::chrono::seconds(5));
			CHECK(cleaned, "raw DISCONNECT removed the registration");
		}

		// DISCONNECT cleanup: the server drops the registration.
		client1.disconnect();
		const bool dropped = waitFor([&] { return server.getClientCount() == 1; },
									 std::chrono::seconds(5));
		CHECK(dropped, "client1 DISCONNECT cleaned the server table");
		client2.disconnect();
		server.stop();
		CHECK(!server.isRunning(), "server stopped");
		// port() stays valid after stop (cached bind, review §4.11).
		CHECK(server.port() == test_port, "port() valid after stop");

		std::cout << "[TEST] network integration: " << testmain::checks << " checks, "
				  << testmain::fails << " failures\n";
		return testmain::exitCode();
	}
	catch (const std::exception &e)
	{
		std::cerr << "[TEST] Exception: " << e.what() << "\n";
		return 1;
	}
}
