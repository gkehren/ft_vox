#pragma once

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <string>

class Voxel {
	public:
		Voxel();
		Voxel(glm::vec3 position, short type);
		~Voxel();

		bool		isVisible() const;
		void		setVisible(bool visible);

		glm::mat4	getModelMatrix() const;
		glm::vec3	getPosition() const;
		float		getSize() const;
		short		getType() const;

		void		setPosition(glm::vec3 position);

	private:
		glm::vec3	position;
		short		type;
		bool		visible;
};
