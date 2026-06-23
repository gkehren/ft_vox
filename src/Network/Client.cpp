#include "Client.hpp"

Client::Client()
	: socket(ioContext), connected(false), worldSeed(0), sequenceNumber(0), playerId(0)
{
}

Client::~Client()
{
	disconnect();
}

void Client::connect(const std::string &host, unsigned short port)
{
	if (connected)
		return;
	connected = true;
	serverEndpoint = boost::asio::ip::udp::endpoint(boost::asio::ip::make_address(host), port);
	socket.open(boost::asio::ip::udp::v4());
	clientThread = std::jthread(&Client::run, this);
}

void Client::stop()
{
	if (!connected)
		return;
	connected = false;
	socket.close();
	ioContext.stop();
}

void Client::disconnect()
{
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
	ioContext.run();
}

void Client::requestSeed()
{
	Message message;
	message.type = MessageType::REQUEST_SEED;
	message.sequenceNumber = sequenceNumber++;
	sendMessage(message);

	// std::unique_lock<std::mutex> lock(seedMutex);
	// if (!seedCondVar.wait_for(lock, std::chrono::seconds(5), [this]() { return worldSeed != 0; })) {
	//	// if we didn't receive the seed in 5 seconds, disconnect
	//	std::cerr << "Failed to receive seed from server" << "\n";
	//	if (connected)
	//		disconnect();
	// }
}

void Client::sendMessage(const Message &message)
{
	boost::asio::post(ioContext, [this, message]()
					  {
		auto data = std::make_shared<std::vector<uint8_t>>();
		ByteBuffer buf;
		buf.writeUInt8(message.type);
		buf.writeUInt32(message.sequenceNumber);
		buf.getBytes().insert(buf.getBytes().end(), message.payload.begin(), message.payload.end());
		*data = std::move(buf.getBytes());

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
		std::vector<uint8_t> data(recvBuffer.begin(), recvBuffer.begin() + bytesTransferred);
		handleMessage(data);
	}
}

void Client::handleMessage(const std::vector<uint8_t> &data)
{
	ByteBuffer mainBuf(data);
	if (!mainBuf.hasMore(sizeof(uint8_t) + sizeof(uint32_t)))
		return;

	Message message;
	message.type = mainBuf.readUInt8();
	message.sequenceNumber = mainBuf.readUInt32();

	message.payload.insert(message.payload.end(), data.begin() + mainBuf.getReadOffset(), data.end());

	if (message.type == MessageType::SEND_SEED)
	{
		ByteBuffer buf(message.payload);
		if (!buf.hasMore(sizeof(uint32_t)))
			return;
		worldSeed = buf.readUInt32();
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
	else if (message.type == MessageType::AUTHENTICATION)
	{
		ByteBuffer buf(message.payload);
		if (!buf.hasMore(sizeof(uint32_t)))
			return;
		playerId = buf.readUInt32();
		std::cout << "Authenticated with player ID: " << playerId << "\n";
		sendAck(playerId);
	}
}

void Client::sendAck(uint32_t playerId)
{
	ByteBuffer buf;
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
	return playerId;
}

void Client::sendPlayerPosition(float x, float y, float z)
{
	ByteBuffer buf;
	buf.writeUInt32(playerId);
	buf.writeFloat(x);
	buf.writeFloat(y);
	buf.writeFloat(z);

	Message message;
	message.type = MessageType::PLAYER_POSITION;
	message.sequenceNumber = sequenceNumber++;
	message.payload = std::move(buf.getBytes());

	sendMessage(message);
}