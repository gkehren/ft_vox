// Unit tests for shipped render helpers (cascade / fog / moon / emissive / block light).
// Cascade path exercises the same frustum-slice builders WorldRenderer packs into FrameUBO.

#include <Renderer/ShadowCascades.hpp>
#include <Renderer/Lighting.hpp>
#include <Renderer/FrameUBO.hpp>
#include <Renderer/MaterialTable.hpp>
#include <Renderer/PostDefaults.hpp>
#include <Renderer/MinecraftTextures.hpp>
#include <Renderer/ResourcePackReader.hpp>
#include <Renderer/IndirectDrawUtils.hpp>
#include <Renderer/ColorSpace.hpp>
#include <Engine/EngineDefs.hpp>
#include <fstream>
#include <filesystem>
#include <utils.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

namespace
{
bool fail(const std::string &msg)
{
	std::cerr << "FAIL: " << msg << "\n";
	return false;
}

bool isFiniteMat(const glm::mat4 &m)
{
	for (int c = 0; c < 4; ++c)
		for (int r = 0; r < 4; ++r)
			if (!std::isfinite(m[c][r]))
				return false;
	return true;
}
} // namespace

int main()
{
	bool ok = true;

	// --- Cascade splits: ordered near → far, cover span ---
	{
		const float nearP = 0.1f;
		const float farP = 280.f;
		const auto splits = shadow::computeCascadeSplits(nearP, farP);
		if (!(splits[0] > nearP))
			ok = fail("cascade split[0] must be > near");
		if (!(splits[1] > splits[0] && splits[2] > splits[1]))
			ok = fail("cascade splits must be strictly increasing");
		if (std::abs(splits[2] - farP) > 1e-3f)
			ok = fail("last cascade split must equal far plane");
	}

	// --- Frustum-slice cascade matrices (shipped path) ---
	{
		const float nearP = 0.1f;
		const float farP = 280.f;
		std::array<glm::mat4, shadow::kCascadeCount> mats{};
		std::array<float, shadow::kCascadeCount> extents{};
		glm::vec4 splitVec{};
		const glm::vec3 camPos(10.f, 80.f, -5.f);
		const glm::vec3 front = glm::normalize(glm::vec3(0.2f, -0.1f, -1.f));
		const glm::vec3 lightDir = glm::normalize(glm::vec3(0.4f, 1.f, 0.2f));

		shadow::buildCascadeUBOFromFront(camPos, front, glm::vec3(0.f, 1.f, 0.f), lightDir,
										nearP, farP, 16.f / 9.f, shadow::kDefaultFovYDegrees,
										mats, splitVec, &extents);

		if (static_cast<int>(splitVec.w + 0.5f) != shadow::kCascadeCount)
			ok = fail("cascadeSplits.w must be cascade count");
		if (!(splitVec.x > 0.f && splitVec.y > splitVec.x && splitVec.z >= splitVec.y - 1e-3f))
			ok = fail("buildCascadeUBOFromFront split distances not ordered");

		for (int i = 0; i < shadow::kCascadeCount; ++i)
		{
			if (!isFiniteMat(mats[i]))
				ok = fail(std::string("cascade matrix ") + std::to_string(i) + " not finite");
			if (!(extents[i] > 1.f))
				ok = fail(std::string("cascade ") + std::to_string(i) + " half-extent must be > 1");
		}
		// Farther cascades cover larger (or equal) light-space extent
		if (!(extents[1] + 1.f >= extents[0] && extents[2] + 1.f >= extents[1]))
			ok = fail("farther cascades should have larger/equal light-space extent");

		// Single-slice bounds helper is finite and progressive
		const glm::vec3 right = glm::normalize(glm::cross(front, glm::vec3(0.f, 1.f, 0.f)));
		const glm::vec3 up = glm::normalize(glm::cross(right, front));
		const auto nearSlice = shadow::computeFrustumSliceCascade(
			camPos, front, right, up, lightDir, glm::radians(80.f), 16.f / 9.f, 0.1f, 40.f);
		const auto farSlice = shadow::computeFrustumSliceCascade(
			camPos, front, right, up, lightDir, glm::radians(80.f), 16.f / 9.f, 80.f, 280.f);
		if (!nearSlice.finite || !farSlice.finite)
			ok = fail("frustum slice cascade bounds must be finite");
		if (!(farSlice.halfExtentX + farSlice.halfExtentY >
			  nearSlice.halfExtentX + nearSlice.halfExtentY - 1.f))
			ok = fail("far frustum slice should cover larger XY extent than near");

		// Stable up when light is vertical
		const glm::vec3 upVert = shadow::stableLightUp(glm::vec3(0.f, 1.f, 0.f));
		if (glm::length(upVert) < 0.5f)
			ok = fail("stableLightUp must return non-zero for vertical light");
		if (std::abs(glm::dot(glm::normalize(glm::vec3(0.f, 1.f, 0.f)), upVert)) > 0.99f)
			ok = fail("stableLightUp should not be parallel to vertical light dir");

		// Light-space depth order: point toward the sun (+lightDir) is closer to the
		// cascade camera than the same offset toward the anti-sun (-lightDir).
		// GLM ZERO_TO_ONE: smaller NDC z = nearer. Wrong eye (center - dir) inverts this.
		{
			const glm::vec3 origin(0.f, 64.f, 0.f);
			const glm::vec3 L = glm::normalize(glm::vec3(0.35f, 0.9f, 0.15f));
			const glm::vec3 f2(0.f, 0.f, -1.f);
			const glm::vec3 r2(1.f, 0.f, 0.f);
			const glm::vec3 u2(0.f, 1.f, 0.f);
			const auto slice = shadow::computeFrustumSliceCascade(
				origin, f2, r2, u2, L, glm::radians(80.f), 16.f / 9.f, 1.f, 80.f);
			if (!slice.finite)
				ok = fail("depth-order cascade slice not finite");
			const glm::vec3 sunSide = origin + L * 20.f;
			const glm::vec3 antiSun = origin - L * 20.f;
			const auto ndcZ = [&](const glm::vec3 &p) -> float {
				const glm::vec4 clip = slice.lightViewProj * glm::vec4(p, 1.f);
				return clip.z / std::max(clip.w, 1e-6f);
			};
			const float zSun = ndcZ(sunSide);
			const float zAnti = ndcZ(antiSun);
			if (!std::isfinite(zSun) || !std::isfinite(zAnti))
				ok = fail("light NDC-Z not finite for depth-order probes");
			// Sun-side point is nearer the light camera → smaller depth (ZERO_TO_ONE)
			if (!(zSun < zAnti - 1e-4f))
				ok = fail(std::string("light-space depth inverted: sunSide z=") + std::to_string(zSun) +
						  " antiSun z=" + std::to_string(zAnti) +
						  " (eye must be center+lightDir, not center-lightDir)");
		}
	}

	// --- Bias / blend pure helpers (shader contract) ---
	{
		const float b0 = shadow::shadowDepthBias(1.0f);
		const float b1 = shadow::shadowDepthBias(0.0f);
		if (!(b1 > b0 && b0 >= 0.003f))
			ok = fail("shadowDepthBias must increase as N·L drops, floor ~0.0035");

		const float wFull = shadow::cascadeBlendWeight(10.f, 50.f, 0.1f, 0.12f);
		const float wEdge = shadow::cascadeBlendWeight(48.f, 50.f, 0.1f, 0.12f);
		const float wPast = shadow::cascadeBlendWeight(50.f, 50.f, 0.1f, 0.12f);
		if (std::abs(wFull - 1.f) > 1e-4f)
			ok = fail("cascadeBlendWeight deep inside split must be 1");
		if (!(wEdge < 1.f && wEdge > 0.f))
			ok = fail("cascadeBlendWeight near split edge must be in (0,1)");
		if (wPast > 1e-4f)
			ok = fail("cascadeBlendWeight at/after split end must be 0");
	}

	// --- Exponential height fog ---
	{
		const float dens = 0.002f;
		const float hf = 0.02f;
		const float lowNear = lighting::exponentialHeightFogFactor(20.f, 64.f, 64.f, dens, hf, 64.f);
		const float lowFar = lighting::exponentialHeightFogFactor(200.f, 64.f, 64.f, dens, hf, 64.f);
		if (!(lowFar > lowNear))
			ok = fail("fog factor must increase with distance");
		const float lowY = lighting::exponentialHeightFogFactor(150.f, 40.f, 40.f, dens, hf, 64.f);
		const float highY = lighting::exponentialHeightFogFactor(150.f, 180.f, 180.f, dens, hf, 64.f);
		if (!(lowY > highY))
			ok = fail("fog factor must decrease with height above fog base");
	}

	// --- Moon ambient ---
	{
		const glm::vec3 dayA = lighting::moonAmbientColor(0.0f, 0.55f);
		const glm::vec3 nightA = lighting::moonAmbientColor(1.0f, 0.55f);
		if (glm::length(dayA) > 1e-4f)
			ok = fail("day moon ambient should be ~0");
		if (!(nightA.b > nightA.r && glm::length(nightA) > 0.1f))
			ok = fail("night moon ambient should be cool and non-zero");
	}

	// --- Emissive / block light / localLightScale ---
	{
		if (lighting::emissiveIntensityForBlock(static_cast<uint8_t>(STONE)) > 1e-5f)
			ok = fail("stone must not be emissive");
		if (!(lighting::emissiveIntensityForBlock(static_cast<uint8_t>(REDSTONE_ORE)) > 0.5f))
			ok = fail("redstone ore must be strongly emissive");
		if (!(lighting::emissiveIntensityForBlock(static_cast<uint8_t>(MAGMA)) > 0.5f))
			ok = fail("magma must be strongly emissive");
		if (!(lighting::emissiveIntensityForBlock(static_cast<uint8_t>(LAVA)) > 0.9f))
			ok = fail("lava must be strongly emissive");
		if (lighting::blockLightEmission(static_cast<uint8_t>(MAGMA)) < 10)
			ok = fail("magma must emit strong propagated block light");
		if (lighting::blockLightEmission(static_cast<uint8_t>(STONE)) != 0)
			ok = fail("stone block light emission must be 0");

		const uint32_t packed = lighting::packLightBits(15, 10);
		uint8_t sky = 0, blk = 0;
		lighting::unpackLightBits(packed, sky, blk);
		if (sky != 15 || blk != 10)
			ok = fail("pack/unpack light bits round-trip failed");

		const float caveScale = lighting::localLightScale(0.0f, 0.0f);
		const float torchScale = lighting::localLightScale(0.0f, 14.0f / 15.0f);
		if (std::abs(caveScale - lighting::kCaveLightFloor) > 1e-5f)
			ok = fail("localLightScale(0,0) must match kCaveLightFloor");
		if (!(caveScale > 0.15f && caveScale < 0.35f))
			ok = fail("cave light floor should be readable (~0.22) not crushed/washed");
		if (!(torchScale > caveScale + 0.4f))
			ok = fail("torch block light must brighten vs unlit cave");
		// Smoothstep curve: at 0.75 input, output > linear interpolation
		const float at75 = lighting::localLightScale(0.75f, 0.0f);
		const float linear75 = lighting::kCaveLightFloor + (1.f - lighting::kCaveLightFloor) * 0.75f;
		if (!(at75 > linear75 + 0.01f))
			ok = fail("localLightScale should use smoothstep curve (> linear at 0.75)");
	}

	// Settings defaults + outdoor look (fog / SSAO mildness)
	{
		PostProcessSettings pp{};
		if (!pp.ssaoEnabled)
			ok = fail("SSAO should default on (mild intensity)");
		if (!(pp.ssaoIntensity <= lighting::kSsaoIntensityDefault + 0.05f &&
			  pp.ssaoIntensity < 0.70f))
			ok = fail("default SSAO intensity must be mild (<0.70, ~0.40) not full veil (~1.05)");
		if (std::abs(lighting::clampSsaoIntensity(2.0f) - lighting::kSsaoIntensityMax) > 1e-5f)
			ok = fail("clampSsaoIntensity must cap at kSsaoIntensityMax");
		if (lighting::clampSsaoIntensity(0.3f) > 0.31f)
			ok = fail("clampSsaoIntensity must pass through mild values");
		if (!pp.godRaysDepthOcclusion)
			ok = fail("god ray depth occlusion should default on");
		ShaderParameters sp{};
		if (sp.moonAmbientStrength <= 0.f)
			ok = fail("moon ambient strength default must be > 0");
		if (sp.fogDensity > 0.10f)
			ok = fail("default fogDensity must be thin (≤0.10) for outdoor chroma");
		if (sp.fogStart < 300.f)
			ok = fail("default fogStart should be farther (~320) so midground stays clear");

		// Mid-range fog under default outdoor params must stay well below old 0.82 wash cap
		const float midFog = lighting::terrainFogAmount(200.f, 80.f, 80.f, sp.fogStart, sp.fogEnd,
													 sp.fogDensity, sp.fogHeightFalloff, sp.fogBaseY);
		const float farFog = lighting::terrainFogAmount(800.f, 80.f, 80.f, sp.fogStart, sp.fogEnd,
													sp.fogDensity, sp.fogHeightFalloff, sp.fogBaseY);
		if (midFog > 0.20f)
			ok = fail(std::string("mid-range fog amount too high for outdoor chroma (got ") +
					  std::to_string(midFog) + ")");
		if (farFog > lighting::kTerrainFogAmountCap + 1e-4f)
			ok = fail("far fog must respect kTerrainFogAmountCap");
		if (!(farFog >= midFog))
			ok = fail("far fog should be ≥ mid fog");
		// localLightScale cave floor readable but still dark vs outdoor full light
		if (std::abs(lighting::localLightScale(0.f, 0.f) - lighting::kCaveLightFloor) > 1e-5f)
			ok = fail("localLightScale cave floor must match kCaveLightFloor");
		if (!(lighting::localLightScale(1.f, 0.f) > 0.98f))
			ok = fail("full sky light should approach 1.0");
		if (lighting::sunShadowWeight(0.f) > 1e-4f)
			ok = fail("sunShadowWeight(0) must mute CSM in caves");
		if (lighting::sunShadowWeight(1.f) < 0.99f)
			ok = fail("sunShadowWeight(1) must fully apply outdoor CSM");
		if (lighting::caveFillAmount(0.f) < 0.99f)
			ok = fail("caveFillAmount at zero light should be ~1");
		// God rays: composite flag must match pass production (shipped helper)
		if (lighting::godRaysPassActive(true, 0.f))
			ok = fail("godRaysPassActive must be false when sunVisibility is 0 (night/stale target)");
		if (lighting::godRaysPassActive(true, lighting::kGodRaysSunVisibilityMin))
			ok = fail("godRaysPassActive must be false at exact visibility min (strict >)");
		if (!lighting::godRaysPassActive(true, 0.5f))
			ok = fail("godRaysPassActive must be true for daytime sun with rays enabled");
		if (lighting::godRaysPassActive(false, 1.f))
			ok = fail("godRaysPassActive must be false when setting disabled");
	}

	// FrameUBO contract + material table (shipped helpers, not test re-implementation)
	{
		if (sizeof(FrameUBO) != 528)
			ok = fail(std::string("FrameUBO sizeof must be 528 (got ") + std::to_string(sizeof(FrameUBO)) + ")");
		if (materials::hasFoliageWind(static_cast<uint8_t>(STONE)))
			ok = fail("stone must not have foliage wind");
		if (!materials::hasFoliageWind(static_cast<uint8_t>(OAK_LEAVES)))
			ok = fail("oak leaves must have foliage wind");
		if (materials::windStrength(static_cast<uint8_t>(DIRT)) > 1e-5f)
			ok = fail("dirt wind must be zero");
		if (!(materials::emissiveStrength(static_cast<uint8_t>(REDSTONE_ORE)) > 0.5f))
			ok = fail("redstone emissive from material table");
		if (materials::iceSpecStrength(static_cast<uint8_t>(SNOW)) <= 0.f)
			ok = fail("snow must have ice specular from material table");
		const auto gpu = materials::buildGpuTable();
		if (gpu.entries[static_cast<size_t>(OAK_LEAVES)].x <= 0.f)
			ok = fail("GPU material table must carry leaf wind in .x");
	}

	// Post 1×1 default selection policy (shipped postCompositeSources)
	{
		const auto allOff = postCompositeSources(false, false, false);
		if (!allOff.bloomUseDefault || !allOff.ssaoUseDefault || !allOff.godRaysUseDefault)
			ok = fail("all effects off must select all defaults");
		const auto allOn = postCompositeSources(true, true, true);
		if (allOn.bloomUseDefault || allOn.ssaoUseDefault || allOn.godRaysUseDefault)
			ok = fail("all effects on must sample real targets");
		const auto nightRays = postCompositeSources(true, true, false);
		if (!nightRays.godRaysUseDefault || nightRays.bloomUseDefault)
			ok = fail("god rays not produced → default black; bloom still real when enabled");
		PostProcessSettings pp{};
		pp.bloomEnabled = false;
		pp.ssaoEnabled = true;
		const auto fromSettings = postCompositeSources(pp, true);
		if (!fromSettings.bloomUseDefault || fromSettings.ssaoUseDefault || fromSettings.godRaysUseDefault)
			ok = fail("postCompositeSources(settings) must map flags correctly");
	}

	// Block layer table: single source for basenames + transparency (shipped kBlockLayers)
	{
		if (sizeof(kBlockLayers) / sizeof(kBlockLayers[0]) != static_cast<size_t>(TextureType::COUNT))
			ok = fail("kBlockLayers size must equal TextureType::COUNT");
		if (textureTypeString.size() != static_cast<std::size_t>(TextureType::COUNT))
			ok = fail("textureTypeString must contain exactly COUNT entries");
		for (std::size_t i = 0; i < textureTypeString.size(); ++i)
		{
			if (textureTypeString[i].empty())
				ok = fail("textureTypeString[" + std::to_string(i) + "] must not be empty");
		}
		if (textureTypeString[BEDROCK] != "Bedrock")
			ok = fail("BEDROCK display name mismatch");
		if (textureTypeString[WATER] != "Water")
			ok = fail("WATER display name mismatch");
		if (textureTypeString[KELP_TOP] != "Kelp Top")
			ok = fail("KELP_TOP display name mismatch");
		for (int i = 0; i < static_cast<int>(TextureType::COUNT); ++i)
		{
			const char *file = blockLayerFile(static_cast<TextureType>(i));
			if (!file || file[0] == '\0')
				ok = fail(std::string("blockLayerFile missing for type ") + std::to_string(i));
		}
		if (!blockLayerIsTransparent(GLASS) || !blockLayerIsTransparent(OAK_LEAVES) ||
			!blockLayerIsTransparent(WATER) || !blockLayerIsTransparent(ICE) ||
			!blockLayerIsTransparent(BIRCH_LEAVES) ||
			!blockLayerIsTransparent(CHERRY_LEAVES) ||
			!blockLayerIsTransparent(MANGROVE_LEAVES) ||
			!blockLayerIsTransparent(KELP) ||
			!blockLayerIsTransparent(KELP_TOP))
			ok = fail("glass/leaves/water/ice must be transparent in kBlockLayers");
		if (blockLayerIsTransparent(STONE) || blockLayerIsTransparent(DIRT) || blockLayerIsTransparent(BEDROCK))
			ok = fail("stone/dirt/bedrock must not be transparent");
		// TextureManager::isTransparent is an alias of blockLayerIsTransparent (header-only).
		if (std::string(blockLayerFile(STONE)) != "stone.png")
			ok = fail("STONE basename must be stone.png");
		if (std::string(blockLayerFile(WATER)) != "water_still.png")
			ok = fail("WATER basename must be water_still.png");
		if (std::string(blockLayerFile(GRASS_TOP)) != "grass_block_top.png")
			ok = fail("GRASS_TOP basename must be grass_block_top.png");
		if (std::string(blockLayerFile(BIRCH_LEAVES)) != "birch_leaves.png")
			ok = fail("BIRCH_LEAVES basename must be birch_leaves.png");
		if (std::string(blockLayerFallbackFile(BIRCH_LEAVES)) != "oak_leaves.png")
			ok = fail("BIRCH_LEAVES fallback file must be oak_leaves.png");
		if (std::string(blockLayerFallbackFile(ICE)) != "glass.png")
			ok = fail("ICE fallback file must be glass.png");
		if (!blockIsFoliage(OAK_LEAVES) || !blockIsFoliage(SPRUCE_LEAVES) ||
			!blockIsFoliage(BIRCH_LEAVES) || !blockIsFoliage(JUNGLE_LEAVES) ||
			!blockIsFoliage(ACACIA_LEAVES) || !blockIsFoliage(DARK_OAK_LEAVES) ||
			!blockIsFoliage(CHERRY_LEAVES) || !blockIsFoliage(MANGROVE_LEAVES) ||
			blockIsFoliage(STONE))
			ok = fail("blockIsFoliage leaf classification wrong");
		if (!blockIsIce(ICE) || !blockIsIce(PACKED_ICE) || blockIsIce(SNOW))
			ok = fail("blockIsIce classification wrong");
		if (blockTopFace(OAK_LOG) != OAK_LOG_TOP || blockTopFace(BIRCH_LOG) != BIRCH_LOG_TOP ||
			blockTopFace(CHERRY_LOG) != CHERRY_LOG_TOP ||
			blockTopFace(MANGROVE_LOG) != MANGROVE_LOG_TOP ||
			blockTopFace(BAMBOO_BLOCK) != BAMBOO_BLOCK_TOP ||
			blockTopFace(BASALT) != BASALT_TOP ||
			blockTopFace(CACTUS) != CACTUS_TOP || blockTopFace(DEEPSLATE) != DEEPSLATE_TOP)
			ok = fail("blockTopFace remaps wrong");
		if (blockBottomFace(GRASS_SIDE) != DIRT)
			ok = fail("GRASS_SIDE bottom must be DIRT");
		// Shared terrain/mesh policy predicates
		if (!blockIsPlantableSurface(GRASS_TOP) || !blockIsPlantableSurface(RED_SAND) ||
			!blockIsPlantableSurface(PACKED_ICE) || !blockIsPlantableSurface(MUD) ||
			blockIsPlantableSurface(STONE) || blockIsPlantableSurface(WATER))
			ok = fail("blockIsPlantableSurface classification wrong");
		if (!blockIsOreHost(STONE) || !blockIsOreHost(DEEPSLATE) || !blockIsOreHost(ANDESITE) ||
			blockIsOreHost(DIRT) || blockIsOreHost(COAL_ORE))
			ok = fail("blockIsOreHost classification wrong");
		if (!blockIsCactusGround(SAND) || !blockIsCactusGround(RED_SAND) ||
			!blockIsCactusGround(ORANGE_TERRACOTTA) || blockIsCactusGround(GRASS_TOP))
			ok = fail("blockIsCactusGround classification wrong");
		if (!blockTransmitsSkyLight(AIR) || !blockTransmitsSkyLight(WATER) ||
			!blockTransmitsSkyLight(GLASS) || !blockTransmitsSkyLight(ICE) ||
			!blockTransmitsSkyLight(BIRCH_LEAVES) || !blockTransmitsSkyLight(BLUE_ICE) ||
			blockTransmitsSkyLight(STONE) || blockTransmitsSkyLight(PACKED_ICE))
			ok = fail("blockTransmitsSkyLight must match transparent layers + AIR");
		// Stable ordinals for pre-expansion types
		if (static_cast<int>(WATER) != 24)
			ok = fail("WATER ordinal must remain 24 (append-only expansion)");
		if (static_cast<int>(BIRCH_LOG) != 25)
			ok = fail("BIRCH_LOG must be first appended type (25)");

		// ResourcePackReader + ZIP reading (default pack)
		namespace fs = std::filesystem;
		const char *defaultPackCandidates[] = {
			"ressources/default-resource-pack.zip",
			"../ressources/default-resource-pack.zip",
			"../../ressources/default-resource-pack.zip",
		};
		std::string foundZip;
		for (const char *candidate : defaultPackCandidates)
		{
			if (fs::exists(candidate))
			{
				foundZip = candidate;
				break;
			}
		}

		if (!foundZip.empty())
		{
			ResourcePackReader reader(foundZip);
			if (!reader.isValid())
				ok = fail("ResourcePackReader failed to open default-resource-pack.zip");

			std::vector<uint8_t> pngData;
			if (!reader.readBlockTexture("stone.png", pngData) || pngData.empty())
				ok = fail("ResourcePackReader failed to read stone.png from default-resource-pack.zip");

			pngData.clear();
			if (!reader.readBlockTexture("birch_leaves.png", pngData) || pngData.empty())
				ok = fail("ResourcePackReader failed to read birch_leaves.png from default-resource-pack.zip");
		}
	}

	// Animation frame size helper
	{
		int fw = 0, fh = 0;
		blockTextureFrameSize(16, 512, fw, fh);
		if (fw != 16 || fh != 16)
			ok = fail("water strip 16x512 first frame must be 16x16");
		blockTextureFrameSize(16, 16, fw, fh);
		if (fw != 16 || fh != 16)
			ok = fail("square 16x16 frame size must stay 16x16");
		blockTextureFrameSize(64, 64, fw, fh);
		if (fw != 64 || fh != 64)
			ok = fail("square 64x64 frame size must stay 64x64");
		blockTextureFrameSize(64, 2048, fw, fh);
		if (fw != 64 || fh != 64)
			ok = fail("strip 64x2048 first frame must be 64x64");
	}

	// Pack load report classification (shipped TextureAtlasLoadReport)
	{
		TextureAtlasLoadReport none{};
		if (none.packInvalid() || none.packIncomplete())
			ok = fail("empty report must not be invalid/incomplete");
		if (!none.packComplete())
			ok = fail("empty report (no pack requested) is complete");

		TextureAtlasLoadReport invalid{};
		invalid.packRequested = true;
		invalid.requiredLayers = 25;
		invalid.packHits = 0;
		invalid.packMisses = 25;
		if (!invalid.packInvalid())
			ok = fail("zero hits with pack requested must be packInvalid");
		if (invalid.packIncomplete())
			ok = fail("fully missing pack is invalid, not incomplete");

		TextureAtlasLoadReport partial{};
		partial.packRequested = true;
		partial.requiredLayers = 25;
		partial.packHits = 20;
		partial.packMisses = 5;
		if (!partial.packIncomplete() || partial.packInvalid() || partial.packComplete())
			ok = fail("partial hits must be packIncomplete only");

		TextureAtlasLoadReport full{};
		full.packRequested = true;
		full.requiredLayers = 25;
		full.packHits = 25;
		full.packMisses = 0;
		if (!full.packComplete() || full.packInvalid() || full.packIncomplete())
			ok = fail("full pack must be packComplete only");
	}

	// Generated FrameUBO GLSL must list C++ field names (build artifact or source mirror)
	{
		namespace fs = std::filesystem;
		const char *candidates[] = {
			"ressources/shaders/vulkan/frame_ubo.inc.glsl",
			"../ressources/shaders/vulkan/frame_ubo.inc.glsl",
			"../../ressources/shaders/vulkan/frame_ubo.inc.glsl",
			"generated/shaders/frame_ubo.inc.glsl",
			"../generated/shaders/frame_ubo.inc.glsl",
		};
		std::string glsl;
		for (const char *c : candidates)
		{
			std::ifstream in(c);
			if (in)
			{
				glsl.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
				break;
			}
		}
		if (glsl.empty())
			ok = fail("generated frame_ubo.inc.glsl not found (run cmake build first)");
		else
		{
			if (glsl.find("AUTO-GENERATED") == std::string::npos)
				ok = fail("frame_ubo.inc.glsl must be marked AUTO-GENERATED");
			// Spot-check fields from FrameUBO.hpp
			for (const char *field : {"mat4 view", "mat4 projection", "vec4 lightingParams", "vec4 waterParams",
									  "vec4 cascadeSplits", "vec4 moonAmbient"})
			{
				if (glsl.find(field) == std::string::npos)
					ok = fail(std::string("generated FrameUBO GLSL missing field: ") + field);
			}
			if (glsl.find("postParams") != std::string::npos)
				ok = fail("generated FrameUBO must not contain dead postParams");
		}
	}

	// --- Indirect batching contract (issue #109 review) ---
	{
		// Batch-limit policy: no multiDrawIndirect -> exactly one command
		// per draw, whatever the hardware claims.
		if (indirectBatchLimit(false, 65535u) != 1)
			ok = fail("indirectBatchLimit without multiDrawIndirect must be 1");
		if (indirectBatchLimit(false, 0u) != 1)
			ok = fail("indirectBatchLimit(false, 0) must still be 1");
		// Degenerate hardware values can never produce a 0 limit.
		if (indirectBatchLimit(true, 0u) != 1)
			ok = fail("indirectBatchLimit(true, 0) must clamp to 1");
		if (indirectBatchLimit(true, 4096u) != 4096u)
			ok = fail("indirectBatchLimit(true, 4096) must pass the hardware limit through");

		const struct
		{
			size_t total;
			uint32_t maxBatch;
			size_t expectedBatches;
		} cases[] = {
			{0, 64, 0},     {1, 1, 1},        {10, 1, 10},
			{10, 4, 3},     {4096, 4096, 1},  {4097, 4096, 2},
		};
		for (const auto &c : cases)
		{
			const std::vector<uint32_t> counts =
				splitIndirectBatchCounts(c.total, c.maxBatch);
			if (counts.size() != c.expectedBatches)
				ok = fail("splitIndirectBatchCounts(" + std::to_string(c.total) + ", " +
				          std::to_string(c.maxBatch) + ") produced " +
				          std::to_string(counts.size()) + " batches, expected " +
				          std::to_string(c.expectedBatches));
			size_t sum = 0;
			for (const uint32_t batch : counts)
			{
				if (batch < 1 || batch > c.maxBatch)
					ok = fail("batch outside [1, maxBatch] for total=" +
					          std::to_string(c.total) + " maxBatch=" +
					          std::to_string(c.maxBatch));
				sum += batch;
			}
			if (sum != c.total)
				ok = fail("split batches must sum to the command count");
		}
		// Exact shapes from the review plan.
		{
			const auto big = splitIndirectBatchCounts(4097, 4096);
			if (big.size() != 2 || big[0] != 4096u || big[1] != 1u)
				ok = fail("4097/4096 must split into [4096, 1]");
			const auto ones = splitIndirectBatchCounts(10, 1);
			if (ones.size() != 10)
				ok = fail("10/1 must split into ten single-command batches");
		}
	}

	// --- Packed Vertex & VoxelDrawData (issue #110) ---
	{
		static_assert(sizeof(Vertex) == 16, "Vertex must be 16 bytes");
		static_assert(sizeof(VoxelDrawData) == 16, "VoxelDrawData must be 16 bytes for std430");

		// Test exact integer boundaries on X, Y, Z
		for (int x = 0; x <= 16; ++x)
		{
			for (int z = 0; z <= 16; ++z)
			{
				for (int y = 0; y <= 256; y += 16)
				{
					const glm::vec3 pos(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
					const uint32_t packed = Vertex::packPosition(pos);
					const glm::vec3 decoded = Vertex::unpackPosition(packed);
					if (decoded != pos)
					{
						ok = fail("Integer coordinate (" + std::to_string(x) + ", " +
						          std::to_string(y) + ", " + std::to_string(z) +
						          ") failed round-trip");
					}
				}
			}
		}

		// Test plant detail coordinates (1/16 fractions)
		const glm::vec3 plantOffsets[] = {
			{1.f / 16.f, 1.f / 16.f, 1.f / 16.f},
			{15.f / 16.f, 1.f / 16.f, 15.f / 16.f},
			{2.f / 16.f, 0.f, 2.f / 16.f},
			{14.f / 16.f, 14.f / 16.f, 14.f / 16.f},
		};
		for (const auto &offset : plantOffsets)
		{
			const glm::vec3 pos = glm::vec3(5.f, 64.f, 7.f) + offset;
			const uint32_t packed = Vertex::packPosition(pos);
			const glm::vec3 decoded = Vertex::unpackPosition(packed);
			if (glm::length(decoded - pos) > 1e-5f)
				ok = fail("Plant fraction failed round-trip");
		}

		// Test UV coordinate precision
		Vertex v{};
		v.texCoordU = 16;
		v.texCoordV = 256;
		const glm::vec2 uv = v.decodeTexCoord();
		if (uv.x != 16.f || uv.y != 256.f)
			ok = fail("UV decode failed");

		// Test large world origin + local position reconstruction
		const glm::ivec3 worldOrigin(100000, 0, -500000);
		const glm::vec3 localPos(8.f, 128.f, 8.f);
		v.packedPos = Vertex::packPosition(localPos);
		const glm::vec3 worldPos = v.decodePosition(glm::vec3(worldOrigin));
		const glm::vec3 expectedWorld = glm::vec3(worldOrigin) + localPos;
		if (worldPos != expectedWorld)
			ok = fail("Large world coordinate reconstruction failed");

		// Format contract maxima: the largest geometry the mesher can emit
		// (greedy endpoints at chunk boundaries, full chunk height) must
		// round-trip exactly through the packed position.
		const glm::vec3 contractMax(CHUNK_SIZE, CHUNK_HEIGHT, CHUNK_SIZE);
		if (Vertex::unpackPosition(Vertex::packPosition(contractMax)) != contractMax)
			ok = fail("Packed position contract maximum (16, 256, 16) failed round-trip");
		static_assert(Vertex::kXMax >= CHUNK_SIZE * 16, "X range covers chunk width");
		static_assert(Vertex::kYMax >= CHUNK_HEIGHT * 16, "Y range covers chunk height");
		static_assert(Vertex::kZMax >= CHUNK_SIZE * 16, "Z range covers chunk width");
	}

	// --- Color space and sRGB output transfer helpers (issue #135) ---
	{
		using namespace colorspace;

		// 1. sRGB decode known points
		if (std::abs(srgbToLinear(0.0f) - 0.0f) > 1e-6f)
			ok = fail("srgbToLinear(0.0) must be 0.0");
		if (std::abs(srgbToLinear(1.0f) - 1.0f) > 1e-6f)
			ok = fail("srgbToLinear(1.0) must be 1.0");

		// 8-bit mid-gray 128 / 255 ~= 0.50196078 -> linear light ~= 0.2158605
		const float midGraySrgb = 128.0f / 255.0f;
		const float midGrayLinear = srgbToLinear(midGraySrgb);
		if (std::abs(midGrayLinear - 0.2158605f) > 1e-4f)
			ok = fail("srgbToLinear mid-gray (128/255) must decode to ~0.21586");

		// Linear toe (< 0.04045): 0.02 / 12.92 ~= 0.0015479876
		const float toeLinear = srgbToLinear(0.02f);
		if (std::abs(toeLinear - (0.02f / 12.92f)) > 1e-6f)
			ok = fail("srgbToLinear linear toe (< 0.04045) mismatch");

		// 2. sRGB encode known points
		if (std::abs(linearToSrgb(0.0f) - 0.0f) > 1e-6f)
			ok = fail("linearToSrgb(0.0) must be 0.0");
		if (std::abs(linearToSrgb(1.0f) - 1.0f) > 1e-6f)
			ok = fail("linearToSrgb(1.0) must be 1.0");
		if (std::abs(linearToSrgb(0.2158605f) - midGraySrgb) > 1e-4f)
			ok = fail("linearToSrgb(0.21586) must encode back to ~0.50196");
		if (std::abs(linearToSrgb(toeLinear) - 0.02f) > 1e-6f)
			ok = fail("linearToSrgb linear toe (< 0.0031308) mismatch");

		// 3. Round-trip accuracy across [0, 1]
		for (int i = 0; i <= 255; ++i)
		{
			const float srgb = static_cast<float>(i) / 255.0f;
			const float lin = srgbToLinear(srgb);
			const float roundtrip = linearToSrgb(lin);
			if (std::abs(roundtrip - srgb) > 1e-4f)
				ok = fail("sRGB round-trip error exceeded 1e-4 at 8-bit index " + std::to_string(i));
		}

		// Vector overloads
		const glm::vec3 srgbV(0.0f, midGraySrgb, 1.0f);
		const glm::vec3 linV = srgbToLinear(srgbV);
		if (std::abs(linV.x - 0.0f) > 1e-6f || std::abs(linV.y - midGrayLinear) > 1e-4f ||
			std::abs(linV.z - 1.0f) > 1e-6f)
			ok = fail("srgbToLinear(vec3) component mismatch");

		const glm::vec3 srgbRoundtrip = linearToSrgb(linV);
		if (glm::length(srgbRoundtrip - srgbV) > 1e-4f)
			ok = fail("linearToSrgb(vec3) round-trip mismatch");

		// 4. Format classification policy
		if (kAlbedoTextureFormat != VK_FORMAT_R8G8B8A8_SRGB)
			ok = fail("kAlbedoTextureFormat must be VK_FORMAT_R8G8B8A8_SRGB");
		if (!isAlbedoColorFormat(VK_FORMAT_R8G8B8A8_SRGB) || !isAlbedoColorFormat(VK_FORMAT_B8G8R8A8_SRGB))
			ok = fail("sRGB formats must be recognized as valid albedo formats");
		if (isAlbedoColorFormat(VK_FORMAT_R8G8B8A8_UNORM) || isAlbedoColorFormat(VK_FORMAT_B8G8R8A8_UNORM))
			ok = fail("UNORM formats must not be accepted as albedo color formats without conversion");

		// 5. Non-color data formats policy
		if (!isNonColorDataFormat(VK_FORMAT_D32_SFLOAT) ||
			!isNonColorDataFormat(VK_FORMAT_R16G16B16A16_SFLOAT) ||
			!isNonColorDataFormat(VK_FORMAT_R8_UNORM))
			ok = fail("Depth, HDR color buffer, and SSAO targets must be classified as non-color data");
		if (isNonColorDataFormat(VK_FORMAT_R8G8B8A8_SRGB))
			ok = fail("sRGB format cannot be non-color data");

		// 6. Swapchain output transfer policy
		if (swapchainRequiresShaderOutputTransfer(VK_FORMAT_B8G8R8A8_SRGB) ||
			swapchainRequiresShaderOutputTransfer(VK_FORMAT_R8G8B8A8_SRGB))
			ok = fail("sRGB swapchain must NOT require shader output transfer (attachment does it)");
		if (!swapchainRequiresShaderOutputTransfer(VK_FORMAT_B8G8R8A8_UNORM) ||
			!swapchainRequiresShaderOutputTransfer(VK_FORMAT_R8G8B8A8_UNORM))
			ok = fail("UNORM fallback swapchain MUST require shader output transfer");

		// 7. End-to-end simulation: sRGB swapchain vs UNORM fallback parity
		const glm::vec3 testColors[] = {
			glm::vec3(0.0f),
			glm::vec3(0.21586f),                   // mid-gray
			glm::vec3(0.12f, 0.55f, 0.22f),        // foliage green
			glm::vec3(0.85f, 0.72f, 0.45f),        // sand
			glm::vec3(0.02f, 0.03f, 0.08f),        // dark cave / night
			glm::vec3(1.0f)                        // peak white
		};
		for (const auto &c : testColors)
		{
			const glm::vec3 outSrgb = simulateDisplayOutput(c, kNeutralGamma, VK_FORMAT_B8G8R8A8_SRGB);
			const glm::vec3 outUnorm = simulateDisplayOutput(c, kNeutralGamma, VK_FORMAT_B8G8R8A8_UNORM);
			if (glm::length(outSrgb - outUnorm) > 1e-4f)
				ok = fail("Discrepancy between sRGB swapchain and UNORM swapchain display output");

			// Verify that output matches exactly one sRGB encode:
			const glm::vec3 expected = linearToSrgb(c);
			if (glm::length(outSrgb - expected) > 1e-4f)
				ok = fail("Output does not match standard single sRGB transfer");
		}

		// 8. Creative gamma operator neutrality
		const glm::vec3 sample(0.35f, 0.62f, 0.18f);
		if (glm::length(applyArtisticGamma(sample, 1.0f) - sample) > 1e-6f)
			ok = fail("applyArtisticGamma with gamma=1.0 must be exact identity");
		if (std::abs(applyArtisticGamma(0.5f, 1.0f) - 0.5f) > 1e-6f)
			ok = fail("applyArtisticGamma(float) with gamma=1.0 must be exact identity");

		// Proves that old double-gamma bug significantly distorted output:
		// e.g. for linear mid-gray 0.21586, correct sRGB is 0.50196,
		// but double-transfer produced ~0.735 (46% brighter!)
		const float oldDoubleTransfer = linearToSrgb(std::pow(midGrayLinear, 1.0f / 2.2f));
		if (std::abs(oldDoubleTransfer - midGraySrgb) < 0.15f)
			ok = fail("Old transfer was not distinctively washed out (double gamma check)");
	}

	if (!ok)
	{
		std::cerr << "test_render_helpers: FAILED\n";
		return EXIT_FAILURE;
	}
	std::cout << "test_render_helpers: OK (cascades + fog + lighting + materials + block textures + FrameUBO + indirect batching + colorspace)\n";
	return EXIT_SUCCESS;
}
