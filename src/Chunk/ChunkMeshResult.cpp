#include "ChunkMeshResult.hpp"
#include <Engine/WorkloadTelemetry.hpp>
#include <algorithm>
#include <cassert>

namespace
{
// Byte totals across the full-quality section payloads and the LOD vectors;
// shared by beginBuild/detach clearing and the pool accounting.
struct PayloadTotals
{
	size_t opaqueVertexSize{0}, opaqueIndexSize{0};
	size_t waterVertexSize{0}, waterIndexSize{0};
	size_t opaqueVertexCapacity{0}, opaqueIndexCapacity{0};
	size_t waterVertexCapacity{0}, waterIndexCapacity{0};
};

PayloadTotals payloadTotals(const MeshBuildResult &r)
{
	PayloadTotals t;
	for (const SectionMeshPayload &s : r.sections)
	{
		t.opaqueVertexSize += s.opaqueVertices.size() * sizeof(Vertex);
		t.opaqueIndexSize += s.opaqueIndices.size() * sizeof(uint32_t);
		t.waterVertexSize += s.waterVertices.size() * sizeof(Vertex);
		t.waterIndexSize += s.waterIndices.size() * sizeof(uint32_t);
		t.opaqueVertexCapacity += s.opaqueVertices.capacity() * sizeof(Vertex);
		t.opaqueIndexCapacity += s.opaqueIndices.capacity() * sizeof(uint32_t);
		t.waterVertexCapacity += s.waterVertices.capacity() * sizeof(Vertex);
		t.waterIndexCapacity += s.waterIndices.capacity() * sizeof(uint32_t);
	}
	t.opaqueVertexSize += r.opaqueVertices.size() * sizeof(Vertex);
	t.opaqueIndexSize += r.opaqueIndices.size() * sizeof(uint32_t);
	t.waterVertexSize += r.waterVertices.size() * sizeof(Vertex);
	t.waterIndexSize += r.waterIndices.size() * sizeof(uint32_t);
	t.opaqueVertexCapacity += r.opaqueVertices.capacity() * sizeof(Vertex);
	t.opaqueIndexCapacity += r.opaqueIndices.capacity() * sizeof(uint32_t);
	t.waterVertexCapacity += r.waterVertices.capacity() * sizeof(Vertex);
	t.waterIndexCapacity += r.waterIndices.capacity() * sizeof(uint32_t);
	return t;
}
} // namespace

void MeshBuildResult::beginBuild(Chunk *chunkOwner, uint64_t chunkGeneration,
								 uint64_t chunkRevision, uint16_t sectionMask)
{
	owner = chunkOwner;
	generation = chunkGeneration;
	revision = chunkRevision;
	sectionsBuilt = sectionMask;
	for (SectionMeshPayload &s : sections)
	{
		s.opaqueVertices.clear();
		s.opaqueIndices.clear();
		s.waterVertices.clear();
		s.waterIndices.clear();
	}
	opaqueVertices.clear();
	opaqueIndices.clear();
	waterVertices.clear();
	waterIndices.clear();
	// `accounted` is intentionally untouched: it belongs to the pool and is
	// only read/written under the pool mutex (finishBuild/release).
}

void MeshBuildResult::detach()
{
	owner = nullptr;
	generation = 0;
	revision = 0;
	isLOD = false;
	sectionsBuilt = 0;
	// Drop content (sizes only - capacity stays with the pool block so the
	// next borrower does not start from zero allocations). The matching
	// size accounting is subtracted by release() under the pool mutex.
	for (SectionMeshPayload &s : sections)
	{
		s.opaqueVertices.clear();
		s.opaqueIndices.clear();
		s.waterVertices.clear();
		s.waterIndices.clear();
	}
	opaqueVertices.clear();
	opaqueIndices.clear();
	waterVertices.clear();
	waterIndices.clear();
}

MeshResultPool::MeshResultPool(size_t initialCapacity)
{
	if (initialCapacity > 0)
		reserve(initialCapacity);
}

MeshResultPool::~MeshResultPool() = default;

MeshBuildResult *MeshResultPool::acquire()
{
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (!m_freeList.empty())
		{
			MeshBuildResult *r = m_freeList.back();
			m_freeList.pop_back();
			--m_stats.free;
			++m_stats.active;
			return r;
		}
	}

	// Allocate outside the mutex. MeshBuildResult holds vectors, so use the
	// constructing make_unique (not make_unique_for_overwrite): the pool
	// owns allocation/ownership/reuse only - it does not impose logical
	// content; the borrower stamps identity via beginBuild() and the mesher
	// fills the vectors.
	auto block = std::make_unique<MeshBuildResult>();
	MeshBuildResult *ptr = block.get();
	ptr->homePool = this;
	std::lock_guard<std::mutex> lock(m_mutex);
	m_storage.push_back(std::move(block));
	auto it = std::lower_bound(m_owned.begin(), m_owned.end(), ptr);
	m_owned.insert(it, ptr);
	++m_stats.capacity;
	++m_stats.active;
	telemetry::registry().add(telemetry::MeshPoolGrow);
	return ptr;
}

void MeshResultPool::accountFinishLocked(MeshBuildResult *result)
{
	const PayloadTotals now = payloadTotals(*result);

	// Replace the block's previously accounted values with the fresh ones:
	// sizes were zero since release(), capacities may have grown during this
	// job (delta accounting without scanning anything).
	m_stats.opaqueVertexSize += now.opaqueVertexSize - result->accounted.opaqueVertexSize;
	m_stats.opaqueIndexSize += now.opaqueIndexSize - result->accounted.opaqueIndexSize;
	m_stats.waterVertexSize += now.waterVertexSize - result->accounted.waterVertexSize;
	m_stats.waterIndexSize += now.waterIndexSize - result->accounted.waterIndexSize;
	m_stats.opaqueVertexCapacity += now.opaqueVertexCapacity - result->accounted.opaqueVertexCapacity;
	m_stats.opaqueIndexCapacity += now.opaqueIndexCapacity - result->accounted.opaqueIndexCapacity;
	m_stats.waterVertexCapacity += now.waterVertexCapacity - result->accounted.waterVertexCapacity;
	m_stats.waterIndexCapacity += now.waterIndexCapacity - result->accounted.waterIndexCapacity;
	result->accounted.opaqueVertexSize = now.opaqueVertexSize;
	result->accounted.opaqueIndexSize = now.opaqueIndexSize;
	result->accounted.waterVertexSize = now.waterVertexSize;
	result->accounted.waterIndexSize = now.waterIndexSize;
	result->accounted.opaqueVertexCapacity = now.opaqueVertexCapacity;
	result->accounted.opaqueIndexCapacity = now.opaqueIndexCapacity;
	result->accounted.waterVertexCapacity = now.waterVertexCapacity;
	result->accounted.waterIndexCapacity = now.waterIndexCapacity;
}

void MeshResultPool::finishBuild(MeshBuildResult *result)
{
	assert(result != nullptr && "Cannot finish null MeshBuildResult");
	if (!result)
		return;

	std::lock_guard<std::mutex> lock(m_mutex);

	// Same ownership validation as release(): only a block that is
	// currently acquired by this pool may be finished. Debug makes the
	// programming error fatal; Release refuses without corrupting the
	// accounting.
	const bool owned = std::binary_search(m_owned.begin(), m_owned.end(), result);
	assert(owned && "finishBuild on foreign MeshBuildResult");
	if (!owned)
		return;
	const bool alreadyFree =
		std::find(m_freeList.begin(), m_freeList.end(), result) != m_freeList.end();
	assert(!alreadyFree && "finishBuild on a released MeshBuildResult");
	if (alreadyFree)
		return;

	accountFinishLocked(result);
}

void MeshResultPool::release(MeshBuildResult *result)
{
	assert(result != nullptr && "Cannot release null MeshBuildResult");
	if (!result)
		return;

	std::lock_guard<std::mutex> lock(m_mutex);

	// Two protection levels (same policy as VoxelPool/BorderPool):
	// Debug: a programming error is fatal and immediately visible.
	// Release: refuse invalid ownership without corrupting the pool.
	const bool owned = std::binary_search(m_owned.begin(), m_owned.end(), result);
	assert(owned && "MeshBuildResult returned to wrong pool");
	if (!owned)
		return;

	const bool alreadyFree =
		std::find(m_freeList.begin(), m_freeList.end(), result) != m_freeList.end();
	assert(!alreadyFree && "Double release of MeshBuildResult detected");
	if (alreadyFree)
		return;

	// Bookkeeping, not content policy: subtract the block's live-payload
	// accounting (capacities stay - the free list keeps them) and detach
	// stale identity/payload so a released block cannot alias a live chunk.
	m_stats.opaqueVertexSize -= result->accounted.opaqueVertexSize;
	m_stats.opaqueIndexSize -= result->accounted.opaqueIndexSize;
	m_stats.waterVertexSize -= result->accounted.waterVertexSize;
	m_stats.waterIndexSize -= result->accounted.waterIndexSize;
	result->accounted.opaqueVertexSize = 0;
	result->accounted.opaqueIndexSize = 0;
	result->accounted.waterVertexSize = 0;
	result->accounted.waterIndexSize = 0;
	result->detach();
	m_freeList.push_back(result);
	--m_stats.active;
	++m_stats.free;
}

size_t MeshResultPool::capacity() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_stats.capacity;
}

size_t MeshResultPool::activeCount() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_stats.active;
}

size_t MeshResultPool::freeCount() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_stats.free;
}

MeshResultPoolStats MeshResultPool::stats() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_stats; // O(1): no block walk, no borrowed-vector reads
}

void MeshResultPool::reserve(size_t minCapacity)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_storage.size() >= minCapacity)
		return;

	const size_t addCount = minCapacity - m_storage.size();
	m_storage.reserve(m_storage.size() + addCount);
	m_freeList.reserve(m_freeList.size() + addCount);
	m_owned.reserve(m_owned.size() + addCount);

	for (size_t i = 0; i < addCount; ++i)
	{
		auto block = std::make_unique<MeshBuildResult>();
		MeshBuildResult *ptr = block.get();
		ptr->homePool = this;
		m_storage.push_back(std::move(block));
		m_freeList.push_back(ptr);
		m_owned.push_back(ptr);
		++m_stats.capacity;
		++m_stats.free;
	}
	std::sort(m_owned.begin(), m_owned.end());
}

MeshResultPool &MeshResultPool::defaultPool()
{
	static MeshResultPool s_defaultPool;
	return s_defaultPool;
}
