#include "Client.hpp"

Client::Client()
	: socket(ioContext), connected(false), worldSeed(0), sequenceNumber(0)
{}

Client::~Client()
{
	disconnect();
}

void Client::connect(const std::string& host, unsigned short port)
{
	if (connected) return;
	connected = true;
	serverEndpoint = boost::asio::ip::udp::endpoint(boost::asio::ip::address::from_string(host), port);
	socket.open(boost::asio::ip::udp::v4());
	clientThread = std::thread(&Client::run, this);
}

void Client::stop()
{
	if (!connected) return;
	connected = false;
	socket.close();
	ioContext.stop();
}

void Client::disconnect()
{
	stop();

	if (clientThread.joinable() && clientThread.get_id() != std::this_thread::get_id())
		clientThread.join();
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

	receive();

	std::unique_lock<std::mutex> lock(seedMutex);
	if (!seedCondVar.wait_for(lock, std::chrono::seconds(5), [this]() { return worldSeed != 0; })) {
		// if we didn't receive the seed in 5 seconds, disconnect
		std::cerr << "Failed to receive seed from server" << std::endl;
		if (connected)
			disconnect();
	}
}

void Client::sendMessage(const Message& message)
{
	std::vector<uint8_t> data;
	data.push_back(message.type);
	uint32_t seqNetOrder = htonl(message.sequenceNumber);
	data.insert(data.end(), reinterpret_cast<uint8_t*>(&seqNetOrder), reinterpret_cast<uint8_t*>(&seqNetOrder) + sizeof(uint32_t));
	data.insert(data.end(), message.payload.begin(), message.payload.end());

	socket.async_send_to(boost::asio::buffer(data), serverEndpoint,
		[this](const boost::system::error_code& error, std::size_t bytesTransferred)
		{
			if (error) {
				std::cerr << "Error sending message: " << error.message() << std::endl;
			}
		}
	);
}

void Client::receive()
{
	socket.async_receive_from(
		boost::asio::buffer(recvBuffer), serverEndpoint,
		[this](const boost::system::error_code& error, std::size_t bytesTransferred)
		{
			handleReceive(error, bytesTransferred);
			if (connected)
				receive();
		}
	);
}

void Client::handleReceive(const boost::system::error_code& error, std::size_t bytesTransferred)
{
	if (!error && bytesTransferred > 0)
	{
		std::vector<uint8_t> data(recvBuffer.begin(), recvBuffer.begin() + bytesTransferred);
		handleMessage(data);
	}
}

void Client::handleMessage(const std::vector<uint8_t>& data)
{
	if (data.size() < sizeof(uint8_t) + sizeof(uint32_t))
		return;

	Message message;
	size_t offset = 0;

	message.type = data[offset++];
	std::memcpy(&message.sequenceNumber, &data[offset], sizeof(uint32_t));
	offset += sizeof(uint32_t);

	message.payload.insert(message.payload.end(), data.begin() + offset, data.end());

	if (message.type == MessageType::SEND_SEED)
	{
		uint32_t seedNetworkOrder;
		std::memcpy(&seedNetworkOrder, message.payload.data(), sizeof(uint32_t));
		worldSeed = ntohl(seedNetworkOrder);
		std::cout << "Seed received: " << worldSeed << std::endl;

		std::unique_lock<std::mutex> lock(seedMutex);
		seedCondVar.notify_one();
	}
}

uint32_t Client::getWorldSeed() const
{
	return worldSeed.load();
}