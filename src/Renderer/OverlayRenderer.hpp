#pragma once

#include "Vulkan/VkContext.hpp"
#include "Vulkan/VkCommands.hpp"
#include "Vulkan/VkBuffer.hpp"
#include "Camera/Camera.hpp"
#include "Chunk/Chunk.hpp"
#include "utils.hpp"

#include <glm/glm.hpp>
#include <vector>
#include <volk.h>

struct OverlayHighlight
{
	bool active{false};
	glm::vec3 position{0.f};
	glm::vec3 color{1.f, 0.2f, 0.2f};
};

struct OverlayPlayer
{
	glm::vec3 position{0.f};
	uint32_t id{0};
};

/// Wireframe boxes + solid player cubes drawn into the HDR scene (PR6).
class OverlayRenderer
{
public:
	OverlayRenderer() = default;
	~OverlayRenderer();

	void init(VkContext &context, ImmediateCommands &imm, VkDescriptorSetLayout frameSetLayout,
			  VkFormat colorFormat, VkFormat depthFormat);
	void shutdown();
	void recreatePipelines(VkFormat colorFormat, VkFormat depthFormat);

	void setShowChunkBorders(bool v) { m_showChunkBorders = v; }
	bool showChunkBorders() const { return m_showChunkBorders; }

	void setHighlight(const OverlayHighlight &h) { m_highlight = h; }
	void setPlayers(std::vector<OverlayPlayer> players) { m_players = std::move(players); }

	void record(VkCommandBuffer cmd, VkDescriptorSet frameSet0,
				const std::vector<Chunk *> &chunks);

	static glm::vec3 colorFromPlayerId(uint32_t playerId);

private:
	void createGeometry(ImmediateCommands &imm);
	void createPipelines(VkFormat colorFormat, VkFormat depthFormat);
	void destroyPipelines();

	void drawBox(VkCommandBuffer cmd, const glm::mat4 &model, const glm::vec4 &color);
	void drawPlayerCube(VkCommandBuffer cmd, const glm::mat4 &model, const glm::vec4 &color);

	VkContext *m_context{nullptr};
	VkDescriptorSetLayout m_frameSetLayout{VK_NULL_HANDLE};

	AllocatedBuffer m_boxVBO{};
	AllocatedBuffer m_boxIBO{};
	AllocatedBuffer m_playerVBO{};
	AllocatedBuffer m_playerIBO{};

	VkPipelineLayout m_layout{VK_NULL_HANDLE};
	VkPipeline m_linePipeline{VK_NULL_HANDLE};
	VkPipeline m_solidPipeline{VK_NULL_HANDLE};

	bool m_showChunkBorders{false};
	OverlayHighlight m_highlight{};
	std::vector<OverlayPlayer> m_players;
};
