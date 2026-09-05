#pragma once
#include <algorithm>
#include <array>
#include <climits>
#include <shared_mutex>

#include <Chunk/ChunkManager.hpp>
#include <Physics/BlockPhysics.hpp>

// Main-thread scoped view. Holds the map lock once, caches chunk lookups only
// for this view's lifetime, and never stores a pointer in the player/physics.
// Generation publishes readable backing via the atomic Chunk state; mesh
// workers only read it. Main-thread edits/unload cannot run during this view.
class ChunkCollisionView final : public physics::VoxelCollisionWorld
{
public:
    explicit ChunkCollisionView(const ChunkManager &manager)
        : manager(manager), lock(manager.m_mutex) {}

    physics::Cell sample(glm::ivec3 p) const override
    {
        // Fixed vertical world, with a solid lower boundary and open sky.
        if (p.y < 0) return {true, true};
        if (p.y >= WORLD_HEIGHT) return {true, false};
        // Preserve the engine's existing finite camera-coordinate envelope.
        if (p.x < SHRT_MIN || p.x >= SHRT_MAX || p.z < SHRT_MIN || p.z >= SHRT_MAX)
            return {true, true};
        const int cx = floorChunk(p.x), cz = floorChunk(p.z);
        const glm::ivec3 key(cx, 0, cz);
        const Chunk *chunk = nullptr;
        bool found = false;
        for (unsigned i = 0; i < count; ++i)
            if (cache[i].key == key) { chunk = cache[i].chunk; found = true; break; }
        if (!found)
        {
            const auto it = manager.m_chunks.find(key);
            chunk = it == manager.m_chunks.end() ? nullptr : it->second;
            cache[next] = {key, chunk};
            next = (next + 1) % cache.size();
            count = std::min<unsigned>(count + 1, static_cast<unsigned>(cache.size()));
        }
        if (!chunk || !chunk->isVoxelBackingReadable() || !chunk->hasVoxelStorage()) return {};
        const int x = p.x - cx * int(CHUNK_SIZE), z = p.z - cz * int(CHUNK_SIZE);
        TextureType type = static_cast<TextureType>(chunk->getVoxel(x, p.y, z).type);
        // Match the logical edit state; never resurrect an edit from a recycled
        // incarnation. Mirrors affect meshing only, not canonical occupancy.
        for (const auto &edit : manager.m_pendingEdits)
            if (!edit.borderNeighbor && edit.chunk == chunk &&
                edit.generation == chunk->meshGeneration() && edit.x == x && edit.y == p.y && edit.z == z)
                type = edit.type;
        return physics::blockCell(type);
    }
private:
    static int floorChunk(int n)
    {
        const int size = static_cast<int>(CHUNK_SIZE);
        return n / size - (n % size < 0 ? 1 : 0);
    }
    const ChunkManager &manager;
    std::shared_lock<std::shared_mutex> lock;
    struct Entry { glm::ivec3 key{}; const Chunk *chunk{nullptr}; };
    mutable std::array<Entry, 16> cache{};
    mutable unsigned count{0}, next{0};
};
