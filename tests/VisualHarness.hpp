#pragma once

/// Deterministic offscreen rendering fixture for the visual-regression
/// harness (issue #142). Boots the production WorldRenderer (Shadow →
/// Opaque → Water → Sky → Post) against a hidden SDL/Vulkan window, then
/// records frames into a caller-owned composite target and reads pixels
/// back synchronously — test-only: the runtime renderer stays fully
/// asynchronous and never presents.
///
/// Initialization is split so the caller can snapshot validation errors
/// after stage 1: only the device/swapchain creation phase is considered
/// potentially polluted by injected overlays (RTSS — docs/vulkan-validation.md).
/// Everything from WorldRenderer::init onward must be validation-clean.

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
	/// Golden-image contract: fixed resolution and an 8-bit sRGB composite
	/// format. A surface that cannot provide it must SKIP, not render
	/// against a reference produced under a different contract.
	static constexpr uint32_t kWidth = 640;
	static constexpr uint32_t kHeight = 360;

	/// Stage 1: SDL window + Vulkan device + swapchain + immediate commands.
	/// Returns false when the environment cannot provide a usable Vulkan
	/// device/surface or the golden-image contract — the caller must SKIP.
	bool initDevice(std::ostream &log);

	/// Stage 2: production renderer + offscreen composite target + readback
	/// buffers. Throws on failure (that is a test failure, not a skip). The
	/// caller asserts no validation errors were raised by this stage.
	void initRenderer(std::ostream &log);

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
	/// Non-finite fp16 samples found in the HDR target during the last
	/// renderFrame (NaN/Inf must be checked pre-tonemap: compositing
	/// quantizes them into undefined bytes).
	long long lastNonFiniteSamples() const { return m_lastNonFinite; }

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
	/// Also copies the HDR scene target and counts non-finite fp16 samples.
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
	AllocatedBuffer m_hdrReadback{};
	long long m_lastNonFinite{0};
	uint64_t m_frameCounter{0};
	bool m_deviceReady{false};
	bool m_rendererReady{false};

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
