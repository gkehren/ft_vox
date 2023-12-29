#include "Engine.hpp"

static double lastX = 0.0f;
static double lastY = 0.0f;
static bool firstMouse = true;

void	mouse_callback(GLFWwindow* window, double xpos, double ypos)
{
	if (firstMouse)
	{
		lastX = xpos;
		lastY = ypos;
		firstMouse = false;
	}

	float xoffset = xpos - lastX;
	float yoffset = lastY - ypos; // reversed since y-coordinates go from bottom to top

	lastX = xpos;
	lastY = ypos;

	Camera* camera = static_cast<Camera*>(glfwGetWindowUserPointer(window));
	camera->processMouseMovement(xoffset, yoffset);
}
