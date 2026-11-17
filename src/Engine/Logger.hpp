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
	// Bounded retention (review §4.8): the old vector grew without limit.
	static constexpr std::size_t kMaxLogs = 512;

	static Logger& getInstance() {
		static Logger instance;
		return instance;
	}

	void logClient(const std::string& msg, bool isError = false) {
		std::lock_guard<std::mutex> lock(mtx);
		appendLocked({msg, isError, false});
		if (isError) std::cerr << "[Client] " << msg << "\n";
		else std::cout << "[Client] " << msg << "\n";
	}

	void logServer(const std::string& msg, bool isError = false) {
		std::lock_guard<std::mutex> lock(mtx);
		appendLocked({msg, isError, true});
		if (isError) std::cerr << "[Server] " << msg << "\n";
		else std::cout << "[Server] " << msg << "\n";
	}

	// Locked snapshot (review §4.8): the old getLogs() handed out an
	// unprotected reference to a vector other threads push into — a data
	// race the moment the logger serves anything beyond the single-threaded
	// test path. Callers now own a stable copy.
	std::vector<LogMessage> getLogs() const {
		std::lock_guard<std::mutex> lock(mtx);
		return logs;
	}

private:
	void appendLocked(LogMessage msg) {
		if (logs.size() >= kMaxLogs)
			logs.erase(logs.begin());
		logs.push_back(std::move(msg));
	}

	std::vector<LogMessage> logs;
	mutable std::mutex mtx;
};
