#include "Chunk.hpp"

Chunk::Chunk(const glm::vec3& pos) : position(pos), position2D(glm::ivec2(pos.x, pos.z)), visible(false)
{
	glm::vec3 halfSize(WIDTH / 2.0f, HEIGHT / 2.0f, DEPTH / 2.0f);
	float diagonal = glm::length(halfSize);
	radius = diagonal * 0.5f;

	//this->generate();
}

Chunk::~Chunk()
{}

void	Chunk::generate()
{
	if (!this->voxels.empty())
		return;
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

const glm::ivec2&	Chunk::getPosition2D() const
{
	return (this->position2D);
}

const std::vector<Voxel>&	Chunk::getVoxels() const
{
	return (this->voxels);
}

const std::vector<Voxel>	Chunk::getVisibleVoxels(const Camera& camera) const
{
	std::vector<Voxel>	visibleVoxels;

	for (auto& voxel : this->voxels) {
		if (isVoxelVisible(camera, voxel)) {
			visibleVoxels.push_back(voxel);
		}
	}

	return (visibleVoxels);
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

bool	Chunk::isVisible() const
{
	return (this->visible);
}

void	Chunk::setVisible(bool visible)
{
	this->visible = visible;
}

bool	Chunk::isVoxelVisible(const Camera& camera, const Voxel& voxel) const
{
	glm::vec3 voxelPos = voxel.getPosition();
	glm::vec3 rayDirection = glm::normalize(voxelPos - camera.getPosition());

	float voxelDistance = glm::length(voxelPos - camera.getPosition());

	for (const auto& otherVoxel : voxels) {
		if (&otherVoxel == &voxel) {
			continue;
		}

		glm::vec3 otherVoxelPos = otherVoxel.getPosition();

		float otherVoxelDistance = glm::length(otherVoxelPos - camera.getPosition());

		if (otherVoxelDistance < voxelDistance) {
			if (isRayIntersectingVoxel(camera.getPosition(), rayDirection, otherVoxelPos)) {
				return false;
			}
		}
	}

	return true;
}

bool	Chunk::isRayIntersectingVoxel(const glm::vec3& rayOrigin, const glm::vec3& rayDirection, const glm::vec3& voxelPosition) const
{
	float voxelSize = 1.0f;
	glm::vec3 min = voxelPosition - glm::vec3(voxelSize / 2.0f);
	glm::vec3 max = voxelPosition + glm::vec3(voxelSize / 2.0f);

	float tMin = 0.0f;
	float tMax = std::numeric_limits<float>::infinity();

	for (int i = 0; i < 3; i++) {
		float invRayDir = 1.0f / rayDirection[i];
		float tNear = (min[i] - rayOrigin[i]) * invRayDir;
		float tFar = (max[i] - rayOrigin[i]) * invRayDir;

		if (invRayDir < 0.0f) {
			std::swap(tNear, tFar);
		}

		tMin = std::max(tNear, tMin);
		tMax = std::min(tFar, tMax);

		if (tMax <= tMin) {
			return false;
		}
	}

	return true;
}
