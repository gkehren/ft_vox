// SimDigest — canonical authoritative-state digest (issue #210, Epic #204
// Phase F). See the header for the exact canonical byte stream; nothing here
// may depend on pointers, container iteration order, padding or render state.
#include "Headless/SimDigest.hpp"

#include <Sim/GameSimulation.hpp>
#include <World/WorldCollisionView.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace headless
{
namespace
{
static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559,
			  "canonical digest requires IEEE-754 binary64 doubles");

/// Item position/velocity quantization (issue #230): 1/1024-block grid,
/// llround to a fixed-width i64. Absorbs last-bit FP noise while keeping any
/// gameplay-visible difference distinct; never hashes raw f64 item motion.
int64_t quantizeMotion(double value)
{
	return static_cast<int64_t>(std::llround(value * 1024.0));
}
} // namespace

void CanonicalHash::bytes(const void *data, size_t size)
{
	const auto *p = static_cast<const uint8_t *>(data);
	for (size_t i = 0; i < size; ++i)
	{
		m_state ^= p[i];
		m_state *= 1099511628211ULL;
	}
}

void CanonicalHash::u8(uint8_t value)
{
	bytes(&value, 1);
}

void CanonicalHash::u16(uint16_t value)
{
	const uint8_t le[2] = {
		static_cast<uint8_t>(value),
		static_cast<uint8_t>(value >> 8)};
	bytes(le, 2);
}

void CanonicalHash::i32(int32_t value)
{
	u32(static_cast<uint32_t>(value));
}

void CanonicalHash::u32(uint32_t value)
{
	// Explicit little-endian: independent of host byte order.
	const uint8_t le[4] = {
		static_cast<uint8_t>(value),
		static_cast<uint8_t>(value >> 8),
		static_cast<uint8_t>(value >> 16),
		static_cast<uint8_t>(value >> 24)};
	bytes(le, 4);
}

void CanonicalHash::i64(int64_t value)
{
	u64(static_cast<uint64_t>(value));
}

void CanonicalHash::u64(uint64_t value)
{
	const uint8_t le[8] = {
		static_cast<uint8_t>(value),
		static_cast<uint8_t>(value >> 8),
		static_cast<uint8_t>(value >> 16),
		static_cast<uint8_t>(value >> 24),
		static_cast<uint8_t>(value >> 32),
		static_cast<uint8_t>(value >> 40),
		static_cast<uint8_t>(value >> 48),
		static_cast<uint8_t>(value >> 56)};
	bytes(le, 8);
}

void CanonicalHash::f64(double value)
{
	uint64_t bits = 0;
	std::memcpy(&bits, &value, 8);
	u64(bits);
}

void CanonicalHash::dvec3(const glm::dvec3 &value)
{
	f64(value.x);
	f64(value.y);
	f64(value.z);
}

std::string CanonicalHash::hex() const
{
	static constexpr char kDigits[] = "0123456789abcdef";
	std::string out(16, '0');
	for (int i = 0; i < 16; ++i)
		out[static_cast<size_t>(15 - i)] = kDigits[(m_state >> (4 * i)) & 0xF];
	return out;
}

CanonicalHash computeStateDigest(const sim::GameSimulation &sim, const DigestArea &area,
								 uint64_t ticks, uint64_t committedEdits)
{
	CanonicalHash hash;
	hash.bytes("FTVHDIG2", 8);

	const sim::WorldSnapshot world = sim.worldSnapshot();
	hash.i32(world.seed);
	hash.u32(world.generatorVersion);
	hash.u64(ticks);
	hash.f64(1.0 / 60.0); // the runner's fixed host frame dt (documented)

	hash.i32(area.min.x);
	hash.i32(area.min.y);
	hash.i32(area.min.z);
	hash.i32(area.max.x);
	hash.i32(area.max.y);
	hash.i32(area.max.z);

	// Observed-area occupancy: canonical x-major -> z -> y order through the
	// authoritative collision view (effective pending edits included; the
	// runner quiesces before hashing, so the queue is empty in practice).
	if (sim.world())
	{
		WorldCollisionView view(*sim.world());
		for (int x = area.min.x; x <= area.max.x; ++x)
			for (int z = area.min.z; z <= area.max.z; ++z)
				for (int y = area.min.y; y <= area.max.y; ++y)
				{
					TextureType type = AIR;
					const physics::Cell cell = view.sampleWithType({x, y, z}, &type);
					hash.u8(cell.available ? 1 : 0);
					hash.u8(cell.available ? static_cast<uint8_t>(type) : 0);
				}
	}

	// Authoritative player body + mode + selection.
	const sim::PlayerSnapshot player = sim.playerSnapshot();
	hash.dvec3(player.position);
	hash.dvec3(player.velocity);
	hash.u8(player.grounded ? 1 : 0);
	hash.u8(player.waitingForTerrain ? 1 : 0);
	hash.u8(player.flight ? 1 : 0);
	hash.i32(static_cast<int32_t>(player.selectedBlock));

	// Logical edit progression.
	hash.u64(world.editRevision);
	hash.u64(committedEdits);

	// Authoritative mob population, canonically sorted by entity id (never
	// the spawn/retire vector order, never the derived render snapshots).
	std::vector<entities::Mob> mobs = sim.mobs().mobs();
	std::sort(mobs.begin(), mobs.end(),
			  [](const entities::Mob &a, const entities::Mob &b) { return a.id < b.id; });
	hash.u64(static_cast<uint64_t>(mobs.size()));
	for (const entities::Mob &mob : mobs)
	{
		hash.u64(mob.id);
		hash.u8(static_cast<uint8_t>(mob.species));
		hash.dvec3(mob.body.position);
		hash.dvec3(mob.body.velocity);
		hash.u8(mob.body.grounded ? 1 : 0);
		hash.u8(mob.body.waitingForTerrain ? 1 : 0);
		hash.f64(mob.yaw);
		hash.f64(mob.targetYaw);
		hash.f64(mob.timer);
		hash.f64(mob.gait);
		hash.f64(mob.stride);
		hash.f64(mob.age);
		hash.u8(mob.walking ? 1 : 0);
		hash.u64(mob.randomState);
	}

	// Gameplay inventory (issue #230): the selected hotbar slot, then all 36
	// slots in FIXED slot order (never a container iteration order). ItemIds
	// are the stable append-only registry ordinals, never TextureType.
	sim::InventorySnapshot inventory;
	sim.inventorySnapshot(inventory);
	hash.i32(inventory.selectedSlot);
	for (const items::ItemStack &stack : inventory.slots)
	{
		hash.u16(static_cast<uint16_t>(stack.item));
		hash.u16(stack.count);
	}

	// Authoritative dropped-item population, canonically sorted by entity id
	// (the mob discipline). Quantized motion + age; the derived interpolation
	// snapshots / render states are NEVER hashed.
	std::vector<items::ItemEntity> itemEntities = sim.itemEntities().entities();
	std::sort(itemEntities.begin(), itemEntities.end(),
			  [](const items::ItemEntity &a, const items::ItemEntity &b) { return a.id < b.id; });
	hash.u64(static_cast<uint64_t>(itemEntities.size()));
	for (const items::ItemEntity &entity : itemEntities)
	{
		hash.u64(entity.id);
		hash.u16(static_cast<uint16_t>(entity.stack.item));
		hash.u16(entity.stack.count);
		hash.i64(quantizeMotion(entity.body.position.x));
		hash.i64(quantizeMotion(entity.body.position.y));
		hash.i64(quantizeMotion(entity.body.position.z));
		hash.i64(quantizeMotion(entity.body.velocity.x));
		hash.i64(quantizeMotion(entity.body.velocity.y));
		hash.i64(quantizeMotion(entity.body.velocity.z));
		hash.u32(entity.ageTicks);
	}

	// Entity-id allocator high-water (ids are never reused across sessions).
	hash.u64(sim.itemIdHighWater());
	return hash;
}

DigestArea defaultDigestArea(const glm::dvec3 &bootstrapCenter)
{
	const glm::ivec3 chunk = VoxelWorld::worldToChunkCoord(glm::vec3(bootstrapCenter));
	DigestArea area;
	area.min = glm::ivec3((chunk.x - 1) * static_cast<int>(CHUNK_SIZE), 0,
						  (chunk.z - 1) * static_cast<int>(CHUNK_SIZE));
	area.max = glm::ivec3((chunk.x + 1) * static_cast<int>(CHUNK_SIZE) - 1,
						  static_cast<int>(WORLD_HEIGHT) - 1,
						  (chunk.z + 1) * static_cast<int>(CHUNK_SIZE) - 1);
	return area;
}

bool validateDigestArea(const DigestArea &area, std::string &error)
{
	if (area.min.x > area.max.x || area.min.y > area.max.y || area.min.z > area.max.z)
	{
		error = "digest area bounds are inverted (min must be <= max on every axis)";
		return false;
	}
	if (area.min.y < 0 || area.max.y >= static_cast<int>(WORLD_HEIGHT))
	{
		error = "digest area y must be inside [0, " + std::to_string(WORLD_HEIGHT - 1) + "]";
		return false;
	}
	// Spans in 64-bit BEFORE subtracting: the bounds are int-range, so
	// max - min can reach ~2^32 and would overflow int32 (UB) if done in int.
	const uint64_t dx = static_cast<uint64_t>(static_cast<int64_t>(area.max.x) - static_cast<int64_t>(area.min.x)) + 1;
	const uint64_t dy = static_cast<uint64_t>(static_cast<int64_t>(area.max.y) - static_cast<int64_t>(area.min.y)) + 1;
	const uint64_t dz = static_cast<uint64_t>(static_cast<int64_t>(area.max.z) - static_cast<int64_t>(area.min.z)) + 1;
	// Per-axis clamp first: a single axis beyond the cell cap already exceeds
	// the volume cap, and clamping keeps the pairwise product well inside
	// uint64 range (kMaxDigestCells^2 ~ 6.4e13). Without this, extreme bounds
	// could wrap the uint64 product BELOW the cap and admit a digest loop
	// spanning billions of cells (issue #210 review round 2).
	if (dx > kMaxDigestCells || dy > kMaxDigestCells || dz > kMaxDigestCells)
	{
		error = "digest area exceeds " + std::to_string(kMaxDigestCells) + " cells (per-axis span too large)";
		return false;
	}
	// Exact, overflow-safe volume check: dx*dy fits uint64 after the clamp;
	// dx*dy*dz > cap  <=>  dx*dy > cap/dz  (dz >= 1, integer division).
	const uint64_t base = dx * dy;
	if (base > kMaxDigestCells / dz)
	{
		error = "digest area exceeds " + std::to_string(kMaxDigestCells) + " cells";
		return false;
	}
	return true;
}

} // namespace headless
