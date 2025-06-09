#include "Chunk.hpp"
#include <vector>
#include <algorithm>
#include <glm/gtx/hash.hpp>

Chunk::Chunk(const glm::vec3 &position, ChunkState state)
	: position(position), visible(false), state(state), VAO(0), VBO(0), EBO(0), meshNeedsUpdate(true)
{
}

Chunk::Chunk(Chunk &&other) noexcept
	: position(std::move(other.position)), visible(other.visible), state(other.state), voxels(std::move(other.voxels)), VAO(other.VAO), VBO(other.VBO), EBO(other.EBO), meshNeedsUpdate(other.meshNeedsUpdate),
	  activeVoxels(std::move(other.activeVoxels)), neighborShellVoxels(std::move(other.neighborShellVoxels)), vertices(std::move(other.vertices)), indices(std::move(other.indices))
{
}
Chunk &Chunk::operator=(Chunk &&other) noexcept
{
	if (this != &other)
	{
		position = std::move(other.position);
		visible = other.visible;
		state = other.state;
		voxels = std::move(other.voxels);
		activeVoxels = std::move(other.activeVoxels);
		neighborShellVoxels = std::move(other.neighborShellVoxels);
		vertices = std::move(other.vertices);
		indices = std::move(other.indices);
		VAO = other.VAO;
		VBO = other.VBO;
		EBO = other.EBO;
		meshNeedsUpdate = other.meshNeedsUpdate;
	}
	return *this;
}

Chunk::~Chunk()
{
	if (VAO != 0)
	{
		glDeleteVertexArrays(1, &VAO);
	}
	if (VBO != 0)
	{
		glDeleteBuffers(1, &VBO);
	}
	if (EBO != 0)
	{
		glDeleteBuffers(1, &EBO);
	}
}

const glm::vec3 &Chunk::getPosition() const
{
	return position;
}

size_t Chunk::getIndex(uint32_t x, uint32_t y, uint32_t z) const
{
	return x + SIZE * (z + SIZE * y);
}

bool Chunk::isVisible() const
{
	return visible;
}

void Chunk::setVisible(bool visible)
{
	this->visible = visible;
}

void Chunk::setState(ChunkState state)
{
	if (state == ChunkState::GENERATED || state == ChunkState::UNLOADED)
	{
		meshNeedsUpdate = true;
	}
	this->state = state;
}

ChunkState Chunk::getState() const
{
	return state;
}

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
	// The logic for neighboursActiveMap has been removed as per the new approach.
	// Boundary voxels are now handled by the extended voxel generation in generateChunk
	// and accessed via neighborShellVoxels during meshing if they fall outside the -1 to SIZE range.

	if (x >= 0 && x < SIZE && y >= 0 && y < HEIGHT && z >= 0 && z < SIZE)
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
	else if ((x == -1 || x == SIZE || z == -1 || z == SIZE) && (y >= 0 && y < HEIGHT))
	{
		// This is a voxel on the 1-thick border, store it in neighborShellVoxels
		// The coordinates are kept local to the chunk's frame of reference (e.g., -1, y, z)
		if (type != AIR)
		{
			neighborShellVoxels[glm::ivec3(x, y, z)] = type;
		}
		else
		{
			neighborShellVoxels.erase(glm::ivec3(x, y, z));
		}
	}
	// else: Voxel is outside the -1 to SIZE boundary, ignore for now or handle as error.
	// This case should ideally not be hit by generateTerrainColumn if loops are correct.
}

bool Chunk::deleteVoxel(const glm::vec3 &position)
{
	int x = static_cast<int>(position.x - this->position.x);
	int y = static_cast<int>(position.y - this->position.y);
	int z = static_cast<int>(position.z - this->position.z);
	if (x < 0)
		x += SIZE;
	if (z < 0)
		z += SIZE;

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
		x += SIZE;
	if (z < 0)
		z += SIZE;

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
	if (x >= 0 && x < SIZE && y >= 0 && y < HEIGHT && z >= 0 && z < SIZE)
	{
		size_t index = getIndex(x, y, z);
		return activeVoxels.test(index);
	}
	else if ((x == -1 || x == SIZE || z == -1 || z == SIZE) && (y >= 0 && y < HEIGHT))
	{
		return neighborShellVoxels.count(glm::ivec3(x, y, z)) > 0;
	}
	return false; // Outside known boundaries
}

bool Chunk::isVoxelActiveGlobalPos(int x, int y, int z) const
{
	int localX = x - static_cast<int>(position.x);
	int localY = y - static_cast<int>(position.y);
	int localZ = z - static_cast<int>(position.z);

	return isVoxelActive(localX, localY, localZ);
}

void Chunk::generateVoxels(siv::PerlinNoise *noise)
{
	if (state != ChunkState::UNLOADED)
		return;

	neighborShellVoxels.clear(); // Clear previous neighbor data
	generateChunk(noise);		 // This generates the main chunk voxels AND the 1-voxel border

	state = ChunkState::GENERATED;
}

void Chunk::generateChunk(siv::PerlinNoise *noise)
{
	static BiomeManager biomeManager; // Static to avoid recreation
	const float blendRange = 0.05f;

	// Loop from -1 to SIZE (inclusive) for x and z to generate the 1-voxel thick border
	// around the main chunk area (0 to SIZE-1).
	for (int x_local = -1; x_local <= SIZE; x_local++)
	{
		for (int z_local = -1; z_local <= SIZE; z_local++)
		{
			// Skip corners of the -1 to SIZE extended area, as they are not direct face neighbors.
			// We only need face-adjacent voxels for meshing continuity.
			if ((x_local == -1 && z_local == -1) || (x_local == -1 && z_local == SIZE) ||
				(x_local == SIZE && z_local == -1) || (x_local == SIZE && z_local == SIZE))
			{
				continue;
			}

			// Get absolute world coordinates
			int worldX = static_cast<int>(position.x) + x_local;
			int worldZ = static_cast<int>(position.z) + z_local;

			// Use a smaller scale for biome regions to create more gradual transitions
			float biomeNoiseValue = noise->noise2D_01(
				static_cast<float>(worldX) / 128.0f, // Reduced scale for smoother biome transitions
				static_cast<float>(worldZ) / 128.0f);

			// Get biome type with blending
			BiomeType biomeType = biomeManager.getBiomeTypeAt(worldX, worldZ, noise);

			// Get terrain height with blending
			float terrainHeightFloat = biomeManager.getTerrainHeightAt(worldX, worldZ, biomeType, noise);

			// Add some noise to the terrain height for more natural variation
			float heightNoise = noise->noise2D_01(
				static_cast<float>(worldX) / 32.0f,
				static_cast<float>(worldZ) / 32.0f) * 2.0f - 1.0f;
			terrainHeightFloat += heightNoise;

			int terrainHeight = static_cast<int>(std::round(terrainHeightFloat));

			// Generate the actual column of voxels
			generateTerrainColumn(x_local, z_local, terrainHeight, biomeNoiseValue, noise);
		}
	}
}

void Chunk::generateTerrainColumn(int x, int z, int terrainHeight, float biomeNoise, siv::PerlinNoise *noise)
{
	static BiomeManager biomeManager;

	int worldX = static_cast<int>(position.x) + x;
	int worldZ = static_cast<int>(position.z) + z;
	BiomeType biomeType = biomeManager.getBiomeTypeAt(worldX, worldZ, noise);
	const BiomeParameters &biomeParams = biomeManager.getBiomeParameters(biomeType);

	for (int y = 0; y < Chunk::HEIGHT; y++)
	{
		if (y < terrainHeight)
		{
			if (y >= terrainHeight - 1)
			{
				setVoxel(x, y, z, biomeParams.surfaceBlock);
			}
			else if (y >= terrainHeight - 3)
			{
				setVoxel(x, y, z, biomeParams.subSurfaceBlock);
			}
			else
			{
				setVoxel(x, y, z, STONE);
			}
		}
		else if (y <= Chunk::WATER_LEVEL)
		{
			setVoxel(x, y, z, WATER);
		}
		else
		{
			setVoxel(x, y, z, AIR);
		}
	}

	if (biomeType == BIOME_FOREST)
	{
		float treeNoise = noise->noise2D_01(worldX * 1.5f, worldZ * 1.5f);
		if (treeNoise > 0.75f)
		{
			generateTree(x, z, terrainHeight);
		}
	}
}

void Chunk::generateFeatures(int x, int z, int terrainHeight, int worldX, int worldZ, float biomeNoise, siv::PerlinNoise *noise)
{
	for (int y = 0; y < Chunk::HEIGHT; y++)
	{
		if (y >= terrainHeight || y == 0)
			continue;

		// Generate caves
		float caveNoise = noise->octave3D_01(
			static_cast<float>(worldX) / 32.0f,
			static_cast<float>(y) / 32.0f,
			static_cast<float>(worldZ) / 32.0f,
			3, 0.5f);

		if (caveNoise < 0.25f)
		{
			setVoxel(x, y, z, AIR);
			continue; // Skip mineral generation if we've carved out a cave
		}

		// Generate minerals based on depth
		if (y < terrainHeight - 5)
		{
			float veinNoise = noise->noise3D_01(
				static_cast<float>(worldX) / 8.0f,
				static_cast<float>(y) / 8.0f,
				static_cast<float>(worldZ) / 8.0f);

			float oreTypeNoise = noise->noise3D_01(
				static_cast<float>(worldX) / 24.0f,
				static_cast<float>(y) / 24.0f,
				static_cast<float>(worldZ) / 24.0f);

			if (veinNoise > 0.8f)
			{
				if (y < 16)
				{
					if (oreTypeNoise < 0.2f)
						setVoxel(x, y, z, DIAMOND_ORE);
					else if (oreTypeNoise < 0.4f)
						setVoxel(x, y, z, GOLD_ORE);
					else if (oreTypeNoise < 0.6f)
						setVoxel(x, y, z, REDSTONE_ORE);
					else if (oreTypeNoise < 0.8f)
						setVoxel(x, y, z, LAPIS_ORE);
					else
						setVoxel(x, y, z, IRON_ORE);
				}
				else if (y < 30)
				{
					if (oreTypeNoise < 0.1f)
						setVoxel(x, y, z, DIAMOND_ORE);
					else if (oreTypeNoise < 0.3f)
						setVoxel(x, y, z, GOLD_ORE);
					else if (oreTypeNoise < 0.5f)
						setVoxel(x, y, z, REDSTONE_ORE);
					else if (oreTypeNoise < 0.65f)
						setVoxel(x, y, z, LAPIS_ORE);
					else if (oreTypeNoise < 0.85f)
						setVoxel(x, y, z, IRON_ORE);
					else
						setVoxel(x, y, z, COAL_ORE);
				}
				else if (y < 50)
				{
					if (oreTypeNoise < 0.05f)
						setVoxel(x, y, z, EMERALD_ORE);

					else if (oreTypeNoise < 0.25f)
						setVoxel(x, y, z, GOLD_ORE);

					else if (oreTypeNoise < 0.35f)
						setVoxel(x, y, z, REDSTONE_ORE);

					else if (oreTypeNoise < 0.45f)
						setVoxel(x, y, z, LAPIS_ORE);

					else if (oreTypeNoise < 0.7f)
						setVoxel(x, y, z, IRON_ORE);

					else if (oreTypeNoise < 0.85f)
						setVoxel(x, y, z, COPPER_ORE);

					else
						setVoxel(x, y, z, COAL_ORE);
				}
				else
				{
					if (oreTypeNoise < 0.1f)
						setVoxel(x, y, z, EMERALD_ORE);

					else if (oreTypeNoise < 0.25f)
						setVoxel(x, y, z, COPPER_ORE);

					else if (oreTypeNoise < 0.45f)
						setVoxel(x, y, z, IRON_ORE);

					else
						setVoxel(x, y, z, COAL_ORE);
				}
			}
		}
	}
}

void Chunk::generateTree(int x, int z, int terrainHeight)
{
	// Only place trees that have their trunk at least 3 blocks from the edge
	if (x < 3 || x >= SIZE - 3 || z < 3 || z >= SIZE - 3)
	{
		return; // Skip trees too close to the edge
	}

	// Tree height: random between 4 and 6 blocks
	int treeHeight = 4 + rand() % 3;

	// Generate the trunk
	for (int y = terrainHeight; y < terrainHeight + treeHeight; y++)
	{
		if (y < HEIGHT)
		{
			setVoxel(x, y, z, OAK_LOG);
		}
	}

	// Generate leaves - roughly a spherical/balloon shape
	int leafStartY = terrainHeight + treeHeight - 3; // Start leaves 3 blocks from the top

	// Generate leaves in a roughly spherical pattern
	for (int offsetY = 0; offsetY <= 3; offsetY++)
	{
		int radius = (offsetY == 0 || offsetY == 3) ? 1 : 2; // Smaller radius at bottom and top

		for (int offsetX = -radius; offsetX <= radius; offsetX++)
		{
			for (int offsetZ = -radius; offsetZ <= radius; offsetZ++)
			{

				// Skip corners for a more rounded look on the bigger layers
				if (radius == 2 && abs(offsetX) == 2 && abs(offsetZ) == 2)
				{
					continue;
				}

				int leafX = x + offsetX;
				int leafY = leafStartY + offsetY;
				int leafZ = z + offsetZ;

				// Only place leaves that are inside this chunk
				if (leafX >= 0 && leafX < SIZE && leafY >= 0 && leafY < HEIGHT && leafZ >= 0 && leafZ < SIZE)
				{
					setVoxel(leafX, leafY, leafZ, OAK_LEAVES);
				}
			}
		}
	}
}

void Chunk::generateMesh(glm::vec3 playerPos, siv::PerlinNoise *noise)
{
	static BiomeManager biomeManager; // Keep static as in original

	vertices.clear();
	indices.clear();
	// Assuming VertexHasher is defined for Vertex struct
	std::unordered_map<Vertex, uint32_t, VertexHasher> vertexMap;
	vertexMap.clear();
	uint32_t indexCounter = 0;

	// Helper to get voxel type, handling out-of-bounds as AIR
	auto getVoxelTypeSafe = [&](int vx, int vy, int vz) -> TextureType
	{
		if (vx < 0 || vx >= SIZE || vy < 0 || vy >= HEIGHT || vz < 0 || vz >= SIZE)
		{
			return AIR;
		}
		return static_cast<TextureType>(getVoxel(vx, vy, vz).type);
	};

	// New helper for greedy meshing that checks local voxels and the precomputed neighbor shell
	auto getVoxelDataForMeshing = [&](int lx, int ly, int lz) -> TextureType
	{
		if (lx >= 0 && lx < SIZE && ly >= 0 && ly < HEIGHT && lz >= 0 && lz < SIZE)
		{
			return static_cast<TextureType>(getVoxel(lx, ly, lz).type);
		}
		// Check the neighbor shell for out-of-bounds coordinates relevant to meshing.
		// These are coordinates like -1, y, z or SIZE, y, z or x, y, -1 or x, y, SIZE.
		auto it = neighborShellVoxels.find(glm::ivec3(lx, ly, lz));
		if (it != neighborShellVoxels.end())
		{
			return it->second;
		}
		return AIR; // Default to AIR if not in chunk and not in precomputed shell
	};

	const int dims[] = {SIZE, HEIGHT, SIZE};

	// Iterate over dimensions (X, Y, Z)
	for (int d = 0; d < 3; ++d)
	{
		int u = (d + 1) % 3; // First axis in the plane of the face
		int v = (d + 2) % 3; // Second axis in the plane of the face

		glm::ivec3 x = {0, 0, 0}; // Current voxel coordinate during slice iteration
		glm::ivec3 q = {0, 0, 0}; // Normal direction for the face (points from x to x+q)
		q[d] = 1;

		// Mask for the current slice to mark processed parts of quads
		std::vector<bool> mask(dims[u] * dims[v]);

		// Iterate over each slice of the chunk along dimension 'd'
		// x[d] ranges from -1 (representing boundary before chunk) to dims[d]-1 (last voxel layer)
		// A face exists between slice x[d] and slice x[d]+1
		for (x[d] = -1; x[d] < dims[d]; ++x[d])
		{
			std::fill(mask.begin(), mask.end(), false); // Reset mask for each slice

			// Iterate over the plane (u, v)
			for (x[u] = 0; x[u] < dims[u]; ++x[u])
			{
				for (x[v] = 0; x[v] < dims[v]; ++x[v])
				{

					if (mask[x[u] * dims[v] + x[v]])
					{
						continue; // Already processed this part of the slice
					}

					// Get types of voxels on either side of the potential face
					// Voxel at x is on one side, voxel at x+q is on the other.
					// Use the new helper function that checks the neighbor shell
					TextureType type1 = getVoxelDataForMeshing(x[0], x[1], x[2]);
					TextureType type2 = getVoxelDataForMeshing(x[0] + q[0], x[1] + q[1], x[2] + q[2]);

					TextureType quad_type = AIR;
					glm::ivec3 quad_normal_dir = {0, 0, 0};
					glm::ivec3 quad_origin_voxel_coord = {0, 0, 0}; // Min corner of the voxel this quad's face belongs to

					if (type1 != AIR && (type2 == AIR || (TextureManager::isTransparent(type2) && type1 != type2)))
					{
						// Face belongs to type1, pointing towards type2
						quad_type = type1;
						quad_normal_dir = q;
						quad_origin_voxel_coord = x;
					}
					else if (type2 != AIR && (type1 == AIR || (TextureManager::isTransparent(type1) && type1 != type2)))
					{
						// Face belongs to type2, pointing towards type1
						quad_type = type2;
						quad_normal_dir = {-q[0], -q[1], -q[2]};
						quad_origin_voxel_coord = x + q;
					}
					else
					{
						continue; // No visible face here, or types are the same opaque.
					}

					if (quad_type == AIR)
						continue;

					// Calculate width (w) of the quad along dimension u
					int w;
					for (w = 1; x[u] + w < dims[u]; ++w)
					{
						if (mask[(x[u] + w) * dims[v] + x[v]])
							break;

						glm::ivec3 next_pos_u_slice = x;
						next_pos_u_slice[u] += w; // Next voxel in u-direction in current slice part

						// Use the new helper function
						TextureType check_type1 = getVoxelDataForMeshing(next_pos_u_slice[0], next_pos_u_slice[1], next_pos_u_slice[2]);
						TextureType check_type2 = getVoxelDataForMeshing(next_pos_u_slice[0] + q[0], next_pos_u_slice[1] + q[1], next_pos_u_slice[2] + q[2]);

						if (quad_normal_dir == q)
						{ // Face is for a block like type1
							if (check_type1 != quad_type || !(check_type2 == AIR || (TextureManager::isTransparent(check_type2) && check_type1 != check_type2)))
								break;
						}
						else
						{ // Face is for a block like type2
							if (check_type2 != quad_type || !(check_type1 == AIR || (TextureManager::isTransparent(check_type1) && check_type2 != check_type1)))
								break;
						}
					}

					// Calculate height (h) of the quad along dimension v
					int h;
					bool h_break = false;
					for (h = 1; x[v] + h < dims[v]; ++h)
					{
						for (int k = 0; k < w; ++k)
						{ // Check all cells in the current row of width w
							if (mask[(x[u] + k) * dims[v] + (x[v] + h)])
							{
								h_break = true;
								break;
							}

							glm::ivec3 next_pos_v_slice = x;
							next_pos_v_slice[u] += k;
							next_pos_v_slice[v] += h;

							// Use the new helper function
							TextureType check_type1 = getVoxelDataForMeshing(next_pos_v_slice[0], next_pos_v_slice[1], next_pos_v_slice[2]);
							TextureType check_type2 = getVoxelDataForMeshing(next_pos_v_slice[0] + q[0], next_pos_v_slice[1] + q[1], next_pos_v_slice[2] + q[2]);

							if (quad_normal_dir == q)
							{
								if (check_type1 != quad_type || !(check_type2 == AIR || (TextureManager::isTransparent(check_type2) && check_type1 != check_type2)))
								{
									h_break = true;
									break;
								}
							}
							else
							{
								if (check_type2 != quad_type || !(check_type1 == AIR || (TextureManager::isTransparent(check_type1) && check_type2 != check_type1)))
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
					glm::vec3 s_coord_float;							// Min corner of the quad in local chunk grid space
					s_coord_float[d] = static_cast<float>(x[d] + 1.0f); // Corrected: Face is always at x[d]+1
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
					glm::vec2 tc[4];
					float tex_w = static_cast<float>(w);
					float tex_h = static_cast<float>(h);

					tc[0] = {0.0f, 0.0f};
					tc[1] = {tex_w, 0.0f};
					tc[2] = {tex_w, tex_h};
					tc[3] = {0.0f, tex_h};

					bool needsBiomeColoring = (quad_type == GRASS_TOP || quad_type == GRASS_SIDE || quad_type == OAK_LEAVES || quad_type == WATER);
					glm::vec3 biomeColorVal(0.0f);
					if (needsBiomeColoring)
					{
						// Use the quad_origin_voxel_coord for biome lookup (center of it)
						BiomeType biome = biomeManager.getBiomeTypeAt(
							this->position.x + quad_origin_voxel_coord[0] + 0.5f,
							this->position.z + quad_origin_voxel_coord[2] + 0.5f,
							noise);
						const BiomeParameters &biomeParams = biomeManager.getBiomeParameters(biome);

						switch (biome)
						{
						case BIOME_DESERT:
							if (quad_type == GRASS_TOP || quad_type == GRASS_SIDE)
								biomeColorVal = glm::vec3(0.76f, 0.70f, 0.48f);
							else if (quad_type == OAK_LEAVES)
								biomeColorVal = glm::vec3(0.5f, 0.45f, 0.2f);
							else if (quad_type == WATER)
								biomeColorVal = biomeParams.waterColor;
							break;
						case BIOME_FOREST:
							if (quad_type == GRASS_TOP || quad_type == GRASS_SIDE)
								biomeColorVal = glm::vec3(0.3f, 0.65f, 0.2f);
							else if (quad_type == OAK_LEAVES)
								biomeColorVal = glm::vec3(0.2f, 0.6f, 0.1f);
							else if (quad_type == WATER)
								biomeColorVal = biomeParams.waterColor;
							break;
						case BIOME_PLAIN:
							if (quad_type == GRASS_TOP || quad_type == GRASS_SIDE)
								biomeColorVal = glm::vec3(0.4f, 0.7f, 0.3f);
							else if (quad_type == OAK_LEAVES)
								biomeColorVal = glm::vec3(0.3f, 0.65f, 0.2f);
							else if (quad_type == WATER)
								biomeColorVal = biomeParams.waterColor;
							break;
						case BIOME_MOUNTAIN:
							if (quad_type == GRASS_TOP || quad_type == GRASS_SIDE)
								biomeColorVal = glm::vec3(0.35f, 0.55f, 0.25f);
							else if (quad_type == OAK_LEAVES)
								biomeColorVal = glm::vec3(0.25f, 0.5f, 0.15f);
							else if (quad_type == WATER)
								biomeColorVal = biomeParams.waterColor;
							break;
						default:
							if (quad_type == GRASS_TOP || quad_type == GRASS_SIDE)
								biomeColorVal = glm::vec3(0.4f, 0.7f, 0.3f);
							else if (quad_type == OAK_LEAVES)
								biomeColorVal = glm::vec3(0.3f, 0.6f, 0.2f);
							else if (quad_type == WATER)
								biomeColorVal = glm::vec3(0.0f, 0.5f, 1.0f); // Default water
						}
						// Variation from original code
						float variation = noise->noise2D_01(
											  this->position.x + quad_origin_voxel_coord[0] * 0.1f,
											  this->position.z + quad_origin_voxel_coord[2] * 0.1f) *
											  0.1f -
										  0.05f;
						biomeColorVal.r = glm::clamp(biomeColorVal.r + variation, 0.0f, 1.0f);
						biomeColorVal.g = glm::clamp(biomeColorVal.g + variation, 0.0f, 1.0f);
						biomeColorVal.b = glm::clamp(biomeColorVal.b + variation, 0.0f, 1.0f);
					}

					float texture_idx_val = static_cast<float>(quad_type);
					if (quad_type == GRASS_SIDE)
					{
						if (quad_normal_dir.y > 0.9f)
							texture_idx_val = static_cast<float>(GRASS_TOP);
						else if (quad_normal_dir.y < -0.9f)
							texture_idx_val = static_cast<float>(DIRT);
						// else remains GRASS_SIDE
					}
					else if (quad_type == OAK_LOG)
					{
						if (std::abs(quad_normal_dir.y) > 0.9f)
							texture_idx_val = static_cast<float>(OAK_LOG_TOP);
						// else remains OAK_LOG
					}

					uint32_t vert_indices[4];
					glm::vec3 quad_vertices_world[4] = {
						this->position + v0_local,
						this->position + v1_local,
						this->position + v2_local,
						this->position + v3_local};

					glm::vec3 normal_vec3 = glm::normalize(glm::vec3(quad_normal_dir));

					for (int i = 0; i < 4; ++i)
					{
						Vertex vert;
						vert.position = quad_vertices_world[i];
						vert.normal = normal_vec3;
						vert.texCoord = tc[i];
						vert.textureIndex = texture_idx_val;
						vert.useBiomeColor = needsBiomeColoring ? 1.0f : 0.0f;
						vert.biomeColor = biomeColorVal;

						auto it = vertexMap.find(vert);
						if (it != vertexMap.end())
						{
							vert_indices[i] = it->second;
						}
						else
						{
							vertices.push_back(vert);
							vert_indices[i] = indexCounter;
							vertexMap[vert] = indexCounter++;
						}
					}

					// Winding order based on normal direction along the main axis 'd'
					if (quad_normal_dir[d] > 0)
					{
						indices.push_back(vert_indices[0]);
						indices.push_back(vert_indices[1]);
						indices.push_back(vert_indices[2]);
						indices.push_back(vert_indices[0]);
						indices.push_back(vert_indices[2]);
						indices.push_back(vert_indices[3]);
					}
					else
					{
						indices.push_back(vert_indices[0]);
						indices.push_back(vert_indices[2]);
						indices.push_back(vert_indices[1]);
						indices.push_back(vert_indices[0]);
						indices.push_back(vert_indices[3]);
						indices.push_back(vert_indices[2]);
					}

					// Mark processed cells in the mask
					for (int iw = 0; iw < w; ++iw)
					{
						for (int ih = 0; ih < h; ++ih)
						{
							mask[(x[u] + iw) * dims[v] + (x[v] + ih)] = true;
						}
					}
				}
			}
		}
	}

	meshNeedsUpdate = true; // Flag for GPU upload
	state = ChunkState::MESHED;
}

void Chunk::uploadMeshToGPU()
{
	// Générer les buffers si nécessaire
	if (VAO == 0)
	{
		glGenVertexArrays(1, &VAO);
	}
	if (VBO == 0)
	{
		glGenBuffers(1, &VBO);
	}
	if (EBO == 0)
	{
		glGenBuffers(1, &EBO);
	}

	glBindVertexArray(VAO);

	// Charger les données de sommets
	glBindBuffer(GL_ARRAY_BUFFER, VBO);
	glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(Vertex), vertices.data(), GL_STATIC_DRAW);

	// Charger les indices
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(uint16_t), indices.data(), GL_STATIC_DRAW);

	// Configurer les attributs de sommets

	// Position (location = 0)
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)offsetof(Vertex, position));

	// Normale (location = 1)
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)offsetof(Vertex, normal));

	// Coordonnées de texture (location = 2)
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)offsetof(Vertex, texCoord));

	// Index de texture (location = 3)
	glEnableVertexAttribArray(3);
	glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)offsetof(Vertex, textureIndex));

	// Flag de coloration biome (location = 4)
	glEnableVertexAttribArray(4);
	glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)offsetof(Vertex, useBiomeColor));

	// Couleur du biome (location = 5)
	glEnableVertexAttribArray(5);
	glVertexAttribPointer(5, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)offsetof(Vertex, biomeColor));

	glBindVertexArray(0);

	meshNeedsUpdate = false;
}

uint32_t Chunk::draw(const Shader &shader, const Camera &camera, GLuint textureArray, const ShaderParameters &params)
{
	if (meshNeedsUpdate)
		uploadMeshToGPU();

	if (indices.empty())
		return 0;

	shader.use();

	glm::vec3 localPos = glm::vec3(position.x / SIZE, position.y, position.z / SIZE);
	shader.setMat4("model", glm::translate(glm::mat4(1.0f), localPos));
	shader.setMat4("view", camera.getViewMatrix());
	shader.setMat4("projection", camera.getProjectionMatrix(1920, 1080, 320));
	shader.setInt("textureArray", 0);

	glm::vec3 sunPosition = camera.getPosition() + params.sunDirection * 2000.0f;
	shader.setVec3("lightPos", sunPosition);
	shader.setVec3("viewPos", camera.getPosition());

	// Paramètres du fog
	shader.setFloat("fogStart", params.fogStart);
	shader.setFloat("fogEnd", params.fogEnd);
	shader.setVec3("fogColor", params.fogColor);
	shader.setFloat("fogDensity", params.fogDensity);

	// Paramètres visuels optionnels
	shader.setFloat("ambientStrength", params.ambientStrength);
	shader.setFloat("diffuseIntensity", params.diffuseIntensity);
	shader.setFloat("lightLevels", params.lightLevels);
	shader.setFloat("saturationLevel", params.saturationLevel);
	shader.setFloat("colorBoost", params.colorBoost);
	shader.setFloat("gamma", params.gamma);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D_ARRAY, textureArray);

	glBindVertexArray(VAO);
	glDrawElements(GL_TRIANGLES, indices.size(), GL_UNSIGNED_SHORT, 0);
	glBindVertexArray(0);

	return indices.size();
}