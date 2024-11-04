#pragma once

#include <memory>
#include <optional>
#include <array>
#include <glm/glm.hpp>
#include <utils.hpp>

class Node
{
	public:
		bool isLeaf = false;
		TextureType voxelType = TEXTURE_AIR;
		std::array<std::shared_ptr<Node>, 8> children;
		//std::optional<std::array<std::shared_ptr<Node>, 8>> children;

		Node() = default;

		void createChild(int index) {
			if (index < 0 || index >= 8) return;
			if (!children[index]) { // If the child node does not exist, create it
				children[index] = std::make_shared<Node>();
			}
		}

		Node* getChild(int index) {
			if (index < 0 || index >= 8) return nullptr;
			return children[index].get();
		}

		const Node* getChild(int index) const {
			if (index < 0 || index >= 8) return nullptr;
			return children[index].get();
		}

		void deleteChild(int index) {
			if (index < 0 || index >= 8) return;
			children[index].reset();
		}
};
