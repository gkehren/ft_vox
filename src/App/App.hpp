#pragma once

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include <stdexcept>

class App {
	public:
		App();
		~App();
		void run();

	private:
		GLFWwindow* window;
};
