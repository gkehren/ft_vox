#include "Chunk.hpp"

Chunk::Chunk(const glm::vec3& position) : position(position)
{
	this->voxels.resize(Chunk::SIZE);
	for (int x = 0; x < Chunk::SIZE; x++) {
		this->voxels[x].resize(Chunk::HEIGHT);
		for (int y = 0; y < Chunk::HEIGHT; y++) {
			for (int z = 0; z < Chunk::SIZE; z++) {
				this->voxels[x][y].push_back(Voxel(glm::vec3(x, y, z) + this->position));
			}
		}
	}

	// DEBUG
	// FILL MESH WITH DATA TO CREATE A CUBE AT 0, 0, 0
	this->voxels[0][0][0].setType(TEXTURE_GRASS);

	// FRONT FACE
	this->mesh.addVertex(glm::vec3(-0.5f, -0.5f, 0.5f));
	this->mesh.addVertex(glm::vec3(0.5f, -0.5f, 0.5f));
	this->mesh.addVertex(glm::vec3(0.5f, 0.5f, 0.5f));
	this->mesh.addVertex(glm::vec3(-0.5f, -0.5f, 0.5f));
	this->mesh.addVertex(glm::vec3(0.5f, 0.5f, 0.5f));
	this->mesh.addVertex(glm::vec3(-0.5f, 0.5f, 0.5f));
	this->mesh.addNormal(glm::vec3(0.0f, 0.0f, 1.0f));
	this->mesh.addNormal(glm::vec3(0.0f, 0.0f, 1.0f));
	this->mesh.addNormal(glm::vec3(0.0f, 0.0f, 1.0f));
	this->mesh.addNormal(glm::vec3(0.0f, 0.0f, 1.0f));
	this->mesh.addNormal(glm::vec3(0.0f, 0.0f, 1.0f));
	this->mesh.addNormal(glm::vec3(0.0f, 0.0f, 1.0f));
	this->mesh.addTexture(glm::vec2(0.0f, 0.0f));
	this->mesh.addTexture(glm::vec2(1.0f, 0.0f));
	this->mesh.addTexture(glm::vec2(1.0f, 1.0f));
	this->mesh.addTexture(glm::vec2(0.0f, 0.0f));
	this->mesh.addTexture(glm::vec2(1.0f, 1.0f));
	this->mesh.addTexture(glm::vec2(0.0f, 1.0f));

	// BACK FACE
	this->mesh.addVertex(glm::vec3(0.5f, -0.5f, -0.5f));
	this->mesh.addVertex(glm::vec3(-0.5f, -0.5f, -0.5f));
	this->mesh.addVertex(glm::vec3(-0.5f, 0.5f, -0.5f));
	this->mesh.addVertex(glm::vec3(0.5f, -0.5f, -0.5f));
	this->mesh.addVertex(glm::vec3(-0.5f, 0.5f, -0.5f));
	this->mesh.addVertex(glm::vec3(0.5f, 0.5f, -0.5f));
	this->mesh.addNormal(glm::vec3(0.0f, 0.0f, -1.0f));
	this->mesh.addNormal(glm::vec3(0.0f, 0.0f, -1.0f));
	this->mesh.addNormal(glm::vec3(0.0f, 0.0f, -1.0f));
	this->mesh.addNormal(glm::vec3(0.0f, 0.0f, -1.0f));
	this->mesh.addNormal(glm::vec3(0.0f, 0.0f, -1.0f));
	this->mesh.addNormal(glm::vec3(0.0f, 0.0f, -1.0f));
	this->mesh.addTexture(glm::vec2(1.0f, 0.0f));
	this->mesh.addTexture(glm::vec2(0.0f, 0.0f));
	this->mesh.addTexture(glm::vec2(0.0f, 1.0f));
	this->mesh.addTexture(glm::vec2(1.0f, 0.0f));
	this->mesh.addTexture(glm::vec2(0.0f, 1.0f));
	this->mesh.addTexture(glm::vec2(1.0f, 1.0f));

	// LEFT FACE
	this->mesh.addVertex(glm::vec3(-0.5f, -0.5f, -0.5f));
	this->mesh.addVertex(glm::vec3(-0.5f, -0.5f, 0.5f));
	this->mesh.addVertex(glm::vec3(-0.5f, 0.5f, 0.5f));
	this->mesh.addVertex(glm::vec3(-0.5f, -0.5f, -0.5f));
	this->mesh.addVertex(glm::vec3(-0.5f, 0.5f, 0.5f));
	this->mesh.addVertex(glm::vec3(-0.5f, 0.5f, -0.5f));
	this->mesh.addNormal(glm::vec3(-1.0f, 0.0f, 0.0f));
	this->mesh.addNormal(glm::vec3(-1.0f, 0.0f, 0.0f));
	this->mesh.addNormal(glm::vec3(-1.0f, 0.0f, 0.0f));
	this->mesh.addNormal(glm::vec3(-1.0f, 0.0f, 0.0f));
	this->mesh.addNormal(glm::vec3(-1.0f, 0.0f, 0.0f));
	this->mesh.addNormal(glm::vec3(-1.0f, 0.0f, 0.0f));
	this->mesh.addTexture(glm::vec2(0.0f, 0.0f));
	this->mesh.addTexture(glm::vec2(1.0f, 0.0f));
	this->mesh.addTexture(glm::vec2(1.0f, 1.0f));
	this->mesh.addTexture(glm::vec2(0.0f, 0.0f));
	this->mesh.addTexture(glm::vec2(1.0f, 1.0f));
	this->mesh.addTexture(glm::vec2(0.0f, 1.0f));

	// RIGHT FACE
	this->mesh.addVertex(glm::vec3(0.5f, -0.5f, 0.5f));
	this->mesh.addVertex(glm::vec3(0.5f, -0.5f, -0.5f));
	this->mesh.addVertex(glm::vec3(0.5f, 0.5f, -0.5f));
	this->mesh.addVertex(glm::vec3(0.5f, -0.5f, 0.5f));
	this->mesh.addVertex(glm::vec3(0.5f, 0.5f, -0.5f));
	this->mesh.addVertex(glm::vec3(0.5f, 0.5f, 0.5f));
	this->mesh.addNormal(glm::vec3(1.0f, 0.0f, 0.0f));
	this->mesh.addNormal(glm::vec3(1.0f, 0.0f, 0.0f));
	this->mesh.addNormal(glm::vec3(1.0f, 0.0f, 0.0f));
	this->mesh.addNormal(glm::vec3(1.0f, 0.0f, 0.0f));
	this->mesh.addNormal(glm::vec3(1.0f, 0.0f, 0.0f));
	this->mesh.addNormal(glm::vec3(1.0f, 0.0f, 0.0f));
	this->mesh.addTexture(glm::vec2(1.0f, 0.0f));
	this->mesh.addTexture(glm::vec2(0.0f, 0.0f));
	this->mesh.addTexture(glm::vec2(0.0f, 1.0f));
	this->mesh.addTexture(glm::vec2(1.0f, 0.0f));
	this->mesh.addTexture(glm::vec2(0.0f, 1.0f));
	this->mesh.addTexture(glm::vec2(1.0f, 1.0f));

	// TOP FACE
	this->mesh.addVertex(glm::vec3(-0.5f, 0.5f, 0.5f));
	this->mesh.addVertex(glm::vec3(0.5f, 0.5f, 0.5f));
	this->mesh.addVertex(glm::vec3(0.5f, 0.5f, -0.5f));
	this->mesh.addVertex(glm::vec3(-0.5f, 0.5f, 0.5f));
	this->mesh.addVertex(glm::vec3(0.5f, 0.5f, -0.5f));
	this->mesh.addVertex(glm::vec3(-0.5f, 0.5f, -0.5f));
	this->mesh.addNormal(glm::vec3(0.0f, 1.0f, 0.0f));
	this->mesh.addNormal(glm::vec3(0.0f, 1.0f, 0.0f));
	this->mesh.addNormal(glm::vec3(0.0f, 1.0f, 0.0f));
	this->mesh.addNormal(glm::vec3(0.0f, 1.0f, 0.0f));
	this->mesh.addNormal(glm::vec3(0.0f, 1.0f, 0.0f));
	this->mesh.addNormal(glm::vec3(0.0f, 1.0f, 0.0f));
	this->mesh.addTexture(glm::vec2(0.0f, 1.0f));
	this->mesh.addTexture(glm::vec2(1.0f, 1.0f));
	this->mesh.addTexture(glm::vec2(1.0f, 0.0f));
	this->mesh.addTexture(glm::vec2(0.0f, 1.0f));
	this->mesh.addTexture(glm::vec2(1.0f, 0.0f));
	this->mesh.addTexture(glm::vec2(0.0f, 0.0f));

	// BOTTOM FACE
	this->mesh.addVertex(glm::vec3(-0.5f, -0.5f, -0.5f));
	this->mesh.addVertex(glm::vec3(0.5f, -0.5f, -0.5f));
	this->mesh.addVertex(glm::vec3(0.5f, -0.5f, 0.5f));
	this->mesh.addVertex(glm::vec3(-0.5f, -0.5f, -0.5f));
	this->mesh.addVertex(glm::vec3(0.5f, -0.5f, 0.5f));
	this->mesh.addVertex(glm::vec3(-0.5f, -0.5f, 0.5f));
	this->mesh.addNormal(glm::vec3(0.0f, -1.0f, 0.0f));
	this->mesh.addNormal(glm::vec3(0.0f, -1.0f, 0.0f));
	this->mesh.addNormal(glm::vec3(0.0f, -1.0f, 0.0f));
	this->mesh.addNormal(glm::vec3(0.0f, -1.0f, 0.0f));
	this->mesh.addNormal(glm::vec3(0.0f, -1.0f, 0.0f));
	this->mesh.addNormal(glm::vec3(0.0f, -1.0f, 0.0f));
	this->mesh.addTexture(glm::vec2(0.0f, 0.0f));
	this->mesh.addTexture(glm::vec2(1.0f, 0.0f));
	this->mesh.addTexture(glm::vec2(1.0f, 1.0f));
	this->mesh.addTexture(glm::vec2(0.0f, 0.0f));
	this->mesh.addTexture(glm::vec2(1.0f, 1.0f));
	this->mesh.addTexture(glm::vec2(0.0f, 1.0f));

	this->mesh.setType(TEXTURE_GRASS);
}

Chunk::~Chunk()
{}

const glm::vec3&	Chunk::getPosition() const
{
	return (this->position);
}

const std::vector<float>	Chunk::getData() const
{
	return mesh.getData();
}
