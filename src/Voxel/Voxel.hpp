#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <unordered_set>
#include <functional>

class Voxel {
	public:
		Voxel();
		Voxel(glm::vec3 position, glm::vec3 color);
		~Voxel();

		glm::mat4	getModelMatrix() const;
		glm::vec3	getPosition() const;
		glm::vec3	getColor() const;
		float		getSize() const;
		bool		isVisible() const;

		void		setPosition(glm::vec3 position);
		void		setColor(glm::vec3 color);
		void		setVisible(bool visible);

	private:
		glm::vec3	position;
		glm::vec3	color;
		bool		visible;
};
