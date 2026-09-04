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
#include <Chunk/VoxelPool.hpp>
#include <Chunk/ChunkBorders.hpp>
#include <Vulkan/VkBuffer.hpp>
#include <Camera/Camera.hpp>
#include <utils.hpp>
#include <Engine/EngineDefs.hpp>

// TextureManager only for static isTransparent — no GL dependency in mesh gen.
#include <Renderer/TextureManager.hpp>

class ImmediateCommands;
class StagingRing;
class GpuResourceRetire;
struct MeshBuildResult;
class MeshResultPool;

class Chunk
{
public:
	Chunk(const glm::vec3 &position, ChunkState state = ChunkState::UNLOADED,
		  VoxelPool *voxelPool = nullptr, BorderPool *borderPool = nullptr,
		  MeshResultPool *meshPool = nullptr);
	Chunk(Chunk &&other) noexcept;
	Chunk &operator=(Chunk &&other) noexcept;
	~Chunk();

	const glm::vec3 &getPosition() const;
	bool isVisible() const;
	void setVisible(bool visible);
	void setState(ChunkState state);
	ChunkState getState() const;

	const Voxel &getVoxel(uint32_t x, uint32_t y, uint32_t z) const;
	bool isVoxelActive(int x, int y, int z) const;
	void setVoxel(int x, int y, int z, TextureType type);

	bool deleteVoxel(const glm::vec3 &position);
	bool placeVoxel(const glm::vec3 &position, TextureType type);

	bool hasVoxelStorage() const { return m_storage != nullptr; }
	VoxelPool *getVoxelPool() const { return m_voxelPool; }
	bool hasBorderStorage() const { return m_borders != nullptr; }
	BorderPool *getBorderPool() const { return m_borderPool; }

	/// Prepare all generation backing on the calling thread:
	/// - voxel storage
	/// - transient neighbor borders
	/// Returns false if an allocation fails (e.g. std::bad_alloc); any
	/// partially acquired backing is returned to its pool first.
	bool prepareVoxelStorageForGeneration();

	/// Release voxel storage upon chunk retirement.
	void releaseVoxelStorageOnRetire();

	/// Bind opaque mesh and draw indexed into cmd. Returns index count.
	uint32_t draw(VkCommandBuffer cmd);
	uint32_t drawWater(VkCommandBuffer cmd);
	void drawShadow(VkCommandBuffer cmd, unsigned cascade = 0) const;

	void generateTerrain(TerrainGenerator &generator);

	// Mesh building (issue #104): the CPU mesh payload no longer lives on
	// the Chunk. Workers build into a pooled MeshBuildResult and publish it
	// for upload; the Chunk keeps only GPU handles/counts.
	//
	// buildMesh/buildLODMesh fill `out` (stamping this chunk's identity) and
	// mark the chunk MESHED; they do not attach anything.
	void buildMesh(MeshBuildResult &out);
	void buildLODMesh(MeshBuildResult &out);
	// Attach a completed build result for upload. Rejects (returns false and
	// returns the block to its pool) when the result is superseded: built by
	// another chunk or for a generation this chunk no longer represents.
	// On success any previously attached result is replaced.
	bool publishMeshResult(MeshBuildResult *result);
	// Synchronous convenience path (bootstrap / tests): acquire a pooled
	// block, build, publish. Returns false when nothing was published
	// (allocation failure or superseded result).
	bool generateMesh();
	bool generateLODMesh();

	bool hasPendingMeshResult() const { return m_pendingResult != nullptr; }
	uint64_t meshGeneration() const { return m_meshGeneration; }
	MeshResultPool *getMeshResultPool() const { return m_resultPool; }
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

	bool isShellEmpty() const { return m_borders == nullptr; }

	/// Layout-independent border sampling for meshing (issue #103).
	/// Accepts the full padded range used by the greedy mesher: in-chunk
	/// coordinates read local voxels, x/z = -1 / CHUNK_SIZE read neighbor
	/// faces, diagonal coordinates read corner columns, and everything else
	/// (including vertical padding) reads as AIR. Missing borders - freed
	/// after upload or never built - also read as AIR.
	TextureType sampleForMeshing(int x, int y, int z) const;
	void releaseNeighborBorders();
	void rebuildBordersFromNeighbors(const Chunk *west, const Chunk *east,
								   const Chunk *south, const Chunk *north);

	enum class ResetMode { Full, ForGeneration };
	/// ForGeneration retains voxel storage (if any) until generateTerrain()
	/// overwrites it, avoiding deallocation/reallocation during pool recycling.
	/// Do not read/mesh voxels in this chunk before generation completes.
	/// Full (the default) releases voxel storage back to the pool, for retirement without regeneration.
	void reset(const glm::vec3 &newPosition, ResetMode mode = ResetMode::Full);

	uint32_t getOpaqueIndexCount() const { return opaqueIndexCount; }

	size_t getActiveIndex() const { return m_activeIndex; }
	void setActiveIndex(size_t index) { m_activeIndex = index; }

private:
    // Published by the exclusive chunk owner at mutation boundaries.
    std::array<uint64_t, 13> m_cpuTelemetry{};
    void publishCpuTelemetry();
    struct MemoryPublication {
        Chunk& chunk;
        ~MemoryPublication() { chunk.publishCpuTelemetry(); }
    };
	glm::vec3 position;
	bool visible;
	std::atomic<ChunkState> state;

	VmaAllocator m_allocator{VK_NULL_HANDLE};
	AllocatedBuffer vertexBuffer{};
	AllocatedBuffer indexBuffer{};
	AllocatedBuffer waterVertexBuffer{};
	AllocatedBuffer waterIndexBuffer{};

	VoxelPool *m_voxelPool{nullptr};
	VoxelStorage *m_storage{nullptr};
	void ensureVoxelStorageForEdit();
	std::bitset<CHUNK_VOLUME> activeVoxels;
	// Borrowed from a BorderPool for generation/meshing and returned after
	// upload (issue #103): no per-chunk border memory is retained.
	ChunkNeighborBorders *m_borders{nullptr};
	BorderPool *m_borderPool{nullptr};
	// Mesh build buffers are pooled too (issue #104): m_pendingResult holds
	// a completed CPU mesh awaiting upload, borrowed from m_resultPool.
	// Released on upload, reset, and moves - no per-chunk mesh capacity.
	MeshResultPool *m_resultPool{nullptr};
	MeshBuildResult *m_pendingResult{nullptr};
	uint64_t m_meshGeneration{0};
	void releasePendingMeshResult();

	std::array<uint32_t, CHUNK_SIZE * CHUNK_SIZE> biomeGrassColors{};
	std::array<uint32_t, CHUNK_SIZE * CHUNK_SIZE> biomeFoliageColors{};

	// Per-column generation state, filled by generateTerrain() (biomes feed
	// vegetation/border passes; heightMap mirrors ChunkData::heightMap).
	// Fully reset on every generation and pool recycle.
	std::array<BiomeType, CHUNK_SIZE * CHUNK_SIZE> biomeTypes{};
	std::array<int, CHUNK_SIZE * CHUNK_SIZE> heightMap{};

	uint32_t opaqueIndexCount{0};
	uint32_t waterIndexCount{0};

	std::atomic<bool> meshNeedsUpdate;
	bool m_isLODMesh{false};
	std::atomic<bool> m_inTransit{false};

	size_t getIndex(uint32_t x, uint32_t y, uint32_t z) const;

private:
	size_t m_activeIndex{SIZE_MAX};

	// Testing hook (issue #78 review): lets the chunk lifecycle test verify
	// full move semantics - including per-column generation state - without
	// exposing that state in the public API.
	friend struct ChunkStateProbe;
};
