#include "Server.hpp"

Server::Server(unsigned short port, uint32_t worldSeed)
	: socket(ioContext, boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), port)),
		running(false),
		worldSeed(worldSeed),
		nextPlayerId(1)
{}

Server::~Server()
{
	stop();
}

void Server::start()
{
	if (running) return;
	running = true;
	serverThread = std::thread(&Server::run, this);
}

void Server::stop()
{
	if (!running) return;
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

size_t Server::getClientCount()
{
	std::lock_guard<std::mutex> lock(playerMutex);
	return playerPositions.size();
}

void Server::run()
{
	receive();
	ioContext.run();
}

void Server::receive()
{
	auto senderEndpoint = std::make_shared<boost::asio::ip::udp::endpoint>();
	socket.async_receive_from(
		boost::asio::buffer(recvBuffer), *senderEndpoint,
		[this, senderEndpoint](const boost::system::error_code& error, std::size_t bytesTransferred)
		{
			handleReceive(*senderEndpoint, error, bytesTransferred);
			if (running)
				receive();
		}
	);
}

void Server::handleReceive(const boost::asio::ip::udp::endpoint& senderEndpoint, const boost::system::error_code& error, std::size_t bytesTransferred)
{
	if (!error && bytesTransferred > 0)
	{
		std::vector<uint8_t> data(recvBuffer.begin(), recvBuffer.begin() + bytesTransferred);
		handleMessage(senderEndpoint, data);
	}
}

void Server::handleMessage(const boost::asio::ip::udp::endpoint& senderEndpoint, const std::vector<uint8_t>& data)
{
	if (data.size() < sizeof(uint8_t) + sizeof(uint32_t))
		return;

	Message message;
	size_t offset = 0;

	message.type = data[offset++];
	std::memcpy(&message.sequenceNumber, &data[offset], sizeof(uint32_t));
	offset += sizeof(uint32_t);

	message.payload.insert(message.payload.end(), data.begin() + offset, data.end());

	if (message.type == MessageType::REQUEST_SEED)
	{
		// Send the world seed to the client
		Message seedMessage;
		seedMessage.type = MessageType::SEND_SEED;
		seedMessage.sequenceNumber = message.sequenceNumber;
		uint32_t seedNetworkOrder = htonl(worldSeed);
		seedMessage.payload.resize(sizeof(uint32_t));
		std::memcpy(seedMessage.payload.data(), &seedNetworkOrder, sizeof(uint32_t));

		sendMessage(senderEndpoint, seedMessage);

		uint32_t playerId = nextPlayerId++;
		Message authMessage;
		authMessage.type = MessageType::AUTHENTICATION;
		authMessage.sequenceNumber = message.sequenceNumber;
		uint32_t playerIdNetworkOrder = htonl(playerId);
		authMessage.payload.resize(sizeof(uint32_t));
		std::memcpy(authMessage.payload.data(), &playerIdNetworkOrder, sizeof(uint32_t));

		sendMessage(senderEndpoint, authMessage);

		{
			std::lock_guard<std::mutex> lock(playerMutex);
			playerEndpoints[playerId] = senderEndpoint;
		}
	}
	else if (message.type == MessageType::PLAYER_POSITION)
	{
		if (message.payload.size() < sizeof(PlayerPosition))
			return;

		PlayerPosition position;
		std::memcpy(&position, message.payload.data(), sizeof(PlayerPosition));

		{
			std::lock_guard<std::mutex> lock(playerMutex);
			playerPositions[position.playerId] = position;
			playerEndpoints[position.playerId] = senderEndpoint;
		}

		broadcastPlayerPosition();
	}
	else if (message.type == MessageType::ACK)
	{
		uint32_t playerId;
		std::memcpy(&playerId, message.payload.data(), sizeof(uint32_t));
		playerId = ntohl(playerId);

		std::cout << "Player " << playerId << " authenticated successfully." << std::endl;
	}
}

void Server::sendMessage(const boost::asio::ip::udp::endpoint& endpoint, const Message& message)
{
	std::vector<uint8_t> data;
	data.push_back(message.type);
	uint32_t seqNetOrder = htonl(message.sequenceNumber);
	data.insert(data.end(), reinterpret_cast<uint8_t*>(&seqNetOrder), reinterpret_cast<uint8_t*>(&seqNetOrder) + sizeof(uint32_t));
	data.insert(data.end(), message.payload.begin(), message.payload.end());

	socket.async_send_to(boost::asio::buffer(data), endpoint,
		[](const boost::system::error_code& error, std::size_t /*bytesTransferred*/)
		{
			if (error)
				std::cerr << "Failed to send message: " << error.message() << std::endl;
		}
	);
}

void Server::broadcastPlayerPosition()
{
	std::lock_guard<std::mutex> lock(playerMutex);

	for (const auto& [playerId, position] : playerPositions)
	{
		Message message;
		message.type = MessageType::PLAYER_POSITION;
		message.sequenceNumber = message.sequenceNumber;
		message.payload.resize(sizeof(PlayerPosition));
		std::memcpy(message.payload.data(), &position, sizeof(PlayerPosition));

		for (const auto& [id, endpoint] : playerEndpoints)
		{
			if (id != playerId)
				sendMessage(endpoint, message);
		}
	}
}