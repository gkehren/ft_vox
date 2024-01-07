#include "Voxel.hpp"

Voxel::Voxel(glm::vec3 position, TextureType type) : position(position), type(type)
{}

Voxel::~Voxel()
{}

const glm::vec3&	Voxel::getPosition() const
{
	return (this->position);
}

const TextureType&	Voxel::getType() const
{
	return (this->type);
}

void	Voxel::setType(const TextureType& type)
{
	this->type = type;
}

void	Voxel::addFaceToMesh(Mesh& mesh, Face face)
{
	glm::vec3	normal;

	switch (face)
	{
		case Face::FRONT:
			normal = glm::vec3(0.0f, 0.0f, 1.0f);
			mesh.addVertex(this->position + glm::vec3(-0.5f, -0.5f, 0.5f));
			mesh.addVertex(this->position + glm::vec3(0.5f, -0.5f, 0.5f));
			mesh.addVertex(this->position + glm::vec3(0.5f, 0.5f, 0.5f));
			mesh.addVertex(this->position + glm::vec3(-0.5f, -0.5f, 0.5f));
			mesh.addVertex(this->position + glm::vec3(0.5f, 0.5f, 0.5f));
			mesh.addVertex(this->position + glm::vec3(-0.5f, 0.5f, 0.5f));
			mesh.addTexture(glm::vec2(0.0f, 0.0f));
			mesh.addTexture(glm::vec2(1.0f, 0.0f));
			mesh.addTexture(glm::vec2(1.0f, 1.0f));
			mesh.addTexture(glm::vec2(0.0f, 0.0f));
			mesh.addTexture(glm::vec2(1.0f, 1.0f));
			mesh.addTexture(glm::vec2(0.0f, 1.0f));
			break;
		case Face::BACK:
			normal = glm::vec3(0.0f, 0.0f, -1.0f);
			mesh.addVertex(this->position + glm::vec3(0.5f, -0.5f, -0.5f));
			mesh.addVertex(this->position + glm::vec3(-0.5f, -0.5f, -0.5f));
			mesh.addVertex(this->position + glm::vec3(-0.5f, 0.5f, -0.5f));
			mesh.addVertex(this->position + glm::vec3(0.5f, -0.5f, -0.5f));
			mesh.addVertex(this->position + glm::vec3(-0.5f, 0.5f, -0.5f));
			mesh.addVertex(this->position + glm::vec3(0.5f, 0.5f, -0.5f));
			mesh.addTexture(glm::vec2(1.0f, 0.0f));
			mesh.addTexture(glm::vec2(0.0f, 0.0f));
			mesh.addTexture(glm::vec2(0.0f, 1.0f));
			mesh.addTexture(glm::vec2(1.0f, 0.0f));
			mesh.addTexture(glm::vec2(0.0f, 1.0f));
			mesh.addTexture(glm::vec2(1.0f, 1.0f));
			break;
		case Face::LEFT:
			normal = glm::vec3(-1.0f, 0.0f, 0.0f);
			mesh.addVertex(this->position + glm::vec3(-0.5f, -0.5f, -0.5f));
			mesh.addVertex(this->position + glm::vec3(-0.5f, -0.5f, 0.5f));
			mesh.addVertex(this->position + glm::vec3(-0.5f, 0.5f, 0.5f));
			mesh.addVertex(this->position + glm::vec3(-0.5f, -0.5f, -0.5f));
			mesh.addVertex(this->position + glm::vec3(-0.5f, 0.5f, 0.5f));
			mesh.addVertex(this->position + glm::vec3(-0.5f, 0.5f, -0.5f));
			mesh.addTexture(glm::vec2(0.0f, 0.0f));
			mesh.addTexture(glm::vec2(1.0f, 0.0f));
			mesh.addTexture(glm::vec2(1.0f, 1.0f));
			mesh.addTexture(glm::vec2(0.0f, 0.0f));
			mesh.addTexture(glm::vec2(1.0f, 1.0f));
			mesh.addTexture(glm::vec2(0.0f, 1.0f));
			break;
		case Face::RIGHT:
			normal = glm::vec3(1.0f, 0.0f, 0.0f);
			mesh.addVertex(this->position + glm::vec3(0.5f, -0.5f, 0.5f));
			mesh.addVertex(this->position + glm::vec3(0.5f, -0.5f, -0.5f));
			mesh.addVertex(this->position + glm::vec3(0.5f, 0.5f, -0.5f));
			mesh.addVertex(this->position + glm::vec3(0.5f, -0.5f, 0.5f));
			mesh.addVertex(this->position + glm::vec3(0.5f, 0.5f, -0.5f));
			mesh.addVertex(this->position + glm::vec3(0.5f, 0.5f, 0.5f));
			mesh.addTexture(glm::vec2(1.0f, 0.0f));
			mesh.addTexture(glm::vec2(0.0f, 0.0f));
			mesh.addTexture(glm::vec2(0.0f, 1.0f));
			mesh.addTexture(glm::vec2(1.0f, 0.0f));
			mesh.addTexture(glm::vec2(0.0f, 1.0f));
			mesh.addTexture(glm::vec2(1.0f, 1.0f));
			break;
		case Face::TOP:
			normal = glm::vec3(0.0f, 1.0f, 0.0f);
			mesh.addVertex(this->position + glm::vec3(-0.5f, 0.5f, 0.5f));
			mesh.addVertex(this->position + glm::vec3(0.5f, 0.5f, 0.5f));
			mesh.addVertex(this->position + glm::vec3(0.5f, 0.5f, -0.5f));
			mesh.addVertex(this->position + glm::vec3(-0.5f, 0.5f, 0.5f));
			mesh.addVertex(this->position + glm::vec3(0.5f, 0.5f, -0.5f));
			mesh.addVertex(this->position + glm::vec3(-0.5f, 0.5f, -0.5f));
			mesh.addTexture(glm::vec2(0.0f, 1.0f));
			mesh.addTexture(glm::vec2(1.0f, 1.0f));
			mesh.addTexture(glm::vec2(1.0f, 0.0f));
			mesh.addTexture(glm::vec2(0.0f, 1.0f));
			mesh.addTexture(glm::vec2(1.0f, 0.0f));
			mesh.addTexture(glm::vec2(0.0f, 0.0f));
			break;
		case Face::BOTTOM:
			normal = glm::vec3(0.0f, -1.0f, 0.0f);
			mesh.addVertex(this->position + glm::vec3(-0.5f, -0.5f, -0.5f));
			mesh.addVertex(this->position + glm::vec3(0.5f, -0.5f, -0.5f));
			mesh.addVertex(this->position + glm::vec3(0.5f, -0.5f, 0.5f));
			mesh.addVertex(this->position + glm::vec3(-0.5f, -0.5f, -0.5f));
			mesh.addVertex(this->position + glm::vec3(0.5f, -0.5f, 0.5f));
			mesh.addVertex(this->position + glm::vec3(-0.5f, -0.5f, 0.5f));
			mesh.addTexture(glm::vec2(0.0f, 0.0f));
			mesh.addTexture(glm::vec2(1.0f, 0.0f));
			mesh.addTexture(glm::vec2(1.0f, 1.0f));
			mesh.addTexture(glm::vec2(0.0f, 0.0f));
			mesh.addTexture(glm::vec2(1.0f, 1.0f));
			mesh.addTexture(glm::vec2(0.0f, 1.0f));
			break;
	}
	mesh.addNormal(normal);
	mesh.addNormal(normal);
	mesh.addNormal(normal);
	mesh.addNormal(normal);
	mesh.addNormal(normal);
	mesh.addNormal(normal);
}
