#include "Client.hpp"
#include "../Engine/Logger.hpp"
#include <vector>
#include <algorithm>
#include <iostream>

Client::Client()
	: connected(false), socket(ioContext), worldSeed(0), handshakeTimer(ioContext),
	  sequenceNumber(0), playerId(0)
{
}

Client::~Client()
{
	disconnect();
}

bool Client::connect(const std::string &host, unsigned short port, std::string &outError)
{
	outError.clear();
	if (connected)
	{
		outError = "client already connected";
		return false;
	}
	// Resolver instead of make_address (review M18): hostnames like
	// "localhost" resolve; a bad host reports through outError instead of
	// throwing into the caller with connected already set.
	try
	{
		boost::asio::ip::udp::resolver resolver(ioContext);
		const auto results = resolver.resolve(boost::asio::ip::udp::v4(), host, std::to_string(port));
		if (results.empty())
		{
			outError = "no UDP address resolved for host '" + host + "'";
			return false;
		}
		serverEndpoint = *results.begin();
		socket.open(boost::asio::ip::udp::v4());
	}
	catch (const std::exception &e)
	{
		outError = e.what();
		boost::system::error_code ec;
		socket.close(ec);
		return false;
	}
	connected = true;
	clientThread = std::jthread(&Client::run, this);
	return true;
}

void Client::stop()
{
	if (!connected.exchange(false))
		return;
	// Socket lifetime belongs to the io thread (review M18): asio sockets
	// are not thread-safe, so close/stop are posted instead of executed on
	// the caller thread while async operations may be in flight.
	boost::asio::post(ioContext, [this]
					  {
		boost::system::error_code ec;
		socket.close(ec);
		ioContext.stop(); });
}

void Client::disconnect()
{
	if (connected && playerId.load() != 0)
	{
		Message message;
		message.type = MessageType::DISCONNECT;
		message.sequenceNumber = sequenceNumber++;
		ByteBuffer buf;
		buf.reserve(sizeof(uint32_t));
		buf.writeUInt32(playerId.load());
		message.payload = std::move(buf.getBytes());

		// Posted BEFORE stop()'s close handler: asio runs posted handlers in
		// order, so the DISCONNECT datagram leaves on the io thread while the
		// socket is still open (review M18: no cross-thread send_to).
		auto data = std::make_shared<std::vector<uint8_t>>(serializeMessage(message));
		boost::asio::post(ioContext, [this, data]
						  {
			boost::system::error_code ec;
			socket.send_to(boost::asio::buffer(*data), serverEndpoint, 0, ec); });
	}
	stop();
}

bool Client::isConnected() const
{
	return connected;
}

void Client::run()
{
	receive();
	requestSeed();
	// Bounded handshake (review M20): without a SEND_SEED response the
	// client must not stay 'connected' forever. The timer lives on the io
	// thread; SEND_SEED cancels it.
	handshakeTimer.expires_after(std::chrono::seconds(5));
	handshakeTimer.async_wait([this](const boost::system::error_code &error)
							  {
		if (error || worldSeed.load() != 0 || !connected)
			return;
		std::cerr << "Failed to receive seed from server within 5s; disconnecting\n";
		connected = false;
		boost::system::error_code ec;
		socket.close(ec);
		ioContext.stop(); });
	ioContext.run();
}

void Client::requestSeed()
{
	Message message;
	message.type = MessageType::REQUEST_SEED;
	message.sequenceNumber = sequenceNumber++;
	sendMessage(message);
}

void Client::sendMessage(const Message &message)
{
	boost::asio::post(ioContext, [this, message]()
					  {
		auto data = std::make_shared<std::vector<uint8_t>>(serializeMessage(message));
		socket.async_send_to(boost::asio::buffer(*data), serverEndpoint,
							 [this, data](const boost::system::error_code &error, std::size_t bytesTransferred)
							 {
								 if (error)
								 {
									 std::cerr << "Error sending message: " << error.message() << "\n";
								 }
							 }); });
}

void Client::receive()
{
	socket.async_receive_from(
		boost::asio::buffer(recvBuffer), serverEndpoint,
		[this](const boost::system::error_code &error, std::size_t bytesTransferred)
		{
			handleReceive(error, bytesTransferred);
			if (connected)
				receive();
		});
}

void Client::handleReceive(const boost::system::error_code &error, std::size_t bytesTransferred)
{
	if (!error && bytesTransferred > 0)
	{
		// thread_local: the buffer keeps its capacity across packets instead
		// of allocating once per receive.
		thread_local std::vector<uint8_t> data;
		data.assign(recvBuffer.begin(), recvBuffer.begin() + bytesTransferred);
		handleMessage(data);
	}
}

void Client::handleMessage(const std::vector<uint8_t> &data)
{
	Message message;
	if (!parseMessage(data, message))
		return; // truncated or foreign protocol version

	if (message.type == MessageType::SEND_SEED)
	{
		ByteBuffer buf(message.payload);
		if (!buf.hasMore(sizeof(uint32_t)))
			return;
		worldSeed = buf.readUInt32();
		handshakeTimer.cancel();
		std::cout << "Seed received: " << worldSeed << "\n";
	}
	else if (message.type == MessageType::PLAYER_POSITION)
	{
		ByteBuffer buf(message.payload);
		if (!buf.hasMore(sizeof(uint32_t) + 3 * sizeof(float)))
			return;

		PlayerPosition position;
		position.playerId = buf.readUInt32();
		position.x = buf.readFloat();
		position.y = buf.readFloat();
		position.z = buf.readFloat();

		{
			std::lock_guard<std::mutex> lock(playerMutex);
			playerPositions[position.playerId] = position;
		}
	}
	else if (message.type == MessageType::WORLD_STATE)
	{
		worldStatePacketCount++;
		lastWorldStateSequenceNumber = message.sequenceNumber;

		ByteBuffer buf(message.payload);
		if (!buf.hasMore(sizeof(uint32_t)))
			return;

		uint32_t numPlayers = buf.readUInt32();
		// 64-bit size arithmetic + hard cap (review M20): numPlayers * 16
		// could wrap on 32-bit platforms and throw out_of_range inside an
		// asio handler (dead io thread / terminate). The cap also bounds the
		// per-packet work; it matches the server's player cap headroom.
		constexpr uint32_t kMaxWorldStatePlayers = 256;
		if (numPlayers > kMaxWorldStatePlayers)
			return;
		const uint64_t needed =
			static_cast<uint64_t>(numPlayers) * (sizeof(uint32_t) + 3 * sizeof(float));
		if (!buf.hasMore(static_cast<std::size_t>(needed)))
			return;

		// thread_local: the vector keeps its capacity across packets instead
		// of allocating once per world-state message.
		thread_local std::vector<uint32_t> currentPlayers;
		currentPlayers.clear();
		currentPlayers.reserve(numPlayers);

		std::lock_guard<std::mutex> lock(playerMutex);
		for (uint32_t i = 0; i < numPlayers; ++i)
		{
			PlayerPosition position;
			position.playerId = buf.readUInt32();
			position.x = buf.readFloat();
			position.y = buf.readFloat();
			position.z = buf.readFloat();
			playerPositions[position.playerId] = position;
			currentPlayers.push_back(position.playerId);
		}

		std::sort(currentPlayers.begin(), currentPlayers.end());

		const uint32_t selfId = playerId.load();
		for (auto it = playerPositions.begin(); it != playerPositions.end(); )
		{
			if (!std::binary_search(currentPlayers.begin(), currentPlayers.end(), it->first) && it->first != selfId)
			{
				Logger::getInstance().logClient("Player " + std::to_string(it->first) + " disconnected.");
				it = playerPositions.erase(it);
			}
			else
			{
				++it;
			}
		}
	}
	else if (message.type == MessageType::VOXEL_EDIT)
	{
		ByteBuffer buf(message.payload);
		if (!buf.hasMore(sizeof(uint32_t) * 4 + sizeof(uint8_t)))
			return;
			
		VoxelEditEvent edit;
		edit.playerId = buf.readUInt32();
		edit.x = static_cast<int32_t>(buf.readUInt32());
		edit.y = static_cast<int32_t>(buf.readUInt32());
		edit.z = static_cast<int32_t>(buf.readUInt32());
		edit.type = buf.readUInt8();

		std::lock_guard<std::mutex> lock(voxelEditMutex);
		pendingVoxelEdits.push_back(edit);
	}
	else if (message.type == MessageType::AUTHENTICATION)
	{
		ByteBuffer buf(message.payload);
		if (!buf.hasMore(sizeof(uint32_t)))
			return;
		playerId.store(buf.readUInt32());
		Logger::getInstance().logClient("Authenticated with player ID: " + std::to_string(playerId.load()));
		sendAck(playerId.load());
	}
}

void Client::sendAck(uint32_t playerId)
{
	ByteBuffer buf;
	buf.reserve(sizeof(uint32_t));
	buf.writeUInt32(playerId);

	Message message;
	message.type = MessageType::ACK;
	message.sequenceNumber = sequenceNumber++;
	message.payload = std::move(buf.getBytes());

	sendMessage(message);
}

uint32_t Client::getWorldSeed() const
{
	return worldSeed.load();
}

uint32_t Client::getPlayerId() const
{
	return playerId.load();
}

void Client::sendPlayerPosition(float x, float y, float z)
{
	ByteBuffer buf;
	buf.reserve(sizeof(uint32_t) + 3 * sizeof(float));
	buf.writeUInt32(playerId.load());
	buf.writeFloat(x);
	buf.writeFloat(y);
	buf.writeFloat(z);

	Message message;
	message.type = MessageType::PLAYER_POSITION;
	message.sequenceNumber = sequenceNumber++;
	message.payload = std::move(buf.getBytes());

	sendMessage(message);
}

void Client::sendVoxelEdit(int32_t x, int32_t y, int32_t z, uint8_t type)
{
	if (!connected || playerId.load() == 0)
		return;

	Message message;
	message.type = MessageType::VOXEL_EDIT;
	message.sequenceNumber = sequenceNumber++;
	
	ByteBuffer buf;
	buf.reserve(sizeof(uint32_t) * 4 + sizeof(uint8_t));
	buf.writeUInt32(playerId.load());
	buf.writeUInt32(static_cast<uint32_t>(x));
	buf.writeUInt32(static_cast<uint32_t>(y));
	buf.writeUInt32(static_cast<uint32_t>(z));
	buf.writeUInt8(type);
	
	message.payload = std::move(buf.getBytes());
	sendMessage(message);
}

std::vector<VoxelEditEvent> Client::getPendingVoxelEdits()
{
	std::lock_guard<std::mutex> lock(voxelEditMutex);
	std::vector<VoxelEditEvent> edits = std::move(pendingVoxelEdits);
	pendingVoxelEdits.clear();
	return edits;
}
