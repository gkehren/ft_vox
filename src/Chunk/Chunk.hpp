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
#include <Octree/SVO.hpp>
#include <utils.hpp>

class Chunk
{
	public:
		static constexpr int SIZE = 16;
		static constexpr int HEIGHT = 256;
		static constexpr float RADIUS = 16.0f;

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

		bool							deleteVoxel(const glm::vec3& position, const glm::vec3& front);
		bool							placeVoxel(const glm::vec3& position, const glm::vec3& front, TextureType type);

		void	generateVoxels(siv::PerlinNoise* perlin);

	private:
		glm::vec3	position;
		bool		visible;
		ChunkState	state;

		std::unique_ptr<SVO>	svo;
		std::vector<float>		cachedMesh;
		bool					meshNeedsUpdate;

		void	generateChunk(siv::PerlinNoise* perlin);
		void	generateMesh();
};
