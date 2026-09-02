#include <Engine/Engine.hpp>
#include <Renderer/MinecraftTextures.hpp>
#include <algorithm>
#include <cstdlib>
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
			config.warmupSec = std::min(2.0f, benchmarkDuration * 0.2f);
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
