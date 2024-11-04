#pragma once

#include <cstdint>
#include <utils.hpp>

class Voxel
{
	public:
		Voxel(TextureType type = TEXTURE_AIR) : type(type) {}

		TextureType getType() const { return type; }
		void setType(TextureType newType) { type = newType; }

	private:
		TextureType	type;
};
