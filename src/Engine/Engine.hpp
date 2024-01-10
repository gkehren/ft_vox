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

#include <Chunk/Chunk.hpp>
#include <Shader/Shader.hpp>
#include <Renderer/Renderer.hpp>
#include <Camera/Camera.hpp>
#include <Renderer/TextRenderer.hpp>
#include <utils.hpp>

struct ChunkHasher {
	std::size_t operator()(const glm::ivec2& k) const {
		return ((k.x ^ (k.y << 4)));
	}
};

class Engine {
	public:
		Engine();
		~Engine();
		void	run();
		void	perlinNoise(unsigned int seed);

	private:
		GLFWwindow*				window;

		float					deltaTime;
		float					lastFrame;
		float					frameCount;
		float					lastTime;
		float					fps;

		Shader*					shader;
		Renderer*				renderer;
		TextRenderer*			textRenderer;
		Camera					camera;

		void	updateUI();
		bool	wireframeMode;
		bool	chunkBorders;
		int		visibleChunksCount;
		int		visibleVoxelsCount;
		int		chunkLoadedMax;

		siv::PerlinNoise*		perlin;

		// Chunk management
		int					renderDistance;
		std::unordered_set<glm::ivec2, ChunkHasher>	chunkPositions;
		std::vector<Chunk>	chunks;

		void	render();
		void	frustumCulling();
		void	chunkManagement();
};

void	mouse_callback(GLFWwindow* window, double xpos, double ypos);
void	key_callback(GLFWwindow* window, int key, int scancode, int action, int mods);
