#pragma once

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

	uint32_t draw(const Shader &shader, const Camera &camera, GLuint textureAtlas);
	void generateVoxels(siv::PerlinNoise *perlin);
	void generateMesh();

private:
	glm::vec3 position;
	bool visible;
	ChunkState state;

	// Stockage linéaire 3D des voxels
	std::array<Voxel, SIZE * HEIGHT * SIZE> voxels;
	std::bitset<SIZE * HEIGHT * SIZE> activeVoxels;

	// neighbours voxels
	std::bitset<SIZE * HEIGHT * 4> neighboursActiveMap;
	Voxel &getNeighbourVoxel(int x, int y, int z);
	size_t getNeighbourIndex(int x, int y, int z) const;

	size_t getIndex(uint32_t x, uint32_t y, uint32_t z) const;

	GLuint VAO;
	GLuint VBO;
	GLuint EBO;

	std::vector<Vertex> vertices;
	std::vector<uint16_t> indices;
	bool meshNeedsUpdate;
	void uploadMeshToGPU();

	void generateChunk(siv::PerlinNoise *perlin);
};
