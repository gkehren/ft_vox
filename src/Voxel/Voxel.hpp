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

		glm::mat4	getModelMatrix() const;
		glm::vec3	getPosition() const;
		float		getSize() const;
		short		getType() const;
		bool		isVisible() const;

		void		setPosition(glm::vec3 position);
		void		setVisible(bool visible);

	private:
		glm::vec3	position;
		short		type;
		bool		visible;
};
