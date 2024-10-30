#include "OctreeNode.hpp"

OctreeNode::OctreeNode(const glm::vec3& position, int size)
	: position(position)
	, size(size)
	, type(TEXTURE_AIR)
	, homogeneous(true)
{
	for (auto& child : children) {
		child = nullptr;
	}
}

OctreeNode::~OctreeNode()
{}

bool OctreeNode::isLeaf() const
{
	return (children[0] == nullptr);
}

bool OctreeNode::isHomogeneous() const
{
	return (homogeneous);
}

TextureType OctreeNode::getType() const
{
	return (type);
}

void OctreeNode::setVoxel(const glm::vec3& pos, TextureType newType)
{
	if (size == 1) {
		type = newType;
		return;
	}

	if (isLeaf() && homogeneous && type == newType) {
		return;
	}

	if (isLeaf()) {
		split();
	}

	int octant = getOctant(pos);
	glm::vec3 childPos = position;
	childPos.x += (octant & 1) ? size / 2 : 0;
	childPos.y += (octant & 2) ? size / 2 : 0;
	childPos.z += (octant & 4) ? size / 2 : 0;

	children[octant]->setVoxel(pos - childPos, newType);
	homogeneous = false;
}

TextureType OctreeNode::getVoxel(const glm::vec3& pos) const
{
	if (size == 1) {
		return (type);
	}

	int octant = getOctant(pos);
	if (children[octant] == nullptr) {
		return TextureType::TEXTURE_AIR;
	}
	return (children[octant]->getVoxel(pos - position));
}

void OctreeNode::optimize()
{
	if (isLeaf()) return;

	bool allSame = true;
	TextureType firstType = children[0]->getType();

	for (int i = 1; i < 8; ++i) {
		children[i]->optimize();
		if (!children[i]->isHomogeneous() || children[i]->getType() != firstType) {
			allSame = false;
			break;
		}
	}

	if (allSame) {
		type = firstType;
		homogeneous = true;
		children = {};
	}
}

int OctreeNode::getOctant(const glm::vec3& pos) const
{
	int octant = 0;
	if (pos.x >= position.x + size / 2) octant |= 1;
	if (pos.y >= position.y + size / 2) octant |= 2;
	if (pos.z >= position.z + size / 2) octant |= 4;
	return octant;
}

void OctreeNode::split()
{
	for (int i = 0; i < 8; ++i) {
		glm::vec3 childPos = position;
		childPos.x += (i & 1) ? size / 2 : 0;
		childPos.y += (i & 2) ? size / 2 : 0;
		childPos.z += (i & 4) ? size / 2 : 0;
		children[i] = std::make_unique<OctreeNode>(childPos, size / 2);
		children[i]->type = type;
	}
}
