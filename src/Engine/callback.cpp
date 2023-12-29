#include "Engine.hpp"

static double lastX = 0.0f;
static double lastY = 0.0f;
static bool firstMouse = true;
static bool cameraEnabled = true;

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

	if (cameraEnabled)
	{
		Camera* camera = static_cast<Camera*>(glfwGetWindowUserPointer(window));
		camera->processMouseMovement(xoffset, yoffset);
	}
}

void	key_callback(GLFWwindow* window, int key, int scancode, int action, int mods)
{
	if (key == GLFW_KEY_C && action == GLFW_PRESS)
	{
		cameraEnabled = !cameraEnabled;
		glfwSetInputMode(window, GLFW_CURSOR, cameraEnabled ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
	}
}
