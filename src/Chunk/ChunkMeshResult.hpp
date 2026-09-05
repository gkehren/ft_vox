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

// Vertical mesh sections (issue #107): the logical chunk stays 16x256x16,
// but its full-quality render mesh is one payload per 16x16x16 section so a
// block edit rebuilds and re-uploads only the affected section(s). The
// section count tiling matches the occupancy metadata (16 sections of 16
// voxels, issue #105).
inline constexpr int kChunkSectionCount = CHUNK_HEIGHT / 16;
static_assert(CHUNK_HEIGHT % 16 == 0, "sections must tile the chunk height");
inline constexpr uint16_t kAllSectionMask = 0xFFFFu;

// One section's render payload. Indices are SECTION-LOCAL (base 0): the
// upload rebase them onto the section's GPU vertex slot, so a section can
// move to a new slot without touching any other section's bytes.
struct SectionMeshPayload
{
	std::vector<Vertex> opaqueVertices;
	std::vector<uint32_t> opaqueIndices;
	std::vector<Vertex> waterVertices;
	std::vector<uint32_t> waterIndices;

	bool operator==(const SectionMeshPayload &other) const = default;
};

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
//
// Since issue #107 the full-quality payload lives in `sections` (one entry
// per vertical 16^3 section; a build may fill only the dirty subset - see
// `sectionsBuilt`). The four flat vectors below remain exclusively for the
// whole-chunk LOD mesh of far-band chunks, which is never section-remeshed.
//
// Per-vector byte counters live in MeshResultAccounting and are maintained
// by the pool under its mutex (finishBuild / release). The pool NEVER reads
// the vectors outside the builder's ownership window, so telemetry can be
// sampled concurrently with running builds (issue #114 review).
struct MeshResultAccounting
{
	size_t opaqueVertexSize{0};
	size_t opaqueIndexSize{0};
	size_t waterVertexSize{0};
	size_t waterIndexSize{0};
	size_t opaqueVertexCapacity{0};
	size_t opaqueIndexCapacity{0};
	size_t waterVertexCapacity{0};
	size_t waterIndexCapacity{0};
};

struct MeshBuildResult
{
	// Identity of the build: validated against the chunk at publish time so
	// a superseded build can never land on a recycled/new-generation chunk
	// or on newer voxel/border content (issue #114 review):
	//   generation -> physical chunk incarnation (recycle/reset ID)
	//   revision   -> logical voxel/border content revision
	// A result must match BOTH to be published.
	Chunk *owner{nullptr};
	uint64_t generation{0};
	uint64_t revision{0};
	// True when built by buildLODMesh(); committed to the chunk by
	// publishMeshResult() on the main thread.
	bool isLOD{false};
	// Bit s set <=> sections[s] holds a fresh full-quality payload from this
	// build (issue #107). Full builds and LOD builds set every bit (LOD
	// payloads ignore it). publishMeshResult() ORs the mask back into the
	// chunk's dirty state on rejection so a superseded section is remeshed.
	uint16_t sectionsBuilt{0};
	// Pool the block came from. Results travel worker -> completion queue ->
	// upload, so every holder can return the block without knowing which
	// pool instance produced it (pools travel with their blocks, same rule
	// as voxel/border storage in #112/#113).
	MeshResultPool *homePool{nullptr};

	// Full-quality payload, one slot per vertical section (issue #107).
	// beginBuild() drops previous content while keeping capacity - reuse
	// across jobs is the point.
	std::array<SectionMeshPayload, kChunkSectionCount> sections;
	// Whole-chunk LOD payload (far band only, never section-remeshed).
	std::vector<Vertex> opaqueVertices;
	std::vector<uint32_t> opaqueIndices;
	std::vector<Vertex> waterVertices;
	std::vector<uint32_t> waterIndices;
	// Pool-maintained byte counters (valid under the pool mutex): sizes are
	// non-zero only between finishBuild() and release().
	MeshResultAccounting accounted{};

	// Stamp identity and drop previous content (capacities are kept).
	// `sectionMask` records which sections this build will refresh
	// (kAllSectionMask for whole-chunk builds). Called by the builder; must
	// not touch `accounted` (pool-owned).
	void beginBuild(Chunk *chunkOwner, uint64_t chunkGeneration, uint64_t chunkRevision,
					uint16_t sectionMask = 0);
	// Detach identity and content without freeing capacity (release path).
	void detach();
};

// Aggregated, coherent view of a MeshResultPool (O(1) copy under the mutex;
// no walk of blocks, no read of borrowed vectors). The engine publishes the
// gauges from this snapshot instead of the pool touching telemetry in its
// hot path (same pattern as VoxelPool/BorderPool).
struct MeshResultPoolStats
{
	size_t capacity{0};
	size_t active{0};
	size_t free{0};
	// Live payload bytes across finished, not-yet-released blocks (in-flight
	// builds past finishBuild + results attached to chunks awaiting upload).
	// Sizes/capacities sum the section payloads and the LOD vectors.
	size_t opaqueVertexSize{0};
	size_t opaqueIndexSize{0};
	size_t waterVertexSize{0};
	size_t waterIndexSize{0};
	// Retained capacity across ALL blocks (active + free list): the
	// high-water reuse footprint that used to sit on pooled chunks. Blocks
	// mid-build are counted at their last finished capacity.
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
// staged to GPU. This is a high-water reusable pool whose capacity follows
// the observed in-flight/backlogged working set (concurrent builds plus
// results parked on chunks waiting for staging room) - it is not bounded by
// a hard maximum; mesh.pool.* telemetry exposes any growth. Thread-safe;
// growth beyond the free list allocates outside the mutex and raises one
// grow event. Invalid releases are fatal asserts in Debug and refused
// without corrupting the pool in Release.
//
// Builder lifecycle:
//   acquire() -> beginBuild() -> build into the vectors -> finishBuild()
//   -> (completion queue / publish / upload) -> release()
// finishBuild() must be called after the last vector mutation and before
// release(); it refreshes the accounting, so it may legitimately be called
// again after a deliberate payload mutation (tests/tools) - production
// builders call it exactly once per acquire. It is what makes the payload
// visible to stats() without racing the builder.
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
	// Publish the finished payload sizes/capacities into the pool
	// aggregates. Called by the builder after its last vector mutation;
	// never touches the vectors themselves.
	void finishBuild(MeshBuildResult *result);
	void release(MeshBuildResult *result);

	size_t capacity() const;
	size_t activeCount() const;
	size_t freeCount() const;
	MeshResultPoolStats stats() const;

	void reserve(size_t minCapacity);

	static MeshResultPool &defaultPool();

private:
	void accountFinishLocked(MeshBuildResult *result); // caller holds m_mutex

	mutable std::mutex m_mutex;
	std::vector<std::unique_ptr<MeshBuildResult>> m_storage;
	std::vector<MeshBuildResult *> m_freeList;
	std::vector<const MeshBuildResult *> m_owned;
	// O(1) aggregate snapshot, only touched under m_mutex.
	MeshResultPoolStats m_stats{};
};
