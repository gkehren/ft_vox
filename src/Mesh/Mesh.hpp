#pragma once

#include <vector>
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <utils.hpp>

class	Mesh
{
	public:
		Mesh();
		~Mesh();

		void	addVertex(const glm::vec3& vertex);
		void	addNormal(const glm::vec3& normal);
		void	addTexture(const glm::vec2& texture);
		void	setType(TextureType type);

		const std::vector<float>		getData() const;
		const std::vector<glm::vec3>&	getVertices() const;
		const std::vector<glm::vec3>&	getNormals() const;
		const std::vector<glm::vec2>&	getTextures() const;
		const TextureType&				getType() const;

	private:
		std::vector<glm::vec3>	vertices;
		std::vector<glm::vec3>	normals;
		std::vector<glm::vec2>	textures;
		TextureType				type;
};
