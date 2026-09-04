#include <Engine/Engine.hpp>
#include <Renderer/MinecraftTextures.hpp>
#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <iostream>
#include <optional>
#include <string>

static void printUsage(const char *argv0)
{
	std::cout << "Usage: " << argv0 << " [--seed <value>] [--resource-pack <path.zip>]"
			  << " [--vsync <on|off>] [--benchmark <seconds>]\n"
			  << "\n"
			  << "Options:\n"
			  << "  --seed <value>              World seed (integer)\n"
			  << "  --resource-pack <path>      Minecraft resource pack archive (.zip) or folder\n"
			  << "                              (defaults to ressources/default-resource-pack.zip)\n"
			  << "  --vsync <on|off>            FIFO when on; strict IMMEDIATE and uncapped when off\n"
			  << "  --benchmark <seconds>       Run a wide streaming benchmark, save report, exit\n"
			  << "  --benchmark-warmup <secs>   Override warmup (0 disables it)\n"
			  << "  --benchmark-map <zoom>      Open fixed-center biome map (zoom 0.1..8)\n"
			  << "  --benchmark-map-sequential  Compare the previous one-job map path\n"
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
	return std::string(RES_PATH) + "default-resource-pack.zip";
}

int main(int argc, char **argv)
{
	unsigned int seed_to_use = 0;
	std::string resourcePackCli;
	std::optional<bool> vsyncOverride;
	float benchmarkDuration = 0.0f;
	std::optional<float> benchmarkWarmup;
	float benchmarkMapZoom = 0.0f;
	bool benchmarkMapSequential = false;

	for (int i = 1; i < argc; ++i)
	{
		const std::string arg = argv[i];
		if (arg == "--help" || arg == "-h")
		{
			printUsage(argv[0]);
			return EXIT_SUCCESS;
		}
		if (arg == "--seed")
		{
			if (i + 1 >= argc)
			{
				std::cerr << "Error: --seed requires a value.\n";
				printUsage(argv[0]);
				return EXIT_FAILURE;
			}
			char *endptr = nullptr;
			const long val = std::strtol(argv[++i], &endptr, 10);
			if (*endptr != '\0' || endptr == argv[i])
			{
				std::cerr << "Error: Seed value '" << argv[i] << "' is not a valid integer.\n";
				printUsage(argv[0]);
				return EXIT_FAILURE;
			}
			seed_to_use = static_cast<unsigned int>(val);
			continue;
		}
		if (arg == "--resource-pack")
		{
			if (i + 1 >= argc)
			{
				std::cerr << "Error: --resource-pack requires a path.\n";
				printUsage(argv[0]);
				return EXIT_FAILURE;
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
				return EXIT_FAILURE;
			}
			char *end = nullptr;
			benchmarkMapZoom = std::strtof(argv[++i], &end);
			if (end == argv[i] || *end || !std::isfinite(benchmarkMapZoom) ||
				benchmarkMapZoom < 0.1f || benchmarkMapZoom > 8.0f)
			{
				std::cerr << "Error: --benchmark-map zoom must be between 0.1 and 8.\n";
				return EXIT_FAILURE;
			}
			continue;
		}
		if (arg == "--benchmark-warmup")
		{
			if (i + 1 >= argc)
			{
				std::cerr << "Error: --benchmark-warmup requires seconds.\n";
				return EXIT_FAILURE;
			}
			char *end = nullptr;
			const float seconds = std::strtof(argv[++i], &end);
			if (end == argv[i] || *end || !std::isfinite(seconds) || seconds < 0.f || seconds > 300.f)
			{
				std::cerr << "Error: benchmark warmup must be between 0 and 300 seconds.\n";
				return EXIT_FAILURE;
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
				return EXIT_FAILURE;
			}
			char *endptr = nullptr;
			benchmarkDuration = std::strtof(argv[++i], &endptr);
			if (*endptr != '\0' || endptr == argv[i] ||
				benchmarkDuration < 5.0f || benchmarkDuration > 300.0f)
			{
				std::cerr << "Error: benchmark duration must be between 5 and 300 seconds.\n";
				return EXIT_FAILURE;
			}
			continue;
		}
		if (arg == "--vsync")
		{
			if (i + 1 >= argc)
			{
				std::cerr << "Error: --vsync requires on or off.\n";
				return EXIT_FAILURE;
			}
			const std::string value = argv[++i];
			if (value == "on")
				vsyncOverride = true;
			else if (value == "off")
				vsyncOverride = false;
			else
			{
				std::cerr << "Error: --vsync accepts only on or off.\n";
				return EXIT_FAILURE;
			}
			continue;
		}
		std::cerr << "Invalid argument: " << arg << "\n";
		printUsage(argv[0]);
		return EXIT_FAILURE;
	}

	if (benchmarkWarmup && (benchmarkDuration == 0.f || *benchmarkWarmup > benchmarkDuration))
	{
		std::cerr << "Error: --benchmark-warmup requires --benchmark and cannot exceed its duration.\n";
		return EXIT_FAILURE;
	}
	if ((benchmarkMapZoom > 0.0f && benchmarkDuration == 0.0f) ||
		(benchmarkMapSequential && benchmarkMapZoom == 0.0f))
	{
		std::cerr << "Error: map benchmark options require --benchmark and --benchmark-map.\n";
		return EXIT_FAILURE;
	}
	const std::string resourcePack = resolveResourcePackRoot(resourcePackCli);

	try
	{
		Engine engine(resourcePack);
		engine.initializeNoiseGenerator(static_cast<int>(seed_to_use));
		if (vsyncOverride)
			engine.setVSync(*vsyncOverride);
		if (benchmarkDuration > 0.0f)
		{
			BenchmarkConfig &config = engine.benchmark().config();
			config.seed = seed_to_use > 0 ? static_cast<int>(seed_to_use) : 42;
			config.durationSec = benchmarkDuration;
			config.biomeMapZoom = benchmarkMapZoom;
			config.biomeMapSequential = benchmarkMapSequential;
			config.warmupSec = benchmarkWarmup.value_or(std::min(2.0f, benchmarkDuration * 0.2f));
			config.pathOrbits = 2;
			config.pathRadius = 512.0f;
			config.pathHeight = 72.0f;
			config.forceVsyncOff =
				!vsyncOverride.has_value() || !*vsyncOverride;
			engine.benchmark().requestStart();
			engine.setExitAfterBenchmark(true);
		}
		engine.run();
	}
	catch (const std::exception &e)
	{
		std::cerr << "Runtime error: " << e.what() << "\n";
		return EXIT_FAILURE;
	}
	catch (...)
	{
		std::cerr << "Unknown exception occurred.\n";
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
