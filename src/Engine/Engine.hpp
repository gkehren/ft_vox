#pragma once

#include <glad/glad.h>
#include <SDL3/SDL.h>
#include <imgui/imgui.h>
#include <imgui/ImGuiFileDialog.h>
#include <imgui/imgui_impl_sdl3.h>
#include <imgui/imgui_impl_opengl3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_access.hpp>
#include <PerlinNoise/PerlinNoise.hpp>

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <unordered_set>
#include <algorithm>
#include <random>
#include <future>
#include <ranges>
#include <memory>
#include <queue>
#include <mutex>

#include <Chunk/Chunk.hpp>
#include <Shader/Shader.hpp>
#include <Renderer/Renderer.hpp>
#include <Camera/Camera.hpp>
#include <Renderer/TextRenderer.hpp>
#include <utils.hpp>
#include <Engine/ThreadPool.hpp>
#include <Network/Server.hpp>
#include <Network/Client.hpp>
#include <Biome/NoiseGenerator.hpp>

class Engine
{
public:
	Engine();
	~Engine();
	void run();
	void perlinNoise(unsigned int seed);

private:
	SDL_Window *window;
	int windowWidth;
	int windowHeight;
	SDL_DisplayMode *mode;

	bool running;
	double deltaTime;
	double lastFrame;
	double frameCount;
	double lastTime;
	double fps;

	bool isMousecaptured;

	std::unique_ptr<Shader> shader;
	std::unique_ptr<Renderer> renderer;
	std::unique_ptr<TextRenderer> textRenderer;
	std::unique_ptr<ThreadPool> threadPool;
	std::unique_ptr<NoiseGenerator> noise;
	std::unique_ptr<Server> server;
	std::unique_ptr<Client> client;

	Camera camera;
	glm::ivec2 playerChunkPos;
	uint32_t seed; // Perlin noise seed

	struct RenderSettings
	{
		bool wireframeMode{false};
		bool chunkBorders{false};
		bool paused{false};
		int visibleChunksCount{0};
		int visibleVoxelsCount{0};
		int chunkLoadedMax{5};
		int minRenderDistance{320};
		int maxRenderDistance{320};
		int raycastDistance{8};
	} renderSettings;

	struct RenderTiming
	{
		float frustumCulling{0.0f};
		float chunkGeneration{0.0f};
		float meshGeneration{0.0f};
		float chunkRendering{0.0f};
		float uiRendering{0.0f};
		float totalFrame{0.0f};
	} renderTiming;

	ShaderParameters shaderParams;
	void handleShaderOptions();

	void handleEvents(bool &keyTPressed);
	void updateUI();

	TextureType selectedTexture;
	std::unordered_map<glm::ivec3, Chunk, ivec3_hash> chunks;
	std::queue<glm::ivec3> chunkGenerationQueue;
	mutable std::mutex chunkMutex;
	GLuint voxelBuffer;

	void updateChunks();
	void processChunkQueue();
	bool isChunkInRange(const glm::ivec3 &chunkPos, float distance) const;

	void render();
	void frustumCulling();

	bool isVoxelActive(float x, float y, float z) const;
	bool raycast(const glm::vec3 &origin, const glm::vec3 &direction, float maxDistance, glm::vec3 &hitPosition, glm::vec3 &previousPosition);

	// Network
	char ipInputBuffer[128] = "127.0.0.1";
	void handleServerControls();

	GLuint biomeMapTexture = 0;
	const int biomeMapSize = 256;
	std::vector<unsigned char> biomeMapData;

	void generateBiomeMapTexture();
	void updateBiomeMapTexture();
	std::string getCurrentBiomeName() const;
};

// void	mouse_callback(SDL_Window* window, double xpos, double ypos);
// void	key_callback(SDL_Window* window, int key, int scancode, int action, int mods);
