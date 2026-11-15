#pragma once

// =============================================================================
// Render-independent simulation / render contracts (issue #205, Epic #204 A).
//
// This header is the Phase-A boundary marker between the AUTHORITATIVE CPU
// simulation and the DERIVED render world. It deliberately introduces no
// services, no std::function DI and no event bus: every contract below is a
// plain value (or a re-export of an existing plain value) so the tight
// fixed-step loops keep their direct C++ calls and data layouts. Names may
// evolve in later phases; the dependency direction may not.
//
// Dependency rule (enforced by tests/check_core_includes.py):
//   Core / Physics / Entities / World  ->  glm + FastNoise2 + std + platform
//   headers only (e.g. WorldPersistence.cpp uses <windows.h>).
//   NEVER  ->  Renderer/, Vulkan/, Camera/, SDL3, ImGui, miniz.
// The render side depends on these contracts; the simulation never depends on
// the render side. See docs/simulation-render-contracts.md for the ownership
// diagram and the B-G migration checkpoints.
//
// Authoritative (owned by the simulation, persisted):
//   voxels + sparse edits, player/mob kinematic state, world/save identity.
// Derived (rebuilt from the authoritative state, never persisted):
//   meshes, lighting caches, GPU uploads, visibility, camera, debug UI,
//   mob render snapshots.
// =============================================================================

#include <Physics/VoxelCollision.hpp>   // Cell, VoxelCollisionWorld, Body, Medium
#include <Physics/PlayerController.hpp> // PlayerInput + the 120 Hz fixed step
#include <Entities/MobTypes.hpp>        // MobRenderState, EntityId, MobSpecies
#include <World/WorldSave.hpp>          // ChunkEdit, WorldMeta (save identity)

#include <cstdint>

namespace sim
{

// --- Fixed-step rates -------------------------------------------------------
// The authoritative tick rates. Player physics runs at 120 Hz, passive mobs at
// 60 Hz; both must survive the refactor unchanged (Epic #204 invariant). The
// player rate is tied to the controller at compile time; the mob rate mirrors
// entities::MobSettings::fixedStep's default and is verified at runtime by
// test_core_contracts (MobSettings stores it as a tunable member).
inline constexpr double kPlayerTickHz = 120.0;
inline constexpr double kMobTickHz = 60.0;
static_assert(physics::PlayerController::fixedStep == 1.0 / kPlayerTickHz,
              "player tick rate must match PlayerController::fixedStep");

// --- Command (input -> simulation) ------------------------------------------
// Plain-value player command for one advance(). Already render-independent;
// re-exported so the runtime/adapter and the headless runner share one type.
using PlayerCommand = physics::PlayerInput;

// --- World query (simulation -> authoritative voxels) -----------------------
// The collision/sampling seam. `Cell.available == false` means the voxel is
// UNKNOWN (unloaded / not yet generated): the simulation must never infer
// solidity from procedural height, only from actual voxels.
using VoxelMedium = physics::Medium;
using VoxelCell = physics::Cell;
using WorldQuery = physics::VoxelCollisionWorld;

// Explicit loaded-vs-unknown vocabulary for the render bridge and streaming
// contracts (phase B/C). Derived from Cell.available; no new storage.
enum class VoxelAvailability : uint8_t
{
    Unknown, // unloaded / not generated yet (Cell.available == false)
    Loaded   // actual voxel present (Cell.available == true)
};
inline VoxelAvailability availabilityOf(const VoxelCell &cell)
{
    return cell.available ? VoxelAvailability::Loaded : VoxelAvailability::Unknown;
}

// --- Simulation state (authoritative kinematics) ----------------------------
// The player/mob body is the authoritative kinematic state (feet position,
// velocity, grounded, waitingForTerrain). Re-exported unchanged so the render
// bridge interpolates from exactly the data the simulation owns. The persisted
// player snapshot is PlayerPersistState (World/WorldPersistence.hpp).
using SimBody = physics::Body;
using QueryStats = physics::QueryStats;

// --- Revision / identity (authoritative content versioning) -----------------
// The render-independent (generation, revision) pair that a mesh build must
// match at publish time. Kept here as a plain value so the authoritative
// world (phase B) and the render bridge (phase C) can stamp/validate content
// without including GPU-coupled Chunk headers.
//   generation -> physical chunk incarnation (recycle / reset id)
//   revision   -> logical voxel/border content revision
// SINGLE SOURCE (issue #206, Phase B): the authoritative WorldChunk record
// owns this pair; MeshBuildResult carries a sim::ChunkIdentity stamped from
// the record at dispatch (its former duplicate generation/revision fields
// were deleted). Publish validates the result's identity against the record.
struct ChunkIdentity
{
    int32_t chunkX{0};
    int32_t chunkZ{0};
    uint64_t generation{0};
    uint64_t revision{0};
};

// Authoritative sparse voxel override (final value, not an operation log) and
// the world/save identity (seed + generator/format version). Re-exported.
using VoxelEdit = worldsave::ChunkEdit;
using WorldIdentity = worldsave::WorldMeta;

// --- Render snapshot (derived, simulation -> render) ------------------------
// Per-mob derived presentation state produced by MobSystem::renderStates().
// Re-exported so the render bridge consumes one plain-value snapshot type.
// Phase D (#208): the simulation facade (Sim/GameSimulation.hpp) owns the
// producers and adds the companion bounded value snapshots —
// sim::PlayerSnapshot (kinematics + mode + diagnostics) and sim::WorldSnapshot
// (identity + edit revision + residency/persistence state). Entity light
// sampling is presentation-side: the host fills MobRenderState.localSkylight
// from the render bridge AFTER taking the snapshot (headless: unassigned).
using MobSnapshot = entities::MobRenderState;
using EntityId = entities::EntityId;

} // namespace sim
