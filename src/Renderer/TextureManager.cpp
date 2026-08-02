#include "TextureManager.hpp"
#include "ResourcePackReader.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image/stb_image.h>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
struct DecodedLayer
{
	unsigned char *pixels{nullptr};
	int frameW{0};
	int frameH{0};
	bool ok{false};
};

void nnScale(const unsigned char *src, int srcW, int srcH, uint8_t *dst, uint32_t dstW, uint32_t dstH)
{
	if (!src || srcW <= 0 || srcH <= 0 || !dst || dstW == 0 || dstH == 0)
		return;
	for (uint32_t y = 0; y < dstH; ++y)
	{
		const int sy = static_cast<int>((static_cast<uint64_t>(y) * static_cast<uint32_t>(srcH)) / dstH);
		for (uint32_t x = 0; x < dstW; ++x)
		{
			const int sx = static_cast<int>((static_cast<uint64_t>(x) * static_cast<uint32_t>(srcW)) / dstW);
			const size_t srcIdx =
				(static_cast<size_t>(sy) * static_cast<uint32_t>(srcW) + static_cast<uint32_t>(sx)) * 4;
			const size_t dstIdx = (static_cast<size_t>(y) * dstW + x) * 4;
			std::memcpy(dst + dstIdx, src + srcIdx, 4);
		}
	}
}

struct CpuAtlasBuild
{
	uint32_t layerSize{16};
	TextureAtlasLoadReport report{};
};

/// Decode all block layers into a CPU atlas. Does not touch GPU resources.
CpuAtlasBuild buildCpuAtlas(const std::string &packRoot, std::vector<uint8_t> &atlasOut)
{
	CpuAtlasBuild built{};
	const uint32_t layers = static_cast<uint32_t>(TextureType::COUNT);
	built.report.requiredLayers = static_cast<int>(layers);
	built.report.packRequested = !packRoot.empty();

	// Default pack: ressources/default-resource-pack.zip
	const std::string defaultZipPath = std::string(RES_PATH) + "default-resource-pack.zip";
	ResourcePackReader defaultPack(defaultZipPath);

	ResourcePackReader customPack;
	if (!packRoot.empty())
	{
		customPack.open(packRoot);
	}

	std::vector<DecodedLayer> decoded(layers);
	std::vector<uint8_t> rawPng;
	uint32_t layerSize = 0;

	for (uint32_t i = 0; i < layers; ++i)
	{
		const auto type = static_cast<TextureType>(i);
		const char *name = blockLayerFile(type);
		const char *fallback = blockLayerFallbackFile(type);
		if (!name)
			continue;

		rawPng.clear();
		bool fromCustom = false;

		if (customPack.isValid() && customPack.readBlockTexture(name, rawPng))
		{
			fromCustom = true;
			++built.report.packHits;
		}
		else
		{
			if (built.report.packRequested)
			{
				++built.report.packMisses;
			}
			// Fallback to default pack
			if (defaultPack.isValid())
			{
				if (!defaultPack.readBlockTexture(name, rawPng))
				{
					if (fallback)
						defaultPack.readBlockTexture(fallback, rawPng);
				}
			}
		}

		if (rawPng.empty())
		{
			std::cerr << "Failed to read texture data for: " << name << "\n";
			continue;
		}

		int w = 0, h = 0, ch = 0;
		unsigned char *data = stbi_load_from_memory(
			rawPng.data(), static_cast<int>(rawPng.size()), &w, &h, &ch, STBI_rgb_alpha);
		if (!data || w <= 0 || h <= 0)
		{
			std::cerr << "Failed to decode texture memory for: " << name << "\n";
			if (data)
				stbi_image_free(data);
			continue;
		}

		int frameW = 0, frameH = 0;
		blockTextureFrameSize(w, h, frameW, frameH);

		DecodedLayer &dl = decoded[i];
		dl.pixels = data;
		dl.frameW = frameW;
		dl.frameH = frameH;
		dl.ok = true;
		layerSize = std::max(layerSize, static_cast<uint32_t>(std::max(frameW, frameH)));
	}

	if (built.report.packInvalid())
	{
		std::cerr << "Invalid resource pack '" << packRoot
				  << "': no block textures found. Falling back to default resource pack.\n";
	}
	else if (built.report.packIncomplete())
	{
		std::cerr << "Resource pack incomplete: " << built.report.packMisses << " of " << layers
				  << " block textures missing (default resource pack used for those).\n";
	}

	if (layerSize == 0)
		layerSize = 16;
	built.layerSize = layerSize;

	const size_t layerBytes = static_cast<size_t>(layerSize) * layerSize * 4;
	atlasOut.assign(layerBytes * layers, 255);

	for (uint32_t i = 0; i < layers; ++i)
	{
		DecodedLayer &dl = decoded[i];
		if (!dl.ok || !dl.pixels)
			continue;
		nnScale(dl.pixels, dl.frameW, dl.frameH, atlasOut.data() + static_cast<size_t>(i) * layerBytes,
				layerSize, layerSize);
		stbi_image_free(dl.pixels);
		dl.pixels = nullptr;
	}

	return built;
}
} // namespace

TextureManager::~TextureManager()
{
	shutdown();
}

void TextureManager::shutdown()
{
	if (!m_context)
		return;
	destroyGpuResources();
	m_context = nullptr;
	m_layerSize = 0;
}

void TextureManager::destroyGpuResources()
{
	if (!m_context)
		return;
	if (m_sampler != VK_NULL_HANDLE)
	{
		vkDestroySampler(m_context->getDevice(), m_sampler, nullptr);
		m_sampler = VK_NULL_HANDLE;
	}
	if (m_image.image != VK_NULL_HANDLE)
		destroyImage(m_context->getAllocator(), m_context->getDevice(), m_image);
}

void TextureManager::nearestNeighborScale(const unsigned char *src, int srcW, int srcH,
										  uint8_t *dst, uint32_t dstW, uint32_t dstH)
{
	nnScale(src, srcW, srcH, dst, dstW, dstH);
}

TextureAtlasLoadReport TextureManager::initialize(VkContext &context, ImmediateCommands &imm,
												  const std::string &resourcePackRoot)
{
	const std::string packRoot = trimTrailingSlashes(resourcePackRoot);
	if (!packRoot.empty())
		std::cout << "Resource pack: " << packRoot << "\n";

	// 1) CPU decode — live GPU atlas is not touched yet.
	std::vector<uint8_t> atlas;
	const CpuAtlasBuild cpu = buildCpuAtlas(packRoot, atlas);
	const uint32_t layerSize = cpu.layerSize;
	const uint32_t layers = static_cast<uint32_t>(TextureType::COUNT);
	const VkDeviceSize layerBytes = static_cast<VkDeviceSize>(layerSize) * layerSize * 4;
	const VkDeviceSize totalSize = layerBytes * layers;

	// 2) Build replacement GPU resources into temps.
	AllocatedImage newImage{};
	VkSampler newSampler = VK_NULL_HANDLE;

	auto cleanupNew = [&]() {
		if (newSampler != VK_NULL_HANDLE)
		{
			vkDestroySampler(context.getDevice(), newSampler, nullptr);
			newSampler = VK_NULL_HANDLE;
		}
		if (newImage.image != VK_NULL_HANDLE)
			destroyImage(context.getAllocator(), context.getDevice(), newImage);
	};

	try
	{
		VkImageCreateInfo imageInfo{};
		imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
		imageInfo.extent = {layerSize, layerSize, 1};
		imageInfo.mipLevels = 1;
		imageInfo.arrayLayers = layers;
		imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
		imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

		VmaAllocationCreateInfo allocInfo{};
		allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

		newImage.format = VK_FORMAT_R8G8B8A8_UNORM;
		newImage.width = layerSize;
		newImage.height = layerSize;
		newImage.mipLevels = 1;
		newImage.arrayLayers = layers;

		if (vmaCreateImage(context.getAllocator(), &imageInfo, &allocInfo, &newImage.image, &newImage.allocation,
						   nullptr) != VK_SUCCESS)
			throw std::runtime_error("Failed to create texture array image");

		VkImageViewCreateInfo viewInfo{};
		viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		viewInfo.image = newImage.image;
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
		viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
		viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		viewInfo.subresourceRange.baseMipLevel = 0;
		viewInfo.subresourceRange.levelCount = 1;
		viewInfo.subresourceRange.baseArrayLayer = 0;
		viewInfo.subresourceRange.layerCount = layers;

		if (vkCreateImageView(context.getDevice(), &viewInfo, nullptr, &newImage.view) != VK_SUCCESS)
			throw std::runtime_error("Failed to create texture array view");

		AllocatedBuffer staging = createBuffer(
			context.getAllocator(), totalSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO,
			VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
		writeBuffer(context.getAllocator(), staging, atlas.data(), totalSize);

		imm.submitAndWait([&](VkCommandBuffer cmd) {
			cmdTransitionImageLayout(cmd, newImage.image, VK_IMAGE_LAYOUT_UNDEFINED,
									 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 1, layers);

			std::vector<VkBufferImageCopy> regions(layers);
			for (uint32_t layer = 0; layer < layers; ++layer)
			{
				regions[layer] = {};
				regions[layer].bufferOffset = layerBytes * layer;
				regions[layer].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				regions[layer].imageSubresource.mipLevel = 0;
				regions[layer].imageSubresource.baseArrayLayer = layer;
				regions[layer].imageSubresource.layerCount = 1;
				regions[layer].imageExtent = {layerSize, layerSize, 1};
			}
			vkCmdCopyBufferToImage(cmd, staging.buffer, newImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
								   static_cast<uint32_t>(regions.size()), regions.data());

			cmdTransitionImageLayout(cmd, newImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
									 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 1,
									 layers);
		});

		destroyBuffer(context.getAllocator(), staging);

		VkSamplerCreateInfo samplerInfo{};
		samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		samplerInfo.magFilter = VK_FILTER_NEAREST;
		samplerInfo.minFilter = VK_FILTER_NEAREST;
		samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		samplerInfo.anisotropyEnable = VK_FALSE;
		samplerInfo.maxAnisotropy = 1.0f;
		samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
		samplerInfo.unnormalizedCoordinates = VK_FALSE;
		samplerInfo.compareEnable = VK_FALSE;
		samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		if (vkCreateSampler(context.getDevice(), &samplerInfo, nullptr, &newSampler) != VK_SUCCESS)
			throw std::runtime_error("Failed to create texture sampler");
	}
	catch (...)
	{
		cleanupNew();
		// Live m_image / m_sampler unchanged.
		throw;
	}

	// 3) Commit: destroy previous live atlas only after the new one is fully ready.
	if (m_context)
		destroyGpuResources();

	m_context = &context;
	m_image = newImage;
	m_sampler = newSampler;
	m_layerSize = layerSize;
	m_lastReport = cpu.report;

	std::cout << "Texture array: " << layers << " layers (" << layerSize << "x" << layerSize << ")\n";
	return m_lastReport;
}
