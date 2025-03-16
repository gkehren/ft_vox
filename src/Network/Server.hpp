#pragma once

#include <thread>
#include <atomic>
#include <unordered_map>
#include <mutex>
#include <boost/asio.hpp>
#include <iostream>

#include "common.hpp"

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
	void receive();
	void handleReceive(const boost::asio::ip::udp::endpoint &senderEndpoint, const boost::system::error_code &error, std::size_t bytesTransferred);

	void handleMessage(const boost::asio::ip::udp::endpoint &senderEndpoint, const std::vector<uint8_t> &data);
	void sendMessage(const boost::asio::ip::udp::endpoint &endpoint, const Message &message);
	void broadcastPlayerPosition();

	std::thread serverThread;
	std::atomic<bool> running;

	boost::asio::io_context ioContext;
	boost::asio::ip::udp::socket socket;

	uint32_t worldSeed;

	std::array<uint8_t, 1024> recvBuffer;

	std::unordered_map<uint32_t, PlayerPosition> playerPositions;
	std::unordered_map<uint32_t, boost::asio::ip::udp::endpoint> playerEndpoints;
	std::mutex playerMutex;

	uint32_t nextPlayerId;
};