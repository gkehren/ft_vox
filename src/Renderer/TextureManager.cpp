#include "TextureManager.hpp"

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
	unsigned char *pixels{nullptr}; // stbi-owned until free
	int imageW{0};
	int imageH{0};
	int frameW{0};
	int frameH{0};
	bool ok{false};
};
} // namespace

TextureManager::~TextureManager()
{
	shutdown();
}

void TextureManager::shutdown()
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
	m_context = nullptr;
	m_layerSize = 0;
}

void TextureManager::nearestNeighborScale(const unsigned char *src, int srcW, int srcH,
										  uint8_t *dst, uint32_t dstW, uint32_t dstH)
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

void TextureManager::initialize(VkContext &context, ImmediateCommands &imm,
								const std::string &resourcePackRoot)
{
	shutdown();
	m_context = &context;

	const std::string packRoot = trimTrailingSlashes(resourcePackRoot);
	if (!packRoot.empty())
		std::cout << "Resource pack: " << packRoot << "\n";

	const uint32_t layers = static_cast<uint32_t>(TextureType::COUNT);
	std::vector<DecodedLayer> decoded(layers);

	// Single decode pass: resolve path, load once, derive frame size from pixels.
	uint32_t layerSize = 0;
	int packMisses = 0;
	for (uint32_t i = 0; i < layers; ++i)
	{
		const auto type = static_cast<TextureType>(i);
		const char *name = blockLayerFile(type);
		if (!name)
			continue;

		bool fellBack = false;
		const std::string path = resolveBlockTexturePath(packRoot, name, &fellBack);
		if (fellBack)
		{
			++packMisses;
			std::cerr << "Resource pack missing block texture '" << name
					  << "', falling back to bundled: " << path << "\n";
		}

		int w = 0, h = 0, ch = 0;
		unsigned char *data = stbi_load(path.c_str(), &w, &h, &ch, STBI_rgb_alpha);
		if (!data || w <= 0 || h <= 0)
		{
			std::cerr << "Failed to load texture: " << path << "\n";
			if (data)
				stbi_image_free(data);
			continue;
		}

		int frameW = 0, frameH = 0;
		blockTextureFrameSize(w, h, frameW, frameH);

		DecodedLayer &dl = decoded[i];
		dl.pixels = data;
		dl.imageW = w;
		dl.imageH = h;
		dl.frameW = frameW;
		dl.frameH = frameH;
		dl.ok = true;

		const int edge = std::max(frameW, frameH);
		layerSize = std::max(layerSize, static_cast<uint32_t>(edge));
	}

	if (!packRoot.empty() && packMisses > 0)
		std::cerr << "Resource pack incomplete: " << packMisses << " of " << layers
				  << " block textures missing from pack (used bundled fallback).\n";

	if (layerSize == 0)
		layerSize = 16;
	m_layerSize = layerSize;

	const VkDeviceSize layerBytes = static_cast<VkDeviceSize>(layerSize) * layerSize * 4;
	std::vector<uint8_t> atlas(static_cast<size_t>(layerBytes) * layers, 255);

	for (uint32_t i = 0; i < layers; ++i)
	{
		DecodedLayer &dl = decoded[i];
		if (!dl.ok || !dl.pixels)
			continue;
		// Scale first frame only: for strips, frameH == frameW and NN samples top rows of full buffer.
		const size_t layerOffset = static_cast<size_t>(i) * layerSize * layerSize * 4;
		nearestNeighborScale(dl.pixels, dl.frameW, dl.frameH, atlas.data() + layerOffset, layerSize,
							 layerSize);
		stbi_image_free(dl.pixels);
		dl.pixels = nullptr;
	}

	// Create 2D array image
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

	m_image.format = VK_FORMAT_R8G8B8A8_UNORM;
	m_image.width = layerSize;
	m_image.height = layerSize;
	m_image.mipLevels = 1;
	m_image.arrayLayers = layers;

	if (vmaCreateImage(context.getAllocator(), &imageInfo, &allocInfo, &m_image.image, &m_image.allocation,
					   nullptr) != VK_SUCCESS)
		throw std::runtime_error("Failed to create texture array image");

	VkImageViewCreateInfo viewInfo{};
	viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image = m_image.image;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
	viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
	viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	viewInfo.subresourceRange.baseMipLevel = 0;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.baseArrayLayer = 0;
	viewInfo.subresourceRange.layerCount = layers;

	if (vkCreateImageView(context.getDevice(), &viewInfo, nullptr, &m_image.view) != VK_SUCCESS)
		throw std::runtime_error("Failed to create texture array view");

	const VkDeviceSize totalSize = layerBytes * layers;
	AllocatedBuffer staging = createBuffer(
		context.getAllocator(), totalSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO,
		VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
	writeBuffer(context.getAllocator(), staging, atlas.data(), totalSize);

	imm.submitAndWait([&](VkCommandBuffer cmd) {
		cmdTransitionImageLayout(cmd, m_image.image, VK_IMAGE_LAYOUT_UNDEFINED,
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
		vkCmdCopyBufferToImage(cmd, staging.buffer, m_image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
							   static_cast<uint32_t>(regions.size()), regions.data());

		cmdTransitionImageLayout(cmd, m_image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
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
	if (vkCreateSampler(context.getDevice(), &samplerInfo, nullptr, &m_sampler) != VK_SUCCESS)
		throw std::runtime_error("Failed to create texture sampler");

	std::cout << "Texture array: " << layers << " layers (" << layerSize << "x" << layerSize << ")\n";
}
