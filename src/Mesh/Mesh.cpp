#include "Mesh.hpp"

Mesh::Mesh() : type(AIR), data()
{
}

Mesh::~Mesh()
{
}

void Mesh::addVertex(const glm::vec3 &vertex)
{
	this->vertices.push_back(vertex);
	this->isDirty = true;
}

void Mesh::addNormal(const glm::vec3 &normal)
{
	this->normals.push_back(normal);
	this->isDirty = true;
}

void Mesh::addTexture(const glm::vec2 &texture)
{
	this->textures.push_back(texture);
	this->isDirty = true;
}

void Mesh::setType(TextureType type)
{
	this->type = type;
	this->isDirty = true;
}

void Mesh::reserve(size_t vertices, size_t textures, size_t normals)
{
	this->vertices.reserve(vertices);
	this->textures.reserve(textures);
	this->normals.reserve(normals);
}

void Mesh::clear()
{
	this->data.clear();
	this->vertices.clear();
	this->normals.clear();
	this->textures.clear();
	this->isDirty = true;
}

const std::vector<float> &Mesh::getData()
{
	if (isDirty)
	{
		size_t totalSize = this->vertices.size() * 8;
		data.clear();
		data.reserve(totalSize);

		for (size_t i = 0; i < this->vertices.size(); i++)
		{
			data.push_back(this->vertices[i].x);
			data.push_back(this->vertices[i].y);
			data.push_back(this->vertices[i].z);

			data.push_back(this->normals[i].x);
			data.push_back(this->normals[i].y);
			data.push_back(this->normals[i].z);

			data.push_back(this->textures[i].x);
			data.push_back(this->textures[i].y);
		}
		isDirty = false;
	}
	return (data);
}

const std::vector<glm::vec3> &Mesh::getVertices() const
{
	return (this->vertices);
}

const std::vector<glm::vec3> &Mesh::getNormals() const
{
	return (this->normals);
}

const std::vector<glm::vec2> &Mesh::getTextures() const
{
	return (this->textures);
}

const TextureType &Mesh::getType() const
{
	return (this->type);
}
