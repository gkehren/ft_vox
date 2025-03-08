#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <SDL3/SDL.h>
#include <algorithm>

class Camera
{
public:
	Camera(glm::vec3 position = glm::vec3(0.0f, 100.0f, 0.0f), glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f), float yaw = -90.0f, float pitch = 0.0f, SDL_Window *window = nullptr);

	void setWindow(SDL_Window *window);

	glm::mat4 getViewMatrix() const;
	glm::mat4 getProjectionMatrix(float screenWidth, float screenHeight, float farPlane) const;
	glm::vec3 getPosition() const;
	glm::vec3 getFront() const;
	float getMovementSpeed() const;

	void processKeyboard(double deltaTime, const bool *keys);
	void processMouseMovement(float xoffset, float yoffset, bool constrainPitch = true);

private:
	SDL_Window *window;
	glm::vec3 position;
	glm::vec3 front;
	glm::vec3 up;
	glm::vec3 right;
	glm::vec3 worldUp;

	float yaw;
	float pitch;

	float movementSpeed;
	float mouseSensitivity;

	void updateCameraVectors();
};
