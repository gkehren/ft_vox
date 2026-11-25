#include <Renderer/GameUiRenderer.hpp>

#include <Renderer/ColorSpace.hpp>
#include <Renderer/TextureManager.hpp>
#include <Vulkan/GraphicsPipelineBuilder.hpp>
#include <Vulkan/ImageBarrier.hpp>
#include <Vulkan/VkShader.hpp>

#include <cmath>
#include <cstring>
#include <stdexcept>

namespace gameui
{
namespace
{
constexpr VkDeviceSize kInstanceBytes = sizeof(Quad) * DrawList::kMaxQuads;
constexpr VkColorComponentFlags kRgba = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
	VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

void requireSuccess(VkResult result, const char *message)
{
	if (result != VK_SUCCESS)
		throw std::runtime_error(message);
}

struct ShaderModule
{
	VkDevice device;
	VkShaderModule handle;
	~ShaderModule() { destroyShaderModule(device, handle); }
};

VkPipeline createPipeline(VkDevice device, VkFormat format, VkPipelineLayout layout, bool encode)
{
	const ShaderModule vert{device, loadShaderModule(device, resolveSpvPath(
		encode ? "game_ui_encode.vert.spv" : "game_ui.vert.spv"))};
	const ShaderModule frag{device, loadShaderModule(device, resolveSpvPath(
		encode ? "game_ui_encode.frag.spv" : "game_ui.frag.spv"))};
	const VkPipelineShaderStageCreateInfo stages[] = {
		{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,
			vert.handle, "main", nullptr},
		{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
			frag.handle, "main", nullptr},
	};
	const VkVertexInputBindingDescription binding{0, sizeof(Quad), VK_VERTEX_INPUT_RATE_INSTANCE};
	const VkVertexInputAttributeDescription attrs[] = {
		{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Quad, rect)},
		{1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Quad, uv)},
		{2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Quad, color)},
		{3, 0, VK_FORMAT_R32_SINT, offsetof(Quad, layer)},
	};
	VkPipelineVertexInputStateCreateInfo input{};
	input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	if (!encode)
	{
		input.vertexBindingDescriptionCount = 1;
		input.pVertexBindingDescriptions = &binding;
		input.vertexAttributeDescriptionCount = std::size(attrs);
		input.pVertexAttributeDescriptions = attrs;
	}

	VkPipelineRasterizationStateCreateInfo raster{};
	raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.cullMode = VK_CULL_MODE_NONE;
	raster.lineWidth = 1.f;
	VkPipelineColorBlendAttachmentState blend{};
	blend.colorWriteMask = kRgba;
	blend.blendEnable = !encode;
	blend.srcColorBlendFactor = blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	blend.dstColorBlendFactor = blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	blend.colorBlendOp = blend.alphaBlendOp = VK_BLEND_OP_ADD;
	return GraphicsPipelineBuilder().setLayout(layout).setShaderStages(stages, 2)
		.setVertexInput(&input).setRaster(raster).setColorBlendAttachments(&blend, 1)
		.setRenderingFormats(&format, 1).build(device);
}

void bindImage(VkDevice device, VkDescriptorSet set, uint32_t binding, VkSampler sampler, VkImageView view)
{
	const VkDescriptorImageInfo info{sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
	VkWriteDescriptorSet write{};
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = set;
	write.dstBinding = binding;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo = &info;
	vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
}

void beginOverlay(VkCommandBuffer cmd, VkImageView view, VkExtent2D extent, VkAttachmentLoadOp load)
{
	VkRenderingAttachmentInfo color{};
	color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
	color.imageView = view;
	color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	color.loadOp = load;
	color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	VkRenderingInfo rendering{};
	rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
	rendering.renderArea = {{0, 0}, extent};
	rendering.layerCount = 1;
	rendering.colorAttachmentCount = 1;
	rendering.pColorAttachments = &color;
	const auto begin = vkCmdBeginRendering ? vkCmdBeginRendering : vkCmdBeginRenderingKHR;
	begin(cmd, &rendering);
	const VkViewport viewport{0.f, 0.f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.f, 1.f};
	vkCmdSetViewport(cmd, 0, 1, &viewport);
}

void endOverlay(VkCommandBuffer cmd)
{
	const auto end = vkCmdEndRendering ? vkCmdEndRendering : vkCmdEndRenderingKHR;
	end(cmd);
}

VkDeviceSize allocationBytes(VmaAllocator allocator, const AllocatedImage &image)
{
	if (!image.allocation)
		return 0;
	VmaAllocationInfo info{};
	vmaGetAllocationInfo(allocator, image.allocation, &info);
	return info.size;
}
} // namespace

Renderer::~Renderer()
{
	if (!m_context)
		return;
	const auto device = m_context->getDevice();
	const auto allocator = m_context->getAllocator();
	vkDestroyPipeline(device, m_drawPipe, nullptr);
	vkDestroyPipeline(device, m_encodePipe, nullptr);
	vkDestroyPipelineLayout(device, m_drawLayout, nullptr);
	vkDestroyPipelineLayout(device, m_encodeLayout, nullptr);
	vkDestroyDescriptorPool(device, m_pool, nullptr);
	vkDestroyDescriptorSetLayout(device, m_imageLayout, nullptr);
	vkDestroyDescriptorSetLayout(device, m_encodeSetLayout, nullptr);
	for (auto &slot : m_slots)
	{
		destroyBuffer(allocator, slot.instances);
		destroyImage(allocator, device, slot.linear);
	}
	vkDestroySampler(device, m_fontSampler, nullptr);
	destroyImage(allocator, device, m_font);
}

void Renderer::init(VkContext &context, ImmediateCommands &immediate, VkExtent2D fontExtent,
					std::span<const uint8_t> coverage, VkFormat format, VkColorSpaceKHR colorSpace)
{
	const uint64_t bytes = uint64_t(fontExtent.width) * fontExtent.height;
	if (bytes == 0 || bytes > 8u * 1024 * 1024 || coverage.size() != bytes)
		throw std::invalid_argument("Game UI font atlas has invalid dimensions");
	m_context = &context;
	const auto device = context.getDevice();
	const auto allocator = context.getAllocator();
	m_font = createImage2D(allocator, device, fontExtent.width, fontExtent.height, VK_FORMAT_R8_UNORM,
		VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
	uploadImage2D(allocator, immediate, m_font, coverage.data(), bytes);
	VkSamplerCreateInfo sampler{};
	sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler.minFilter = sampler.magFilter = VK_FILTER_LINEAR;
	sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	requireSuccess(vkCreateSampler(device, &sampler, nullptr, &m_fontSampler), "Game UI font sampler");
	createDescriptors();
	for (auto &slot : m_slots)
	{
		slot.instances = createBuffer(allocator, kInstanceBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
			VMA_MEMORY_USAGE_AUTO, VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
			VMA_ALLOCATION_CREATE_MAPPED_BIT);
		bindImage(device, slot.images, 0, m_fontSampler, m_font.view);
	}
	setOutput(format, colorSpace);
}

bool Renderer::setFont(ImmediateCommands &immediate, VkExtent2D extent,
				   std::span<const uint8_t> coverage)
{
	const uint64_t bytes = uint64_t(extent.width) * uint64_t(extent.height);
	if (!m_context || extent.width == 0 || extent.height == 0 || bytes > 8u * 1024 * 1024 ||
		coverage.size() != bytes)
		return false;
	const auto device = m_context->getDevice();
	const auto allocator = m_context->getAllocator();
	// Prepare the replacement completely before touching anything the frames in
	// flight can see (R07). A failure here leaves the current atlas untouched.
	AllocatedImage next{};
	try
	{
		next = createImage2D(allocator, device, extent.width, extent.height, VK_FORMAT_R8_UNORM,
			VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
		if (!next.image || !next.view)
		{
			destroyImage(allocator, device, next);
			return false;
		}
		uploadImage2D(allocator, immediate, next, coverage.data(), bytes);
	}
	catch (const std::exception &)
	{
		destroyImage(allocator, device, next);
		return false;
	}
	// Safe boundary: every queued sample of the current atlas has completed.
	m_context->waitIdle();
	for (auto &slot : m_slots)
		bindImage(device, slot.images, 0, m_fontSampler, next.view);
	destroyImage(allocator, device, m_font);
	m_font = next;
	return true;
}

void Renderer::createDescriptors()
{
	const auto device = m_context->getDevice();
	const VkDescriptorSetLayoutBinding bindings[] = {
		{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
		{1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
	};
	VkDescriptorSetLayoutCreateInfo setLayout{};
	setLayout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	setLayout.pBindings = bindings;
	setLayout.bindingCount = 2;
	requireSuccess(vkCreateDescriptorSetLayout(device, &setLayout, nullptr, &m_imageLayout), "Game UI texture layout");
	setLayout.bindingCount = 1;
	requireSuccess(vkCreateDescriptorSetLayout(device, &setLayout, nullptr, &m_encodeSetLayout), "Game UI encode set layout");

	constexpr uint32_t frames = VkFrameContext::kMaxFramesInFlight;
	const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 * frames};
	VkDescriptorPoolCreateInfo pool{};
	pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool.maxSets = 2 * frames;
	pool.poolSizeCount = 1;
	pool.pPoolSizes = &size;
	requireSuccess(vkCreateDescriptorPool(device, &pool, nullptr, &m_pool), "Game UI descriptor pool");
	for (auto &slot : m_slots)
	{
		const VkDescriptorSetLayout layouts[] = {m_imageLayout, m_encodeSetLayout};
		VkDescriptorSet sets[2]{};
		VkDescriptorSetAllocateInfo alloc{};
		alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		alloc.descriptorPool = m_pool;
		alloc.descriptorSetCount = 2;
		alloc.pSetLayouts = layouts;
		requireSuccess(vkAllocateDescriptorSets(device, &alloc, sets), "Game UI slot descriptor sets");
		slot.images = sets[0];
		slot.encode = sets[1];
	}
	const VkPushConstantRange ndc{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::vec2)};
	VkPipelineLayoutCreateInfo layout{};
	layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout.setLayoutCount = 1;
	layout.pSetLayouts = &m_imageLayout;
	layout.pushConstantRangeCount = 1;
	layout.pPushConstantRanges = &ndc;
	requireSuccess(vkCreatePipelineLayout(device, &layout, nullptr, &m_drawLayout), "Game UI draw layout");
	layout.pSetLayouts = &m_encodeSetLayout;
	layout.pushConstantRangeCount = 0;
	layout.pPushConstantRanges = nullptr;
	requireSuccess(vkCreatePipelineLayout(device, &layout, nullptr, &m_encodeLayout), "Game UI encode layout");
}

void Renderer::setOutput(VkFormat format, VkColorSpaceKHR colorSpace)
{
	const auto output = colorspace::classifyOutputTransfer(format, colorSpace);
	if (output == colorspace::OutputTransfer::Unsupported)
		throw std::runtime_error("Game UI requires an SDR sRGB swapchain format");
	if (m_outputFormat == format)
		return;

	const auto device = m_context->getDevice();
	const bool linear = output == colorspace::OutputTransfer::ShaderSrgb;
	if (linear)
	{
		VkFormatProperties props{};
		vkGetPhysicalDeviceFormatProperties(m_context->getPhysicalDevice(), kLinearTargetFormat, &props);
		constexpr VkFormatFeatureFlags required = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
			VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
		if ((props.optimalTilingFeatures & required) != required)
			throw std::runtime_error("Game UI linear composition requires sampled, blendable RGBA16F");
	}
	const auto draw = createPipeline(device, linear ? kLinearTargetFormat : format, m_drawLayout, false);
	VkPipeline encode{};
	try
	{
		if (linear)
			encode = createPipeline(device, format, m_encodeLayout, true);
	}
	catch (...)
	{
		vkDestroyPipeline(device, draw, nullptr);
		throw;
	}
	vkDestroyPipeline(device, m_drawPipe, nullptr);
	vkDestroyPipeline(device, m_encodePipe, nullptr);
	m_drawPipe = draw;
	m_encodePipe = encode;
	m_outputFormat = format;
	m_linear = linear;
	for (auto &slot : m_slots)
	{
		slot.batchCount = slot.instancesUsed = 0;
		if (!linear)
			destroyImage(m_context->getAllocator(), device, slot.linear);
	}
}

void Renderer::ensureLinearTarget(Slot &slot, VkExtent2D extent)
{
	if (slot.linear.width == extent.width && slot.linear.height == extent.height)
		return;
	const auto device = m_context->getDevice();
	const auto allocator = m_context->getAllocator();
	auto replacement = createImage2D(allocator, device, extent.width, extent.height, kLinearTargetFormat,
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
	bindImage(device, slot.encode, 0, m_fontSampler, replacement.view);
	// The previous generation can only have been used by this slot.
	destroyImage(allocator, device, slot.linear);
	slot.linear = replacement;
}

void Renderer::prepare(uint32_t index, VkExtent2D extent, const DrawList &list,
					   const TextureManager &atlas, uint64_t currentGeneration)
{
	Slot &slot = m_slots[index];
	slot.batchCount = slot.instancesUsed = 0;
	slot.dropped = list.dropped();
	if (list.quads().empty() || extent.width == 0 || extent.height == 0 ||
		list.atlasGeneration() != currentGeneration || !atlas.isValid())
		return;

	const glm::vec2 pixels{extent.width, extent.height};
	const auto pixelsPerLogical = framebufferScale(list.size(), pixels);
	for (const auto &batch : list.batches())
	{
		const auto clip = pixelClip(batch.clip, pixelsPerLogical, pixels).rect;
		const auto left = static_cast<int32_t>(clip.x);
		const auto top = static_cast<int32_t>(clip.y);
		const auto right = static_cast<int32_t>(clip.z);
		const auto bottom = static_cast<int32_t>(clip.w);
		if (left < right && top < bottom)
		{
			const VkRect2D scissor{{left, top}, {uint32_t(right - left), uint32_t(bottom - top)}};
			slot.batches[slot.batchCount++] = {scissor, batch.first, batch.count};
			slot.instancesUsed += batch.count;
		}
	}
	if (slot.batchCount == 0)
		return;
	if (!slot.atlasBound || slot.atlasGeneration != currentGeneration)
	{
		bindImage(m_context->getDevice(), slot.images, 1, atlas.getSampler(), atlas.getImageView());
		slot.atlasBound = true;
		slot.atlasGeneration = currentGeneration;
	}
	if (m_linear)
		ensureLinearTarget(slot, extent);
	slot.ndc = 2.f / list.size();
	const auto bytes = list.quads().size_bytes();
	std::memcpy(slot.instances.info.pMappedData, list.quads().data(), bytes);
	requireSuccess(vmaFlushAllocation(m_context->getAllocator(), slot.instances.allocation, 0, bytes),
		"Game UI instance flush");
}

const AllocatedImage *Renderer::linearTarget(uint32_t index) const
{
	return m_linear && hasDraws(index) ? &m_slots[index].linear : nullptr;
}

void Renderer::record(VkCommandBuffer cmd, uint32_t index, VkExtent2D extent, VkImage output,
					  VkImageView outputView, VkGpuProfiler *gpu)
{
	if (!hasDraws(index))
		return;
	const Slot &slot = m_slots[index];
	const auto target = m_linear ? slot.linear.image : output;
	vkbar::cmdTransitionColor(cmd, target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
	if (gpu) gpu->beginPass(cmd, GpuPass::GameUi);
	beginOverlay(cmd, m_linear ? slot.linear.view : outputView, extent, VK_ATTACHMENT_LOAD_OP_LOAD);
	const VkDeviceSize offset = 0;
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_drawPipe);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_drawLayout, 0, 1, &slot.images, 0, nullptr);
	vkCmdBindVertexBuffers(cmd, 0, 1, &slot.instances.buffer, &offset);
	vkCmdPushConstants(cmd, m_drawLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(slot.ndc), &slot.ndc);
	for (const auto &batch : std::span(slot.batches.data(), slot.batchCount))
	{
		vkCmdSetScissor(cmd, 0, 1, &batch.scissor);
		vkCmdDraw(cmd, 6, batch.count, 0, batch.first);
	}
	endOverlay(cmd);
	if (gpu) gpu->endPass(cmd, GpuPass::GameUi);

	if (m_linear)
	{
		if (gpu) gpu->beginPass(cmd, GpuPass::GameUiEncode);
		vkbar::cmdTransitionColor(cmd, target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
		// Chain the first swapchain layout transition to the WSI acquire wait.
		vkbar::cmdTransitionColor(cmd, output, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
		beginOverlay(cmd, outputView, extent, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
		const VkRect2D full{{0, 0}, extent};
		vkCmdSetScissor(cmd, 0, 1, &full);
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_encodePipe);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_encodeLayout, 0, 1, &slot.encode, 0, nullptr);
		vkCmdDraw(cmd, 3, 1, 0, 0);
		endOverlay(cmd);
		if (gpu) gpu->endPass(cmd, GpuPass::GameUiEncode);
	}
}

RenderStats Renderer::stats(uint32_t index) const
{
	const Slot &slot = m_slots[index];
	const uint32_t encode = m_linear && slot.batchCount ? 1u : 0u;
	RenderStats out{slot.instancesUsed * 6 + encode * 3, slot.batchCount + encode, slot.dropped,
		2 * VkFrameContext::kMaxFramesInFlight, kInstanceBytes * VkFrameContext::kMaxFramesInFlight};
	const auto allocator = m_context->getAllocator();
	out.fontBytes = allocationBytes(allocator, m_font);
	for (const auto &entry : m_slots)
		out.compositionBytes += allocationBytes(allocator, entry.linear);
	return out;
}
} // namespace gameui
