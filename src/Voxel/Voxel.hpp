#pragma once

#include <glm/glm.hpp>
#include <utils.hpp>
#include <Mesh/Mesh.hpp>

class Voxel
{
	public:
		Voxel(glm::vec3 position, TextureType type = TEXTURE_AIR, bool highest = false);
		~Voxel();

		const glm::vec3&	getPosition() const;
		const TextureType&	getType() const;
		void				setType(const TextureType& type);
		void				setType(const TextureType& type, bool highest);
		void				setHighest(bool highest);
		bool				isHighest() const;

		void	addFaceToMesh(Mesh& mesh, Face face, TextureType type);

	private:
		glm::vec3	position;
		bool		highest;
		TextureType	type;
};
