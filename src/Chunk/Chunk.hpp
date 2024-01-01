#pragma once

#include <Voxel/Voxel.hpp>

#include <vector>
#include <glm/glm.hpp>

#include <utils.hpp>

class	Chunk {
	public:
		static const int WIDTH = 16;
		static const int HEIGHT = 1; // 256
		static const int DEPTH = 16;

		Chunk(const glm::vec3& position);
		~Chunk();

		void	generate();

		std::vector<glm::mat4>		getModelMatrices() const;
		const glm::vec3&			getPosition() const;
		std::vector<Voxel>&			getVoxels();
		std::vector<Voxel>&			getVoxelsSorted(const glm::vec3& position);
		float						getRadius() const;

	private:
		glm::vec3				position;
		std::vector<Voxel>		voxels;
		float					radius;
};
