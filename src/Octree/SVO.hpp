#pragma once

#include <memory>
#include <Octree/Node.hpp>

class SVO
{
	public:
		std::shared_ptr<Node> root;

		SVO() {
			root = std::make_shared<Node>();
		}

		// Add a voxel at the given position (x, y, z)
		void addVoxel(int x, int y, int z, int depth, TextureType type) {
			//addVoxel(root.get(), x, y, z, depth, type);
			Node* currentNode = root.get();

			while (depth > 1) {
				int half = depth * 0.5f;
				int childIndex = ((x >= half) << 2) | ((y >= half) << 1) | (z >= half);

				if (!currentNode->children[childIndex]) {
					currentNode->createChild(childIndex);
				}

				currentNode = currentNode->getChild(childIndex);
				x %= half;
				y %= half;
				z %= half;
				depth = half;
			}

			currentNode->isLeaf = true;
			currentNode->voxelType = type;
		}

		// Delete a voxel at the given position (x, y, z)
		void deleteVoxel(int x, int y, int z, int depth) {
			deleteVoxel(root.get(), x, y, z, depth);
		}

		// Get the type of the voxel at the given position (x, y, z)
		TextureType getVoxel(int x, int y, int z, int depth) {
			Node* currentNode = root.get();

			while (currentNode && depth > 1) {
				int half = depth * 0.5f;
				int childIndex = ((x >= half) << 2) | ((y >= half) << 1) | (z >= half);

				currentNode = currentNode->getChild(childIndex);
				if (!currentNode) return TEXTURE_AIR;

				x %= half;
				y %= half;
				z %= half;
				depth = half;
			}

			return currentNode && currentNode->isLeaf ? currentNode->voxelType : TEXTURE_AIR;
		}

		// Get the root node
		Node* getRoot() {
			return root.get();
		}

	private:
		void addVoxel(Node* node, int x, int y, int z, int depth, TextureType type) {
			if (depth == 0) {
				node->voxelType = type;
				node->isLeaf = true;
				return;
			}

			int index = getChildIndex(x, y, z, depth);
			node->createChild(index);
			addVoxel(node->getChild(index), x, y, z, depth - 1, type);
		}

		void deleteVoxel(Node* node, int x, int y, int z, int depth) {
			if (depth == 0) {
				node->voxelType = TEXTURE_AIR;
				return;
			}

			int index = getChildIndex(x, y, z, depth);
			if (node->children[index]) {
				deleteVoxel(node->children[index].get(), x, y, z, depth - 1);

				// If the child node is empty and all its children are empty, delete it
				if (isNodeEmpty(node->children[index].get())) {
					node->deleteChild(index);
				}
			}
		}

		// Return the index of the child node at the given position
		int getChildIndex(int x, int y, int z, int depth) {
			int half = 1 << (depth - 1);
			return (x >= half ? 4 : 0) + (y >= half ? 2 : 0) + (z >= half ? 1 : 0);
		}

		// Check if the node is empty (i.e. all its children are empty)
		bool isNodeEmpty(Node* node) {
			if (node->isLeaf) return node->voxelType == TEXTURE_AIR;
			for (const auto& child : node->children) {
				if (child && !isNodeEmpty(child.get())) return false;
			}
			return true;
		}
};