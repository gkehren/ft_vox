#pragma once

#include <Engine/EngineDefs.hpp>
#include <Engine/Benchmark.hpp>
#include <Camera/Camera.hpp>
#include <Physics/PlayerController.hpp>
#include <Chunk/ChunkManager.hpp>
#include <Chunk/ChunkPool.hpp>
#include <Chunk/TerrainGenerator.hpp>
#include <Renderer/OverlayRenderer.hpp>
#include <Renderer/WorldRenderer.hpp>
#include <Vulkan/VkContext.hpp>
#include <Vulkan/VkCommands.hpp>
#include <Vulkan/VkImage.hpp>
#include <utils.hpp>

#include <Engine/DebugUI/DebugUiCore.hpp>
#include <Engine/GameUIBiomeMap.hpp>

#include <future>
#include <atomic>
#include <vector>
#include <functional>
#include <cstdint>
#include <string>

class StagingRing;

/// Frame snapshot for ImGui panels (pointers owned by Engine).
/// Debug/telemetry data reaches panels through debugui::UiState snapshots
/// (issue #179); the raw pointers here are the explicit settings structs and
/// the callbacks for actions that need engine coordination.
struct GameUIFrame
{
	Camera *camera{nullptr};
	const physics::PlayerController *player{nullptr};
	bool playerFlight{false};
	const char *playerStatus{""};
	std::function<void(bool)> setPlayerFlight;
	std::function<void(CameraMode)> setCameraMode;
	ChunkManager *chunks{nullptr};
	ChunkPool *pool{nullptr};
	TerrainGenerator *generator{nullptr};
	WorldRenderer *worldRenderer{nullptr};
	/// Frame-in-flight slot whose fence beginFrame has already waited — the
	/// only slot from which a CPU debug readback is safe.
	uint32_t frameIndex{0};
	VkContext *vk{nullptr};
	VkGpuProfiler *gpu{nullptr};
	ImmediateCommands *imm{nullptr};

	ShaderParameters *shader{nullptr};
	RenderSettings *render{nullptr};
	RenderTiming *timing{nullptr};
	/// Frame staging ring (Engine-owned): live slice usage/capacity for the
	/// memory panel. Never used by the UI for allocation.
	StagingRing *staging{nullptr};
	TextureType *selectedTexture{nullptr};
	OverlayHighlight *highlight{nullptr};

	bool *mouseCaptured{nullptr};
	bool *showChunkBorders{nullptr};
	bool *showDemoPlayers{nullptr};
    bool *mobsEnabled{nullptr};
    size_t mobCount{}, mobVisible{};
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

/// Multi-panel ImGui surface for the Vulkan engine.
///
/// GameUI itself is only the shell: main menu bar, gameplay HUD, biome-map
/// window, help, on-screen hints, input shortcuts and biome-map plumbing.
/// Every developer-tool panel (Overview, Performance, Rendering, Streaming,
/// Chunk inspector, Memory, Benchmark) lives in src/Engine/DebugUI/ and
/// consumes the read-only snapshots in debugui::UiState (issue #179).
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
	///   (F1-F12 panel toggles, F10 VSync) - honored even while ImGui
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

	bool showHud() const { return m_debug.panels.hud; }
	bool showGraphics() const { return m_debug.panels.rendering; }
	bool showStreaming() const { return m_debug.panels.streaming; }
	bool showWorld() const { return m_debug.panels.world; }
	bool showHelp() const { return m_debug.panels.help; }
	bool showProfiler() const { return m_debug.panels.performance; }

	/// Reproducible streaming benchmark: map stays open at a fixed center.
	void configureBenchmarkMap(float zoom)
	{
		m_debug.panels.world = true;
		m_mapZoom = zoom;
		m_mapFollow = false;
		invalidateBiomeMap();
	}

	void setShowHud(bool v) { m_debug.panels.hud = v; }

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

	/// Developer-console state: panel visibility + debug snapshots + bounded
	/// histories (issue #179). Refreshed once per draw by updateDebugUiState.
	debugui::UiState m_debug{};

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
	uint64_t m_mapCaptureEpoch{0};
	BiomeMapUpload m_pendingUpload{};
	// Owner scratch for sequential/small maps. Parallel tiles use separate
	// bounded thread-local scratch; this retention cap allows dense reuse so
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
};
