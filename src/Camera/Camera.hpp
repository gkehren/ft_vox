#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <GLFW/glfw3.h>

class Camera {
	public:
		Camera(glm::vec3 position = glm::vec3(0.0f, 100.0f, 3.0f), glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f), float yaw = -90.0f, float pitch = 0.0f, GLFWwindow* window = nullptr);

		void		setWindow(GLFWwindow* window);

		glm::mat4	getViewMatrix() const;
		glm::mat4	getProjectionMatrix(float screenWidth, float screenHeight, float farPlane) const;
		glm::vec3	getPosition() const;
		float		getMovementSpeed() const;

		void		processKeyboard(float deltaTime);
		void		processMouseMovement(float xoffset, float yoffset, GLboolean constrainPitch = true);

	private:
		GLFWwindow*	window;
		glm::vec3	position;
		glm::vec3	front;
		glm::vec3	up;
		glm::vec3	right;
		glm::vec3	worldUp;

		float		yaw;
		float		pitch;

		float		movementSpeed;
		float		mouseSensitivity;

		void		updateCameraVectors();
};
