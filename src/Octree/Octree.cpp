#include "Octree.hpp"

Octree::Octree(const glm::vec3& position, int size)
{
	root = std::make_unique<OctreeNode>(position, size);
}

void Octree::setVoxel(const glm::vec3& position, TextureType type)
{
	root->setVoxel(position, type);
}

TextureType Octree::getVoxel(const glm::vec3& position) const
{
	return root->getVoxel(position);
}

void Octree::optimize()
{
	root->optimize();
}
