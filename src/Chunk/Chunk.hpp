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
#include <PerlinNoise/PerlinNoise.hpp>
#include <glm/gtx/hash.hpp> // For glm::ivec3 hashing

#include <chrono>

#include <Renderer/TextureManager.hpp>
#include <Shader/Shader.hpp>
#include <Camera/Camera.hpp>
#include <Biome/BiomeManager.hpp>
#include <utils.hpp>
#include <Engine/EngineDefs.hpp>

struct IVec3Hash
{
	std::size_t operator()(const glm::ivec3 &v) const
	{
		std::size_t h1 = std::hash<int>()(v.x);
		std::size_t h2 = std::hash<int>()(v.y);
		std::size_t h3 = std::hash<int>()(v.z);
		return h1 ^ (h2 << 1) ^ (h3 << 2);
	}
};

class Chunk
{
public:
	static constexpr int SIZE = 16;
	static constexpr int HEIGHT = 256;
	static constexpr float RADIUS = 16.0f;
	static constexpr int WATER_LEVEL = 58;

	Chunk(const glm::vec3 &position, ChunkState state = ChunkState::UNLOADED);
	Chunk(Chunk &&other) noexcept;
	Chunk &operator=(Chunk &&other) noexcept;
	~Chunk();

	const glm::vec3 &getPosition() const;
	bool isVisible() const;
	void setVisible(bool visible);
	void setState(ChunkState state);
	ChunkState getState() const;
	Voxel &getVoxel(uint32_t x, uint32_t y, uint32_t z);
	const Voxel &getVoxel(uint32_t x, uint32_t y, uint32_t z) const;
	bool isVoxelActive(int x, int y, int z) const;
	bool isVoxelActiveGlobalPos(int x, int y, int z) const;
	void setVoxel(int x, int y, int z, TextureType type);

	bool deleteVoxel(const glm::vec3 &position);
	bool placeVoxel(const glm::vec3 &position, TextureType type);

	uint32_t draw(const Shader &shader, const Camera &camera, GLuint textureArray, const ShaderParameters &params);
	void generateVoxels(siv::PerlinNoise *noise);
	void generateMesh(glm::vec3 playerPos, siv::PerlinNoise *noise);

private:
	glm::vec3 position;
	bool visible;
	ChunkState state;

	GLuint VAO;
	GLuint VBO;
	GLuint EBO;

	std::vector<Vertex> vertices;
	std::vector<uint16_t> indices;
	std::array<Voxel, SIZE * HEIGHT * SIZE> voxels;
	std::bitset<SIZE * HEIGHT * SIZE> activeVoxels;

	std::unordered_map<glm::ivec3, TextureType, IVec3Hash> neighborShellVoxels;

	bool meshNeedsUpdate;
	void uploadMeshToGPU();

	size_t getIndex(uint32_t x, uint32_t y, uint32_t z) const;
	void generateChunk(siv::PerlinNoise *noise);
	void generateTerrainColumn(int x, int z, int terrainHeight, float biomeNoise, siv::PerlinNoise *noise);
	void generateFeatures(int x, int z, int terrainHeight, int worldX, int worldZ, float biomeNoise, siv::PerlinNoise *noise);
	void generateTree(int x, int z, int terrainHeight);
};
