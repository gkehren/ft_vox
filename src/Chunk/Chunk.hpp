#pragma once

#include <vector>
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <array>
#include <thread>
#include <unordered_map>
#include <PerlinNoise/PerlinNoise.hpp>

#include <chrono>

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

		Chunk(const glm::vec3& position, ChunkState state = ChunkState::UNLOADED);
		~Chunk();

		const glm::vec3&				getPosition() const;
		const std::vector<float>&		getData();
		bool							isVisible() const;
		void							setVisible(bool visible);
		void							setState(ChunkState state);
		ChunkState						getState() const;
		bool							contains(int x, int y, int z) const;
		bool							containesUpper(int x, int y, int z) const;
		const Voxel&					getVoxel(int x, int y, int z) const;
		bool							deleteVoxel(glm::vec3 position, glm::vec3 front);
		bool							placeVoxel(glm::vec3 position, glm::vec3 front, TextureType type);

		void	generateVoxel(siv::PerlinNoise* perlin);
		void	generateMesh(std::unordered_map<glm::ivec3, Chunk, ivec3_hash>& chunks, siv::PerlinNoise* perlin);

	private:
		glm::vec3	position;
		bool		visible;
		ChunkState	state;

		std::vector<std::vector<std::vector<Voxel>>>		voxels;
		std::unordered_map<glm::ivec3, Voxel, ivec3_hash>	voxelsUpper;
		Mesh												mesh;

		bool			addVoxelToMesh(std::unordered_map<glm::ivec3, Chunk, ivec3_hash>& chunks, Voxel& voxel, int x, int y, int z, siv::PerlinNoise* perlin);
		void			generateChunk(int startX, int endX, int startZ, int endZ, siv::PerlinNoise* perlin);
		bool			adjacentChunksGenerated(std::unordered_map<glm::ivec3, Chunk, ivec3_hash>& chunks) const;
};
