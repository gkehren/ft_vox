#include <Server/ServerHost.hpp>
#include <Server/TickSchedule.hpp>
#include <Server/LauncherControl.hpp>
#include <Server/RolesCommand.hpp>

#include <atomic>
#include <charconv>
#include <csignal>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace
{

enum class ExitCode { Ok = 0, Usage = 2, WorldOpen = 3, Run = 4, Save = 5 };

struct Options
{
	server::HostConfig host;
	std::string listenAddress{"127.0.0.1"};
	unsigned port{7777};
	unsigned playerCap{16};
	unsigned ticks{0}; // live mode when omitted
	unsigned maxCatchUp{4};
	unsigned metricsEvery{600};
	std::filesystem::path stopFile;
	bool launcherControl{};
	bool launcherBootstrap{};
};

static_assert(std::atomic<bool>::is_always_lock_free);
std::atomic<bool> stopRequested{false};

void requestStop(int)
{
	stopRequested.store(true, std::memory_order_relaxed);
}

#ifdef _WIN32
BOOL WINAPI consoleStop(DWORD type)
{
	if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT)
	{
		requestStop(0);
		return TRUE;
	}
	return FALSE;
}
#endif

bool installStopHandlers(bool launched)
{
#ifdef _WIN32
	return (launched || SetConsoleCtrlHandler(consoleStop, TRUE) != FALSE) &&
		std::signal(SIGTERM, requestStop) != SIG_ERR;
#else
	// On unexpected launcher exit the log reader can disappear before
	// the server observes control EOF and commits the final checkpoint.
	if (launched && std::signal(SIGPIPE, SIG_IGN) == SIG_ERR)
		return false;
	return std::signal(SIGINT, requestStop) != SIG_ERR && std::signal(SIGTERM, requestStop) != SIG_ERR;
#endif
}

void printUsage()
{
	std::cout <<
		"Usage: ft_vox_server --world NAME [options]\n"
		"  --saves-root PATH         save root (default saves)\n"
		"  --seed N                  positive seed (default stored seed or 42 for a new world)\n"
		"  --listen-address ADDRESS  live UDP bind IPv4 address (default 127.0.0.1)\n"
		"  --port N                  live UDP bind port, 0..65535, 0 is ephemeral (default 7777)\n"
		"  --player-cap N            session limit, 1..256 including local anchor (default 16)\n"
		"  --ticks N                 run exactly N ticks, 1..1000000, without wall-clock pacing\n"
		"  --max-catch-up N          live ticks per scheduling batch, 1..8 (default 4)\n"
		"  --workers N               terrain workers, 0..64 (default 2; 0 is synchronous)\n"
		"  --interest-radius N       per-player radius in chunks, 0..8 (default 3)\n"
		"  --load-budget N           chunk records per tick, 0..128 (default 32)\n"
		"  --generation-budget N     terrain starts per tick, 0..64 (default 8)\n"
		"  --eviction-budget N       chunk evictions per tick, 0..64 (default 16)\n"
		"  --metrics-every N         completed ticks between log records, 0..1000000 (default 600; 0 disables)\n"
		"  --stop-file PATH          save and stop when PATH exists\n"
		"  --launcher-control MODE   inherited stdin: stdin for EOF only, bootstrap for owner startup\n"
		"  --help                    show this help\n";
	server::printRolesUsage();
}

bool parseOptions(int argc, char **argv, Options &options, std::string &error)
{
	for (int i = 1; i < argc; ++i)
	{
		const std::string_view option = argv[i];
		if (i + 1 == argc)
		{
			error = "missing value for " + std::string(option);
			return false;
		}
		const std::string_view value = argv[++i];
		if (value.empty() || value.starts_with("--"))
		{
			error = "missing value for " + std::string(option);
			return false;
		}

		const auto number = [&]<class T>(T &field, T min, T max) {
			const char *end = value.data() + value.size();
			const auto parsed = std::from_chars(value.data(), end, field);
			if (parsed.ec != std::errc{} || parsed.ptr != end || field < min || field > max)
				error = std::string(option) + " expects an integer in [" +
					std::to_string(min) + ", " + std::to_string(max) + "]";
		};
		if (option == "--world")
			options.host.world.worldName = value;
		else if (option == "--saves-root")
			options.host.world.savesRoot = value;
		else if (option == "--seed")
			number(options.host.world.seed, 1, std::numeric_limits<int>::max());
		else if (option == "--listen-address")
		{
			if (value.size() > 253 || value.find_first_of(" \t\r\n") != std::string_view::npos)
				error = "--listen-address expects a hostname or address without whitespace";
			else
				options.listenAddress = value;
		}
		else if (option == "--port")
			number(options.port, 0u, 65535u);
		else if (option == "--player-cap")
			number(options.playerCap, 1u, static_cast<unsigned>(sim::GameSimulation::kMaxPlayers));
		else if (option == "--ticks")
			number(options.ticks, 1u, 1000000u);
		else if (option == "--max-catch-up")
			number(options.maxCatchUp, 1u, 8u);
		else if (option == "--workers")
			number(options.host.workers, 0u, 64u);
		else if (option == "--interest-radius")
			number(options.host.interestRadiusChunks, 0, sim::kMaxPlayerInterestRadiusChunks);
		else if (option == "--load-budget")
			number(options.host.streaming.loads, 0u, PlayerStreamingBudget::kMaxLoads);
		else if (option == "--generation-budget")
			number(options.host.streaming.generations, 0u, PlayerStreamingBudget::kMaxGenerations);
		else if (option == "--eviction-budget")
			number(options.host.streaming.evictions, 0u, PlayerStreamingBudget::kMaxEvictions);
		else if (option == "--metrics-every")
			number(options.metricsEvery, 0u, 1000000u);
		else if (option == "--stop-file")
			options.stopFile = value;
		else if (option == "--launcher-control" && (value == "stdin" || value == "bootstrap"))
		{
			options.launcherControl = true;
			options.launcherBootstrap = value == "bootstrap";
		}
		else
			error = "unknown option " + std::string(option);
		if (!error.empty())
			return false;
	}
	if (options.host.world.worldName.empty())
		error = "--world is required";
	else if (options.launcherControl && options.ticks)
		error = "--launcher-control requires live mode";
	return error.empty();
}

void printMetrics(const server::HostMetrics &metrics, uint64_t droppedTicks, net::udp::WorldTraffic traffic = {})
{
	std::cout << " seed=" << metrics.seed << " ticks=" << metrics.ticks
			  << " dropped_ticks=" << droppedTicks << " world_epoch=" << metrics.worldEpoch
			  << " next_edit_id=" << metrics.nextEditId << " chunks=" << metrics.chunks
			  << " players=" << metrics.players << " mobs=" << metrics.mobs << " items=" << metrics.items
			  << " generation_in_flight=" << metrics.pendingGeneration
			  << " waiting_for_record=" << metrics.waitingForRecord
			  << " waiting_for_generation=" << metrics.waitingForGeneration
			  << " checkpoint_generation=" << metrics.checkpointGeneration
			  << " world_tx_bytes=" << traffic.bytes << " world_tx_frames=" << traffic.frames
			  << " world_retransmits=" << traffic.retransmits << " world_resyncs=" << traffic.resyncs << std::endl;
}

const char *runTicks(server::ServerHost &host, net::udp::ServerTransport *transport,
                     const Options &options, uint64_t &droppedTicks, std::string &error)
{
	const bool deterministic = options.ticks > 0;
	using server::TickSchedule;
	TickSchedule schedule(TickSchedule::Clock::now(), options.maxCatchUp);
	unsigned batchRemaining = 0;
	const auto timing = deterministic ? server::TerrainTiming::QuiescePerTick : server::TerrainTiming::Background;
	for (;;)
	{
		bool discardInput = false;
		if (stopRequested.load(std::memory_order_relaxed))
			return "signal";
		if (options.launcherControl)
		{
			switch (server::pollLauncherPipe())
			{
			case server::LauncherInput::Open: break;
			case server::LauncherInput::Closed: return "launcher";
			case server::LauncherInput::Invalid:
				error = "unexpected input on the private launcher control pipe";
				return "runtime_error";
			}
		}
		if (!options.stopFile.empty())
		{
			std::error_code statusError;
			if (std::filesystem::exists(options.stopFile, statusError))
				return "admin";
			if (statusError)
			{
				error = "could not check stop file: " + statusError.message();
				return "runtime_error";
			}
		}
		if (deterministic && host.ticks() == options.ticks)
			return "tick_limit";
		if (!deterministic)
		{
			if (batchRemaining == 0)
			{
				const auto batch = schedule.takeDue(TickSchedule::Clock::now());
				droppedTicks += batch.dropped;
				discardInput = batch.dropped != 0;
				batchRemaining = batch.run;
				if (batchRemaining == 0)
				{
					std::this_thread::sleep_until(schedule.nextDeadline());
					continue;
				}
			}
			--batchRemaining;
		}
		if (transport)
		{
			transport->rethrowIoError();
			if (discardInput)
				host.suspendPlayerInput();
			transport->serviceOwner(host, host.ticks(), discardInput);
		}
		if (!host.step(timing, error))
			return "runtime_error";
		if (options.metricsEvery && host.ticks() % options.metricsEvery == 0)
		{
			std::cout << "SERVER status=metrics";
			printMetrics(host.metrics(), droppedTicks, transport ? transport->worldTraffic() : net::udp::WorldTraffic{});
		}
	}
}

ExitCode runServer(const Options &options)
{
	std::optional<net::udp::Address> listen;
	if (!options.ticks)
	{
		try
		{
			listen = net::udp::parseAddress(options.listenAddress, static_cast<uint16_t>(options.port));
		}
		catch (const std::invalid_argument &ex)
		{
			std::cerr << "SERVER error=usage detail=" << ex.what() << '\n';
			return ExitCode::Usage;
		}
	}
	if (options.launcherControl && !server::hasLauncherPipe())
	{
		std::cerr << "SERVER error=usage detail=launcher control requires an inherited stdin pipe\n";
		return ExitCode::Usage;
	}
	if (!installStopHandlers(options.launcherControl))
	{
		std::cerr << "SERVER error=runtime detail=failed to install stop handlers\n";
		return ExitCode::Run;
	}
	auto config = options.host;
	if (options.launcherBootstrap)
	{
		config.ownerBootstrap = server::readLauncherBootstrap();
		if (!config.ownerBootstrap)
		{
			std::cerr << "SERVER error=usage detail=missing or invalid local owner bootstrap\n";
			return ExitCode::Usage;
		}
	}
	server::ServerHost host(std::move(config));
	std::string error;
	if (!host.start(error))
	{
		std::cerr << "SERVER error=world_open detail=" << error << '\n';
		return ExitCode::WorldOpen;
	}

	uint64_t droppedTicks = 0;
	const char *reason = "runtime_error";
	std::unique_ptr<net::udp::ServerTransport> transport;
	net::udp::WorldTraffic worldTraffic;
	try
	{
		const auto control = options.launcherControl ? server::pollLauncherPipe() : server::LauncherInput::Open;
		if (control == server::LauncherInput::Closed)
			reason = "launcher";
		else if (control == server::LauncherInput::Invalid)
			error = "unexpected input on the private launcher control pipe";
		else
		{
			if (listen)
			{
				transport = std::make_unique<net::udp::ServerTransport>(*listen, options.playerCap - 1, host.sessionWorld());
				transport->start();
			}
			std::cout << "SERVER status=ready mode=" << (options.ticks ? "deterministic" : "live")
					  << " transport=" << (transport ? "udp" : "disabled")
					  << " listen_address=" << options.listenAddress
					  << " port=" << (transport ? transport->address().port : options.port)
					  << " player_cap=" << options.playerCap << " workers=" << options.host.workers
					  << " restored=" << host.restoredPlayer();
			printMetrics(host.metrics(), 0);
			reason = runTicks(host, transport.get(), options, droppedTicks, error);
		}
	}
	catch (const std::exception &ex)
	{
		error = ex.what();
	}
	if (transport)
	{
		try
		{
			transport->stop(host);
			transport->rethrowIoError();
		}
		catch (const std::exception &ex)
		{
			if (error.empty())
				error = ex.what();
			reason = "runtime_error";
		}
		worldTraffic = transport->worldTraffic();
		transport.reset();
	}
	if (!error.empty())
		std::cerr << "SERVER error=runtime detail=" << error << '\n';
	std::cout << "SERVER status=stopping reason=" << reason << std::endl;
	std::string saveError;
	bool saved = false;
	try
	{
		saved = host.stop(saveError);
	}
	catch (const std::exception &ex)
	{
		saveError = ex.what();
	}
	if (!saved)
		std::cerr << "SERVER error=persistence detail=" << saveError << '\n';

	const auto result = saved ? (error.empty() ? ExitCode::Ok : ExitCode::Run) : ExitCode::Save;
	std::cout << "SERVER status=stopped reason=" << reason << " saved=" << saved
			  << " exit_code=" << static_cast<int>(result);
	printMetrics(host.metrics(), droppedTicks, worldTraffic);
	return result;
}

} // namespace

int main(int argc, char **argv)
{
	if (argc >= 2 && std::string_view(argv[1]) == "roles")
	{
		try
		{
			return server::runRolesCommand(argc, argv);
		}
		catch (const std::exception &)
		{
			std::cerr << "ROLES error=local role procedure failed\n";
			return static_cast<int>(ExitCode::Run);
		}
	}
	if (argc == 2 && std::string_view(argv[1]) == "--help")
	{
		printUsage();
		return 0;
	}
	Options options;
	std::string error;
	if (!parseOptions(argc, argv, options, error))
	{
		std::cerr << "SERVER error=usage detail=" << error << '\n';
		printUsage();
		return static_cast<int>(ExitCode::Usage);
	}
	try
	{
		return static_cast<int>(runServer(options));
	}
	catch (const std::exception &ex)
	{
		std::cerr << "SERVER error=runtime detail=" << ex.what() << '\n';
		return static_cast<int>(ExitCode::Run);
	}
}
