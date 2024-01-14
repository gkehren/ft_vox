#include "Chunk.hpp"

Chunk::Chunk(const glm::vec3& position) : position(position), visible(false), state(ChunkState::UNLOADED)
{
	this->voxels.resize(Chunk::SIZE);
	for (int x = 0; x < Chunk::SIZE; x++) {
		this->voxels[x].resize(Chunk::HEIGHT);
		for (int y = 0; y < Chunk::HEIGHT; y++) {
			for (int z = 0; z < Chunk::SIZE; z++) {
				this->voxels[x][y].push_back(Voxel(glm::vec3(x, y, z), TEXTURE_AIR));
			}
		}
	}
}

Chunk::~Chunk()
{}

const glm::vec3&	Chunk::getPosition() const
{
	return (this->position);
}

const std::vector<float>&	Chunk::getData()
{
	return mesh.getData();
}

bool	Chunk::isVisible() const
{
	return (this->visible);
}

void	Chunk::setVisible(bool visible)
{
	this->visible = visible;
}

void	Chunk::setState(ChunkState state)
{
	this->state = state;
}

ChunkState	Chunk::getState() const
{
	return (this->state);
}

void	Chunk::generateMesh(const std::unordered_map<glm::ivec3, Chunk, ivec3_hash>& chunks)
{
	if (state != ChunkState::GENERATED) return;

	for (int x = 0; x < Chunk::SIZE; x++) {
		for (int y = 0; y < Chunk::HEIGHT; y++) {
			for (int z = 0; z < Chunk::SIZE; z++) {
				if (this->voxels[x][y][z].getType() != TEXTURE_AIR) {
					this->addVoxelToMesh(chunks, this->voxels[x][y][z], x, y, z);
				}
			}
		}
	}
	state = ChunkState::MESHED;
}

void	Chunk::addVoxelToMesh(const std::unordered_map<glm::ivec3, Chunk, ivec3_hash>& chunks, Voxel& voxel, int x, int y, int z)
{
	TextureType voxelType = voxel.getType();
	for (auto& dir : directions) {
		int dx, dy, dz;
		Face face;
		std::tie(dx, dy, dz, face) = dir;

		int nx = x + dx;
		int ny = y + dy;
		int nz = z + dz;

		bool isInside = nx >= 0 && nx < Chunk::SIZE && ny >= 0 && ny < Chunk::HEIGHT && nz >= 0 && nz < Chunk::SIZE;

		if (isInside) {
			if (this->voxels[nx][ny][nz].getType() == TEXTURE_AIR) {
				voxel.addFaceToMesh(mesh, face, voxelType);
			}
		} else {
			if (voxel.isHighest()) {
				voxel.addFaceToMesh(mesh, face, voxelType);
			} else {
				glm::ivec3 adjacentChunkPos = glm::ivec3(this->position) + glm::ivec3(dx, dy, dz) * Chunk::SIZE;
				adjacentChunkPos.x = floor(adjacentChunkPos.x / Chunk::SIZE);
				adjacentChunkPos.y = floor(adjacentChunkPos.y / Chunk::HEIGHT);
				adjacentChunkPos.z = floor(adjacentChunkPos.z / Chunk::SIZE);
				auto adjacentChunk = chunks.find(adjacentChunkPos);
				if (adjacentChunk != chunks.end()) {
					if (adjacentChunk->second.state == ChunkState::UNLOADED) continue;
					int adjacentX = (nx + Chunk::SIZE) % Chunk::SIZE;
					int adjacentY = (ny + Chunk::HEIGHT) % Chunk::HEIGHT;
					int adjacentZ = (nz + Chunk::SIZE) % Chunk::SIZE;
					if (adjacentChunk->second.getVoxel(adjacentX, adjacentY, adjacentZ).getType() == TEXTURE_AIR) {
						voxel.addFaceToMesh(mesh, face, voxelType);
					}
				}
			}
		}
	}
}

bool	Chunk::contains(int x, int y, int z) const
{
	return (x >= position.x && x < position.x + Chunk::SIZE &&
			y >= position.y && y < position.y + Chunk::HEIGHT &&
			z >= position.z && z < position.z + Chunk::SIZE);
}

const Voxel&	Chunk::getVoxel(int x, int y, int z) const
{
	return (this->voxels[x][y][z]);
}

bool	Chunk::deleteVoxel(glm::vec3 position, glm::vec3 front)
{
	// X, Y, Z are world coordinates, need to convert them to chunk coordinates
	// Check if the voxel is in this chunk
	glm::vec3 target = position + front * 0.5f;
	int x = floor(target.x);
	int y = floor(target.y);
	int z = floor(target.z);
	if (!this->contains(x, y, z)) return false;

	// Convert world coordinates to chunk coordinates
	x = floor(x - this->position.x);
	y = floor(y - this->position.y);
	z = floor(z - this->position.z);

	if (this->voxels[x][y][z].getType() == TEXTURE_AIR) return false;

	// Delete the voxel
	this->voxels[x][y][z].setType(TEXTURE_AIR);

	// Update the mesh
	mesh.clear();
	this->state = ChunkState::GENERATED;
	return true;
}

bool	Chunk::placeVoxel(glm::vec3 position, glm::vec3 front)
{
	// X, Y, Z are world coordinates, need to convert them to chunk coordinates
	// Check if the voxel is in this chunk
	glm::vec3 target = position + front * 0.5f;
	int x = floor(target.x);
	int y = floor(target.y);
	int z = floor(target.z);
	if (!this->contains(x, y, z)) return false;

	// Convert world coordinates to chunk coordinates
	x = floor(x - this->position.x);
	y = floor(y - this->position.y);
	z = floor(z - this->position.z);

	if (this->voxels[x][y][z].getType() != TEXTURE_AIR) return false;

	// Delete the voxel
	this->voxels[x][y][z].setType(TEXTURE_PLANK);

	// Update the mesh
	mesh.clear();
	this->state = ChunkState::GENERATED;
	return true;
}

void	Chunk::generateChunk(int startX, int endX, int startZ, int endZ, siv::PerlinNoise* perlin) {
	for (int x = startX; x < endX; x++) {
		for (int z = startZ; z < endZ; z++) {
			// Generate Perlin noise values at different scales
			float noise1 = perlin->noise2D_01((position.x + x) / 200.0f, (position.z + z) / 200.0f) * 1.5f;
			float noise2 = perlin->noise2D_01((position.x + x) / 50.0f, (position.z + z) / 50.0f) * 0.5f;
			float noise3 = perlin->noise2D_01((position.x + x) / 25.0f, (position.z + z) / 25.0f) * 0.2f;
			float mountainNoise = perlin->noise2D_01((position.x + x) / 1000.0f, (position.z + z) / 1000.0f); // New noise layer for mountains

			// Combine the Perlin noise values to determine the surface height at this point
			int surfaceHeight = static_cast<int>((noise1 + noise2 + noise3) * Chunk::HEIGHT / 2) - 32;

			if (mountainNoise > 0.8f) { // This threshold determines how much of the terrain will be mountains
				surfaceHeight += static_cast<int>(mountainNoise * 64);
			}

			for (int y = 0; y < Chunk::HEIGHT; y++) {
				double caveNoise = perlin->noise3D_01((x + position.x) / Chunk::SIZE, (y + position.y) / Chunk::SIZE, (z + position.z) / Chunk::SIZE);
				if (y < surfaceHeight) {
					if (caveNoise > 0.25) {
						this->voxels[x][y][z].setType(TextureType::TEXTURE_STONE);
					} else {
						this->voxels[x][y][z].setType(TextureType::TEXTURE_AIR);
					}
				} else if (y == surfaceHeight) {
					// Check if there is a cave just below this voxel
					if (caveNoise <= 0.2) {
						// This voxel is at the surface and there is a cave below, so fill it with air to make the cave accessible
						this->voxels[x][y][z].setType(TextureType::TEXTURE_AIR);
					} else {
						// This voxel is at the surface, so fill it with grass
						this->voxels[x][y][z].setType(TextureType::TEXTURE_GRASS, true);
					}
				} else {
					if (surfaceHeight <= 1 && y == 0) {
						this->voxels[x][y][z].setType(TextureType::TEXTURE_GRASS); // MAYBE CHANGE TO WATER
					} else {
						// This voxel is above the surface, so fill it with air
						this->voxels[x][y][z].setType(TextureType::TEXTURE_AIR);
					}
				}
			}
		}
	}
}

void	Chunk::generateVoxel(siv::PerlinNoise* perlin)
{
	if (state != ChunkState::UNLOADED) return;

	const int numThreads = 4;
	const int chunkWidth = Chunk::SIZE / numThreads;

	std::vector<std::thread> threads;

	for (int i = 0; i < numThreads; i++) {
		int startX = i * chunkWidth;
		int endX = (i + 1) * chunkWidth;
		threads.emplace_back(&Chunk::generateChunk, this, startX, endX, 0, 16, perlin);
	}

	for (auto& thread : threads) {
		thread.join();
	}

	state = ChunkState::GENERATED;

	// DEBUG
	// FILL MESH WITH DATA TO CREATE A CUBE AT 0, 0, 0
	//this->voxels.resize(Chunk::SIZE);
	//for (int x = 0; x < Chunk::SIZE; x++) {
	//	this->voxels[x].resize(Chunk::HEIGHT);
	//	for (int y = 0; y < Chunk::HEIGHT; y++) {
	//		for (int z = 0; z < Chunk::SIZE; z++) {
	//			this->voxels[x][y].push_back(Voxel(glm::vec3(x, y, z), TEXTURE_AIR));
	//		}
	//	}
	//}

	//this->voxels[0][0][0] = Voxel(glm::vec3(0, 0, 0), TEXTURE_GRASS);
	//this->voxels[0][0][1] = Voxel(glm::vec3(0, 0, 1), TEXTURE_GRASS);
	//this->voxels[0][1][0] = Voxel(glm::vec3(0, 1, 0), TEXTURE_GRASS);
}
