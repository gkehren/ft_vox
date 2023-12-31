#include "Voxel.hpp"

Voxel::Voxel()
{}

Voxel::Voxel(glm::vec3 position, glm::vec3 color) : position(position), color(color), visible(false)
{}

Voxel::~Voxel()
{}

glm::mat4	Voxel::getModelMatrix() const
{
	glm::mat4	modelMatrix(1.0f);

	modelMatrix = glm::translate(modelMatrix, this->position);
	return modelMatrix;
}

glm::vec3	Voxel::getPosition() const
{
	return this->position;
}

glm::vec3	Voxel::getColor() const
{
	return this->color;
}

float Voxel::getSize() const
{
	return 1.0f;
}

bool	Voxel::isVisible() const
{
	return this->visible;
}

void	Voxel::setPosition(glm::vec3 position)
{
	this->position = position;
}

void	Voxel::setColor(glm::vec3 color)
{
	this->color = color;
}

void	Voxel::setVisible(bool v)
{
	this->visible = v;
}
