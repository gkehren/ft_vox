#pragma once

#include <SDL3/SDL.h>

#include <filesystem>
#include <future>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <cstdint>
#include <chrono>
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
#include <Engine/InventoryUi.hpp>
#include <Engine/PlayerUiPreferences.hpp>
#include <Engine/PlayerSessionUi.hpp>
#include <Engine/PlayerUiInteraction.hpp>
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
#include <Network/WireProtocol.hpp>
#include <utils.hpp>

namespace gameui { struct InventoryView; }

namespace net::udp { struct Address; }
class GameUiSample;
namespace gameui { struct HudView; }

/// Vulkan engine: streaming procedural world, post, overlays, ImGui.
class Engine
{
public:
	/// @param resourcePackRoot Minecraft pack root (`assets/minecraft/textures/block/…`).
	///   Empty uses bundled `ressources/textures/`. Caller (main) resolves CLI/env.
	explicit Engine(std::string resourcePackRoot = {});
	~Engine();

	void run();
	void enableGameUi(bool standalone = false, bool previewStrip = true);
	void enableUiSpike() { enableGameUi(true); }
	/// Start-with / restore the developer workspace (`--no-dev-ui`). Runtime
	/// changes go through setDeveloperWorkspaceVisible so developer-owned input
	/// is cancelled and the dockspace identity survives (issue #303 F01/F04).
	void setShowDeveloperUi(bool visible) { m_showDeveloperUi = visible; }
	/// Show or hide the complete developer workspace at runtime without losing
	/// the persisted docking layout or restarting the application.
	void setDeveloperWorkspaceVisible(bool visible);
	bool developerWorkspaceVisible() const { return m_showDeveloperUi; }
	void setShowPlayerHud(bool visible) { m_showPlayerHud = visible; }
	void initializeNoiseGenerator(int seed);
	void connectRemote(net::udp::Address address, std::optional<uint32_t> expectedSeed,
					   const net::wire::PlayerKey &key);

	/// Request a persistent world save (issue #180): `<cwd>/saves/<name>/` is
	/// opened (or created) inside initializeNoiseGenerator, edits stream to
	/// disk, and the player state is restored/saved around the session.
	/// Setter only — call BEFORE initializeNoiseGenerator. Empty name (the
	/// default) keeps the classic transient procedural world.
	void requestOpenWorld(const std::string &name) { m_openWorldName = name; }

	void setVSync(bool enabled);
	/// The seed of the currently generated world (tests/tools; the World
	/// panel shows the same value through the UI frame).
	int worldSeed() const;
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
	/// Derived dropped-item presentation count (issue #229 epoch test).
	size_t itemStateCountForTests() const { return m_itemStates.size(); }
	/// Test-only simulation access (the *ForTests hook pattern): lets the
	/// reload test spawn gameplay state before the epoch barrier check.
	sim::GameSimulation &simForTests() { return *sim; }
	/// Test-only mutator (like setSavesRootForTests / the WorldPersistence
	/// *ForTests hooks): drive one real derived-projection frame step
	/// (streaming + mobs) so the per-frame containers hold genuine data before
	/// a reload — this makes the epoch barrier's "derived state dropped" checks
	/// non-vacuous (they fail if resetWorldProjection's clears are removed). The
	/// bootstrapped chunks are meshed + GPU-uploaded, so the streaming step fills
	/// the draw/shadow lists. Natural mob spawning needs streamed terrain beyond
	/// the bootstrap radius (spawn ring 24–80 blocks vs ~32 bootstrapped), so a
	/// sentinel presentation state — parked far below the world so it cannot
	/// render as a phantom — is seeded when the step produced none; the barrier's
	/// mobStates clear is independent of how the snapshot was produced.
	void stepDerivedProjectionForTests(double dt);
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
	uint64_t worldGenerationId() const;

	/// Whether the native inventory currently owns the player screen.
	bool inventoryPanelOpen() const;

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
	friend struct EngineJoinProbe;
	friend struct UiSpikeProbe;
	friend struct GameUiBackendProbe;
	struct RemoteSession;
	void loadJoinPreferences();
	/// Player UI preferences (issue #302): the persisted scale override, loaded
	/// once at startup, applied at the next frame boundary and re-saved when a
	/// player surface raises a request through the settings seam.
	void loadPlayerUiPreferences();
	void persistPlayerUiPreferences();
	void applyPlayerUiScaleRequests();
	void loadHosting();
	void populateHostingUi(GameUIFrame &frame);
	void tickHosting();
	void queueHostedJoin(const hosting::Snapshot &server, const std::string &profile);
	void requestExit();
	void pumpPlayerSession();
	void showMainMenu();
	void queuePlayerSession(gameui::SessionRequest request);
	bool acceptsPlayerSession(gameui::SessionAction action) const;
	bool blocksNetworkSessionRequests() const;
	void startDeferredQuit();
	void noteSessionFramePresented() { m_sessionProgressPresented |= m_playerSessionRequest.has_value(); }
	void completePlayerSession();
	void failPlayerSession(std::string message, bool retryable = true);
	void finishSoloBootstrap(glm::dvec3 center, bool resident);
	struct SoloWorkResult
	{
		std::unique_ptr<sim::GameSimulation> simulation;
		glm::dvec3 center{};
		std::string error;
		bool retryable{true};
	};
	static SoloWorkResult prepareLocalWorld(const std::filesystem::path &root, const std::string &name,
		int seed, gameui::SessionAction action, ThreadPool *pool,
		const std::function<void(const std::filesystem::path &)> &beforeInitialize);
	// Fault/delay seam for the opt-in session lifecycle proof.
	std::function<void(const std::filesystem::path &)> m_beforeLocalWorldInitializeForTests;
	std::future<SoloWorkResult> m_soloWork;
	std::future<gameui::WorldCatalog> m_worldCatalog;
	std::optional<gameui::SessionRequest> m_playerSessionRequest;
	std::optional<gameui::SessionRequest> m_failedPlayerSession;
	bool m_waitingForHostExit{}, m_hostExitFailed{}, m_deferredQuit{}, m_retryablePlayerFailure{};
	bool m_sessionProgressPresented{}, m_leavingToMenu{}, m_useMainMenu{};
	void persistJoinPreferences();
	void populateJoinUi(GameUIFrame &frame);
	void applySessionRequests();
	void prepareRemoteJoin(const client::JoinRequest &request);
	struct NamedProfile
	{
		std::string name;
		net::wire::PlayerKey key;
	};
	std::optional<NamedProfile> loadNamedProfile(std::string_view name, std::string &error) const;
	void startRemote(std::unique_ptr<RemoteSession> remote);
	void resumeSolo();
	void retireWorldProjection();
	bool switchingSession() const { return m_nextRemote || m_returnToSolo || m_leavingToMenu; }
	void pollRemote();
	void sendRemoteInput(const physics::PlayerInput &input);
	void updateRemoteCamera();
	void traceRemoteMotion(double frameDt);
	void updateRemoteEntities();
	void populateRemoteUi(GameUIFrame &frame);
	sim::VoxelTargetTrace remoteTarget() const;
	VoxelWorld *viewWorld() const;
	void selectHotbarSlot(int slot);
	int selectedHotbarSlot() const;
	void dropSelectedItem();
	void sendRemoteVoxelAction(bool placing);
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
	/// Dropped-item simulation step (issue #227, Epic #223 Phase D): the
	/// facade's advanceItems next to the mob step, same host suspension
	/// policy. No presentation consumer yet — #229 renders the snapshots.
	void tickItems(double dt);
	void updateHighlight();
	/// Thin forward to sim::raycastVoxel (issue #227): the targeting RULE
	/// lives in the simulation; the host supplies camera origin/front and
	/// renderSettings.raycastDistance as plain values. Kept as the highlight
	/// + mouse-handler seam (presentation consumers).
	bool raycastVoxel(glm::vec3 &outBlock, glm::vec3 &outPrevious);
	void drawUi();
	/// Push the developer-key overlay toggles (chunk borders, demo players) to
	/// the overlay layer. Called by every UI path, including the dockspace-only
	/// frame that draws no developer surface (issue #303 F09).
	void applyOverlayToggles();
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
	void recordFrame(uint32_t frameIndex, uint32_t imageIndex, int uploadBudget,
					 std::optional<uint64_t> gpuTag = std::nullopt);
	bool setMouseCaptured(bool captured);
	void setInventoryOpen(bool open);
	gameui::InventoryView playerInventoryView() const;
	void submitInventoryAction(inventoryui::Action action);
	/// PR #316 F04: the one submission adapter for remote inventory actions —
	/// HUD shortcuts (`selectHotbarSlot`/`dropSelectedItem`) and the native
	/// inventory's gestures route through it. Publishes `pendingInventoryAction`
	/// only after a successful submission so a failed send never leaves stale
	/// slot feedback; the result/reconnect/resync paths clear it.
	bool submitRemoteInventoryAction(inventoryui::Action action);
	void syncUiInput();
	void cancelDeveloperInput();
	void cancelGameUiPointer();
	void clearGameplayInput();
	bool nativePause() const;
	bool suppressDeveloperUi() const;
	/// ImGui can build a frame outside exclusive player modals and proof views.
	/// A hidden workspace still submits its KeepAliveOnly dockspace.
	bool developerUiPresent() const;
	/// The visible workspace may own pointer and keyboard input.
	bool developerUiAvailable() const;
	gameui::HudView playerHudView() const;
	gameui::InputContext inputContext() const;
	ImGuiLayer::PointerHit developerPointerAt(float x, float y) const;
	bool routeUiEvent(const SDL_Event &event);
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
	///     daylight camera), UI hidden, day cycle pinned. Mobs are NOT
	///     suspended by inspection itself — they keep advancing unless the
	///     window is unfocused, a benchmark is active, or the mobs toggle is
	///     off (m_inspectionView is not part of simulationSuspended()).
	bool simulationSuspended() const
	{
		return paused || nativePause() || !windowFocused || m_benchmark.isActive() || m_uiSpike;
	}

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
	/// Derived per-frame dropped-item presentation (issue #229, the mobStates
	/// pattern): id-sorted sim snapshots + interpolated render states fed to
	/// WorldRenderer::setItemEntities. Dropped by the epoch barrier on world
	/// transitions; the authoritative entities live in sim::GameSimulation.
	std::vector<sim::ItemEntitySnapshot> m_itemSnapshots;
	std::vector<ItemRenderState> m_itemStates;
	/// Frame-cadence drain scratch for the item lifecycle stream (reused
	/// buffer; v1 discards drained events — presentation is snapshot-driven).
	std::vector<items::ItemEntityEvent> m_itemEventsScratch;
	/// Bumped on every block-atlas (re)load so the UI icon descriptors
	/// revalidate against the live atlas (issue #229).
	uint64_t m_atlasVersion{1};
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

	// Per-instance streaming budget accumulators (review M5): these used to
	// be function-local statics — process-global state inherited by any
	// second Engine instance (tests, tools). streamAccum was already a
	// member; the whole family now lives on the instance.
	double streamAccum{0.0};
	double m_genAccum{0.0};
	double m_meshAccum{0.0};
	double m_uploadAccum{0.0};
	double m_lightCacheAccum{0.0};
	size_t m_lastLoggedChunks{0};
	double m_lastStreamLogTime{0.0};

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
	std::unique_ptr<GameUiSample> m_playerUi;
	gameui::InputRouter m_inputRouter;
	std::optional<bool> m_captureBeforeScreen;
	uint64_t m_uiScreenRevision{};
	bool m_developerSuppressed{}, m_movementAllowed{}, m_windowHidden{}, m_windowMinimized{}, m_captureFailed{};
	bool m_uiSpike{false};
	bool m_showDeveloperUi{true};
	bool m_showPlayerHud{true};
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
	struct SuspendedSolo
	{
		std::unique_ptr<sim::GameSimulation> simulation;
		Camera camera;
		bool paused;
		float dayTime;
	};
	std::optional<SuspendedSolo> m_suspendedSolo;
	std::unique_ptr<RemoteSession> m_remote;
	std::unique_ptr<RemoteSession> m_nextRemote;
	uint64_t m_remoteRenderEpoch{};
	uint64_t m_nextMotionTraceFile{1};
	bool m_returnToSolo{};
	std::optional<client::JoinRequest> m_joinRequest;
	client::JoinPreferences m_joinPreferences;
	gameui::PlayerUiSettings m_playerUiSettings{};
	std::string m_playerUiError;
	/// True while the in-memory preference is not known to be on disk. A failed
	/// write is retried by the next selection, even of the same value (F17).
	bool m_playerUiDirty{};
	std::vector<std::string> m_joinProfiles;
	std::string m_joinError, m_joinPreferencesError;
	hosting::ServerSupervisor m_hostServer;
	hosting::Snapshot m_hostSnapshot;
	uint64_t m_hostRevision{};
	std::filesystem::path m_hostExecutable, m_hostSavesRoot{"server-saves"};
	std::vector<std::string> m_hostWorlds;
	std::string m_hostError;
	std::optional<std::string> m_hostAutoProfile;
	bool m_exitAfterHostedStop{};
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
	std::optional<items::ItemResult> m_inventoryResult;
	std::chrono::steady_clock::time_point m_inventoryResultUntil{};

	Benchmark m_benchmark{};
	bool m_exitAfterBenchmark{false};
};
