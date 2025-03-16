#include <Engine/Engine.hpp>

int main(int argc, char **argv)
{
	if (argc == 2 && (std::string(argv[1]) == "--seed" || std::string(argv[1]) == "--help"))
	{
		std::cerr << "Usage: " << argv[0] << " [--seed <seed>]" << std::endl;
		return EXIT_FAILURE;
	}

	try
	{
		Engine engine;
		if (argc > 2 || (argc == 2 && std::string(argv[1]) == "--seed"))
		{
			engine.perlinNoise(std::atoi(argv[2]));
		}
		else
		{
			engine.perlinNoise(0);
		}
		engine.run();
	}
	catch (const std::exception &e)
	{
		std::cerr << e.what() << std::endl;
		return EXIT_FAILURE;
	}
	catch (...)
	{
		std::cerr << "Unknown exception" << std::endl;
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
