#include "Voxel.hpp"

Voxel::Voxel()
{}

Voxel::Voxel(glm::vec3 position, glm::vec3 color) : position(position), color(color)
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

void	Voxel::setPosition(glm::vec3 position)
{
	this->position = position;
}

void	Voxel::setColor(glm::vec3 color)
{
	this->color = color;
}

bool Voxel::isSurrounded(const VoxelSet& voxels) const {
	for (int dx = -1; dx <= 1; dx++) {
		for (int dy = -1; dy <= 1; dy++) {
			for (int dz = -1; dz <= 1; dz++) {
				if (dx == 0 && dy == 0 && dz == 0) continue;
				glm::vec3 neighborPosition = this->position + glm::vec3(dx, dy, dz);
				if (voxels.find(Voxel(neighborPosition, glm::vec3(1.0f))) == voxels.end()) {
					return false;
				}
			}
		}
	}
	return true;
}
