#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>
#include <utils.hpp>

class Chunk;
class MeshResultPool;

// Completed CPU mesh build detached from Chunk lifetime (issue #104).
//
// The greedy mesher used to write straight into four vectors owned by the
// Chunk; after GPU upload only the sizes were cleared and the capacities
// stayed attached to the pooled Chunk object forever. A pool entry that had
// once meshed a complex forest kept that high-water capacity even when its
// next incarnation needed far less, so long-run CPU mesh memory trended
// toward the historical worst case across the whole chunk pool.
//
// A build result is instead borrowed from a MeshResultPool: workers build
// into it, hand it to the upload stage, and the block goes back to the pool
// once the payload is staged. Retained capacity therefore scales with
// concurrent meshing/upload work, not with the number of pooled chunks.
struct MeshBuildResult
{
	// Identity of the build: validated against the chunk at publish time so
	// a superseded build can never land on a recycled/new-generation chunk.
	Chunk *owner{nullptr};
	uint64_t generation{0};
	// Pool the block came from. Results travel worker -> completion queue ->
	// upload, so every holder can return the block without knowing which
	// pool instance produced it (pools travel with their blocks, same rule
	// as voxel/border storage in #112/#113).
	MeshResultPool *homePool{nullptr};

	// No default member initializers on the vectors beyond empty construction:
	// pool blocks are created with make_unique_for_overwrite and their
	// capacities are (re)grown by consumers. beginBuild() drops previous
	// content while keeping capacity - reuse across jobs is the point.
	std::vector<Vertex> opaqueVertices;
	std::vector<uint32_t> opaqueIndices;
	std::vector<Vertex> waterVertices;
	std::vector<uint32_t> waterIndices;

	// Stamp identity and drop previous content (capacities are kept).
	void beginBuild(Chunk *chunkOwner, uint64_t chunkGeneration);
	// Detach identity and content without freeing capacity (release path).
	void detach();
};

// Aggregated, coherent view of a MeshResultPool (one locked walk). The
// engine publishes the gauges from this snapshot instead of the pool
// touching telemetry in its hot path (same pattern as VoxelPool/BorderPool).
struct MeshResultPoolStats
{
	size_t capacity{0};
	size_t active{0};
	size_t free{0};
	// Live payload bytes across ACTIVE blocks (in-flight builds + results
	// attached to chunks awaiting upload).
	size_t opaqueVertexSize{0};
	size_t opaqueIndexSize{0};
	size_t waterVertexSize{0};
	size_t waterIndexSize{0};
	// Retained capacity across ALL blocks (active + free list): the
	// high-water reuse footprint that used to sit on pooled chunks.
	size_t opaqueVertexCapacity{0};
	size_t opaqueIndexCapacity{0};
	size_t waterVertexCapacity{0};
	size_t waterIndexCapacity{0};
	size_t capacityBytes() const
	{
		return opaqueVertexCapacity + opaqueIndexCapacity +
			   waterVertexCapacity + waterIndexCapacity;
	}
};

// Pool of pointer-stable MeshBuildResult blocks (same contract as
// BorderPool): borrowed for a mesh build, returned after the payload is
// staged to GPU. Thread-safe; growth beyond the free list allocates outside
// the mutex and raises one grow event. Invalid releases are fatal asserts in
// Debug and refused without corrupting the pool in Release.
class MeshResultPool
{
public:
	explicit MeshResultPool(size_t initialCapacity = 0);
	~MeshResultPool();

	MeshResultPool(const MeshResultPool &) = delete;
	MeshResultPool &operator=(const MeshResultPool &) = delete;
	MeshResultPool(MeshResultPool &&) = delete;
	MeshResultPool &operator=(MeshResultPool &&) = delete;

	MeshBuildResult *acquire();
	void release(MeshBuildResult *result);

	size_t capacity() const;
	size_t activeCount() const;
	size_t freeCount() const;
	MeshResultPoolStats stats() const;

	void reserve(size_t minCapacity);

	static MeshResultPool &defaultPool();

private:
	mutable std::mutex m_mutex;
	std::vector<std::unique_ptr<MeshBuildResult>> m_storage;
	std::vector<MeshBuildResult *> m_freeList;
	std::vector<const MeshBuildResult *> m_owned;
	std::atomic<size_t> m_activeCount{0};
};
