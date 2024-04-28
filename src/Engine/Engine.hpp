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
		int						windowWidth;
		int						windowHeight;
		GLFWmonitor*			monitor;
		const GLFWvidmode*		mode;

		float					deltaTime;
		float					lastFrame;
		float					frameCount;
		float					lastTime;
		float					fps;
		bool					paused;

		Shader*					shader;
		Renderer*				renderer;
		TextRenderer*			textRenderer;
		Camera					camera;

		glm::ivec2				playerChunkPos;

		void	handleInput(bool& keyTPressed);
		void	updateUI();
		bool	wireframeMode;
		bool	chunkBorders;
		int		visibleChunksCount;
		int		visibleVoxelsCount;
		int		chunkLoadedMax;

		int			minRenderDistance;
		int			maxRenderDistance;
		TextureType	selectedTexture;
		siv::PerlinNoise*	perlin;
		std::unordered_map<glm::ivec3, Chunk, ivec3_hash>	chunks;

		void	render();
		void	frustumCulling();
		void	chunkManagement();
};

void	mouse_callback(GLFWwindow* window, double xpos, double ypos);
void	key_callback(GLFWwindow* window, int key, int scancode, int action, int mods);
