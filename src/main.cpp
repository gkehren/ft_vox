#include <Engine/Engine.hpp>
#include <Renderer/MinecraftTextures.hpp>
#include <string>
#include <iostream>
#include <cstdlib>

static void printUsage(const char *argv0)
{
	std::cout << "Usage: " << argv0 << " [--seed <value>] [--resource-pack <path>]\n"
			  << "\n"
			  << "Options:\n"
			  << "  --seed <value>              World seed (integer)\n"
			  << "  --resource-pack <path>      Minecraft resource pack root\n"
			  << "                              (loads assets/minecraft/textures/block/*.png)\n"
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
	return {};
}

int main(int argc, char **argv)
{
	unsigned int seed_to_use = 0;
	std::string resourcePackCli;

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
		std::cerr << "Invalid argument: " << arg << "\n";
		printUsage(argv[0]);
		return EXIT_FAILURE;
	}

	const std::string resourcePack = resolveResourcePackRoot(resourcePackCli);

	try
	{
		Engine engine(resourcePack);
		engine.initializeNoiseGenerator(static_cast<int>(seed_to_use));
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
