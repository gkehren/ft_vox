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

#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>

#include <Chunk/Chunk.hpp>
#include <Shader/Shader.hpp>
#include <Renderer/Renderer.hpp>
#include <Camera/Camera.hpp>
#include <Renderer/TextRenderer.hpp>
#include <utils.hpp>


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

		std::vector<std::thread>		threads;

		void	updateUI();
		bool	wireframeMode;
		bool	chunkBorders;
		int		visibleChunksCount;
		int		visibleVoxelsCount;
		int		chunkLoadedMax;

		siv::PerlinNoise*		perlin;

		// Chunk management
		int					renderDistance;
		std::unordered_map<glm::ivec3, Chunk, ivec3_hash>	chunks;

		void	render();
		void	frustumCulling();
		void	chunkManagement();
};

void	mouse_callback(GLFWwindow* window, double xpos, double ypos);
void	key_callback(GLFWwindow* window, int key, int scancode, int action, int mods);
