#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <iostream>

struct LogMessage {
	std::string text;
	bool isError;
	bool isServer;
};

class Logger {
public:
	static Logger& getInstance() {
		static Logger instance;
		return instance;
	}

	void logClient(const std::string& msg, bool isError = false) {
		std::lock_guard<std::mutex> lock(mtx);
		logs.push_back({msg, isError, false});
		if (isError) std::cerr << "[Client] " << msg << "\n";
		else std::cout << "[Client] " << msg << "\n";
	}

	void logServer(const std::string& msg, bool isError = false) {
		std::lock_guard<std::mutex> lock(mtx);
		logs.push_back({msg, isError, true});
		if (isError) std::cerr << "[Server] " << msg << "\n";
		else std::cout << "[Server] " << msg << "\n";
	}

	const std::vector<LogMessage>& getLogs() const { return logs; }
	std::mutex& getMutex() { return mtx; }

private:
	std::vector<LogMessage> logs;
	std::mutex mtx;
};
