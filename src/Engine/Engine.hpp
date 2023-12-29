#pragma once

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <imgui/imgui.h>
#include <imgui/ImGuiFileDialog.h>
#include <imgui/imgui_impl_glfw.h>
#include <imgui/imgui_impl_opengl3.h>

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <Shader/Shader.hpp>
#include <Voxel/Voxel.hpp>
#include <Renderer/Renderer.hpp>
#include <Camera/Camera.hpp>

#define WINDOW_WIDTH	1920
#define WINDOW_HEIGHT	1080
#define VERTEX_PATH		"/Users/gkehren/Documents/ft_vox/ressources/vertex.glsl"
#define FRAGMENT_PATH	"/Users/gkehren/Documents/ft_vox/ressources/fragment.glsl"

class Engine {
	public:
		Engine();
		~Engine();
		void run();

	private:
		GLFWwindow*	window;

		float deltaTime;
		float lastFrame;

		Shader*		shader;
		Renderer*	renderer;
		Camera		camera;

		std::vector<Voxel>	voxels;

		void updateUI();
};

void	mouse_callback(GLFWwindow* window, double xpos, double ypos);
