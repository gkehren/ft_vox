#pragma once

// =============================================================================
// ChunkCollisionView — render-side collision adapter (issue #206, Epic #204
// Phase B). The authoritative collision/query semantics now live in the
// CPU-only WorldCollisionView over VoxelWorld; this thin wrapper keeps the
// historical constructor (const ChunkManager&) and adds the DERIVED entity
// light sampling from the render light caches.
//
// Temporary adapter per the issue's architectural constraints: Phase C/E
// re-home the light seam (the bridge owns the caches) and remove this
// wrapper. Physics behavior is unchanged: same scoped-lock contract, same
// cache, same unknown/solid/medium vocabulary, same pending-edit overlay.
// =============================================================================

#include <Chunk/ChunkManager.hpp>
#include <World/WorldCollisionView.hpp>

// Main-thread scoped view. Holds the world map lock once (via the CPU view)
// plus the render map lock for light-cache reads, caches chunk lookups only
// for this view's lifetime, and never stores a pointer in the player/physics.
// Generation publishes readable backing via the atomic record state; mesh
// workers only read it. Main-thread edits/unload cannot run during this view.
//
// LOCK ORDER (issue #206 review): this view acquires world -> render. The
// mesh dispatch path acquires render -> world (ChunkManager::meshPendingChunks
// holds the render lock, then VoxelWorld::ensureShellPopulated takes the world
// lock). The two orders are only safe because BOTH paths run exclusively on
// the main thread today — neither map lock is ever contended across threads
// (workers take only the completion-queue and pool mutexes). When Phase C/E
// separates these across threads, this inversion MUST be resolved (e.g. a
// single lock ordering or a lock-free light snapshot); do not introduce a
// second thread that takes both locks without revisiting this.
class ChunkCollisionView final : public physics::VoxelCollisionWorld
{
public:
    explicit ChunkCollisionView(const ChunkManager &manager)
        : view(manager.world()), renderLock(manager.m_mutex), manager(manager) {}

    physics::Cell sample(glm::ivec3 p) const override { return view.sample(p); }
    physics::Cell sampleWithType(glm::ivec3 p, TextureType *outType) const
    {
        return view.sampleWithType(p, outType);
    }

    // World Y of the water surface above the given eye cell (issue #144).
    float waterSurfaceAbove(glm::ivec3 eye) const { return view.waterSurfaceAbove(eye); }

    lighting::LocalVoxelLight sampleLight(const glm::vec3 &worldPos) const
    {
        // Derived render light caches: the render lock is held for the
        // view's lifetime, so the Unlocked sampling contract is satisfied.
        return manager.sampleSmoothedLightUnlocked(worldPos);
    }

private:
    WorldCollisionView view;
    std::shared_lock<std::shared_mutex> renderLock;
    const ChunkManager &manager;
};
