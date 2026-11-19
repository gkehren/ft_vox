// WorldChunk — authoritative CPU-only chunk record (issue #206, Epic #204 B).
// Ported from the voxel side of Chunk.cpp / ChunkEditing.cpp / ChunkMeshing.cpp
// with render scheduling (dirty sections, mesh flags, GPU, light caches) left
// on the render-side Chunk. Behavior of every authoritative path is unchanged:
// storage lifecycle, occupancy deltas, persistent-edit capture, border mirror
// writes, generation and recycle identity bumps.
#include "WorldChunk.hpp"

#include <algorithm>
#include <cmath>
#include <new>

WorldChunk::WorldChunk(const glm::vec3 &position, VoxelPool *voxelPool, BorderPool *borderPool)
	: position(position),
	  m_voxelPool(voxelPool ? voxelPool : &VoxelPool::defaultPool()),
	  m_borderPool(borderPool ? borderPool : &BorderPool::defaultPool())
{
	publishCpuTelemetry();
}

WorldChunk::~WorldChunk()
{
	releaseNeighborBorders();
	releaseVoxelStorageOnRetire();
	telemetry::registry().replaceCpu(m_cpuTelemetry, std::array<uint64_t, 13>{});
}

glm::ivec3 WorldChunk::coord() const
{
	return ::worldToChunkCoord(position);
}

bool WorldChunk::prepareVoxelStorageForGeneration()
{
	m_terrainPublished = false;
	// Borders are borrowed on the same main-thread step (issue #103): workers
	// stay allocation-free and generation writes into the record's border
	// block. Ownership ONLY (issue #115): a freshly acquired voxel block keeps
	// its previous incarnation's bytes until generateTerrain() overwrites them.
	if (m_storage && m_borders)
		return true;
	try
	{
		if (!m_storage)
		{
			m_storage = m_voxelPool->acquire();
			// Logical occupancy is empty until generation initializes the backing.
			m_sectionNonAir.fill(0);
			publishCpuTelemetry();
		}
		if (!m_borders)
		{
			m_borders = m_borderPool->acquire();
			publishCpuTelemetry();
		}
		return true;
	}
	catch (const std::bad_alloc &)
	{
		// Return whatever was acquired so partially prepared state cannot leak.
		releaseNeighborBorders();
		releaseVoxelStorageOnRetire();
		return false;
	}
}

void WorldChunk::releaseVoxelStorageOnRetire()
{
	m_terrainPublished = false;
	if (!m_storage)
		return;
	VoxelStorage *st = m_storage;
	m_storage = nullptr;
	m_voxelPool->release(st);
	// Invariant: no storage => empty occupancy metadata (issue #105).
	m_sectionNonAir.fill(0);
	publishCpuTelemetry();
}

void WorldChunk::ensureVoxelStorageForEdit()
{
	if (!m_storage)
	{
		m_storage = m_voxelPool->acquire();
		std::fill(m_storage->voxels.begin(), m_storage->voxels.end(), Voxel{static_cast<uint8_t>(AIR)});
		// Fresh storage is all-air: pin the occupancy metadata to it.
		m_sectionNonAir.fill(0);
		publishCpuTelemetry();
	}
}

TextureType WorldChunk::setVoxel(int x, int y, int z, TextureType type)
{
	if (static_cast<uint32_t>(x) < CHUNK_SIZE && static_cast<uint32_t>(y) < CHUNK_HEIGHT &&
		static_cast<uint32_t>(z) < CHUNK_SIZE)
	{
		const size_t idx = index(x, y, z);
		const TextureType previousType =
			m_storage ? static_cast<TextureType>(m_storage->voxels[idx].type) : AIR;
		const bool wasAir = !m_storage ||
							m_storage->voxels[idx].type == static_cast<uint8_t>(AIR);
		if (type != AIR)
		{
			ensureVoxelStorageForEdit();
			m_storage->voxels[idx].type = static_cast<uint8_t>(type);
		}
		else
		{
			if (m_storage)
				m_storage->voxels[idx].type = static_cast<uint8_t>(AIR);
		}
		// Persistent-edit capture (issue #180): only authoritative in-chunk
		// writes that actually change the voxel are recorded; border-shell
		// writes (the else branch below) and no-op rewrites never record.
		if (m_trackPersistentEdits && previousType != type)
			m_persistentEdits[static_cast<uint32_t>(idx)] = static_cast<uint8_t>(type);
		// Incremental occupancy (issue #105): a cell moved in or out of its
		// vertical section; type->type rewrites leave the count untouched.
		const bool nowAir = (type == AIR);
		if (wasAir != nowAir)
		{
			[[maybe_unused]] constexpr uint16_t kSectionVolume =
				static_cast<uint16_t>(kOccupancySectionSize) * CHUNK_SIZE * CHUNK_SIZE;
			const int section = y / kOccupancySectionSize;
			uint16_t &count = m_sectionNonAir[section];
			if (nowAir)
			{
				assert(count > 0 && "occupancy section underflow");
				--count;
			}
			else
			{
				assert(count < kSectionVolume && "occupancy section overflow");
				++count;
			}
		}
		// Content invalidation (issue #114 review): any in-flight mesh built
		// from the previous content is rejected at publish time. A no-op
		// rewrite (previousType == type) changes no content, so it must NOT
		// bump the revision — otherwise placing an identical block would
		// reject in-flight builds and force a pointless remesh (review M10).
		if (previousType != type)
			m_revision.fetch_add(1, std::memory_order_relaxed);
		return previousType;
	}
	if (x >= -1 && x <= CHUNK_SIZE && z >= -1 && z <= CHUNK_SIZE &&
		y >= 0 && y < static_cast<int>(CHUNK_HEIGHT))
	{
		// Border (mirror) write. Lazily re-borrow borders if they were freed
		// after a previous GPU upload; a freshly acquired pool block holds
		// stale bytes, so initialize it to AIR first (issue #113). Acquisition
		// may throw on OOM; a failed edit write is not fatal (block unchanged).
		if (!m_borders)
		{
			try
			{
				m_borders = m_borderPool->acquire();
				m_borders->resetToAir();
				publishCpuTelemetry();
			}
			catch (const std::bad_alloc &)
			{
				return AIR;
			}
		}
		const TextureType previousType = static_cast<TextureType>(m_borders->at(x, y, z));
		m_borders->mutableAt(x, y, z) = static_cast<uint8_t>(type);
		// Border content invalidation (issue #114 review): mirror writes from
		// a neighbor's boundary edit invalidate in-flight meshes of THIS chunk
		// just like in-chunk edits do. No-op mirror rewrites skip the bump
		// for the same reason as in-chunk writes (review M10).
		if (previousType != type)
			m_revision.fetch_add(1, std::memory_order_relaxed);
		return previousType;
	}
	return AIR;
}

bool WorldChunk::isVoxelActive(int x, int y, int z) const
{
	if (static_cast<uint32_t>(x) < CHUNK_SIZE && static_cast<uint32_t>(y) < CHUNK_HEIGHT &&
		static_cast<uint32_t>(z) < CHUNK_SIZE)
	{
		if (!m_storage)
			return false;
		// Canonical occupancy (issue #105): the voxel type itself, no bitset.
		return m_storage->voxels[index(x, y, z)].type != static_cast<uint8_t>(AIR);
	}
	if (x >= -1 && x <= CHUNK_SIZE && z >= -1 && z <= CHUNK_SIZE)
		return m_borders && m_borders->at(x, y, z) != static_cast<uint8_t>(AIR);
	return false; // Outside known boundaries
}

TextureType WorldChunk::sampleForMeshing(int x, int y, int z) const
{
	if (static_cast<uint32_t>(x) < CHUNK_SIZE && static_cast<uint32_t>(y) < CHUNK_HEIGHT &&
		static_cast<uint32_t>(z) < CHUNK_SIZE)
		return static_cast<TextureType>(getVoxel(x, y, z).type);
	if (x < -1 || x > CHUNK_SIZE || z < -1 || z > CHUNK_SIZE)
		return AIR; // outside the padded neighborhood
	if (!m_borders)
		return AIR; // borders freed after upload or never built (documented default)
	return static_cast<TextureType>(m_borders->at(x, y, z));
}

void WorldChunk::releaseNeighborBorders()
{
	if (!m_borders)
		return;
	MemoryPublication memoryPublication{*this};
	// True release (issue #103): border blocks live in a pool, so the block is
	// reusable by other chunks. A later remesh re-borrows and rebuilds from
	// neighbors (AIR where neighbors are missing).
	ChunkNeighborBorders *b = m_borders;
	m_borders = nullptr;
	m_borderPool->release(b);
	publishCpuTelemetry();
}

void WorldChunk::rebuildBordersFromNeighbors(const WorldChunk *west, const WorldChunk *east,
											 const WorldChunk *south, const WorldChunk *north)
{
	MemoryPublication memoryPublication{*this};
	if (!m_borders)
	{
		try
		{
			m_borders = m_borderPool->acquire();
		}
		catch (const std::bad_alloc &)
		{
			return; // leave borders missing: mesher samples AIR (documented default)
		}
		publishCpuTelemetry();
	}
	ChunkNeighborBorders &b = *m_borders;
	b.resetToAir(); // faces and corners start as AIR for this rebuild

	// West face  (local x = -1):  neighbor's x = CHUNK_SIZE-1
	if (west)
		for (uint32_t y = 0; y < CHUNK_HEIGHT; ++y)
			for (uint32_t z = 0; z < CHUNK_SIZE; ++z)
				b.west[y * CHUNK_SIZE + z] = west->getVoxel(CHUNK_SIZE - 1, y, z).type;

	// East face  (local x = CHUNK_SIZE): neighbor's x = 0
	if (east)
		for (uint32_t y = 0; y < CHUNK_HEIGHT; ++y)
			for (uint32_t z = 0; z < CHUNK_SIZE; ++z)
				b.east[y * CHUNK_SIZE + z] = east->getVoxel(0, y, z).type;

	// South face (local z = -1):  neighbor's z = CHUNK_SIZE-1
	if (south)
		for (uint32_t y = 0; y < CHUNK_HEIGHT; ++y)
			for (uint32_t x = 0; x < CHUNK_SIZE; ++x)
				b.south[y * CHUNK_SIZE + x] = south->getVoxel(x, y, CHUNK_SIZE - 1).type;

	// North face (local z = CHUNK_SIZE): neighbor's z = 0
	if (north)
		for (uint32_t y = 0; y < CHUNK_HEIGHT; ++y)
			for (uint32_t x = 0; x < CHUNK_SIZE; ++x)
				b.north[y * CHUNK_SIZE + x] = north->getVoxel(x, y, 0).type;

	// Corner columns stay AIR exactly as in the previous rebuild path, which
	// never populated the diagonal shell columns either.

	// Border content changed: invalidate in-flight results stamped with the
	// previous revision (issue #114 review).
	m_revision.fetch_add(1, std::memory_order_relaxed);
}

void WorldChunk::recountOccupancy()
{
	m_sectionNonAir.fill(0);
	if (!m_storage)
		return;
	constexpr size_t kSectionVolume =
		static_cast<size_t>(kOccupancySectionSize) * CHUNK_SIZE * CHUNK_SIZE;
	for (int section = 0; section < kOccupancySections; ++section)
	{
		const Voxel *cells = m_storage->voxels.data() + section * kSectionVolume;
		uint16_t count = 0;
		for (size_t i = 0; i < kSectionVolume; ++i)
			count += cells[i].type != static_cast<uint8_t>(AIR) ? 1 : 0;
		m_sectionNonAir[section] = count;
	}
}

bool WorldChunk::occupiedSpanY(int &outMinY, int &outMaxY) const
{
#ifndef NDEBUG
	validateOccupancyMetadata();
#endif
	int first = 0;
	int last = kOccupancySections - 1;
	while (first <= last && m_sectionNonAir[first] == 0)
		++first;
	if (first > last)
		return false; // no non-air voxel anywhere
	while (m_sectionNonAir[last] == 0)
		--last;
	outMinY = first * kOccupancySectionSize;
	outMaxY = last * kOccupancySectionSize + (kOccupancySectionSize - 1);
	return true;
}

void WorldChunk::refineOccupiedSpanY(int &occMinY, int &occMaxY) const
{
	if (!m_storage)
		return;
	const auto layerHasVoxel = [this](int y) -> bool
	{
		const Voxel *layer =
			m_storage->voxels.data() + static_cast<size_t>(y) * CHUNK_SIZE * CHUNK_SIZE;
		for (int i = 0; i < CHUNK_SIZE * CHUNK_SIZE; ++i)
			if (layer[i].type != static_cast<uint8_t>(AIR))
				return true;
		return false;
	};
	while (occMinY < occMaxY && !layerHasVoxel(occMinY))
		++occMinY;
	while (occMaxY > occMinY && !layerHasVoxel(occMaxY))
		--occMaxY;
}

uint16_t WorldChunk::occupiedSectionMask() const
{
	uint16_t mask = 0;
	for (int section = 0; section < kOccupancySections; ++section)
		if (m_sectionNonAir[section] != 0)
			mask |= static_cast<uint16_t>(1u << section);
	return mask;
}

void WorldChunk::validateOccupancyMetadata() const
{
	// A per-section count can never exceed the section's voxel count, and a
	// storageless record must be empty (issue #115 review). The locals are
	// assert-only: [[maybe_unused]] keeps Release builds warning-clean.
	[[maybe_unused]] constexpr uint16_t kSectionVolume =
		static_cast<uint16_t>(kOccupancySectionSize) * CHUNK_SIZE * CHUNK_SIZE;
	for ([[maybe_unused]] const uint16_t count : m_sectionNonAir)
		assert(count <= kSectionVolume && "occupancy count above section volume");
	if (!m_storage)
	{
		for ([[maybe_unused]] const uint16_t count : m_sectionNonAir)
			assert(count == 0 && "storageless chunk must have empty occupancy");
	}
}

bool WorldChunk::generateTerrain(TerrainGenerator &generator)
{
	MemoryPublication memoryPublication{*this};
	if (state.load() != ChunkState::UNLOADED)
		return false;

	// Ensure we use integer coordinates aligned with the world grid.
	const int genX = static_cast<int>(std::round(position.x));
	const int genZ = static_cast<int>(std::round(position.z));

	// Generate directly into this record's reusable storage: no temporary
	// ChunkData and no CHUNK_VOLUME / border-shell copies after generation.
	// Storage and border blocks MUST have been prepared on the main thread
	// prior to dispatch (issue #103).
	assert(m_storage != nullptr && "VoxelStorage must be prepared before generateTerrain");
	assert(m_borders != nullptr && "Borders must be prepared before generateTerrain");
	if (!m_storage || !m_borders)
		return false;

	ChunkGenerationTarget target{
		.voxels = std::span<Voxel, CHUNK_VOLUME>(m_storage->voxels),
		.borders = m_borders,
		.biomes = biomeTypes,
		.heightMap = heightMap,
		.grassColors = biomeGrassColors,
		.foliageColors = biomeFoliageColors};
	generator.generateChunkInto(genX, genZ, target);

	// Compact occupancy metadata (issue #105): one canonical pass over the
	// freshly written voxels fills the 16 per-section counters.
	recountOccupancy();

	state = ChunkState::GENERATED;
	// Full content replacement invalidates any build result stamped with the
	// previous revision (issue #114 review).
	m_revision.fetch_add(1, std::memory_order_relaxed);
	return true;
}

void WorldChunk::reset(const glm::vec3 &newPosition, ResetMode mode)
{
	MemoryPublication memoryPublication{*this};

	// Reset identity.
	position = newPosition;
	state.store(ChunkState::UNLOADED);
	m_inTransit.store(false);
	m_replicaRetiring.store(false, std::memory_order_relaxed);
	m_terrainPublished = false;

	// A recycled record is a new identity (issue #104/#114): in-flight results
	// for the old incarnation are rejected at publish time via both counters.
	++m_generation;
	m_revision.fetch_add(1, std::memory_order_relaxed);

	// Full reset returns voxel storage to the pool for retirement.
	// ForGeneration retains dirty storage until generateTerrain() overwrites it.
	if (mode == ResetMode::Full)
		releaseVoxelStorageOnRetire();

	// Persistent-edit bookkeeping (issue #180): the edit map never leaks
	// across recycled records — capturing it is the owner's job before the
	// record is released. ForGeneration keeps tracking armed (the world
	// re-acquires the same record while a world is open); the Full reset
	// (retirement) disarms tracking entirely.
	m_persistentEdits.clear();
	if (mode == ResetMode::Full)
		m_trackPersistentEdits = false;

	// Empty occupancy metadata for the recycled incarnation; generation
	// recounts it (issue #105).
	m_sectionNonAir.fill(0);

	// Return transient neighbor-border storage to the BorderPool.
	releaseNeighborBorders();

	// Reset biome colors and per-column generation state.
	biomeGrassColors.fill(0);
	biomeFoliageColors.fill(0);
	biomeTypes.fill(static_cast<BiomeType>(0));
	heightMap.fill(0);
}

void WorldChunk::publishCpuTelemetry()
{
	if (!telemetry::registry().enabled)
		return;
	// Same slots the pre-split Chunk published (issue #206: the authoritative
	// backing moved here; mesh bytes stay render-side and are published by the
	// engine from MeshResultPoolStats).
	const std::array<uint64_t, 13> current{
		m_storage ? sizeof(VoxelStorage) : 0,
		m_borders ? sizeof(ChunkNeighborBorders) : 0,
		m_borders ? sizeof(ChunkNeighborBorders) : 0,
		0, 0, 0, 0, 0, 0, 0, 0,
		sizeof(biomeGrassColors) + sizeof(biomeFoliageColors) + sizeof(biomeTypes) + sizeof(heightMap),
		sizeof(m_sectionNonAir)};
	telemetry::registry().replaceCpu(m_cpuTelemetry, current);
	m_cpuTelemetry = current;
}
