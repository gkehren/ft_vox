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
	if (mode == CameraMode::ISOMETRIC)
	{
		// Compute a fixed look direction from the isometric yaw/pitch angles
		glm::vec3 isoFront;
		isoFront.x = cos(glm::radians(isometricYaw)) * cos(glm::radians(isometricPitch));
		isoFront.y = sin(glm::radians(isometricPitch));
		isoFront.z = sin(glm::radians(isometricYaw)) * cos(glm::radians(isometricPitch));
		isoFront = glm::normalize(isoFront);

		// Place the eye far enough behind the focal point so nothing clips
		glm::vec3 eye = position - isoFront * (isometricZoom * 4.0f);
		return glm::lookAt(eye, position, glm::vec3(0.0f, 1.0f, 0.0f));
	}
	return glm::lookAt(position, position + front, up);
}

glm::mat4 Camera::getProjectionMatrix(float screenWidth, float screenHeight, float farPlane) const
{
	if (mode == CameraMode::ISOMETRIC)
	{
		float aspect = screenWidth / screenHeight;
		float halfH = isometricZoom;
		float halfW = isometricZoom * aspect;
		// Near/far span the whole potential camera arm plus world depth
		return glm::ortho(-halfW, halfW, -halfH, halfH, -farPlane, farPlane);
	}
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

	if (mode == CameraMode::ISOMETRIC)
	{
		// Derive the isometric right/forward axes from the fixed yaw only
		// (horizontal plane movement, ignoring pitch so the view doesn't tilt)
		glm::vec3 isoForward;
		isoForward.x = cos(glm::radians(isometricYaw));
		isoForward.y = 0.0f;
		isoForward.z = sin(glm::radians(isometricYaw));
		isoForward = glm::normalize(isoForward);

		glm::vec3 isoRight = glm::normalize(glm::cross(isoForward, glm::vec3(0.0f, 1.0f, 0.0f)));

		if (keys[SDL_SCANCODE_W])
			position += isoForward * velocity;
		if (keys[SDL_SCANCODE_S])
			position -= isoForward * velocity;
		if (keys[SDL_SCANCODE_A])
			position -= isoRight * velocity;
		if (keys[SDL_SCANCODE_D])
			position += isoRight * velocity;
		if (keys[SDL_SCANCODE_X])
		{
			if (this->movementSpeed == 2.5f)
				this->movementSpeed *= 50.0f;
			else
				this->movementSpeed = 2.5f;
		}

		position.x = std::clamp(position.x, static_cast<float>(SHRT_MIN), static_cast<float>(SHRT_MAX));
		position.y = 100.0f;
		position.z = std::clamp(position.z, static_cast<float>(SHRT_MIN), static_cast<float>(SHRT_MAX));
		return;
	}

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
	// Mouse look is disabled in isometric mode
	if (mode == CameraMode::ISOMETRIC)
		return;

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

void Camera::toggleMode()
{
	mode = (mode == CameraMode::PERSPECTIVE) ? CameraMode::ISOMETRIC : CameraMode::PERSPECTIVE;
}

void Camera::addIsometricZoom(float delta)
{
	isometricZoom = std::clamp(isometricZoom - delta, 8.0f, 512.0f);
}
