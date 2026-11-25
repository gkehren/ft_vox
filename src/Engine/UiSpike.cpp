#include <Engine/UiSpike.hpp>

#include <SDL3/SDL.h>
#include <Vulkan/GraphicsPipelineBuilder.hpp>
#include <Vulkan/VkContext.hpp>
#include <Vulkan/VkShader.hpp>
#include <imgui/imgui.h>
#include <glm/vec2.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string_view>

namespace
{
constexpr uint32_t kQuadCapacity = 192;
constexpr float kPanelWidth = 400.f;
constexpr float kPanelHeight = 240.f;
constexpr glm::vec4 kInk{0.94f, 0.97f, 1.f, 1.f};
constexpr glm::vec4 kMuted{0.63f, 0.73f, 0.81f, 1.f};
constexpr glm::vec4 kAccent{0.31f, 0.89f, 0.76f, 1.f};

struct Layout
{
	glm::vec2 size;
	glm::vec2 origin;
	float scale;

	glm::vec4 rect(glm::vec4 local) const
	{
		return local * scale + glm::vec4(origin, origin);
	}

	glm::vec4 button() const { return rect({24, 143, 220, 185}); }
};

Layout layoutFor(SDL_Window *window)
{
	int w = 0, h = 0;
	SDL_GetWindowSize(window, &w, &h);
	const glm::vec2 size{static_cast<float>(w), static_cast<float>(h)};
	const float density = SDL_GetWindowPixelDensity(window);
	const float display = SDL_GetWindowDisplayScale(window);
	const float logicalScale = density > 0.f && display > 0.f ? display / density : 1.f;
	const float scale = std::min({logicalScale, size.x / (kPanelWidth + 24.f),
		size.y / (kPanelHeight + 24.f)});
	const glm::vec2 origin = (size - scale * glm::vec2(kPanelWidth, kPanelHeight)) * 0.5f;
	return {size, origin, scale};
}

bool contains(glm::vec4 rect, float x, float y)
{
	return x >= rect.x && x < rect.z && y >= rect.y && y < rect.w;
}

struct Quad
{
	glm::vec4 rect;
	glm::vec4 uv;
	glm::vec4 color;
};

struct ShaderModule
{
	VkDevice device;
	VkShaderModule handle;

	~ShaderModule() { destroyShaderModule(device, handle); }
};

void requireSuccess(VkResult result, const char *message)
{
	if (result != VK_SUCCESS)
		throw std::runtime_error(message);
}
} // namespace

UiSpike::~UiSpike()
{
	if (!m_device)
		return;
	for (auto &buffer : m_instances)
		destroyBuffer(m_allocator, buffer);
	vkDestroyPipeline(m_device, m_pipeline, nullptr);
	vkDestroyPipelineLayout(m_device, m_layout, nullptr);
	vkDestroyDescriptorPool(m_device, m_pool, nullptr);
	vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr);
	vkDestroySampler(m_device, m_sampler, nullptr);
	destroyImage(m_allocator, m_device, m_atlas);
}

void UiSpike::init(SDL_Window *window, VkContext &context, ImmediateCommands &immediate, VkFormat target)
{
	m_windowFlags = SDL_GetWindowFlags(window);
	m_device = context.getDevice();
	m_allocator = context.getAllocator();
	createAtlas(immediate);

	const VkDescriptorSetLayoutBinding atlasBinding{
		0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
	VkDescriptorSetLayoutCreateInfo setLayout{};
	setLayout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	setLayout.bindingCount = 1;
	setLayout.pBindings = &atlasBinding;
	requireSuccess(vkCreateDescriptorSetLayout(m_device, &setLayout, nullptr, &m_setLayout),
		"UI spike descriptor layout creation failed");

	const VkDescriptorPoolSize atlasSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
	VkDescriptorPoolCreateInfo pool{};
	pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool.maxSets = 1;
	pool.poolSizeCount = 1;
	pool.pPoolSizes = &atlasSize;
	requireSuccess(vkCreateDescriptorPool(m_device, &pool, nullptr, &m_pool),
		"UI spike descriptor pool creation failed");

	VkDescriptorSetAllocateInfo alloc{};
	alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	alloc.descriptorPool = m_pool;
	alloc.descriptorSetCount = 1;
	alloc.pSetLayouts = &m_setLayout;
	requireSuccess(vkAllocateDescriptorSets(m_device, &alloc, &m_atlasSet),
		"UI spike descriptor allocation failed");

	VkSamplerCreateInfo sampler{};
	sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler.magFilter = VK_FILTER_NEAREST;
	sampler.minFilter = VK_FILTER_NEAREST;
	sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	requireSuccess(vkCreateSampler(m_device, &sampler, nullptr, &m_sampler),
		"UI spike sampler creation failed");

	const VkDescriptorImageInfo atlasInfo{m_sampler, m_atlas.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
	VkWriteDescriptorSet write{};
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = m_atlasSet;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo = &atlasInfo;
	vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);

	const VkPushConstantRange view{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::vec2)};
	VkPipelineLayoutCreateInfo layout{};
	layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout.setLayoutCount = 1;
	layout.pSetLayouts = &m_setLayout;
	layout.pushConstantRangeCount = 1;
	layout.pPushConstantRanges = &view;
	requireSuccess(vkCreatePipelineLayout(m_device, &layout, nullptr, &m_layout),
		"UI spike pipeline layout creation failed");
	setTargetFormat(target);

	for (auto &buffer : m_instances)
	{
		buffer = createBuffer(m_allocator, kQuadCapacity * sizeof(Quad),
			VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO,
			VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
	}
}

void UiSpike::createAtlas(ImmediateCommands &immediate)
{
	ImFontAtlas atlas;
	atlas.Flags = ImFontAtlasFlags_NoMouseCursors | ImFontAtlasFlags_NoBakedLines;
	ImFont *font = atlas.AddFontDefault();
	const int sampleIndex = atlas.AddCustomRectRegular(16, 16);
	unsigned char *pixels = nullptr;
	int width = 0, height = 0;
	atlas.GetTexDataAsRGBA32(&pixels, &width, &height);
	const ImFontAtlasCustomRect &sample = *atlas.GetCustomRectByIndex(sampleIndex);
	if (!pixels || !sample.IsPacked())
		throw std::runtime_error("UI spike atlas bake failed");

	for (int y = 0; y < sample.Height; ++y)
	{
		for (int x = 0; x < sample.Width; ++x)
		{
			const bool light = (x / 4 + y / 4) % 2 == 0;
			unsigned char *pixel = pixels + 4 * ((sample.Y + y) * width + sample.X + x);
			pixel[0] = light ? 78 : 23;
			pixel[1] = light ? 222 : 89;
			pixel[2] = light ? 194 : 118;
			pixel[3] = 255;
		}
	}
	const ImVec2 white = atlas.TexUvWhitePixel;
	m_whiteUv = {white.x, white.y, white.x, white.y};
	const glm::vec4 texSize{width, height, width, height};
	m_sampleUv = glm::vec4(sample.X, sample.Y, sample.X + sample.Width, sample.Y + sample.Height)
		/ texSize;
	for (ImWchar c = 32; c < m_glyphs.size(); ++c)
	{
		const ImFontGlyph &g = *font->FindGlyph(c);
		m_glyphs[c] = {{g.X0, g.Y0, g.X1, g.Y1}, {g.U0, g.V0, g.U1, g.V1},
			g.AdvanceX, g.Visible != 0};
	}

	// Keep the sRGB-authored bytes intact for the opaque UNORM fallback.
	m_atlas = createImage2D(m_allocator, m_device, static_cast<uint32_t>(width),
		static_cast<uint32_t>(height), VK_FORMAT_R8G8B8A8_UNORM,
		VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
	uploadImage2D(m_allocator, immediate, m_atlas, pixels, VkDeviceSize(width) * height * 4);
}

void UiSpike::setTargetFormat(VkFormat target)
{
	if (m_target == target)
		return;

	const ShaderModule vert{m_device, loadShaderModule(m_device, resolveSpvPath("ui_spike.vert.spv"))};
	const ShaderModule frag{m_device, loadShaderModule(m_device, resolveSpvPath("ui_spike.frag.spv"))};
	const VkBool32 srgb = target == VK_FORMAT_B8G8R8A8_SRGB || target == VK_FORMAT_R8G8B8A8_SRGB;
	const VkSpecializationMapEntry map{0, 0, sizeof(srgb)};
	const VkSpecializationInfo transfer{1, &map, sizeof(srgb), &srgb};
	const VkPipelineShaderStageCreateInfo stages[] = {
		{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,
			vert.handle, "main", nullptr},
		{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
			frag.handle, "main", &transfer},
	};
	const VkVertexInputBindingDescription binding{0, sizeof(Quad), VK_VERTEX_INPUT_RATE_INSTANCE};
	const VkVertexInputAttributeDescription attributes[] = {
		{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Quad, rect)},
		{1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Quad, uv)},
		{2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Quad, color)},
	};
	VkPipelineVertexInputStateCreateInfo input{};
	input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	input.vertexBindingDescriptionCount = 1;
	input.pVertexBindingDescriptions = &binding;
	input.vertexAttributeDescriptionCount = 3;
	input.pVertexAttributeDescriptions = attributes;

	VkPipelineRasterizationStateCreateInfo raster{};
	raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.cullMode = VK_CULL_MODE_NONE;
	raster.lineWidth = 1.f;
	VkPipelineColorBlendAttachmentState color{};
	color.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
		VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	const VkPipeline replacement = GraphicsPipelineBuilder()
		.setLayout(m_layout)
		.setShaderStages(stages, 2)
		.setVertexInput(&input)
		.setRaster(raster)
		.setColorBlendAttachments(&color, 1)
		.setRenderingFormats(&target, 1)
		.build(m_device);
	vkDestroyPipeline(m_device, m_pipeline, nullptr);
	m_pipeline = replacement;
	m_target = target;
}

bool UiSpike::windowInteractive() const
{
	constexpr SDL_WindowFlags mask = SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED;
	return (m_windowFlags & mask) == SDL_WINDOW_INPUT_FOCUS;
}

void UiSpike::handleWindowEvent(Uint32 type)
{
	switch (type)
	{
	case SDL_EVENT_WINDOW_FOCUS_GAINED:
		m_windowFlags |= SDL_WINDOW_INPUT_FOCUS;
		break;
	case SDL_EVENT_WINDOW_FOCUS_LOST:
		m_windowFlags &= ~SDL_WINDOW_INPUT_FOCUS;
		break;
	case SDL_EVENT_WINDOW_HIDDEN:
		m_windowFlags |= SDL_WINDOW_HIDDEN;
		m_windowFlags &= ~SDL_WINDOW_INPUT_FOCUS;
		break;
	case SDL_EVENT_WINDOW_SHOWN:
		m_windowFlags &= ~SDL_WINDOW_HIDDEN;
		break;
	case SDL_EVENT_WINDOW_MINIMIZED:
		m_windowFlags |= SDL_WINDOW_MINIMIZED;
		break;
	case SDL_EVENT_WINDOW_RESTORED:
	case SDL_EVENT_WINDOW_MAXIMIZED:
		m_windowFlags &= ~SDL_WINDOW_MINIMIZED;
		break;
	case SDL_EVENT_WINDOW_RESIZED:
	case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
	case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
		break;
	default:
		return;
	}
	m_armed = false;
	if (!windowInteractive())
		m_buttonFocused = false;
}

UiSpike::InputResult UiSpike::handleEvent(SDL_Window *window, const SDL_Event &event)
{
	const SDL_WindowID id = SDL_GetWindowID(window);
	if (event.type >= SDL_EVENT_WINDOW_FIRST && event.type <= SDL_EVENT_WINDOW_LAST)
	{
		if (event.window.windowID == id)
			handleWindowEvent(event.type);
		return InputResult::Unhandled;
	}

	switch (event.type)
	{
	case SDL_EVENT_KEY_DOWN:
		if (event.key.windowID == id && windowInteractive() && !event.key.repeat)
		{
			if (event.key.key == SDLK_ESCAPE)
				return InputResult::Exit;
			if (event.key.key == SDLK_TAB)
				m_buttonFocused = true;
			else if (m_buttonFocused && (event.key.key == SDLK_RETURN || event.key.key == SDLK_SPACE))
				activate("keyboard");
		}
		return InputResult::Consumed;
	case SDL_EVENT_MOUSE_BUTTON_DOWN:
	case SDL_EVENT_MOUSE_BUTTON_UP:
		if (event.button.windowID == id && windowInteractive() && event.button.button == SDL_BUTTON_LEFT)
		{
			const bool inside = contains(layoutFor(window).button(), event.button.x, event.button.y);
			if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
				m_buttonFocused = m_armed = inside;
			else
			{
				if (m_armed && inside)
					activate("mouse");
				m_armed = false;
			}
		}
		return InputResult::Consumed;
	case SDL_EVENT_KEY_UP:
	case SDL_EVENT_TEXT_INPUT:
	case SDL_EVENT_TEXT_EDITING:
	case SDL_EVENT_MOUSE_MOTION:
	case SDL_EVENT_MOUSE_WHEEL:
		return InputResult::Consumed;
	default:
		return InputResult::Unhandled;
	}
}

void UiSpike::activate(const char *source)
{
	++m_activations;
	std::printf("UI spike activated by %s (%u)\n", source, m_activations);
}

void UiSpike::record(VkCommandBuffer cmd, uint32_t frameIndex, SDL_Window *window, VkExtent2D extent)
{
	const Layout layout = layoutFor(window);
	if (layout.scale <= 0.f || extent.width == 0 || extent.height == 0)
		return;

	std::array<Quad, kQuadCapacity> quads;
	uint32_t count = 0;
	const auto quad = [&](glm::vec4 rect, glm::vec4 uv, glm::vec4 color) {
		if (count < quads.size())
			quads[count++] = {layout.rect(rect), uv, color};
	};
	const auto box = [&](glm::vec4 rect, glm::vec4 color) { quad(rect, m_whiteUv, color); };
	const auto label = [&](glm::vec2 pos, std::string_view value, glm::vec4 color, float scale = 1.f) {
		for (const unsigned char c : value)
		{
			const Glyph &g = m_glyphs[c < m_glyphs.size() ? c : '?'];
			if (g.visible)
				quad(g.rect * scale + glm::vec4(pos, pos), g.uv, color);
			pos.x += g.advance * scale;
		}
	};

	float x = 0.f, y = 0.f;
	SDL_GetMouseState(&x, &y);
	const bool hovered = windowInteractive() && SDL_GetMouseFocus() == window && contains(layout.button(), x, y);
	box({0, 0, kPanelWidth, kPanelHeight}, {0.065f, 0.10f, 0.14f, 1.f});
	box({0, 0, kPanelWidth, 4}, kAccent);
	label({24, 20}, "Game UI prototype", kInk, 2.f);
	label({24, 53}, "Texture atlas sample clipped to this frame", kMuted);
	box({24, 76, 376, 128}, {0.24f, 0.34f, 0.43f, 1.f});
	if (m_buttonFocused)
		box({22, 141, 222, 187}, kInk);
	const glm::vec4 fill = m_armed && hovered ? kAccent :
		(hovered ? glm::vec4(0.23f, 0.55f, 0.50f, 1.f) : glm::vec4(0.15f, 0.38f, 0.37f, 1.f));
	box({24, 143, 220, 185}, fill);
	label({39, 157}, "Activate button", m_armed && hovered ? glm::vec4(0.06f, 0.13f, 0.13f, 1.f) : kInk);
	char status[32];
	std::snprintf(status, sizeof(status), "Activations: %u", m_activations);
	label({239, 157}, status, kInk);
	label({24, 205}, "Tab focus   Enter or click activate   Esc exit", kMuted);

	const uint32_t sampleFirst = count;
	quad({12, 65, 388, 143}, m_sampleUv, glm::vec4(1.f));
	const glm::vec2 pixelsPerLogical = glm::vec2(extent.width, extent.height) / layout.size;
	const glm::vec4 clip = layout.rect({26, 78, 374, 126}) * glm::vec4(pixelsPerLogical, pixelsPerLogical);
	const VkOffset2D clipStart{static_cast<int32_t>(std::ceil(clip.x)), static_cast<int32_t>(std::ceil(clip.y))};
	const int32_t clipRight = std::min(static_cast<int32_t>(std::floor(clip.z)), static_cast<int32_t>(extent.width));
	const int32_t clipBottom = std::min(static_cast<int32_t>(std::floor(clip.w)), static_cast<int32_t>(extent.height));

	// Engine records only after the fence for this frame-in-flight slot.
	const AllocatedBuffer &buffer = m_instances[frameIndex];
	const auto bytes = VkDeviceSize(count) * sizeof(Quad);
	std::memcpy(buffer.info.pMappedData, quads.data(), static_cast<size_t>(bytes));
	vmaFlushAllocation(m_allocator, buffer.allocation, 0, bytes);

	const VkDeviceSize offset = 0;
	const glm::vec2 ndc = 2.f / layout.size;
	const VkRect2D full{{0, 0}, extent};
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &m_atlasSet, 0, nullptr);
	vkCmdBindVertexBuffers(cmd, 0, 1, &buffer.buffer, &offset);
	vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(ndc), &ndc);
	vkCmdSetScissor(cmd, 0, 1, &full);
	vkCmdDraw(cmd, 6, sampleFirst, 0, 0);
	if (clipRight > clipStart.x && clipBottom > clipStart.y)
	{
		const VkRect2D scissor{clipStart, {static_cast<uint32_t>(clipRight - clipStart.x),
			static_cast<uint32_t>(clipBottom - clipStart.y)}};
		vkCmdSetScissor(cmd, 0, 1, &scissor);
		vkCmdDraw(cmd, 6, 1, 0, sampleFirst);
		vkCmdSetScissor(cmd, 0, 1, &full);
	}
}
