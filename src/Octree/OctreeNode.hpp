#pragma once

#include <array>
#include <memory>
#include <glm/glm.hpp>
#include <Voxel/Voxel.hpp>

class OctreeNode
{
	public:
		OctreeNode(const glm::vec3& position, int size);
		~OctreeNode();

		bool isLeaf() const;
		bool isHomogeneous() const;
		TextureType getType() const;

		void setVoxel(const glm::vec3& pos, TextureType type);
		TextureType getVoxel(const glm::vec3& pos) const;
		void optimize();

	private:
		glm::vec3 position;
		int size;
		TextureType type;
		bool homogeneous;
		std::array<std::unique_ptr<OctreeNode>, 8> children;

		void split();
		int getOctant(const glm::vec3& pos) const;
};