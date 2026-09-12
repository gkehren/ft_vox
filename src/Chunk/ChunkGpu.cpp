// Chunk implementation - GPU publication: mesh result commit, the
// prepare/record/publish/discard arena replacement phases, indirect draw
// cache maintenance and range retirement (issue #181 split).
#include "Chunk.hpp"
#include <Chunk/ChunkMeshResult.hpp>
#include <Engine/WorkloadTelemetry.hpp>
#include <Vulkan/VkUpload.hpp>
#include <Vulkan/VkCommands.hpp>
#include <Vulkan/StagingRing.hpp>
#include <Vulkan/GpuResourceRetire.hpp>

#include <algorithm>
#include <cassert>
#include <cstring>

bool Chunk::hasUnuploadedFullMesh() const
{
  return m_pendingResult != nullptr && !m_pendingResult->isLOD;
}

bool Chunk::publishMeshResult(MeshBuildResult *result)
{
  if (!result)
    return false;
  // Publish-time identity validation (issue #104/#114): a result built by
  // another chunk, for a retired incarnation (generation), or from older
  // voxel/border content (revision) must never land here. The block goes
  // straight back to its pool and NO chunk state is touched - a stale
  // publish cannot mark the chunk MESHED or clobber a newer pending mesh.
  // The built sections are re-armed so the superseded geometry is rebuilt
  // (issue #107: a rejected section build must not leave its mask empty).
  if (result->owner != this ||
      result->generation != m_meshGeneration ||
      result->revision != m_meshRevision.load(std::memory_order_relaxed))
  {
    m_dirtySections.fetch_or(result->sectionsBuilt, std::memory_order_relaxed);
    result->homePool->release(result);
    return false;
  }
  // Repeated remesh without upload: the newest complete build wins and the
  // previous one returns to the pool.
  if (m_pendingResult && m_pendingResult != result)
    m_pendingResult->homePool->release(m_pendingResult);
  m_pendingResult = result;

  const bool wantLightNow = localLightCacheWanted();
  if (!wantLightNow)
  {
    releaseLightStorage();
    if (result->lightStorage && result->lightPool)
    {
      result->lightPool->release(result->lightStorage);
      result->lightStorage = nullptr;
    }
    result->lightCacheAction = LightCacheAction::Unchanged;
  }
  else if (result->lightCacheWantedAtBuild)
  {
    switch (result->lightCacheAction)
    {
    case LightCacheAction::Replace:
      if (result->lightStorage)
      {
        if (m_lightStorage && m_lightPool)
          m_lightPool->release(m_lightStorage);
        m_lightStorage = result->lightStorage;
        result->lightStorage = nullptr;
      }
      break;
    case LightCacheAction::Clear:
      releaseLightStorage();
      break;
    case LightCacheAction::Unchanged:
      break;
    }
  }
  else
  {
    // Not wanted during build, but wanted now: do not apply stale Clear, keep existing cache.
    result->lightCacheAction = LightCacheAction::Unchanged;
  }
  // Single state commit point: the mesh becomes official here, on the main
  // thread, only after validation succeeded.
  m_isLODMesh = result->isLOD;
  state = ChunkState::MESHED;
  meshNeedsUpdate.store(true);
  return true;
}

void Chunk::releasePendingMeshResult()
{
  if (!m_pendingResult)
    return;
  MeshBuildResult *result = m_pendingResult;
  m_pendingResult = nullptr;
  result->homePool->release(result);
}


// Draw command collection (issue #109 / issue #122): draw descriptors are
// cached per chunk and rebuilt exclusively when GPU-side truth changes
// (upload commit paths, LOD transitions, reset/release). The passes bind the
// shared mesh arenas once per page pair and submit one indirect draw per live
// section; every section's stored indices stay section-local (vertexOffset
// does the rebase on the GPU).
void Chunk::rebuildIndirectDrawCache()
{
  m_cachedOpaqueDrawCount = 0;
  m_cachedWaterDrawCount = 0;

  if (m_isLODMesh)
  {
    if (opaqueIndexCount > 0 && !m_lodOpaqueIndices.empty() && !m_lodOpaqueVertices.empty())
    {
      IndirectDraw &d = m_cachedOpaqueDraws[0];
      d.cmd = {static_cast<uint32_t>(m_lodOpaqueIndices.bytes / sizeof(uint32_t)), 1,
               static_cast<uint32_t>(m_lodOpaqueIndices.offset / sizeof(uint32_t)),
               static_cast<int32_t>(m_lodOpaqueVertices.offset / sizeof(Vertex)), 0};
      d.vertexPage = m_lodOpaqueVertices.page;
      d.indexPage = m_lodOpaqueIndices.page;
      d.chunkOrigin = glm::ivec3(this->position);
      m_cachedOpaqueDrawCount = 1;
    }

    if (waterIndexCount > 0 && !m_lodWaterIndices.empty() && !m_lodWaterVertices.empty())
    {
      IndirectDraw &d = m_cachedWaterDraws[0];
      d.cmd = {static_cast<uint32_t>(m_lodWaterIndices.bytes / sizeof(uint32_t)), 1,
               static_cast<uint32_t>(m_lodWaterIndices.offset / sizeof(uint32_t)),
               static_cast<int32_t>(m_lodWaterVertices.offset / sizeof(Vertex)), 0};
      d.vertexPage = m_lodWaterVertices.page;
      d.indexPage = m_lodWaterIndices.page;
      d.chunkOrigin = glm::ivec3(this->position);
      m_cachedWaterDrawCount = 1;
    }
    return;
  }

  // One command per live section (issue #109). Sections are NEVER merged
  // into a single command: each section's stored indices are section-local
  // and its vertices start at the section's own vertexBase, so only a
  // per-section vertexOffset fetches them correctly. Merging contiguous
  // sections would keep the first section's vertexOffset and redirect every
  // following section's indices into the first section's vertices -
  // destroyed geometry and missing faces.
  if (opaqueIndexCount > 0)
  {
    for (const SectionGpuSlot &slot : m_sectionGpu)
    {
      if (slot.indexCount == 0 || !slot.hasVertexRange() || !slot.hasIndexRange())
        continue;
      IndirectDraw &d = m_cachedOpaqueDraws[m_cachedOpaqueDrawCount++];
      d.cmd = {slot.indexCount, 1,
               static_cast<uint32_t>(slot.indexOffset / sizeof(uint32_t)),
               static_cast<int32_t>(slot.vertexBase), 0};
      d.vertexPage = slot.vertexPage;
      d.indexPage = slot.indexPage;
      d.chunkOrigin = glm::ivec3(this->position);
    }
  }

  // Same rule as opaque: one command per live section, each carrying its
  // own vertexOffset (issue #109). Never merge - section-local indices
  // would redirect into the first merged section's vertices.
  if (waterIndexCount > 0)
  {
    for (const SectionGpuSlot &slot : m_sectionGpuWater)
    {
      if (slot.indexCount == 0 || !slot.hasVertexRange() || !slot.hasIndexRange())
        continue;
      IndirectDraw &d = m_cachedWaterDraws[m_cachedWaterDrawCount++];
      d.cmd = {slot.indexCount, 1,
               static_cast<uint32_t>(slot.indexOffset / sizeof(uint32_t)),
               static_cast<int32_t>(slot.vertexBase), 0};
      d.vertexPage = slot.vertexPage;
      d.indexPage = slot.indexPage;
      d.chunkOrigin = glm::ivec3(this->position);
    }
  }
}

void Chunk::releaseGPU()
{
  if (!m_arenas)
  {
    m_cachedOpaqueDrawCount = 0;
    m_cachedWaterDrawCount = 0;
    return;
  }
  // Immediate free: releaseGPU runs on the destructor/reset/bootstrap paths
  // where no in-flight frame can still reference the ranges.
  retireSectionSlots(m_sectionGpu, m_arenas->opaqueVertex, m_arenas->opaqueIndex, true);
  retireSectionSlots(m_sectionGpuWater, m_arenas->waterVertex, m_arenas->waterIndex, true);
  retireLodRanges(m_arenas->opaqueVertex, m_arenas->opaqueIndex, m_lodOpaqueVertices,
                  m_lodOpaqueIndices, true);
  retireLodRanges(m_arenas->waterVertex, m_arenas->waterIndex, m_lodWaterVertices,
                  m_lodWaterIndices, true);
  m_allocator = VK_NULL_HANDLE;
  opaqueIndexCount = 0;
  waterIndexCount = 0;
  m_cachedOpaqueDrawCount = 0;
  m_cachedWaterDrawCount = 0;
}

void Chunk::releaseGPUDeferred()
{
  if (!m_arenas)
  {
    m_cachedOpaqueDrawCount = 0;
    m_cachedWaterDrawCount = 0;
    return;
  }
  // Frame-aware free: the arenas hand retired ranges back to their free
  // lists (and destroy emptied pages) only after the frames-in-flight
  // delay, so streaming unload needs no device wait.
  retireSectionSlots(m_sectionGpu, m_arenas->opaqueVertex, m_arenas->opaqueIndex, false);
  retireSectionSlots(m_sectionGpuWater, m_arenas->waterVertex, m_arenas->waterIndex, false);
  retireLodRanges(m_arenas->opaqueVertex, m_arenas->opaqueIndex, m_lodOpaqueVertices,
                  m_lodOpaqueIndices, false);
  retireLodRanges(m_arenas->waterVertex, m_arenas->waterIndex, m_lodWaterVertices,
                  m_lodWaterIndices, false);
  m_allocator = VK_NULL_HANDLE;
  opaqueIndexCount = 0;
  waterIndexCount = 0;
  m_cachedOpaqueDrawCount = 0;
  m_cachedWaterDrawCount = 0;
}

namespace
{
inline uint32_t alignUpBytes(uint32_t value, uint32_t alignment)
{
  return (value + alignment - 1) / alignment * alignment;
}
} // namespace

void Chunk::retireSectionSlots(std::array<SectionGpuSlot, kOccupancySections> &slots,
                               MeshArena &vertexArena, MeshArena &indexArena, bool immediate)
{
  // Single retirement path for every section range of a stream (issue #109
  // review): full->LOD, unload and release all funnel through here so no
  // path can forget a range.
  auto freeOne = [&](MeshArena &arena, const MeshArena::Range &r)
  {
    if (immediate)
      arena.freeImmediate(r);
    else
      arena.retire(r);
  };
  for (SectionGpuSlot &slot : slots)
  {
    if (slot.hasVertexRange())
      freeOne(vertexArena, {slot.vertexPage, slot.vertexOffset, slot.vertexSlotBytes});
    if (slot.hasIndexRange())
      freeOne(indexArena, {slot.indexPage, slot.indexOffset, slot.indexSlotBytes});
    slot = {};
  }
}

void Chunk::retireLodRanges(MeshArena &vertexArena, MeshArena &indexArena,
                            MeshArena::Range &vertices, MeshArena::Range &indices,
                            bool immediate)
{
  auto freeOne = [&](MeshArena &arena, const MeshArena::Range &r)
  {
    if (immediate)
      arena.freeImmediate(r);
    else
      arena.retire(r);
  };
  if (!vertices.empty())
    freeOne(vertexArena, vertices);
  if (!indices.empty())
    freeOne(indexArena, indices);
  vertices = {};
  indices = {};
}

// ALLOCATE phase of the sectioned transaction (PR #178 review round 4):
// PLAN + ALLOCATE. Nothing is published, nothing touches staging and no
// copy command is recorded; any failure frees every range the plan created
// and leaves the published mesh exactly as it is. The plan survives frames
// inside a commit group until publication.
bool Chunk::prepareSectionReplacement(MeshBuildResult &result, MeshArenas &arenas,
                                      GpuReplacement &out)
{
  out.result = &result;
  out.lod = false;

  auto opaquePayload = [&result](int s)
  {
    return std::make_pair(&result.sections[s].opaqueVertices,
                          &result.sections[s].opaqueIndices);
  };
  auto waterPayload = [&result](int s)
  {
    return std::make_pair(&result.sections[s].waterVertices,
                          &result.sections[s].waterIndices);
  };

  auto planStream = [&](auto payloadSel, std::array<SectionGpuSlot, kOccupancySections> &slots,
                        GpuReplacement::StreamPlan &plan)
  {
    for (int s = 0; s < kOccupancySections; ++s)
    {
      if (((result.sectionsBuilt >> s) & 1u) == 0)
        continue; // untouched: the published slot is preserved as-is
      GpuReplacement::SectionPlan &p = plan.sections[static_cast<size_t>(s)];
      p.touched = true;
      const auto [verts, idxs] = payloadSel(s);
      p.vertexBytes = static_cast<uint32_t>(verts->size() * sizeof(Vertex));
      p.indexBytes = static_cast<uint32_t>(idxs->size() * sizeof(uint32_t));
      if (p.vertexBytes != 0 || p.indexBytes != 0)
        p.active = true;
    }
  };

  // Allocate every fresh range of BOTH streams. Pure CPU bookkeeping: if
  // anything fails, every range this transaction created is freed here and
  // no slot, LOD handle or draw count has been touched.
  auto allocateAll = [&](GpuReplacement::StreamPlan &opaquePlan,
                         GpuReplacement::StreamPlan &waterPlan) -> bool
  {
    bool ok = true;
    GpuReplacement::StreamPlan *plans[2] = {&opaquePlan, &waterPlan};
    for (int pi = 0; pi < 2 && ok; ++pi)
    {
      GpuReplacement::StreamPlan &plan = *plans[pi];
      MeshArena &vA = (pi == 0) ? arenas.opaqueVertex : arenas.waterVertex;
      MeshArena &iA = (pi == 0) ? arenas.opaqueIndex : arenas.waterIndex;
      for (int s = 0; s < kOccupancySections && ok; ++s)
      {
        GpuReplacement::SectionPlan &p = plan.sections[static_cast<size_t>(s)];
        if (!p.active)
          continue;
        ok = vA.allocate(p.vertexBytes, p.newV) &&
             iA.allocate(p.indexBytes, p.newI);
        if (ok)
        {
          p.vertexBase = p.newV.offset / sizeof(Vertex);
          // Precompute the future published slot: the swap itself happens
          // at publish time and cannot fail.
          SectionGpuSlot &ns = p.future;
          ns.vertexPage = p.newV.page;
          ns.vertexOffset = p.newV.offset;
          ns.vertexSlotBytes = p.newV.bytes;
          ns.vertexUsedBytes = p.vertexBytes;
          ns.vertexBase = p.vertexBase;
          ns.indexPage = p.newI.page;
          ns.indexOffset = p.newI.offset;
          ns.indexSlotBytes = p.newI.bytes;
          ns.indexUsedBytes = p.indexBytes;
          ns.indexCount = p.indexBytes / sizeof(uint32_t);
        }
      }
    }
    if (!ok)
    {
      // Free every fresh range this transaction created (V and I
      // separately: a section can have only one of the two).
      GpuReplacement::StreamPlan *rollbackPlans[2] = {&opaquePlan, &waterPlan};
      for (int pi = 0; pi < 2; ++pi)
      {
        GpuReplacement::StreamPlan &plan = *rollbackPlans[pi];
        MeshArena &vA = (&plan == &opaquePlan) ? arenas.opaqueVertex : arenas.waterVertex;
        MeshArena &iA = (&plan == &opaquePlan) ? arenas.opaqueIndex : arenas.waterIndex;
        for (int s = 0; s < kOccupancySections; ++s)
        {
          GpuReplacement::SectionPlan &p = plan.sections[static_cast<size_t>(s)];
          if (!p.newV.empty())
          {
            vA.freeImmediate(p.newV);
            p.newV = {};
          }
          if (!p.newI.empty())
          {
            iA.freeImmediate(p.newI);
            p.newI = {};
          }
        }
      }
      return false;
    }
    return true;
  };

  planStream(opaquePayload, m_sectionGpu, out.opaque);
  planStream(waterPayload, m_sectionGpuWater, out.water);
  return allocateAll(out.opaque, out.water);
}

// COPY phase of the sectioned transaction: preflight the exact staging
// requirement, reserve it, memcpy the payloads and record the copies.
// Frame-bound: the reservations and the recorded commands belong to the
// current frame only - the fresh ranges keep the payload once the submit
// completes. Returns false when the ring lacks room this frame (nothing
// was mutated; the caller retries next frame).
bool Chunk::recordSectionUpload(VmaAllocator allocator, StagingRing *staging,
                                VkCommandBuffer cmd, ImmediateCommands *imm,
                                MeshArenas &arenas, GpuReplacement &replacement)
{
  MeshBuildResult &result = *replacement.result;
  auto opaquePayload = [&result](int s)
  {
    return std::make_pair(&result.sections[s].opaqueVertices,
                          &result.sections[s].opaqueIndices);
  };
  auto waterPayload = [&result](int s)
  {
    return std::make_pair(&result.sections[s].waterVertices,
                          &result.sections[s].waterIndices);
  };

  auto totalStaging = [&](const GpuReplacement::StreamPlan &a,
                          const GpuReplacement::StreamPlan &b)
  {
    VkDeviceSize total = 0;
    for (const GpuReplacement::StreamPlan *plan : {&a, &b})
      for (int s = 0; s < kOccupancySections; ++s)
      {
        const GpuReplacement::SectionPlan &p = plan->sections[static_cast<size_t>(s)];
        if (!p.active)
          continue;
        total += static_cast<VkDeviceSize>(alignUpBytes(p.vertexBytes, StagingRing::kAlignment)) +
                 alignUpBytes(p.indexBytes, StagingRing::kAlignment);
      }
    return total;
  };

  // Frame-bound staging scratch, indexed per stream and section: opaque
  // and water reservations must remain distinct until both copy passes
  // have consumed them.
  struct StreamStagingScratch
  {
    std::array<VkDeviceSize, kOccupancySections> vertexOffsets{};
    std::array<VkDeviceSize, kOccupancySections> indexOffsets{};
    std::array<void *, kOccupancySections> vertexPointers{};
    std::array<void *, kOccupancySections> indexPointers{};
  };
  StreamStagingScratch opaqueScratch{};
  StreamStagingScratch waterScratch{};

  auto reserveStream = [&](auto payloadSel, GpuReplacement::StreamPlan &plan,
                           StreamStagingScratch &scratch) -> bool
  {
    if (!staging)
      return true;
    for (int s = 0; s < kOccupancySections; ++s)
    {
      GpuReplacement::SectionPlan &p = plan.sections[static_cast<size_t>(s)];
      if (!p.active)
        continue;
      const auto [verts, idxs] = payloadSel(s);
      if (p.vertexBytes != 0)
      {
        if (!staging->alloc(p.vertexBytes, scratch.vertexOffsets[static_cast<size_t>(s)],
                            scratch.vertexPointers[static_cast<size_t>(s)]))
          return false;
        std::memcpy(scratch.vertexPointers[static_cast<size_t>(s)], verts->data(), p.vertexBytes);
      }
      if (p.indexBytes != 0)
      {
        if (!staging->alloc(p.indexBytes, scratch.indexOffsets[static_cast<size_t>(s)],
                            scratch.indexPointers[static_cast<size_t>(s)]))
          return false;
        // Indices are stored section-local: the indirect draw's
        // vertexOffset rebases them on the GPU (issue #109).
        std::memcpy(scratch.indexPointers[static_cast<size_t>(s)], idxs->data(), p.indexBytes);
      }
    }
    return true;
  };

  if (staging &&
      totalStaging(replacement.opaque, replacement.water) >
          staging->sliceCapacity() - staging->usedThisFrame())
    return false;
  if (!reserveStream(opaquePayload, replacement.opaque, opaqueScratch) ||
      !reserveStream(waterPayload, replacement.water, waterScratch))
    return false; // unreachable after the exact preflight

  auto recordStream = [&](auto payloadSel, GpuReplacement::StreamPlan &plan, MeshArena &vArena,
                          MeshArena &iArena, const StreamStagingScratch &scratch)
  {
    for (int s = 0; s < kOccupancySections; ++s)
    {
      GpuReplacement::SectionPlan &p = plan.sections[static_cast<size_t>(s)];
      if (!p.active)
        continue;
      if (staging)
      {
        if (p.vertexBytes != 0)
        {
          VkBufferCopy copy{};
          copy.srcOffset = scratch.vertexOffsets[static_cast<size_t>(s)];
          copy.dstOffset = p.newV.offset;
          copy.size = p.vertexBytes;
          vkCmdCopyBuffer(cmd, staging->buffer(), vArena.pageBuffer(p.newV.page), 1, &copy);
        }
        if (p.indexBytes != 0)
        {
          VkBufferCopy copy{};
          copy.srcOffset = scratch.indexOffsets[static_cast<size_t>(s)];
          copy.dstOffset = p.newI.offset;
          copy.size = p.indexBytes;
          vkCmdCopyBuffer(cmd, staging->buffer(), iArena.pageBuffer(p.newI.page), 1, &copy);
        }
      }
      else
      {
        const auto [verts, idxs] = payloadSel(s);
        if (p.vertexBytes != 0)
          uploadBuffer(allocator, *imm, vArena.pageBufferRef(p.newV.page), verts->data(),
                       p.vertexBytes, p.newV.offset);
        if (p.indexBytes != 0)
          uploadBuffer(allocator, *imm, iArena.pageBufferRef(p.newI.page), idxs->data(),
                       p.indexBytes, p.newI.offset);
      }
    }
  };

  recordStream(opaquePayload, replacement.opaque, arenas.opaqueVertex, arenas.opaqueIndex,
               opaqueScratch);
  recordStream(waterPayload, replacement.water, arenas.waterVertex, arenas.waterIndex,
               waterScratch);
  replacement.recorded = true;
  return true;
}

// PUBLISH phase of the sectioned transaction: swap the prepared slot
// tables in, retire the replaced ranges frame-aware, update the draw
// counts and rebuild the indirect draw cache. Cannot fail.
void Chunk::publishSectionUpload(GpuResourceRetire *retire, MeshArenas &arenas,
                                 GpuReplacement &replacement)
{
  const bool frameAware = retire != nullptr;

  auto retireOldSlot = [&](const SectionGpuSlot &old, MeshArena &vArena, MeshArena &iArena)
  {
    // The published slots are read live: nothing else mutates them between
    // ALLOCATE and PUBLISH, so they still describe the ranges the
    // renderer has been drawing all along.
    if (old.hasVertexRange())
    {
      const MeshArena::Range r{old.vertexPage, old.vertexOffset, old.vertexSlotBytes};
      if (frameAware)
        vArena.retire(r);
      else
        vArena.freeImmediate(r);
    }
    if (old.hasIndexRange())
    {
      const MeshArena::Range r{old.indexPage, old.indexOffset, old.indexSlotBytes};
      if (frameAware)
        iArena.retire(r);
      else
        iArena.freeImmediate(r);
    }
  };

  auto commitStream = [&](GpuReplacement::StreamPlan &plan, MeshArena &vArena, MeshArena &iArena,
                          std::array<SectionGpuSlot, kOccupancySections> &slots)
  {
    auto newSlots = slots; // transactional commit: build aside, swap in
    for (int s = 0; s < kOccupancySections; ++s)
    {
      GpuReplacement::SectionPlan &p = plan.sections[static_cast<size_t>(s)];
      if (!p.touched)
        continue;
      retireOldSlot(slots[static_cast<size_t>(s)], vArena, iArena);
      newSlots[static_cast<size_t>(s)] = p.active ? p.future : SectionGpuSlot{};
    }
    slots = newSlots;
  };

  commitStream(replacement.opaque, arenas.opaqueVertex, arenas.opaqueIndex, m_sectionGpu);
  commitStream(replacement.water, arenas.waterVertex, arenas.waterIndex, m_sectionGpuWater);

  // Draw counts derive from the per-section ranges.
  opaqueIndexCount = 0;
  for (const SectionGpuSlot &slot : m_sectionGpu)
    opaqueIndexCount += slot.indexCount;
  waterIndexCount = 0;
  for (const SectionGpuSlot &slot : m_sectionGpuWater)
    waterIndexCount += slot.indexCount;

  // LOD -> full-quality transition: retire any existing LOD ranges and clear them.
  if (m_isLODMesh || !m_lodOpaqueVertices.empty() || !m_lodWaterVertices.empty())
  {
    retireLodRanges(arenas.opaqueVertex, arenas.opaqueIndex, m_lodOpaqueVertices,
                    m_lodOpaqueIndices, !frameAware);
    retireLodRanges(arenas.waterVertex, arenas.waterIndex, m_lodWaterVertices,
                    m_lodWaterIndices, !frameAware);
    m_isLODMesh = false;
  }

  rebuildIndirectDrawCache();

  uint64_t stagedVertexBytes = 0;
  uint64_t stagedIndexBytes = 0;
  for (GpuReplacement::StreamPlan *plan : {&replacement.opaque, &replacement.water})
    for (int s = 0; s < kOccupancySections; ++s)
    {
      stagedVertexBytes += plan->sections[static_cast<size_t>(s)].active
                               ? plan->sections[static_cast<size_t>(s)].vertexBytes
                               : 0;
      stagedIndexBytes += plan->sections[static_cast<size_t>(s)].active
                              ? plan->sections[static_cast<size_t>(s)].indexBytes
                              : 0;
    }
  telemetry::registry().add(telemetry::UploadVertexBytes, stagedVertexBytes);
  telemetry::registry().add(telemetry::UploadIndexBytes, stagedIndexBytes);
}

// Shared sectioned-upload core (issue #107/#109). Every upload is a
// TRANSACTION built on fresh arena ranges - a published range is never
// rewritten in place - run here as one ALLOCATE -> COPY -> PUBLISH
// sequence (the bootstrap/imm path frees replaced ranges immediately:
// nothing is in flight). Returns false only when staging space or an
// arena range cannot be obtained: the result stays attached and the
// upload retries next frame with every slot, range and draw count
// untouched.
bool Chunk::uploadSectionSlots(MeshBuildResult &result, VmaAllocator allocator,
                               StagingRing *staging, VkCommandBuffer cmd,
                               GpuResourceRetire *retire, ImmediateCommands *imm,
                               MeshArenas &arenas)
{
  m_arenas = &arenas;
  GpuReplacement replacement;
  if (!prepareSectionReplacement(result, arenas, replacement))
    return false;
  if (!recordSectionUpload(allocator, staging, cmd, imm, arenas, replacement))
  {
    discardSectionReplacement(arenas, replacement);
    return false;
  }
  publishSectionUpload(retire, arenas, replacement);
  return true;
}

// ALLOCATE phase of the whole-chunk LOD replacement (issue #109): all four
// ranges (opaque + water) are reserved before anything else; any failure
// frees every fresh range and leaves the old LOD untouched.

void Chunk::uploadToGPU(VmaAllocator allocator, ImmediateCommands &imm, MeshArenas &arenas)
{
  MemoryPublication memoryPublication{*this};
  if (allocator == VK_NULL_HANDLE)
    throw std::runtime_error("Chunk::uploadToGPU: null allocator");

  // Bootstrap path: immediate destroy is OK (caller waited idle or nothing draws yet).
  if (m_allocator != VK_NULL_HANDLE)
    releaseGPU();
  m_allocator = allocator;
  m_arenas = &arenas;

  // The CPU payload lives in the attached build result (issue #104).
  // Bootstrap is synchronous, so an arena OOM is fatal - but it must be loud
  // AND clean: every range a partially-successful sequence allocated is freed
  // before throwing, no count or slot pretends a mesh was uploaded, and the
  // pending CPU result stays attached (nothing was consumed).
  MeshBuildResult *result = m_pendingResult;
  if (result && result->isLOD)
  {
    const bool needOpaque = !result->opaqueVertices.empty() && !result->opaqueIndices.empty();
    const bool needWater = !result->waterVertices.empty() && !result->waterIndices.empty();
    auto rollbackLodRanges = [&]()
    {
      arenas.opaqueVertex.freeImmediate(m_lodOpaqueVertices);
      arenas.opaqueIndex.freeImmediate(m_lodOpaqueIndices);
      arenas.waterVertex.freeImmediate(m_lodWaterVertices);
      arenas.waterIndex.freeImmediate(m_lodWaterIndices);
      m_lodOpaqueVertices = {};
      m_lodOpaqueIndices = {};
      m_lodWaterVertices = {};
      m_lodWaterIndices = {};
      opaqueIndexCount = 0;
      waterIndexCount = 0;
      m_cachedOpaqueDrawCount = 0;
      m_cachedWaterDrawCount = 0;
    };
    if (needOpaque)
    {
      if (!arenas.opaqueVertex.allocate(
              static_cast<uint32_t>(result->opaqueVertices.size() * sizeof(Vertex)),
              m_lodOpaqueVertices) ||
          !arenas.opaqueIndex.allocate(
              static_cast<uint32_t>(result->opaqueIndices.size() * sizeof(uint32_t)),
              m_lodOpaqueIndices))
      {
        rollbackLodRanges();
        throw std::runtime_error("Chunk::uploadToGPU: arena allocation failed");
      }
      uploadBuffer(allocator, imm, arenas.opaqueVertex.pageBufferRef(m_lodOpaqueVertices.page),
                   result->opaqueVertices.data(), m_lodOpaqueVertices.bytes,
                   m_lodOpaqueVertices.offset);
      uploadBuffer(allocator, imm, arenas.opaqueIndex.pageBufferRef(m_lodOpaqueIndices.page),
                   result->opaqueIndices.data(), m_lodOpaqueIndices.bytes,
                   m_lodOpaqueIndices.offset);
      opaqueIndexCount = static_cast<uint32_t>(result->opaqueIndices.size());
    }
    else
    {
      opaqueIndexCount = 0;
    }
    if (needWater)
    {
      if (!arenas.waterVertex.allocate(
              static_cast<uint32_t>(result->waterVertices.size() * sizeof(Vertex)),
              m_lodWaterVertices) ||
          !arenas.waterIndex.allocate(
              static_cast<uint32_t>(result->waterIndices.size() * sizeof(uint32_t)),
              m_lodWaterIndices))
      {
        rollbackLodRanges();
        throw std::runtime_error("Chunk::uploadToGPU: arena allocation failed");
      }
      uploadBuffer(allocator, imm, arenas.waterVertex.pageBufferRef(m_lodWaterVertices.page),
                   result->waterVertices.data(), m_lodWaterVertices.bytes,
                   m_lodWaterVertices.offset);
      uploadBuffer(allocator, imm, arenas.waterIndex.pageBufferRef(m_lodWaterIndices.page),
                   result->waterIndices.data(), m_lodWaterIndices.bytes,
                   m_lodWaterIndices.offset);
      waterIndexCount = static_cast<uint32_t>(result->waterIndices.size());
    }
    else
    {
      waterIndexCount = 0;
    }
    // Bootstrap: releaseGPU() above already returned every previous range.
    m_sectionGpu.fill({});
    m_sectionGpuWater.fill({});
    m_isLODMesh = true;
    rebuildIndirectDrawCache();
  }
  else if (result)
  {
    // Full-quality sectioned upload: arena range per section. The upload is
    // transactional - on failure nothing was published and the pending CPU
    // result stays attached - so a synchronous bootstrap must not swallow it.
    if (!uploadSectionSlots(*result, allocator, nullptr, VK_NULL_HANDLE, nullptr, &imm, arenas))
      throw std::runtime_error("Chunk::uploadToGPU: arena allocation failed");
  }
  else
  {
    opaqueIndexCount = 0;
    waterIndexCount = 0;
    m_cachedOpaqueDrawCount = 0;
    m_cachedWaterDrawCount = 0;
  }

  releasePendingMeshResult();

  releaseNeighborBorders();
  meshNeedsUpdate = false;
}

bool Chunk::prepareLodReplacement(MeshBuildResult &result, MeshArenas &arenas,
                                  GpuReplacement &out)
{
  out.lod = true;
  out.needOpaque =
      !result.opaqueVertices.empty() && !result.opaqueIndices.empty();
  out.needWater =
      !result.waterVertices.empty() && !result.waterIndices.empty();
  auto rollback = [&]()
  {
    if (!out.newOV.empty())
      arenas.opaqueVertex.freeImmediate(out.newOV);
    if (!out.newOI.empty())
      arenas.opaqueIndex.freeImmediate(out.newOI);
    if (!out.newWV.empty())
      arenas.waterVertex.freeImmediate(out.newWV);
    if (!out.newWI.empty())
      arenas.waterIndex.freeImmediate(out.newWI);
    out.newOV = {};
    out.newOI = {};
    out.newWV = {};
    out.newWI = {};
  };
  if (out.needOpaque &&
      (!arenas.opaqueVertex.allocate(
           static_cast<uint32_t>(result.opaqueVertices.size() * sizeof(Vertex)), out.newOV) ||
       !arenas.opaqueIndex.allocate(
           static_cast<uint32_t>(result.opaqueIndices.size() * sizeof(uint32_t)), out.newOI)))
  {
    rollback();
    return false;
  }
  if (out.needWater &&
      (!arenas.waterVertex.allocate(
           static_cast<uint32_t>(result.waterVertices.size() * sizeof(Vertex)), out.newWV) ||
       !arenas.waterIndex.allocate(
           static_cast<uint32_t>(result.waterIndices.size() * sizeof(uint32_t)), out.newWI)))
  {
    rollback();
    return false;
  }
  return true;
}

// COPY phase of the LOD replacement: reserve staging, memcpy and record
// the copies. Frame-bound: nothing here survives the submit.
bool Chunk::recordLodReplacement(VmaAllocator allocator, StagingRing &staging,
                                 VkCommandBuffer cmd, MeshArenas &arenas,
                                 GpuReplacement &replacement)
{
  MeshBuildResult &result = *replacement.result;
  (void)allocator;
  // Staging scratch is frame-bound: the offsets and mapped pointers live
  // only inside this function and never in the persistent replacement.
  VkDeviceSize offV = 0, offI = 0, offWV = 0, offWI = 0;
  void *ptrV = nullptr, *ptrI = nullptr, *ptrWV = nullptr, *ptrWI = nullptr;
  if (replacement.needOpaque &&
      (!staging.alloc(replacement.newOV.bytes, offV, ptrV) ||
       !staging.alloc(replacement.newOI.bytes, offI, ptrI)))
    return false;
  if (replacement.needWater &&
      (!staging.alloc(replacement.newWV.bytes, offWV, ptrWV) ||
       !staging.alloc(replacement.newWI.bytes, offWI, ptrWI)))
    return false;

  if (replacement.needOpaque)
  {
    std::memcpy(ptrV, result.opaqueVertices.data(), replacement.newOV.bytes);
    std::memcpy(ptrI, result.opaqueIndices.data(), replacement.newOI.bytes);
  }
  if (replacement.needWater)
  {
    std::memcpy(ptrWV, result.waterVertices.data(), replacement.newWV.bytes);
    std::memcpy(ptrWI, result.waterIndices.data(), replacement.newWI.bytes);
  }

  if (replacement.needOpaque)
  {
    VkBufferCopy c{};
    c.srcOffset = offV;
    c.dstOffset = replacement.newOV.offset;
    c.size = replacement.newOV.bytes;
    vkCmdCopyBuffer(cmd, staging.buffer(), arenas.opaqueVertex.pageBuffer(replacement.newOV.page), 1, &c);
    c.srcOffset = offI;
    c.dstOffset = replacement.newOI.offset;
    c.size = replacement.newOI.bytes;
    vkCmdCopyBuffer(cmd, staging.buffer(), arenas.opaqueIndex.pageBuffer(replacement.newOI.page), 1, &c);
  }
  if (replacement.needWater)
  {
    VkBufferCopy c{};
    c.srcOffset = offWV;
    c.dstOffset = replacement.newWV.offset;
    c.size = replacement.newWV.bytes;
    vkCmdCopyBuffer(cmd, staging.buffer(), arenas.waterVertex.pageBuffer(replacement.newWV.page), 1, &c);
    c.srcOffset = offWI;
    c.dstOffset = replacement.newWI.offset;
    c.size = replacement.newWI.bytes;
    vkCmdCopyBuffer(cmd, staging.buffer(), arenas.waterIndex.pageBuffer(replacement.newWI.page), 1, &c);
  }
  replacement.recorded = true;
  return true;
}

// PUBLISH phase of the LOD replacement: retire every old range, swap in
// the new handles and rebuild the draw cache. Cannot fail.
void Chunk::publishLodReplacement(MeshArenas &arenas, GpuReplacement &replacement)
{
  MeshBuildResult &result = *replacement.result;

  if (!m_lodOpaqueVertices.empty())
    arenas.opaqueVertex.retire(m_lodOpaqueVertices);
  if (!m_lodOpaqueIndices.empty())
    arenas.opaqueIndex.retire(m_lodOpaqueIndices);
  m_lodOpaqueVertices = replacement.needOpaque ? replacement.newOV : MeshArena::Range{};
  m_lodOpaqueIndices = replacement.needOpaque ? replacement.newOI : MeshArena::Range{};
  if (!m_lodWaterVertices.empty())
    arenas.waterVertex.retire(m_lodWaterVertices);
  if (!m_lodWaterIndices.empty())
    arenas.waterIndex.retire(m_lodWaterIndices);
  m_lodWaterVertices = replacement.needWater ? replacement.newWV : MeshArena::Range{};
  m_lodWaterIndices = replacement.needWater ? replacement.newWI : MeshArena::Range{};
  // A sectioned layout never survives a LOD replacement: its ranges were
  // retired when the full build was replaced (or there were none).
  retireSectionSlots(m_sectionGpu, arenas.opaqueVertex, arenas.opaqueIndex, false);
  retireSectionSlots(m_sectionGpuWater, arenas.waterVertex, arenas.waterIndex, false);
  opaqueIndexCount = replacement.needOpaque
                         ? static_cast<uint32_t>(result.opaqueIndices.size())
                         : 0;
  waterIndexCount = replacement.needWater
                        ? static_cast<uint32_t>(result.waterIndices.size())
                        : 0;
  m_isLODMesh = true;
  rebuildIndirectDrawCache();
}

// Releases a replacement's fresh ranges without publishing. Ranges whose
// copies were already submitted retire frame-aware; never-recorded ranges
// free immediately (no command references them).
void Chunk::discardLodReplacement(MeshArenas &arenas, GpuReplacement &replacement)
{
  const bool inFlight = replacement.recorded;
  auto freeOne = [&](MeshArena &arena, MeshArena::Range &range)
  {
    if (!range.empty())
    {
      if (inFlight)
        arena.retire(range);
      else
        arena.freeImmediate(range);
      range = {};
    }
  };
  freeOne(arenas.opaqueVertex, replacement.newOV);
  freeOne(arenas.opaqueIndex, replacement.newOI);
  freeOne(arenas.waterVertex, replacement.newWV);
  freeOne(arenas.waterIndex, replacement.newWI);
}

void Chunk::discardSectionReplacement(MeshArenas &arenas, GpuReplacement &replacement)
{
  const bool inFlight = replacement.recorded;
  auto freeOne = [&](MeshArena &arena, MeshArena::Range &range)
  {
    if (!range.empty())
    {
      if (inFlight)
        arena.retire(range);
      else
        arena.freeImmediate(range);
      range = {};
    }
  };
  for (GpuReplacement::StreamPlan *plan : {&replacement.opaque, &replacement.water})
  {
    MeshArena &vA = (plan == &replacement.opaque) ? arenas.opaqueVertex : arenas.waterVertex;
    MeshArena &iA = (plan == &replacement.opaque) ? arenas.opaqueIndex : arenas.waterIndex;
    for (int s = 0; s < kOccupancySections; ++s)
    {
      GpuReplacement::SectionPlan &p = plan->sections[static_cast<size_t>(s)];
      freeOne(vA, p.newV);
      freeOne(iA, p.newI);
    }
  }
}

// ALLOCATE phase (frame-independent, failure-capable): plans and reserves
// every fresh arena range the attached pending result needs. On failure
// everything is rolled back; on success the replacement stays private to
// the caller until publishGPUUpload() swaps it in.
bool Chunk::prepareGPUUpload(MeshArenas &arenas, GpuReplacement &out)
{
  MemoryPublication memoryPublication{*this};
  m_arenas = &arenas;

  out = GpuReplacement{};
  out.result = m_pendingResult;
  if (!out.result)
    return true; // nothing pending: publish will just clear the draw counters
  out.valid = true;

  out.generation = m_meshGeneration;
  out.revision = m_meshRevision.load(std::memory_order_relaxed);
  const bool prepared = out.result->isLOD
                            ? prepareLodReplacement(*out.result, arenas, out)
                            : prepareSectionReplacement(*out.result, arenas, out);
  if (!prepared)
  {
    // A failed ALLOCATE must never leave a half-valid replacement behind:
    // the group gate skips re-preparing members whose replacement claims
    // to be valid, so a stale flag here would record from empty ranges.
    out = GpuReplacement{};
    return false;
  }
  return true;
}

// COPY phase (frame-bound): reserves staging and records the copies.
// Returns false only when the ring lacks room this frame; the allocated
// replacement stays private and the caller retries next frame.
bool Chunk::recordGPUUpload(VmaAllocator allocator, StagingRing &staging, VkCommandBuffer cmd,
                            MeshArenas &arenas, GpuReplacement &replacement)
{
  if (!replacement.valid || replacement.recorded)
    return true; // nothing pending or already resident
  if (replacement.lod)
    return recordLodReplacement(allocator, staging, cmd, arenas, replacement);
  return recordSectionUpload(allocator, &staging, cmd, nullptr, arenas, replacement);
}

// PUBLISH phase (cannot fail, CPU-only): swaps the prepared slots in,
// retires the replaced ranges frame-aware, rebuilds the draw caches and
// consumes the pending result. Atomic from the renderer's perspective: the
// previous line drew the old mesh, the next line draws the new one.
void Chunk::publishGPUUpload(GpuResourceRetire &retire, MeshArenas &arenas,
                             GpuReplacement &replacement)
{
  MemoryPublication memoryPublication{*this};
  if (!replacement.valid)
  {
    opaqueIndexCount = 0;
    waterIndexCount = 0;
    m_cachedOpaqueDrawCount = 0;
    m_cachedWaterDrawCount = 0;
  }
  else if (replacement.lod)
  {
    publishLodReplacement(arenas, replacement);
  }
  else
  {
    publishSectionUpload(&retire, arenas, replacement);
  }

  releasePendingMeshResult();

  releaseNeighborBorders();
  meshNeedsUpdate = false;
  replacement = GpuReplacement{};
}

// Releases a replacement's fresh ranges without publishing (group
// rollback, supersession, unload of a member). Recorded replacements
// retire frame-aware - their copies may still be executing - while
// never-recorded ranges free immediately.
void Chunk::discardGPUUpload(GpuReplacement &replacement)
{
  if (!replacement.valid)
    return;
  if (replacement.lod)
    discardLodReplacement(*m_arenas, replacement);
  else
    discardSectionReplacement(*m_arenas, replacement);
  replacement = GpuReplacement{};
}

bool Chunk::uploadToGPUAsync(VmaAllocator allocator, StagingRing &staging, VkCommandBuffer cmd,
                             GpuResourceRetire &retire, MeshArenas &arenas)
{
  const bool hadResult = m_pendingResult != nullptr;

  GpuReplacement replacement;
  if (!prepareGPUUpload(arenas, replacement))
  {
    telemetry::registry().add(telemetry::UploadDeferred);
    return false;
  }
  if (!recordGPUUpload(allocator, staging, cmd, arenas, replacement))
  {
    // The ring lacks room this frame: release the prepared ranges (they
    // were never recorded, so immediate free is safe) and retry next
    // frame. The CPU result stays attached and every published slot,
    // range and draw count is untouched.
    discardGPUUpload(replacement);
    telemetry::registry().add(telemetry::UploadDeferred);
    return false;
  }
  publishGPUUpload(retire, arenas, replacement);
  if (hadResult)
    telemetry::registry().add(telemetry::UploadChunks);
  return true;
}
