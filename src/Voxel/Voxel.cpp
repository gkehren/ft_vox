#include "Voxel.hpp"

Voxel::Voxel()
{}

Voxel::Voxel(glm::vec3 position, glm::vec3 color) : position(position), color(color)
{}

Voxel::~Voxel()
{}

glm::vec3	Voxel::getPosition() const
{
	return this->position;
}

glm::vec3	Voxel::getColor() const
{
	return this->color;
}

void	Voxel::setPosition(glm::vec3 position)
{
	this->position = position;
}

void	Voxel::setColor(glm::vec3 color)
{
	this->color = color;
}
