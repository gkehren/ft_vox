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
		static const int WIDTH = 16;
		static const int HEIGHT = 16; // 256
		static const int DEPTH = 16;
		static constexpr float FWIDTH = 16.0f;
		static constexpr float FHEIGHT = 16.0f;
		static constexpr float FDEPTH = 16.0f;

		Chunk(const glm::vec3& position);
		~Chunk();

		void	generate();

		std::vector<glm::mat4>		getModelMatrices() const;
		const glm::vec3&			getPosition() const;
		const glm::ivec2&			getPosition2D() const;
		const std::vector<Voxel>&	getVoxels() const;
		std::vector<Voxel>&			getVoxels();
		std::vector<Voxel>&			getVoxelsSorted(const glm::vec3& position);
		float						getRadius() const;

	private:
		glm::vec3				position;
		glm::ivec2				position2D;
		std::vector<Voxel>		voxels;
		float					radius;
};
