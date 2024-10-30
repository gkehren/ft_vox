#include "Octree.hpp"

OctreeNode::OctreeNode(const glm::vec3& position, int size)
	: position(position)
	, size(size)
	, type(TEXTURE_AIR)
	, mask{0, 0, 0}
	, children{nullptr}
{}

OctreeNode::~OctreeNode()
{}

bool OctreeNode::isLeaf() const
{
	return children[0] == nullptr;
}

TextureType OctreeNode::getType() const
{
	return type;
}

glm::vec3 OctreeNode::getPosition() const
{
	return position;
}

void OctreeNode::setVoxel(const glm::vec3& pos, TextureType newType)
{
	needsUpdate = true;

	if (size == 1) {
		type = newType;
		mask.voxelMask = (newType != TEXTURE_AIR) ? 0xFF : 0x00;
		return;
	}

	int octant = getOctant(pos);

	// if it's a leaf and we try to add air, we can optimize
	if (isLeaf() && type != TEXTURE_AIR && type != newType) {
		split();
	}

	// Create children
	if (!children[octant]) {
		glm::vec3 childPos = position;
		childPos.x += (octant & 1) ? size / 2 : 0;
		childPos.y += (octant & 2) ? size / 2 : 0;
		childPos.z += (octant & 4) ? size / 2 : 0;
		children[octant] = std::make_unique<OctreeNode>(childPos, size / 2);
		mask.validMask |= (1 << octant);
	}

	children[octant]->setVoxel(pos - children[octant]->getPosition(), newType);

	if (newType != TEXTURE_AIR) {
		mask.voxelMask |= (1 << octant);
	} else {
		mask.voxelMask &= ~(1 << octant);
	}
}

TextureType OctreeNode::getVoxel(const glm::vec3& pos) const
{
	if (size == 1) {
		return type;
	}

	int octant = getOctant(pos);

	// Quick check with the mask
	if (!(mask.validMask & (1 << octant))) {
		return TEXTURE_AIR;
	}

	// if it's a homogeneous leaf
	if (mask.leafMask & (1 << octant)) {
		return type;
	}

	return children[octant]->getVoxel(pos - children[octant]->getPosition());
}

void OctreeNode::optimize()
{
	if (!needsUpdate) return;

	if (isLeaf()) return;

	for (int i = 0; i < 8; ++i) {
		if (mask.validMask & (1 << i)) {
			children[i]->optimize();
		}
	}

	bool canMerge = true;
	TextureType commonType = TEXTURE_AIR;
	bool firstValidChild = true;

	for (int i = 0; i < 8; ++i) {
		if (mask.validMask & (1 << i)) {
			if (!children[i]->isLeaf()) {
				canMerge = false;
				break;
			}

			if (firstValidChild) {
				commonType = children[i]->getType();
				firstValidChild = false;
			} else if (children[i]->getType() != commonType) {
				canMerge = false;
				break;
			}
		} else if (commonType == TEXTURE_AIR) {
			canMerge = false;
			break;
		}
	}

	if (canMerge && !firstValidChild) {
		type = commonType;
		children = {};
		mask.validMask = 0;
		mask.leafMask = 0xFF;
		mask.voxelMask = (commonType != TEXTURE_AIR) ? 0xFF : 0x00;
	}

	needsUpdate = false;
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
	if (!isLeaf()) return;

	TextureType currentType = type;
	for (int i = 0; i < 8; ++i) {
		glm::vec3 childPos = position;
		childPos.x += (i & 1) ? size / 2 : 0;
		childPos.y += (i & 2) ? size / 2 : 0;
		childPos.z += (i & 4) ? size / 2 : 0;
		children[i] = std::make_unique<OctreeNode>(childPos, size / 2);
		children[i]->type = currentType;
		mask.validMask |= (1 << i);
	}
	mask.leafMask = 0;
	if (currentType != TEXTURE_AIR) {
		mask.voxelMask = 0xFF;
	}
}

bool OctreeNode::hasVoxel(int childIndex) const
{
	return (mask.voxelMask & (1 << childIndex)) != 0;
}

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
