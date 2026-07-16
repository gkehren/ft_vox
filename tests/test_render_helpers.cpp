// Unit tests for shipped render helpers (cascade / fog / moon / emissive / block light).
// Cascade path exercises the same frustum-slice builders WorldRenderer packs into FrameUBO.

#include <Renderer/ShadowCascades.hpp>
#include <Renderer/Lighting.hpp>
#include <Renderer/FrameUBO.hpp>
#include <Renderer/MaterialTable.hpp>
#include <Engine/EngineDefs.hpp>
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

	if (!ok)
	{
		std::cerr << "test_render_helpers: FAILED\n";
		return EXIT_FAILURE;
	}
	std::cout << "test_render_helpers: OK (cascades + fog + lighting + materials + FrameUBO)\n";
	return EXIT_SUCCESS;
}
