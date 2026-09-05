#pragma once

#include <utils.hpp>

// Semantic block traits shared by worldgen, physics and the renderer.
// This header must stay free of any render-only metadata (shapes, atlas
// layers, transparency): gameplay code may depend on it without pulling the
// Renderer in.

inline bool blockIsLeaves(TextureType t)
{
	switch (t)
	{
	case OAK_LEAVES:
	case BIRCH_LEAVES:
	case SPRUCE_LEAVES:
	case JUNGLE_LEAVES:
	case ACACIA_LEAVES:
	case DARK_OAK_LEAVES:
	case CHERRY_LEAVES:
	case MANGROVE_LEAVES:
		return true;
	default:
		return false;
	}
}

inline bool blockIsIce(TextureType t)
{
	return t == ICE || t == PACKED_ICE || t == BLUE_ICE;
}

/// Surfaces that can host trees / cacti / ice spikes.
inline bool blockIsPlantableSurface(TextureType t)
{
	switch (t)
	{
	case GRASS_TOP:
	case DIRT:
	case SAND:
	case SNOW:
	case RED_SAND:
	case PODZOL:
	case COARSE_DIRT:
	case MUD:
	case MOSS_BLOCK:
	case TERRACOTTA:
	case ORANGE_TERRACOTTA:
	case RED_TERRACOTTA:
	case YELLOW_TERRACOTTA:
	case BROWN_TERRACOTTA:
	case WHITE_TERRACOTTA:
	case PACKED_ICE:
		return true;
	default:
		return false;
	}
}

/// Stone-like hosts that ore veins may replace.
inline bool blockIsOreHost(TextureType t)
{
	switch (t)
	{
	case STONE:
	case ANDESITE:
	case DIORITE:
	case GRANITE:
	case TUFF:
	case DEEPSLATE:
		return true;
	default:
		return false;
	}
}

/// Soft ground valid for cactus placement (desert / badlands).
inline bool blockIsCactusGround(TextureType t)
{
	return t == SAND || t == RED_SAND || t == TERRACOTTA || t == ORANGE_TERRACOTTA ||
		   t == RED_TERRACOTTA;
}
