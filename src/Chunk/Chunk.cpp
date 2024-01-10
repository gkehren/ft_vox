#include "Chunk.hpp"

Chunk::Chunk(const glm::vec3& position, siv::PerlinNoise* perlin) : position(position), visible(false), state(ChunkState::UNLOADED)
{}

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

void	Chunk::generateMesh(const std::vector<Chunk>& chunks)
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

void	Chunk::addVoxelToMesh(const std::vector<Chunk>& chunks, Voxel& voxel, int x, int y, int z)
{
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
				voxel.addFaceToMesh(mesh, face, voxel.getType());
			}
		} else { // TODO: Still some empty face to fill
			int worldX = this->getPosition().x + nx;
			int worldY = this->getPosition().y + ny;
			int worldZ = this->getPosition().z + nz;

			for (const auto& chunk : chunks) {
				if (chunk.getState() == ChunkState::MESHED) {
					if (chunk.contains(worldX, worldY, worldZ)) {
						int localX = worldX - chunk.getPosition().x;
						int localY = worldY - chunk.getPosition().y;
						int localZ = worldZ - chunk.getPosition().z;

						if (chunk.getVoxel(localX, localY, localZ).getType() == TEXTURE_AIR) {
							voxel.addFaceToMesh(mesh, face, voxel.getType());
						}
						break;
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

void	Chunk::generateVoxel(siv::PerlinNoise* perlin)
{
	if (state != ChunkState::UNLOADED) return;

	this->voxels.resize(Chunk::SIZE);
	for (int x = 0; x < Chunk::SIZE; x++) {
		this->voxels[x].resize(Chunk::HEIGHT);
		for (int z = 0; z < Chunk::SIZE; z++) {
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
				if (y < surfaceHeight) {
					// This voxel is below the surface, so fill it with dirt or stone
					if (y > surfaceHeight - 5) {
						this->voxels[x][y].push_back(Voxel(glm::vec3(x, y, z), TEXTURE_DIRT));
					} else {
						this->voxels[x][y].push_back(Voxel(glm::vec3(x, y, z), TEXTURE_STONE));
					}
				} else if (y == surfaceHeight) {
					// This voxel is at the surface, so fill it with grass
					this->voxels[x][y].push_back(Voxel(glm::vec3(x, y, z), TEXTURE_GRASS));
				} else {
					// This voxel is above the surface, so fill it with air
					this->voxels[x][y].push_back(Voxel(glm::vec3(x, y, z), TEXTURE_AIR));
				}
			}
		}
	}
	state = ChunkState::GENERATED;

	// GOOD FOR CAVE GENERATION (TWEEK THE VALUES FOR BETTER RESULTS)
	//for (int x = 0; x < Chunk::SIZE; x++) {
	//	this->voxels[x].resize(Chunk::HEIGHT);
	//	for (int y = 0; y < Chunk::HEIGHT; y++) {
	//		for (int z = 0; z < Chunk::SIZE; z++) {
	//			double noise = perlin->noise3D_01((x + position.x) / Chunk::RADIUS, (y + position.y) / Chunk::RADIUS, (z + position.z) / Chunk::RADIUS);
	//			if (noise > 0.5) {
	//				this->voxels[x][y].push_back(Voxel(glm::vec3(x, y, z), TEXTURE_GRASS));
	//			} else if (noise > 0.4) {
	//				this->voxels[x][y].push_back(Voxel(glm::vec3(x, y, z), TEXTURE_DIRT));
	//			} else if (noise > 0.3) {
	//				this->voxels[x][y].push_back(Voxel(glm::vec3(x, y, z), TEXTURE_STONE));
	//			} else {
	//				this->voxels[x][y].push_back(Voxel(glm::vec3(x, y, z), TEXTURE_AIR));
	//			}
	//		}
	//	}
	//}

	// DEBUG
	// FILL MESH WITH DATA TO CREATE A CUBE AT 0, 0, 0
	//for (int x = 0; x < Chunk::SIZE; x++) {
	//	for (int y = 0; y < 3; y++) {
	//		for (int z = 0; z < Chunk::SIZE; z++) {
	//			// generate random texture Type
	//			TextureType type = static_cast<TextureType>(rand() % TEXTURE_COUNT);
	//			this->voxels[x][y][z].setType(type);
	//		}
	//	}
	//}
}
