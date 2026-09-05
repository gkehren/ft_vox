// Greedy mesher face-key contract (issue #106): the greedy merge phase must
// keep the exact merge/non-merge semantics of the rule-based mesher while
// classifying every slice face once. These deterministic scenes pin the
// contract (block identity, transparency pairs, water, biome tint, chunk
// borders, AO) and force the key path to stay byte-identical to a
// full-range build. CLI dev tools (not run by ctest):
//   test_mesh_facekey --hash-corpus [chunks] [stride]   per-chunk mesh hashes
//   test_mesh_facekey --bench-corpus [chunks] [stride]  median/p95 + stage timers
#include <Chunk/Chunk.hpp>
#include <Chunk/ChunkMeshResult.hpp>
#include <Chunk/TerrainGenerator.hpp>
#include <Engine/WorkloadTelemetry.hpp>

#include <algorithm>
#include <functional>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

static int g_fails = 0;

#define CHECK(cond, msg)                                                       \
	do                                                                         \
	{                                                                          \
		if (!(cond))                                                           \
		{                                                                      \
			std::cerr << "FAIL: " << msg << " (" << __LINE__ << ")\n";         \
			++g_fails;                                                         \
		}                                                                      \
	} while (0)

static_assert(sizeof(Vertex) == 28, "Vertex must be padding-free for byte hashing");

// Friend probe declared in Chunk.hpp: private state and ranged build entry
// points for the equivalence checks.
struct ChunkStateProbe
{
	static std::array<uint32_t, CHUNK_SIZE * CHUNK_SIZE> &grassMut(Chunk &c)
	{
		return c.biomeGrassColors;
	}
	static std::array<uint32_t, CHUNK_SIZE * CHUNK_SIZE> &foliageMut(Chunk &c)
	{
		return c.biomeFoliageColors;
	}
	static const std::array<BiomeType, CHUNK_SIZE * CHUNK_SIZE> &biomes(const Chunk &c)
	{
		return c.biomeTypes;
	}
	static void buildMeshRanged(Chunk &c, MeshBuildResult &out, uint64_t generation,
								uint64_t revision, int minY, int maxY)
	{
		c.buildMeshRanged(out, generation, revision, minY, maxY);
	}
	static void buildLODMeshRanged(Chunk &c, MeshBuildResult &out, int scanTopY)
	{
		c.buildLODMeshRanged(out, scanTopY);
	}
	static MeshBuildResult *pendingResult(const Chunk &c) { return c.m_pendingResult; }
};

// ---------------------------------------------------------------------------
// Quad decoding: greedy quads never share vertices (4 verts + 6 indices each,
// pushed sequentially), so quad i is vertices[4i..4i+3].
// ---------------------------------------------------------------------------

struct QuadView
{
	glm::ivec3 mn{0}; // local min corner (relative to chunk origin)
	glm::ivec3 mx{0}; // local max corner
	uint32_t normalIdx{0};
	uint32_t texture{0};
	bool biomeColor{false};
	uint32_t packedColor{0};
	int section{0};       // payload section this quad lives in
	uint32_t firstVertex{0}; // quad's first vertex index within that section
};

static uint32_t vNormal(const Vertex &v) { return v.packedData & 0x7; }
static uint32_t vTexture(const Vertex &v) { return (v.packedData >> 3) & 0xFF; }
static bool vBiome(const Vertex &v) { return (v.packedData >> 11) & 1; }
static uint32_t vAo(const Vertex &v) { return (v.packedData >> 12) & 0x3; }

static glm::ivec3 localPos(const Vertex &v, const Chunk &chunk)
{
	const glm::vec3 d = v.position - chunk.getPosition();
	return {static_cast<int>(std::lround(d.x)), static_cast<int>(std::lround(d.y)),
			static_cast<int>(std::lround(d.z))};
}

static std::vector<QuadView> decodeQuads(const std::vector<Vertex> &verts,
										 const std::vector<uint32_t> &indices,
										 const Chunk &chunk, int section)
{
	std::vector<QuadView> quads;
	if (verts.size() % 4 != 0 || indices.size() != verts.size() / 4 * 6)
		return quads; // malformed: caller's count checks will fire
	quads.reserve(verts.size() / 4);
	for (size_t q = 0; q < verts.size() / 4; ++q)
	{
		QuadView quad;
		quad.section = section;
		quad.firstVertex = static_cast<uint32_t>(q * 4);
		quad.mn = glm::ivec3(std::numeric_limits<int>::max());
		quad.mx = glm::ivec3(std::numeric_limits<int>::lowest());
		for (int i = 0; i < 4; ++i)
		{
			const Vertex &v = verts[q * 4 + static_cast<size_t>(i)];
			const glm::ivec3 p = localPos(v, chunk);
			quad.mn = glm::min(quad.mn, p);
			quad.mx = glm::max(quad.mx, p);
		}
		const Vertex &v0 = verts[q * 4];
		quad.normalIdx = vNormal(v0);
		quad.texture = vTexture(v0);
		quad.biomeColor = vBiome(v0);
		quad.packedColor = v0.packedBiomeColor;
		quads.push_back(quad);
	}
	return quads;
}

static const Vertex &sectionVertex(const MeshBuildResult &r, const QuadView &quad, int i)
{
	return r.sections[static_cast<size_t>(quad.section)]
		.opaqueVertices[static_cast<size_t>(quad.firstVertex) + static_cast<size_t>(i)];
}

static size_t totalOpaqueIndices(const MeshBuildResult &r)
{
	size_t n = 0;
	for (const SectionMeshPayload &s : r.sections)
		n += s.opaqueIndices.size();
	return n;
}
static size_t totalOpaqueVertices(const MeshBuildResult &r)
{
	size_t n = 0;
	for (const SectionMeshPayload &s : r.sections)
		n += s.opaqueVertices.size();
	return n;
}
static size_t totalWaterIndices(const MeshBuildResult &r)
{
	size_t n = 0;
	for (const SectionMeshPayload &s : r.sections)
		n += s.waterIndices.size();
	return n;
}
static size_t totalWaterVertices(const MeshBuildResult &r)
{
	size_t n = 0;
	for (const SectionMeshPayload &s : r.sections)
		n += s.waterVertices.size();
	return n;
}

static size_t countQuads(const std::vector<QuadView> &quads, uint32_t normalIdx,
						 uint32_t texture)
{
	size_t n = 0;
	for (const QuadView &q : quads)
		if (q.normalIdx == normalIdx && q.texture == texture)
			++n;
	return n;
}

static const QuadView *findQuad(const std::vector<QuadView> &quads, uint32_t normalIdx,
								uint32_t texture, glm::ivec3 at)
{
	for (const QuadView &q : quads)
		if (q.normalIdx == normalIdx && q.texture == texture && q.mn == at)
			return &q;
	return nullptr;
}

// ---------------------------------------------------------------------------
// Scene plumbing
// ---------------------------------------------------------------------------

struct BuiltMesh
{
	MeshResultPool *pool{nullptr};
	MeshBuildResult *result{nullptr};

	std::vector<QuadView> opaque;
	std::vector<QuadView> water;

	// Quads compose seamlessly across sections: positions are chunk-space
	// and each section's payload holds whole quads (4 verts + 6 indices).
	void decode(const Chunk &chunk)
	{
		opaque.clear();
		water.clear();
		for (int s = 0; s < kChunkSectionCount; ++s)
		{
			const SectionMeshPayload &p = result->sections[static_cast<size_t>(s)];
			std::vector<QuadView> q = decodeQuads(p.opaqueVertices, p.opaqueIndices, chunk, s);
			opaque.insert(opaque.end(), q.begin(), q.end());
			q = decodeQuads(p.waterVertices, p.waterIndices, chunk, s);
			water.insert(water.end(), q.begin(), q.end());
		}
	}
	void release()
	{
		if (pool && result)
			pool->release(result);
		pool = nullptr;
		result = nullptr;
	}
};

// Direct worker-path build into a pooled block (no publish): the caller owns
// the block and releases it via BuiltMesh::release().
static BuiltMesh buildWithMetadataBounds(Chunk &chunk, MeshResultPool &pool)
{
	BuiltMesh m;
	m.pool = &pool;
	m.result = pool.acquire();
	chunk.buildMesh(*m.result, chunk.meshGeneration(), chunk.meshRevision());
	pool.finishBuild(m.result);
	m.decode(chunk);
	return m;
}

static BuiltMesh buildForcedFullRange(Chunk &chunk, MeshResultPool &pool)
{
	BuiltMesh m;
	m.pool = &pool;
	m.result = pool.acquire();
	m.result->beginBuild(&chunk, chunk.meshGeneration(), chunk.meshRevision());
	m.result->isLOD = false;
	ChunkStateProbe::buildMeshRanged(chunk, *m.result, chunk.meshGeneration(),
									 chunk.meshRevision(), 0, CHUNK_HEIGHT - 1);
	pool.finishBuild(m.result);
	m.decode(chunk);
	return m;
}

static BuiltMesh buildLodWithMetadata(Chunk &chunk, MeshResultPool &pool)
{
	BuiltMesh m;
	m.pool = &pool;
	m.result = pool.acquire();
	chunk.buildLODMesh(*m.result, chunk.meshGeneration(), chunk.meshRevision());
	pool.finishBuild(m.result);
	m.decode(chunk);
	return m;
}

static void expectIdentical(const BuiltMesh &a, const BuiltMesh &b, const char *what)
{
	CHECK(a.result->sections == b.result->sections &&
			  a.result->opaqueVertices == b.result->opaqueVertices &&
			  a.result->waterVertices == b.result->waterVertices,
			  what);
}

// Synthetic chunk: storage is created lazily by setVoxel (air-filled), so
// scenes are fully deterministic without invoking the terrain generator.
struct Scene
{
	MeshResultPool pool;
	Chunk chunk;

	explicit Scene(const glm::vec3 &origin = glm::vec3(0.0f)) : chunk(origin, ChunkState::UNLOADED, nullptr, nullptr, &pool) {}
};

static void fillSlab(Chunk &chunk, int x0, int z0, int w, int d, int y, TextureType type)
{
	for (int x = x0; x < x0 + w; ++x)
		for (int z = z0; z < z0 + d; ++z)
			chunk.setVoxel(x, y, z, type);
}

// ---------------------------------------------------------------------------

static void testUniformSlabMerges()
{
	// A 4x4x1 slab in open air must merge into exactly 6 quads (top, bottom,
	// and four 4x1 sides); every AO corner is unoccluded.
	Scene s;
	fillSlab(s.chunk, 0, 0, 4, 4, 8, STONE);

	BuiltMesh m = buildWithMetadataBounds(s.chunk, s.pool);
	CHECK(totalOpaqueVertices(*m.result) == 24, "slab: 6 quads x 4 vertices");
	CHECK(totalOpaqueIndices(*m.result) == 36, "slab: 6 quads x 6 indices");
	CHECK(totalWaterVertices(*m.result) == 0 && totalWaterIndices(*m.result) == 0,
		  "slab: no water geometry");

	const QuadView *top = findQuad(m.opaque, 2, STONE, glm::ivec3(0, 9, 0));
	CHECK(top != nullptr, "slab: one +Y top quad exists");
	if (top)
	{
		CHECK(top->mx == glm::ivec3(4, 9, 4), "slab: top quad merged to 4x4");
		// Corner AO is sampled at the final quad corners after merging. The
		// current sampler counts air as an occluder (isSolid = !isTransparent
		// and AIR is not in the layer table), so a uniformly exposed quad
		// shows one corner value across all four vertices. Byte-exact AO is
		// pinned by the hash corpus and the key-vs-full-range equivalence.
		CHECK(vAo(sectionVertex(*m.result, *top, 0)) == vAo(sectionVertex(*m.result, *top, 1)) &&
				  vAo(sectionVertex(*m.result, *top, 0)) == vAo(sectionVertex(*m.result, *top, 2)) &&
				  vAo(sectionVertex(*m.result, *top, 0)) == vAo(sectionVertex(*m.result, *top, 3)),
			  "slab: uniform exposure keeps uniform corner AO");
	}
	CHECK(findQuad(m.opaque, 3, STONE, glm::ivec3(0, 8, 0)) != nullptr &&
			  findQuad(m.opaque, 3, STONE, glm::ivec3(0, 8, 0))->mx == glm::ivec3(4, 8, 4),
		  "slab: one 4x4 -Y bottom quad");
	CHECK(countQuads(m.opaque, 0, STONE) == 1 && countQuads(m.opaque, 1, STONE) == 1,
		  "slab: each X side is a single merged quad");
	CHECK(countQuads(m.opaque, 4, STONE) == 1 && countQuads(m.opaque, 5, STONE) == 1,
		  "slab: each Z side is a single merged quad");

	m.release();
}

static void testBlockTypeBoundary()
{
	// Two half-slabs of different opaque types: no quad may cross the type
	// boundary, on any face orientation.
	Scene s;
	fillSlab(s.chunk, 0, 0, 2, 4, 8, STONE);
	fillSlab(s.chunk, 2, 0, 2, 4, 8, BRICKS);

	BuiltMesh m = buildWithMetadataBounds(s.chunk, s.pool);
	const QuadView *topStone = findQuad(m.opaque, 2, STONE, glm::ivec3(0, 9, 0));
	const QuadView *topBricks = findQuad(m.opaque, 2, BRICKS, glm::ivec3(2, 9, 0));
	CHECK(topStone != nullptr && topStone->mx == glm::ivec3(2, 9, 4),
		  "type split: stone top quad stays 2x4");
	CHECK(topBricks != nullptr && topBricks->mx == glm::ivec3(4, 9, 4),
		  "type split: bricks top quad stays 2x4");
	CHECK(countQuads(m.opaque, 2, STONE) == 1 && countQuads(m.opaque, 2, BRICKS) == 1,
		  "type split: exactly two top quads");
	// The ±Z sides each split per type; ±X sides stay whole.
	CHECK(countQuads(m.opaque, 4, STONE) == 1 && countQuads(m.opaque, 4, BRICKS) == 1,
		  "type split: -Z side splits per type");
	CHECK(countQuads(m.opaque, 5, STONE) == 1 && countQuads(m.opaque, 5, BRICKS) == 1,
		  "type split: +Z side splits per type");
	CHECK(countQuads(m.opaque, 0, BRICKS) == 1 && countQuads(m.opaque, 1, STONE) == 1,
		  "type split: outer X sides stay whole");
	for (const QuadView &q : m.opaque)
		CHECK(!q.biomeColor && q.packedColor == 0,
			  "type split: plain blocks carry no biome tint");
	m.release();
}

static void testTransparencyPairs()
{
	{
		// Opaque | transparent: exactly one face between the pair and it
		// belongs to the opaque block. Same opaque pair: no face. Same
		// transparent pair: no face either.
		Scene s;
		s.chunk.setVoxel(3, 8, 0, STONE);
		s.chunk.setVoxel(4, 8, 0, GLASS);
		BuiltMesh m = buildWithMetadataBounds(s.chunk, s.pool);
		CHECK(findQuad(m.opaque, 0, STONE, glm::ivec3(4, 8, 0)) != nullptr,
			  "opaque|glass: stone +X face exists against glass");
		CHECK(findQuad(m.opaque, 1, GLASS, glm::ivec3(4, 8, 0)) == nullptr,
			  "opaque|glass: glass owns no -X face against stone");
		m.release();
	}
	{
		Scene s;
		s.chunk.setVoxel(3, 8, 0, STONE);
		s.chunk.setVoxel(4, 8, 0, STONE);
		BuiltMesh m = buildWithMetadataBounds(s.chunk, s.pool);
		CHECK(findQuad(m.opaque, 0, STONE, glm::ivec3(4, 8, 0)) == nullptr,
			  "stone|stone: no internal face");
		m.release();
	}
	{
		Scene s;
		s.chunk.setVoxel(3, 8, 0, GLASS);
		s.chunk.setVoxel(4, 8, 0, GLASS);
		BuiltMesh m = buildWithMetadataBounds(s.chunk, s.pool);
		CHECK(findQuad(m.opaque, 0, GLASS, glm::ivec3(4, 8, 0)) == nullptr &&
				  findQuad(m.opaque, 1, GLASS, glm::ivec3(4, 8, 0)) == nullptr,
			  "glass|glass: no internal face between same transparent type");
		// Air-facing quads of the same transparent type still merge.
		const QuadView *top = findQuad(m.opaque, 2, GLASS, glm::ivec3(3, 9, 0));
		CHECK(top != nullptr && top->mx == glm::ivec3(5, 9, 1),
			  "glass|glass: top faces merge into one 2x1 quad");
		m.release();
	}
	{
		// Transparent | opaque: the opaque side owns the shared face.
		Scene s;
		s.chunk.setVoxel(3, 8, 0, GLASS);
		s.chunk.setVoxel(4, 8, 0, STONE);
		BuiltMesh m = buildWithMetadataBounds(s.chunk, s.pool);
		CHECK(findQuad(m.opaque, 1, STONE, glm::ivec3(4, 8, 0)) != nullptr,
			  "glass|stone: stone -X face exists against glass");
		CHECK(findQuad(m.opaque, 0, GLASS, glm::ivec3(4, 8, 0)) == nullptr,
			  "glass|stone: glass owns no +X face against stone");
		m.release();
	}
}

static void testWater()
{
	{
		// Floating 4x4x2 water box in air: every quad is water; top/bottom
		// merge to 4x4, sides to 4x2; tint is the constant water color.
		Scene s;
		for (int y = 8; y < 10; ++y)
			fillSlab(s.chunk, 0, 0, 4, 4, y, WATER);

		BuiltMesh m = buildWithMetadataBounds(s.chunk, s.pool);
		CHECK(totalOpaqueVertices(*m.result) == 0 && totalOpaqueIndices(*m.result) == 0,
			  "water box: no opaque geometry");
		CHECK(totalWaterVertices(*m.result) == 24 && totalWaterIndices(*m.result) == 36,
			  "water box: 6 quads (top, bottom, four 4x2 sides)");
		const QuadView *top = findQuad(m.water, 2, WATER, glm::ivec3(0, 10, 0));
		CHECK(top != nullptr && top->mx == glm::ivec3(4, 10, 4),
			  "water box: top face merged 4x4");
		for (const SectionMeshPayload &p : m.result->sections)
			for (const Vertex &v : p.waterVertices)
			{
				CHECK(vBiome(v) && v.packedBiomeColor == 0xFFE6804Du,
					  "water box: constant water tint on every vertex");
			}
		m.release();
	}
	{
		// Water over stone: the stone top face stays visible through the
		// water (opaque buffer), and the water emits no face against stone.
		Scene s;
		fillSlab(s.chunk, 0, 0, 4, 4, 7, STONE);
		fillSlab(s.chunk, 0, 0, 4, 4, 8, WATER);

		BuiltMesh m = buildWithMetadataBounds(s.chunk, s.pool);
		const QuadView *stoneTop = findQuad(m.opaque, 2, STONE, glm::ivec3(0, 8, 0));
		CHECK(stoneTop != nullptr && stoneTop->mx == glm::ivec3(4, 8, 4),
			  "water|stone: stone +Y face exists under water");
		CHECK(m.opaque.size() == 6,
			  "water|stone: slab keeps all six faces (stone visible through water)");
		CHECK(findQuad(m.water, 3, WATER, glm::ivec3(0, 8, 0)) == nullptr,
			  "water|stone: water emits no -Y face against stone");
		const QuadView *waterTop = findQuad(m.water, 2, WATER, glm::ivec3(0, 9, 0));
		CHECK(waterTop != nullptr && waterTop->mx == glm::ivec3(4, 9, 4),
			  "water|stone: water top face still merged 4x4");
		CHECK(m.water.size() == 5, "water|stone: water keeps top + four sides only");
		m.release();
	}
}

static void testBiomeTintBoundary()
{
	{
		// A 4x1 grass run whose columns carry two grass colors must split
		// exactly at the color boundary; vertices carry their column color.
		Scene s;
		const uint32_t c1 = 0xFF5D9C42u;
		const uint32_t c2 = 0xFF7FB069u;
		auto &grass = ChunkStateProbe::grassMut(s.chunk);
		for (int x = 0; x < 4; ++x)
		{
			s.chunk.setVoxel(x, 8, 0, GRASS_TOP);
			grass[0 * CHUNK_SIZE + x] = (x < 2) ? c1 : c2;
		}

		BuiltMesh m = buildWithMetadataBounds(s.chunk, s.pool);
		CHECK(countQuads(m.opaque, 2, GRASS_TOP) == 2,
			  "tint: grass top face splits on color change");
		const QuadView *left = findQuad(m.opaque, 2, GRASS_TOP, glm::ivec3(0, 9, 0));
		const QuadView *right = findQuad(m.opaque, 2, GRASS_TOP, glm::ivec3(2, 9, 0));
		CHECK(left != nullptr && left->mx == glm::ivec3(2, 9, 1) && left->packedColor == c1,
			  "tint: left quad keeps column color 1");
		CHECK(right != nullptr && right->mx == glm::ivec3(4, 9, 1) && right->packedColor == c2,
			  "tint: right quad keeps column color 2");
		m.release();
	}
	{
		// Uniform colors merge back into a single quad.
		Scene s;
		auto &grass = ChunkStateProbe::grassMut(s.chunk);
		for (int x = 0; x < 4; ++x)
		{
			s.chunk.setVoxel(x, 8, 0, GRASS_TOP);
			grass[0 * CHUNK_SIZE + x] = 0xFF5D9C42u;
		}
		BuiltMesh m = buildWithMetadataBounds(s.chunk, s.pool);
		const QuadView *top = findQuad(m.opaque, 2, GRASS_TOP, glm::ivec3(0, 9, 0));
		CHECK(top != nullptr && top->mx == glm::ivec3(4, 9, 1),
			  "tint: equal colors merge the top face into one quad");
		m.release();
	}
	{
		// Foliage splits on the foliage color, not the grass color.
		Scene s;
		auto &grass = ChunkStateProbe::grassMut(s.chunk);
		auto &foliage = ChunkStateProbe::foliageMut(s.chunk);
		const uint32_t f1 = 0xFF3A7D2Cu;
		const uint32_t f2 = 0xFF62A83Bu;
		for (int x = 0; x < 2; ++x)
		{
			s.chunk.setVoxel(x, 8, 0, OAK_LEAVES);
			grass[0 * CHUNK_SIZE + x] = 0xFFFFFFFFu; // must be ignored
			foliage[0 * CHUNK_SIZE + x] = (x == 0) ? f1 : f2;
		}
		BuiltMesh m = buildWithMetadataBounds(s.chunk, s.pool);
		CHECK(countQuads(m.opaque, 2, OAK_LEAVES) == 2,
			  "tint: foliage top face splits on foliage color");
		const QuadView *first = findQuad(m.opaque, 2, OAK_LEAVES, glm::ivec3(0, 9, 0));
		CHECK(first != nullptr && first->packedColor == f1,
			  "tint: foliage quad carries foliage color");
		m.release();
	}
}

static void testChunkBorders()
{
	{
		// A solid neighbor column occludes the in-chunk face; a missing
		// (AIR) border exposes it; a transparent border does not occlude.
		Scene s;
		s.chunk.setVoxel(0, 8, 0, STONE);
		s.chunk.setVoxel(0, 9, 0, STONE);

		BuiltMesh open = buildWithMetadataBounds(s.chunk, s.pool);
		CHECK(findQuad(open.opaque, 1, STONE, glm::ivec3(0, 8, 0)) != nullptr,
			  "borders: -X face exists against AIR border");
		open.release();

		// West border column (x = -1) filled with the same opaque type.
		s.chunk.setVoxel(-1, 8, 0, STONE);
		s.chunk.setVoxel(-1, 9, 0, STONE);
		BuiltMesh occluded = buildWithMetadataBounds(s.chunk, s.pool);
		CHECK(findQuad(occluded.opaque, 1, STONE, glm::ivec3(0, 8, 0)) == nullptr,
			  "borders: solid neighbor occludes the -X face");
		occluded.release();

		// Transparent neighbor: the face must come back.
		s.chunk.setVoxel(-1, 8, 0, GLASS);
		s.chunk.setVoxel(-1, 9, 0, GLASS);
		BuiltMesh glass = buildWithMetadataBounds(s.chunk, s.pool);
		CHECK(findQuad(glass.opaque, 1, STONE, glm::ivec3(0, 8, 0)) != nullptr,
			  "borders: transparent neighbor keeps the -X face visible");
		glass.release();
	}
	{
		// Border voxels never own faces: border columns against in-chunk
		// air produce no geometry here (the neighbor chunk renders them).
		// An in-chunk block keeps the occupied span non-empty so the mesher
		// actually walks the border slices.
		Scene s;
		s.chunk.setVoxel(8, 8, 8, STONE);
		s.chunk.setVoxel(-1, 8, 0, STONE);
		s.chunk.setVoxel(CHUNK_SIZE, 8, 5, STONE);
		s.chunk.setVoxel(4, 8, -1, STONE);
		s.chunk.setVoxel(4, 8, CHUNK_SIZE, STONE);

		BuiltMesh m = buildWithMetadataBounds(s.chunk, s.pool);
		CHECK(m.opaque.size() == 6 && m.water.empty(),
			  "borders: only the in-chunk block's six faces exist");
		m.release();
	}
}

static void testOccupiedSpanEdges()
{
	// Content hugging the world floor and ceiling must not lose boundary
	// faces under the metadata-driven span.
	Scene s;
	fillSlab(s.chunk, 0, 0, 4, 4, 0, STONE);
	s.chunk.setVoxel(8, CHUNK_HEIGHT - 1, 8, BRICKS);
	s.chunk.setVoxel(8, CHUNK_HEIGHT - 2, 8, BRICKS);

	BuiltMesh m = buildWithMetadataBounds(s.chunk, s.pool);
	CHECK(findQuad(m.opaque, 2, STONE, glm::ivec3(0, 1, 0)) != nullptr,
		  "span edges: floor slab keeps its top face");
	CHECK(findQuad(m.opaque, 3, STONE, glm::ivec3(0, 0, 0)) != nullptr,
		  "span edges: floor slab keeps its bottom face");
	CHECK(findQuad(m.opaque, 2, BRICKS, glm::ivec3(8, CHUNK_HEIGHT, 8)) != nullptr,
		  "span edges: ceiling block keeps its top face");
	CHECK(findQuad(m.opaque, 3, BRICKS, glm::ivec3(8, CHUNK_HEIGHT - 2, 8)) != nullptr,
		  "span edges: ceiling column keeps its bottom face");
	m.release();
}

static void testAoCornerSampling()
{
	// Pin the AO corner-sampling contract across the greedy merge: corners
	// are sampled at the final quad corners, and a transparent neighbor does
	// not occlude (isSolid = !isTransparent), while air/opaque do. Values
	// are not hardcoded; the byte-exact behavior is pinned by the hash
	// corpus and the key-vs-full-range equivalence checks.
	MeshResultPool pool;
	auto build = [&](bool withWater, uint32_t &waterCornerAo, uint32_t &farCornerAo)
	{
		Scene s;
		fillSlab(s.chunk, 0, 0, 4, 4, 7, STONE);
		if (withWater)
			s.chunk.setVoxel(0, 8, 0, WATER); // sits on the slab, in front of one corner
		BuiltMesh m = buildWithMetadataBounds(s.chunk, s.pool);
		const QuadView *top = findQuad(m.opaque, 2, STONE, glm::ivec3(0, 8, 0));
		CHECK(top != nullptr && top->mx == glm::ivec3(4, 8, 4),
			  "ao: stone top face still merged under the water cell");
		waterCornerAo = vAo(sectionVertex(*m.result, *top, 0)); // corner at (0,8,0)
		farCornerAo = vAo(sectionVertex(*m.result, *top, 2));   // far corner (4,8,4)
		m.release();
	};
	uint32_t plainNear = 0, plainFar = 0, waterNear = 0, waterFar = 0;
	build(false, plainNear, plainFar);
	CHECK(plainNear == plainFar,
		  "ao: without a transparent neighbor all corners agree");
	build(true, waterNear, waterFar);
	CHECK(waterNear > waterFar,
		  "ao: transparent neighbor brightens the corner it touches");
}

// ---------------------------------------------------------------------------
// Section seam ownership (PR #117 review phases 13-15, 39): an interface
// between a y=15 and a y=16 voxel must classify exactly like the historical
// whole-chunk mesher no matter which section's pass visits the pair - one
// face, owned by the first-match block, emitted by its owner's section.
// ---------------------------------------------------------------------------

struct LegacyFace
{
	bool hasFace{false};
	bool negSide{false};    // face owned by the upper block, pointing -q
	TextureType owner{AIR}; // owning block type
};

static void testKeyPathMatchesFullRange(const char *label, Chunk &chunk,
										MeshResultPool &pool);

// The historical (#106) whole-chunk rules for one interface: at most one
// face per cell pair - the lower block's +q face when it is visible against
// the upper block, otherwise the upper block's -q face when visible against
// the lower block. Written independently of the Chunk classifier on
// purpose: it is the oracle the seam tests are judged against, so an error
// shared by the sectioned and full paths cannot hide it.
static LegacyFace classifyLegacyInterface(TextureType below, TextureType above)
{
	auto visible = [](TextureType t, TextureType backing)
	{
		return backing == AIR ||
			   (TextureManager::isTransparent(backing) && backing != t);
	};
	LegacyFace f;
	if (below != AIR && visible(below, above))
	{
		f.hasFace = true;
		f.owner = below;
	}
	else if (above != AIR && visible(above, below))
	{
		f.hasFace = true;
		f.negSide = true;
		f.owner = above;
	}
	return f;
}

static void checkSeamInterface(const char *label, TextureType below,
							   TextureType above, int x0, int z0, int w, int d_)
{
	Scene s;
	for (int x = x0; x < x0 + w; ++x)
		for (int z = z0; z < z0 + d_; ++z)
		{
			s.chunk.setVoxel(x, 15, z, below);
			s.chunk.setVoxel(x, 16, z, above);
		}
	BuiltMesh m = buildWithMetadataBounds(s.chunk, s.pool);

	const LegacyFace expected = classifyLegacyInterface(below, above);
	size_t plane16 = 0, expectedFaces = 0, spurious = 0;
	auto inspect = [&](const std::vector<QuadView> &quads, bool waterStream)
	{
		for (const QuadView &q : quads)
		{
			// Horizontal faces only (side quads span y=15..16 or 16..17).
			if (q.mn.y != 16 || q.mx.y != 16)
				continue;
			++plane16;
			const bool plusY = q.normalIdx == 2;
			const bool rightStream = waterStream == (expected.owner == WATER);
			if (expected.hasFace &&
				((expected.negSide && !plusY) || (!expected.negSide && plusY)) &&
				q.texture == static_cast<uint32_t>(expected.owner) && rightStream &&
				q.section == (expected.negSide ? 1 : 0))
				++expectedFaces;
			else
				++spurious;
		}
	};
	inspect(m.opaque, false);
	inspect(m.water, true);

	CHECK(plane16 == (expected.hasFace ? 1 : 0) && expectedFaces == plane16 &&
			  spurious == 0,
		  label);
	m.release();
}

static void testSectionSeamOwnership()
{
	// The pair is visited by section 0's pass (slice 15) AND by section 1's
	// pass (the same slice, as its ySliceMin). The pre-review bug: section
	// 1's pass gated the first match away and classified a second, spurious
	// face owned by the upper block. 2x2 columns merge each interface face
	// into one quad, so any extra face or misplacement shows up as a count.
	struct Pair
	{
		const char *label;
		TextureType below;
		TextureType above;
	};
	const Pair pairs[] = {
		{"seam stone/water: stone top face only", STONE, WATER},
		{"seam water/stone: water top face only", WATER, STONE},
		{"seam glass/water: glass top face only", GLASS, WATER},
		{"seam water/glass: water top face only", WATER, GLASS},
		{"seam leaves/water: leaves top face only", OAK_LEAVES, WATER},
		{"seam water/leaves: water top face only", WATER, OAK_LEAVES},
		{"seam glass/leaves: glass top face only", GLASS, OAK_LEAVES},
		{"seam leaves/glass: leaves top face only", OAK_LEAVES, GLASS},
		{"seam stone/glass: stone top face only", STONE, GLASS},
		{"seam glass/stone: glass top face only", GLASS, STONE},
		{"seam water/water: no interface face", WATER, WATER},
		{"seam glass/glass: no interface face", GLASS, GLASS},
		{"seam stone/stone: no interface face", STONE, STONE},
	};
	for (const Pair &p : pairs)
		checkSeamInterface(p.label, p.below, p.above, 0, 0, 2, 2);

	// negSide emission at the seam: water floating ON the boundary plane
	// (air below at y=15, water at y=16) owns its bottom face at y=16 -
	// emitted by section 1's pass, which visits the y=15 slice only for
	// faces owned in section 1.
	checkSeamInterface("seam air/water: water bottom face owned at y=16",
					   AIR, WATER, 0, 0, 2, 2);

	// Merge-across-seam scenes (4x4): the interface face merges into ONE
	// quad placed in the owner's section, and no second face appears.
	checkSeamInterface("seam 4x4 stone/water: one merged stone top quad",
					   STONE, WATER, 0, 0, 4, 4);
	checkSeamInterface("seam 4x4 air/water: one merged water bottom quad",
					   AIR, WATER, 0, 0, 4, 4);

	// The seam scenes must also compose: a partial build of the sections the
	// owner lives in reproduces the full build byte for byte.
	{
		Scene s;
		for (int x = 0; x < 4; ++x)
			for (int z = 0; z < 4; ++z)
			{
				s.chunk.setVoxel(x, 15, z, WATER); // section 0 (y=15)
				s.chunk.setVoxel(x, 16, z, GLASS); // section 1 (y=16)
			}
		BuiltMesh meta = buildWithMetadataBounds(s.chunk, s.pool);
		CHECK(totalWaterIndices(*meta.result) > 0 &&
				  totalOpaqueIndices(*meta.result) > 0,
			  "seam composition scene: water and glass geometry present");
		testKeyPathMatchesFullRange("seam composition equivalence", s.chunk, s.pool);
		meta.release();
	}
}

static void testKeyPathMatchesFullRange(const char *label, Chunk &chunk,
										MeshResultPool &pool)
{
	// The key path runs on the metadata-derived span; a forced full-volume
	// build must produce byte-identical output (the clamped classification
	// rect is semantics-neutral).
	BuiltMesh meta = buildWithMetadataBounds(chunk, pool);
	BuiltMesh full = buildForcedFullRange(chunk, pool);
	expectIdentical(meta, full, "key path: metadata-bounds build matches forced full range");

	BuiltMesh lodMeta = buildLodWithMetadata(chunk, pool);
	CHECK(lodMeta.result != nullptr, label);
	meta.release();
	full.release();
	lodMeta.release();
}

static void testNoisyEditedChunk()
{
	// A dense mixed-type block pile with random edits: the strongest local
	// equivalence + determinism check (many quad boundaries in one chunk).
	MeshResultPool pool;
	TerrainGenerator tgen(4242);
	Chunk chunk(glm::vec3(0.0f), ChunkState::UNLOADED, nullptr, nullptr, &pool);
	CHECK(chunk.prepareVoxelStorageForGeneration(), "noisy: prepare");
	chunk.generateTerrain(tgen);

	uint32_t rng = 0x9E3779B9u;
	auto next = [&rng]()
	{ rng = rng * 1664525u + 1013904223u; return rng; };
	const TextureType palette[] = {STONE, BRICKS, GLASS, WATER, OAK_LEAVES, DIRT, COBBLESTONE, AIR};
	for (int i = 0; i < 1200; ++i)
	{
		const int x = static_cast<int>(next() % CHUNK_SIZE);
		const int z = static_cast<int>(next() % CHUNK_SIZE);
		const int y = static_cast<int>(next() % CHUNK_HEIGHT);
		chunk.setVoxel(x, y, z, palette[next() % std::size(palette)]);
	}

	BuiltMesh a = buildWithMetadataBounds(chunk, pool);
	BuiltMesh b = buildForcedFullRange(chunk, pool);
	expectIdentical(a, b, "noisy: metadata-bounds build matches forced full range");
	CHECK(totalOpaqueIndices(*a.result) > 0, "noisy: terrain emits geometry");

	// Rebuild determinism through the public path (publish + pending).
	CHECK(chunk.generateMesh(), "noisy: public mesh build publishes");
	const MeshBuildResult *published = ChunkStateProbe::pendingResult(chunk);
	CHECK(published != nullptr && published->sections == b.result->sections,
		  "noisy: public path reproduces the same payload");

	a.release();
	b.release();
}

static void testMultithreadedDeterminism()
{
	// The face-key workspace is thread-local scratch: concurrent builds of
	// the same chunk on different workers must agree byte for byte.
	MeshResultPool pool;
	TerrainGenerator tgen(9021);
	Chunk chunk(glm::vec3(0.0f), ChunkState::UNLOADED, nullptr, nullptr, &pool);
	CHECK(chunk.prepareVoxelStorageForGeneration(), "threads: prepare");
	chunk.generateTerrain(tgen);

	constexpr int kWorkers = 4;
	std::array<std::array<SectionMeshPayload, kChunkSectionCount>, kWorkers> sections{};
	{
		std::vector<MeshBuildResult *> results(kWorkers);
		for (int i = 0; i < kWorkers; ++i)
			results[i] = pool.acquire();
		std::vector<std::thread> workers;
		for (int i = 0; i < kWorkers; ++i)
		{
			workers.emplace_back([&chunk, &pool, i, &results]
			{
				chunk.buildMesh(*results[i], chunk.meshGeneration(), chunk.meshRevision());
				pool.finishBuild(results[i]);
			});
		}
		for (auto &w : workers)
			w.join();
		for (int i = 0; i < kWorkers; ++i)
		{
			sections[i] = results[i]->sections;
			pool.release(results[i]);
		}
	}
	for (int i = 1; i < kWorkers; ++i)
	{
		CHECK(sections[i] == sections[0],
			  "threads: worker builds are byte-identical");
	}
}

// ---------------------------------------------------------------------------
// Dev tools: hash corpus (before/after mesher changes) and bench corpus.
// ---------------------------------------------------------------------------

static uint64_t fnv1a(const void *data, size_t bytes, uint64_t h)
{
	const uint8_t *p = static_cast<const uint8_t *>(data);
	for (size_t i = 0; i < bytes; ++i)
	{
		h ^= p[i];
		h *= 1099511628211ull;
	}
	return h;
}

static uint64_t meshHash(const MeshBuildResult &r)
{
	uint64_t h = 1469598103934665603ull;
	for (const SectionMeshPayload &s : r.sections)
	{
		h = fnv1a(s.opaqueVertices.data(), s.opaqueVertices.size() * sizeof(Vertex), h);
		h = fnv1a(s.opaqueIndices.data(), s.opaqueIndices.size() * sizeof(uint32_t), h);
		h = fnv1a(s.waterVertices.data(), s.waterVertices.size() * sizeof(Vertex), h);
		h = fnv1a(s.waterIndices.data(), s.waterIndices.size() * sizeof(uint32_t), h);
	}
	// LOD payload (empty for full-quality builds).
	h = fnv1a(r.opaqueVertices.data(), r.opaqueVertices.size() * sizeof(Vertex), h);
	h = fnv1a(r.opaqueIndices.data(), r.opaqueIndices.size() * sizeof(uint32_t), h);
	h = fnv1a(r.waterVertices.data(), r.waterVertices.size() * sizeof(Vertex), h);
	h = fnv1a(r.waterIndices.data(), r.waterIndices.size() * sizeof(uint32_t), h);
	return h;
}

static size_t resultOpaqueVerts(const MeshBuildResult &r) { return totalOpaqueVertices(r); }
static size_t resultOpaqueIdx(const MeshBuildResult &r) { return totalOpaqueIndices(r); }
static size_t resultWaterVerts(const MeshBuildResult &r) { return totalWaterVertices(r); }
static size_t resultWaterIdx(const MeshBuildResult &r) { return totalWaterIndices(r); }

// One meshed chunk with a generated 4-neighborhood for border sampling.
struct CorpusChunk
{
	std::unique_ptr<Chunk> center;
	std::unique_ptr<Chunk> west, east, south, north;
};

static CorpusChunk makeCorpusChunk(TerrainGenerator &gen, int cx, int cz)
{
	CorpusChunk c;
	auto make = [&](int x, int z)
	{
		auto chunk = std::make_unique<Chunk>(glm::vec3(float(x * CHUNK_SIZE), 0.f, float(z * CHUNK_SIZE)));
		if (!chunk->prepareVoxelStorageForGeneration())
			std::cerr << "FAIL: corpus prepare\n";
		chunk->generateTerrain(gen);
		return chunk;
	};
	c.center = make(cx, cz);
	c.west = make(cx - 1, cz);
	c.east = make(cx + 1, cz);
	c.south = make(cx, cz - 1);
	c.north = make(cx, cz + 1);
	c.center->rebuildBordersFromNeighbors(c.west.get(), c.east.get(),
										  c.south.get(), c.north.get());
	return c;
}

static int runHashCorpus(int argc, char **argv)
{
	const int chunks = (argc > 2) ? std::max(1, std::atoi(argv[2])) : 12;
	const int stride = (argc > 3) ? std::max(2, std::atoi(argv[3])) : 97;
	constexpr int kSeeds[] = {1337, 4242, 9001};

	MeshResultPool pool;
	for (const int seed : kSeeds)
	{
		TerrainGenerator gen(seed);
		for (int i = 0; i < chunks; ++i)
		{
			const int gx = (i % 5) - 2;
			const int gz = ((i / 5) % 5) - 2;
			const int cx = gx * stride;
			const int cz = gz * stride;
			CorpusChunk cc = makeCorpusChunk(gen, cx, cz);

			cc.center->generateMesh();
			const MeshBuildResult *m = ChunkStateProbe::pendingResult(*cc.center);
			const uint64_t hash = m ? meshHash(*m) : 0;
			const size_t ov = m ? resultOpaqueVerts(*m) : 0, oi = m ? resultOpaqueIdx(*m) : 0;
			const size_t wv = m ? resultWaterVerts(*m) : 0, wi = m ? resultWaterIdx(*m) : 0;
			cc.center->generateLODMesh();
			const MeshBuildResult *lod = ChunkStateProbe::pendingResult(*cc.center);
			if (!m || !lod)
			{
				std::cerr << "FAIL: corpus mesh missing\n";
				++g_fails;
				continue;
			}
			// Publishing the LOD replaces the pending opaque result, so the
			// opaque payload was captured (sizes + hash) before that.
			std::cout << "HASH seed=" << seed << " chunk=(" << cx << "," << cz << ")"
					  << " opaque=" << ov << "/" << oi
					  << " water=" << wv << "/" << wi
					  << " lod=" << lod->opaqueVertices.size() << "/" << lod->opaqueIndices.size()
					  << " hash=0x" << std::hex << hash << std::dec
					  << " lodHash=0x" << std::hex << meshHash(*lod) << std::dec << "\n";
		}
	}

	// Edited/noisy corpus: terrain + random edits, one row per seed.
	for (const int seed : kSeeds)
	{
		TerrainGenerator gen(seed);
		CorpusChunk cc = makeCorpusChunk(gen, 0, 0);
		uint32_t rng = 0x9E3779B9u ^ static_cast<uint32_t>(seed);
		auto next = [&rng]
		{ rng = rng * 1664525u + 1013904223u; return rng; };
		const TextureType palette[] = {STONE, BRICKS, GLASS, WATER, OAK_LEAVES, DIRT, AIR};
		for (int i = 0; i < 600; ++i)
		{
			const int x = static_cast<int>(next() % CHUNK_SIZE);
			const int z = static_cast<int>(next() % CHUNK_SIZE);
			const int y = static_cast<int>(next() % CHUNK_HEIGHT);
			cc.center->setVoxel(x, y, z, palette[next() % std::size(palette)]);
		}
		cc.center->generateMesh();
		const MeshBuildResult *m = ChunkStateProbe::pendingResult(*cc.center);
		if (m)
			std::cout << "HASH seed=" << seed << " chunk=edited"
					  << " opaque=" << resultOpaqueVerts(*m) << "/" << resultOpaqueIdx(*m)
					  << " water=" << resultWaterVerts(*m) << "/" << resultWaterIdx(*m)
					  << " hash=0x" << std::hex << meshHash(*m) << std::dec << "\n";
	}
	return g_fails != 0 ? 1 : 0;
}

static double percentile(std::vector<double> &v, double p)
{
	if (v.empty())
		return 0.0;
	std::sort(v.begin(), v.end());
	const size_t idx = std::min(v.size() - 1,
								static_cast<size_t>(p * (v.size() - 1) + 0.5));
	return v[idx];
}

static int runBenchCorpus(int argc, char **argv)
{
	const int chunks = (argc > 2) ? std::max(1, std::atoi(argv[2])) : 12;
	const int stride = (argc > 3) ? std::max(2, std::atoi(argv[3])) : 97;
	constexpr int kSeeds[] = {1337, 4242, 9001};

	telemetry::registry().beginCapture();
	MeshResultPool pool;
	std::vector<double> meshMs;
	for (const int seed : kSeeds)
	{
		TerrainGenerator gen(seed);
		std::vector<double> perSeed;
		for (int i = 0; i < chunks; ++i)
		{
			const int gx = (i % 5) - 2;
			const int gz = ((i / 5) % 5) - 2;
			CorpusChunk cc = makeCorpusChunk(gen, (gx * stride), (gz * stride));

			MeshBuildResult *r = pool.acquire();
			const auto t0 = std::chrono::steady_clock::now();
			cc.center->buildMesh(*r, cc.center->meshGeneration(), cc.center->meshRevision());
			const auto t1 = std::chrono::steady_clock::now();
			pool.finishBuild(r);
			pool.release(r);
			const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
			perSeed.push_back(ms);
			meshMs.push_back(ms);
		}
		std::cout << "MESH seed=" << seed << " median=" << percentile(perSeed, 0.5)
				  << " ms p95=" << percentile(perSeed, 0.95) << " ms\n";
	}

	const telemetry::Snapshot snap = telemetry::registry().snapshot();
	std::cout << "STAGES";
	for (size_t s = 0; s < telemetry::StageCount; ++s)
	{
		const double calls = snap.stageCalls[s] ? static_cast<double>(snap.stageCalls[s]) : 1.0;
		std::cout << " " << telemetry::stageNames[s] << "="
				  << (static_cast<double>(snap.stageNs[s]) / calls / 1000.0) << "us"
				  << "(n=" << snap.stageCalls[s] << ")";
	}
	std::cout << "\nMESH overall median=" << percentile(meshMs, 0.5)
			  << " ms p95=" << percentile(meshMs, 0.95) << " ms over " << meshMs.size()
			  << " builds\n";
	return g_fails != 0 ? 1 : 0;
}


// ---------------------------------------------------------------------------
// Edit benchmark (issue #107 validation): section-selective rebuilds vs
// whole-chunk rebuilds across the issue's edit scenarios.
// ---------------------------------------------------------------------------

struct EditBenchRow
{
	std::string scenario;
	double sectionMs{0};
	double fullMs{0};
	int sectionsRebuilt{0};
	size_t sectionStagedBytes{0};
	size_t fullBytes{0};
};

static size_t maskedPayloadBytes(const MeshBuildResult &r, uint16_t mask)
{
	size_t bytes = 0;
	for (int s = 0; s < kChunkSectionCount; ++s)
	{
		if (((mask >> s) & 1u) == 0)
			continue;
		const SectionMeshPayload &p = r.sections[static_cast<size_t>(s)];
		bytes += p.opaqueVertices.size() * sizeof(Vertex);
		bytes += p.opaqueIndices.size() * sizeof(uint32_t);
		bytes += p.waterVertices.size() * sizeof(Vertex);
		bytes += p.waterIndices.size() * sizeof(uint32_t);
	}
	return bytes;
}

static size_t totalPayloadBytes(const MeshBuildResult &r)
{
	return maskedPayloadBytes(r, kAllSectionMask);
}

static double medianOf(std::vector<double> &v)
{
	if (v.empty())
		return 0.0;
	std::sort(v.begin(), v.end());
	return v[v.size() / 2];
}

// Applies `edit` to the chunk (+ mirror for border edits), builds ONLY the
// dirty sections (repeat runs for a stable median), then a full rebuild.
static EditBenchRow measureEdit(const std::string &name, Chunk &chunk,
								const std::function<void()> &edit, int repeatBuilds)
{
	EditBenchRow row;
	row.scenario = name;

	chunk.takeDirtySections();
	edit();
	const uint16_t mask = chunk.takeDirtySections();
	row.sectionsRebuilt = 0;
	for (int s = 0; s < kChunkSectionCount; ++s)
		if ((mask >> s) & 1u)
			++row.sectionsRebuilt;

	MeshResultPool *pool = chunk.getMeshResultPool();
	MeshBuildResult *r = pool->acquire();

	// Section-selective rebuild timing (first build warms the workspace).
	std::vector<double> sectionMs;
	for (int i = 0; i < repeatBuilds; ++i)
	{
		const auto t0 = std::chrono::steady_clock::now();
		chunk.buildMesh(*r, chunk.meshGeneration(), chunk.meshRevision(), mask);
		const auto t1 = std::chrono::steady_clock::now();
		pool->finishBuild(r);
		sectionMs.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
		if (i == 0)
			row.sectionStagedBytes = maskedPayloadBytes(*r, mask);
	}
	row.sectionMs = medianOf(sectionMs);

	// Whole-chunk rebuild timing on the same content.
	std::vector<double> fullMs;
	for (int i = 0; i < repeatBuilds; ++i)
	{
		const auto t0 = std::chrono::steady_clock::now();
		chunk.buildMesh(*r, chunk.meshGeneration(), chunk.meshRevision());
		const auto t1 = std::chrono::steady_clock::now();
		pool->finishBuild(r);
		fullMs.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
		if (i == 0)
			row.fullBytes = totalPayloadBytes(*r);
	}
	row.fullMs = medianOf(fullMs);

	pool->release(r);
	return row;
}

static int runEditBench(int argc, char **argv)
{
	const int repeatBuilds = (argc > 2) ? std::max(1, std::atoi(argv[2])) : 5;
	MeshResultPool pool;

	// Initial generation throughput: full builds over generated chunks.
	{
		TerrainGenerator gen(1337);
		std::vector<double> genMs;
		Chunk warm(glm::vec3(0.0f), ChunkState::UNLOADED, nullptr, nullptr, &pool);
		warm.prepareVoxelStorageForGeneration();
		warm.generateTerrain(gen);
		for (int i = 0; i < 8; ++i)
		{
			MeshBuildResult *r = pool.acquire();
			const auto t0 = std::chrono::steady_clock::now();
			warm.buildMesh(*r, warm.meshGeneration(), warm.meshRevision());
			const auto t1 = std::chrono::steady_clock::now();
			pool.finishBuild(r);
			genMs.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
		}
		std::cout << "EDITBENCH initial full build: median=" << medianOf(genMs)
				  << " ms over " << genMs.size() << " builds\n";
	}

	TerrainGenerator gen(1337);
	Chunk chunk(glm::vec3(0.0f), ChunkState::UNLOADED, nullptr, nullptr, &pool);
	chunk.prepareVoxelStorageForGeneration();
	chunk.generateTerrain(gen);
	// Neighbors so border sampling matches production.
	Chunk east(glm::vec3(float(CHUNK_SIZE), 0.0f, 0.0f), ChunkState::UNLOADED, nullptr, nullptr, &pool);
	east.prepareVoxelStorageForGeneration();
	east.generateTerrain(gen);
	chunk.rebuildBordersFromNeighbors(nullptr, &east, nullptr, nullptr);

	// Warm the workspace once.
	{
		MeshBuildResult *r = pool.acquire();
		chunk.buildMesh(*r, chunk.meshGeneration(), chunk.meshRevision());
		pool.finishBuild(r);
		pool.release(r);
	}

	std::vector<EditBenchRow> rows;

	// 2. one edit in the middle of a section: place a block on the surface.
	{
		int surfaceY = -1;
		for (int y = CHUNK_HEIGHT - 2; y > 0; --y)
			if (chunk.getVoxel(8, y, 8).type != static_cast<uint8_t>(AIR) &&
					chunk.getVoxel(8, y + 1, 8).type == static_cast<uint8_t>(AIR))
			{
				surfaceY = y;
				break;
			}
		const int py = surfaceY + 1;
		rows.push_back(measureEdit(
			"mid-section place @(" + std::to_string(8) + "," + std::to_string(py) + ",8)",
			chunk, [&chunk, py]
			{ chunk.setVoxel(8, py, 8, BRICKS); }, repeatBuilds));
		chunk.takeDirtySections();
		chunk.setVoxel(8, py, 8, AIR); // revert
		chunk.takeDirtySections();
	}

	// 3. one edit at the y=15/16 boundary: place at the first layer of a
	// section on top of solid ground.
	{
		int boundaryY = -1;
		for (int y = 96; y > 16; y -= 16)
			if (chunk.getVoxel(4, y, 4).type != static_cast<uint8_t>(AIR))
			{
				boundaryY = y;
				break;
			}
		if (boundaryY > 0)
		{
			rows.push_back(measureEdit(
				"y-boundary place @(4," + std::to_string(boundaryY) + ",4)",
				chunk, [&chunk, boundaryY]
				{ chunk.setVoxel(4, boundaryY, 4, BRICKS); }, repeatBuilds));
			chunk.takeDirtySections();
			chunk.setVoxel(4, boundaryY, 4, AIR);
			chunk.takeDirtySections();
		}
	}

	// 4. one edit at the x/z chunk border: x=0 mirrors into the east
	// neighbor's border strip.
	{
		int surfaceY = -1;
		for (int y = CHUNK_HEIGHT - 2; y > 0; --y)
			if (chunk.getVoxel(0, y, 9).type != static_cast<uint8_t>(AIR) &&
					chunk.getVoxel(0, y + 1, 9).type == static_cast<uint8_t>(AIR))
			{
				surfaceY = y;
				break;
			}
		const int py = surfaceY + 1;
		rows.push_back(measureEdit(
			"chunk-border place @(0," + std::to_string(py) + ",9) + mirror",
			chunk, [&chunk, &east, py]
			{
				chunk.setVoxel(0, py, 9, BRICKS);
				east.setVoxel(CHUNK_SIZE, py, 9, BRICKS); // mirror write
			}, repeatBuilds));
		chunk.takeDirtySections();
		east.takeDirtySections();
		chunk.setVoxel(0, py, 9, AIR);
		east.setVoxel(CHUNK_SIZE, py, 9, AIR);
		chunk.takeDirtySections();
		east.takeDirtySections();
	}

	// 5. burst of edits in one section: 24 place/delete pairs at y=40.
	{
		rows.push_back(measureEdit(
			"burst 24 edits in section 2",
			chunk, [&chunk]
			{
				for (int i = 0; i < 12; ++i)
				{
					chunk.setVoxel(i, 40, i, BRICKS);
					chunk.setVoxel(i + 1, 41, i + 1, BRICKS);
				}
			}, repeatBuilds));
		chunk.takeDirtySections();
		for (int i = 0; i < 12; ++i)
		{
			chunk.setVoxel(i, 40, i, AIR);
			chunk.setVoxel(i + 1, 41, i + 1, AIR);
		}
		chunk.takeDirtySections();
	}

	// 6. edits distributed across many sections.
	{
		rows.push_back(measureEdit(
			"16 edits spread over 8 sections",
			chunk, [&chunk]
			{
				for (int s = 0; s < 8; ++s)
				{
					chunk.setVoxel(2 + s, s * 16 + 5, 2, BRICKS);
					chunk.setVoxel(13 - s, s * 16 + 9, 13, BRICKS);
				}
			}, repeatBuilds));
		chunk.takeDirtySections();
		for (int s = 0; s < 8; ++s)
		{
			chunk.setVoxel(2 + s, s * 16 + 5, 2, AIR);
			chunk.setVoxel(13 - s, s * 16 + 9, 13, AIR);
		}
		chunk.takeDirtySections();
	}

	std::cout << "EDITBENCH scenario | section ms | full ms | sections | staged KiB | full KiB\n";
	for (const EditBenchRow &r : rows)
	{
		std::cout << "EDITBENCH " << r.scenario << " | " << r.sectionMs << " | "
				  << r.fullMs << " | " << r.sectionsRebuilt << " | "
				  << (r.sectionStagedBytes / 1024.0) << " | "
				  << (r.fullBytes / 1024.0) << "\n";
	}
	return 0;
}

// ---------------------------------------------------------------------------

static void testSmallPlantGeometry()
{
    for (const auto type : {SHORT_GRASS, FERN, WILDFLOWER, DRY_SHRUB, SEAGRASS, LILY_PAD})
    {
        Scene s(glm::vec3(-16.f, 0.f, -32.f));
        // Plant on the first cell of a section: the section beneath still
        // owns and emits the support's entire top face.
        s.chunk.setVoxel(0, 15, 0, STONE);
        s.chunk.setVoxel(0, 16, 0, type);
        auto *r = s.pool.acquire();
        s.chunk.buildMesh(*r, s.chunk.meshGeneration(), s.chunk.meshRevision());
        s.pool.finishBuild(r);
        CHECK(r->sections[0].opaqueVertices.size() == 24, "plant does not hide the support cube");
        const auto &p = r->sections[1];
        const size_t quads = type == LILY_PAD ? 1u : 2u;
        CHECK(p.opaqueVertices.size() == quads * 4 && p.opaqueIndices.size() == quads * 12,
              "small plants emit only explicit double-sided planes");
        CHECK(p.waterVertices.empty(), "alpha-cut plants use the opaque stream");
        for (const auto &v : p.opaqueVertices)
        {
            const auto local = v.position - s.chunk.getPosition();
            CHECK(local.x >= 0.f && local.x <= 1.f && local.z >= 0.f && local.z <= 1.f,
                  "detail geometry remains inside its owning column");
            CHECK(local.y >= 16.f && local.y < 17.f, "detail remains in its owning section");
            CHECK(vTexture(v) == static_cast<uint32_t>(type), "detail keeps its atlas layer");
            CHECK(vNormal(v) == 2u, "detail quads use the stylized upward lighting normal");
            CHECK(vBiome(v) == blockUsesGrassTint(type), "only grayscale plants receive grass tint");
        }
        for (uint32_t index : p.opaqueIndices)
            CHECK(index < p.opaqueVertices.size(), "detail indices are section local");
        for (size_t q = 0; q < quads; ++q)
            CHECK(p.opaqueIndices[q * 12] == p.opaqueIndices[q * 12 + 8] &&
                  p.opaqueIndices[q * 12 + 2] == p.opaqueIndices[q * 12 + 6],
                  "back triangles reverse the front winding");
        s.pool.release(r);
        r = s.pool.acquire();
        s.chunk.buildLODMesh(*r, s.chunk.meshGeneration(), s.chunk.meshRevision());
        s.pool.finishBuild(r);
        for (const auto &v : r->opaqueVertices)
            CHECK(vTexture(v) == STONE && v.position.y == 16.f, "LOD omits the plant and preserves its support");
        s.pool.release(r);
    }
}

int main(int argc, char **argv)
{
	if (argc > 1 && std::strcmp(argv[1], "--hash-corpus") == 0)
		return runHashCorpus(argc, argv);
	if (argc > 1 && std::strcmp(argv[1], "--bench-corpus") == 0)
		return runBenchCorpus(argc, argv);
	if (argc > 1 && std::strcmp(argv[1], "--edit-bench") == 0)
		return runEditBench(argc, argv);

    testSmallPlantGeometry();
	testUniformSlabMerges();
	testBlockTypeBoundary();
	testTransparencyPairs();
	testWater();
	testBiomeTintBoundary();
	testChunkBorders();
	testOccupiedSpanEdges();
	testAoCornerSampling();
	testSectionSeamOwnership();

	// Every scene must also build identically through the forced full-range
	// path (exercises the clamped classification rect of the key mesher).
	{
		Scene s;
		fillSlab(s.chunk, 0, 0, 4, 4, 8, STONE);
		testKeyPathMatchesFullRange("slab equivalence", s.chunk, s.pool);
	}
	{
		Scene s;
		fillSlab(s.chunk, 0, 0, 2, 4, 8, STONE);
		fillSlab(s.chunk, 2, 0, 2, 4, 8, BRICKS);
		s.chunk.setVoxel(5, 8, 5, GLASS);
		testKeyPathMatchesFullRange("type-split equivalence", s.chunk, s.pool);
	}
	{
		Scene s;
		for (int y = 8; y < 10; ++y)
			fillSlab(s.chunk, 0, 0, 4, 4, y, WATER);
		testKeyPathMatchesFullRange("water equivalence", s.chunk, s.pool);
	}
	{
		Scene s;
		auto &grass = ChunkStateProbe::grassMut(s.chunk);
		for (int x = 0; x < 4; ++x)
		{
			s.chunk.setVoxel(x, 8, 0, GRASS_TOP);
			grass[0 * CHUNK_SIZE + x] = (x < 2) ? 0xFF5D9C42u : 0xFF7FB069u;
		}
		testKeyPathMatchesFullRange("tint equivalence", s.chunk, s.pool);
	}
	{
		Scene s;
		fillSlab(s.chunk, 0, 0, 4, 4, 0, STONE);
		s.chunk.setVoxel(8, CHUNK_HEIGHT - 1, 8, BRICKS);
		testKeyPathMatchesFullRange("span-edge equivalence", s.chunk, s.pool);
	}
	{
		// Border-influenced scene: occlusion across x = -1 / CHUNK_SIZE.
		Scene s;
		s.chunk.setVoxel(0, 8, 0, STONE);
		s.chunk.setVoxel(-1, 8, 0, STONE);
		s.chunk.setVoxel(CHUNK_SIZE - 1, 12, 7, BRICKS);
		s.chunk.setVoxel(CHUNK_SIZE, 12, 7, GLASS);
		testKeyPathMatchesFullRange("border equivalence", s.chunk, s.pool);
	}

	testNoisyEditedChunk();
	testMultithreadedDeterminism();

	if (g_fails != 0)
	{
		std::cerr << g_fails << " check(s) failed\n";
		return 1;
	}
	std::cout << "All face-key mesher checks passed\n";
	return 0;
}
