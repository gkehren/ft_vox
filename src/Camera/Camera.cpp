#include "Camera.hpp"

Camera::Camera(glm::vec3 position, glm::vec3 up, float yaw, float pitch, GLFWwindow* window)
	: position(position), worldUp(up), yaw(yaw), pitch(pitch), movementSpeed(2.5f), mouseSensitivity(0.1f), window(window)
{
	updateCameraVectors();
}

void	Camera::setWindow(GLFWwindow* window) {
	this->window = window;
}

glm::mat4	Camera::getViewMatrix() const
{
	return glm::lookAt(position, position + front, up);
}

glm::mat4	Camera::getProjectionMatrix(float screenWidth, float screenHeight, float farPlane) const
{
	return glm::perspective(glm::radians(45.0f), screenWidth / screenHeight, 0.1f, farPlane);
}

glm::vec3	Camera::getPosition() const
{
	return this->position;
}

float	Camera::getMovementSpeed() const
{
	return this->movementSpeed;
}

void	Camera::processKeyboard(float deltaTime)
{
	if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
		glfwSetWindowShouldClose(window, true);

	float velocity = movementSpeed * deltaTime;

	if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS)
		position += front * velocity;
	if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS)
		position -= front * velocity;
	if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS)
		position -= right * velocity;
	if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS)
		position += right * velocity;
	if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS)
		position += up * velocity;
	if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS)
		position -= up * velocity;
	if (glfwGetKey(window, GLFW_KEY_X) == GLFW_PRESS)
	{
		if (this->movementSpeed == 2.5f)
			this->movementSpeed *= 20.0f;
		else
			this->movementSpeed = 2.5f;
	}
}

void	Camera::updateCameraVectors()
{
	front.x = cos(glm::radians(yaw)) * cos(glm::radians(pitch));
    front.y = sin(glm::radians(pitch));
    front.z = sin(glm::radians(yaw)) * cos(glm::radians(pitch));
	front = glm::normalize(front);

	right = glm::normalize(glm::cross(front, worldUp));
	up = glm::normalize(glm::cross(right, front));
}

void Camera::processMouseMovement(float xoffset, float yoffset, GLboolean constrainPitch)
{
	xoffset *= mouseSensitivity;
	yoffset *= mouseSensitivity;

	yaw += xoffset;
	pitch += yoffset;

	if (constrainPitch) {
		if (pitch > 89.0f)
			pitch = 89.0f;
		else if (pitch < -89.0f)
			pitch = -89.0f;
	}

	updateCameraVectors();
}
