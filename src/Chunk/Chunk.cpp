#include "Chunk.hpp"
#include <FastNoise/FastNoiseLite.h>
#include <utils.hpp>
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
	return y * CHUNK_SIZE * CHUNK_SIZE + z * CHUNK_SIZE + x;
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
	if (x >= 0 && x < CHUNK_SIZE && y >= 0 && y < CHUNK_HEIGHT && z >= 0 && z < CHUNK_SIZE)
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
	else if ((x == -1 || x == CHUNK_SIZE || z == -1 || z == CHUNK_SIZE) && (y >= 0 && y < CHUNK_HEIGHT))
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
	if (x >= 0 && x < CHUNK_SIZE && y >= 0 && y < CHUNK_HEIGHT && z >= 0 && z < CHUNK_SIZE)
	{
		size_t index = getIndex(x, y, z);
		return activeVoxels.test(index);
	}
	else if ((x == -1 || x == CHUNK_SIZE || z == -1 || z == CHUNK_SIZE) && (y >= 0 && y < CHUNK_HEIGHT))
	{
		return neighborShellVoxels.count(glm::ivec3(x, y, z)) > 0;
	}
	return false; // Outside known boundaries
}

void Chunk::generateTerrain(TerrainGenerator &generator)
{
	if (state != ChunkState::UNLOADED)
		return;

	neighborShellVoxels.clear();

	auto terrainData = generator.generateChunk(position.x, position.z);
	voxels = std::move(terrainData);

	// Update bitset for active voxels
	activeVoxels.reset(); // Clear all bits first
	for (int i = 0; i < CHUNK_VOLUME; ++i)
	{
		if (this->voxels[i].type != TextureType::AIR)
		{ // Or your equivalent of an air block
			activeVoxels.set(i);
		}
	}

	state = ChunkState::GENERATED;
	meshNeedsUpdate = true;
}

void Chunk::generateMesh()
{
	vertices.clear();
	indices.clear();
	// Assuming VertexHasher is defined for Vertex struct
	std::unordered_map<Vertex, uint32_t, VertexHasher> vertexMap;
	vertexMap.clear();
	uint32_t indexCounter = 0;

	// New helper for greedy meshing that checks local voxels and the precomputed neighbor shell
	auto getVoxelDataForMeshing = [&](int lx, int ly, int lz) -> TextureType
	{
		if (lx >= 0 && lx < CHUNK_SIZE && ly >= 0 && ly < CHUNK_HEIGHT && lz >= 0 && lz < CHUNK_SIZE)
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

	const int dims[] = {CHUNK_SIZE, CHUNK_HEIGHT, CHUNK_SIZE};

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
						next_pos_u_slice[u] += w; // Next voxel in u-direction in current slice

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
					// TODO: Implement biome coloring logic later
					// For now, we will just use a default color for these types

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

	glm::vec3 localPos = glm::vec3(position.x / CHUNK_SIZE, position.y, position.z / CHUNK_SIZE);
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