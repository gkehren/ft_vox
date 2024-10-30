#pragma once

#include "OctreeNode.hpp"

class Octree
{
	public:
		Octree(const glm::vec3& position, int size);

		void setVoxel(const glm::vec3& position, TextureType type);
		TextureType getVoxel(const glm::vec3& position) const;
		void optimize();

	private:
		std::unique_ptr<OctreeNode> root;
};
