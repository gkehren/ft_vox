#pragma once

#include <SDL3/SDL.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <cstdint>
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
	void setVSync(bool enabled);
	void setExitAfterBenchmark(bool enabled) { m_exitAfterBenchmark = enabled; }

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
	void placeCameraOnSurface();

	SDL_Window *window{nullptr};
	int windowWidth{1920};
	int windowHeight{1080};

	bool running{false};
	bool mouseCaptured{true};
	bool m_swapchainRecreateRequested{false};
	std::optional<bool> m_pendingVSync;
	bool showChunkBorders{false};
	bool showDemoPlayers{true};
	bool paused{false};

	double deltaTime{0.0};
	double lastFrame{0.0};
	double frameCount{0.0};
	double lastTime{0.0};
	double fps{0.0};

	double streamAccum{0.0};

	int seed{0};
	uint64_t m_worldGenerationId{1};
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

	OverlayHighlight highlight{};
	std::vector<OverlayPlayer> demoPlayers;

	Benchmark m_benchmark{};
	bool m_exitAfterBenchmark{false};
};
