#pragma once

#include <thread>
#include <atomic>
#include <boost/asio.hpp>
#include <condition_variable>
#include <iostream>

#include "common.hpp"

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
		void stop();
		void sendMessage(const Message& message);
		void receive();
		void handleReceive(const boost::system::error_code& error, std::size_t bytesTransferred);
		void handleMessage(const std::vector<uint8_t>& data);

		void requestSeed();

		std::thread clientThread;
		std::atomic<bool> connected;

		boost::asio::io_context ioContext;
		boost::asio::ip::udp::socket socket;
		boost::asio::ip::udp::endpoint serverEndpoint;

		std::atomic<uint32_t> worldSeed;
		std::mutex seedMutex;
		std::condition_variable seedCondVar;

		uint32_t sequenceNumber;

		std::array<uint8_t, 1024> recvBuffer;
};