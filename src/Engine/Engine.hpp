#pragma once

#include <SDL3/SDL.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <cstdint>
#include <limits>
#include <thread>

#include <Vulkan/VkContext.hpp>
#include <Vulkan/VkSwapchain.hpp>
#include <Vulkan/VkFrame.hpp>
#include <Vulkan/VkCommands.hpp>
#include <Vulkan/StagingRing.hpp>
#include <Vulkan/GpuResourceRetire.hpp>
#include <Renderer/WorldRenderer.hpp>
#include <Renderer/OverlayRenderer.hpp>
#include <Engine/ImGuiLayer.hpp>
#include <Engine/GameUI.hpp>
#include <Core/ThreadPool.hpp>
#include <Engine/EngineDefs.hpp>
#include <Engine/Benchmark.hpp>
#include <Chunk/Chunk.hpp>
#include <Chunk/ChunkPool.hpp>
#include <Renderer/RenderWorldBridge.hpp>
#include <Sim/GameSimulation.hpp>
#include <Camera/Camera.hpp>
#include <World/WorldPersistence.hpp>
#include <Entities/MobSystem.hpp>
#include <utils.hpp>

/// Vulkan engine: streaming procedural world, post, overlays, ImGui.
class Engine
{
public:
	/// @param resourcePackRoot Minecraft pack root (`assets/minecraft/textures/block/…`).
	///   Empty uses bundled `ressources/textures/`. Caller (main) resolves CLI/env.
	explicit Engine(std::string resourcePackRoot = {});
	~Engine();

	void run();
	void initializeNoiseGenerator(int seed);

	/// Request a persistent world save (issue #180): `<cwd>/saves/<name>/` is
	/// opened (or created) inside initializeNoiseGenerator, edits stream to
	/// disk, and the player state is restored/saved around the session.
	/// Setter only — call BEFORE initializeNoiseGenerator. Empty name (the
	/// default) keeps the classic transient procedural world.
	void requestOpenWorld(const std::string &name) { m_openWorldName = name; }

	void setVSync(bool enabled);
	/// The seed of the currently generated world (tests/tools; the World
	/// panel shows the same value through the UI frame).
	int worldSeed() const { return sim ? sim->seed() : 0; }
	/// Override the persistent-save root (tests/tools). Must be called
	/// BEFORE requestOpenWorld/initializeNoiseGenerator; the runtime default
	/// stays "<cwd>/saves" (issue #180 review round 4).
	void setSavesRootForTests(std::filesystem::path root) { m_savesRoot = std::move(root); }
	/// Read-only test accessors (issue #180 review round 6): restored-player
	/// bootstrap coverage needs to observe the live player and the loaded
	/// chunk set after initializeNoiseGenerator.
	glm::dvec3 playerPositionForTests() const { return sim ? sim->playerFeet() : glm::dvec3(0.0); }
	bool playerWaitingForTerrainForTests() const { return sim && sim->playerSnapshot().waitingForTerrain; }
	const RenderWorldBridge *chunksForTests() const { return renderBridge.get(); }
	/// Epoch-barrier accessors (issue #209): the live projection must carry
	/// the simulation's world generation, and a world transition must drop
	/// every derived per-world snapshot before the next frame.
	uint64_t bridgeWorldGenerationForTests() const { return renderBridge ? renderBridge->worldGenerationId() : 0; }
	size_t drawListSizeForTests() const { return drawList.size(); }
	size_t shadowListSizeForTests() const { return shadowList.size(); }
	size_t mobStateCountForTests() const { return mobStates.size(); }
	void setExitAfterBenchmark(bool enabled) { m_exitAfterBenchmark = enabled; }

	/// Override the streaming front load bias (clamped to the unload-safe
	/// maximum in StreamHelpers.hpp). Call after construction, before run();
	/// writes RenderSettings directly so the value is live for this session.
	void setStreamFrontBias(float bias) { renderSettings.streamFrontBias = normalizedStreamFrontBias(bias); }

	/// Apply a named graphics quality preset (Low/Medium/High/Cinematic) to
	/// the post stack. Applies immediately when the renderer exists,
	/// otherwise on run() — safe to call from CLI parsing before init.
	void setGraphicsQualityPreset(GraphicsQualityPreset preset);

	/// Shadow map resolution override (issue #137): 512/1024/2048/4096. Call
	/// after construction, before run(); the engine recreates the shadow map
	/// deferred on the first frame.
	void setShadowMapSize(int size)
	{
		if (worldRenderer)
			worldRenderer->postSettings().shadowMapSize = size;
	}

    /// Fixed daylight camera for reproducible world-generation visual review.
    /// Call after initializeNoiseGenerator. Exits after seconds (0 = interactive).
    void setInspectionView(glm::vec3 position, float yaw, float pitch, float seconds);

	Benchmark &benchmark() { return m_benchmark; }
	const Benchmark &benchmark() const { return m_benchmark; }

	/// Recreate terrain for seed (device idle). Used by benchmark and tools.
	/// Rebuild the world (benchmark/tools). Returns FALSE and leaves the
	/// previous world authoritative when the reload cannot run safely
	/// (missing engine subsystems, or the persistent world refused to close
	/// - stranded logical edits). Callers must not continue with a
	/// destructive transition on false (issue #180 review round 4).
	bool reloadWorld(int newSeed);

	/// Monotonically increasing world generation / reload version counter.
	uint64_t worldGenerationId() const { return sim ? sim->worldGenerationId() : 0; }

	/// Result of applying a pack (atlas is still usable when pack is invalid — bundled used).
	struct ResourcePackApplyResult
	{
		bool atlasOk{false};	 ///< GPU atlas ready
		bool isError{false};	 ///< invalid pack or hard failure
		bool isWarning{false};	 ///< incomplete pack
		std::string message;	 ///< UI / console facing status
		int packHits{0};
		int packMisses{0};
        int entityHits{0}, entityMisses{0};
	};

	/// Hot-reload block atlas from Minecraft pack root (empty = bundled textures).
	/// Invalid packs still load bundled textures and report isError + message.
	ResourcePackApplyResult applyResourcePack(const std::string &resourcePackRoot);
	const std::string &resourcePackRoot() const { return m_resourcePackRoot; }

private:
	void handleEvents();
	void onResize(int width, int height);
	void requestSwapchainRecreate();
	void recreateSwapchainIfNeeded(uint32_t width, uint32_t height);
	void tickStreaming(double dt);
	void tickDayCycle(double dt);
	void processInput(double dt);
	/// Mob simulation + derived presentation (issue #209 frame step):
	/// sim->advanceMobs, mob snapshots, bridge entity-light sampling and
	/// WorldRenderer::setMobs. The authoritative step never sees the camera
	/// beyond a plain observer point.
	void tickMobs(double dt);
	void updateHighlight();
	bool raycastVoxel(glm::vec3 &outBlock, glm::vec3 &outPrevious);
	void drawUi();
	void tickBenchmark(double dt);
	void sampleBenchmarkFrame();
	/// Publish main-thread memory/workload gauges every frame (issue #179):
	/// live, read-only data source for the developer console.
	void publishFrameTelemetry();
	/// Per-frame presentation uniforms (issue #209 frame step): underwater
	/// sample at the camera eye + WorldRenderer::updateFrameUBO.
	void updateFrameUniforms(uint32_t frameIndex, int pixelW, int pixelH, double currentFrame);
	/// Record the frame's passes (issue #209 frame step). Mesh uploads are
	/// recorded HERE (after acquire) — the only GPU upload point; no device
	/// idle. ImGui draws and the biome-map upload keep their pass ordering.
	void recordFrame(uint32_t frameIndex, uint32_t imageIndex, int uploadBudget);
	/// FPS / world-state window title, once per second of frame clock.
	void updateWindowTitle(double currentFrame);
	void placeCameraOnSurface();
	void setPlayerFlight(bool enabled);
	void resetPlayerAtCamera();
	void updateDisplayRefreshRate();
	/// Re-synchronize the high-resolution frame clock to "now" (issue: a
	/// non-simulated stretch — minimized window, failed acquire, UI stall —
	/// must never feed its elapsed time into the next gameplay timestep).
	void resetFrameClock();

	/// Host suspension policy (issue #209, Epic #204 Phase E). The POLICY of
	/// when gameplay freezes lives here; the simulation implements the freeze
	/// semantics (accumulators cleared, previous/current synced, no catch-up
	/// burst on resume). Player and mobs suspend while paused, unfocused or
	/// during an active benchmark. The full viewport-freeze matrix:
	///   * pause (P / UI toggle): player + mobs frozen, day cycle frozen,
	///     streaming keeps visibility/draw lists live but dispatches no jobs
	///     and records no uploads; atmosphere still tracks settings edits.
	///   * focus lost / gained: player suspended on BOTH transitions (buffered
	///     input dropped); mobs suspended while unfocused.
	///   * minimized / hidden window: whole frames skipped; the frame clock
	///     resyncs AFTER the sleep so the skipped stretch never feeds a
	///     gameplay timestep on restore.
	///   * benchmark: player suspended (camera is benchmark-owned), mobs
	///     disabled; input locked.
	///   * inspection view (--view): processInput skipped entirely (fixed
	///     daylight camera), mobs suspended, UI hidden.
	bool simulationSuspended() const { return paused || !windowFocused || m_benchmark.isActive(); }

	/// World-generation epoch barrier (issue #209, Epic #204 Phase E).
	/// Called on EVERY authoritative world transition (initialize / reload),
	/// BEFORE the old render bridge is detached: the derived lists reference
	/// its handles, and every per-world CPU snapshot (draw/shadow lists, mob
	/// presentation states, block highlight, upload budget, UI biome caches,
	/// biome map) must be dropped so nothing stale can feed the new
	/// projection. The simulation's worldGenerationId() is the epoch token;
	/// the replacement bridge is stamped with it at attach. Async safety is
	/// owned by the bridge destructor (quiesces every in-flight job against
	/// the still-live old world) — this barrier clears the CPU-side derived
	/// state that outlives the bridge handles.
	void resetWorldProjection();

	SDL_Window *window{nullptr};
	int windowWidth{1920};
	int windowHeight{1080};

	bool running{false};
	bool mouseCaptured{true};
	bool m_swapchainRecreateRequested{false};
	std::optional<bool> m_pendingVSync;
	bool showChunkBorders{false};
	bool showDemoPlayers{true};
	/// Derived per-frame mob presentation states (sim snapshots + the
	/// bridge's entity light samples). The authoritative mob simulation lives
	/// in sim::GameSimulation (issue #208).
	std::vector<entities::MobRenderState> mobStates;
	bool paused{false};
    bool m_inspectionView{false};
    float m_inspectionSeconds{0.f};
    std::optional<GraphicsQualityPreset> m_pendingQualityPreset;

	double deltaTime{0.0};
	double lastFrame{0.0};
	double frameCount{0.0};
	double lastTime{0.0};
	double fps{0.0};
	uint64_t m_perfFrequency{0};
	uint64_t m_lastPerfCounter{0};
	float m_displayRefreshRate{0.0f};

	double streamAccum{0.0};

	/// Requested world-save name (issue #180); empty = transient session.
	/// Consumed by sim::GameSimulation::initialize (the seed, the world
	/// generation id and the open error live in the simulation, issue #208).
	std::string m_openWorldName;
	/// Root directory of persistent saves (issue #180 review round 4):
	/// "<cwd>/saves" at runtime, injected for tests/tools.
	std::filesystem::path m_savesRoot{"saves"};
	/// UI biome cache (issue #184 review): the Detailed overlay is the only
	/// biome consumer, and the player can stay in one voxel column for many
	/// frames — so drawUi samples getBiomeAt() only when the column or the
	/// world generation changes instead of every frame. Keying on the
	/// generation id makes reloads/seed changes invalidate the cache without
	/// a dedicated reset path.
	glm::ivec2 m_uiBiomeColumn{std::numeric_limits<int>::min(),
							   std::numeric_limits<int>::min()};
	uint64_t m_uiBiomeWorldGen{0};
	int m_uiBiome{-1};
	std::string m_resourcePackRoot;

	RenderSettings renderSettings{};
	RenderTiming renderTiming{};
	ShaderParameters shaderParams{};

	std::unique_ptr<VkContext> vkContext;
	std::unique_ptr<VkSwapchain> swapchain;
	std::unique_ptr<VkFrameContext> frameCtx;
	std::unique_ptr<ImmediateCommands> immediate;
	std::unique_ptr<WorldRenderer> worldRenderer;
	std::unique_ptr<ImGuiLayer> imgui;
	std::unique_ptr<GameUI> gameUi;
	std::unique_ptr<ThreadPool> threadPool;
	std::unique_ptr<ChunkPool> chunkPool;
	// Authoritative simulation + derived render projection (Epic #204,
	// issues #208/#209). GameSimulation owns the TerrainGenerator,
	// VoxelWorld, PlayerController and MobSystem (CPU-only ft_vox_core); the
	// bridge only references sim->world() and is stamped with the sim's
	// world generation at attach. Declaration order is destruction order
	// reversed: the bridge drains its render jobs and releases its handles
	// BEFORE the simulation (and the pools/workers it references) tear down.
	// The Engine is the graphical host: SDL/Vulkan/ImGui, camera, pause /
	// focus / benchmark policy and the epoch barrier — no authoritative
	// gameplay state lives here.
	std::unique_ptr<sim::GameSimulation> sim;
	std::unique_ptr<RenderWorldBridge> renderBridge;

	StagingRing stagingRing;
	GpuResourceRetire resourceRetire;
	uint64_t frameNumber{0};

	std::vector<Chunk *> drawList;
	std::vector<Chunk *> shadowList;
	int uploadBudgetThisFrame{0};
	Camera camera;
	bool windowFocused{true};
	bool jumpPressed{false};
	bool benchmarkOwnedCamera{false};
	bool flightBeforeBenchmark{false};

	OverlayHighlight highlight{};
	std::vector<OverlayPlayer> demoPlayers;

	Benchmark m_benchmark{};
	bool m_exitAfterBenchmark{false};
};
