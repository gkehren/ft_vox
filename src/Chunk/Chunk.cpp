#include "Chunk.hpp"

Chunk::Chunk(const glm::vec3& position, siv::PerlinNoise* perlin) : position(position)
{
	this->voxels.resize(Chunk::SIZE);
	for (int x = 0; x < Chunk::SIZE; x++) {
		this->voxels[x].resize(Chunk::HEIGHT);
		for (int y = 0; y < Chunk::HEIGHT; y++) {
			for (int z = 0; z < Chunk::SIZE; z++) {
				double noise = perlin->noise3D_01((x + position.x) / Chunk::RADIUS, (y + position.y) / Chunk::RADIUS, (z + position.z) / Chunk::RADIUS);
				if (noise > 0.5) {
					this->voxels[x][y].push_back(Voxel(glm::vec3(x, y, z), TEXTURE_GRASS));
				} else if (noise > 0.4) {
					this->voxels[x][y].push_back(Voxel(glm::vec3(x, y, z), TEXTURE_DIRT));
				} else if (noise > 0.3) {
					this->voxels[x][y].push_back(Voxel(glm::vec3(x, y, z), TEXTURE_STONE));
				} else {
					this->voxels[x][y].push_back(Voxel(glm::vec3(x, y, z), TEXTURE_AIR));
				}
			}
		}
	}

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
	this->generateMesh();
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

void	Chunk::generateMesh()
{
	for (int x = 0; x < Chunk::SIZE; x++) {
		for (int y = 0; y < Chunk::HEIGHT; y++) {
			for (int z = 0; z < Chunk::SIZE; z++) {
				if (this->voxels[x][y][z].getType() != TEXTURE_AIR) {
					this->addVoxelToMesh(this->voxels[x][y][z], x, y, z);
				}
			}
		}
	}
}

void	Chunk::addVoxelToMesh(Voxel& voxel, int x, int y, int z)
{
	for (auto& dir : directions) {
		int dx, dy, dz;
		Face face;
		std::tie(dx, dy, dz, face) = dir;

		int nx = x + dx;
		int ny = y + dy;
		int nz = z + dz;

		bool isInside = nx >= 0 && nx < Chunk::SIZE && ny >= 0 && ny < Chunk::HEIGHT && nz >= 0 && nz < Chunk::SIZE;

		bool isAirOrOutside = !isInside || this->voxels[nx][ny][nz].getType() == TEXTURE_AIR;

		if (isAirOrOutside) voxel.addFaceToMesh(mesh, face, voxel.getType());
	}
}
