#pragma once

#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif

#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <array>
#include <atomic>
#include <bitset>
#include <mutex>
#include <unordered_map>
#include <glm/gtx/hash.hpp>

#include <chrono>

#include <Chunk/TerrainGenerator.hpp>
#include <Vulkan/VkBuffer.hpp>
#include <Camera/Camera.hpp>
#include <utils.hpp>
#include <Engine/EngineDefs.hpp>

// TextureManager only for static isTransparent — no GL dependency in mesh gen.
#include <Renderer/TextureManager.hpp>

class ImmediateCommands;
class StagingRing;
class GpuResourceRetire;

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

	/// Bind opaque mesh and draw indexed into cmd. Returns index count.
	uint32_t draw(VkCommandBuffer cmd);
	uint32_t drawWater(VkCommandBuffer cmd);
	void drawShadow(VkCommandBuffer cmd) const;

	void generateTerrain(TerrainGenerator &generator);
	void generateMesh();
	void generateLODMesh();
	bool hasWaterMesh() const { return waterIndexCount > 0; }
	bool isLODMesh() const { return m_isLODMesh; }
	bool needsGPUUpload() const { return meshNeedsUpdate.load(); }
	bool isInTransit() const { return m_inTransit.load(); }
	void setInTransit(bool val) { m_inTransit.store(val); }

	/// Synchronous upload (bootstrap / tests). Prefer uploadToGPUAsync on the hot path.
	void uploadToGPU(VmaAllocator allocator, ImmediateCommands &imm);

	/// Record buffer copies into cmd using the staging ring; retires previous GPU buffers.
	/// Returns false if staging is full (CPU mesh kept; try again next frame).
	bool uploadToGPUAsync(VmaAllocator allocator, StagingRing &staging, VkCommandBuffer cmd,
						  GpuResourceRetire &retire);

	/// Immediate destroy (shutdown / destructor only — not while frames may reference buffers).
	void releaseGPU();
	/// Hand buffers to the retire queue; safe during streaming unload/remesh.
	void releaseGPUDeferred(GpuResourceRetire &retire);

	bool isShellEmpty() const { return neighborShellVoxels.empty(); }
	void freeShellVoxels();
	void rebuildShellFromNeighbors(const Chunk *west, const Chunk *east,
								   const Chunk *south, const Chunk *north);

	void reset(const glm::vec3 &newPosition);

	uint32_t getOpaqueIndexCount() const { return opaqueIndexCount; }

	size_t getActiveIndex() const { return m_activeIndex; }
	void setActiveIndex(size_t index) { m_activeIndex = index; }

private:
	glm::vec3 position;
	bool visible;
	std::atomic<ChunkState> state;

	VmaAllocator m_allocator{VK_NULL_HANDLE};
	AllocatedBuffer vertexBuffer{};
	AllocatedBuffer indexBuffer{};
	AllocatedBuffer waterVertexBuffer{};
	AllocatedBuffer waterIndexBuffer{};

	std::vector<Vertex> vertices;
	std::vector<uint32_t> indices;
	std::vector<Vertex> waterVertices;
	std::vector<uint32_t> waterIndices;
	std::vector<Voxel> voxels;
	std::bitset<CHUNK_VOLUME> activeVoxels;
	std::vector<uint8_t> neighborShellVoxels;

	std::array<uint32_t, CHUNK_SIZE * CHUNK_SIZE> biomeGrassColors{};
	std::array<uint32_t, CHUNK_SIZE * CHUNK_SIZE> biomeFoliageColors{};

	uint32_t opaqueIndexCount{0};
	uint32_t waterIndexCount{0};

	std::atomic<bool> meshNeedsUpdate;
	bool m_isLODMesh{false};
	std::atomic<bool> m_inTransit{false};

	size_t getIndex(uint32_t x, uint32_t y, uint32_t z) const;

private:
	size_t m_activeIndex{SIZE_MAX};
};
