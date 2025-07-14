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

#include <FastNoise/FastNoiseLite.h>
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

	void setVoxels(const std::array<Voxel, CHUNK_VOLUME> &voxels);
	void setBiomeMap(const std::array<BiomeType, CHUNK_SIZE * CHUNK_SIZE> &biomes);

	Voxel &getVoxel(uint32_t x, uint32_t y, uint32_t z);
	const Voxel &getVoxel(uint32_t x, uint32_t y, uint32_t z) const;
	bool isVoxelActive(int x, int y, int z) const;
	void setVoxel(int x, int y, int z, TextureType type);

	bool deleteVoxel(const glm::vec3 &position);
	bool placeVoxel(const glm::vec3 &position, TextureType type);
	uint32_t draw(const Shader &shader, const Camera &camera, GLuint textureArray, const ShaderParameters &params);
	void generateTerrain(TerrainGenerator &generator);
	void generateMesh();

private:
	glm::vec3 position;
	bool visible;
	ChunkState state;

	GLuint VAO;
	GLuint VBO;
	GLuint EBO;

	std::vector<Vertex> vertices;
	std::vector<uint16_t> indices;
	std::array<Voxel, CHUNK_VOLUME> voxels;
	std::bitset<CHUNK_VOLUME> activeVoxels;
	std::unordered_map<glm::ivec3, TextureType, IVec3Hash> neighborShellVoxels;
	std::array<BiomeType, CHUNK_SIZE * CHUNK_SIZE> biomeMap;

	bool meshNeedsUpdate;
	void uploadMeshToGPU();

	glm::vec3 getBiomeColor(BiomeType biome, TextureType textureType) const;
	size_t getIndex(uint32_t x, uint32_t y, uint32_t z) const;
};
