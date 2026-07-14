#include "Vulkan/VkResourceSmoke.hpp"

#include "Vulkan/VkBuffer.hpp"
#include "Vulkan/VkCommands.hpp"
#include "Vulkan/VkImage.hpp"
#include "Vulkan/VkShader.hpp"
#include "Vulkan/VkUpload.hpp"

#include <array>
#include <cstring>
#include <iostream>
#include <vector>

bool runVulkanResourceSmoke(VkContext &context,
							const std::string &spvVertPath,
							const std::string &spvFragPath)
{
	try
	{
		VmaAllocator allocator = context.getAllocator();
		VkDevice device = context.getDevice();

		ImmediateCommands imm;
		imm.init(context);

		// --- Buffer: GPU-only + staging upload ---
		const std::array<float, 8> pattern = {1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f, 8.f};
		const VkDeviceSize bufSize = sizeof(pattern);

		AllocatedBuffer gpuBuf = createBuffer(
			allocator,
			bufSize,
			VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
			VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);

		uploadBuffer(allocator, imm, gpuBuf, pattern.data(), bufSize);
		std::cout << "PASS: buffer create + staging upload (" << bufSize << " bytes)\n";

		// --- Host-visible staging buffer write/read round-trip ---
		AllocatedBuffer hostBuf = createBuffer(
			allocator,
			bufSize,
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VMA_MEMORY_USAGE_AUTO,
			VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
		writeBuffer(allocator, hostBuf, pattern.data(), bufSize);
		{
			void *mapped = hostBuf.info.pMappedData ? hostBuf.info.pMappedData : mapBuffer(allocator, hostBuf);
			if (std::memcmp(mapped, pattern.data(), static_cast<size_t>(bufSize)) != 0)
				throw std::runtime_error("host buffer round-trip mismatch");
			if (!hostBuf.info.pMappedData)
				unmapBuffer(allocator, hostBuf);
		}
		std::cout << "PASS: host-visible buffer write/read\n";

		// --- Image: 4x4 RGBA8 + staging upload ---
		constexpr uint32_t kW = 4, kH = 4;
		std::vector<uint8_t> pixels(kW * kH * 4, 0);
		for (uint32_t y = 0; y < kH; ++y)
		{
			for (uint32_t x = 0; x < kW; ++x)
			{
				const size_t i = (static_cast<size_t>(y) * kW + x) * 4;
				pixels[i + 0] = static_cast<uint8_t>(x * 60);
				pixels[i + 1] = static_cast<uint8_t>(y * 60);
				pixels[i + 2] = 128;
				pixels[i + 3] = 255;
			}
		}

		AllocatedImage image = createImage2D(
			allocator,
			device,
			kW,
			kH,
			VK_FORMAT_R8G8B8A8_UNORM,
			VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);

		uploadImage2D(allocator, imm, image, pixels.data(), pixels.size());
		std::cout << "PASS: image create + staging upload (" << kW << "x" << kH << ")\n";

		// --- SPIR-V modules ---
		VkShaderModule vert = loadShaderModule(device, spvVertPath);
		VkShaderModule frag = loadShaderModule(device, spvFragPath);
		std::cout << "PASS: SPIR-V modules loaded\n"
				  << "  vert=" << spvVertPath << "\n"
				  << "  frag=" << spvFragPath << "\n";

		destroyShaderModule(device, frag);
		destroyShaderModule(device, vert);
		std::cout << "PASS: SPIR-V modules destroyed\n";

		destroyImage(allocator, device, image);
		destroyBuffer(allocator, hostBuf);
		destroyBuffer(allocator, gpuBuf);
		std::cout << "PASS: buffer/image destroyed via VMA\n";

		imm.shutdown();
		context.waitIdle();
		std::cout << "PASS: Vulkan resource smoke complete\n";
		return true;
	}
	catch (const std::exception &e)
	{
		std::cerr << "FAIL: Vulkan resource smoke: " << e.what() << "\n";
		return false;
	}
}
