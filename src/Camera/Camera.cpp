#include "Camera.hpp"

Camera::Camera(glm::vec3 position, glm::vec3 up, float yaw, float pitch, GLFWwindow* window)
	: window(window), position(position), worldUp(up), yaw(yaw), pitch(pitch), movementSpeed(2.5f), mouseSensitivity(0.1f)
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
	return glm::perspective(glm::radians(80.0f), screenWidth / screenHeight, 0.1f, farPlane);
}

glm::vec3	Camera::getPosition() const
{
	return this->position;
}

glm::vec3	Camera::getFront() const
{
	return this->front;
}

float	Camera::getMovementSpeed() const
{
	return this->movementSpeed;
}

void	Camera::processKeyboard(double deltaTime)
{
	if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
		glfwSetWindowShouldClose(window, true);

	float velocity = movementSpeed * static_cast<float>(deltaTime);

	if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS)
		position += glm::normalize(glm::vec3(front.x, 0.0f, front.z)) * velocity;
	if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS)
		position -= glm::normalize(glm::vec3(front.x, 0.0f, front.z)) * velocity;
	if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS)
		position -= right * velocity;
	if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS)
		position += right * velocity;
	if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS)
		position += worldUp * velocity;
	if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS)
		position -= worldUp * velocity;
	if (glfwGetKey(window, GLFW_KEY_X) == GLFW_PRESS) {
		if (this->movementSpeed == 2.5f)
			this->movementSpeed *= 20.0f;
		else
			this->movementSpeed = 2.5f;
	}

	if (position.x >= SHRT_MAX)
		position.x = SHRT_MAX;
	if (position.x <= SHRT_MIN)
		position.x = SHRT_MIN;
	if (position.z >= SHRT_MAX)
		position.z = SHRT_MAX;
	if (position.z <= SHRT_MIN)
		position.z = SHRT_MIN;
	if (position.y >= SHRT_MAX)
		position.y = SHRT_MAX;
	if (position.y <= SHRT_MIN)
		position.y = SHRT_MIN;
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
