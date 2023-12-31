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
#include <algorithm>

#include <Shader/Shader.hpp>
#include <Chunk/Chunk.hpp>
#include <Renderer/Renderer.hpp>
#include <Camera/Camera.hpp>

#define WINDOW_WIDTH	1920
#define WINDOW_HEIGHT	1080

#ifdef __linux__	// linux
#define BASE_PATH		"/home/gkehren/Documents/ft_vox/ressources/"
#elif __APPLE__	// Mac
#define BASE_PATH		"/Users/gkehren/Documents/ft_vox/ressources/"
#endif


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
		std::vector<Chunk>		chunks;

		int						chunkX;
		int						chunkY;

		float					frustumDistance;

		void	updateUI();
		int		visibleChunksCount;
		int		visibleVoxelsCount;

		// Chunk management
		void	generateChunks();

		void	cullChunks();
		void	frustumCulling(std::vector<Chunk>& visibleChunks);
		void	occlusionCulling(std::vector<Chunk>& visibleChunks);
};

void	mouse_callback(GLFWwindow* window, double xpos, double ypos);
void	key_callback(GLFWwindow* window, int key, int scancode, int action, int mods);
