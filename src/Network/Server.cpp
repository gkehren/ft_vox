#include "Server.hpp"

Server::Server(unsigned short port, uint32_t worldSeed)
	: acceptor(ioContext, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), port)),
		running(false),
		worldSeed(worldSeed)
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
	acceptor.close();
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
	std::lock_guard<std::mutex> lock(clientMutex);
	return clients.size();
}

void Server::run()
{
	doAccept();
	ioContext.run();
}

void Server::doAccept()
{
	auto socket = std::make_shared<boost::asio::ip::tcp::socket>(ioContext);
	acceptor.async_accept(*socket,
		[this, socket](boost::system::error_code ec)
		{
			if (!ec && running)
			{
				{
					std::lock_guard<std::mutex> lock(clientMutex);
					clients.push_back(socket);
				}
				handleClient(socket);
			}
			if (running)
				doAccept();
		});
}

void Server::handleClient(std::shared_ptr<boost::asio::ip::tcp::socket> client)
{
	std::cout << "New client connected: " << client->remote_endpoint() << std::endl;

	uint32_t seedNetworkOrder = htonl(worldSeed);
	auto buffer = std::make_shared<std::vector<char>>(reinterpret_cast<char*>(&seedNetworkOrder),
														reinterpret_cast<char*>(&seedNetworkOrder) + sizeof(seedNetworkOrder));
	boost::asio::async_write(*client, boost::asio::buffer(*buffer),
		[this, client, buffer](boost::system::error_code ec, std::size_t)
		{
			if (ec)
			{
				// Handle error
				disconnectClient(client);
			}
			else
			{
				// Handle success
			}
		});

	auto readBuffer = std::make_shared<std::vector<char>>(1024);
	client->async_read_some(boost::asio::buffer(*readBuffer),
		[this, client, readBuffer](boost::system::error_code ec, std::size_t length)
		{
			if (ec)
			{
				// Handle error
				disconnectClient(client);
			}
			else
			{
				// Handle success
			}
		});
}

void Server::disconnectClient(std::shared_ptr<boost::asio::ip::tcp::socket> client)
{
	std::cout << "Client disconnected: " << client->remote_endpoint() << std::endl;
	std::lock_guard<std::mutex> lock(clientMutex);
	clients.erase(std::remove(clients.begin(), clients.end(), client), clients.end());
	client->close();
}