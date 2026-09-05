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
										 const Chunk &chunk)
{
	std::vector<QuadView> quads;
	if (verts.size() % 4 != 0 || indices.size() != verts.size() / 4 * 6)
		return quads; // malformed: caller's count checks will fire
	quads.reserve(verts.size() / 4);
	for (size_t q = 0; q < verts.size() / 4; ++q)
	{
		QuadView quad;
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

	void decode(const Chunk &chunk)
	{
		opaque = decodeQuads(result->opaqueVertices, result->opaqueIndices, chunk);
		water = decodeQuads(result->waterVertices, result->waterIndices, chunk);
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
	CHECK(a.result->opaqueVertices == b.result->opaqueVertices &&
			  a.result->opaqueIndices == b.result->opaqueIndices &&
			  a.result->waterVertices == b.result->waterVertices &&
			  a.result->waterIndices == b.result->waterIndices,
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
	CHECK(m.result->opaqueVertices.size() == 24, "slab: 6 quads x 4 vertices");
	CHECK(m.result->opaqueIndices.size() == 36, "slab: 6 quads x 6 indices");
	CHECK(m.result->waterVertices.empty() && m.result->waterIndices.empty(),
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
		const size_t vi = static_cast<size_t>(top - m.opaque.data()) * 4;
		CHECK(vAo(m.result->opaqueVertices[vi]) == vAo(m.result->opaqueVertices[vi + 1]) &&
				  vAo(m.result->opaqueVertices[vi]) == vAo(m.result->opaqueVertices[vi + 2]) &&
				  vAo(m.result->opaqueVertices[vi]) == vAo(m.result->opaqueVertices[vi + 3]),
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
		CHECK(m.result->opaqueVertices.empty() && m.result->opaqueIndices.empty(),
			  "water box: no opaque geometry");
		CHECK(m.result->waterVertices.size() == 24 && m.result->waterIndices.size() == 36,
			  "water box: 6 quads (top, bottom, four 4x2 sides)");
		const QuadView *top = findQuad(m.water, 2, WATER, glm::ivec3(0, 10, 0));
		CHECK(top != nullptr && top->mx == glm::ivec3(4, 10, 4),
			  "water box: top face merged 4x4");
		for (const Vertex &v : m.result->waterVertices)
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
		const size_t vi = static_cast<size_t>(top - m.opaque.data()) * 4;
		waterCornerAo = vAo(m.result->opaqueVertices[vi]); // corner at (0,8,0)
		farCornerAo = vAo(m.result->opaqueVertices[vi + 2]); // far corner (4,8,4)
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
	CHECK(!a.result->opaqueVertices.empty(), "noisy: terrain emits geometry");

	// Rebuild determinism through the public path (publish + pending).
	CHECK(chunk.generateMesh(), "noisy: public mesh build publishes");
	const MeshBuildResult *published = ChunkStateProbe::pendingResult(chunk);
	CHECK(published != nullptr &&
			  published->opaqueVertices == b.result->opaqueVertices &&
			  published->waterVertices == b.result->waterVertices,
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
	std::array<std::vector<Vertex>, kWorkers> opaque{};
	std::array<std::vector<uint32_t>, kWorkers> indices{};
	std::array<std::vector<Vertex>, kWorkers> water{};
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
			opaque[i] = results[i]->opaqueVertices;
			indices[i] = results[i]->opaqueIndices;
			water[i] = results[i]->waterVertices;
			pool.release(results[i]);
		}
	}
	for (int i = 1; i < kWorkers; ++i)
	{
		CHECK(opaque[i] == opaque[0] && indices[i] == indices[0] && water[i] == water[0],
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
	h = fnv1a(r.opaqueVertices.data(), r.opaqueVertices.size() * sizeof(Vertex), h);
	h = fnv1a(r.opaqueIndices.data(), r.opaqueIndices.size() * sizeof(uint32_t), h);
	h = fnv1a(r.waterVertices.data(), r.waterVertices.size() * sizeof(Vertex), h);
	h = fnv1a(r.waterIndices.data(), r.waterIndices.size() * sizeof(uint32_t), h);
	return h;
}

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
			const size_t ov = m ? m->opaqueVertices.size() : 0, oi = m ? m->opaqueIndices.size() : 0;
			const size_t wv = m ? m->waterVertices.size() : 0, wi = m ? m->waterIndices.size() : 0;
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
					  << " opaque=" << m->opaqueVertices.size() << "/" << m->opaqueIndices.size()
					  << " water=" << m->waterVertices.size() << "/" << m->waterIndices.size()
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

int main(int argc, char **argv)
{
	if (argc > 1 && std::strcmp(argv[1], "--hash-corpus") == 0)
		return runHashCorpus(argc, argv);
	if (argc > 1 && std::strcmp(argv[1], "--bench-corpus") == 0)
		return runBenchCorpus(argc, argv);

	testUniformSlabMerges();
	testBlockTypeBoundary();
	testTransparencyPairs();
	testWater();
	testBiomeTintBoundary();
	testChunkBorders();
	testOccupiedSpanEdges();
	testAoCornerSampling();

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
