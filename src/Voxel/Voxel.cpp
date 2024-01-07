#include "Voxel.hpp"

Voxel::Voxel(glm::vec3 position, TextureType type) : position(position), type(type)
{}

Voxel::~Voxel()
{}

const glm::vec3&	Voxel::getPosition() const
{
	return (this->position);
}

const TextureType&	Voxel::getType() const
{
	return (this->type);
}

void	Voxel::setType(const TextureType& type)
{
	this->type = type;
}
