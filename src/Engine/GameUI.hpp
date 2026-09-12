#pragma once

#include <Engine/EngineDefs.hpp>
#include <Engine/Benchmark.hpp>
#include <Engine/PlayerUi.hpp>
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

#include <Engine/DebugUI/DebugUiCore.hpp>
#include <Engine/UiShell.hpp>
#include <Engine/GameUIBiomeMap.hpp>

#include <future>
#include <atomic>
#include <vector>
#include <functional>
#include <cstdint>
#include <string>

class StagingRing;

/// Plain-copy world-save snapshot for the World panel (issue #180, Phase 5).
/// Filled by Engine::drawUi from ChunkManager::worldPersistence()->status();
/// deliberately value-only so GameUI keeps no dependency on the save layer.
struct WorldSaveUiState
{
	bool active{false}; ///< persistence running for this session
	bool error{false};  ///< lastError non-empty
	std::string worldName;
	int seed{0};
	size_t queueDepth{0};
	size_t queueDepthPeak{0};
	double avgSerializeMs{0.0}, maxSerializeMs{0.0};
	double avgWriteMs{0.0}, maxWriteMs{0.0};
	uint64_t rejectedBusyQueueFull{0};
	uint64_t rejectedBusyCommitGate{0};
	// Instant state (issue #180 review round 7): coordinates currently dirty
	// or whose CURRENT attempt failed. Drives the Saved/Saving/Failed label;
	// a retry clears them while the historical counters below keep counting.
	uint64_t dirtyCoordinates{0};
	uint64_t failedCoordinates{0};
	// History (lifetime counters).
	uint64_t enqueued{0};
	uint64_t completed{0};  ///< chunk files written
	uint64_t superseded{0}; ///< pending writes dropped for a newer revision
	uint64_t failed{0};
	uint64_t deleted{0}; ///< chunk files removed (overrides reverted)
	uint64_t bytesWritten{0};
	std::string lastError;
};

/// Instant save health for the World panel label (issue #180 review round
/// 7, item 1): active coordinate failures win over queued work; an old
/// historical failure never keeps the label at "Failed" once the retry
/// cleared the coordinate. Pure function - unit-tested headlessly.
enum class SaveUiHealth
{
	Saved,
	Saving,
	Failed
};
inline SaveUiHealth computeSaveUiHealth(uint64_t dirtyCoordinates,
                                        uint64_t failedCoordinates, size_t queueDepth)
{
	if (failedCoordinates > 0)
		return SaveUiHealth::Failed;
	if (dirtyCoordinates > 0 || queueDepth > 0)
		return SaveUiHealth::Saving;
	return SaveUiHealth::Saved;
}

/// Whether lastError belongs on the MAIN panel surface (issue #180 review
/// round 7, item 7): yes while persistence is off (an open-world error is
/// still current) or while the health is actively Failed; a RECOVERED
/// failure's error text moves to the details block - history must stay
/// accessible without polluting the main surface. Pure - unit-tested
/// headlessly.
inline bool shouldShowSaveErrorProminently(bool persistenceActive,
                                           SaveUiHealth health, bool hasError)
{
	if (!hasError)
		return false;
	if (!persistenceActive)
		return true;
	return health == SaveUiHealth::Failed;
}

/// Presentation decisions for the World panel, derived ONCE from the frame
/// state (issue #180 review round 9): callers render from this struct so the
/// active/open-failure branching cannot be accidentally coupled to the
/// wrong block again. Pure - unit-tested headlessly.
struct SaveUiPresentation
{
	bool showStatus{false};        ///< "Save Saved/Saving…/Failed" row
	bool showProminentError{false}; ///< red error on the main surface
	SaveUiHealth health{SaveUiHealth::Saved};
};
inline SaveUiPresentation computeSaveUiPresentation(const WorldSaveUiState &state)
{
	SaveUiPresentation out;
	out.showStatus = state.active;
	if (state.active)
	{
		out.health = computeSaveUiHealth(state.dirtyCoordinates,
		                                 state.failedCoordinates, state.queueDepth);
	}
	out.showProminentError = shouldShowSaveErrorProminently(
		state.active, out.health, !state.lastError.empty());
	return out;
}

/// Frame snapshot for ImGui panels (pointers owned by Engine).
/// Debug/telemetry data reaches panels through debugui::UiState snapshots
/// (issue #179); the raw pointers here are the explicit settings structs and
/// the callbacks for actions that need engine coordination. The player is
/// carried as the read-only playerui::PlayerSnapshot value (issue #184), so
/// UI surfaces never touch physics::PlayerController directly.
struct GameUIFrame
{
	Camera *camera{nullptr};
	/// Read-only player view (position/motion/selection context + raw physics
	/// counters for the developer console). Filled by Engine::drawUi.
	playerui::PlayerSnapshot player{};
	std::function<void(bool)> setPlayerFlight;
	std::function<void(CameraMode)> setCameraMode;
	/// Camera tuning commands (issue #184 review): the panel adjusts these
	/// through Engine instead of writing the Camera directly, keeping the
	/// UI surfaces snapshot-only for display data.
	std::function<void(float)> setCameraMovementSpeed;
	std::function<void(float)> setMouseSensitivity;
	std::function<void(float)> setIsometricZoom;
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
	/// World-save state (issue #180, Phase 5); see WorldSaveUiState above.
	WorldSaveUiState worldSave;
	float fps{0.f};
	float frameMs{0.f};
	/// Hierarchical-profiler CPU frame time (events -> present), used by
	/// the shell status strip; frameMs is the paced simulation delta.
	float cpuFrameMs{0.f};
	size_t drawCount{0};
	int windowW{0};
	int windowH{0};
	uint32_t vkApiVersion{0};
	const char *deviceName{nullptr};
	const char *presentModeName{nullptr};
	bool validation{false};

	std::function<void(bool)> setVSync;
	/// True while a VSync change awaits the deferred swapchain recreate
	/// (Graphics ▸ Display marks the present mode "(applying)").
	bool vsyncPending{false};

	// Application shell wiring (issue #183): current UI scale plus callbacks
	// into Engine / ImGuiLayer. setUiScale applies at the next frame boundary
	// (never mid-frame); requestExit mirrors the Escape quit path.
	float uiScale{1.f};
	std::function<void(float)> setUiScale;
	std::function<void()> requestExit;

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
/// GameUI itself is only the shell: main menu bar, read-only Status Overlay
/// (F1), the interactive Player / Gameplay panel, biome-map window, help,
/// on-screen hints, input shortcuts and biome-map plumbing (issue #184).
/// Every developer-tool panel (Overview, Performance, Rendering, Streaming,
/// Player Diagnostics, Chunk inspector, Memory, Benchmark) lives in
/// src/Engine/DebugUI/ and consumes the read-only snapshots in
/// debugui::UiState (issue #179).
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

	bool showStatusOverlay() const
	{
		return m_debug.panels.statusOverlay != playerui::StatusOverlayDensity::Off;
	}
	bool showGraphics() const { return m_debug.panels.rendering; }
	bool showPlayerPanel() const { return m_debug.panels.playerPanel; }
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

	void setShowStatusOverlay(bool v)
	{
		m_debug.panels.statusOverlay = v ? playerui::StatusOverlayDensity::Detailed
										 : playerui::StatusOverlayDensity::Off;
	}

	/// True when a visible UI surface needs the player biome sampled this
	/// frame (issue #184 review): only the Detailed overlay displays it, so
	/// Engine::drawUi can skip the query entirely in every other state.
	bool needsPlayerBiome() const
	{
		return playerui::statusOverlayNeedsBiome(m_debug.panels.statusOverlay);
	}

	/// Application shell (dockspace, menus, layout actions). Engine queues the
	/// default developer layout here on first run.
	ui::UiShell &shell() { return m_shell; }

	/// Open the primary panels used by the default developer layout so the
	/// docked slots materialize on the first frame. Overview stays closed to
	/// limit DebugUI consumers on first launch.
	void enableDefaultDeveloperPanels()
	{
		m_debug.panels.rendering = true;
		m_debug.panels.streaming = true;
		m_debug.panels.world = true;
		m_debug.panels.performance = true;
		m_debug.panels.help = true;
	}

	/// Invalidate any active or in-flight biome map task and clear current texture.
	/// Supersedes existing request ID and marks backing texture as inactive.
	void invalidateBiomeMap();
	bool isBiomeMapPending() const { return m_mapJob.isRunning(); }
	uint64_t currentWorldGenId() const { return m_currentWorldGenId; }
	uint64_t currentMapRequestId() const { return m_mapRequestId; }

	bool hasPendingBiomeMapUpload() const { return m_mapPresentation.hasPending(); }
	const BiomeMapUpload &pendingBiomeMapUpload() const { return m_mapPresentation.pending; }
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

	void drawStatusOverlay(GameUIFrame &frame);
	void drawPlayerPanel(GameUIFrame &frame);
	void drawWorld(GameUIFrame &frame);
	void drawHelp(GameUIFrame &frame);
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

		// Drops the pending upload (pixels + its grid). The PUBLISHED grid is
		// intentionally untouched: the older texture remains displayed and
		// must keep its own mapping (issue #191 review).
		m_mapPresentation.dropPending();
		m_mapNeedsUpdate = true;
	}

	void tickBiomeMap(GameUIFrame &frame);
	void ensureBiomeTexture(int size);

	/// Developer-console state: panel visibility + debug snapshots + bounded
	/// histories (issue #179). Refreshed once per draw by updateDebugUiState.
	debugui::UiState m_debug{};

	/// One-shot: Help window opens with the requested tab selected.
	ui::HelpTabRequest m_helpTabRequest{ui::HelpTabRequest::None};

	/// Application shell (dockspace, main menu, status strip, layout).
	ui::UiShell m_shell{};

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
	/// Published texture + staged upload for the World panel (issue #191
	/// review round 2): publishedGrid/hasTexture describe exactly what ImGui
	/// samples in the current frame (overlays read them during the UI
	/// build); a freshly accepted result waits in `pending` and is published
	/// — pixels and grid together — by recordPendingBiomeMapUpload, which
	/// Engine records AFTER the ImGui pass. No double buffering: each frame
	/// builds from the last publication.
	BiomeMapPresentationState m_mapPresentation{};
	/// World-panel overlay toggles (issue #186 §8).
	bool m_mapShowViewDistance{true};
	bool m_mapShowChunkMarker{true};

	BiomeMapJob m_mapJob{};
	uint64_t m_mapCaptureEpoch{0};
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
};
