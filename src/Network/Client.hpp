#pragma once

#include <thread>
#include <atomic>
#include <boost/asio.hpp>
#include <iostream>

class Client
{
	public:
		Client();
		~Client();

		void connect(const std::string& host, unsigned short port);
		void disconnect();
		bool isConnected() const;

		uint32_t getWorldSeed() const;

	private:
		void run();
		void handleConnect(const boost::system::error_code& error);
		void readSeed();

		std::thread clientThread;
		std::atomic<bool> connected;

		boost::asio::io_context ioContext;
		boost::asio::ip::tcp::socket socket;

		std::atomic<uint32_t> worldSeed;
};