#include <Engine/WorkloadTelemetry.hpp>
#include "Chunk.hpp"
#include <Chunk/StreamHelpers.hpp>
#include <Renderer/ShadowCascades.hpp>
#include <Renderer/Lighting.hpp>
#include <Renderer/TextureManager.hpp>
#include <Renderer/MinecraftTextures.hpp>
#include <Vulkan/VkUpload.hpp>
#include <Vulkan/VkCommands.hpp>
#include <Vulkan/StagingRing.hpp>
#include <Vulkan/GpuResourceRetire.hpp>
#include <stdexcept>
#include <algorithm>
#include <cstring>
#include <glm/gtx/hash.hpp>
#include <utils.hpp>
#include <vector>

// Packed ABGR water tint colour shared by the full mesh and LOD mesh generators.
static constexpr uint32_t WATER_COLOR = 0xFF'E6804D;

struct MeshWorkspace
{
  std::vector<uint8_t> mask;
  std::vector<glm::ivec3> skyQ;
  std::vector<glm::ivec3> blockQ;

  MeshWorkspace()
  {
    mask.reserve(CHUNK_HEIGHT * CHUNK_SIZE);
    skyQ.reserve(512);
    blockQ.reserve(256);
  }
};

static thread_local MeshWorkspace s_meshWorkspace;

Chunk::Chunk(const glm::vec3 &position, ChunkState state)
    : position(position), visible(false), state(state),
      opaqueIndexCount(0), waterIndexCount(0),
      voxels(CHUNK_VOLUME),
      neighborShellVoxels(18 * (CHUNK_HEIGHT + 2) * 18,
                          static_cast<uint8_t>(AIR)),
      meshNeedsUpdate(true) { publishCpuTelemetry(); }

Chunk::Chunk(Chunk &&other) noexcept
    : position(std::move(other.position)), visible(other.visible),
      state(other.state.load()), voxels(std::move(other.voxels)),
      m_allocator(other.m_allocator),
      vertexBuffer(other.vertexBuffer),
      indexBuffer(other.indexBuffer),
      waterVertexBuffer(other.waterVertexBuffer),
      waterIndexBuffer(other.waterIndexBuffer),
      opaqueIndexCount(other.opaqueIndexCount), waterIndexCount(other.waterIndexCount),
      meshNeedsUpdate(other.meshNeedsUpdate.load()),
      activeVoxels(std::move(other.activeVoxels)),
      neighborShellVoxels(std::move(other.neighborShellVoxels)),
      biomeGrassColors(other.biomeGrassColors),
      biomeFoliageColors(other.biomeFoliageColors),
      biomeTypes(other.biomeTypes),
      heightMap(other.heightMap),
      vertices(std::move(other.vertices)), indices(std::move(other.indices)),
      waterVertices(std::move(other.waterVertices)), waterIndices(std::move(other.waterIndices)),
      m_isLODMesh(other.m_isLODMesh)
{
  other.publishCpuTelemetry();
  publishCpuTelemetry();
  other.m_allocator = VK_NULL_HANDLE;
  other.vertexBuffer = {};
  other.indexBuffer = {};
  other.waterVertexBuffer = {};
  other.waterIndexBuffer = {};
  other.opaqueIndexCount = 0;
  other.waterIndexCount = 0;
}
Chunk &Chunk::operator=(Chunk &&other) noexcept
{
  if (this != &other)
  {
    releaseGPU();

    position = std::move(other.position);
    visible = other.visible;
    state.store(other.state.load());
    voxels = std::move(other.voxels);
    activeVoxels = std::move(other.activeVoxels);
    neighborShellVoxels = std::move(other.neighborShellVoxels);
    biomeGrassColors = other.biomeGrassColors;
    biomeFoliageColors = other.biomeFoliageColors;
    biomeTypes = other.biomeTypes;
    heightMap = other.heightMap;
    vertices = std::move(other.vertices);
    indices = std::move(other.indices);
    waterVertices = std::move(other.waterVertices);
    waterIndices = std::move(other.waterIndices);
    m_allocator = other.m_allocator;
    vertexBuffer = other.vertexBuffer;
    indexBuffer = other.indexBuffer;
    waterVertexBuffer = other.waterVertexBuffer;
    waterIndexBuffer = other.waterIndexBuffer;
    opaqueIndexCount = other.opaqueIndexCount;
    waterIndexCount = other.waterIndexCount;
    meshNeedsUpdate.store(other.meshNeedsUpdate.load());
    m_isLODMesh = other.m_isLODMesh;
    m_inTransit.store(other.m_inTransit.load());

    other.publishCpuTelemetry();
    publishCpuTelemetry();
    other.m_allocator = VK_NULL_HANDLE;
    other.vertexBuffer = {};
    other.indexBuffer = {};
    other.waterVertexBuffer = {};
    other.waterIndexBuffer = {};
    other.opaqueIndexCount = 0;
    other.waterIndexCount = 0;
  }
  return *this;
}

Chunk::~Chunk()
{
  telemetry::registry().replaceCpu(m_cpuTelemetry, std::array<uint64_t, 13>{});
  releaseGPU();
}

const glm::vec3 &Chunk::getPosition() const { return position; }

size_t Chunk::getIndex(uint32_t x, uint32_t y, uint32_t z) const
{
  return y * CHUNK_SIZE * CHUNK_SIZE + z * CHUNK_SIZE + x;
}

bool Chunk::isVisible() const { return visible; }

void Chunk::setVisible(bool visible) { this->visible = visible; }

void Chunk::setState(ChunkState state)
{
  if (state == ChunkState::GENERATED || state == ChunkState::UNLOADED)
  {
    meshNeedsUpdate = true;
  }
  this->state = state;
}

ChunkState Chunk::getState() const { return state; }

Voxel &Chunk::getVoxel(uint32_t x, uint32_t y, uint32_t z)
{
  return voxels[getIndex(x, y, z)];
}

const Voxel &Chunk::getVoxel(uint32_t x, uint32_t y, uint32_t z) const
{
  return voxels[getIndex(x, y, z)];
}

void Chunk::setVoxel(int x, int y, int z, TextureType type)
{
  if (static_cast<uint32_t>(x) < CHUNK_SIZE && static_cast<uint32_t>(y) < CHUNK_HEIGHT &&
      static_cast<uint32_t>(z) < CHUNK_SIZE)
  {
    size_t index = getIndex(x, y, z);
    voxels[index].type = static_cast<uint8_t>(type);
    if (type != AIR)
    {
      activeVoxels.set(index);
    }
    else
    {
      activeVoxels.reset(index);
    }
  }
  else if (x >= -1 && x <= CHUNK_SIZE && y >= -1 && y <= CHUNK_HEIGHT &&
           z >= -1 && z <= CHUNK_SIZE)
  {
    // Lazily allocate shell if it was freed after a previous GPU upload (improvement E)
    if (neighborShellVoxels.empty())
      neighborShellVoxels.assign(18 * (CHUNK_HEIGHT + 2) * 18, static_cast<uint8_t>(AIR));
    size_t shellIndex =
        (y + 1) * 18 * 18 + (z + 1) * 18 + (x + 1);
    neighborShellVoxels[shellIndex] = static_cast<uint8_t>(type);
  }
}

bool Chunk::deleteVoxel(const glm::vec3 &position)
{
  int x = static_cast<int>(position.x - this->position.x);
  int y = static_cast<int>(position.y - this->position.y);
  int z = static_cast<int>(position.z - this->position.z);
  if (x < 0)
    x += CHUNK_SIZE;
  if (z < 0)
    z += CHUNK_SIZE;

  if (isVoxelActive(x, y, z))
  {
    setVoxel(x, y, z, AIR);
    meshNeedsUpdate = true;
    state = ChunkState::GENERATED;
    return true;
  }
  return false;
}

bool Chunk::placeVoxel(const glm::vec3 &position, TextureType type)
{
  int x = static_cast<int>(position.x - this->position.x);
  int y = static_cast<int>(position.y - this->position.y);
  int z = static_cast<int>(position.z - this->position.z);
  if (x < 0)
    x += CHUNK_SIZE;
  if (z < 0)
    z += CHUNK_SIZE;

  if (!isVoxelActive(x, y, z))
  {
    setVoxel(x, y, z, type);
    meshNeedsUpdate = true;
    state = ChunkState::GENERATED;
    return true;
  }
  return false;
}

bool Chunk::isVoxelActive(int x, int y, int z) const
{
  if (static_cast<uint32_t>(x) < CHUNK_SIZE && static_cast<uint32_t>(y) < CHUNK_HEIGHT &&
      static_cast<uint32_t>(z) < CHUNK_SIZE)
  {
    size_t index = getIndex(x, y, z);
    return activeVoxels.test(index);
  }
  else if (x >= -1 && x <= CHUNK_SIZE && y >= -1 && y <= CHUNK_HEIGHT &&
           z >= -1 && z <= CHUNK_SIZE)
  {
    if (neighborShellVoxels.empty())
      return false;
    size_t shellIndex =
        (y + 1) * 18 * 18 + (z + 1) * 18 + (x + 1);
    return neighborShellVoxels[shellIndex] != static_cast<uint8_t>(AIR);
  }
  return false; // Outside known boundaries
}

void Chunk::generateTerrain(TerrainGenerator &generator)
{
  MemoryPublication memoryPublication{*this};
  if (state.load() != ChunkState::UNLOADED)
    return;

  // Ensure we use integer coordinates aligned with world grid
  int genX = static_cast<int>(std::round(position.x));
  int genZ = static_cast<int>(std::round(position.z));

  // Generate directly into this pooled chunk's reusable storage: no
  // temporary ChunkData and no CHUNK_VOLUME / border-shell copies after
  // generation. resize() only ever grows once; pool recycles keep capacity.
  voxels.resize(CHUNK_VOLUME);
  neighborShellVoxels.resize(kBorderVoxelCount);

  ChunkGenerationTarget target{
      .voxels = std::span<Voxel, CHUNK_VOLUME>(voxels),
      .borderVoxels = std::span<uint8_t, kBorderVoxelCount>(neighborShellVoxels),
      .biomes = biomeTypes,
      .heightMap = heightMap,
      .grassColors = biomeGrassColors,
      .foliageColors = biomeFoliageColors};
  generator.generateChunkInto(genX, genZ, target);

  // Update bitset for active voxels
  activeVoxels.reset(); // Clear all bits first
  for (int i = 0; i < CHUNK_VOLUME; ++i)
  {
    if (this->voxels[i].type !=
        TextureType::AIR)
    { // Or your equivalent of an air block
      activeVoxels.set(i);
    }
  }

  state = ChunkState::GENERATED;
  meshNeedsUpdate = true;
}

void Chunk::generateMesh()
{
  // Declare MemoryPublication first so MeshSample is destroyed first.
  // Mesh timing must exclude telemetry ownership publication overhead.
  MemoryPublication memoryPublication{*this};
  telemetry::MeshSample meshSample(telemetry::Skylight);
  m_isLODMesh = false; // K: mark as full-quality mesh
  vertices.clear();
  indices.clear();
  waterVertices.clear();
  waterIndices.clear();

  auto &workspace = s_meshWorkspace;
  uint32_t indexCounter = 0;
  uint32_t waterIndexCounter = 0;

  // Coarse Minecraft-style light field (sky + block) for Tier 1 shading.
  // Stored as high-nibble sky / low-nibble block in a flat array matching getIndex.
  thread_local std::vector<uint8_t> skyLight;
  thread_local std::vector<uint8_t> blockLight;
  skyLight.assign(static_cast<size_t>(CHUNK_VOLUME), 0);
  blockLight.assign(static_cast<size_t>(CHUNK_VOLUME), 0);
  {
    auto idxOf = [](int x, int y, int z) -> size_t {
      return static_cast<size_t>(x + CHUNK_SIZE * (y + CHUNK_HEIGHT * z));
    };
    auto isAirLike = [&](int x, int y, int z) -> bool {
      if (x < 0 || x >= CHUNK_SIZE || y < 0 || y >= CHUNK_HEIGHT || z < 0 || z >= CHUNK_SIZE)
        return true;
      const auto t = static_cast<TextureType>(getVoxel(x, y, z).type);
      return blockTransmitsSkyLight(t);
    };

    // Sky light: per column, cast down until solid (open sky = 15)
    for (int z = 0; z < CHUNK_SIZE; ++z)
    {
      for (int x = 0; x < CHUNK_SIZE; ++x)
      {
        uint8_t light = 15;
        for (int y = CHUNK_HEIGHT - 1; y >= 0; --y)
        {
          if (!isAirLike(x, y, z))
          {
            light = 0;
            continue;
          }
          skyLight[idxOf(x, y, z)] = light;
          // Full sky light continues down open air; solid roof zeros it above
          if (light > 0 && y > 0 && !isAirLike(x, y - 1, z))
            ; // keep full until blocked
          else if (light > 0)
            light = static_cast<uint8_t>(light > 0 ? light : 0);
        }
      }
    }

    // Horizontal sky flood into caves/overhangs (Minecraft-style −1 per step).
    // Vertical cast alone leaves cavern mouths pitch-black one block in.
    {
      auto &skyQ = workspace.skyQ;
      skyQ.clear();
      for (int z = 0; z < CHUNK_SIZE; ++z)
        for (int y = 0; y < CHUNK_HEIGHT; ++y)
          for (int x = 0; x < CHUNK_SIZE; ++x)
          {
            if (skyLight[idxOf(x, y, z)] > 0 && isAirLike(x, y, z))
              skyQ.emplace_back(x, y, z);
          }
      size_t skyHead = 0;
      const int skyDirs[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
      while (skyHead < skyQ.size())
      {
        const glm::ivec3 p = skyQ[skyHead++];
        const uint8_t cur = skyLight[idxOf(p.x, p.y, p.z)];
        if (cur <= 1)
          continue;
        const uint8_t next = static_cast<uint8_t>(cur - 1);
        for (auto &d : skyDirs)
        {
          const int nx = p.x + d[0], ny = p.y + d[1], nz = p.z + d[2];
          if (nx < 0 || nx >= CHUNK_SIZE || ny < 0 || ny >= CHUNK_HEIGHT || nz < 0 || nz >= CHUNK_SIZE)
            continue;
          const size_t i = idxOf(nx, ny, nz);
          if (skyLight[i] < next)
          {
            skyLight[i] = next;
            // Propagate through air; also write into solids so face sampling sees light
            if (isAirLike(nx, ny, nz))
              skyQ.emplace_back(nx, ny, nz);
          }
        }
      }
    }

    meshSample.next(telemetry::Blocklight);
    // Seed block light from emissive solids into neighboring air
    auto &queue = workspace.blockQ;
    queue.clear();
    for (int z = 0; z < CHUNK_SIZE; ++z)
      for (int y = 0; y < CHUNK_HEIGHT; ++y)
        for (int x = 0; x < CHUNK_SIZE; ++x)
        {
          const uint8_t em = lighting::blockLightEmission(getVoxel(x, y, z).type);
          if (em == 0)
            continue;
          // Light lives in air cells around the emitter
          const int dirs[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
          for (auto &d : dirs)
          {
            const int nx = x + d[0], ny = y + d[1], nz = z + d[2];
            if (nx < 0 || nx >= CHUNK_SIZE || ny < 0 || ny >= CHUNK_HEIGHT || nz < 0 || nz >= CHUNK_SIZE)
              continue;
            if (!isAirLike(nx, ny, nz) && static_cast<TextureType>(getVoxel(nx, ny, nz).type) != AIR)
            {
              // still light the solid face later via adjacent air — also seed the solid cell
            }
            const size_t i = idxOf(nx, ny, nz);
            if (blockLight[i] < em)
            {
              blockLight[i] = em;
              queue.emplace_back(nx, ny, nz);
            }
          }
          // Also seed emitter cell for face sampling
          const size_t ei = idxOf(x, y, z);
          if (blockLight[ei] < em)
          {
            blockLight[ei] = em;
            queue.emplace_back(x, y, z);
          }
        }

    // Propagate block light (coarse BFS, attenuation 1 per step)
    size_t head = 0;
    const int dirs[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    while (head < queue.size())
    {
      const glm::ivec3 p = queue[head++];
      const uint8_t cur = blockLight[idxOf(p.x, p.y, p.z)];
      if (cur <= 1)
        continue;
      const uint8_t next = static_cast<uint8_t>(cur - 1);
      for (auto &d : dirs)
      {
        const int nx = p.x + d[0], ny = p.y + d[1], nz = p.z + d[2];
        if (nx < 0 || nx >= CHUNK_SIZE || ny < 0 || ny >= CHUNK_HEIGHT || nz < 0 || nz >= CHUNK_SIZE)
          continue;
        // Propagate through air-like; allow into solids so faces pick up light
        const size_t i = idxOf(nx, ny, nz);
        if (blockLight[i] < next)
        {
          blockLight[i] = next;
          if (isAirLike(nx, ny, nz))
            queue.emplace_back(nx, ny, nz);
        }
      }
    }
  }

  meshSample.next(telemetry::Occupancy);
  // New helper for greedy meshing that checks local voxels and the precomputed
  // neighbor shell
  auto getVoxelDataForMeshing = [&](int lx, int ly, int lz) -> TextureType
  {
    if (static_cast<uint32_t>(lx) < CHUNK_SIZE && static_cast<uint32_t>(ly) < CHUNK_HEIGHT &&
        static_cast<uint32_t>(lz) < CHUNK_SIZE)
    {
      return static_cast<TextureType>(getVoxel(lx, ly, lz).type);
    }
    // Check the neighbor shell for out-of-bounds coordinates relevant to
    // meshing.
    if (lx >= -1 && lx <= CHUNK_SIZE && ly >= -1 && ly <= CHUNK_HEIGHT &&
        lz >= -1 && lz <= CHUNK_SIZE)
    {
      if (neighborShellVoxels.empty())
        return AIR; // shell freed after upload; treat as AIR
      size_t shellIndex = (ly + 1) * 18 * 18 + (lz + 1) * 18 + (lx + 1);
      return static_cast<TextureType>(neighborShellVoxels[shellIndex]);
    }
    return AIR; // Default to AIR if not in chunk and not in precomputed shell
  };

  const int dims[] = {CHUNK_SIZE, CHUNK_HEIGHT, CHUNK_SIZE};

  // Bound meshing to occupied Y span (skip empty sky / deep-empty slabs).
  int occMinY = 0;
  int occMaxY = CHUNK_HEIGHT - 1;
  {
    // Dense type strip for the pure helper (same layout as getIndex).
    thread_local std::vector<uint8_t> typeScratch;
    typeScratch.resize(CHUNK_VOLUME);
    for (size_t i = 0; i < voxels.size() && i < static_cast<size_t>(CHUNK_VOLUME); ++i)
      typeScratch[i] = voxels[i].type;
    if (!computeOccupancyY(typeScratch.data(), occMinY, occMaxY))
    {
      meshNeedsUpdate = true;
      state = ChunkState::MESHED;
      return;
    }
  }
  meshSample.next(telemetry::FacesGreedyAO);
  // Slice range needs neighbors one cell outside solids for face detection.
  const int ySliceMin = std::max(-1, occMinY - 1);
  const int ySliceMax = std::min(CHUNK_HEIGHT - 1, occMaxY); // x[d] runs to dims[d]-1 inclusive via < dims

  // Iterate over dimensions (X, Y, Z)
  for (int d = 0; d < 3; ++d)
  {
    int u = (d + 1) % 3; // First axis in the plane of the face
    int v = (d + 2) % 3; // Second axis in the plane of the face

    glm::ivec3 x = {0, 0, 0}; // Current voxel coordinate during slice iteration
    glm::ivec3 q = {0, 0,
                    0}; // Normal direction for the face (points from x to x+q)
    q[d] = 1;

    // Ensure mask is large enough for the current slice
    if (workspace.mask.size() < static_cast<size_t>(dims[u] * dims[v]))
    {
      workspace.mask.resize(dims[u] * dims[v]);
    }

    // Slice range along d; clamp Y when d==1.
    int dStart = -1;
    int dEnd = dims[d]; // exclusive upper for x[d] < dEnd
    if (d == 1)
    {
      dStart = ySliceMin;
      dEnd = ySliceMax + 1;
    }

    // Iterate over each slice of the chunk along dimension 'd'
    // x[d] ranges from -1 (representing boundary before chunk) to dims[d]-1
    // (last voxel layer) A face exists between slice x[d] and slice x[d]+1
    for (x[d] = dStart; x[d] < dEnd; ++x[d])
    {
      meshSample.data.maskCells += dims[u] * dims[v];
      std::fill(workspace.mask.begin(), workspace.mask.begin() + (dims[u] * dims[v]), 0); // Reset mask for each slice

      // Bound Y when it is a plane axis.
      int uStart = 0, uEnd = dims[u];
      int vStart = 0, vEnd = dims[v];
      if (u == 1)
      {
        uStart = std::max(0, occMinY);
        uEnd = std::min(dims[u], occMaxY + 1);
      }
      if (v == 1)
      {
        vStart = std::max(0, occMinY);
        vEnd = std::min(dims[v], occMaxY + 1);
      }

      // Iterate over the plane (u, v)
      for (x[u] = uStart; x[u] < uEnd; ++x[u])
      {
        for (x[v] = vStart; x[v] < vEnd; ++x[v])
        {

          if (workspace.mask[x[u] * dims[v] + x[v]])
          {
            continue; // Already processed this part of the slice
          }

          // Get types of voxels on either side of the potential face
          // Voxel at x is on one side, voxel at x+q is on the other.
          // Use the new helper function that checks the neighbor shell
          TextureType type1 = getVoxelDataForMeshing(x[0], x[1], x[2]);
          TextureType type2 =
              getVoxelDataForMeshing(x[0] + q[0], x[1] + q[1], x[2] + q[2]);

          TextureType quad_type = AIR;
          glm::ivec3 quad_normal_dir = {0, 0, 0};
          glm::ivec3 quad_origin_voxel_coord = {
              0, 0, 0}; // Min corner of the voxel this quad's face belongs to

          // Only generate faces for voxels that are inside this chunk.
          // Border/shell voxels are used solely for occlusion checks — the
          // neighboring chunk is responsible for rendering its own faces.
          bool type1InChunk = (x[0] >= 0 && x[0] < CHUNK_SIZE &&
                               x[1] >= 0 && x[1] < CHUNK_HEIGHT &&
                               x[2] >= 0 && x[2] < CHUNK_SIZE);
          glm::ivec3 xq = x + q;
          bool type2InChunk = (xq[0] >= 0 && xq[0] < CHUNK_SIZE &&
                               xq[1] >= 0 && xq[1] < CHUNK_HEIGHT &&
                               xq[2] >= 0 && xq[2] < CHUNK_SIZE);

          if (type1 != AIR && type1InChunk &&
              (type2 == AIR ||
               (TextureManager::isTransparent(type2) && type1 != type2)))
          {
            // Face belongs to type1, pointing towards type2
            quad_type = type1;
            quad_normal_dir = q;
            quad_origin_voxel_coord = x;
          }
          else if (type2 != AIR && type2InChunk &&
                   (type1 == AIR || (TextureManager::isTransparent(type1) &&
                                     type1 != type2)))
          {
            // Face belongs to type2, pointing towards type1
            quad_type = type2;
            quad_normal_dir = {-q[0], -q[1], -q[2]};
            quad_origin_voxel_coord = xq;
          }
          else
          {
            continue; // No visible face here, or types are the same opaque.
          }

          if (quad_type == AIR)
            continue;

          // Biome-tint types must not greedy-merge across color boundaries
          // (water uses a constant color, so it never splits on color).
          const bool isBiomeColoredType =
              (quad_type == GRASS_TOP || quad_type == GRASS_SIDE ||
               blockIsFoliage(quad_type));

          uint32_t originBiomeColor = 0;
          if (isBiomeColoredType)
          {
            int originColIdx = quad_origin_voxel_coord[2] * CHUNK_SIZE + quad_origin_voxel_coord[0];
            originBiomeColor = blockIsFoliage(quad_type)
                                   ? biomeFoliageColors[originColIdx]
                                   : biomeGrassColors[originColIdx];
          }

          auto getCandidateBiomeColor = [&](const glm::ivec3 &coord) -> uint32_t
          {
            int colIdx = coord[2] * CHUNK_SIZE + coord[0];
            return blockIsFoliage(quad_type)
                       ? biomeFoliageColors[colIdx]
                       : biomeGrassColors[colIdx];
          };

          // Calculate width (w) of the quad along dimension u
          int w;
          for (w = 1; x[u] + w < dims[u]; ++w)
          {
            if (workspace.mask[(x[u] + w) * dims[v] + x[v]])
              break;

            glm::ivec3 next_pos_u_slice = x;
            next_pos_u_slice[u] +=
                w; // Next voxel in u-direction in current slice

            // Use the new helper function
            TextureType check_type1 = getVoxelDataForMeshing(
                next_pos_u_slice[0], next_pos_u_slice[1], next_pos_u_slice[2]);
            TextureType check_type2 = getVoxelDataForMeshing(
                next_pos_u_slice[0] + q[0], next_pos_u_slice[1] + q[1],
                next_pos_u_slice[2] + q[2]);

            if (quad_normal_dir == q)
            { // Face is for a block like type1
              if (check_type1 != quad_type ||
                  !(check_type2 == AIR ||
                    (TextureManager::isTransparent(check_type2) &&
                     check_type1 != check_type2)))
                break;
            }
            else
            { // Face is for a block like type2
              if (check_type2 != quad_type ||
                  !(check_type1 == AIR ||
                    (TextureManager::isTransparent(check_type1) &&
                     check_type2 != check_type1)))
                break;
            }

            // Prevent merging across biome color boundaries
            if (isBiomeColoredType)
            {
              glm::ivec3 candidateVoxel = (quad_normal_dir == q)
                                              ? next_pos_u_slice
                                              : next_pos_u_slice + q;
              if (getCandidateBiomeColor(candidateVoxel) != originBiomeColor)
                break;
            }
          }

          // Calculate height (h) of the quad along dimension v
          int h;
          bool h_break = false;
          for (h = 1; x[v] + h < dims[v]; ++h)
          {
            for (int k = 0; k < w;
                 ++k)
            { // Check all cells in the current row of width w
              if (workspace.mask[(x[u] + k) * dims[v] + (x[v] + h)])
              {
                h_break = true;
                break;
              }

              glm::ivec3 next_pos_v_slice = x;
              next_pos_v_slice[u] += k;
              next_pos_v_slice[v] += h;

              // Use the new helper function
              TextureType check_type1 = getVoxelDataForMeshing(
                  next_pos_v_slice[0], next_pos_v_slice[1],
                  next_pos_v_slice[2]);
              TextureType check_type2 = getVoxelDataForMeshing(
                  next_pos_v_slice[0] + q[0], next_pos_v_slice[1] + q[1],
                  next_pos_v_slice[2] + q[2]);

              if (quad_normal_dir == q)
              {
                if (check_type1 != quad_type ||
                    !(check_type2 == AIR ||
                      (TextureManager::isTransparent(check_type2) &&
                       check_type1 != check_type2)))
                {
                  h_break = true;
                  break;
                }
              }
              else
              {
                if (check_type2 != quad_type ||
                    !(check_type1 == AIR ||
                      (TextureManager::isTransparent(check_type1) &&
                       check_type2 != check_type1)))
                {
                  h_break = true;
                  break;
                }
              }

              // Prevent merging across biome color boundaries
              if (isBiomeColoredType)
              {
                glm::ivec3 candidateVoxel = (quad_normal_dir == q)
                                                ? next_pos_v_slice
                                                : next_pos_v_slice + q;
                if (getCandidateBiomeColor(candidateVoxel) != originBiomeColor)
                {
                  h_break = true;
                  break;
                }
              }
            }
            if (h_break)
              break;
          }

          // Add quad to mesh
          glm::vec3
              s_coord_float; // Min corner of the quad in local chunk grid space
          s_coord_float[d] = static_cast<float>(
              x[d] + 1.0f); // Corrected: Face is always at x[d]+1
          s_coord_float[u] = static_cast<float>(x[u]);
          s_coord_float[v] = static_cast<float>(x[v]);

          glm::vec3 quad_width_vec = {0, 0, 0};
          quad_width_vec[u] = static_cast<float>(w);
          glm::vec3 quad_height_vec = {0, 0, 0};
          quad_height_vec[v] = static_cast<float>(h);

          glm::vec3 v0_local = s_coord_float;
          glm::vec3 v1_local = s_coord_float + quad_width_vec;
          glm::vec3 v2_local = s_coord_float + quad_width_vec + quad_height_vec;
          glm::vec3 v3_local = s_coord_float + quad_height_vec;

          // Texture coordinates for tiling
          // The greedy mesher uses u=(d+1)%3, v=(d+2)%3.
          // We need consistent texture orientation regardless of face:
          //   d=0 (±X faces): u=Y, v=Z → swap so tex_u=Z(h), tex_v=Y(w)
          //   d=1 (±Y faces): u=Z, v=X → swap so tex_u=X(h), tex_v=Z(w)
          //   d=2 (±Z faces): u=X, v=Y → already correct
          glm::vec2 tc[4];
          float tex_w = static_cast<float>(w);
          float tex_h = static_cast<float>(h);

          bool swapUV = (d == 0 || d == 1);
          float tc_u = swapUV ? tex_h : tex_w;
          float tc_v = swapUV ? tex_w : tex_h;

          if (swapUV)
          {
            // After swap: vertex 0→(0,0), 1→(0,tc_v), 2→(tc_u,tc_v), 3→(tc_u,0)
            tc[0] = {0.0f, 0.0f};
            tc[1] = {0.0f, tc_v};
            tc[2] = {tc_u, tc_v};
            tc[3] = {tc_u, 0.0f};
          }
          else
          {
            tc[0] = {0.0f, 0.0f};
            tc[1] = {tc_u, 0.0f};
            tc[2] = {tc_u, tc_v};
            tc[3] = {0.0f, tc_v};
          }

          // Biome tint: grass, all leaf types, water
          const bool needsBiomeColoring =
              (quad_type == GRASS_TOP || quad_type == GRASS_SIDE ||
               blockIsFoliage(quad_type) || quad_type == WATER);

          const TextureType faceTex = blockFaceTexture(quad_type, quad_normal_dir.y);
          const float texture_idx_val = static_cast<float>(faceTex);

          uint32_t vert_indices[4];
          glm::vec3 quad_vertices_world[4] = {
              this->position + v0_local, this->position + v1_local,
              this->position + v2_local, this->position + v3_local};

          int normalIdx = 0;
          if (quad_normal_dir.x > 0)
            normalIdx = 0;
          else if (quad_normal_dir.x < 0)
            normalIdx = 1;
          else if (quad_normal_dir.y > 0)
            normalIdx = 2;
          else if (quad_normal_dir.y < 0)
            normalIdx = 3;
          else if (quad_normal_dir.z > 0)
            normalIdx = 4;
          else if (quad_normal_dir.z < 0)
            normalIdx = 5;

          uint32_t packedData = (normalIdx & 0x7) |
                                ((static_cast<uint32_t>(texture_idx_val) & 0xFF) << 3) |
                                (needsBiomeColoring ? (1 << 11) : 0);

          // Look up precomputed biome color from per-column arrays
          uint32_t packedColor = 0;
          if (needsBiomeColoring)
          {
            int colIdx = quad_origin_voxel_coord[2] * CHUNK_SIZE + quad_origin_voxel_coord[0];
            if (quad_type == GRASS_TOP || quad_type == GRASS_SIDE)
              packedColor = biomeGrassColors[colIdx];
            else if (blockIsFoliage(quad_type))
              packedColor = biomeFoliageColors[colIdx];
            else if (quad_type == WATER)
              packedColor = WATER_COLOR;
          }

          auto calculateAO = [&](const glm::vec3 &localPos, int cornerIdx) -> uint32_t
          {
            int pd = (int)std::round(localPos[d]);
            int pu = (int)std::round(localPos[u]);
            int pv = (int)std::round(localPos[v]);

            int layerD = (quad_normal_dir[d] > 0) ? pd : pd - 1;

            auto isSolid = [&](int du, int dv)
            {
              return !TextureManager::isTransparent(getVoxelDataForMeshing(
                  (d == 0 ? layerD : (u == 0 ? pu + du : pv + du)),
                  (d == 1 ? layerD : (u == 1 ? pu + du : pv + du)),
                  (d == 2 ? layerD : (u == 2 ? pu + du : pv + du))));
            };

            bool q1 = isSolid(0, 0);
            bool q2 = isSolid(-1, 0);
            bool q3 = isSolid(-1, -1);
            bool q4 = isSolid(0, -1);

            bool s1, s2, c;
            if (cornerIdx == 0)
            {
              s1 = q2;
              s2 = q4;
              c = q3;
            }
            else if (cornerIdx == 1)
            {
              s1 = q1;
              s2 = q3;
              c = q4;
            }
            else if (cornerIdx == 2)
            {
              s1 = q2;
              s2 = q4;
              c = q1;
            }
            else
            {
              s1 = q1;
              s2 = q3;
              c = q2;
            }

            if (s1 && s2)
              return 0;
            return 3 - (s1 + s2 + c);
          };

          // Determine which mesh buffer this quad goes to
          bool isWater = (quad_type == WATER);
          auto &targetVertices = isWater ? waterVertices : vertices;
          auto &targetIndices = isWater ? waterIndices : indices;
          auto &targetIndexCounter = isWater ? waterIndexCounter : indexCounter;

          // Sample light from air cell in front of the face (Minecraft-style)
          uint8_t faceSky = 15;
          uint8_t faceBlock = 0;
          {
            glm::ivec3 solid = quad_origin_voxel_coord;
            // Face sits between solid and air along normal
            glm::ivec3 airCell = solid;
            if (quad_normal_dir.x > 0)
              airCell.x += 1;
            else if (quad_normal_dir.x < 0)
              ; // solid already on + side; air is solid
            else if (quad_normal_dir.y > 0)
              airCell.y += 1;
            else if (quad_normal_dir.y < 0)
              ;
            else if (quad_normal_dir.z > 0)
              airCell.z += 1;

            // Prefer the open side of the face
            glm::ivec3 sample = solid;
            sample.x += static_cast<int>(quad_normal_dir.x);
            sample.y += static_cast<int>(quad_normal_dir.y);
            sample.z += static_cast<int>(quad_normal_dir.z);
            if (sample.x >= 0 && sample.x < CHUNK_SIZE && sample.y >= 0 && sample.y < CHUNK_HEIGHT &&
                sample.z >= 0 && sample.z < CHUNK_SIZE)
            {
              const size_t li = static_cast<size_t>(sample.x + CHUNK_SIZE * (sample.y + CHUNK_HEIGHT * sample.z));
              faceSky = skyLight[li];
              faceBlock = blockLight[li];
            }
            else
            {
              faceSky = 12;
              faceBlock = 0;
            }
            // Emissive solid itself glows
            if (solid.x >= 0 && solid.x < CHUNK_SIZE && solid.y >= 0 && solid.y < CHUNK_HEIGHT &&
                solid.z >= 0 && solid.z < CHUNK_SIZE)
            {
              faceBlock = std::max(faceBlock, lighting::blockLightEmission(getVoxel(solid.x, solid.y, solid.z).type));
            }
          }
          const uint32_t lightBits = lighting::packLightBits(faceSky, faceBlock);

          for (int i = 0; i < 4; ++i)
          {
            Vertex vert;
            vert.position = quad_vertices_world[i];

            glm::vec3 localPos = vert.position - this->position;
            uint32_t ao = calculateAO(localPos, i);
            ++meshSample.data.aoVertices;

            vert.packedData = packedData | (ao << 12) | lightBits;
            vert.texCoord = tc[i];
            vert.packedBiomeColor = packedColor;

            // I: Direct push for both water and opaque — greedy quads never share vertices
            targetVertices.push_back(vert);
            vert_indices[i] = targetIndexCounter++;
          }

          // Winding order based on normal direction along the main axis 'd'
          if (quad_normal_dir[d] > 0)
          {
            targetIndices.push_back(vert_indices[0]);
            targetIndices.push_back(vert_indices[1]);
            targetIndices.push_back(vert_indices[2]);
            targetIndices.push_back(vert_indices[0]);
            targetIndices.push_back(vert_indices[2]);
            targetIndices.push_back(vert_indices[3]);
          }
          else
          {
            targetIndices.push_back(vert_indices[0]);
            targetIndices.push_back(vert_indices[2]);
            targetIndices.push_back(vert_indices[1]);
            targetIndices.push_back(vert_indices[0]);
            targetIndices.push_back(vert_indices[3]);
            targetIndices.push_back(vert_indices[2]);
          }

          // Mark processed cells in the mask
          for (int iw = 0; iw < w; ++iw)
          {
            for (int ih = 0; ih < h; ++ih)
            {
              workspace.mask[(x[u] + iw) * dims[v] + (x[v] + ih)] = 1;
            }
          }
        }
      }
    }
  }

  meshSample.data.opaqueVertices = vertices.size();
  meshSample.data.opaqueIndices = indices.size();
  meshSample.data.waterVertices = waterVertices.size();
  meshSample.data.waterIndices = waterIndices.size();
  meshNeedsUpdate = true; // Flag for GPU upload
  state = ChunkState::MESHED;
}

// K: Simplified LOD mesh — one top-face quad per non-empty XZ column.
// Max 256 opaque + 256 water quads vs thousands for a full greedy mesh.
void Chunk::generateLODMesh()
{
  // Declare MemoryPublication first so MeshSample is destroyed first.
  // Mesh timing must exclude telemetry ownership publication overhead.
  MemoryPublication memoryPublication{*this};
  telemetry::MeshSample meshSample(telemetry::Lod);
  m_isLODMesh = true;
  vertices.clear();
  indices.clear();
  waterVertices.clear();
  waterIndices.clear();

  uint32_t indexCounter = 0;
  uint32_t waterIndexCounter = 0;

  for (int cx = 0; cx < CHUNK_SIZE; ++cx)
  {
    for (int cz = 0; cz < CHUNK_SIZE; ++cz)
    {
      // Find topmost non-AIR voxel in this column
      int topY = -1;
      TextureType topType = AIR;
      for (int cy = CHUNK_HEIGHT - 1; cy >= 0; --cy)
      {
        TextureType t = static_cast<TextureType>(getVoxel(cx, cy, cz).type);
        if (t != AIR)
        {
          topY = cy;
          topType = t;
          break;
        }
      }
      if (topY < 0)
        continue; // Empty column

      bool isWater = (topType == WATER);

      // Remap block type to its top-face texture
      const TextureType texType = blockTopFace(topType);

      const bool needsBiomeColoring = (topType == GRASS_TOP || topType == GRASS_SIDE ||
                                       blockIsFoliage(topType) || topType == WATER);
      int colIdx = cz * CHUNK_SIZE + cx;
      uint32_t packedColor = 0;
      if (needsBiomeColoring)
      {
        if (topType == GRASS_TOP || topType == GRASS_SIDE)
          packedColor = biomeGrassColors[colIdx];
        else if (blockIsFoliage(topType))
          packedColor = biomeFoliageColors[colIdx];
        else
          packedColor = WATER_COLOR;
      }

      // normalIdx=2 (+Y), ao=3 (no occlusion — skip expensive AO for LOD)
      // Full sky light for distant LOD columns (block light 0)
      uint32_t packedData = (2u & 0x7u) |
                            ((static_cast<uint32_t>(texType) & 0xFFu) << 3) |
                            (needsBiomeColoring ? (1u << 11) : 0u) |
                            (3u << 12) |
                            lighting::packLightBits(15, 0);

      // Top face vertices in world space; axis mapping: d=1(Y), u=2(Z), v=0(X)
      float fy = float(topY + 1);
      float fx = float(cx);
      float fz = float(cz);

      Vertex v0, v1, v2, v3;
      v0.position = this->position + glm::vec3(fx, fy, fz);
      v1.position = this->position + glm::vec3(fx, fy, fz + 1.f);
      v2.position = this->position + glm::vec3(fx + 1.f, fy, fz + 1.f);
      v3.position = this->position + glm::vec3(fx + 1.f, fy, fz);

      // swapUV=true (d=1), w=1, h=1 -> tc_u=1, tc_v=1
      v0.texCoord = {0.f, 0.f};
      v1.texCoord = {0.f, 1.f};
      v2.texCoord = {1.f, 1.f};
      v3.texCoord = {1.f, 0.f};

      v0.packedData = v1.packedData = v2.packedData = v3.packedData = packedData;
      v0.packedBiomeColor = v1.packedBiomeColor =
          v2.packedBiomeColor = v3.packedBiomeColor = packedColor;

      auto &tVerts = isWater ? waterVertices : vertices;
      auto &tIndices = isWater ? waterIndices : indices;
      auto &cnt = isWater ? waterIndexCounter : indexCounter;

      uint32_t base = cnt;
      tVerts.push_back(v0);
      tVerts.push_back(v1);
      tVerts.push_back(v2);
      tVerts.push_back(v3);
      cnt += 4;

      // Winding for +Y normal (quad_normal_dir[d] > 0)
      tIndices.push_back(base + 0);
      tIndices.push_back(base + 1);
      tIndices.push_back(base + 2);
      tIndices.push_back(base + 0);
      tIndices.push_back(base + 2);
      tIndices.push_back(base + 3);
    }
  }

  meshSample.data.opaqueVertices = vertices.size();
  meshSample.data.opaqueIndices = indices.size();
  meshSample.data.waterVertices = waterVertices.size();
  meshSample.data.waterIndices = waterIndices.size();
  meshNeedsUpdate = true;
  state = ChunkState::MESHED;
}

void Chunk::releaseGPU()
{
  if (m_allocator == VK_NULL_HANDLE)
    return;
  destroyBuffer(m_allocator, vertexBuffer);
  destroyBuffer(m_allocator, indexBuffer);
  destroyBuffer(m_allocator, waterVertexBuffer);
  destroyBuffer(m_allocator, waterIndexBuffer);
  m_allocator = VK_NULL_HANDLE;
  opaqueIndexCount = 0;
  waterIndexCount = 0;
}

void Chunk::uploadToGPU(VmaAllocator allocator, ImmediateCommands &imm)
{
  MemoryPublication memoryPublication{*this};
  if (allocator == VK_NULL_HANDLE)
    throw std::runtime_error("Chunk::uploadToGPU: null allocator");

  // Bootstrap path: immediate destroy is OK (caller waited idle or nothing draws yet).
  if (m_allocator != VK_NULL_HANDLE)
    releaseGPU();
  m_allocator = allocator;

  opaqueIndexCount = static_cast<uint32_t>(indices.size());
  if (!vertices.empty() && opaqueIndexCount > 0)
  {
    const VkDeviceSize vSize = vertices.size() * sizeof(Vertex);
    const VkDeviceSize iSize = indices.size() * sizeof(uint32_t);
    vertexBuffer = createBuffer(allocator, vSize,
      VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
      VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
    trackMeshBuffer(vertexBuffer, telemetry::GpuOpaqueVertex);
    indexBuffer = createBuffer(allocator, iSize,
      VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
      VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
    trackMeshBuffer(indexBuffer, telemetry::GpuOpaqueIndex);
    uploadBuffer(allocator, imm, vertexBuffer, vertices.data(), vSize);
    uploadBuffer(allocator, imm, indexBuffer, indices.data(), iSize);
  }

  vertices.clear();
  indices.clear();

  waterIndexCount = static_cast<uint32_t>(waterIndices.size());
  if (!waterVertices.empty() && waterIndexCount > 0)
  {
    const VkDeviceSize vSize = waterVertices.size() * sizeof(Vertex);
    const VkDeviceSize iSize = waterIndices.size() * sizeof(uint32_t);
    waterVertexBuffer = createBuffer(allocator, vSize,
      VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
      VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
    trackMeshBuffer(waterVertexBuffer, telemetry::GpuWaterVertex);
    waterIndexBuffer = createBuffer(allocator, iSize,
      VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
      VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
    trackMeshBuffer(waterIndexBuffer, telemetry::GpuWaterIndex);
    uploadBuffer(allocator, imm, waterVertexBuffer, waterVertices.data(), vSize);
    uploadBuffer(allocator, imm, waterIndexBuffer, waterIndices.data(), iSize);
  }

  waterVertices.clear();
  waterIndices.clear();

  freeShellVoxels();
  meshNeedsUpdate = false;
}

bool Chunk::uploadToGPUAsync(VmaAllocator allocator, StagingRing &staging, VkCommandBuffer cmd,
                             GpuResourceRetire &retire)
{
  MemoryPublication memoryPublication{*this};
  if (allocator == VK_NULL_HANDLE || cmd == VK_NULL_HANDLE || !staging.isValid())
    return false;

  m_allocator = allocator;

  const uint32_t newOpaqueCount = static_cast<uint32_t>(indices.size());
  const uint32_t newWaterCount = static_cast<uint32_t>(waterIndices.size());

  AllocatedBuffer newV{}, newI{}, newWV{}, newWI{};
  std::vector<VkBufferCopy> copies;
  copies.reserve(4);
  struct CopyJob
  {
    VkBuffer dst;
    VkDeviceSize srcOffset;
    VkDeviceSize size;
  };
  std::vector<CopyJob> jobs;
  jobs.reserve(4);

  auto stageInto = [&](const void *data, VkDeviceSize size, AllocatedBuffer &dstBuf,
                       VkBufferUsageFlags usage, telemetry::Gauge kind) -> bool {
    if (!data || size == 0)
      return true;
    dstBuf = createBuffer(allocator, size,
                          VK_BUFFER_USAGE_TRANSFER_DST_BIT | usage,
                          VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
    trackMeshBuffer(dstBuf, kind);
    VkDeviceSize off = 0;
    void *ptr = nullptr;
    if (!staging.alloc(size, off, ptr))
    {
      destroyBuffer(allocator, dstBuf);
      return false;
    }
    std::memcpy(ptr, data, static_cast<size_t>(size));
    jobs.push_back({dstBuf.buffer, off, size});
    return true;
  };

  const bool needOpaque = !vertices.empty() && newOpaqueCount > 0;
  const bool needWater = !waterVertices.empty() && newWaterCount > 0;

  if (needOpaque)
  {
    const VkDeviceSize vSize = vertices.size() * sizeof(Vertex);
    const VkDeviceSize iSize = indices.size() * sizeof(uint32_t);
    if (!stageInto(vertices.data(), vSize, newV, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, telemetry::GpuOpaqueVertex) ||
        !stageInto(indices.data(), iSize, newI, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, telemetry::GpuOpaqueIndex))
    {
      destroyBuffer(allocator, newV);
      destroyBuffer(allocator, newI);
      telemetry::registry().add(telemetry::UploadDeferred);
      return false;
    }
  }

  if (needWater)
  {
    const VkDeviceSize vSize = waterVertices.size() * sizeof(Vertex);
    const VkDeviceSize iSize = waterIndices.size() * sizeof(uint32_t);
    if (!stageInto(waterVertices.data(), vSize, newWV, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, telemetry::GpuWaterVertex) ||
        !stageInto(waterIndices.data(), iSize, newWI, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, telemetry::GpuWaterIndex))
    {
      destroyBuffer(allocator, newV);
      destroyBuffer(allocator, newI);
      destroyBuffer(allocator, newWV);
      destroyBuffer(allocator, newWI);
      telemetry::registry().add(telemetry::UploadDeferred);
      return false;
    }
  }

  telemetry::registry().add(telemetry::UploadChunks);
  telemetry::registry().add(telemetry::UploadVertexBytes, newV.size + newWV.size);
  telemetry::registry().add(telemetry::UploadIndexBytes, newI.size + newWI.size);
  for (const auto &j : jobs)
  {
    VkBufferCopy copy{};
    copy.srcOffset = j.srcOffset;
    copy.dstOffset = 0;
    copy.size = j.size;
    vkCmdCopyBuffer(cmd, staging.buffer(), j.dst, 1, &copy);
  }

  // Retire previous GPU meshes (still referenced by in-flight frames).
  if (vertexBuffer.buffer != VK_NULL_HANDLE)
    retire.retireBuffer(vertexBuffer);
  if (indexBuffer.buffer != VK_NULL_HANDLE)
    retire.retireBuffer(indexBuffer);
  if (waterVertexBuffer.buffer != VK_NULL_HANDLE)
    retire.retireBuffer(waterVertexBuffer);
  if (waterIndexBuffer.buffer != VK_NULL_HANDLE)
    retire.retireBuffer(waterIndexBuffer);

  vertexBuffer = newV;
  indexBuffer = newI;
  waterVertexBuffer = newWV;
  waterIndexBuffer = newWI;
  opaqueIndexCount = needOpaque ? newOpaqueCount : 0;
  waterIndexCount = needWater ? newWaterCount : 0;

  vertices.clear();
  indices.clear();
  waterVertices.clear();
  waterIndices.clear();

  freeShellVoxels();
  meshNeedsUpdate = false;
  return true;
}

void Chunk::releaseGPUDeferred(GpuResourceRetire &retire)
{
  if (vertexBuffer.buffer != VK_NULL_HANDLE)
    retire.retireBuffer(vertexBuffer);
  if (indexBuffer.buffer != VK_NULL_HANDLE)
    retire.retireBuffer(indexBuffer);
  if (waterVertexBuffer.buffer != VK_NULL_HANDLE)
    retire.retireBuffer(waterVertexBuffer);
  if (waterIndexBuffer.buffer != VK_NULL_HANDLE)
    retire.retireBuffer(waterIndexBuffer);
  vertexBuffer = {};
  indexBuffer = {};
  waterVertexBuffer = {};
  waterIndexBuffer = {};
  m_allocator = VK_NULL_HANDLE;
  opaqueIndexCount = 0;
  waterIndexCount = 0;
}

uint32_t Chunk::draw(VkCommandBuffer cmd)
{
  if (opaqueIndexCount == 0 || vertexBuffer.buffer == VK_NULL_HANDLE)
    return 0;

  VkDeviceSize offset = 0;
  vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer.buffer, &offset);
  vkCmdBindIndexBuffer(cmd, indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
  telemetry::registry().add(telemetry::OpaqueDraws);
  vkCmdDrawIndexed(cmd, opaqueIndexCount, 1, 0, 0, 0);
  return opaqueIndexCount;
}

uint32_t Chunk::drawWater(VkCommandBuffer cmd)
{
  if (waterIndexCount == 0 || waterVertexBuffer.buffer == VK_NULL_HANDLE)
    return 0;

  VkDeviceSize offset = 0;
  vkCmdBindVertexBuffers(cmd, 0, 1, &waterVertexBuffer.buffer, &offset);
  vkCmdBindIndexBuffer(cmd, waterIndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
  telemetry::registry().add(telemetry::WaterDraws);
  vkCmdDrawIndexed(cmd, waterIndexCount, 1, 0, 0, 0);
  return waterIndexCount;
}

void Chunk::drawShadow(VkCommandBuffer cmd, unsigned cascade) const
{
  if (opaqueIndexCount == 0 || meshNeedsUpdate.load() || vertexBuffer.buffer == VK_NULL_HANDLE)
    return;

  VkDeviceSize offset = 0;
  vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer.buffer, &offset);
  vkCmdBindIndexBuffer(cmd, indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
  telemetry::registry().add(static_cast<telemetry::Event>(telemetry::Shadow0 + std::min(cascade, 2u)));
  vkCmdDrawIndexed(cmd, opaqueIndexCount, 1, 0, 0, 0);
}

void Chunk::freeShellVoxels()
{
  MemoryPublication memoryPublication{*this};
  // Keep capacity for remesh/edit — avoid realloc thrash.
  neighborShellVoxels.clear();
}

void Chunk::rebuildShellFromNeighbors(const Chunk *west, const Chunk *east,
                                      const Chunk *south, const Chunk *north)
{
  MemoryPublication memoryPublication{*this};
  neighborShellVoxels.assign(18 * (CHUNK_HEIGHT + 2) * 18, static_cast<uint8_t>(AIR));

  // West face  (local x = -1,  shell column x = 0):  neighbor's x = CHUNK_SIZE-1
  if (west)
    for (uint32_t y = 0; y < CHUNK_HEIGHT; ++y)
      for (uint32_t z = 0; z < CHUNK_SIZE; ++z)
        neighborShellVoxels[(y + 1) * 18 * 18 + (z + 1) * 18 + 0] =
            west->getVoxel(CHUNK_SIZE - 1, y, z).type;

  // East face  (local x = CHUNK_SIZE, shell column x = 17): neighbor's x = 0
  if (east)
    for (uint32_t y = 0; y < CHUNK_HEIGHT; ++y)
      for (uint32_t z = 0; z < CHUNK_SIZE; ++z)
        neighborShellVoxels[(y + 1) * 18 * 18 + (z + 1) * 18 + 17] =
            east->getVoxel(0, y, z).type;

  // South face (local z = -1,  shell row z = 0):  neighbor's z = CHUNK_SIZE-1
  if (south)
    for (uint32_t y = 0; y < CHUNK_HEIGHT; ++y)
      for (uint32_t x = 0; x < CHUNK_SIZE; ++x)
        neighborShellVoxels[(y + 1) * 18 * 18 + 0 * 18 + (x + 1)] =
            south->getVoxel(x, y, CHUNK_SIZE - 1).type;

  // North face (local z = CHUNK_SIZE, shell row z = 17): neighbor's z = 0
  if (north)
    for (uint32_t y = 0; y < CHUNK_HEIGHT; ++y)
      for (uint32_t x = 0; x < CHUNK_SIZE; ++x)
        neighborShellVoxels[(y + 1) * 18 * 18 + 17 * 18 + (x + 1)] =
            north->getVoxel(x, y, 0).type;
}

void Chunk::reset(const glm::vec3 &newPosition, ResetMode mode)
{
  MemoryPublication memoryPublication{*this};
  // Drop GPU meshes — next upload recreates VMA buffers.
  releaseGPU();
  opaqueIndexCount = 0;
  waterIndexCount = 0;

  // Reset identity
  position = newPosition;
  visible = false;
  state.store(ChunkState::UNLOADED);
  meshNeedsUpdate.store(true);
  m_isLODMesh = false;
  m_inTransit.store(false);
  m_activeIndex = SIZE_MAX;

  // Clear buffers but retain capacity for reuse (avoid reallocation)
  vertices.clear();
  indices.clear();
  waterVertices.clear();
  waterIndices.clear();

  // Generation resets caller-owned storage itself. Avoid another full-buffer
  // write on acquisition; retirement still leaves a completely empty chunk.
  if (mode == ResetMode::Full)
  {
    if (voxels.size() != CHUNK_VOLUME)
      voxels.resize(CHUNK_VOLUME);
    std::fill(voxels.begin(), voxels.end(), Voxel{static_cast<uint8_t>(AIR)});
  }

  activeVoxels.reset();

  // Clear shell — keep capacity for reuse
  neighborShellVoxels.clear();

  // Reset biome colors and per-column generation state
  biomeGrassColors.fill(0);
  biomeFoliageColors.fill(0);
  biomeTypes.fill(static_cast<BiomeType>(0));
  heightMap.fill(0);
}


void Chunk::publishCpuTelemetry()
{
  if (!telemetry::registry().enabled) return;
  const std::array<uint64_t, 13> current{
    voxels.capacity()*sizeof(Voxel), neighborShellVoxels.size(), neighborShellVoxels.capacity(),
    vertices.size()*sizeof(Vertex), vertices.capacity()*sizeof(Vertex),
    indices.size()*sizeof(uint32_t), indices.capacity()*sizeof(uint32_t),
    waterVertices.size()*sizeof(Vertex), waterVertices.capacity()*sizeof(Vertex),
    waterIndices.size()*sizeof(uint32_t), waterIndices.capacity()*sizeof(uint32_t),
    sizeof(biomeGrassColors)+sizeof(biomeFoliageColors)+sizeof(biomeTypes)+sizeof(heightMap), sizeof(activeVoxels)
  };
  telemetry::registry().replaceCpu(m_cpuTelemetry, current);
  m_cpuTelemetry = current;
}
