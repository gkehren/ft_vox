#pragma once

#define GLM_ENABLE_EXPERIMENTAL

#include <vector>
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <array>
#include <bitset>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <glm/gtx/hash.hpp>

#include <chrono>

#include <Chunk/TerrainGenerator.hpp>
#include <Renderer/TextureManager.hpp>
#include <Shader/Shader.hpp>
#include <Camera/Camera.hpp>
#include <utils.hpp>
#include <Engine/EngineDefs.hpp>

class Chunk
{
public:
	Chunk(const glm::vec3 &position, ChunkState state = ChunkState::UNLOADED);
	Chunk(Chunk &&other) noexcept;
	Chunk &operator=(Chunk &&other) noexcept;
	~Chunk();

	const glm::vec3 &getPosition() const;
	bool isVisible() const;
	void setVisible(bool visible);
	void setState(ChunkState state);
	ChunkState getState() const;

	void setVoxels(const std::vector<Voxel> &voxels);


	Voxel &getVoxel(uint32_t x, uint32_t y, uint32_t z);
	const Voxel &getVoxel(uint32_t x, uint32_t y, uint32_t z) const;
	bool isVoxelActive(int x, int y, int z) const;
	void setVoxel(int x, int y, int z, TextureType type);

	bool deleteVoxel(const glm::vec3 &position);
	bool placeVoxel(const glm::vec3 &position, TextureType type);
	uint32_t draw();
	uint32_t drawWater();
	void drawShadow(const Shader &shader) const;
	void generateTerrain(TerrainGenerator &generator);
	void generateMesh();
	bool hasWaterMesh() const { return waterIndexCount > 0; }

private:
	glm::vec3 position;
	bool visible;
	ChunkState state;

	GLuint VAO;
	GLuint VBO;
	GLuint EBO;

	// Separate water mesh for transparency pass
	GLuint waterVAO;
	GLuint waterVBO;
	GLuint waterEBO;

	std::vector<Vertex> vertices;
	std::vector<uint16_t> indices;
	std::vector<Vertex> waterVertices;
	std::vector<uint16_t> waterIndices;
	std::vector<Voxel> voxels;
	std::bitset<CHUNK_VOLUME> activeVoxels;
	std::vector<uint8_t> neighborShellVoxels; // Flat array for 1-thick shell (18x(H+2)x18)

	// Precomputed packed RGBA biome colors per column (from terrain generation)
	std::array<uint32_t, CHUNK_SIZE * CHUNK_SIZE> biomeGrassColors{};
	std::array<uint32_t, CHUNK_SIZE * CHUNK_SIZE> biomeFoliageColors{};

	uint32_t opaqueIndexCount;
	uint32_t waterIndexCount;

	bool meshNeedsUpdate;
	void uploadMeshToGPU();


	size_t getIndex(uint32_t x, uint32_t y, uint32_t z) const;
};
