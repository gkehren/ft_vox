#include "Chunk.hpp"

Chunk::Chunk(const glm::vec3& pos) : position(pos)
{}

Chunk::~Chunk()
{
}

void	Chunk::generate()
{
	for (int x = 0; x < CHUNK_SIZE; x++) {
		for (int y = 0; y < CHUNK_SIZE; y++) {
			for (int z = 0; z < CHUNK_SIZE; z++) {
				this->voxels.push_back(Voxel(glm::vec3(x, y, z) + position, glm::vec3(1.0f)));
			}
		}
	}
}

std::vector<glm::mat4>	Chunk::getModelMatrices() const
{
	std::vector<glm::mat4>	modelMatrices;
	for (const Voxel& voxel : this->voxels) {
		modelMatrices.push_back(voxel.getModelMatrix());
	}
	return (modelMatrices);
}

const glm::vec3&	Chunk::getPosition() const
{
	return (this->position);
}

const std::vector<Voxel>&	Chunk::getVoxels() const
{
	return (this->voxels);
}
