#include "Chunk.hpp"

Chunk::Chunk(const glm::vec3& position) : position(position)
{
	this->voxels.resize(Chunk::SIZE);
	for (int x = 0; x < Chunk::SIZE; x++) {
		this->voxels[x].resize(Chunk::HEIGHT);
		for (int y = 0; y < Chunk::HEIGHT; y++) {
			for (int z = 0; z < Chunk::SIZE; z++) {
				this->voxels[x][y].push_back(Voxel(glm::vec3(x, y, z)));
			}
		}
	}

	// DEBUG
	// FILL MESH WITH DATA TO CREATE A CUBE AT 0, 0, 0
	for (int x = 0; x < Chunk::SIZE; x++) {
		for (int y = 0; y < 1; y++) {
			for (int z = 0; z < Chunk::SIZE; z++) {
				this->voxels[x][y][z].setType(TEXTURE_GRASS);
			}
		}
	}
	this->generateMesh();
}

Chunk::~Chunk()
{}

const glm::vec3&	Chunk::getPosition() const
{
	return (this->position);
}

const std::vector<float>	Chunk::getData() const
{
	return mesh.getData();
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
	for (int dx = -1; dx <= 1; dx++)
	{
		for (int dy = -1; dy <= 1; dy++)
		{
			for (int dz = -1; dz <= 1; dz++)
			{
				// Skip the voxel itself
				if (dx == 0 && dy == 0 && dz == 0)
					continue;

				// Compute the neighbor's coordinates
				int nx = x + dx;
				int ny = y + dy;
				int nz = z + dz;

				// Check if the neighbor's coordinates are valid
				if (nx >= 0 && nx < Chunk::SIZE && ny >= 0 && ny < Chunk::HEIGHT && nz >= 0 && nz < Chunk::SIZE)
				{
					// Check if the neighbor is air
					if (voxels[nx][ny][nz].getType() == TEXTURE_AIR)
					{
						// Add the corresponding face of the voxel to the mesh
						if (dx == -1) voxel.addFaceToMesh(mesh, Face::LEFT);
						else if (dx == 1) voxel.addFaceToMesh(mesh, Face::RIGHT);
						else if (dy == -1) voxel.addFaceToMesh(mesh, Face::BOTTOM);
						else if (dy == 1) voxel.addFaceToMesh(mesh, Face::TOP);
						else if (dz == -1) voxel.addFaceToMesh(mesh, Face::BACK);
						else if (dz == 1) voxel.addFaceToMesh(mesh, Face::FRONT);
					}
				} else {
					if (dx == -1) voxel.addFaceToMesh(mesh, Face::LEFT);
					else if (dx == 1) voxel.addFaceToMesh(mesh, Face::RIGHT);
					else if (dy == -1) voxel.addFaceToMesh(mesh, Face::BOTTOM);
					else if (dy == 1) voxel.addFaceToMesh(mesh, Face::TOP);
					else if (dz == -1) voxel.addFaceToMesh(mesh, Face::BACK);
					else if (dz == 1) voxel.addFaceToMesh(mesh, Face::FRONT);
				}
			}
		}
	}
}
