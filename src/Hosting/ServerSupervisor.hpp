#pragma once

#include <Network/WireProtocol.hpp>
#include <Core/ServerStatus.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace hosting
{
inline constexpr int kMaxPlayerSlots = static_cast<int>(monitoring::kMaxPlayers);
inline constexpr int kMaxInterestRadius = 8;
inline constexpr size_t kMaxLogLines = 48;
inline constexpr size_t kMaxLogLineBytes = 1024;

enum class StreamingPreset { Low, Balanced, High };

struct Options
{
	std::string world;
	int port{7777};
	bool lan{};
	std::string address;
	int playerSlots{15}, interestRadius{3}, workers{2};
	StreamingPreset streaming{StreamingPreset::Balanced};
};

struct OwnerProfile
{
	net::wire::PlayerKey player;
	std::filesystem::path controlFile;
};

std::string validate(const Options &options);
std::vector<std::string> listWorlds(const std::filesystem::path &root);

enum class State { Idle, Starting, Ready, Stopping, Stopped, Failed };

struct Snapshot
{
	State state{State::Idle};
	std::string world, address, message;
	uint16_t port{};
	uint64_t checkpoint{};
	uint64_t launch{};
	bool delayed{};
	std::deque<std::string> logs;
	std::shared_ptr<const monitoring::Status> status;

	bool active() const { return state == State::Starting || state == State::Ready || state == State::Stopping; }
};

// One child per launcher. All process creation, pipe reads, waits and signals
// run on the supervisor worker. The frame owner only reads state or queues work.
class ServerSupervisor
{
public:
	explicit ServerSupervisor(std::chrono::milliseconds notice = std::chrono::seconds(5)) : m_notice(notice) {}
	~ServerSupervisor();
	ServerSupervisor(const ServerSupervisor &) = delete;
	ServerSupervisor &operator=(const ServerSupervisor &) = delete;

	bool start(Options options, std::filesystem::path executable, std::filesystem::path savesRoot,
		std::optional<OwnerProfile> owner = std::nullopt);
	void stop();
	bool forceStop();
	Snapshot snapshot() const;
	std::optional<Snapshot> snapshotIfChanged(uint64_t &revision) const;

private:
	enum class Command { None, Stop, Force };
	void run(std::stop_token lifetime, const Options &options, const std::filesystem::path &executable,
		const std::filesystem::path &savesRoot, const std::optional<OwnerProfile> &owner);
	void publish(const Snapshot &next);

	const std::chrono::milliseconds m_notice;
	mutable std::mutex m_mutex;
	Snapshot m_snapshot;
	std::atomic<uint64_t> m_revision{};
	std::atomic<Command> m_command{Command::None};
	std::jthread m_worker;
};
}
