#pragma once

#include <SDL3/SDL.h>

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
#include <Engine/ThreadPool.hpp>
#include <Engine/EngineDefs.hpp>
#include <Engine/Benchmark.hpp>
#include <Chunk/Chunk.hpp>
#include <Chunk/ChunkPool.hpp>
#include <Chunk/ChunkManager.hpp>
#include <Chunk/TerrainGenerator.hpp>
#include <Camera/Camera.hpp>
#include <Physics/PlayerController.hpp>
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
	int worldSeed() const { return seed; }
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
	void reloadWorld(int newSeed);

	/// Monotonically increasing world generation / reload version counter.
	uint64_t worldGenerationId() const { return m_worldGenerationId; }

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
	void updateHighlight();
	bool raycastVoxel(glm::vec3 &outBlock, glm::vec3 &outPrevious);
	void drawUi();
	void tickBenchmark(double dt);
	void sampleBenchmarkFrame();
	/// Publish main-thread memory/workload gauges every frame (issue #179):
	/// live, read-only data source for the developer console.
	void publishFrameTelemetry();
	void placeCameraOnSurface();
	void setPlayerFlight(bool enabled);
	void resetPlayerAtCamera();
	void updateDisplayRefreshRate();
	/// Re-synchronize the high-resolution frame clock to "now" (issue: a
	/// non-simulated stretch — minimized window, failed acquire, UI stall —
	/// must never feed its elapsed time into the next gameplay timestep).
	void resetFrameClock();

	SDL_Window *window{nullptr};
	int windowWidth{1920};
	int windowHeight{1080};

	bool running{false};
	bool mouseCaptured{true};
	bool m_swapchainRecreateRequested{false};
	std::optional<bool> m_pendingVSync;
	bool showChunkBorders{false};
	bool showDemoPlayers{true};
    bool mobsEnabled{true};
    entities::MobSystem mobs;
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

	int seed{0};
	uint64_t m_worldGenerationId{1};
	/// Requested world-save name (issue #180); empty = transient session.
	std::string m_openWorldName;
	/// Why persistence is off for this session despite a requested world
	/// (seed mismatch / open failure); surfaced in the World UI panel.
	std::string m_openWorldError;
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
	TextureType selectedTexture{STONE};

	static constexpr int kBootstrapRadius = 2;

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
	std::unique_ptr<TerrainGenerator> terrainGenerator;
	std::unique_ptr<ThreadPool> threadPool;
	std::unique_ptr<ChunkPool> chunkPool;
	std::unique_ptr<ChunkManager> chunkManager;

	StagingRing stagingRing;
	GpuResourceRetire resourceRetire;
	uint64_t frameNumber{0};

	std::vector<Chunk *> drawList;
	std::vector<Chunk *> shadowList;
	int uploadBudgetThisFrame{0};
	Camera camera;
	physics::PlayerController player;
	bool playerFlight{false};
	bool windowFocused{true};
	bool jumpPressed{false};
	bool benchmarkOwnedCamera{false};
	bool flightBeforeBenchmark{false};
	const char *playerStatus{""};

	OverlayHighlight highlight{};
	std::vector<OverlayPlayer> demoPlayers;

	Benchmark m_benchmark{};
	bool m_exitAfterBenchmark{false};
};
