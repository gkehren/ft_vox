#pragma once

// =============================================================================
// SimDigest — canonical authoritative-state digest of a headless simulation
// run (issue #210, Epic #204 Phase F; gameplay fields issue #230, Epic #223
// Phase G).
//
// Hashes DOCUMENTED CANONICAL FIELDS ONLY: world identity, the observed-area
// voxel occupancy, the authoritative player body, the authoritative mob
// population (sorted by entity id), the logical edit counters, the gameplay
// inventory + hotbar selection, the id-sorted dropped-item population and the
// entity-id allocator high-water. Never raw pointers, unordered iteration
// order, struct padding or derived/GPU state.
//
// Canonical byte stream (FNV-1a 64-bit; every integer little-endian fixed
// width, every double the IEEE-754 binary64 bit pattern, little-endian):
//   1. magic "FTVHDIG2"                       (8 bytes, format version)
//   2. seed i32, generatorVersion u32, ticks u64, frameDt f64
//   3. area bounds minX,minY,minZ,maxX,maxY,maxZ (6 x i32, inclusive)
//   4. area cells, x-major then z then y: availability u8 + block type u8
//      (type byte is 0 when the cell is unavailable/unknown)
//   5. player: position 3xf64, velocity 3xf64, grounded u8,
//      waitingForTerrain u8, flight u8, selectedBlock i32
//   6. editRevision u64 (next logical edit id), committedEdits u64
//   7. mob count u64, then per mob sorted by id ascending:
//      id u64, species u8, position 3xf64, velocity 3xf64, grounded u8,
//      waitingForTerrain u8, yaw f64, targetYaw f64, timer f64, gait f64,
//      stride f64, age f64, walking u8, randomState u64
//   8. selected hotbar slot i32, then the 36 inventory slots in FIXED slot
//      order 0..35: item u16 (ItemId registry ordinal) + count u16
//   9. item-entity count u64, then per entity sorted by id ascending:
//      id u64, item u16, count u16, quantized position 3xi64, quantized
//      velocity 3xi64, ageTicks u32. Quantization: llround(value * 1024)
//      (1/1024-block grid) — absorbs last-bit FP noise across compilers
//      while keeping any gameplay-visible difference distinct.
//  10. entity-id allocator high-water u64 (ids are never reused, #226/#228)
//
// FORMAT VERSION: "FTVHDIG1" -> "FTVHDIG2" (issue #230) — the gameplay fields
// (8..10) were appended DELIBERATELY; digests recorded before the bump are
// not comparable. No expected digest is hardcoded in tests/fixtures (they are
// computed at runtime), so the bump needed no fixture rewrite.
//
// Determinism scope: identical on repeated runs of the SAME binary on the
// same platform. Cross-platform / cross-compiler bit-identity is NOT promised
// (floating-point contraction and libm differences); see
// docs/headless-runner.md and the FT_VOX_PORTABLE_FP build option.
// =============================================================================

#include <cstdint>
#include <string>

#include <glm/glm.hpp>

namespace sim
{
class GameSimulation;
}

namespace headless
{

/// Inclusive observed-area cell bounds.
struct DigestArea
{
	glm::ivec3 min{0, 0, 0};
	glm::ivec3 max{-1, -1, -1};
};

/// FNV-1a 64-bit over an explicitly canonicalized byte stream.
class CanonicalHash
{
public:
	void bytes(const void *data, size_t size);
	void u8(uint8_t value);
	void u16(uint16_t value);
	void i32(int32_t value);
	void u32(uint32_t value);
	void i64(int64_t value);
	void u64(uint64_t value);
	void f64(double value);
	void dvec3(const glm::dvec3 &value);
	uint64_t value() const { return m_state; }
	/// 16 lowercase hex digits.
	std::string hex() const;

private:
	uint64_t m_state{14695981039346656037ULL};
};

/// Compute the run digest from the authoritative simulation state. The world
/// must be quiesced (no pending generation / logical edits) so the sampled
/// occupancy is the durable authoritative content. `ticks` and
/// `committedEdits` are the runner's plain counters.
CanonicalHash computeStateDigest(const sim::GameSimulation &sim, const DigestArea &area,
								 uint64_t ticks, uint64_t committedEdits);

/// Default observed area: the 32x32 column block (chunks cx-1..cx, cz-1..cz)
/// around the bootstrap chunk, full world height.
DigestArea defaultDigestArea(const glm::dvec3 &bootstrapCenter);

/// Validate user-supplied bounds: ordered, y inside the world, bounded volume
/// (kMaxDigestCells). Returns false and fills `error` otherwise.
bool validateDigestArea(const DigestArea &area, std::string &error);

inline constexpr uint64_t kMaxDigestCells = 8000000;

} // namespace headless
