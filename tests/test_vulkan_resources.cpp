// Device smoke test for PR2 Vulkan resource helpers (VMA buffers/images + SPIR-V).
// Requires a working ICD (MoltenVK on macOS). Uses the same loader probe as Engine.

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <Vulkan/VkContext.hpp>
#include <Vulkan/VkLoadLibrary.hpp>
#include <Vulkan/VkResourceSmoke.hpp>
#include <Vulkan/VkSwapchain.hpp>

#include <algorithm>
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

	// Shipped helper — same load order as Engine (FT_VOX_VULKAN_LIB / Homebrew / default).
	if (!loadVulkanLibrary(&std::cout))
	{
		std::cerr << "FAIL: SDL_Vulkan_LoadLibrary: " << SDL_GetError() << "\n"
				  << vulkanLibraryLoadHint() << "\n";
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

		const SwapchainSupportDetails support = VkSwapchain::querySupport(
			context.getPhysicalDevice(), context.getSurface());
		const auto hasMode = [&](VkPresentModeKHR mode) {
			return std::find(support.presentModes.begin(),
							 support.presentModes.end(),
							 mode) != support.presentModes.end();
		};
		if (!hasMode(VK_PRESENT_MODE_FIFO_KHR))
			throw std::runtime_error("surface is missing mandatory FIFO mode");

		const std::vector<VkPresentModeKHR> syntheticModes = {
			VK_PRESENT_MODE_MAILBOX_KHR,
			VK_PRESENT_MODE_FIFO_KHR,
			VK_PRESENT_MODE_IMMEDIATE_KHR};
		if (VkSwapchain::selectPresentMode(syntheticModes, false) !=
			VK_PRESENT_MODE_IMMEDIATE_KHR)
			throw std::runtime_error(
				"VSync-off policy preferred a synchronized mode over IMMEDIATE");

		bool rejectedSynchronizedFallback = false;
		try
		{
			VkSwapchain::selectPresentMode(
				{VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_FIFO_KHR},
				false);
		}
		catch (const std::runtime_error &)
		{
			rejectedSynchronizedFallback = true;
		}
		if (!rejectedSynchronizedFallback)
			throw std::runtime_error(
				"VSync-off policy accepted MAILBOX/FIFO without IMMEDIATE");

		VkSwapchain swapchain;
		swapchain.init(context, 64, 64, true);
		if (swapchain.getPresentMode() != VK_PRESENT_MODE_FIFO_KHR)
			throw std::runtime_error("VSync on did not select strict FIFO mode");

		if (hasMode(VK_PRESENT_MODE_IMMEDIATE_KHR))
		{
			swapchain.setVSync(false);
			if (swapchain.isVSync() ||
				swapchain.getPresentMode() != VK_PRESENT_MODE_IMMEDIATE_KHR)
				throw std::runtime_error(
					"VSync off did not select strict IMMEDIATE mode");

			swapchain.setVSync(true);
			if (!swapchain.isVSync() ||
				swapchain.getPresentMode() != VK_PRESENT_MODE_FIFO_KHR)
				throw std::runtime_error(
					"re-enabled VSync did not restore strict FIFO mode");
			std::cout << "PASS: VSync toggle FIFO -> IMMEDIATE -> FIFO"
					  << " (no MAILBOX/FIFO fallback while disabled)\n";
		}
		else
		{
			std::cout << "PASS: surface lacks IMMEDIATE; strict policy rejects"
					  << " disabling VSync instead of falling back\n";
		}
		swapchain.shutdown();

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
