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

void	Voxel::addFaceToMesh(Mesh& mesh, const glm::vec3& chunkPos, Face face, TextureType type)
{
	glm::vec3	normal;
	std::vector<glm::vec3>	vertices;
	std::tie(normal, vertices) = faceData.at(face);
	static const float		textureSize = 32.0f / 512.0f;
	float		textureX = static_cast<float>(type % 16) * textureSize;
	float		textureY = static_cast<float>(type / 16) * textureSize;

	mesh.reserve(vertices.size(), 6, 6);
	for (const auto& vertex : vertices) {
		mesh.addVertex(chunkPos + this->position + vertex);
	}

	mesh.addTexture(glm::vec2(textureX, textureY));
	mesh.addTexture(glm::vec2(textureX + textureSize, textureY));
	mesh.addTexture(glm::vec2(textureX + textureSize, textureY + textureSize));
	mesh.addTexture(glm::vec2(textureX, textureY));
	mesh.addTexture(glm::vec2(textureX + textureSize, textureY + textureSize));
	mesh.addTexture(glm::vec2(textureX, textureY + textureSize));

	for (int i = 0; i < 6; i++) {
		mesh.addNormal(normal);
	}
}
