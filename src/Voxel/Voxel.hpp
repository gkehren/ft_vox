#pragma once

#include <glm/glm.hpp>

class Voxel {
	public:
		Voxel();
		Voxel(glm::vec3 position, glm::vec3 color);
		~Voxel();

		glm::vec3	getPosition() const;
		glm::vec3	getColor() const;

		void	setPosition(glm::vec3 position);
		void	setColor(glm::vec3 color);

	private:
		glm::vec3	position;
		glm::vec3	color;
};
