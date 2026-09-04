// Device smoke test for PR2 Vulkan resource helpers (VMA buffers/images + SPIR-V).
// Requires a working ICD (MoltenVK on macOS). Uses the same loader probe as Engine.

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <Vulkan/VkContext.hpp>
#include <Vulkan/VkLoadLibrary.hpp>
#include <Vulkan/VkResourceSmoke.hpp>
#include <Vulkan/VkSwapchain.hpp>
#include <Vulkan/VkGpuProfiler.hpp>
#include <Vulkan/VkCommands.hpp>
#include <Vulkan/StagingRing.hpp>
#include <Vulkan/GpuResourceRetire.hpp>

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

int main(int argc, char **argv)
{
	const bool validationErrorProbe = argc == 2 && std::string(argv[1]) == "--validation-error-probe";
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
        {
            using namespace telemetry;
            auto& t = registry();
            t.beginCapture();
            auto buffer = createBuffer(context.getAllocator(), 1024,
                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
            trackMeshBuffer(buffer, GpuOpaqueVertex);
            GpuResourceRetire retire;
            retire.init(context.getAllocator(), 2);
            retire.retireBuffer(buffer);
            buffer = {};
            auto pending = t.snapshot();
            retire.beginFrame(1);
            const auto early = t.snapshot();
            retire.beginFrame(2);
            const auto complete = t.snapshot();
            retire.shutdown();
            if (t.enabled && (pending.current[GpuOpaqueVertex] != 0 ||
                pending.current[RetiredBytes] != 1024 || pending.current[RetiredBuffers] != 1 ||
                early.current[RetiredBytes] != 1024 || complete.current[RetiredBytes] != 0 ||
                complete.current[RetiredBuffers] != 0 || complete.events[AllocCreated] != 1 ||
                complete.events[AllocDestroyed] != 1))
                throw std::runtime_error("mesh retirement telemetry mismatch");
            StagingRing staging;
            staging.init(context.getAllocator(), 2, 8*1024*1024);
            staging.beginFrame(0);
            VkDeviceSize offset{}; void* ptr{};
            const bool first = staging.alloc(1, offset, ptr);
            const bool overflow = staging.alloc(staging.sliceCapacity(), offset, ptr);
            const auto full = t.snapshot();
            staging.beginFrame(1);
            const auto next = t.snapshot();
            staging.shutdown();
            if (!first || overflow || (t.enabled && (full.current[StagingUsed] != StagingRing::kAlignment ||
                full.events[StagingFailures] != 1 || next.current[StagingUsed] != 0 ||
                next.peak[StagingUsed] != StagingRing::kAlignment)))
                throw std::runtime_error("staging pressure telemetry mismatch");
            std::cout << "PASS: mesh allocation/retirement and staging telemetry\n";
        }
		if (validationErrorProbe && !context.isValidationEnabled())
		{
			std::cout << "SKIP: validation error probe requires Khronos validation\n";
			context.shutdown();
			SDL_DestroyWindow(window);
			SDL_Vulkan_UnloadLibrary();
			SDL_Quit();
			return 77;
		}
		if (!context.hasDynamicRendering() ||
			((!vkCmdBeginRendering && !vkCmdBeginRenderingKHR) ||
			 (!vkCmdEndRendering && !vkCmdEndRenderingKHR)))
		{
			throw std::runtime_error(
				"dynamic rendering feature or entry points unavailable");
		}
		std::cout << "PASS: dynamic rendering feature + entry points\n";

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
			swapchain.recreate(64, 64, false);
			if (swapchain.isVSync() ||
				swapchain.getPresentMode() != VK_PRESENT_MODE_IMMEDIATE_KHR)
				throw std::runtime_error(
					"VSync off did not select strict IMMEDIATE mode");

			swapchain.recreate(64, 64, true);
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

		VkGpuProfiler gpu;
		gpu.init(context, 0);
		if (gpu.supported() || std::string(gpu.status()).find("no frame slots") == std::string::npos)
			throw std::runtime_error("zero-slot profiler should be unavailable with a diagnostic");
		gpu.shutdown();
		gpu.init(context, 2);
		uint32_t familyCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(context.getPhysicalDevice(), &familyCount, nullptr);
		std::vector<VkQueueFamilyProperties> families(familyCount);
		vkGetPhysicalDeviceQueueFamilyProperties(context.getPhysicalDevice(), &familyCount, families.data());
		const auto &limits = context.getDeviceProperties().limits;
		std::cout << "GPU timestamp capability:\n  graphics family: " << context.getGraphicsQueueFamily()
			<< "\n  valid bits: " << families.at(context.getGraphicsQueueFamily()).timestampValidBits
			<< "\n  period: " << limits.timestampPeriod << " ns"
			<< "\n  timestampComputeAndGraphics: " << (limits.timestampComputeAndGraphics ? "true" : "false")
			<< "\n  selected queue supported: " << (gpu.supported() ? "yes" : "no")
			<< "\n  status: " << gpu.status() << '\n';
		if (gpu.supported())
		{
			gpu.setEnabled(true);
			ImmediateCommands commands;
			commands.init(context);
			const auto record = [&](uint32_t slot) {
				commands.submitAndWait([&](VkCommandBuffer cmd) {
					gpu.beginRecording(cmd, slot, 42);
					gpu.beginPass(cmd, GpuPass::Shadow);
					gpu.endPass(cmd, GpuPass::Shadow);
					gpu.endRecording(cmd);
				}); // Test fence; production reuses VkFrameContext's existing fence.
				gpu.markSubmitted(slot);
			};
			for (uint32_t i = 0; i < 6; ++i)
			{
				record(i % 2);
				gpu.onSlotReady(i % 2);
				if (gpu.latest().serial != i + 1 || !gpu.latest().present[2] ||
					gpu.latest().present[3] || gpu.latest().benchmarkTag != 42)
					throw std::runtime_error("GPU slot recycling or absent-pass availability failed");
			}
			record(0);
			gpu.syncCapture(1);
			gpu.onSlotReady(0);
			if (gpu.latest().serial || gpu.historyCount())
				throw std::runtime_error("old GPU capture survived reset");
			gpu.setEnabled(false);
			record(1);
			gpu.onSlotReady(1);
			if (gpu.latest().serial) throw std::runtime_error("disabled GPU profiler collected samples");
			gpu.setEnabled(true);
			record(0);
			gpu.onSlotReady(0);
			if (!gpu.latest().serial) throw std::runtime_error("GPU profiler failed to resume");
			record(1); // Result completed but not yet consumed when capture is toggled.
			gpu.setEnabled(false);
			if (gpu.latest().serial || gpu.historyCount())
				throw std::runtime_error("GPU toggle did not immediately clear displayed data");
			gpu.setEnabled(true);
			gpu.onSlotReady(1);
			if (gpu.latest().serial || gpu.historyCount())
				throw std::runtime_error("pending GPU sample survived OFF/ON");
			record(0);
			gpu.onSlotReady(0);
			if (!gpu.latest().serial || gpu.historyCount() != 1)
				throw std::runtime_error("fresh capture did not resume after OFF/ON");
			std::cout << "PASS: GPU timestamp recycling, partial queries, reset and toggle\n";
		}
		else std::cout << "SKIP: GPU timestamps unsupported\n";
		gpu.shutdown();

		const std::string vert = resolveSpv("smoke.vert.spv");
		const std::string frag = resolveSpv("smoke.frag.spv");
		std::cout << "Using SPIR-V:\n  " << vert << "\n  " << frag << "\n";

		if (runVulkanResourceSmoke(context, vert, frag))
		{
			std::cout << "RESOURCE OPERATIONS PASSED\n";
			exitCode = EXIT_SUCCESS;
		}

		if (validationErrorProbe)
		{
			// Exercise the real debug messenger without issuing an invalid GPU command.
			VkDebugUtilsMessengerCallbackDataEXT message{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT};
			message.pMessageIdName = "FT_VOX_TEST_VALIDATION_ERROR";
			message.pMessage = "Intentional diagnostic error: the smoke must return failure.";
			vkSubmitDebugUtilsMessageEXT(context.getInstance(), VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
				VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT, &message);
		}
		context.shutdown();
		if (context.validationErrorCount() != 0)
			throw std::runtime_error("Vulkan validation reported " +
				std::to_string(context.validationErrorCount()) + " error(s); resource success is not clean validation");
		if (exitCode == EXIT_SUCCESS)
			std::cout << "ALL RESOURCE SMOKES PASSED (no validation errors reported)\n";
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
