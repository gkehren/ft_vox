#include <Engine/Engine.hpp>
#include <Engine/ClientProfile.hpp>
#include <Engine/MultiplayerJoin.hpp>
#include <Network/UdpSocket.hpp>
#include <World/WorldPersistence.hpp>
#include <Chunk/StreamHelpers.hpp>
#include <Renderer/MinecraftTextures.hpp>
#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <cmath>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

// Exit-status vocabulary harmonized with the headless runner (review §4.1,
// docs/headless-runner.md): 2 = CLI usage error, 4 = runtime failure.
// (The headless runner additionally distinguishes 3 = script, 5 = assertion,
// 6 = timeout; the graphical host has no script/assertion/timeout paths.)
static constexpr int kExitUsage = 2;
static constexpr int kExitRuntime = 4;

static void printUsage(const char *argv0)
{
	std::cout << "Usage: " << argv0 << " [--seed <value>] [--world <name>] [--resource-pack <path.zip>]"
			  << " [--connect <IPv4:port>] [--vsync <on|off>] [--benchmark <seconds>]\n"
			  << "\n"
			  << "Options:\n"
			  << "  --seed <value>              Start solo play directly or set the expected server seed\n"
			  << "  --connect <IPv4:port>       Join an authoritative ft_vox_server via UDP\n"
			  << "  --player-profile <dir>     Reuse a multiplayer key across launches or endpoint changes\n"
			  << "  --world <name>              Open (or create) the persistent save saves/<name>\n"
			  << "                              (player state + block edits survive restarts)\n"
			  << "  --resource-pack <path>      Minecraft resource pack archive (.zip) or folder\n"
			  << "                              (defaults to ressources/default-resource-pack.zip)\n"
			  << "  --vsync <on|off>            FIFO when on; strict IMMEDIATE and uncapped when off\n"
			  << "  --benchmark <seconds>       Run a wide streaming benchmark, save report, exit\n"
			  << "  --ui-spike                  Run the standalone Game UI prototype over a solo scene\n"
			  << "  --game-ui                   Show the Game UI sample alongside the existing interface\n"
			  << "  --no-dev-ui                 Start without the developer workspace (F5 / Ctrl+Shift+D opens it)\n"
			  << "  --no-hud                    Start with the player HUD hidden\n"
			  << "  --front-bias <value>        Override streaming front load bias [0, "
			  << kSafeMaxStreamFrontBias << "] (clamped)\n"
			  << "  --quality <name|0-3>        Graphics preset: low|medium|high|cinematic\n"
			  << "  --benchmark-warmup <secs>   Override warmup (0 disables it)\n"
			  << "  --benchmark-map <zoom>      Open fixed-center biome map (zoom 0.1..8)\n"
			  << "  --benchmark-map-sequential  Compare the previous one-job map path\n"
			  << "  --benchmark-view-switch <n> Switch view distance (64..1024) at 25% of the\n"
			  << "                              measurement; reports PoolGrow metrics when\n"
			  << "                              additional pool capacity is required\n"
			  << "  --shadow-size <n>           Shadow map resolution tier (512, 1024, 2048, 4096)\n"
              << "  --view x y z yaw pitch secs Fixed daylight view for terrain review (secs 0 = no timeout)\n"
			  << "  --help                      Show this help\n"
			  << "\n"
			  << "Env: FT_VOX_RESOURCE_PACK=<path>  used when --resource-pack is not set\n";
}

/// Process-entry only: CLI --resource-pack wins over FT_VOX_RESOURCE_PACK.
static std::string resolveResourcePackRoot(const std::string &cliPack)
{
	if (!cliPack.empty())
		return trimTrailingSlashes(cliPack);
	if (const char *env = std::getenv("FT_VOX_RESOURCE_PACK"))
		return trimTrailingSlashes(env);
	return resourceRootPath() + "default-resource-pack.zip";
}

int main(int argc, char **argv)
{
	unsigned int seed_to_use = 0;
	bool directSeed = false;
	std::optional<net::udp::Address> server;
	std::string serverEndpoint;
	std::filesystem::path playerProfile;
	std::string worldName;
	std::string resourcePackCli;
	std::optional<bool> vsyncOverride;
	float benchmarkDuration = 0.0f;
	bool uiSpike = false;
	bool gameUi = false;
	bool showDeveloperUi = true;
	bool showPlayerHud = true;
	std::optional<float> benchmarkWarmup;
	std::optional<float> frontBiasOverride;
	std::optional<int> qualityPreset;
	float benchmarkMapZoom = 0.0f;
	bool benchmarkMapSequential = false;
	int benchmarkViewSwitch = 0;
	int shadowMapSizeOverride = 0;
    std::optional<std::array<float, 6>> inspection;

	for (int i = 1; i < argc; ++i)
	{
		const std::string arg = argv[i];
		if (arg == "--game-ui")
		{
			gameUi = true;
			continue;
		}
		if (arg == "--no-dev-ui")
		{
			showDeveloperUi = false;
			continue;
		}
		if (arg == "--no-hud")
		{
			showPlayerHud = false;
			continue;
		}
		if (arg == "--ui-spike")
		{
			uiSpike = true;
			continue;
		}
		if (arg == "--shadow-size")
		{
			if (i + 1 >= argc)
			{
				std::cerr << "Error: --shadow-size requires a resolution (512, 1024, 2048 or 4096).\n";
				return kExitUsage;
			}
			shadowMapSizeOverride = std::atoi(argv[++i]);
			if (shadowMapSizeOverride != 512 && shadowMapSizeOverride != 1024 &&
				shadowMapSizeOverride != 2048 && shadowMapSizeOverride != 4096)
			{
				std::cerr << "Error: --shadow-size must be 512, 1024, 2048 or 4096.\n";
				return kExitUsage;
			}
			continue;
		}
        if (arg == "--view")
        {
            if (i + 6 >= argc) { std::cerr << "--view requires x y z yaw pitch seconds\n"; return kExitUsage; }
            std::array<float, 6> values{};
            for (auto &value : values)
            {
                char *end = nullptr;
                const char *start = argv[++i];
                value = std::strtof(start, &end);
                if (end == start || *end || !std::isfinite(value))
                { std::cerr << "--view requires finite numbers\n"; return kExitUsage; }
            }
            if (std::abs(values[0]) > 1000000.f || std::abs(values[2]) > 1000000.f ||
                values[1] < 0.f || values[1] > 512.f || std::abs(values[3]) > 360.f ||
                std::abs(values[4]) > 89.f || values[5] < 0.f || values[5] > 300.f)
            { std::cerr << "--view coordinates or duration outside inspection bounds\n"; return kExitUsage; }
            inspection = values;
            continue;
        }
		if (arg == "--help" || arg == "-h")
		{
			printUsage(argv[0]);
			return EXIT_SUCCESS;
		}
		if (arg == "--connect")
		{
			if (i + 1 >= argc || server)
			{
				std::cerr << "Error: specify one --connect <IPv4:port> endpoint.\n";
				return kExitUsage;
			}
			const std::string_view endpoint = argv[++i];
			const auto split = endpoint.find(':');
			server = client::parseEndpoint(endpoint);
			if (!server)
			{
				std::cerr << "Error: --connect expects a numeric IPv4 address and port 1–65535.\n";
				return kExitUsage;
			}
			serverEndpoint = endpoint;
			serverEndpoint[split] = '-';
			continue;
		}
		if (arg == "--player-profile")
		{
			if (i + 1 >= argc || !playerProfile.empty() || std::string_view(argv[i + 1]).starts_with("--") ||
				std::string_view(argv[i + 1]).empty())
			{
				std::cerr << "Error: specify one --player-profile <directory>.\n";
				return kExitUsage;
			}
			playerProfile = argv[++i];
			continue;
		}
		if (arg == "--seed")
		{
			if (i + 1 >= argc)
			{
				std::cerr << "Error: --seed requires a value.\n";
				printUsage(argv[0]);
				return kExitUsage;
			}
			// Same contract as the headless runner (review §4.1): strict
			// from_chars parse, range [1, 2147483647]. The old strtol path
			// silently truncated overflow (LONG_MAX) and wrapped negatives;
			// --seed 0 was silently replaced by 42 in benchmark mode.
			const std::string_view seedArg = argv[++i];
			uint32_t parsedSeed = 0;
			const auto [ptr, ec] = std::from_chars(
				seedArg.data(), seedArg.data() + seedArg.size(), parsedSeed);
			if (ec != std::errc{} || ptr != seedArg.data() + seedArg.size() ||
				parsedSeed == 0 || parsedSeed > 2147483647u)
			{
				std::cerr << "Error: --seed expects an integer in [1, 2147483647] (got '"
						  << seedArg << "').\n";
				printUsage(argv[0]);
				return kExitUsage;
			}
			seed_to_use = parsedSeed;
			directSeed = true;
			continue;
		}
		if (arg == "--world")
		{
			if (i + 1 >= argc)
			{
				std::cerr << "Error: --world requires a name.\n";
				printUsage(argv[0]);
				return kExitUsage;
			}
			worldName = argv[++i];
			// Validation lives in the persistence layer (issue #180 review):
			// the same rule guards openOrCreate/worldExists/peekStoredSeed.
			if (!WorldPersistence::isValidWorldName(worldName))
			{
				std::cerr << "Error: --world name must be non-empty and must not contain "
				          << "any of / \\ : * ? \" < > | or \"..\".\n";
				printUsage(argv[0]);
				return kExitUsage;
			}
			continue;
		}
		if (arg == "--resource-pack")
		{
			if (i + 1 >= argc)
			{
				std::cerr << "Error: --resource-pack requires a path.\n";
				printUsage(argv[0]);
				return kExitUsage;
			}
			resourcePackCli = argv[++i];
			continue;
		}
		if (arg == "--benchmark-map-sequential")
		{
			benchmarkMapSequential = true;
			continue;
		}
		if (arg == "--benchmark-map")
		{
			if (i + 1 >= argc)
			{
				std::cerr << "Error: --benchmark-map requires a zoom.\n";
				return kExitUsage;
			}
			char *end = nullptr;
			benchmarkMapZoom = std::strtof(argv[++i], &end);
			if (end == argv[i] || *end || !std::isfinite(benchmarkMapZoom) ||
				benchmarkMapZoom < 0.1f || benchmarkMapZoom > 8.0f)
			{
				std::cerr << "Error: --benchmark-map zoom must be between 0.1 and 8.\n";
				return kExitUsage;
			}
			continue;
		}
		if (arg == "--benchmark-warmup")
		{
			if (i + 1 >= argc)
			{
				std::cerr << "Error: --benchmark-warmup requires seconds.\n";
				return kExitUsage;
			}
			char *end = nullptr;
			const float seconds = std::strtof(argv[++i], &end);
			if (end == argv[i] || *end || !std::isfinite(seconds) || seconds < 0.f || seconds > 300.f)
			{
				std::cerr << "Error: benchmark warmup must be between 0 and 300 seconds.\n";
				return kExitUsage;
			}
			benchmarkWarmup = seconds;
			continue;
		}
		if (arg == "--benchmark")
		{
			if (i + 1 >= argc)
			{
				std::cerr << "Error: --benchmark requires a duration in seconds.\n";
				printUsage(argv[0]);
				return kExitUsage;
			}
			char *endptr = nullptr;
			benchmarkDuration = std::strtof(argv[++i], &endptr);
			if (*endptr != '\0' || endptr == argv[i] ||
				benchmarkDuration < 5.0f || benchmarkDuration > 300.0f)
			{
				std::cerr << "Error: benchmark duration must be between 5 and 300 seconds.\n";
				return kExitUsage;
			}
			continue;
		}
		if (arg == "--benchmark-view-switch")
		{
			if (i + 1 >= argc)
			{
				std::cerr << "Error: --benchmark-view-switch requires a view distance in blocks.\n";
				return kExitUsage;
			}
			char *endptr = nullptr;
			const long val = std::strtol(argv[++i], &endptr, 10);
			if (*endptr != '\0' || endptr == argv[i] || val < 64 || val > 1024)
			{
				std::cerr << "Error: --benchmark-view-switch must be between 64 and 1024 blocks.\n";
				return kExitUsage;
			}
			benchmarkViewSwitch = static_cast<int>(val);
			continue;
		}
		if (arg == "--front-bias")
		{
			if (i + 1 >= argc)
			{
				std::cerr << "Error: --front-bias requires a value.\n";
				return kExitUsage;
			}
			char *end = nullptr;
			const float val = std::strtof(argv[++i], &end);
			if (end == argv[i] || *end || !std::isfinite(val) || val < 0.f)
			{
				std::cerr << "Error: --front-bias requires a number in [0, "
						  << kSafeMaxStreamFrontBias << "] (values above are clamped).\n";
				return kExitUsage;
			}
			frontBiasOverride = val;
			continue;
		}
		if (arg == "--quality")
		{
			if (i + 1 >= argc)
			{
				std::cerr << "Error: --quality requires low|medium|high|cinematic (or 0-3).\n";
				return kExitUsage;
			}
			const std::string value = argv[++i];
			int preset = -1;
			if (value == "low" || value == "0")
				preset = 0;
			else if (value == "medium" || value == "1")
				preset = 1;
			else if (value == "high" || value == "2")
				preset = 2;
			else if (value == "cinematic" || value == "3")
				preset = 3;
			if (preset < 0)
			{
				std::cerr << "Error: --quality accepts low|medium|high|cinematic (or 0-3).\n";
				return kExitUsage;
			}
			qualityPreset = preset;
			continue;
		}
		if (arg == "--vsync")
		{
			if (i + 1 >= argc)
			{
				std::cerr << "Error: --vsync requires on or off.\n";
				return kExitUsage;
			}
			const std::string value = argv[++i];
			if (value == "on")
				vsyncOverride = true;
			else if (value == "off")
				vsyncOverride = false;
			else
			{
				std::cerr << "Error: --vsync accepts only on or off.\n";
				return kExitUsage;
			}
			continue;
		}
		std::cerr << "Invalid argument: " << arg << "\n";
		printUsage(argv[0]);
		return kExitUsage;
	}

    if (inspection && benchmarkDuration > 0.f)
    { std::cerr << "--view and --benchmark are separate camera modes\n"; return kExitUsage; }
	if (benchmarkWarmup && (benchmarkDuration == 0.f || *benchmarkWarmup > benchmarkDuration))
	{
		std::cerr << "Error: --benchmark-warmup requires --benchmark and cannot exceed its duration.\n";
		return kExitUsage;
	}
	if ((benchmarkMapZoom > 0.0f && benchmarkDuration == 0.0f) ||
		(benchmarkMapSequential && benchmarkMapZoom == 0.0f))
	{
		std::cerr << "Error: map benchmark options require --benchmark and --benchmark-map.\n";
		return kExitUsage;
	}
	if (benchmarkViewSwitch > 0 && benchmarkDuration == 0.0f)
	{
		std::cerr << "Error: --benchmark-view-switch requires --benchmark.\n";
		return kExitUsage;
	}
	const std::string resourcePack = resolveResourcePackRoot(resourcePackCli);

	if (uiSpike && (server || !worldName.empty() || benchmarkDuration > 0.f || inspection))
	{
		std::cerr << "Error: --ui-spike uses a transient solo scene and cannot be combined with --connect, --world, --benchmark or --view.\n";
		return kExitUsage;
	}
	if (server && (!worldName.empty() || benchmarkDuration > 0.f || inspection))
	{
		std::cerr << "Error: --connect cannot be combined with --world, --benchmark or --view.\n";
		return kExitUsage;
	}
	if (!server && !playerProfile.empty())
	{
		std::cerr << "Error: --player-profile requires --connect.\n";
		return kExitUsage;
	}
	if (!worldName.empty() && benchmarkDuration > 0.0f)
	{
		// Benchmark worlds are transient by contract (issue #180): they must
		// never open a persistent save. Fail fast instead of silently
		// ignoring one of the two flags.
		std::cerr << "Error: --world and --benchmark cannot be combined." << std::endl;
		return kExitUsage;
	}

	try
	{
		std::optional<net::wire::PlayerKey> playerKey;
		if (server)
		{
			if (playerProfile.empty())
				playerProfile = std::filesystem::path("saves") / ".client-profiles" / serverEndpoint;
			playerKey = client::playerKey(playerProfile, client::ProfileAccess::LoadOrCreate);
		}
		const bool directSolo = !worldName.empty() || directSeed || benchmarkDuration > 0.f ||
			inspection || uiSpike || gameUi;
		Engine engine(resourcePack);
		engine.setPlayerSettingsOverrides(vsyncOverride.has_value(), qualityPreset.has_value());
		engine.setShowDeveloperUi(showDeveloperUi && (directSolo || server.has_value()));
		engine.setShowPlayerHud(showPlayerHud);
		if (server)
			engine.connectRemote(*server, seed_to_use ? std::optional<uint32_t>(seed_to_use) : std::nullopt, *playerKey);
		else if (directSolo)
		{
			if (!worldName.empty())
				engine.requestOpenWorld(worldName);
			engine.initializeNoiseGenerator(static_cast<int>(seed_to_use));
		}
        if (inspection)
        {
            const auto &v = *inspection;
            engine.setInspectionView({v[0], v[1], v[2]}, v[3], v[4], v[5]);
        }
		if (vsyncOverride)
			engine.setVSync(*vsyncOverride);
		if (qualityPreset)
			engine.setGraphicsQualityPreset(static_cast<GraphicsQualityPreset>(*qualityPreset));
		if (frontBiasOverride)
			engine.setStreamFrontBias(*frontBiasOverride);
		if (shadowMapSizeOverride > 0)
			engine.setShadowMapSize(shadowMapSizeOverride);
		if (benchmarkDuration > 0.0f)
		{
			BenchmarkConfig &config = engine.benchmark().config();
			config.seed = seed_to_use > 0 ? static_cast<int>(seed_to_use) : 42;
			config.durationSec = benchmarkDuration;
			config.biomeMapZoom = benchmarkMapZoom;
			config.biomeMapSequential = benchmarkMapSequential;
			config.viewDistanceSwitch = benchmarkViewSwitch;
			config.warmupSec = benchmarkWarmup.value_or(std::min(2.0f, benchmarkDuration * 0.2f));
			config.pathOrbits = 2;
			config.pathRadius = 512.0f;
			config.pathHeight = 72.0f;
			config.forceVsyncOff =
				!vsyncOverride.has_value() || !*vsyncOverride;
			engine.benchmark().requestStart();
			engine.setExitAfterBenchmark(true);
		}
		if (uiSpike || (gameUi && !inspection && benchmarkDuration == 0.f))
			engine.enableGameUi(uiSpike);
		engine.run();
	}
	catch (const std::exception &e)
	{
		std::cerr << "Runtime error: " << e.what() << "\n";
		return kExitRuntime;
	}
	catch (...)
	{
		std::cerr << "Unknown exception occurred.\n";
		return kExitRuntime;
	}

	return EXIT_SUCCESS;
}
