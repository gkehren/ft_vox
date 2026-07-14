// Device smoke test for PR2 Vulkan resource helpers (VMA buffers/images + SPIR-V).
// Requires a working ICD (MoltenVK on macOS).

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <Vulkan/VkContext.hpp>
#include <Vulkan/VkResourceSmoke.hpp>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

static std::string resolveSpv(const char *name)
{
	// Prefer next to the test binary: ./ressources/shaders/spv/<name>
	// CMake copies SPIR-V next to ft_vox; tests may live in build/tests/.
	const char *candidates[] = {
		"ressources/shaders/spv/",
		"../ressources/shaders/spv/",
		"../../ressources/shaders/spv/",
		"./build-vk/ressources/shaders/spv/",
	};
	for (const char *prefix : candidates)
	{
		std::string path = std::string(prefix) + name;
		FILE *f = std::fopen(path.c_str(), "rb");
		if (f)
		{
			std::fclose(f);
			return path;
		}
	}
	return std::string("ressources/shaders/spv/") + name;
}

int main()
{
	if (!SDL_Init(SDL_INIT_VIDEO))
	{
		std::cerr << "FAIL: SDL_Init: " << SDL_GetError() << "\n";
		return EXIT_FAILURE;
	}

	if (!SDL_Vulkan_LoadLibrary(nullptr))
	{
		std::cerr << "FAIL: SDL_Vulkan_LoadLibrary: " << SDL_GetError() << "\n";
		SDL_Quit();
		return EXIT_FAILURE;
	}

	SDL_Window *window = SDL_CreateWindow("ft_vox vulkan resource test", 64, 64,
										  SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
	if (!window)
	{
		std::cerr << "FAIL: SDL_CreateWindow: " << SDL_GetError() << "\n";
		SDL_Vulkan_UnloadLibrary();
		SDL_Quit();
		return EXIT_FAILURE;
	}

	int exitCode = EXIT_FAILURE;
	try
	{
		VkContext context;
		context.init(window);

		const std::string vert = resolveSpv("smoke.vert.spv");
		const std::string frag = resolveSpv("smoke.frag.spv");
		std::cout << "Using SPIR-V:\n  " << vert << "\n  " << frag << "\n";

		if (runVulkanResourceSmoke(context, vert, frag))
		{
			std::cout << "ALL RESOURCE SMOKES PASSED\n";
			exitCode = EXIT_SUCCESS;
		}

		context.shutdown();
	}
	catch (const std::exception &e)
	{
		std::cerr << "FAIL: " << e.what() << "\n";
		exitCode = EXIT_FAILURE;
	}

	SDL_DestroyWindow(window);
	SDL_Vulkan_UnloadLibrary();
	SDL_Quit();
	return exitCode;
}
