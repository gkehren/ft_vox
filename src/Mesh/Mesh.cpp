#include "Mesh.hpp"

Mesh::Mesh() : type(TEXTURE_AIR), data()
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

void	Mesh::reserve(size_t vertices, size_t textures, size_t normals)
{
	this->vertices.reserve(vertices);
	this->textures.reserve(textures);
	this->normals.reserve(normals);
}

void	Mesh::clear()
{
	this->data.clear();
	this->vertices.clear();
	this->normals.clear();
	this->textures.clear();
}

const std::vector<float>&	Mesh::getData()
{
	if (this->data.empty()) {
		this->data.reserve(this->vertices.size() * 8);

		for (size_t i = 0; i < this->vertices.size(); i++) {
			this->data.push_back(this->vertices[i].x);
			this->data.push_back(this->vertices[i].y);
			this->data.push_back(this->vertices[i].z);
			this->data.push_back(this->normals[i].x);
			this->data.push_back(this->normals[i].y);
			this->data.push_back(this->normals[i].z);
			this->data.push_back(this->textures[i].x);
			this->data.push_back(this->textures[i].y);
		}
	}

	return (this->data);
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
