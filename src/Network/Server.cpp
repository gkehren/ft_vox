#include "Server.hpp"
#include "../Engine/Logger.hpp"

Server::Server(unsigned short port, uint32_t worldSeed)
	: socket(ioContext, boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), port)),
	  running(false),
	  tickTimer(ioContext),
	  worldSeed(worldSeed),
	  nextPlayerId(1)
{
}

Server::~Server()
{
	stop();
}

void Server::start()
{
	if (running)
		return;
	running = true;
	serverThread = std::thread(&Server::run, this);
}

void Server::stop()
{
	if (!running)
		return;
	running = false;
	socket.close();
	ioContext.stop();
	if (serverThread.joinable())
		serverThread.join();
}

bool Server::isRunning() const
{
	return running;
}

size_t Server::getClientCount() const
{
	return playerPositions.size();
}

void Server::run()
{
	receive();
	tickTimer.expires_after(std::chrono::milliseconds(50));
	tick();
	ioContext.run();
}

void Server::receive()
{
	socket.async_receive_from(
		boost::asio::buffer(recvBuffer), senderEndpoint,
		[this](const boost::system::error_code &error, std::size_t bytesTransferred)
		{
			boost::asio::ip::udp::endpoint senderCopy = senderEndpoint;
			handleReceive(senderCopy, error, bytesTransferred);
			if (running)
				receive();
		});
}

void Server::handleReceive(const boost::asio::ip::udp::endpoint &senderEndpoint, const boost::system::error_code &error, std::size_t bytesTransferred)
{
	if (!error && bytesTransferred > 0)
	{
		std::vector<uint8_t> data(recvBuffer.begin(), recvBuffer.begin() + bytesTransferred);
		handleMessage(senderEndpoint, data);
	}
}

void Server::handleMessage(const boost::asio::ip::udp::endpoint &senderEndpoint, const std::vector<uint8_t> &data)
{
	ByteBuffer mainBuf(data);
	if (!mainBuf.hasMore(sizeof(uint8_t) + sizeof(uint32_t)))
		return;

	Message message;
	message.type = mainBuf.readUInt8();
	message.sequenceNumber = mainBuf.readUInt32();

	message.payload.insert(message.payload.end(), data.begin() + mainBuf.getReadOffset(), data.end());

	if (message.type == MessageType::REQUEST_SEED)
	{
		// Send the world seed to the client
		ByteBuffer seedBuf;
		seedBuf.writeUInt32(worldSeed);

		Message seedMessage;
		seedMessage.type = MessageType::SEND_SEED;
		seedMessage.sequenceNumber = message.sequenceNumber;
		seedMessage.payload = std::move(seedBuf.getBytes());

		sendMessage(senderEndpoint, seedMessage);

		uint32_t playerId = nextPlayerId++;
		ByteBuffer authBuf;
		authBuf.writeUInt32(playerId);

		Message authMessage;
		authMessage.type = MessageType::AUTHENTICATION;
		authMessage.sequenceNumber = message.sequenceNumber;
		authMessage.payload = std::move(authBuf.getBytes());

		sendMessage(senderEndpoint, authMessage);

		{
			std::lock_guard<std::mutex> lock(playerMutex);
			playerEndpoints[playerId] = senderEndpoint;
		}
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
			auto it = playerEndpoints.find(position.playerId);
			if (it == playerEndpoints.end() || it->second != senderEndpoint)
			{
				std::cerr << "Suspicious/unauthorized PLAYER_POSITION packet from " << senderEndpoint 
						  << " claiming to be player " << position.playerId << "\n";
				return;
			}
			playerPositions[position.playerId] = position;
		}
	}
	else if (message.type == MessageType::ACK)
	{
		ByteBuffer buf(message.payload);
		if (!buf.hasMore(sizeof(uint32_t)))
			return;

		uint32_t playerId = buf.readUInt32();

		{
			std::lock_guard<std::mutex> lock(playerMutex);
			auto it = playerEndpoints.find(playerId);
			if (it == playerEndpoints.end() || it->second != senderEndpoint)
			{
				Logger::getInstance().logServer("Suspicious/unauthorized ACK packet claiming to be player " + std::to_string(playerId), true);
				return;
			}
		}

		Logger::getInstance().logServer("Player " + std::to_string(playerId) + " authenticated successfully.");
	}
	else if (message.type == MessageType::DISCONNECT)
	{
		ByteBuffer buf(message.payload);
		if (!buf.hasMore(sizeof(uint32_t)))
			return;

		uint32_t playerId = buf.readUInt32();

		{
			std::lock_guard<std::mutex> lock(playerMutex);
			auto it = playerEndpoints.find(playerId);
			if (it == playerEndpoints.end() || it->second != senderEndpoint)
			{
				Logger::getInstance().logServer("Suspicious/unauthorized DISCONNECT packet claiming to be player " + std::to_string(playerId), true);
				return;
			}
			
			Logger::getInstance().logServer("Player " + std::to_string(playerId) + " disconnected.");
			playerEndpoints.erase(playerId);
			playerPositions.erase(playerId);
			playerSequenceNumbers.erase(playerId);
		}
	}
	else if (message.type == MessageType::VOXEL_EDIT)
	{
		ByteBuffer buf(message.payload);
		if (!buf.hasMore(sizeof(uint32_t) * 4 + sizeof(uint8_t)))
			return;

		uint32_t playerId = buf.readUInt32();
		
		{
			std::lock_guard<std::mutex> lock(playerMutex);
			auto it = playerEndpoints.find(playerId);
			if (it == playerEndpoints.end() || it->second != senderEndpoint)
			{
				Logger::getInstance().logServer("Suspicious/unauthorized VOXEL_EDIT packet claiming to be player " + std::to_string(playerId), true);
				return;
			}
		}

		Message broadcastMsg;
		broadcastMsg.type = MessageType::VOXEL_EDIT;
		broadcastMsg.payload = message.payload; // Contains playerId, x, y, z, type

		std::lock_guard<std::mutex> lock(playerMutex);
		for (const auto &[id, endpoint] : playerEndpoints)
		{
			if (id != playerId)
			{
				broadcastMsg.sequenceNumber = playerSequenceNumbers[id]++;
				sendMessage(endpoint, broadcastMsg);
			}
		}
	}
}

void Server::sendMessage(const boost::asio::ip::udp::endpoint &endpoint, const Message &message)
{
	auto data = std::make_shared<std::vector<uint8_t>>();
	ByteBuffer buf;

	// ⚡ Bolt: Pre-allocate buffer capacity to avoid intermediate dynamic allocations
	// during message serialization, drastically reducing overhead on the network thread.
	buf.reserve(sizeof(uint8_t) + sizeof(uint32_t) + message.payload.size());

	buf.writeUInt8(message.type);
	buf.writeUInt32(message.sequenceNumber);
	buf.getBytes().insert(buf.getBytes().end(), message.payload.begin(), message.payload.end());
	*data = std::move(buf.getBytes());

	socket.async_send_to(boost::asio::buffer(*data), endpoint,
						 [data](const boost::system::error_code &error, std::size_t /*bytesTransferred*/)
						 {
							 if (error)
								 std::cerr << "Failed to send message: " << error.message() << "\n";
						 });
}

void Server::tick()
{
	if (!running)
		return;

	broadcastWorldState();

	tickTimer.expires_after(std::chrono::milliseconds(50));
	tickTimer.async_wait([this](const boost::system::error_code &error)
						 {
							 if (!error)
								 tick();
						 });
}

void Server::broadcastWorldState()
{
	std::lock_guard<std::mutex> lock(playerMutex);

	if (playerPositions.empty())
		return;

	ByteBuffer buf;
	// Write the number of players in this snapshot
	buf.writeUInt32(static_cast<uint32_t>(playerPositions.size()));

	for (const auto &[playerId, position] : playerPositions)
	{
		buf.writeUInt32(position.playerId);
		buf.writeFloat(position.x);
		buf.writeFloat(position.y);
		buf.writeFloat(position.z);
	}

	Message message;
	message.type = MessageType::WORLD_STATE;
	message.payload = std::move(buf.getBytes());

	for (const auto &[id, endpoint] : playerEndpoints)
	{
		message.sequenceNumber = playerSequenceNumbers[id]++;
		sendMessage(endpoint, message);
	}
}