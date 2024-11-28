#pragma once

#include <vector>
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <array>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <PerlinNoise/PerlinNoise.hpp>

#include <chrono>

#include <Shader/Shader.hpp>
#include <Camera/Camera.hpp>
#include <utils.hpp>

class Chunk
{
	public:
		static constexpr int SIZE = 16;
		static constexpr int HEIGHT = 256;
		static constexpr float RADIUS = 16.0f;

		GLuint	VBO;
		bool	VBONeedsUpdate;

		Chunk(const glm::vec3& position, ChunkState state = ChunkState::UNLOADED);
		Chunk(Chunk&& other) noexcept;
		Chunk& operator=(Chunk&& other) noexcept;
		~Chunk() = default;

		const glm::vec3&				getPosition() const;
		const std::vector<float>&		getMeshData();
		bool							isVisible() const;
		void							setVisible(bool visible);
		void							setState(ChunkState state);
		ChunkState						getState() const;
		bool							getMeshNeedsUpdate() const;

		Voxel&							getVoxel(uint32_t x, uint32_t y, uint32_t z);
		const Voxel&					getVoxel(uint32_t x, uint32_t y, uint32_t z) const;
		void							setVoxel(int x, int y, int z, TextureType type);

		bool							deleteVoxel(const glm::vec3& position, const glm::vec3& front);
		bool							placeVoxel(const glm::vec3& position, const glm::vec3& front, TextureType type);

		void	generateVoxels(siv::PerlinNoise* perlin);
		void	generateMesh();

	private:
		glm::vec3	position;
		bool		visible;
		ChunkState	state;

		// Stockage linéaire 3D des voxels
		std::vector<Voxel> voxels;

		// neighbours voxels
		std::vector<Voxel> neighbours;
		Voxel& getNeighbourVoxel(int x, int y, int z);

		inline size_t getIndex(uint32_t x, uint32_t y, uint32_t z) const {
			return x + SIZE * (z + SIZE * y);
		}

		std::vector<float>		cachedMesh;
		bool					meshNeedsUpdate;

		void	generateChunk(siv::PerlinNoise* perlin);
};
