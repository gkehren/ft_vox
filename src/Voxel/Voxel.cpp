#include "Voxel.hpp"

Voxel::Voxel(glm::vec3 position, TextureType type, bool highest) : position(position), type(type), highest(highest)
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

void	Voxel::setType(const TextureType& type, bool highest)
{
	this->type = type;
	this->highest = highest;
}

void	Voxel::setHighest(bool highest)
{
	this->highest = highest;
}

bool	Voxel::isHighest() const
{
	return (this->highest);
}

void	Voxel::addFaceToMesh(Mesh& mesh, Face face, TextureType type)
{
	glm::vec3	normal;
	float		textureSize = 32.0f / 512.0f;
	float		textureX = static_cast<float>(type % 16) * textureSize;
	float		textureY = static_cast<float>(type / 16) * textureSize;

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
			mesh.addTexture(glm::vec2(textureX, textureY));
			mesh.addTexture(glm::vec2(textureX + textureSize, textureY));
			mesh.addTexture(glm::vec2(textureX + textureSize, textureY + textureSize));
			mesh.addTexture(glm::vec2(textureX, textureY));
			mesh.addTexture(glm::vec2(textureX + textureSize, textureY + textureSize));
			mesh.addTexture(glm::vec2(textureX, textureY + textureSize));
			break;
		case Face::BACK:
			normal = glm::vec3(0.0f, 0.0f, -1.0f);
			mesh.addVertex(this->position + glm::vec3(0.5f, -0.5f, -0.5f));
			mesh.addVertex(this->position + glm::vec3(-0.5f, -0.5f, -0.5f));
			mesh.addVertex(this->position + glm::vec3(-0.5f, 0.5f, -0.5f));
			mesh.addVertex(this->position + glm::vec3(0.5f, -0.5f, -0.5f));
			mesh.addVertex(this->position + glm::vec3(-0.5f, 0.5f, -0.5f));
			mesh.addVertex(this->position + glm::vec3(0.5f, 0.5f, -0.5f));
			mesh.addTexture(glm::vec2(textureX + textureSize, textureY));
			mesh.addTexture(glm::vec2(textureX, textureY));
			mesh.addTexture(glm::vec2(textureX, textureY + textureSize));
			mesh.addTexture(glm::vec2(textureX + textureSize, textureY));
			mesh.addTexture(glm::vec2(textureX, textureY + textureSize));
			mesh.addTexture(glm::vec2(textureX + textureSize, textureY + textureSize));
			break;
		case Face::LEFT:
			normal = glm::vec3(-1.0f, 0.0f, 0.0f);
			mesh.addVertex(this->position + glm::vec3(-0.5f, -0.5f, -0.5f));
			mesh.addVertex(this->position + glm::vec3(-0.5f, -0.5f, 0.5f));
			mesh.addVertex(this->position + glm::vec3(-0.5f, 0.5f, 0.5f));
			mesh.addVertex(this->position + glm::vec3(-0.5f, -0.5f, -0.5f));
			mesh.addVertex(this->position + glm::vec3(-0.5f, 0.5f, 0.5f));
			mesh.addVertex(this->position + glm::vec3(-0.5f, 0.5f, -0.5f));
			mesh.addTexture(glm::vec2(textureX, textureY));
			mesh.addTexture(glm::vec2(textureX + textureSize, textureY));
			mesh.addTexture(glm::vec2(textureX + textureSize, textureY + textureSize));
			mesh.addTexture(glm::vec2(textureX, textureY));
			mesh.addTexture(glm::vec2(textureX + textureSize, textureY + textureSize));
			mesh.addTexture(glm::vec2(textureX, textureY + textureSize));
			break;
		case Face::RIGHT:
			normal = glm::vec3(1.0f, 0.0f, 0.0f);
			mesh.addVertex(this->position + glm::vec3(0.5f, -0.5f, 0.5f));
			mesh.addVertex(this->position + glm::vec3(0.5f, -0.5f, -0.5f));
			mesh.addVertex(this->position + glm::vec3(0.5f, 0.5f, -0.5f));
			mesh.addVertex(this->position + glm::vec3(0.5f, -0.5f, 0.5f));
			mesh.addVertex(this->position + glm::vec3(0.5f, 0.5f, -0.5f));
			mesh.addVertex(this->position + glm::vec3(0.5f, 0.5f, 0.5f));
			mesh.addTexture(glm::vec2(textureX + textureSize, textureY));
			mesh.addTexture(glm::vec2(textureX, textureY));
			mesh.addTexture(glm::vec2(textureX, textureY + textureSize));
			mesh.addTexture(glm::vec2(textureX + textureSize, textureY));
			mesh.addTexture(glm::vec2(textureX, textureY + textureSize));
			mesh.addTexture(glm::vec2(textureX + textureSize, textureY + textureSize));
			break;
		case Face::TOP:
			normal = glm::vec3(0.0f, 1.0f, 0.0f);
			mesh.addVertex(this->position + glm::vec3(-0.5f, 0.5f, 0.5f));
			mesh.addVertex(this->position + glm::vec3(0.5f, 0.5f, 0.5f));
			mesh.addVertex(this->position + glm::vec3(0.5f, 0.5f, -0.5f));
			mesh.addVertex(this->position + glm::vec3(-0.5f, 0.5f, 0.5f));
			mesh.addVertex(this->position + glm::vec3(0.5f, 0.5f, -0.5f));
			mesh.addVertex(this->position + glm::vec3(-0.5f, 0.5f, -0.5f));
			mesh.addTexture(glm::vec2(textureX, textureY + textureSize));
			mesh.addTexture(glm::vec2(textureX + textureSize, textureY + textureSize));
			mesh.addTexture(glm::vec2(textureX + textureSize, textureY));
			mesh.addTexture(glm::vec2(textureX, textureY + textureSize));
			mesh.addTexture(glm::vec2(textureX + textureSize, textureY));
			mesh.addTexture(glm::vec2(textureX, textureY));
			break;
		case Face::BOTTOM:
			normal = glm::vec3(0.0f, -1.0f, 0.0f);
			mesh.addVertex(this->position + glm::vec3(-0.5f, -0.5f, -0.5f));
			mesh.addVertex(this->position + glm::vec3(0.5f, -0.5f, -0.5f));
			mesh.addVertex(this->position + glm::vec3(0.5f, -0.5f, 0.5f));
			mesh.addVertex(this->position + glm::vec3(-0.5f, -0.5f, -0.5f));
			mesh.addVertex(this->position + glm::vec3(0.5f, -0.5f, 0.5f));
			mesh.addVertex(this->position + glm::vec3(-0.5f, -0.5f, 0.5f));
			mesh.addTexture(glm::vec2(textureX, textureY));
			mesh.addTexture(glm::vec2(textureX + textureSize, textureY));
			mesh.addTexture(glm::vec2(textureX + textureSize, textureY + textureSize));
			mesh.addTexture(glm::vec2(textureX, textureY));
			mesh.addTexture(glm::vec2(textureX + textureSize, textureY + textureSize));
			mesh.addTexture(glm::vec2(textureX, textureY + textureSize));
			break;
	}
	mesh.addNormal(normal);
	mesh.addNormal(normal);
	mesh.addNormal(normal);
	mesh.addNormal(normal);
	mesh.addNormal(normal);
	mesh.addNormal(normal);
}
