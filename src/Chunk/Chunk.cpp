#include "Chunk.hpp"
#include <algorithm>
#include <glm/gtx/hash.hpp>
#include <utils.hpp>
#include <vector>

struct MeshWorkspace {
  std::vector<uint8_t> mask;
  std::unordered_map<Vertex, uint32_t, VertexHasher> vertexMap;

  MeshWorkspace() {
    mask.reserve(CHUNK_HEIGHT * CHUNK_SIZE);
    vertexMap.reserve(4096);
  }
};

static thread_local MeshWorkspace s_meshWorkspace;

Chunk::Chunk(const glm::vec3 &position, ChunkState state)
    : position(position), visible(false), state(state), VAO(0), VBO(0), EBO(0),
      waterVAO(0), waterVBO(0), waterEBO(0),
      opaqueIndexCount(0), waterIndexCount(0),
      voxels(CHUNK_VOLUME),
      neighborShellVoxels(18 * (CHUNK_HEIGHT + 2) * 18,
                          static_cast<uint8_t>(AIR)),
      meshNeedsUpdate(true) {}

Chunk::Chunk(Chunk &&other) noexcept
    : position(std::move(other.position)), visible(other.visible),
      state(other.state), voxels(std::move(other.voxels)), VAO(other.VAO),
      VBO(other.VBO), EBO(other.EBO),
      waterVAO(other.waterVAO), waterVBO(other.waterVBO), waterEBO(other.waterEBO),
      opaqueIndexCount(other.opaqueIndexCount), waterIndexCount(other.waterIndexCount),
      meshNeedsUpdate(other.meshNeedsUpdate),
      activeVoxels(std::move(other.activeVoxels)),
      neighborShellVoxels(std::move(other.neighborShellVoxels)),
      biomeGrassColors(other.biomeGrassColors),
      biomeFoliageColors(other.biomeFoliageColors),
      vertices(std::move(other.vertices)), indices(std::move(other.indices)),
      waterVertices(std::move(other.waterVertices)), waterIndices(std::move(other.waterIndices)) {
  other.VAO = 0;
  other.VBO = 0;
  other.EBO = 0;
  other.waterVAO = 0;
  other.waterVBO = 0;
  other.waterEBO = 0;
  other.opaqueIndexCount = 0;
  other.waterIndexCount = 0;
}
Chunk &Chunk::operator=(Chunk &&other) noexcept {
  if (this != &other) {
    if (VAO != 0) glDeleteVertexArrays(1, &VAO);
    if (VBO != 0) glDeleteBuffers(1, &VBO);
    if (EBO != 0) glDeleteBuffers(1, &EBO);
    if (waterVAO != 0) glDeleteVertexArrays(1, &waterVAO);
    if (waterVBO != 0) glDeleteBuffers(1, &waterVBO);
    if (waterEBO != 0) glDeleteBuffers(1, &waterEBO);

    position = std::move(other.position);
    visible = other.visible;
    state = other.state;
    voxels = std::move(other.voxels);
    activeVoxels = std::move(other.activeVoxels);
    neighborShellVoxels = std::move(other.neighborShellVoxels);
    biomeGrassColors = other.biomeGrassColors;
    biomeFoliageColors = other.biomeFoliageColors;
    vertices = std::move(other.vertices);
    indices = std::move(other.indices);
    waterVertices = std::move(other.waterVertices);
    waterIndices = std::move(other.waterIndices);
    VAO = other.VAO;
    VBO = other.VBO;
    EBO = other.EBO;
    waterVAO = other.waterVAO;
    waterVBO = other.waterVBO;
    waterEBO = other.waterEBO;
    opaqueIndexCount = other.opaqueIndexCount;
    waterIndexCount = other.waterIndexCount;
    meshNeedsUpdate = other.meshNeedsUpdate;

    other.VAO = 0;
    other.VBO = 0;
    other.EBO = 0;
    other.waterVAO = 0;
    other.waterVBO = 0;
    other.waterEBO = 0;
  }
  return *this;
}

Chunk::~Chunk() {
  if (VAO != 0) {
    glDeleteVertexArrays(1, &VAO);
  }
  if (VBO != 0) {
    glDeleteBuffers(1, &VBO);
  }
  if (EBO != 0) {
    glDeleteBuffers(1, &EBO);
  }
  if (waterVAO != 0) {
    glDeleteVertexArrays(1, &waterVAO);
  }
  if (waterVBO != 0) {
    glDeleteBuffers(1, &waterVBO);
  }
  if (waterEBO != 0) {
    glDeleteBuffers(1, &waterEBO);
  }
}

const glm::vec3 &Chunk::getPosition() const { return position; }

size_t Chunk::getIndex(uint32_t x, uint32_t y, uint32_t z) const {
  return y * CHUNK_SIZE * CHUNK_SIZE + z * CHUNK_SIZE + x;
}

bool Chunk::isVisible() const { return visible; }

void Chunk::setVisible(bool visible) { this->visible = visible; }

void Chunk::setState(ChunkState state) {
  if (state == ChunkState::GENERATED || state == ChunkState::UNLOADED) {
    meshNeedsUpdate = true;
  }
  this->state = state;
}

ChunkState Chunk::getState() const { return state; }

Voxel &Chunk::getVoxel(uint32_t x, uint32_t y, uint32_t z) {
  return voxels[getIndex(x, y, z)];
}

const Voxel &Chunk::getVoxel(uint32_t x, uint32_t y, uint32_t z) const {
  return voxels[getIndex(x, y, z)];
}

void Chunk::setVoxel(int x, int y, int z, TextureType type) {
  if (x >= 0 && x < CHUNK_SIZE && y >= 0 && y < CHUNK_HEIGHT && z >= 0 &&
      z < CHUNK_SIZE) {
    size_t index = getIndex(x, y, z);
    voxels[index].type = static_cast<uint8_t>(type);
    if (type != AIR) {
      activeVoxels.set(index);
    } else {
      activeVoxels.reset(index);
    }
  } else if (x >= -1 && x <= CHUNK_SIZE && y >= -1 && y <= CHUNK_HEIGHT &&
             z >= -1 && z <= CHUNK_SIZE) {
    // This is a voxel on the 1-thick border, store it in neighborShellVoxels
    size_t shellIndex =
        (y + 1) * 18 * 18 + (z + 1) * 18 + (x + 1);
    neighborShellVoxels[shellIndex] = static_cast<uint8_t>(type);
  }
}

void Chunk::setVoxels(const std::vector<Voxel> &voxels) {
  this->voxels = voxels;
}

bool Chunk::deleteVoxel(const glm::vec3 &position) {
  int x = static_cast<int>(position.x - this->position.x);
  int y = static_cast<int>(position.y - this->position.y);
  int z = static_cast<int>(position.z - this->position.z);
  if (x < 0)
    x += CHUNK_SIZE;
  if (z < 0)
    z += CHUNK_SIZE;

  if (isVoxelActive(x, y, z)) {
    setVoxel(x, y, z, AIR);
    meshNeedsUpdate = true;
    state = ChunkState::GENERATED;
    return true;
  }
  return false;
}

bool Chunk::placeVoxel(const glm::vec3 &position, TextureType type) {
  int x = static_cast<int>(position.x - this->position.x);
  int y = static_cast<int>(position.y - this->position.y);
  int z = static_cast<int>(position.z - this->position.z);
  if (x < 0)
    x += CHUNK_SIZE;
  if (z < 0)
    z += CHUNK_SIZE;

  if (!isVoxelActive(x, y, z)) {
    setVoxel(x, y, z, type);
    meshNeedsUpdate = true;
    state = ChunkState::GENERATED;
    return true;
  }
  return false;
}

bool Chunk::isVoxelActive(int x, int y, int z) const {
  if (x >= 0 && x < CHUNK_SIZE && y >= 0 && y < CHUNK_HEIGHT && z >= 0 &&
      z < CHUNK_SIZE) {
    size_t index = getIndex(x, y, z);
    return activeVoxels.test(index);
  } else if (x >= -1 && x <= CHUNK_SIZE && y >= -1 && y <= CHUNK_HEIGHT &&
             z >= -1 && z <= CHUNK_SIZE) {
    size_t shellIndex =
        (y + 1) * 18 * 18 + (z + 1) * 18 + (x + 1);
    return neighborShellVoxels[shellIndex] != static_cast<uint8_t>(AIR);
  }
  return false; // Outside known boundaries
}

void Chunk::generateTerrain(TerrainGenerator &generator) {
  if (state != ChunkState::UNLOADED)
    return;

  std::fill(neighborShellVoxels.begin(), neighborShellVoxels.end(),
            static_cast<uint8_t>(AIR));

  // Ensure we use integer coordinates aligned with world grid
  int genX = static_cast<int>(std::round(position.x));
  int genZ = static_cast<int>(std::round(position.z));

  auto chunkData = generator.generateChunk(genX, genZ);
  setVoxels(chunkData.voxels);

  neighborShellVoxels = chunkData.borderVoxels;
  biomeGrassColors = chunkData.grassColors;
  biomeFoliageColors = chunkData.foliageColors;

  // Update bitset for active voxels
  activeVoxels.reset(); // Clear all bits first
  for (int i = 0; i < CHUNK_VOLUME; ++i) {
    if (this->voxels[i].type !=
        TextureType::AIR) { // Or your equivalent of an air block
      activeVoxels.set(i);
    }
  }

  state = ChunkState::GENERATED;
  meshNeedsUpdate = true;
}

void Chunk::generateMesh() {
  vertices.clear();
  indices.clear();
  waterVertices.clear();
  waterIndices.clear();

  auto &workspace = s_meshWorkspace;
  workspace.vertexMap.clear();
  uint32_t indexCounter = 0;
  uint32_t waterIndexCounter = 0;

  // New helper for greedy meshing that checks local voxels and the precomputed
  // neighbor shell
  auto getVoxelDataForMeshing = [&](int lx, int ly, int lz) -> TextureType {
    if (lx >= 0 && lx < CHUNK_SIZE && ly >= 0 && ly < CHUNK_HEIGHT && lz >= 0 &&
        lz < CHUNK_SIZE) {
      return static_cast<TextureType>(getVoxel(lx, ly, lz).type);
    }
    // Check the neighbor shell for out-of-bounds coordinates relevant to
    // meshing.
    if (lx >= -1 && lx <= CHUNK_SIZE && ly >= -1 && ly <= CHUNK_HEIGHT &&
        lz >= -1 && lz <= CHUNK_SIZE) {
      size_t shellIndex = (ly + 1) * 18 * 18 + (lz + 1) * 18 + (lx + 1);
      return static_cast<TextureType>(neighborShellVoxels[shellIndex]);
    }
    return AIR; // Default to AIR if not in chunk and not in precomputed shell
  };

  const int dims[] = {CHUNK_SIZE, CHUNK_HEIGHT, CHUNK_SIZE};

  // Iterate over dimensions (X, Y, Z)
  for (int d = 0; d < 3; ++d) {
    int u = (d + 1) % 3; // First axis in the plane of the face
    int v = (d + 2) % 3; // Second axis in the plane of the face

    glm::ivec3 x = {0, 0, 0}; // Current voxel coordinate during slice iteration
    glm::ivec3 q = {0, 0,
                    0}; // Normal direction for the face (points from x to x+q)
    q[d] = 1;

    // Ensure mask is large enough for the current slice
    if (workspace.mask.size() < static_cast<size_t>(dims[u] * dims[v])) {
      workspace.mask.resize(dims[u] * dims[v]);
    }

    // Iterate over each slice of the chunk along dimension 'd'
    // x[d] ranges from -1 (representing boundary before chunk) to dims[d]-1
    // (last voxel layer) A face exists between slice x[d] and slice x[d]+1
    for (x[d] = -1; x[d] < dims[d]; ++x[d]) {
      std::fill(workspace.mask.begin(), workspace.mask.begin() + (dims[u] * dims[v]), 0); // Reset mask for each slice

      // Iterate over the plane (u, v)
      for (x[u] = 0; x[u] < dims[u]; ++x[u]) {
        for (x[v] = 0; x[v] < dims[v]; ++x[v]) {

          if (workspace.mask[x[u] * dims[v] + x[v]]) {
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

          if (type1 != AIR &&
              (type2 == AIR ||
               (TextureManager::isTransparent(type2) && type1 != type2))) {
            // Face belongs to type1, pointing towards type2
            quad_type = type1;
            quad_normal_dir = q;
            quad_origin_voxel_coord = x;
          } else if (type2 != AIR &&
                     (type1 == AIR || (TextureManager::isTransparent(type1) &&
                                       type1 != type2))) {
            // Face belongs to type2, pointing towards type1
            quad_type = type2;
            quad_normal_dir = {-q[0], -q[1], -q[2]};
            quad_origin_voxel_coord = x + q;
          } else {
            continue; // No visible face here, or types are the same opaque.
          }

          if (quad_type == AIR)
            continue;

          // Check if this block type needs biome color — used to prevent
          // greedy merging across biome color boundaries
          bool isBiomeColoredType =
              (quad_type == GRASS_TOP || quad_type == GRASS_SIDE ||
               quad_type == OAK_LEAVES);

          // Precompute the origin quad's biome color for merge comparisons
          // (water uses a constant color, so it never splits)
          uint32_t originBiomeColor = 0;
          if (isBiomeColoredType) {
            int originColIdx = quad_origin_voxel_coord[2] * CHUNK_SIZE
                             + quad_origin_voxel_coord[0];
            originBiomeColor = (quad_type == OAK_LEAVES)
                ? biomeFoliageColors[originColIdx]
                : biomeGrassColors[originColIdx];
          }

          // Helper: get the packed biome color for a candidate voxel coordinate
          auto getCandidateBiomeColor = [&](const glm::ivec3 &coord) -> uint32_t {
            int colIdx = coord[2] * CHUNK_SIZE + coord[0];
            return (quad_type == OAK_LEAVES)
                ? biomeFoliageColors[colIdx]
                : biomeGrassColors[colIdx];
          };

          // Calculate width (w) of the quad along dimension u
          int w;
          for (w = 1; x[u] + w < dims[u]; ++w) {
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

            if (quad_normal_dir == q) { // Face is for a block like type1
              if (check_type1 != quad_type ||
                  !(check_type2 == AIR ||
                    (TextureManager::isTransparent(check_type2) &&
                     check_type1 != check_type2)))
                break;
            } else { // Face is for a block like type2
              if (check_type2 != quad_type ||
                  !(check_type1 == AIR ||
                    (TextureManager::isTransparent(check_type1) &&
                     check_type2 != check_type1)))
                break;
            }

            // Prevent merging across biome color boundaries
            if (isBiomeColoredType) {
              glm::ivec3 candidateVoxel = (quad_normal_dir == q)
                  ? next_pos_u_slice : next_pos_u_slice + q;
              if (getCandidateBiomeColor(candidateVoxel) != originBiomeColor)
                break;
            }
          }

          // Calculate height (h) of the quad along dimension v
          int h;
          bool h_break = false;
          for (h = 1; x[v] + h < dims[v]; ++h) {
            for (int k = 0; k < w;
                 ++k) { // Check all cells in the current row of width w
              if (workspace.mask[(x[u] + k) * dims[v] + (x[v] + h)]) {
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

              if (quad_normal_dir == q) {
                if (check_type1 != quad_type ||
                    !(check_type2 == AIR ||
                      (TextureManager::isTransparent(check_type2) &&
                       check_type1 != check_type2))) {
                  h_break = true;
                  break;
                }
              } else {
                if (check_type2 != quad_type ||
                    !(check_type1 == AIR ||
                      (TextureManager::isTransparent(check_type1) &&
                       check_type2 != check_type1))) {
                  h_break = true;
                  break;
                }
              }

              // Prevent merging across biome color boundaries
              if (isBiomeColoredType) {
                glm::ivec3 candidateVoxel = (quad_normal_dir == q)
                    ? next_pos_v_slice : next_pos_v_slice + q;
                if (getCandidateBiomeColor(candidateVoxel) != originBiomeColor) {
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

          if (swapUV) {
            // After swap: vertex 0→(0,0), 1→(0,tc_v), 2→(tc_u,tc_v), 3→(tc_u,0)
            tc[0] = {0.0f, 0.0f};
            tc[1] = {0.0f, tc_v};
            tc[2] = {tc_u, tc_v};
            tc[3] = {tc_u, 0.0f};
          } else {
            tc[0] = {0.0f, 0.0f};
            tc[1] = {tc_u, 0.0f};
            tc[2] = {tc_u, tc_v};
            tc[3] = {0.0f, tc_v};
          }

          // Determine if this block type needs biome-specific coloring
          bool needsBiomeColoring =
              (quad_type == GRASS_TOP || quad_type == GRASS_SIDE ||
               quad_type == OAK_LEAVES || quad_type == WATER);

          float texture_idx_val = static_cast<float>(quad_type);
          if (quad_type == GRASS_SIDE) {
            if (quad_normal_dir.y > 0.9f)
              texture_idx_val = static_cast<float>(GRASS_TOP);
            else if (quad_normal_dir.y < -0.9f)
              texture_idx_val = static_cast<float>(DIRT);
            // else remains GRASS_SIDE
          } else if (quad_type == OAK_LOG) {
            if (std::abs(quad_normal_dir.y) > 0.9f)
              texture_idx_val = static_cast<float>(OAK_LOG_TOP);
            // else remains OAK_LOG
          }

          uint32_t vert_indices[4];
          glm::vec3 quad_vertices_world[4] = {
              this->position + v0_local, this->position + v1_local,
              this->position + v2_local, this->position + v3_local};

          int normalIdx = 0;
          if (quad_normal_dir.x > 0) normalIdx = 0;
          else if (quad_normal_dir.x < 0) normalIdx = 1;
          else if (quad_normal_dir.y > 0) normalIdx = 2;
          else if (quad_normal_dir.y < 0) normalIdx = 3;
          else if (quad_normal_dir.z > 0) normalIdx = 4;
          else if (quad_normal_dir.z < 0) normalIdx = 5;

          uint32_t packedData = (normalIdx & 0x7) | 
                                ((static_cast<uint32_t>(texture_idx_val) & 0xFF) << 3) | 
                                (needsBiomeColoring ? (1 << 11) : 0);
          
          // Look up precomputed biome color from per-column arrays
          static constexpr uint32_t WATER_COLOR = 0xFF'E6804D; // RGBA for (0.3, 0.5, 0.9)
          uint32_t packedColor = 0;
          if (needsBiomeColoring) {
              int colIdx = quad_origin_voxel_coord[2] * CHUNK_SIZE + quad_origin_voxel_coord[0];
              if (quad_type == GRASS_TOP || quad_type == GRASS_SIDE)
                  packedColor = biomeGrassColors[colIdx];
              else if (quad_type == OAK_LEAVES)
                  packedColor = biomeFoliageColors[colIdx];
              else if (quad_type == WATER)
                  packedColor = WATER_COLOR;
          }

          auto calculateAO = [&](const glm::vec3& localPos, int cornerIdx) -> uint32_t {
              int pd = (int)std::round(localPos[d]);
              int pu = (int)std::round(localPos[u]);
              int pv = (int)std::round(localPos[v]);

              int layerD = (quad_normal_dir[d] > 0) ? pd : pd - 1;

              auto isSolid = [&](int du, int dv) {
                  return !TextureManager::isTransparent(getVoxelDataForMeshing(
                      (d==0 ? layerD : (u==0 ? pu+du : pv+du)),
                      (d==1 ? layerD : (u==1 ? pu+du : pv+du)),
                      (d==2 ? layerD : (u==2 ? pu+du : pv+du))
                  ));
              };

              bool q1 = isSolid(0, 0);
              bool q2 = isSolid(-1, 0);
              bool q3 = isSolid(-1, -1);
              bool q4 = isSolid(0, -1);

              bool s1, s2, c;
              if (cornerIdx == 0) { s1=q2; s2=q4; c=q3; } 
              else if (cornerIdx == 1) { s1=q1; s2=q3; c=q4; } 
              else if (cornerIdx == 2) { s1=q2; s2=q4; c=q1; } 
              else { s1=q1; s2=q3; c=q2; } 

              if (s1 && s2) return 0;
              return 3 - (s1 + s2 + c);
          };

          // Determine which mesh buffer this quad goes to
          bool isWater = (quad_type == WATER);
          auto &targetVertices = isWater ? waterVertices : vertices;
          auto &targetIndices = isWater ? waterIndices : indices;
          auto &targetIndexCounter = isWater ? waterIndexCounter : indexCounter;

          for (int i = 0; i < 4; ++i) {
            Vertex vert;
            vert.position = quad_vertices_world[i];
            
            glm::vec3 localPos = vert.position - this->position;
            uint32_t ao = calculateAO(localPos, i);

            vert.packedData = packedData | (ao << 12);
            vert.texCoord = tc[i];
            vert.packedBiomeColor = packedColor;

            // For water, don't deduplicate with the opaque vertex map
            // Just add directly to keep it simple and avoid cross-buffer refs
            if (isWater) {
              targetVertices.push_back(vert);
              vert_indices[i] = targetIndexCounter++;
            } else {
              auto it = workspace.vertexMap.find(vert);
              if (it != workspace.vertexMap.end()) {
                vert_indices[i] = it->second;
              } else {
                targetVertices.push_back(vert);
                vert_indices[i] = targetIndexCounter;
                workspace.vertexMap[vert] = targetIndexCounter++;
              }
            }
          }

          // Winding order based on normal direction along the main axis 'd'
          if (quad_normal_dir[d] > 0) {
            targetIndices.push_back(vert_indices[0]);
            targetIndices.push_back(vert_indices[1]);
            targetIndices.push_back(vert_indices[2]);
            targetIndices.push_back(vert_indices[0]);
            targetIndices.push_back(vert_indices[2]);
            targetIndices.push_back(vert_indices[3]);
          } else {
            targetIndices.push_back(vert_indices[0]);
            targetIndices.push_back(vert_indices[2]);
            targetIndices.push_back(vert_indices[1]);
            targetIndices.push_back(vert_indices[0]);
            targetIndices.push_back(vert_indices[3]);
            targetIndices.push_back(vert_indices[2]);
          }

          // Mark processed cells in the mask
          for (int iw = 0; iw < w; ++iw) {
            for (int ih = 0; ih < h; ++ih) {
              workspace.mask[(x[u] + iw) * dims[v] + (x[v] + ih)] = 1;
            }
          }
        }
      }
    }
  }

  meshNeedsUpdate = true; // Flag for GPU upload
  state = ChunkState::MESHED;
}

// P5: Shared vertex attribute layout — avoids copy-paste divergence
static void configureVertexAttributes() {
  // Position (location = 0)
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                        (void *)offsetof(Vertex, position));
  // Packed Data (location = 1)
  glEnableVertexAttribArray(1);
  glVertexAttribIPointer(1, 1, GL_UNSIGNED_INT, sizeof(Vertex),
                         (void *)offsetof(Vertex, packedData));
  // Texture coordinates (location = 2)
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                        (void *)offsetof(Vertex, texCoord));
  // Packed Biome Color (location = 3)
  glEnableVertexAttribArray(3);
  glVertexAttribIPointer(3, 1, GL_UNSIGNED_INT, sizeof(Vertex),
                         (void *)offsetof(Vertex, packedBiomeColor));
}

void Chunk::uploadMeshToGPU() {
  // --- Opaque mesh ---
  if (VAO == 0) glGenVertexArrays(1, &VAO);
  if (VBO == 0) glGenBuffers(1, &VBO);
  if (EBO == 0) glGenBuffers(1, &EBO);

  opaqueIndexCount = static_cast<uint32_t>(indices.size());

  glBindVertexArray(VAO);
  glBindBuffer(GL_ARRAY_BUFFER, VBO);
  glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(Vertex),
               vertices.data(), GL_STATIC_DRAW);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(uint16_t),
               indices.data(), GL_STATIC_DRAW);
  configureVertexAttributes();
  glBindVertexArray(0);

  // P2: Free CPU-side data after GPU upload
  vertices = {};
  indices = {};

  // --- Water mesh ---
  waterIndexCount = static_cast<uint32_t>(waterIndices.size());

  if (waterIndexCount > 0) {
    if (waterVAO == 0) glGenVertexArrays(1, &waterVAO);
    if (waterVBO == 0) glGenBuffers(1, &waterVBO);
    if (waterEBO == 0) glGenBuffers(1, &waterEBO);

    glBindVertexArray(waterVAO);
    glBindBuffer(GL_ARRAY_BUFFER, waterVBO);
    glBufferData(GL_ARRAY_BUFFER, waterVertices.size() * sizeof(Vertex),
                 waterVertices.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, waterEBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, waterIndices.size() * sizeof(uint16_t),
                 waterIndices.data(), GL_STATIC_DRAW);
    configureVertexAttributes();
    glBindVertexArray(0);
  }

  // P2: Free CPU-side water data after GPU upload
  waterVertices = {};
  waterIndices = {};

  meshNeedsUpdate = false;
}

// P1: draw() is now minimal — all shared uniforms set once in drawVisibleChunks
uint32_t Chunk::draw() {
  if (meshNeedsUpdate)
    uploadMeshToGPU();

  if (opaqueIndexCount == 0)
    return 0;

  glBindVertexArray(VAO);
  glDrawElements(GL_TRIANGLES, opaqueIndexCount, GL_UNSIGNED_SHORT, 0);
  glBindVertexArray(0);

  return opaqueIndexCount;
}

uint32_t Chunk::drawWater() {
  if (meshNeedsUpdate)
    uploadMeshToGPU();

  if (waterIndexCount == 0)
    return 0;

  glBindVertexArray(waterVAO);
  glDrawElements(GL_TRIANGLES, waterIndexCount, GL_UNSIGNED_SHORT, 0);
  glBindVertexArray(0);

  return waterIndexCount;
}

void Chunk::drawShadow(const Shader &shader) const {
  if (indices.empty() || meshNeedsUpdate)
    return;

  shader.setMat4("model", glm::mat4(1.0f));
  glBindVertexArray(VAO);
  glDrawElements(GL_TRIANGLES, indices.size(), GL_UNSIGNED_SHORT, 0);
  glBindVertexArray(0);
}
