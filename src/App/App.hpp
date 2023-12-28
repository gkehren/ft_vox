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

#include <Shader/Shader.hpp>

#define WINDOW_WIDTH	1920
#define WINDOW_HEIGHT	1080
#define VERTEX_PATH		"/home/gkehren/ft_vox/ressources/vertex.glsl"
#define FRAGMENT_PATH	"/home/gkehren/ft_vox/ressources/fragment.glsl"

class App {
	public:
		App();
		~App();
		void run();

	private:
		GLFWwindow*	window;

		Shader*		shader;

		void updateUI();
};
