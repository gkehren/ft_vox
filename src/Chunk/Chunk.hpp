#pragma once

#include <vector>
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <array>

#include <Shader/Shader.hpp>
#include <Camera/Camera.hpp>
#include <Voxel/Voxel.hpp>
#include <Mesh/Mesh.hpp>
#include <utils.hpp>

class Chunk
{
	public:
		static const int SIZE = 16;
		static const int HEIGHT = 16;

		Chunk(const glm::vec3& position);
		~Chunk();

		const glm::vec3&				getPosition() const;
		const std::vector<float>		getData() const;

		void	generateMesh();

	private:
		glm::vec3	position;

		std::vector<std::vector<std::vector<Voxel>>>	voxels;
		Mesh											mesh;

		void	addVoxelToMesh(Voxel& voxel, int x, int y, int z);
};
