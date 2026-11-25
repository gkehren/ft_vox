#include <Hosting/ChildProcess.hpp>
#include <Hosting/ServerSupervisor.hpp>
#include <Network/ClientTransport.hpp>
#include <Server/LauncherControl.hpp>
#include <Server/MonitorWriter.hpp>
#include <World/WorldCheckpoint.hpp>

#include "TestMain.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <stdexcept>

namespace
{
namespace fs = std::filesystem;
using namespace std::chrono_literals;
using hosting::Options;
using hosting::ServerSupervisor;
using hosting::Snapshot;
using hosting::State;

Snapshot await(const ServerSupervisor &supervisor, const auto &done)
{
	const auto deadline = std::chrono::steady_clock::now() + 15s;
	Snapshot state;
	do
	{
		state = supervisor.snapshot();
		if (done(state))
			return state;
		std::this_thread::sleep_for(10ms);
	} while (std::chrono::steady_clock::now() < deadline);
	throw std::runtime_error("server did not reach expected state: " + state.message);
}

Snapshot ready(const ServerSupervisor &supervisor)
{
	const auto state = await(supervisor, [](const Snapshot &s) { return s.state == State::Ready || !s.active(); });
	if (state.state != State::Ready)
		throw std::runtime_error(state.message);
	return state;
}

Snapshot finished(const ServerSupervisor &supervisor)
{
	return await(supervisor, [](const Snapshot &s) { return !s.active(); });
}

Options options(std::string world)
{
	Options result;
	result.world = std::move(world);
	result.port = 0;
	result.playerSlots = 1;
	result.interestRadius = 1;
	result.workers = 0;
	return result;
}

void checkValidation()
{
	auto settings = options("World one");
	CHECK(hosting::validate(settings).empty(), "ephemeral loopback defaults to a valid launch");
	settings.lan = true;
	for (const char *address : {"10.0.0.2", "172.31.1.2", "192.168.0.2", "100.75.0.2"})
	{
		settings.address = address;
		CHECK(hosting::validate(settings).empty(), "private address syntax passes before the UDP bind");
	}
	for (const char *address : {"0.0.0.0", "127.0.0.1", "203.0.113.2", "8.8.8.8", "172.32.0.1",
		"localhost", "10.0.0.2 --port 1"})
	{
		settings.address = address;
		CHECK(!hosting::validate(settings).empty(), "LAN bind requires a concrete private numeric address");
	}
	settings.lan = false;
	for (const char *world : {"../world", "C:/world", "--world", "CON", "trailing ", "world & other"})
	{
		settings.world = world;
		CHECK(!hosting::validate(settings).empty(), "unsafe world names are refused");
	}
	settings = options("Options");
	settings.port = 65536;
	CHECK(!hosting::validate(settings).empty(), "invalid port is refused");
	settings.port = 0;
	settings.playerSlots = 256;
	CHECK(!hosting::validate(settings).empty(), "player slots fit the server cap");
	settings.playerSlots = 1;
	settings.interestRadius = 9;
	CHECK(!hosting::validate(settings).empty(), "interest radius stays bounded");
}

void checkRealServer(const fs::path &exe, const fs::path &root)
{
	ServerSupervisor supervisor;
	auto settings = options("World with spaces");
	CHECK(supervisor.start(settings, exe, root), "launch accepted");
	const auto initial = ready(supervisor);
	CHECK(initial.address == "127.0.0.1" && initial.port > 0 && initial.checkpoint > 0,
		"ready publishes the bound port and initial checkpoint");
	CHECK(fs::is_regular_file(root / settings.world / "world.meta"), "world and save root arguments stay intact");
	const auto worlds = hosting::listWorlds(root);
	CHECK(std::ranges::find(worlds, settings.world) != worlds.end(), "hosted world can be selected again");
	CHECK(!supervisor.start(options("Another"), exe, root), "one supervisor owns at most one child");
	CHECK(!supervisor.forceStop(), "ready server cannot be force stopped directly");
	const auto empty = await(supervisor, [](const Snapshot &s) { return s.status != nullptr; });
	CHECK(empty.status->transport.count == 0 && empty.status->transport.capacity == 1 &&
		empty.status->checkpoint == initial.checkpoint && empty.status->ticks > 0,
		"empty roster and real owner values are published by the child");

	net::udp::ClientTransport client(net::udp::parseAddress(initial.address, initial.port));
	using Clock = net::udp::ClientTransport::Clock;
	client.connect(Clock::now());
	const auto deadline = Clock::now() + 5s;
	while (client.status() == net::udp::ClientStatus::Connecting && Clock::now() < deadline)
	{
		client.poll(Clock::now());
		std::this_thread::sleep_for(10ms);
	}
	CHECK(client.status() == net::udp::ClientStatus::Connected, "real client joins the published UDP endpoint");
	const auto connected = await(supervisor, [&](const Snapshot &s) {
		client.poll(Clock::now());
		return s.status && s.status->transport.count == 1 &&
			s.status->transport.roster().front().state == monitoring::SessionState::Connected;
	});
	const auto player = connected.status->transport.roster().front().id;
	CHECK(client.welcome() && client.welcome()->player == player &&
		connected.status->registeredProfiles == 1 && connected.status->offlineProfiles == 0 &&
		connected.status->transport.receivedBytes > 0 && connected.status->transport.sentBytes > 0 &&
		connected.status->ticks > empty.status->ticks && connected.status->uptimeMs >= 900,
		"live monitoring carries real sessions, profile counts, traffic and monotonic timing");
	client.disconnect(Clock::now());
	const auto left = await(supervisor, [&](const Snapshot &s) {
		client.poll(Clock::now());
		return s.status && s.status->transport.count == 0 && client.status() == net::udp::ClientStatus::Disconnected;
	});
	CHECK(left.status->offlineProfiles == 1 && left.status->registeredProfiles == 1,
		"a departed registered profile is counted offline and omitted from the roster");
	client.connect(Clock::now());
	const auto rejoined = await(supervisor, [&](const Snapshot &s) {
		client.poll(Clock::now());
		return s.status && s.status->transport.count == 1 &&
			s.status->transport.roster().front().state == monitoring::SessionState::Connected;
	});
	CHECK(rejoined.status->transport.roster().front().id == player && rejoined.status->offlineProfiles == 0,
		"reconnect preserves the registered display identity without a ghost row");

	ServerSupervisor duplicate;
	CHECK(duplicate.start(settings, exe, root), "duplicate world request launches a separate process");
	auto failure = finished(duplicate);
	CHECK(failure.state == State::Failed && failure.message.find("lock") != std::string::npos &&
		supervisor.snapshot().state == State::Ready, "existing save lock is visible and the first server stays ready");

	supervisor.stop();
	auto stop = finished(supervisor);
	CHECK(stop.state == State::Stopped && !stop.status && stop.checkpoint > initial.checkpoint &&
		std::ranges::any_of(stop.logs, [](const auto &line) { return line.find("reason=launcher saved=1") != std::string::npos; }),
		"EOF is graceful and a committed checkpoint is required for completion");
	settings.workers = 2;
	settings.streaming = hosting::StreamingPreset::Low;
	CHECK(supervisor.start(settings, exe, root), "reopen the world with new settings");
	CHECK(ready(supervisor).checkpoint >= stop.checkpoint, "restart observes the committed checkpoint");
	supervisor.stop();
	CHECK(finished(supervisor).state == State::Stopped, "reopened server commits");

	net::udp::Socket occupied(net::udp::parseAddress("127.0.0.1", 0));
	auto busy = options("Occupied port");
	busy.port = occupied.address().port;
	supervisor.start(busy, exe, root);
	failure = finished(supervisor);
	CHECK(failure.state == State::Failed && failure.port == 0 && failure.message.find("bind") != std::string::npos,
		"an occupied port never becomes joinable and the bind error is visible");

	fs::create_directories(root / "Invalid save");
	std::ofstream(root / "Invalid save" / "world.meta") << "invalid metadata";
	supervisor.start(options("Invalid save"), exe, root);
	failure = finished(supervisor);
	CHECK(failure.state == State::Failed && failure.message.find("world_open") != std::string::npos,
		"invalid saved world produces an actionable startup error");
	supervisor.start(options("Missing binary"), root / "missing-server", root);
	failure = finished(supervisor);
	CHECK(failure.state == State::Failed && failure.message.find("executable is missing") != std::string::npos,
		"missing executable is reported");

	uint64_t beforeExit{};
	{
		ServerSupervisor owner;
		owner.start(settings, exe, root);
		beforeExit = ready(owner).checkpoint;
	}
	supervisor.start(settings, exe, root);
	CHECK(ready(supervisor).checkpoint > beforeExit, "teardown requests a save and releases the world lock");
	supervisor.stop();
	CHECK(finished(supervisor).state == State::Stopped, "supervisor recovers after startup failures");
}

int fixture(std::string_view world, const fs::path &root, std::optional<uintptr_t> inheritedMonitor)
{
	if (!server::hasLauncherPipe())
		return 8;
	if (world == "slow")
	{
		std::cout << "SERVER status=ready mode=live transport=udp listen_address=127.0.0.1 port=62000 "
			<< std::string(16000, 'z') << '\n';
		for (int i = 0; i < 200; ++i)
			std::cout << "test log line " << i << '\n';
		std::cout << std::flush;
		std::this_thread::sleep_for(600ms);
	}
	std::cout << "SERVER status=ready mode=live transport=udp listen_address=127.0.0.1 port=62001 checkpoint_generation=1"
		<< std::endl;
	server::MonitorWriter monitor(world.starts_with("monitor ") ? inheritedMonitor : std::nullopt);
	if (monitor)
	{
		monitoring::Status sample;
		sample.sampledMs = monitoring::monotonicMs();
		sample.checkpoint = 1;
		sample.transport.capacity = 1;
		sample.transport.count = 1;
		sample.transport.players[0] = {{2}, monitoring::SessionState::Connected};
		monitor.offer(sample);
		if (world == "monitor pressure")
		{
			for (unsigned i = 0; i < 130; ++i)
			{
				sample.sampledMs = monitoring::monotonicMs();
				monitor.offer(sample);
				std::this_thread::sleep_for(20ms);
			}
			std::cout << "fixture completed under pressure" << std::endl;
		}
		else
		{
			// Monitor offers are opportunistic. Keep publishing until the
			// parent has observed a sample, then let freshness expire.
			while (server::pollLauncherPipe() == server::LauncherInput::Open && !fs::exists(root / "stall-monitor"))
			{
				sample.sampledMs = monitoring::monotonicMs();
				monitor.offer(sample);
				std::this_thread::sleep_for(20ms);
			}
			while (server::pollLauncherPipe() == server::LauncherInput::Open && !fs::exists(root / "resume-monitor"))
				std::this_thread::sleep_for(10ms);
			if (world == "monitor crash")
				std::_Exit(42);
			sample.transport.count = 0;
			while (server::pollLauncherPipe() == server::LauncherInput::Open)
			{
				sample.sampledMs = monitoring::monotonicMs();
				monitor.offer(sample);
				std::this_thread::sleep_for(20ms);
			}
		}
	}
	if (world == "log updates")
	{
		while (server::pollLauncherPipe() == server::LauncherInput::Open && !fs::is_regular_file(root / "emit-log"))
			std::this_thread::sleep_for(10ms);
		std::cout << "fixture log while ready" << std::endl;
	}
	if (world == "crash")
	{
		std::this_thread::sleep_for(150ms);
		std::_Exit(42);
	}
	while (world == "hang" || server::pollLauncherPipe() == server::LauncherInput::Open)
		std::this_thread::sleep_for(10ms);
	std::cout << "SERVER status=stopping reason=launcher" << std::endl;
	if (world == "save failure")
	{
		std::cerr << "SERVER error=persistence detail=injected checkpoint failure\n";
		std::cout << "SERVER status=stopped reason=launcher saved=0 exit_code=5 checkpoint_generation=1" << std::endl;
		return 5;
	}
	std::cout << "SERVER status=stopped reason=launcher saved=1 exit_code=0 checkpoint_generation=2" << std::endl;
	return 0;
}

void checkMonitorCodec()
{
	monitoring::Status status;
	status.transport.capacity = static_cast<uint16_t>(monitoring::kMaxPlayers);
	status.transport.count = status.transport.capacity;
	for (uint32_t i = 0; i < status.transport.count; ++i)
		status.transport.players[i] = {{i + 2}, monitoring::SessionState::Connected};
	const auto full = monitoring::decode(monitoring::encode(status));
	CHECK(full && full->transport.roster().size() == monitoring::kMaxPlayers, "maximum roster fits one bounded record");
	status.transport.players[1] = status.transport.players[0];
	CHECK(!monitoring::decode(monitoring::encode(status)), "duplicate player identities are rejected");
	status.transport.count = 1;
	auto frame = monitoring::encode(status);
	frame[4] = 2;
	CHECK(!monitoring::decode(frame), "unexpected monitor version is rejected");
	frame = monitoring::encode(status);
	frame.back() = 1;
	CHECK(!monitoring::decode(frame), "unused roster padding must be empty");
	CHECK(!monitoring::fresh(100, 99) && monitoring::fresh(100, 3100) && !monitoring::fresh(100, 3101),
		"delayed frames and future capture times cannot refresh a stale roster");
}

void checkMonitoringLifecycle(const fs::path &fixturePath, const fs::path &root)
{
	ServerSupervisor supervisor;
	supervisor.start(options("monitor stale"), fixturePath, root);
	const auto first = await(supervisor, [](const Snapshot &s) { return s.status != nullptr; });
	CHECK(first.status->transport.count == 1, "fixture published a player before stalling");
	std::ofstream(root / "stall-monitor").put('\n');
	await(supervisor, [](const Snapshot &s) { return s.state == State::Ready && !s.status; });
	std::ofstream(root / "resume-monitor").put('\n');
	const auto resumed = await(supervisor, [](const Snapshot &s) { return s.status != nullptr; });
	CHECK(resumed.status->transport.count == 0, "fresh empty sample replaces expired roster after resuming");
	supervisor.stop();
	finished(supervisor);

	fs::remove(root / "resume-monitor");
	fs::remove(root / "stall-monitor");
	supervisor.start(options("monitor crash"), fixturePath, root);
	await(supervisor, [](const Snapshot &s) { return s.status != nullptr; });
	std::ofstream(root / "stall-monitor").put('\n');
	std::ofstream(root / "resume-monitor").put('\n');
	const auto crashed = finished(supervisor);
	CHECK(crashed.state == State::Failed && !crashed.status, "process crash clears the roster immediately");
}

void checkMonitorBackpressure(const fs::path &fixturePath, const fs::path &root)
{
	hosting::ChildProcess child(fs::absolute(fixturePath),
		{"--world", "monitor pressure", "--saves-root", root}, true);
	const auto deadline = std::chrono::steady_clock::now() + 12s;
	std::string output;
	bool done = false;
	// Intentionally leave the monitoring pipe unread for the whole run.
	while (std::chrono::steady_clock::now() < deadline)
	{
		std::array<char, 1024> buffer{};
		output.append(buffer.data(), child.readOutput(buffer));
		if (!done && output.find("fixture completed under pressure") != std::string::npos)
		{
			done = true;
			child.closeControl();
		}
		if (const auto exit = child.pollExit())
		{
			CHECK(done && *exit == 0, "full monitor pipe cannot delay the publisher or prevent clean exit");
			return;
		}
		std::this_thread::sleep_for(10ms);
	}
	child.forceStop(); // Owned fixture only.
	CHECK(false, "fixture stalled behind its unread monitor pipe");
}

void checkSnapshotRevisions(const fs::path &fixturePath, const fs::path &root)
{
	ServerSupervisor supervisor;
	uint64_t revision{};
	CHECK(!supervisor.snapshotIfChanged(revision), "idle frames have no snapshot to copy");
	CHECK(!supervisor.start(options("../invalid"), fixturePath, root), "invalid configuration fails before launch");
	const auto failure = supervisor.snapshotIfChanged(revision);
	CHECK(failure && failure->state == State::Failed, "validation errors publish a snapshot");

	supervisor.start(options("log updates"), fixturePath, root);
	ready(supervisor);
	const auto running = supervisor.snapshotIfChanged(revision);
	CHECK(running && running->state == State::Ready && !running->logs.empty(), "readiness publishes logs and state");
	bool stable = true;
	for (int i = 0; i < 300; ++i)
		stable = stable && !supervisor.snapshotIfChanged(revision);
	CHECK(stable, "stable frames receive no snapshot or log copy");

	std::ofstream(root / "emit-log").put('\n');
	await(supervisor, [](const Snapshot &s) { return !s.logs.empty() && s.logs.back() == "fixture log while ready"; });
	const auto logged = supervisor.snapshotIfChanged(revision);
	CHECK(logged && logged->state == State::Ready && logged->logs.back() == "fixture log while ready",
		"new logs publish a revision even without a state transition");
	CHECK(!supervisor.snapshotIfChanged(revision), "a revision is consumed once");
	supervisor.stop();
	finished(supervisor);
	const auto stopped = supervisor.snapshotIfChanged(revision);
	CHECK(stopped && stopped->state == State::Stopped && stopped->checkpoint == 2,
		"stopping publishes the final checkpoint");
}

void checkSupervision(const fs::path &fixturePath, const fs::path &root)
{
	ServerSupervisor supervisor(100ms);
	supervisor.start(options("slow"), fixturePath, root);
	const auto slow = await(supervisor, [](const Snapshot &s) { return s.state == State::Starting && s.delayed; });
	CHECK(slow.port == 0 && slow.logs.size() <= hosting::kMaxLogLines, "slow or truncated readiness never offers a join");
	CHECK(ready(supervisor).port == 62001, "a subsequent valid readiness record is accepted");
	supervisor.stop();
	CHECK(finished(supervisor).state == State::Stopped, "slow launch still supports graceful stop");

	supervisor.start(options("crash"), fixturePath, root);
	auto state = finished(supervisor);
	CHECK(state.state == State::Failed && state.message.find("exit 42") != std::string::npos,
		"process exit without a terminal checkpoint is reported as failure");

	supervisor.start(options("save failure"), fixturePath, root);
	ready(supervisor);
	supervisor.stop();
	state = finished(supervisor);
	CHECK(state.state == State::Failed && state.message.find("injected checkpoint failure") != std::string::npos,
		"a failed checkpoint stays visible");

	supervisor.start(options("hang"), fixturePath, root);
	ready(supervisor);
	ServerSupervisor neighbor;
	neighbor.start(options("neighbor"), fixturePath, root);
	ready(neighbor);
	supervisor.stop();
	await(supervisor, [](const Snapshot &s) { return s.state == State::Stopping && s.delayed; });
	CHECK(supervisor.forceStop(), "unresponsive stop allows explicit escalation");
	state = finished(supervisor);
	CHECK(state.state == State::Failed && state.message.find("force stopped") != std::string::npos,
		"forced termination is distinguished from a committed checkpoint");
	CHECK(neighbor.snapshot().state == State::Ready, "escalation affects only the owned child");
	neighbor.stop();
	CHECK(finished(neighbor).state == State::Stopped, "unrelated child remains independently stoppable");
}

void checkLauncherLoss(const fs::path &fixturePath, const fs::path &exe, const fs::path &root)
{
	hosting::ChildProcess launcher(fs::absolute(fixturePath), {"--abandon", exe, root});
	const auto deadline = std::chrono::steady_clock::now() + 15s;
	auto exit = launcher.pollExit();
	while (!exit && std::chrono::steady_clock::now() < deadline)
	{
		std::this_thread::sleep_for(10ms);
		exit = launcher.pollExit();
	}
	if (exit != 0)
		throw std::runtime_error("launcher exit fixture failed");
	std::array<char, 128> buffer{};
	const auto size = launcher.readOutput(buffer);
	const uint64_t initial = std::stoull(std::string(buffer.data(), size));
	const auto path = worldsave::checkpointManifestPath(root / "Abandoned launcher");
	while (std::chrono::steady_clock::now() < deadline)
	{
		std::vector<uint8_t> bytes;
		worldsave::CheckpointManifest manifest;
		if (worldsave::readFileInto(path, bytes) == worldsave::SaveStatus::Ok &&
			worldsave::decodeCheckpointManifest(bytes.data(), bytes.size(), manifest) == worldsave::SaveStatus::Ok &&
			manifest.committedGeneration > initial)
		{
			CHECK(initial > 0, "launcher loss commits a new checkpoint even with the log reader gone");
			return;
		}
		std::this_thread::sleep_for(10ms);
	}
	CHECK(false, "server did not commit after abrupt launcher exit");
}
}

int main(int argc, char **argv)
{
	if (argc >= 5 && std::string_view(argv[1]) == "--world" && std::string_view(argv[3]) == "--saves-root")
	{
		std::optional<uintptr_t> monitor;
		if (argc >= 7 && std::string_view(argv[argc - 2]) == "--launcher-monitor")
			monitor = static_cast<uintptr_t>(std::stoull(argv[argc - 1]));
		return fixture(argv[2], argv[4], monitor);
	}
	if (argc == 4 && std::string_view(argv[1]) == "--abandon")
	{
		ServerSupervisor supervisor;
		supervisor.start(options("Abandoned launcher"), argv[2], argv[3]);
		std::cout << ready(supervisor).checkpoint << std::endl;
		std::_Exit(0); // Bypass supervisor teardown and lose both pipe ends.
	}
	if (argc != 2)
		return 2;
	const auto root = fs::temp_directory_path() / ("ft vox server & supervisor " + std::to_string(net::udp::randomId()));
	try
	{
		checkValidation();
		checkMonitorCodec();
		checkRealServer(fs::path(argv[1]), root);
		checkMonitoringLifecycle(fs::path(argv[0]), root);
		checkMonitorBackpressure(fs::path(argv[0]), root);
		checkSnapshotRevisions(fs::path(argv[0]), root);
		checkSupervision(fs::path(argv[0]), root);
		checkLauncherLoss(fs::path(argv[0]), fs::path(argv[1]), root);
	}
	catch (const std::exception &error)
	{
		CHECK(false, error.what());
	}
	std::error_code ignored;
	fs::remove_all(root, ignored);
	return testmain::exitCode();
}
