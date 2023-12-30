#pragma once

#include <Voxel/Voxel.hpp>

#include <vector>
#include <glm/glm.hpp>

#define CHUNK_SIZE	16

class	Chunk {
	public:
		Chunk(const glm::vec3& position);
		~Chunk();

		void	generate();
		std::vector<glm::mat4>	getModelMatrices() const;

		const glm::vec3&	getPosition() const;
		const std::vector<Voxel>&	getVoxels() const;

	private:
		glm::vec3				position;
		std::vector<Voxel>		voxels;
};
