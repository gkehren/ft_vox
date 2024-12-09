#include "Client.hpp"

Client::Client()
	: socket(ioContext), connected(false), worldSeed(0)
{}

Client::~Client()
{
	disconnect();
}

void Client::connect(const std::string& host, unsigned short port)
{
	if (connected) return;
	connected = true;
	clientThread = std::thread(&Client::run, this);

	boost::asio::ip::tcp::resolver resolver(ioContext);
	auto endpoints = resolver.resolve(host, std::to_string(port));

	boost::asio::async_connect(socket, endpoints,
		[this](const boost::system::error_code& error, const boost::asio::ip::tcp::endpoint&)
		{
			handleConnect(error);
		});
}

void Client::handleConnect(const boost::system::error_code& error)
{
	if (error)
	{
		std::cerr << "Failed to connect to server: " << error.message() << std::endl;
		connected = false;
		disconnect();
		return;
	}

	readSeed();
}

void Client::readSeed()
{
	auto buffer = std::make_shared<std::vector<char>>(sizeof(uint32_t));
	boost::asio::async_read(socket, boost::asio::buffer(*buffer),
		[this, buffer](boost::system::error_code ec, std::size_t)
		{
			if (!ec)
			{
				uint32_t seedNetworkOrder;
				std::memcpy(&seedNetworkOrder, buffer->data(), sizeof(uint32_t));
				worldSeed = ntohl(seedNetworkOrder);
				std::cout << "Seed reçue : " << worldSeed << std::endl;
				// Continuer la communication si nécessaire
			}
			else
			{
				std::cerr << "Erreur de lecture de la seed : " << ec.message() << std::endl;
				connected = false;
				disconnect();
			}
		});
}

void Client::disconnect()
{
	if (!connected) return;
	connected = false;
	ioContext.stop();
	if (clientThread.joinable())
		clientThread.join();
}

bool Client::isConnected() const
{
	return connected;
}

uint32_t Client::getWorldSeed() const
{
	return worldSeed.load();
}

void Client::run()
{
	ioContext.run();
}