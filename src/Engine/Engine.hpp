#pragma once

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <imgui/imgui.h>
#include <imgui/ImGuiFileDialog.h>
#include <imgui/imgui_impl_glfw.h>
#include <imgui/imgui_impl_opengl3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_access.hpp>

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <unordered_set>
#include <algorithm>

#include <Shader/Shader.hpp>
#include <Chunk/Chunk.hpp>
#include <Renderer/Renderer.hpp>
#include <Camera/Camera.hpp>
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
		void run();

	private:
		GLFWwindow*				window;

		float					deltaTime;
		float					lastFrame;

		Shader*					shader;
		Shader*					boundingBoxShader;
		Renderer*				renderer;
		Camera					camera;

		int						chunkX;
		int						chunkZ;

		float					frustumDistance;

		void	updateUI();
		int		visibleChunksCount;
		int		visibleVoxelsCount;

		// Chunk management
		void	generateChunks();
		int		renderDistance;
		std::unordered_set<glm::ivec2, ChunkHasher>	chunkPositions;
		std::vector<Chunk>	chunks;
		//std::vector<Chunk>	chunkLoadList;
		//std::vector<Chunk>	chunkRenderList;
		//std::vector<Chunk>	chunkUnloadList;
		//std::vector<Chunk>	chunkVisibilityList;
		//std::vector<Chunk>	chunkSetupList;

		void	render();
		void	frustumCulling();
		void	chunkManagement();
};

void	mouse_callback(GLFWwindow* window, double xpos, double ypos);
void	key_callback(GLFWwindow* window, int key, int scancode, int action, int mods);
