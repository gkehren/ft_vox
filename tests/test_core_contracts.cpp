// CPU-only contracts test (issue #205, Epic #204 Phase A).
//
// Proves acceptance criterion #2: the ft_vox_core static library and the
// render-independent contracts build, LINK and RUN without initializing a GPU
// or SDL. This target links ONLY ft_vox_core (transitively glm + FastNoise2 +
// Threads) and includes no SDL / Vulkan / ImGui header. It exercises a symbol
// from every one of the six core translation units so the linker must resolve
// them all from the CPU-only library:
//
//   WorldSave.cpp        -> fnv1a64 / ByteWriter / ByteReader / diffEditsAgainstBase
//   WorldPersistence.cpp -> WorldPersistence::isValidWorldName
//   TerrainGenerator.cpp -> TerrainGenerator::generateChunk / getBiomeAt
//   TerrainVegetation.cpp-> vegetation placed by generateChunk
//   MobSystem.cpp        -> MobSystem::reset / update / renderStates
//   MobModel.cpp         -> entities::MobModels bake
//
// It also pins the Phase-A invariants: the 120 Hz player / 60 Hz mob tick rates,
// the plain-value contract aliases, and fixed-step determinism.

#include <Core/SimContracts.hpp>
#include <Entities/MobSystem.hpp>
#include <Entities/MobModel.hpp>
#include <Chunk/TerrainGenerator.hpp>
#include <World/WorldPersistence.hpp>
#include <World/WorldSave.hpp>
#include <Physics/PlayerController.hpp>
#include <Physics/VoxelCollision.hpp>

#include <glm/glm.hpp>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <type_traits>
#include <vector>

namespace
{
int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char *msg)
{
    ++g_checks;
    if (!ok)
    {
        ++g_failures;
        std::printf("  FAIL: %s\n", msg);
    }
}

// --- Contract aliases are the existing plain values (single source of truth) -
static_assert(std::is_same_v<sim::PlayerCommand, physics::PlayerInput>);
static_assert(std::is_same_v<sim::VoxelCell, physics::Cell>);
static_assert(std::is_same_v<sim::VoxelMedium, physics::Medium>);
static_assert(std::is_same_v<sim::WorldQuery, physics::VoxelCollisionWorld>);
static_assert(std::is_same_v<sim::SimBody, physics::Body>);
static_assert(std::is_same_v<sim::QueryStats, physics::QueryStats>);
static_assert(std::is_same_v<sim::VoxelEdit, worldsave::ChunkEdit>);
static_assert(std::is_same_v<sim::WorldIdentity, worldsave::WorldMeta>);
static_assert(std::is_same_v<sim::MobSnapshot, entities::MobRenderState>);
static_assert(std::is_same_v<sim::EntityId, entities::EntityId>);

// --- Contracts are plain values: trivially copyable, no hidden state ---------
static_assert(std::is_trivially_copyable_v<sim::PlayerCommand>);
static_assert(std::is_trivially_copyable_v<sim::VoxelCell>);
static_assert(std::is_trivially_copyable_v<sim::SimBody>);
static_assert(std::is_trivially_copyable_v<sim::VoxelEdit>);
static_assert(std::is_trivially_copyable_v<sim::WorldIdentity>);
static_assert(std::is_trivially_copyable_v<sim::MobSnapshot>);
static_assert(std::is_trivially_copyable_v<sim::ChunkIdentity>);

// --- Tick rates (Epic #204 invariant: player 120 Hz, mobs 60 Hz) -------------
static_assert(sim::kPlayerTickHz == 120.0);
static_assert(sim::kMobTickHz == 60.0);
static_assert(physics::PlayerController::fixedStep == 1.0 / 120.0);

// A flat deterministic voxel world: solid below y = 64, air above. No Chunk,
// no terrain inference, no GPU - exactly the abstract seam the simulation uses.
class FlatWorld : public physics::VoxelCollisionWorld
{
public:
    physics::Cell sample(glm::ivec3 p) const override
    {
        if (p.y < 64)
            return {true, true, physics::Medium::Air};
        return {true, false, physics::Medium::Air};
    }
};

class FlatMobWorld : public entities::MobWorld
{
public:
    physics::Cell sample(glm::ivec3 p) const override
    {
        if (p.y < 64)
            return {true, true, physics::Medium::Air};
        return {true, false, physics::Medium::Air};
    }
    std::optional<glm::dvec3> surface(int x, int z) const override
    {
        return glm::dvec3(x + 0.5, 64.0, z + 0.5);
    }
};

void testAvailabilityVocabulary()
{
    sim::VoxelCell unknown{};
    unknown.available = false;
    sim::VoxelCell loaded{};
    loaded.available = true;
    check(sim::availabilityOf(unknown) == sim::VoxelAvailability::Unknown,
          "Cell.available==false maps to VoxelAvailability::Unknown");
    check(sim::availabilityOf(loaded) == sim::VoxelAvailability::Loaded,
          "Cell.available==true maps to VoxelAvailability::Loaded");
}

void testIdentityContracts()
{
    sim::ChunkIdentity a{};
    a.chunkX = 3;
    a.chunkZ = -2;
    a.generation = 7;
    a.revision = 42;
    sim::ChunkIdentity b = a;
    check(b.chunkX == 3 && b.chunkZ == -2 && b.generation == 7 && b.revision == 42,
          "ChunkIdentity is a copyable plain value (generation + revision pair)");

    sim::VoxelEdit edit{};
    edit.localIndex = 1234;
    edit.blockType = STONE;
    check(edit.localIndex == 1234 && edit.blockType == STONE,
          "VoxelEdit re-exports worldsave::ChunkEdit (final-value override)");

    sim::WorldIdentity world{};
    world.formatVersion = worldsave::kWorldSaveFormatVersion;
    world.seed = 1337;
    world.generatorVersion = TerrainGenerator::kGeneratorVersion;
    check(world.seed == 1337 && world.generatorVersion == TerrainGenerator::kGeneratorVersion,
          "WorldIdentity re-exports worldsave::WorldMeta (seed + generator version)");
}

void testMobTickRate()
{
    entities::MobSettings settings{};
    check(settings.fixedStep == 1.0 / sim::kMobTickHz,
          "MobSettings default fixedStep matches the 60 Hz contract");
    check(settings.maxSteps == 8, "MobSettings default maxSteps preserved");
    check(physics::PlayerController::maxSteps == 8, "PlayerController maxSteps preserved");
}

void testPlayerDeterminism()
{
    FlatWorld world;
    auto run = [&]() {
        physics::PlayerController player;
        player.reset(glm::dvec3(0.5, 64.0, 0.5));
        sim::PlayerCommand input{};
        input.move = glm::dvec2(1.0, 0.0);
        for (int i = 0; i < 240; ++i)
        {
            input.jumpPressed = (i == 30);
            player.advance(1.0 / 60.0, input, world);
        }
        return player.body;
    };
    const sim::SimBody a = run();
    const sim::SimBody b = run();
    check(a.position == b.position, "player fixed-step position is deterministic");
    check(a.velocity == b.velocity, "player fixed-step velocity is deterministic");
    check(a.grounded == b.grounded, "player grounded flag is deterministic");
    check(a.position.y >= 64.0 - 1e-6, "player rests on the authoritative floor (no fall-through)");
}

void testMobDeterminism()
{
    FlatMobWorld world;
    auto run = [&]() {
        entities::MobSystem mobs;
        mobs.reset(987654321ULL);
        for (int i = 0; i < 180; ++i)
            mobs.update(1.0 / 60.0, world, glm::dvec3(0.0, 64.0, 0.0), 200.0, false);
        std::vector<sim::MobSnapshot> states;
        mobs.renderStates(states);
        return states;
    };
    const std::vector<sim::MobSnapshot> a = run();
    const std::vector<sim::MobSnapshot> b = run();
    check(a.size() == b.size(), "mob snapshot count is deterministic");
    check(!a.empty(), "mob simulation spawned passive mobs on the flat world");
    bool identical = a.size() == b.size();
    for (size_t i = 0; identical && i < a.size(); ++i)
    {
        identical = a[i].position == b[i].position && a[i].yaw == b[i].yaw &&
                    a[i].species == b[i].species && a[i].gait == b[i].gait;
    }
    check(identical, "mob fixed-step render snapshots are deterministic for a fixed seed");
}

void testMobModelBake()
{
    // Forces MobModel.cpp to link from the CPU-only library: bakes the
    // articulated box-UV meshes for every species with no GPU involvement.
    entities::MobModels models;
    check(!models.vertices.empty(), "MobModels bake produces vertices CPU-only");
    bool allParts = true;
    for (size_t s = 0; s < entities::kMobSpeciesCount; ++s)
        allParts = allParts && !models.models[s].parts.empty();
    check(allParts, "every species has baked parts");

    sim::MobSnapshot state{};
    state.species = entities::MobSpecies::Cow;
    state.position = glm::vec3(0.0f);
    const glm::mat4 xform = entities::mobPartTransform(state, models.models[0].parts[0]);
    check(std::isfinite(xform[0][0]), "mobPartTransform yields a finite matrix CPU-only");
}

void testTerrainDeterminism()
{
    // Forces TerrainGenerator.cpp + TerrainVegetation.cpp to link CPU-only.
    TerrainGenerator gen(1337);
    const ChunkData a = gen.generateChunk(0, 0);
    const ChunkData b = gen.generateChunk(0, 0);
    check(a.voxels.size() == CHUNK_VOLUME, "generateChunk fills the full chunk volume");
    bool voxelsEqual = a.voxels.size() == b.voxels.size();
    for (size_t i = 0; voxelsEqual && i < a.voxels.size(); ++i)
        voxelsEqual = a.voxels[i].type == b.voxels[i].type;
    check(voxelsEqual, "terrain voxels are bit-identical for a fixed seed (CPU-only)");
    check(a.biomes == b.biomes, "terrain biome columns are deterministic");
    check(a.heightMap == b.heightMap, "terrain height map is deterministic");
    check(gen.getBiomeAt(0, 0) == a.biomes[0], "getBiomeAt agrees with the generated column");
}

void testSaveSerialization()
{
    // Forces WorldSave.cpp to link CPU-only: checksum + byte round-trip + diff.
    const char *payload = "ft_vox-core";
    const uint64_t hash = worldsave::fnv1a64(payload, std::strlen(payload));
    check(hash == worldsave::fnv1a64(payload, std::strlen(payload)), "fnv1a64 is deterministic");

    worldsave::ByteWriter writer;
    writer.u32(0x12345678u);
    writer.u64(0x9ABCDEF012345678ULL);
    const auto bytes = writer.take();
    worldsave::ByteReader reader(bytes.data(), bytes.size());
    check(reader.u32() == 0x12345678u, "ByteWriter/ByteReader u32 round-trip");
    check(reader.u64() == 0x9ABCDEF012345678ULL, "ByteWriter/ByteReader u64 round-trip");
    check(!reader.failed(), "ByteReader stays in bounds");

    std::vector<uint8_t> base(CHUNK_VOLUME, STONE);
    std::vector<sim::VoxelEdit> current;
    sim::VoxelEdit edit{};
    edit.localIndex = 100;
    edit.blockType = AIR;
    current.push_back(edit);
    const auto diff = worldsave::diffEditsAgainstBase(base.data(), current);
    check(diff.size() == 1 && diff[0].localIndex == 100 && diff[0].blockType == AIR,
          "diffEditsAgainstBase emits only the changed voxel");
}

void testPersistenceNameValidation()
{
    // Forces WorldPersistence.cpp to link CPU-only (no I/O, no GPU, no SDL).
    check(WorldPersistence::isValidWorldName("my_world"), "valid world name accepted");
    check(!WorldPersistence::isValidWorldName(""), "empty world name rejected");
    check(!WorldPersistence::isValidWorldName("../escape"), "path-traversal world name rejected");
}
} // namespace

int main()
{
    std::printf("test_core_contracts: CPU-only ft_vox_core (no GPU, no SDL)\n");
    testAvailabilityVocabulary();
    testIdentityContracts();
    testMobTickRate();
    testPlayerDeterminism();
    testMobDeterminism();
    testMobModelBake();
    testTerrainDeterminism();
    testSaveSerialization();
    testPersistenceNameValidation();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0)
        std::printf("PASS\n");
    return g_failures == 0 ? 0 : 1;
}
