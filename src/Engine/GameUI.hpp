#pragma once

#include <Engine/EngineDefs.hpp>
#include <Engine/Benchmark.hpp>
#include <Camera/Camera.hpp>
#include <Chunk/ChunkManager.hpp>
#include <Chunk/ChunkPool.hpp>
#include <Chunk/TerrainGenerator.hpp>
#include <Renderer/OverlayRenderer.hpp>
#include <Renderer/WorldRenderer.hpp>
#include <Vulkan/VkContext.hpp>
#include <Vulkan/VkCommands.hpp>
#include <Vulkan/VkImage.hpp>
#include <utils.hpp>

#include <future>
#include <atomic>
#include <vector>
#include <functional>
#include <cstdint>
#include <string>

#include <Engine/GameUIBiomeMap.hpp>

class StagingRing;

/// Frame snapshot for ImGui panels (pointers owned by Engine).
struct GameUIFrame
{
	Camera *camera{nullptr};
	ChunkManager *chunks{nullptr};
	ChunkPool *pool{nullptr};
	TerrainGenerator *generator{nullptr};
	WorldRenderer *worldRenderer{nullptr};
	VkContext *vk{nullptr};
	ImmediateCommands *imm{nullptr};

	ShaderParameters *shader{nullptr};
	RenderSettings *render{nullptr};
	RenderTiming *timing{nullptr};
	TextureType *selectedTexture{nullptr};
	OverlayHighlight *highlight{nullptr};

	bool *mouseCaptured{nullptr};
	bool *showChunkBorders{nullptr};
	bool *showDemoPlayers{nullptr};
	bool *paused{nullptr};

	int seed{0};
	uint64_t worldGenerationId{0};
	float fps{0.f};
	float frameMs{0.f};
	size_t drawCount{0};
	int windowW{0};
	int windowH{0};
	uint32_t vkApiVersion{0};
	const char *deviceName{nullptr};
	const char *presentModeName{nullptr};
	bool validation{false};

	std::function<void(bool)> setVSync;

	/// Active pack root (empty = bundled). Owned by Engine.
	const std::string *resourcePackRoot{nullptr};

	/// Result of Apply / Use bundled (atlas may still load from bundled on invalid pack).
	struct ResourcePackUiResult
	{
		bool atlasOk{false};
		bool isError{false};
		bool isWarning{false};
		std::string message;
	};
	std::function<ResourcePackUiResult(const std::string &)> applyResourcePack;

	Benchmark *benchmark{nullptr};

	/// Asynchronous biome map job submission abstraction (e.g. Engine's ThreadPool with low priority).
	std::function<std::future<BiomeMapResult>(BiomeMapRequest)> submitBiomeMap;
};

#include <Engine/GameUIResourcePack.hpp>

/// Multi-panel ImGui HUD for the Vulkan engine.
/// Restores prior OpenGL UI features (adapted), with cleaner layout + shortcuts.
class GameUI
{
public:
	GameUI() = default;
	~GameUI();

	void init(VkContext &context, ImmediateCommands &imm);
	void shutdown();
	/// Re-register UI textures after the ImGui Vulkan descriptor pool changes.
	void onImGuiVulkanBackendRecreate();

	/// Draw all panels. Call between ImGui beginFrame / endFrame.
	void draw(GameUIFrame &frame);

	/// Keyboard shortcut handling, split by input-routing policy (issue #76):
	/// - handleGlobalShortcut: intentional global non-text shortcuts
	///   (F1-F7 panel toggles, F10 VSync) - honored even while ImGui
	///   captures the keyboard.
	/// - handleGameplayShortcut: gameplay state changes (P pause) - the
	///   caller must only invoke these while ImGui does NOT capture the
	///   keyboard, so typing in a text field stays inert.
	/// Returns true if the key was consumed.
	bool handleGlobalShortcut(int sdlKeycode, GameUIFrame &frame);
	bool handleGameplayShortcut(int sdlKeycode, GameUIFrame &frame);

	/// Used by Engine to prevent application quit while the modal handles
	/// Escape itself (ImGuiFileDialog cancels via IGFD_EXIT_KEY).
	bool isFileDialogOpen() const;

	bool showHud() const { return m_showHud; }
	bool showGraphics() const { return m_showGraphics; }
	bool showStreaming() const { return m_showStreaming; }
	bool showWorld() const { return m_showWorld; }
	bool showHelp() const { return m_showHelp; }
	bool showProfiler() const { return m_showProfiler; }

	void setShowHud(bool v) { m_showHud = v; }

	/// Invalidate any active or in-flight biome map task and clear current texture.
	/// Supersedes existing request ID and marks backing texture as inactive.
	void invalidateBiomeMap();
	bool isBiomeMapPending() const { return m_mapJob.isRunning(); }
	uint64_t currentWorldGenId() const { return m_currentWorldGenId; }
	uint64_t currentMapRequestId() const { return m_mapRequestId; }

	bool hasPendingBiomeMapUpload() const { return !m_pendingUpload.rgba.empty(); }
	const BiomeMapUpload &pendingBiomeMapUpload() const { return m_pendingUpload; }
	void recordPendingBiomeMapUpload(VkCommandBuffer cmd, StagingRing &stagingRing);

private:
	struct BiomeMapJob
	{
		uint64_t requestId{0};
		std::shared_ptr<std::atomic<bool>> cancel;
		std::future<BiomeMapResult> future;

		bool isRunning() const
		{
			return future.valid() &&
				   future.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
		}

		bool isReady() const
		{
			return future.valid() &&
				   future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
		}

		void reset()
		{
			if (cancel)
				cancel->store(true, std::memory_order_relaxed);
			cancel.reset();
			future = {};
			requestId = 0;
		}
	};

	void drawMenuBar(GameUIFrame &frame);
	void drawHud(GameUIFrame &frame);
	void drawGraphics(GameUIFrame &frame);
	void drawStreaming(GameUIFrame &frame);
	void drawProfiler(GameUIFrame &frame);
	void drawBenchmarkReport(GameUIFrame &frame);
	void drawWorld(GameUIFrame &frame);
	void drawHelp();
	void drawOverlayHints(GameUIFrame &frame);

	void requestBiomeMapRefresh()
	{
		m_mapNeedsUpdate = true;
	}

	void supersedeBiomeMapRequest()
	{
		++m_mapRequestId;

		if (m_mapJob.cancel)
			m_mapJob.cancel->store(true, std::memory_order_relaxed);

		m_pendingUpload = {};
		m_mapNeedsUpdate = true;
	}

	void tickBiomeMap(GameUIFrame &frame);
	void ensureBiomeTexture(int size);

	bool m_showHud{true};
	bool m_showGraphics{false};
	bool m_showStreaming{false};
	bool m_showProfiler{false};
	bool m_showWorld{false};
	bool m_showHelp{false};
	bool m_showOverlayHints{true};

	// Biome map
	int m_mapSize{256};
	float m_mapZoom{0.5f};
	glm::vec2 m_mapCenter{0.f, 0.f};
	bool m_mapFollow{true};
	bool m_mapNeedsUpdate{true};
	double m_mapLastPublishedAt{0.0};
	glm::vec2 m_mapLastPlayer{0.f, 0.f};
	uint64_t m_currentWorldGenId{0};
	int m_currentSeed{0};
	uint64_t m_mapRequestId{0};

	BiomeMapJob m_mapJob{};
	BiomeMapUpload m_pendingUpload{};
	// Single process-wide region scratch for the biome-map job (never one
	// per pool worker): its retention cap allows dense-capacity reuse so
	// refreshes do not re-allocate the dense peak, while retention stays
	// bounded by kMaxDenseDomainPoints per field (~10 MiB logical payload
	// in total). The scratch holds only scratch floats - no seed- or
	// world-dependent state - so it survives world/seed changes and is
	// never reset on map invalidation; shared ownership releases it at
	// GameUI destruction. Shared ownership keeps it alive for the duration
	// of an in-flight job.
	std::shared_ptr<TerrainGenerator::BiomeRegionScratch> m_mapScratch;

	VkContext *m_vk{nullptr};
	ImmediateCommands *m_imm{nullptr};
	AllocatedImage m_mapImage{};
	VkImageLayout m_mapImageLayout{VK_IMAGE_LAYOUT_UNDEFINED};
	VkSampler m_mapSampler{VK_NULL_HANDLE};
	VkDescriptorSet m_mapDesc{VK_NULL_HANDLE};
	int m_mapImageSize{0};
	bool m_mapHasTexture{false};

	ResourcePackUiState m_resourcePackUi{};
};
