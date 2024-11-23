#pragma once

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <imgui/imgui.h>
#include <imgui/ImGuiFileDialog.h>
#include <imgui/imgui_impl_glfw.h>
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


class Engine {
	public:
		Engine();
		~Engine();
		void	run();
		void	perlinNoise(unsigned int seed);

	private:
		GLFWwindow*				window;
		int						windowWidth;
		int						windowHeight;
		GLFWmonitor*			monitor;
		const GLFWvidmode*		mode;

		float					deltaTime;
		float					lastFrame;
		float					frameCount;
		float					lastTime;
		float					fps;

		std::unique_ptr<Shader>				shader;
		std::unique_ptr<Renderer>			renderer;
		std::unique_ptr<TextRenderer>		textRenderer;
		std::unique_ptr<ThreadPool>			threadPool;
		std::unique_ptr<siv::PerlinNoise>	perlin;
		Camera								camera;
		glm::ivec2							playerChunkPos;

		static constexpr int DEFAULT_CHUNK_LOADED_MAX = 5;
		static constexpr float FPS_UPDATE_INTERVAL = 1.0f;

		struct RenderSettings {
			bool	wireframeMode{false};
			bool	chunkBorders{false};
			bool	paused{false};
			bool	perfMode{false};
			int		visibleChunksCount{0};
			int		visibleVoxelsCount{0};
			int		chunkLoadedMax{DEFAULT_CHUNK_LOADED_MAX};
			int		minRenderDistance{224};
			int		maxRenderDistance{320};
		} renderSettings;

		struct RenderTiming {
			float frustumCulling{0.0f};
			float chunkGeneration{0.0f};
			float meshGeneration{0.0f};
			float chunkRendering{0.0f};
			float uiRendering{0.0f};
			float totalFrame{0.0f};
		} renderTiming;

		void	handleInput(bool& keyTPressed);
		void	updateUI();

		TextureType	selectedTexture;
		std::unordered_map<glm::ivec3, Chunk, ivec3_hash>	chunks;
		std::queue<glm::ivec3>								chunkGenerationQueue;
		mutable std::mutex									chunkMutex;

		void	updateChunks();
		void	processChunkQueue();
		bool	isChunkInRange(const glm::ivec3& chunkPos, float distance) const;

		void	render();
		void	frustumCulling();
};

void	mouse_callback(GLFWwindow* window, double xpos, double ypos);
void	key_callback(GLFWwindow* window, int key, int scancode, int action, int mods);
