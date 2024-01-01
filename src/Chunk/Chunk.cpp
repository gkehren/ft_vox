#include "Chunk.hpp"

Chunk::Chunk(const glm::vec3& pos) : position(pos)
{
	glm::vec3 halfSize(WIDTH / 2.0f, HEIGHT / 2.0f, DEPTH / 2.0f);
	float diagonal = glm::length(halfSize);
	radius = diagonal * 0.5f;

	this->generate();
}

Chunk::~Chunk()
{}

void	Chunk::generate()
{
	for (int x = 0; x < WIDTH; x++) {
		for (int z = 0; z < DEPTH; z++) {
			//TextureType randomTexture = static_cast<TextureType>(std::rand() % TEXTURE_COUNT);
			this->voxels.push_back(Voxel(glm::vec3(x, 0, z) + position, TEXTURE_COBBLESTONE));
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

std::vector<Voxel>&	Chunk::getVoxels()
{
	return (this->voxels);
}

std::vector<Voxel>&	Chunk::getVoxelsSorted(const glm::vec3& position)
{
	std::sort(this->voxels.begin(), this->voxels.end(), [position](const Voxel& a, const Voxel& b) {
		float distA = glm::distance(position, a.getPosition());
		float distB = glm::distance(position, b.getPosition());
		return distA < distB;
	});
	return (this->voxels);
}

float	Chunk::getRadius() const
{
	return (this->radius);
}
