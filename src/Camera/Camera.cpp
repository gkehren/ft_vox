#include "Camera.hpp"

Camera::Camera(glm::vec3 position, glm::vec3 up, float yaw, float pitch, SDL_Window *window)
	: window(window), position(position), worldUp(up), yaw(yaw), pitch(pitch), movementSpeed(2.5f), mouseSensitivity(0.1f)
{
	updateCameraVectors();
}

void Camera::setWindow(SDL_Window *window)
{
	this->window = window;
}

glm::mat4 Camera::getViewMatrix() const
{
	return glm::lookAt(position, position + front, up);
}

glm::mat4 Camera::getProjectionMatrix(float screenWidth, float screenHeight, float farPlane) const
{
	return glm::perspective(glm::radians(80.0f), screenWidth / screenHeight, 0.1f, farPlane);
}

glm::vec3 Camera::getPosition() const
{
	return this->position;
}

glm::vec3 Camera::getFront() const
{
	return this->front;
}

float Camera::getMovementSpeed() const
{
	return this->movementSpeed;
}

void Camera::processKeyboard(double deltaTime, const bool *keys)
{
	float velocity = movementSpeed * static_cast<float>(deltaTime);

	if (keys[SDL_SCANCODE_W])
	{
		position += glm::normalize(glm::vec3(front.x, 0.0f, front.z)) * velocity;
	}
	if (keys[SDL_SCANCODE_S])
	{
		position -= glm::normalize(glm::vec3(front.x, 0.0f, front.z)) * velocity;
	}
	if (keys[SDL_SCANCODE_A])
	{
		position -= right * velocity;
	}
	if (keys[SDL_SCANCODE_D])
	{
		position += right * velocity;
	}
	if (keys[SDL_SCANCODE_SPACE])
	{
		position += worldUp * velocity;
	}
	if (keys[SDL_SCANCODE_LSHIFT])
	{
		position -= worldUp * velocity;
	}
	if (keys[SDL_SCANCODE_X])
	{
		if (this->movementSpeed == 2.5f)
		{
			this->movementSpeed *= 50.0f;
		}
		else
		{
			this->movementSpeed = 2.5f;
		}
	}

	position.x = std::clamp(position.x, static_cast<float>(SHRT_MIN), static_cast<float>(SHRT_MAX));
	position.y = std::clamp(position.y, static_cast<float>(SHRT_MIN), static_cast<float>(SHRT_MAX));
	position.z = std::clamp(position.z, static_cast<float>(SHRT_MIN), static_cast<float>(SHRT_MAX));
}

void Camera::updateCameraVectors()
{
	front.x = cos(glm::radians(yaw)) * cos(glm::radians(pitch));
	front.y = sin(glm::radians(pitch));
	front.z = sin(glm::radians(yaw)) * cos(glm::radians(pitch));
	front = glm::normalize(front);

	right = glm::normalize(glm::cross(front, worldUp));
	up = glm::normalize(glm::cross(right, front));
}

void Camera::processMouseMovement(float xoffset, float yoffset, bool constrainPitch)
{
	xoffset *= mouseSensitivity;
	yoffset *= mouseSensitivity;

	yaw += xoffset;
	pitch -= yoffset;

	if (constrainPitch)
	{
		if (pitch > 89.0f)
			pitch = 89.0f;
		else if (pitch < -89.0f)
			pitch = -89.0f;
	}

	updateCameraVectors();
}
