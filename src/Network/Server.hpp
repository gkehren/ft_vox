#pragma once

#include <thread>
#include <atomic>
#include <vector>
#include <mutex>
#include <boost/asio.hpp>
#include <iostream>

class Server
{
	public:
		Server(unsigned short port, uint32_t worldSeed);
		~Server();

		void start();
		void stop();
		bool isRunning() const;

		size_t getClientCount();

	private:
		void run();
		void doAccept();
		void handleClient(std::shared_ptr<boost::asio::ip::tcp::socket> client);
		void disconnectClient(std::shared_ptr<boost::asio::ip::tcp::socket> client);

		std::thread serverThread;
		std::atomic<bool> running;
		std::mutex clientMutex;

		boost::asio::io_context ioContext;
		boost::asio::ip::tcp::acceptor acceptor;
		std::vector<std::shared_ptr<boost::asio::ip::tcp::socket>> clients;
		uint32_t worldSeed;
};