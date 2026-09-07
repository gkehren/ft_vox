#pragma once

/// Deterministic offscreen rendering fixture for the visual-regression
/// harness (issue #142). Boots the production WorldRenderer (Shadow →
/// Opaque → Water → Sky → Post) against a hidden SDL/Vulkan window, then
/// records frames into a caller-owned composite target and reads pixels
/// back synchronously — test-only: the runtime renderer stays fully
/// asynchronous and never presents.

#include "VisualImage.hpp"

#include "Vulkan/VkContext.hpp"
#include "Vulkan/VkSwapchain.hpp"
#include "Vulkan/VkCommands.hpp"
#include "Vulkan/VkBuffer.hpp"
#include "Vulkan/VkImage.hpp"
#include "Vulkan/GpuResourceRetire.hpp"
#include "Renderer/WorldRenderer.hpp"
#include "Chunk/ChunkManager.hpp"
#include "Chunk/ChunkPool.hpp"
#include "Chunk/TerrainGenerator.hpp"
#include "Engine/EngineDefs.hpp"
#include "Engine/ThreadPool.hpp"
#include "Camera/Camera.hpp"
#include "Entities/MobTypes.hpp"

#include <glm/glm.hpp>
#include <volk.h>

#include <memory>
#include <ostream>
#include <vector>

class VisualHarness
{
public:
	/// Boot SDL (hidden window), Vulkan, the renderer and the shared world
	/// infrastructure. Returns false when the environment cannot provide a
	/// usable Vulkan device/surface — the caller must SKIP (exit 77), not fail.
	bool init(std::ostream &log);
	void shutdown();

	VkExtent2D extent() const { return m_extent; }
	WorldRenderer &renderer() { return m_renderer; }
	ChunkManager &chunks() { return *m_chunkManager; }
	TerrainGenerator &terrain() { return *m_terrain; }
	Camera &camera() { return m_camera; }
	ShaderParameters &shader() { return m_shader; }
	PostProcessSettings &post() { return m_renderer.postSettings(); }
	RenderSettings &renderSettings() { return m_renderSettings; }
	long validationErrors() const { return m_context.validationErrorCount(); }
	bool isValidationEnabled() const { return m_context.isValidationEnabled(); }

	/// Rebuild the world for a scene: destroys the previous chunk manager
	/// (mirrors Engine::reloadWorld ordering) and re-seeds terrain.
	void beginScene(int seed);

	/// Synchronous full-mesh bootstrap around a center — no thread pool, no
	/// streaming: voxel content and meshes are seed-deterministic.
	void buildArea(const glm::vec3 &center, int radiusChunks);

	/// Re-mesh + re-upload every chunk dirtied by voxel edits (state
	/// GENERATED after placeVoxel/deleteVoxel). Synchronous.
	void remeshEditedChunks();

	/// Record one frame with the pinned animation time and read the composite
	/// target back as RGBA8 (BGRA readback is swizzled). The frame UBO uses
	/// slot 0 — submitAndWait guarantees the GPU is idle between frames.
	visual::RgbaImage renderFrame(float time, const std::vector<entities::MobRenderState> &mobs);

  private:
	SDL_Window *m_window{nullptr};
	VkContext m_context;
	VkSwapchain m_swapchain;
	ImmediateCommands m_imm;
	GpuResourceRetire m_retire;
	VkExtent2D m_extent{0, 0};
	VkFormat m_targetFormat{VK_FORMAT_UNDEFINED};
	AllocatedImage m_target{};
	AllocatedBuffer m_readback{};
	uint64_t m_frameCounter{0};
	bool m_ready{false};

	std::unique_ptr<ThreadPool> m_threadPool;
	std::unique_ptr<ChunkPool> m_chunkPool;
	std::unique_ptr<TerrainGenerator> m_terrain;
	std::unique_ptr<ChunkManager> m_chunkManager;

	Camera m_camera{glm::vec3(0.0f, 100.0f, 0.0f)};
	ShaderParameters m_shader{};
	RenderSettings m_renderSettings{};
	WorldRenderer m_renderer;
	std::vector<Chunk *> m_drawList;
	std::vector<Chunk *> m_shadowList;
};
