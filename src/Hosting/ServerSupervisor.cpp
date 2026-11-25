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
		"--eviction-budget", std::to_string(streams.evictions), "--launcher-control", bootstrap ? "admin" : "stdin"};
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
		{
			committed = field(record, "saved=") == "1" && field(record, "exit_code=") == "0" &&
				readNumber(field(record, "checkpoint_generation="), checkpoint) && checkpoint > 0;
			controlStop = field(record, "reason=") == "control";
		}
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
	bool truncated{}, committed{}, controlStop{};
	uint64_t checkpoint{};
};

struct MonitorInput
{
	std::optional<localcontrol::Reply> reply;

	bool drain(ChildProcess &child, Snapshot &snapshot, int capacity)
	{
		bool changed = false;

		std::optional<monitoring::Status> latest;
		for (unsigned batch = 0; batch < 24; ++batch)
		{
			const auto read = child.readMonitoring({reinterpret_cast<char *>(frame.data()) + used, needed - used});
			if (!read)
				break;
			used += read;
			if (used != needed)
				continue;
			if (needed == localcontrol::kReplyHeaderSize)
			{
				admin = std::equal(localcontrol::kReplyMagic.begin(), localcontrol::kReplyMagic.end(), frame.begin());
				if (admin)
				{
					const auto size = localcontrol::replySize(std::span{frame}.first(used));
					if (!size)
					{
						changed = !std::exchange(snapshot.adminChannelFailed, true);
						break;
					}
					needed = *size;
				}
				else
					needed = monitoring::kFrameSize;
				continue;
			}
			if (admin)
			{
				if (auto decoded = localcontrol::decodeReply(std::span{frame}.first(used)))
					reply = std::move(decoded);
			}
			else
			{
				monitoring::Frame sample;
				std::copy_n(frame.begin(), sample.size(), sample.begin());
				auto decoded = monitoring::decode(sample);
				if (decoded && decoded->transport.capacity == capacity && decoded->sampledMs > lastSample &&
					monitoring::fresh(decoded->sampledMs, monitoring::monotonicMs()))
				{
					lastSample = decoded->sampledMs;
					latest = std::move(decoded);
				}
			}
			used = 0;
			needed = localcontrol::kReplyHeaderSize;
		}
		if (snapshot.state != State::Ready)
			return static_cast<bool>(std::exchange(snapshot.status, {})) || changed;
		if (latest)
		{
			snapshot.checkpoint = latest->checkpoint;
			snapshot.status = std::make_shared<const monitoring::Status>(std::move(*latest));
			return true;
		}
		if (snapshot.status && !monitoring::fresh(snapshot.status->sampledMs, monitoring::monotonicMs()))
		{
			snapshot.status.reset();
			return true;
		}
		return changed;
	}

	std::array<uint8_t, localcontrol::kMaxReplySize> frame{};
	size_t used{}, needed{localcontrol::kReplyHeaderSize};
	bool admin{};
	uint64_t lastSample{};
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
	m_adminRequest.reset();
	m_adminInFlight = false;
	m_snapshot.launch = m_revision.load(std::memory_order_relaxed) + 1;
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

bool ServerSupervisor::administer(uint64_t launch, AdminAction action)
{
	std::lock_guard lock(m_mutex);
	if (m_snapshot.launch != launch || m_snapshot.state != State::Ready || !m_snapshot.administration ||
		m_snapshot.adminChannelFailed || !localcontrol::permitted(m_snapshot.administration->role, action.operation) ||
		(localcontrol::targeted(action.operation) ? !localcontrol::validActor(action.target) : action.target.valid()) ||
		(action.delegate && action.operation != localcontrol::Operation::GrantAdmin) ||
		m_adminRequest || m_adminInFlight)
		return false;
	m_adminRequest = std::move(action);
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
		const auto worldDir = savesRoot / options.world;
		const auto worldType = fs::symlink_status(worldDir, worldError).type();
		if (worldType != fs::file_type::not_found && (worldError || worldType != fs::file_type::directory))
			throw std::runtime_error("The hosted world path must be a local directory under server-saves.");
		std::optional<localcontrol::BootstrapFrame> bootstrap;
		if (owner)
		{
			const bool fresh = worldType == fs::file_type::not_found || localcontrol::isFreshWorldDirectory(worldDir);
			const auto control = fresh ? localcontrol::loadOrCreateControlKey(owner->controlFile) :
				localcontrol::readControlKey(owner->controlFile);
			localcontrol::Bootstrap request{owner->player, {}};
			if (control && localcontrol::validBootstrap({owner->player, *control}))
				request.control = *control;
			else if (fresh)
				throw std::runtime_error("Could not read or create the owner credential. Use a profile folder that supports private file permissions.");
			bootstrap = localcontrol::encodeBootstrap(request);
		}
		ChildProcess child(fs::absolute(executable), launchArguments(options, savesRoot, next.address, bootstrap.has_value()), true);
		if (bootstrap && !child.writeControl(*bootstrap))
		{
			// Let the child report its own startup error and reap it normally.
			child.closeControl();
		}
		bootstrap.reset();
		ServerOutput output;
		MonitorInput monitor;
		const auto started = Clock::now();
		std::optional<Clock::time_point> stopping, ownerGone;
		std::optional<Clock::time_point> adminDeadline, adminHelloDeadline;
		uint64_t adminSequence = 0;
		bool forced = false;
		const auto completeAdmin = [&](localcontrol::Result result) {
			next.adminResult = result;
			next.adminPending = false;
			adminDeadline.reset();
			std::lock_guard lock(m_mutex);
			m_adminInFlight = false;
		};
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
				next.status.reset();
				next.message = "Stopping the server and committing the world.";
				next.delayed = false;
				publish(next);
			}
			bool changed = output.drain(child, next, options.port, 4);
			changed |= monitor.drain(child, next, options.playerSlots);
			if (auto reply = std::exchange(monitor.reply, std::nullopt); owner && reply)
			{
				if (!next.administration && !reply->sequence && reply->result == localcontrol::Result::Ready)
				{
					next.administration = std::make_shared<const localcontrol::Reply>(std::move(*reply));
					changed = true;
				}
				else if (adminDeadline && next.administration && reply->world == next.administration->world &&
					reply->epoch == next.administration->epoch &&
					(reply->sequence == adminSequence || (!reply->sequence && reply->result == localcontrol::Result::Malformed)))
				{
					completeAdmin(reply->result);
					next.administration = std::make_shared<const localcontrol::Reply>(std::move(*reply));
					changed = true;
				}
			}
			if (adminDeadline && now >= *adminDeadline)
			{
				next.adminChannelFailed = true;
				completeAdmin(localcontrol::Result::ChannelError);
				changed = true;
			}
			if (owner && next.state == State::Ready && !next.administration && !next.adminChannelFailed)
			{
				if (!adminHelloDeadline)
					adminHelloDeadline = now + m_notice;
				else if (now >= *adminHelloDeadline)
				{
					next.adminChannelFailed = true;
					changed = true;
				}
			}
			std::optional<AdminAction> action;
			if (next.state == State::Ready && !next.adminChannelFailed && !adminDeadline && next.administration &&
				command == Command::None && !ownerGone)
			{
				std::lock_guard lock(m_mutex);
				action = std::exchange(m_adminRequest, std::nullopt);
				m_adminInFlight |= action.has_value();
			}
			if (action)
			{
				next.adminOperation = action->operation;
				next.adminResult.reset();
				localcontrol::Request request{next.administration->world, next.administration->epoch, adminSequence + 1,
					action->operation, action->target, std::nullopt};
				if (action->delegate)
				{
					if (const auto key = localcontrol::loadOrCreateControlKey(action->delegate->controlFile);
						key && localcontrol::validBootstrap({action->delegate->player, *key}))
						request.delegation = localcontrol::Bootstrap{action->delegate->player, *key};
					else
						completeAdmin(localcontrol::Result::InvalidCredential);
				}
				if (!next.adminResult)
				{
					if (request.sequence && child.writeControl(localcontrol::encodeRequest(request)))
					{
						adminSequence = request.sequence;
						adminDeadline = now + m_notice;
						next.adminPending = true;
					}
					else
					{
						next.adminChannelFailed = true;
						completeAdmin(localcontrol::Result::ChannelError);
					}
				}
				changed = true;
			}
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
				if (next.adminPending)
					completeAdmin(next.state == State::Stopped && output.controlStop && next.adminOperation == localcontrol::Operation::Stop ?
						localcontrol::Result::Applied : localcontrol::Result::ChannelError);
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
		if (next.adminPending)
			next.adminResult = localcontrol::Result::ChannelError;
	}
	next.adminPending = false;
	next.status.reset();
	next.administration.reset();
	publish(next);
}
}
