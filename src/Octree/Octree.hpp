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
		TextureType getType() const;
		glm::vec3 getPosition() const;

		void setVoxel(const glm::vec3& pos, TextureType type);
		TextureType getVoxel(const glm::vec3& pos) const;
		void optimize();

		struct NodeMask
		{
			uint8_t	validMask; // Indicate which children exist
			uint8_t leafMask; // Indicate which children are leaves
			uint8_t voxelMask; // Indicate which children contain a voxel
		};

	private:
		glm::vec3 position;
		int size;
		TextureType type;
		NodeMask mask;
		bool needsUpdate;
		std::array<std::unique_ptr<OctreeNode>, 8> children;

		void split();
		int getOctant(const glm::vec3& pos) const;
		bool hasVoxel(int childIndex) const;
};

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
