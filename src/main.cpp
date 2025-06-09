#include <Engine/Engine.hpp>
#include <string>
#include <iostream>
#include <cstdlib>

int main(int argc, char **argv)
{
	unsigned int seed_to_use = 0;

	if (argc == 1)
	{
		seed_to_use = 0; // Default seed
	}
	else if (argc == 2 && std::string(argv[1]) == "--help")
	{
		std::cout << "Usage: " << argv[0] << " [--seed <value>]" << std::endl;
		return EXIT_SUCCESS;
	}
	else if (argc == 3 && std::string(argv[1]) == "--seed")
	{
		char *endptr;
		long val = std::strtol(argv[2], &endptr, 10);

		if (*endptr != '\0' || argv[2] == endptr) // Check if conversion failed or no digits were read
		{
			std::cerr << "Error: Seed value '" << argv[2] << "' is not a valid integer." << std::endl;
			std::cerr << "Usage: " << argv[0] << " [--seed <value>]" << std::endl;
			return EXIT_FAILURE;
		}
		seed_to_use = static_cast<unsigned int>(val);
	}
	else
	{
		std::cerr << "Invalid arguments." << std::endl;
		std::cerr << "Usage: " << argv[0] << " [--seed <value>]" << std::endl;
		return EXIT_FAILURE;
	}

	try
	{
		Engine engine;
		engine.initializeNoiseGenerator(seed_to_use);
		engine.run();
	}
	catch (const std::exception &e)
	{
		std::cerr << "Runtime error: " << e.what() << std::endl;
		return EXIT_FAILURE;
	}
	catch (...)
	{
		std::cerr << "Unknown exception occurred." << std::endl;
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
