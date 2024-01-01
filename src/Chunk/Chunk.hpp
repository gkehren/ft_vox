#pragma once

#include <Voxel/Voxel.hpp>

#include <vector>
#include <glm/glm.hpp>
#include <algorithm>
#include <cstdlib>
#include <ctime>

#include <utils.hpp>

class	Chunk {
	public:
		static constexpr float WIDTH = 16.0f;
		static constexpr float HEIGHT = 16.0f; // 256
		static constexpr float DEPTH = 16.0f;

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
