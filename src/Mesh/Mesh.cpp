#include "Mesh.hpp"

Mesh::Mesh() : type(TEXTURE_AIR)
{}

Mesh::~Mesh()
{}

void	Mesh::addVertex(const glm::vec3& vertex)
{
	this->vertices.push_back(vertex);
}

void	Mesh::addNormal(const glm::vec3& normal)
{
	this->normals.push_back(normal);
}

void	Mesh::addTexture(const glm::vec2& texture)
{
	this->textures.push_back(texture);
}

void	Mesh::setType(TextureType type)
{
	this->type = type;
}

const std::vector<float>	Mesh::getData() const
{
	std::vector<float>	data;

	for (size_t i = 0; i < this->vertices.size(); i++) {
		data.push_back(this->vertices[i].x);
		data.push_back(this->vertices[i].y);
		data.push_back(this->vertices[i].z);
		data.push_back(this->normals[i].x);
		data.push_back(this->normals[i].y);
		data.push_back(this->normals[i].z);
		data.push_back(this->textures[i].x);
		data.push_back(this->textures[i].y);
	}
	return (data);
}

const std::vector<glm::vec3>&	Mesh::getVertices() const
{
	return (this->vertices);
}

const std::vector<glm::vec3>&	Mesh::getNormals() const
{
	return (this->normals);
}

const std::vector<glm::vec2>&	Mesh::getTextures() const
{
	return (this->textures);
}

const TextureType&	Mesh::getType() const
{
	return (this->type);
}
