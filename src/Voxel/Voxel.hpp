#pragma once

#include <glm/glm.hpp>
#include <utils.hpp>
#include <Mesh/Mesh.hpp>

class Voxel
{
	public:
		Voxel(glm::vec3 position, TextureType type = TEXTURE_AIR);
		~Voxel();

		const glm::vec3&	getPosition() const;
		const TextureType&	getType() const;
		void				setType(const TextureType& type);

		void	addFaceToMesh(Mesh& mesh, Face face);

	private:
		glm::vec3	position;
		TextureType	type;
};
