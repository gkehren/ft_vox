#pragma once

#include <glad/glad.h>
#include <SDL3/SDL.h>
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
#include <Biome/BiomeManager.hpp>
#include <Engine/EngineDefs.hpp>
#include <Engine/UIManager.hpp>
#include <Engine/ChunkManager.hpp> // Include ChunkManager

class Engine
{
public:
	Engine();
	~Engine();
	void run();
	void perlinNoise(unsigned int seed);

	// Getters for UIManager and other components
	Camera &getCamera() { return camera; }
	const Camera &getCamera() const { return camera; }
	double getDeltaTime() const { return deltaTime; }
	// const glm::ivec2 &getPlayerChunkPos() const { return playerChunkPos; } // playerChunkPos is now managed internally by Engine, UIManager can get it if needed via a new getter
	const glm::ivec2 &getPlayerChunkPos() const { return playerChunkPos; } // Keep for UIManager for now, or UIManager can get from Engine
	siv::PerlinNoise *getNoise() const { return noise.get(); }
	BiomeManager *getBiomeManager() const { return biomeManager.get(); }
	Server *getServer() const { return server.get(); }
	Client *getClient() const { return client.get(); }
	RenderTiming &getRenderTiming();
	TextureType getSelectedTexture() const { return selectedTexture; }
	UIManager *getUIManager() const { return uiManager.get(); }
	Shader *getShader() const { return shader.get(); }		 // Getter for shader
	Renderer *getRenderer() const { return renderer.get(); } // Getter for renderer

	void setWireframeMode(bool enabled);

	void startServer();
	void stopServer();
	void connectToServer(const std::string &ip);
	void disconnectClient();

private:
	SDL_Window *window;
	SDL_GLContext glContext;
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
	std::unique_ptr<siv::PerlinNoise> noise;
	std::unique_ptr<Server> server;
	std::unique_ptr<Client> client;
	std::unique_ptr<BiomeManager> biomeManager;
	std::unique_ptr<UIManager> uiManager;
	std::unique_ptr<ChunkManager> chunkManager; // Added ChunkManager

	Camera camera;
	glm::ivec2 playerChunkPos;
	uint32_t seed; // Perlin noise seed

	TextureType selectedTexture;
	// std::unordered_map<glm::ivec3, Chunk, ivec3_hash> chunks; // Moved to ChunkManager
	// std::queue<glm::ivec3> chunkGenerationQueue; // Moved to ChunkManager
	// mutable std::mutex chunkMutex; // Moved to ChunkManager

	void handleEvents(bool &keyTPressed);

	// void updateChunks(); // Logic moved to ChunkManager
	// void processChunkQueue(); // Logic moved to ChunkManager
	void updateWorldState(); // New method to call ChunkManager updates

	void renderScene(); // Renamed from render to renderScene to avoid conflict with UIManager::render
	// void frustumCulling(); // Logic moved to ChunkManager

	bool isVoxelActive(float x, float y, float z) const; // Will use ChunkManager
	bool raycast(const glm::vec3 &origin, const glm::vec3 &direction, float maxDistance, glm::vec3 &hitPosition, glm::vec3 &previousPosition);

	// Voxel selection highlight
	struct VoxelHighlight
	{
		bool active{false};
		glm::vec3 position{0.0f};
		glm::vec3 color{0.8f, 0.2f, 0.2f}; // Red for destruction
	} destructionHighlight, placementHighlight;

	void updateVoxelHighlights();
	void drawVoxelHighlight(const VoxelHighlight &highlight);
};
