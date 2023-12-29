#include <Engine/Engine.hpp>

int main(void)
{
	try {
		Engine engine;
		engine.run();
	} catch (const std::exception& e) {
		std::cerr << e.what() << std::endl;
		return EXIT_FAILURE;
	} catch (...) {
		std::cerr << "Unknown exception" << std::endl;
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
