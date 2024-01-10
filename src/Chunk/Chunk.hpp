#pragma once

#include <vector>
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <array>
#include <PerlinNoise/PerlinNoise.hpp>

#include <Shader/Shader.hpp>
#include <Camera/Camera.hpp>
#include <Voxel/Voxel.hpp>
#include <Mesh/Mesh.hpp>
#include <utils.hpp>

class Chunk
{
	public:
		static const int SIZE = 16;
		static const int HEIGHT = 128;
		static constexpr float RADIUS = 16.0f;

		Chunk(const glm::vec3& position, siv::PerlinNoise* perlin);
		~Chunk();

		const glm::vec3&				getPosition() const;
		const std::vector<float>&		getData();
		bool							isVisible() const;
		void							setVisible(bool visible);
		void							setState(ChunkState state);
		ChunkState						getState() const;
		bool							contains(int x, int y, int z) const;
		const Voxel&					getVoxel(int x, int y, int z) const;

		void	generateVoxel(siv::PerlinNoise* perlin);
		void	generateMesh(const std::vector<Chunk>& chunks);

	private:
		glm::vec3	position;
		bool		visible;
		ChunkState	state;

		std::vector<std::vector<std::vector<Voxel>>>	voxels;
		Mesh											mesh;

		void	addVoxelToMesh(const std::vector<Chunk>& chunks, Voxel& voxel, int x, int y, int z);
};
