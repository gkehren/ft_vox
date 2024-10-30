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
#include <Octree/Octree.hpp>
#include <Mesh/Mesh.hpp>
#include <utils.hpp>

class Chunk
{
	public:
		static const int SIZE = 16;
		static const int HEIGHT = 256;
		static constexpr float RADIUS = 16.0f;

		Chunk(const glm::vec3& position, ChunkState state = ChunkState::UNLOADED);
		Chunk(Chunk&& other) noexcept;
		Chunk& operator=(Chunk&& other) noexcept;
		~Chunk() = default;

		const glm::vec3&				getPosition() const;
		const std::vector<float>&		getData();
		bool							isVisible() const;
		void							setVisible(bool visible);
		void							setState(ChunkState state);
		ChunkState						getState() const;

		TextureType						getVoxel(int x, int y, int z) const;
		bool							deleteVoxel(const glm::vec3& position, const glm::vec3& front);
		bool							placeVoxel(const glm::vec3& position, const glm::vec3& front, TextureType type);

		void	generateVoxel(siv::PerlinNoise* perlin);
		void	generateMesh(std::unordered_map<glm::ivec3, Chunk, ivec3_hash>& chunks, siv::PerlinNoise* perlin);

	private:
		glm::vec3	position;
		bool		visible;
		ChunkState	state;

		Octree	octree;
		Mesh	mesh;

		struct ChunkColumn
		{
			int surfaceHeight;
			TextureType biomeType;
			bool isMountain;
		};

		TextureType getBiomeType(float biomeNoise);
		void generateOctant(int startX, int startY, int startZ, int endX, int endY, int endZ, const std::vector<ChunkColumn>& columns, siv::PerlinNoise* perlin);
		TextureType determineVoxelType(int x, int y, int z, const ChunkColumn& column, siv::PerlinNoise* perlin);

		bool			addVoxelToMesh(std::unordered_map<glm::ivec3, Chunk, ivec3_hash>& chunks, const glm::vec3& pos, TextureType type, siv::PerlinNoise* perlin);
		void			addFaceToMesh(const glm::vec3& pos, Face face, TextureType type);
		void			generateChunk(int startX, int endX, int startZ, int endZ, siv::PerlinNoise* perlin);
		bool			adjacentChunksGenerated(const std::unordered_map<glm::ivec3, Chunk, ivec3_hash>& chunks) const;
};
