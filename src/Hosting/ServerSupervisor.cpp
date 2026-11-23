#include <Hosting/ServerSupervisor.hpp>
#include <Hosting/ChildProcess.hpp>
#include <Hosting/RoleCredentials.hpp>

#include <Network/UdpSocket.hpp>
#include <Sim/GameSimulation.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <optional>
#include <stdexcept>
#include <utility>

namespace hosting
{
namespace
{
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

static_assert(kMaxPlayerSlots + 1 == sim::GameSimulation::kMaxPlayers);
static_assert(kMaxInterestRadius == sim::kMaxPlayerInterestRadiusChunks);

bool validWorldName(std::string_view name)
{
	if (name.empty() || name.size() > 48 || name.front() == '-' || name.front() == ' ' || name.back() == ' ')
		return false;
	std::string lower;
	for (char c : name)
	{
		if (c >= 'A' && c <= 'Z')
			c = static_cast<char>(c - 'A' + 'a');
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_'))
			return false;
		lower += c;
	}
	return lower != "con" && lower != "prn" && lower != "aux" && lower != "nul" &&
		!(lower.size() == 4 && (lower.starts_with("com") || lower.starts_with("lpt")) &&
			lower.back() >= '0' && lower.back() <= '9');
}

bool isPrivateAddress(uint32_t ipv4)
{
	return (ipv4 >> 24) == 10 || (ipv4 >> 20) == 0xac1 || (ipv4 >> 16) == 0xc0a8 ||
		(ipv4 >> 22) == 0x191; // RFC 1918 and 100.64.0.0/10 (private VPNs)
}

PlayerStreamingBudget budget(StreamingPreset preset)
{
	switch (preset)
	{
	case StreamingPreset::Low: return {12, 2, 8};
	case StreamingPreset::Balanced: return {32, 8, 16};
	case StreamingPreset::High: return {64, 16, 32};
	}
	return {};
}

std::vector<fs::path> launchArguments(const Options &options, const fs::path &savesRoot, std::string_view address,
	bool bootstrap)
{
	const auto streams = budget(options.streaming);
	return {"--world", options.world, "--saves-root", savesRoot, "--listen-address", std::string(address),
		"--port", std::to_string(options.port), "--player-cap", std::to_string(options.playerSlots + 1),
		"--interest-radius", std::to_string(options.interestRadius), "--workers", std::to_string(options.workers),
		"--load-budget", std::to_string(streams.loads), "--generation-budget", std::to_string(streams.generations),
		"--eviction-budget", std::to_string(streams.evictions), "--launcher-control", bootstrap ? "bootstrap" : "stdin"};
}

std::string_view field(std::string_view line, std::string_view key)
{
	while (!line.empty())
	{
		const auto end = line.find(' ');
		const auto token = line.substr(0, end);
		if (token.starts_with(key))
			return token.substr(key.size());
		if (end == std::string_view::npos)
			break;
		line.remove_prefix(end + 1);
	}
	return {};
}

template<class Integer>
bool readNumber(std::string_view text, Integer &value)
{
	if (text.empty())
		return false;
	const char *end = text.data() + text.size();
	const auto parsed = std::from_chars(text.data(), end, value);
	return parsed.ec == std::errc{} && parsed.ptr == end;
}

struct ServerOutput
{
	void onLine(Snapshot &snapshot, std::string_view record, int requestedPort)
	{
		if (record.starts_with("SERVER error="))
			error = record;
		if (record.starts_with("SERVER status=ready ") && snapshot.state == State::Starting)
		{
			uint16_t port{};
			if (field(record, "mode=") == "live" && field(record, "transport=") == "udp" &&
				field(record, "listen_address=") == snapshot.address &&
				readNumber(field(record, "port="), port) && port > 0 && (requestedPort == 0 || port == requestedPort))
			{
				snapshot.port = port;
				snapshot.state = State::Ready;
				snapshot.delayed = false;
				snapshot.message = "Server is ready.";
				readNumber(field(record, "checkpoint_generation="), snapshot.checkpoint);
			}
		}
		if (record.starts_with("SERVER status=stopped "))
			committed = field(record, "saved=") == "1" && field(record, "exit_code=") == "0" &&
				readNumber(field(record, "checkpoint_generation="), checkpoint) && checkpoint > 0;
		if (record.starts_with("SERVER status=stopping ") && snapshot.state != State::Stopping)
		{
			snapshot.state = State::Stopping;
			snapshot.message = "The server is committing the world.";
			snapshot.delayed = false;
		}
	}

	bool drain(ChildProcess &child, Snapshot &snapshot, int requestedPort, unsigned batches)
	{
		bool changed = false;
		std::array<char, 4096> bytes{};
		for (unsigned i = 0; i < batches; ++i)
		{
			const auto count = child.readOutput(bytes);
			if (count == 0)
				break;
			for (size_t j = 0; j < count; ++j)
			{
				const char c = bytes[j];
				if (c == '\n')
				{
					if (!truncated)
						onLine(snapshot, line, requestedPort);
					if (!line.empty())
					{
						if (snapshot.logs.size() == kMaxLogLines)
							snapshot.logs.pop_front();
						snapshot.logs.push_back(std::move(line));
						line.clear();
						changed = true;
					}
					truncated = false;
				}
				else if (c != '\r')
				{
					if (line.size() < kMaxLogLineBytes)
						line += c == '\t' || static_cast<unsigned char>(c) >= 32 ? c : ' ';
					else
						truncated = true;
				}
			}
		}
		return changed;
	}

	std::string line, error;
	bool truncated{}, committed{};
	uint64_t checkpoint{};
};
}

std::string validate(const Options &options)
{
	if (!validWorldName(options.world))
		return "Enter a world name of up to 48 letters, digits, spaces, dashes or underscores.";
	if (options.port < 0 || options.port > 65535 || options.playerSlots < 1 ||
		options.playerSlots > kMaxPlayerSlots || options.interestRadius < 0 ||
		options.interestRadius > kMaxInterestRadius || options.workers < 0 || options.workers > 64 ||
		options.streaming < StreamingPreset::Low || options.streaming > StreamingPreset::High)
		return "Check the port, player slots, interest radius and server presets.";
	if (options.lan)
	{
		try
		{
			if (options.address.size() <= 15 && isPrivateAddress(net::udp::parseAddress(options.address, 1).ipv4))
				return {};
		}
		catch (const std::exception &) {}
		return "Enter a numeric IPv4 address in a private LAN or VPN range.";
	}
	return {};
}

std::vector<std::string> listWorlds(const fs::path &root)
{
	std::vector<std::string> worlds;
	std::error_code error;
	for (fs::directory_iterator it(root, error), end; !error && it != end; it.increment(error))
	{
		std::error_code typeError;
		if (it->symlink_status(typeError).type() != fs::file_type::directory ||
			!fs::is_regular_file(it->path() / "world.meta", typeError))
			continue;
		const auto filename = it->path().filename();
		if (!std::ranges::all_of(filename.native(), [](auto c) { return static_cast<unsigned>(c) <= 127; }))
			continue;
		const auto name = filename.generic_string();
		if (!validWorldName(name))
			continue;
		if (worlds.size() == 64)
		{
			if (name >= worlds.back())
				continue;
			worlds.pop_back();
		}
		worlds.insert(std::ranges::lower_bound(worlds, name), name);
	}
	return worlds;
}

ServerSupervisor::~ServerSupervisor()
{
	stop();
	// Normal UI exit waits until the worker has reaped the child. During
	// unwinding the worker closes our pipe and makes a bounded reap attempt.
}

bool ServerSupervisor::start(Options options, fs::path executable, fs::path savesRoot,
	std::optional<OwnerProfile> owner)
{
	std::lock_guard lock(m_mutex);
	if (m_snapshot.active())
		return false;
	m_snapshot = {};
	m_snapshot.world = options.world;
	m_snapshot.address = options.lan ? options.address : "127.0.0.1";
	if (auto error = validate(options); !error.empty())
	{
		m_snapshot.state = State::Failed;
		m_snapshot.message = std::move(error);
	}
	else
	{
		m_command.store(Command::None);
		m_snapshot.state = State::Starting;
		m_snapshot.message = "Starting the dedicated server.";
		try
		{
			m_worker = std::jthread([this, options = std::move(options), executable = std::move(executable),
				savesRoot = std::move(savesRoot), owner = std::move(owner)](std::stop_token lifetime) {
				run(lifetime, options, executable, savesRoot, owner);
			});
		}
		catch (const std::exception &error)
		{
			m_snapshot.state = State::Failed;
			m_snapshot.message = error.what();
		}
	}
	m_revision.fetch_add(1, std::memory_order_release);
	return m_snapshot.state == State::Starting;
}

void ServerSupervisor::stop()
{
	auto expected = Command::None;
	m_command.compare_exchange_strong(expected, Command::Stop);
}

bool ServerSupervisor::forceStop()
{
	std::lock_guard lock(m_mutex);
	if (m_snapshot.state != State::Stopping || !m_snapshot.delayed)
		return false;
	m_command.store(Command::Force);
	return true;
}

Snapshot ServerSupervisor::snapshot() const
{
	std::lock_guard lock(m_mutex);
	return m_snapshot;
}

std::optional<Snapshot> ServerSupervisor::snapshotIfChanged(uint64_t &revision) const
{
	if (m_revision.load(std::memory_order_acquire) == revision)
		return std::nullopt;
	std::lock_guard lock(m_mutex);
	Snapshot next = m_snapshot;
	revision = m_revision.load(std::memory_order_relaxed);
	return next;
}

void ServerSupervisor::publish(const Snapshot &next)
{
	std::lock_guard lock(m_mutex);
	m_snapshot = next;
	m_revision.fetch_add(1, std::memory_order_release);
}

void ServerSupervisor::run(std::stop_token lifetime, const Options &options, const fs::path &executable,
	const fs::path &savesRoot, const std::optional<OwnerProfile> &owner)
{
	if (lifetime.stop_requested())
		return;
	Snapshot next = snapshot();
	try
	{
		if (!fs::is_regular_file(executable))
			throw std::runtime_error("Dedicated server executable is missing. Place ft_vox_server next to the game.");
		std::error_code worldError;
		const auto worldType = fs::symlink_status(savesRoot / options.world, worldError).type();
		if (worldType != fs::file_type::not_found && (worldError || worldType != fs::file_type::directory))
			throw std::runtime_error("The hosted world path must be a local directory under server-saves.");
		std::optional<localcontrol::BootstrapFrame> bootstrap;
		if (owner)
		{
			const auto control = localcontrol::loadOrCreateControlKey(owner->controlFile);
			if (!control || !localcontrol::validBootstrap({owner->player, *control}))
				throw std::runtime_error("Could not read or create the owner credential. Use a profile folder that supports private file permissions.");
			bootstrap = localcontrol::encodeBootstrap({owner->player, *control});
		}
		ChildProcess child(fs::absolute(executable), launchArguments(options, savesRoot, next.address, bootstrap.has_value()));
		if (bootstrap && !child.writeControl(*bootstrap))
		{
			// Let the child report its own startup error and reap it normally.
			child.closeControl();
		}
		bootstrap.reset();
		ServerOutput output;
		const auto started = Clock::now();
		std::optional<Clock::time_point> stopping, ownerGone;
		bool forced = false;
		for (;;)
		{
			const auto now = Clock::now();
			const auto command = m_command.load();
			if (lifetime.stop_requested() && !ownerGone)
				ownerGone = now;
			if ((command != Command::None || ownerGone) && !stopping)
			{
				child.closeControl();
				stopping = now;
				next.state = State::Stopping;
				next.message = "Stopping the server and committing the world.";
				next.delayed = false;
				publish(next);
			}
			bool changed = output.drain(child, next, options.port, 4);
			if (!stopping && next.state == State::Stopping)
			{
				stopping = now;
				child.closeControl();
			}
			if (const auto exit = child.pollExit())
			{
				output.drain(child, next, options.port, 64);
				next.checkpoint = output.committed ? output.checkpoint : next.checkpoint;
				next.state = !forced && output.committed && *exit == 0 ? State::Stopped : State::Failed;
				next.delayed = false;
				if (next.state == State::Stopped)
					next.message = "Server stopped. World checkpoint committed.";
				else if (forced)
					next.message = "Server was force stopped. Recent changes may not have been saved.";
				else if (!output.error.empty())
					next.message = std::move(output.error);
				else
					next.message = "Server exited without confirming a saved checkpoint (exit " +
						std::to_string(*exit) + ").";
				break;
			}
			if (command == Command::Force && stopping && !forced)
			{
				forced = child.forceStop();
				if (!forced)
					m_command.store(Command::Stop);
				next.message = forced ? "Waiting for the force-stopped server to exit." :
					"Could not force stop the child. Waiting for it to exit.";
				changed = true;
			}
			if (!next.delayed && ((next.state == State::Starting && now - started >= m_notice) ||
				(stopping && now - *stopping >= m_notice)))
			{
				next.delayed = true;
				next.message = stopping ? "Stopping is taking longer than expected. The server may still be saving." :
					"Startup is taking longer than expected. Check the server log or stop the launch.";
				changed = true;
			}
			if (changed)
				publish(next);
			if (ownerGone && now - *ownerGone >= 3s)
				return; // An unresponsive child keeps its save lock. Never force it here.
			std::this_thread::sleep_for(15ms);
		}
	}
	catch (const std::exception &error)
	{
		next.state = State::Failed;
		next.message = error.what();
	}
	publish(next);
}
}
