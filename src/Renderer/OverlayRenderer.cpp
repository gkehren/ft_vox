#include "Renderer/OverlayRenderer.hpp"
#include "Vulkan/VkShader.hpp"
#include "Vulkan/VkUpload.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace
{
std::string spvPath(const char *name)
{
	const char *prefixes[] = {"./ressources/shaders/spv/", "ressources/shaders/spv/", "../ressources/shaders/spv/"};
	for (const char *p : prefixes)
	{
		std::string path = std::string(p) + name;
		std::ifstream f(path, std::ios::binary);
		if (f)
			return path;
	}
	return std::string(RES_PATH) + "shaders/spv/" + name;
}

struct PushPC
{
	glm::mat4 model;
	glm::vec4 color;
};
static_assert(sizeof(PushPC) <= 128, "push constants must fit device limit");
} // namespace

OverlayRenderer::~OverlayRenderer()
{
	shutdown();
}

void OverlayRenderer::shutdown()
{
	if (!m_context)
		return;
	m_context->waitIdle();
	destroyPipelines();
	if (m_boxVBO.buffer)
		destroyBuffer(m_context->getAllocator(), m_boxVBO);
	if (m_boxIBO.buffer)
		destroyBuffer(m_context->getAllocator(), m_boxIBO);
	if (m_playerVBO.buffer)
		destroyBuffer(m_context->getAllocator(), m_playerVBO);
	if (m_playerIBO.buffer)
		destroyBuffer(m_context->getAllocator(), m_playerIBO);
	m_context = nullptr;
}

void OverlayRenderer::createGeometry(ImmediateCommands &imm)
{
	const float unitCube[] = {
		0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0,
		0, 0, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1,
	};
	const uint16_t lineIdx[] = {
		0, 1, 1, 2, 2, 3, 3, 0,
		4, 5, 5, 6, 6, 7, 7, 4,
		0, 4, 1, 5, 2, 6, 3, 7,
	};
	// Centered unit cube for players (-0.5..0.5), slightly taller
	const float playerVerts[] = {
		-0.4f, 0.0f, -0.4f, 0.4f, 0.0f, -0.4f, 0.4f, 1.8f, -0.4f, -0.4f, 1.8f, -0.4f,
		-0.4f, 0.0f, 0.4f, 0.4f, 0.0f, 0.4f, 0.4f, 1.8f, 0.4f, -0.4f, 1.8f, 0.4f,
	};
	const uint16_t playerIdx[] = {
		0, 1, 2, 0, 2, 3, 4, 6, 5, 4, 7, 6,
		0, 4, 5, 0, 5, 1, 2, 6, 7, 2, 7, 3,
		0, 3, 7, 0, 7, 4, 1, 5, 6, 1, 6, 2,
	};

	auto upload = [&](const void *data, VkDeviceSize size, VkBufferUsageFlags usage) {
		AllocatedBuffer buf = createBuffer(m_context->getAllocator(), size,
										   VK_BUFFER_USAGE_TRANSFER_DST_BIT | usage,
										   VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
		uploadBuffer(m_context->getAllocator(), imm, buf, data, size);
		return buf;
	};

	m_boxVBO = upload(unitCube, sizeof(unitCube), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
	m_boxIBO = upload(lineIdx, sizeof(lineIdx), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
	m_playerVBO = upload(playerVerts, sizeof(playerVerts), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
	m_playerIBO = upload(playerIdx, sizeof(playerIdx), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
}

void OverlayRenderer::createPipelines(VkFormat colorFormat, VkFormat depthFormat)
{
	destroyPipelines();

	VkPushConstantRange pcr{};
	pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	pcr.offset = 0;
	pcr.size = sizeof(PushPC);

	VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	pl.setLayoutCount = 1;
	pl.pSetLayouts = &m_frameSetLayout;
	pl.pushConstantRangeCount = 1;
	pl.pPushConstantRanges = &pcr;
	if (vkCreatePipelineLayout(m_context->getDevice(), &pl, nullptr, &m_layout) != VK_SUCCESS)
		throw std::runtime_error("overlay pipeline layout failed");

	VkShaderModule vert = loadShaderModule(m_context->getDevice(), spvPath("overlay.vert.spv"));
	VkShaderModule frag = loadShaderModule(m_context->getDevice(), spvPath("overlay.frag.spv"));

	VkPipelineShaderStageCreateInfo stages[2] = {
		{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vert, "main", nullptr},
		{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main", nullptr},
	};

	VkVertexInputBindingDescription bind{0, sizeof(float) * 3, VK_VERTEX_INPUT_RATE_VERTEX};
	VkVertexInputAttributeDescription attr{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
	VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	vi.vertexBindingDescriptionCount = 1;
	vi.pVertexBindingDescriptions = &bind;
	vi.vertexAttributeDescriptionCount = 1;
	vi.pVertexAttributeDescriptions = &attr;

	VkPipelineInputAssemblyStateCreateInfo iaLines{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
	iaLines.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
	VkPipelineInputAssemblyStateCreateInfo iaTris = iaLines;
	iaTris.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
	vp.viewportCount = vp.scissorCount = 1;

	VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.cullMode = VK_CULL_MODE_NONE;
	rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rs.lineWidth = 1.0f;

	VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
	ds.depthTestEnable = VK_TRUE;
	ds.depthWriteEnable = VK_FALSE;
	ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

	VkPipelineColorBlendAttachmentState ba{};
	ba.colorWriteMask = 0xF;
	ba.blendEnable = VK_TRUE;
	ba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
	ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	ba.colorBlendOp = VK_BLEND_OP_ADD;
	ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	ba.alphaBlendOp = VK_BLEND_OP_ADD;
	VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	cb.attachmentCount = 1;
	cb.pAttachments = &ba;

	std::array<VkDynamicState, 2> dynS = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
	dyn.dynamicStateCount = 2;
	dyn.pDynamicStates = dynS.data();

	VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
	ri.colorAttachmentCount = 1;
	ri.pColorAttachmentFormats = &colorFormat;
	ri.depthAttachmentFormat = depthFormat;

	auto make = [&](VkPipelineInputAssemblyStateCreateInfo &ia, VkPipeline &out) {
		VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
		gi.pNext = &ri;
		gi.stageCount = 2;
		gi.pStages = stages;
		gi.pVertexInputState = &vi;
		gi.pInputAssemblyState = &ia;
		gi.pViewportState = &vp;
		gi.pRasterizationState = &rs;
		gi.pMultisampleState = &ms;
		gi.pDepthStencilState = &ds;
		gi.pColorBlendState = &cb;
		gi.pDynamicState = &dyn;
		gi.layout = m_layout;
		if (vkCreateGraphicsPipelines(m_context->getDevice(), VK_NULL_HANDLE, 1, &gi, nullptr, &out) != VK_SUCCESS)
			throw std::runtime_error("overlay pipeline failed");
	};

	make(iaLines, m_linePipeline);
	make(iaTris, m_solidPipeline);

	destroyShaderModule(m_context->getDevice(), vert);
	destroyShaderModule(m_context->getDevice(), frag);
}

void OverlayRenderer::destroyPipelines()
{
	if (!m_context)
		return;
	if (m_linePipeline)
		vkDestroyPipeline(m_context->getDevice(), m_linePipeline, nullptr);
	if (m_solidPipeline)
		vkDestroyPipeline(m_context->getDevice(), m_solidPipeline, nullptr);
	if (m_layout)
		vkDestroyPipelineLayout(m_context->getDevice(), m_layout, nullptr);
	m_linePipeline = m_solidPipeline = VK_NULL_HANDLE;
	m_layout = VK_NULL_HANDLE;
}

void OverlayRenderer::init(VkContext &context, ImmediateCommands &imm, VkDescriptorSetLayout frameSetLayout,
						   VkFormat colorFormat, VkFormat depthFormat)
{
	m_context = &context;
	m_frameSetLayout = frameSetLayout;
	createGeometry(imm);
	createPipelines(colorFormat, depthFormat);
}

void OverlayRenderer::recreatePipelines(VkFormat colorFormat, VkFormat depthFormat)
{
	createPipelines(colorFormat, depthFormat);
}

void OverlayRenderer::drawBox(VkCommandBuffer cmd, const glm::mat4 &model, const glm::vec4 &color)
{
	PushPC pc{model, color};
	vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
	VkDeviceSize off = 0;
	vkCmdBindVertexBuffers(cmd, 0, 1, &m_boxVBO.buffer, &off);
	vkCmdBindIndexBuffer(cmd, m_boxIBO.buffer, 0, VK_INDEX_TYPE_UINT16);
	vkCmdDrawIndexed(cmd, 24, 1, 0, 0, 0);
}

void OverlayRenderer::drawPlayerCube(VkCommandBuffer cmd, const glm::mat4 &model, const glm::vec4 &color)
{
	PushPC pc{model, color};
	vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
	VkDeviceSize off = 0;
	vkCmdBindVertexBuffers(cmd, 0, 1, &m_playerVBO.buffer, &off);
	vkCmdBindIndexBuffer(cmd, m_playerIBO.buffer, 0, VK_INDEX_TYPE_UINT16);
	vkCmdDrawIndexed(cmd, 36, 1, 0, 0, 0);
}

glm::vec3 OverlayRenderer::colorFromPlayerId(uint32_t playerId)
{
	float h = std::fmod(static_cast<float>(playerId) * 0.61803398875f, 1.0f);
	float s = 0.55f, v = 0.95f;
	int i = int(h * 6.0f);
	float f = h * 6.0f - i;
	float p = v * (1.0f - s);
	float q = v * (1.0f - f * s);
	float t = v * (1.0f - (1.0f - f) * s);
	switch (i % 6)
	{
	case 0: return {v, t, p};
	case 1: return {q, v, p};
	case 2: return {p, v, t};
	case 3: return {p, q, v};
	case 4: return {t, p, v};
	default: return {v, p, q};
	}
}

void OverlayRenderer::record(VkCommandBuffer cmd, VkDescriptorSet frameSet0,
							 const std::vector<Chunk *> &chunks)
{
	if (!m_linePipeline)
		return;

	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &frameSet0, 0, nullptr);

	if (m_showChunkBorders)
	{
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_linePipeline);
		const glm::vec4 red(1.f, 0.15f, 0.1f, 0.9f);
		for (Chunk *chunk : chunks)
		{
			if (!chunk)
				continue;
			glm::mat4 model(1.f);
			model = glm::translate(model, chunk->getPosition());
			model = glm::scale(model, glm::vec3(float(CHUNK_SIZE), float(CHUNK_HEIGHT), float(CHUNK_SIZE)));
			drawBox(cmd, model, red);
		}
	}

	if (m_highlight.active)
	{
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_linePipeline);
		glm::mat4 model(1.f);
		model = glm::translate(model, m_highlight.position);
		// slight inflate so lines aren't z-fighting
		model = glm::translate(model, glm::vec3(-0.005f));
		model = glm::scale(model, glm::vec3(1.01f));
		drawBox(cmd, model, glm::vec4(m_highlight.color, 1.f));
	}

	if (!m_players.empty())
	{
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_solidPipeline);
		for (const auto &p : m_players)
		{
			glm::mat4 model(1.f);
			model = glm::translate(model, p.position);
			const glm::vec3 c = colorFromPlayerId(p.id);
			drawPlayerCube(cmd, model, glm::vec4(c, 0.95f));
		}
	}
}
