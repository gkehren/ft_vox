#pragma once

#include <Engine/EngineDefs.hpp>
#include <Camera/Camera.hpp>
#include <Chunk/ChunkManager.hpp>
#include <Chunk/ChunkPool.hpp>
#include <Chunk/TerrainGenerator.hpp>
#include <Renderer/OverlayRenderer.hpp>
#include <Renderer/TerrainRenderer.hpp>
#include <Vulkan/VkContext.hpp>
#include <Vulkan/VkCommands.hpp>
#include <Vulkan/VkImage.hpp>
#include <utils.hpp>

#include <future>
#include <atomic>
#include <vector>
#include <functional>
#include <cstdint>

/// Frame snapshot for ImGui panels (pointers owned by Engine).
struct GameUIFrame
{
	Camera *camera{nullptr};
	ChunkManager *chunks{nullptr};
	ChunkPool *pool{nullptr};
	TerrainGenerator *generator{nullptr};
	TerrainRenderer *terrain{nullptr};
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
	float fps{0.f};
	float frameMs{0.f};
	size_t drawCount{0};
	int windowW{0};
	int windowH{0};
	uint32_t vkApiVersion{0};
	const char *deviceName{nullptr};
	bool validation{false};

	std::function<void(bool)> setVSync;
};

/// Multi-panel ImGui HUD for the Vulkan engine.
/// Restores prior OpenGL UI features (adapted), with cleaner layout + shortcuts.
class GameUI
{
public:
	GameUI() = default;
	~GameUI();

	void init(VkContext &context, ImmediateCommands &imm);
	void shutdown();

	/// Draw all panels. Call between ImGui beginFrame / endFrame.
	void draw(GameUIFrame &frame);

	/// Keyboard shortcuts that don't need ImGui capture (F-keys, etc.).
	/// Returns true if a toggle consumed the key.
	bool handleShortcut(int sdlKeycode, GameUIFrame &frame);

	bool showHud() const { return m_showHud; }
	bool showGraphics() const { return m_showGraphics; }
	bool showStreaming() const { return m_showStreaming; }
	bool showWorld() const { return m_showWorld; }
	bool showHelp() const { return m_showHelp; }

	void setShowHud(bool v) { m_showHud = v; }

private:
	void drawMenuBar(GameUIFrame &frame);
	void drawHud(GameUIFrame &frame);
	void drawGraphics(GameUIFrame &frame);
	void drawStreaming(GameUIFrame &frame);
	void drawWorld(GameUIFrame &frame);
	void drawHelp();
	void drawOverlayHints(GameUIFrame &frame);

	void tickBiomeMap(GameUIFrame &frame);
	void ensureBiomeTexture(int size);
	void uploadBiomeTexture(const std::vector<unsigned char> &rgb, int size);

	bool m_showHud{true};
	bool m_showGraphics{false};
	bool m_showStreaming{false};
	bool m_showWorld{false};
	bool m_showHelp{false};
	bool m_showOverlayHints{true};

	// Biome map
	int m_mapSize{256};
	float m_mapZoom{0.5f};
	glm::vec2 m_mapCenter{0.f, 0.f};
	bool m_mapFollow{true};
	bool m_mapNeedsUpdate{true};
	double m_mapLastUpdate{0.0};
	glm::vec2 m_mapLastPlayer{0.f, 0.f};

	std::future<void> m_mapFuture;
	std::vector<unsigned char> m_mapPending;
	std::atomic<bool> m_mapReady{false};
	glm::vec2 m_mapPendingCenter{0.f};
	float m_mapPendingZoom{0.5f};
	int m_mapPendingGridX{0};
	int m_mapPendingGridZ{0};

	VkContext *m_vk{nullptr};
	ImmediateCommands *m_imm{nullptr};
	AllocatedImage m_mapImage{};
	VkSampler m_mapSampler{VK_NULL_HANDLE};
	VkDescriptorSet m_mapDesc{VK_NULL_HANDLE};
	int m_mapImageSize{0};
	bool m_mapHasTexture{false};
};
