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
#include <Renderer/ScreenSpace.hpp>
#include <Renderer/TextureMips.hpp>
// stb_image decodes bundled pack PNGs byte-exactly for the glass/ice mip
// contract (mip block 13). This test owns the IMPLEMENTATION translation
// unit: no other stb-using .cpp is compiled into test_render_helpers.
#ifndef STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#endif
#include <stb_image/stb_image.h>
#include <Engine/EngineDefs.hpp>
#include <fstream>
#include <filesystem>
#include <utils.hpp>

#include <chrono>
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

		// Issue #137 resolution contract: doubling the shadow map resolution
		// must halve the world-units-per-texel footprint the receivers use.
		{
			std::array<glm::mat4, shadow::kCascadeCount> matsHi{};
			std::array<float, shadow::kCascadeCount> extentsHi{};
			glm::vec4 splitsHi{};
			shadow::buildCascadeUBOFromFront(camPos, front, glm::vec3(0.f, 1.f, 0.f), lightDir,
											nearP, farP, 16.f / 9.f, shadow::kDefaultFovYDegrees,
											matsHi, splitsHi, &extentsHi, shadow::kShadowMapSize * 2);
			for (int c = 0; c < shadow::kCascadeCount; ++c)
			{
				const float lowTexel = extents[size_t(c)] / float(shadow::kShadowMapSize);
				const float highTexel = extentsHi[size_t(c)] / float(shadow::kShadowMapSize * 2);
				if (!(highTexel < lowTexel * 0.75f))
					ok = fail("cascade world-units-per-texel must shrink with resolution (cascade " +
							  std::to_string(c) + ")");
			}
		}

		// Issue #137 swimming contract: a fixed world point keeps its
		// ABSOLUTE shadow texel across camera motion. When the cascade
		// recenters, the local texel shifts by -N while the grid origin
		// shifts by +N — the test must actually CROSS a grid boundary
		// (local texel changes) and still find the absolute texel intact;
		// that identity is exactly what the Poisson rotation hash consumes.
		{
			const glm::vec3 worldPoint = camPos + front * 20.f + glm::vec3(1.f, 0.f, 0.f);
			auto texelAt = [&](const glm::vec3 &cam, const glm::vec3 &f) {
				std::array<glm::mat4, shadow::kCascadeCount> m{};
				glm::vec4 s{};
				std::array<glm::ivec2, shadow::kCascadeCount> grids{};
				shadow::buildCascadeUBOFromFront(cam, f, glm::vec3(0.f, 1.f, 0.f), lightDir,
												nearP, farP, 16.f / 9.f, shadow::kDefaultFovYDegrees, m, s,
												nullptr, shadow::kShadowMapSize, nullptr, &grids);
				const glm::vec3 ls = glm::vec3(m[0] * glm::vec4(worldPoint, 1.f));
				glm::ivec2 local = {int(std::floor((ls.x * 0.5f + 0.5f) * float(shadow::kShadowMapSize))),
									int(std::floor((ls.y * 0.5f + 0.5f) * float(shadow::kShadowMapSize)))};
				return std::pair<glm::ivec2, glm::ivec2>{local, grids[0]};
			};
			const auto [localA, gridA] = texelAt(camPos, front);
			const auto [localT, gridT] = texelAt(camPos + glm::vec3(0.15f, 0.f, 0.1f), front);
			const glm::vec3 rotatedFront = glm::normalize(front + glm::vec3(0.1f, 0.f, 0.f));
			const auto [localR, gridR] = texelAt(camPos, rotatedFront);
			if (localA == localT || localA == localR)
				ok = fail("shadow-grid boundary was not crossed by the test motion — test is vacuous");
			if (localA + gridA != localT + gridT || localA + gridA != localR + gridR)
				ok = fail("absolute shadow texel of a fixed world point changed across camera motion");
		}

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

		// Issue #137 receiver-bias contract: dimensionless factors applied to
		// FrameUBO::cascadeBiasScales (normalized depth per world texel).
		if (!(shadow::kReceiverBiasSlope > shadow::kReceiverBiasBase && shadow::kReceiverBiasBase > 0.f))
			ok = fail("kReceiverBiasSlope/Base must be positive with slope > base");

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

	// --- NDC -> framebuffer/post UV viewport convention (issue #158) ---
	{
		using screenspace::ndcToFramebufferUv;

		// Canonical points: the negative-height production viewport puts NDC
		// +y at the TOP of the framebuffer (post UV v = 0). The old
		// positive-viewport form (ndc.y * 0.5 + 0.5) mapped each of these to
		// the mirrored v and aimed the god-ray scattering center at the
		// mirrored sun.
		const struct
		{
			glm::vec2 ndc;
			glm::vec2 uv;
		} points[] = {
			{{0.f, 1.f}, {0.5f, 0.0f}},  // NDC top edge -> framebuffer row 0
			{{0.f, -1.f}, {0.5f, 1.0f}}, // NDC bottom edge -> last row
			{{1.f, 0.f}, {1.0f, 0.5f}},  // x passes through unflipped
			{{-1.f, 0.f}, {0.0f, 0.5f}},
			{{0.f, 0.f}, {0.5f, 0.5f}}, // center is fixed
		};
		for (const auto &p : points)
		{
			const glm::vec2 uv = ndcToFramebufferUv(p.ndc);
			if (glm::length(uv - p.uv) > 1e-6f)
				ok = fail("ndcToFramebufferUv((" + std::to_string(p.ndc.x) + ", " + std::to_string(p.ndc.y) +
						  ")) must be (" + std::to_string(p.uv.x) + ", " + std::to_string(p.uv.y) + "), got (" +
						  std::to_string(uv.x) + ", " + std::to_string(uv.y) + ")");
		}

		// Derived from the actual viewport state (OpaquePass/WaterPass/SkyPass
		// set {0, height, width, -height, 0, 1}): the Vulkan viewport transform
		// is px = vpX + (ndc.x + 1) * vpW / 2, py = vpY + (ndc.y + 1) * vpH / 2,
		// so the helper must agree with the negative-height transform exactly.
		{
			const float width = 640.f, height = 360.f;
			const float vpY = height, vpH = -height;
			for (const float ndcY : {-1.f, -0.5f, 0.f, 0.5f, 1.f})
			{
				const float uvY = (vpY + (ndcY + 1.f) * vpH * 0.5f) / height;
				if (std::abs(uvY - ndcToFramebufferUv({0.f, ndcY}).y) > 1e-6f)
					ok = fail("helper must match the negative-height viewport transform for ndc.y " +
							  std::to_string(ndcY));
			}
		}

		// Monotonicity / no horizontal flip: larger NDC y moves UP the
		// framebuffer (smaller v); x is passthrough.
		if (!(ndcToFramebufferUv({0.f, 0.5f}).y < ndcToFramebufferUv({0.f, -0.5f}).y))
			ok = fail("larger NDC y must map to a smaller (higher) framebuffer v");
		if (!(ndcToFramebufferUv({-0.5f, 0.f}).x < ndcToFramebufferUv({0.5f, 0.f}).x))
			ok = fail("NDC x must map monotonically to u (no horizontal flip)");

		// Tied to the real projection: a point above the view axis (world +y,
		// horizontal camera) must land in the TOP half of the framebuffer —
		// the same half the negative-height viewport rasterizes NDC +y into.
		{
			const glm::mat4 proj = glm::perspective(glm::radians(80.f), 16.f / 9.f, 0.1f, 400.f);
			const glm::mat4 view = glm::lookAt(glm::vec3(0.f), glm::vec3(0.f, 0.f, -1.f), glm::vec3(0.f, 1.f, 0.f));
			const glm::vec4 clip = proj * view * glm::vec4(0.f, 40.f, -80.f, 1.f);
			const glm::vec2 ndc = glm::vec2(clip) / clip.w;
			const glm::vec2 uv = ndcToFramebufferUv(ndc);
			if (!(ndc.y > 0.f) || !(uv.y < 0.5f))
				ok = fail("a point above the view axis must project into the top framebuffer half (uv.y " +
						  std::to_string(uv.y) + ")");
			if (uv.x < 0.f || uv.x > 1.f || uv.y < 0.f || uv.y > 1.f)
				ok = fail("in-frustum projection must stay inside [0, 1] post UVs");
		}
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

	// --- Emissive / block light ---
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

		const uint32_t packed = lighting::packLightBits(15, 10, 8, 2);
		uint8_t sky = 0, blockR = 0, blockG = 0, blockB = 0;
		lighting::unpackLightBits(packed, sky, blockR, blockG, blockB);
		if (sky != 15 || blockR != 10 || blockG != 8 || blockB != 2)
			ok = fail("pack/unpack RGB light bits round-trip failed");
		if (lighting::packLightBitsRGB4(15, lighting::packBlockLightRGB4(10, 8, 2)) != packed)
			ok = fail("packLightBitsRGB4 must alias the four-argument packLightBits");
	}

	// --- Colored block light (issue #141) ---
	{
		// Source policy: non-emissive blocks are not light sources.
		const auto stone = lighting::blockLightSourceForBlock(static_cast<uint8_t>(STONE));
		if (stone.intensity != 0)
			ok = fail("stone must not be a colored block light source");
		if (stone.colorLinear.r != 0.f || stone.colorLinear.g != 0.f || stone.colorLinear.b != 0.f)
			ok = fail("stone block light source color must be black");

		// Per-type color identity (linear-space semantic hues).
		const auto lava = lighting::blockLightSourceForBlock(static_cast<uint8_t>(LAVA));
		if (lava.intensity != 15)
			ok = fail("lava must be a full-intensity (15) block light source");
		if (!(lava.colorLinear.r > lava.colorLinear.g && lava.colorLinear.g > lava.colorLinear.b))
			ok = fail("lava block light must be warm orange/red (r > g > b)");
		const auto magma = lighting::blockLightSourceForBlock(static_cast<uint8_t>(MAGMA));
		if (magma.intensity != 13)
			ok = fail("magma must be an intensity-13 block light source");
		if (!(magma.colorLinear.r > magma.colorLinear.g && magma.colorLinear.r > magma.colorLinear.b &&
			  magma.colorLinear.b < magma.colorLinear.g))
			ok = fail("magma block light must be orange (r max, b smallest)");
		const auto redstone = lighting::blockLightSourceForBlock(static_cast<uint8_t>(REDSTONE_ORE));
		if (!(redstone.colorLinear.r > redstone.colorLinear.g &&
			  redstone.colorLinear.r > redstone.colorLinear.b))
			ok = fail("redstone ore block light must be red-dominant");
		const auto lapis = lighting::blockLightSourceForBlock(static_cast<uint8_t>(LAPIS_ORE));
		if (!(lapis.colorLinear.b > lapis.colorLinear.g && lapis.colorLinear.g > lapis.colorLinear.r))
			ok = fail("lapis ore block light must be blue-dominant (b > g > r)");
		const auto diamond = lighting::blockLightSourceForBlock(static_cast<uint8_t>(DIAMOND_ORE));
		if (!(diamond.colorLinear.b >= diamond.colorLinear.g &&
			  diamond.colorLinear.g > diamond.colorLinear.r))
			ok = fail("diamond ore block light must be cyan-ish (b >= g > r)");
		const auto gold = lighting::blockLightSourceForBlock(static_cast<uint8_t>(GOLD_ORE));
		if (!(gold.colorLinear.r > gold.colorLinear.g && gold.colorLinear.g > gold.colorLinear.b))
			ok = fail("gold ore block light must be warm yellow (r > g > b)");

		// blockLightEmission is defined as the source intensity.
		for (const uint8_t type : {static_cast<uint8_t>(LAVA), static_cast<uint8_t>(MAGMA),
								   static_cast<uint8_t>(REDSTONE_ORE), static_cast<uint8_t>(LAPIS_ORE),
								   static_cast<uint8_t>(DIAMOND_ORE), static_cast<uint8_t>(EMERALD_ORE),
								   static_cast<uint8_t>(GOLD_ORE), static_cast<uint8_t>(STONE)})
		{
			if (lighting::blockLightEmission(type) != lighting::blockLightSourceForBlock(type).intensity)
				ok = fail("blockLightEmission must equal the source intensity for type " +
						  std::to_string(static_cast<int>(type)));
		}

		// Quantized RGB4 emission: channel = clamp(lround(color * intensity), 0, 15)
		// with round-half-away-from-zero (0.30f * 15 = 4.5 -> 5, 0.08f * 15 = 1.2 -> 1).
		if (lighting::blockLightEmissionRGB4(static_cast<uint8_t>(LAVA)) !=
			lighting::packBlockLightRGB4(15, 5, 1))
			ok = fail("lava RGB4 emission must be (15, 5, 1)");
		if (lighting::blockLightEmissionRGB4(static_cast<uint8_t>(MAGMA)) !=
			lighting::packBlockLightRGB4(13, 6, 2))
			ok = fail("magma RGB4 emission must be (13, 6, 2)");
		if (lighting::blockLightEmissionRGB4(static_cast<uint8_t>(REDSTONE_ORE)) !=
			lighting::packBlockLightRGB4(14, 2, 1))
			ok = fail("redstone ore RGB4 emission must be (14, 2, 1)");
		if (lighting::blockLightEmissionRGB4(static_cast<uint8_t>(LAPIS_ORE)) !=
			lighting::packBlockLightRGB4(1, 3, 7))
			ok = fail("lapis ore RGB4 emission must be (1, 3, 7)");
		if (lighting::blockLightEmissionRGB4(static_cast<uint8_t>(DIAMOND_ORE)) !=
			lighting::packBlockLightRGB4(2, 3, 4))
			ok = fail("diamond ore RGB4 emission must be (2, 3, 4)");
		if (lighting::blockLightEmissionRGB4(static_cast<uint8_t>(EMERALD_ORE)) !=
			lighting::packBlockLightRGB4(1, 3, 1))
			ok = fail("emerald ore RGB4 emission must be (1, 3, 1)");
		// 0.25f * 2 = 0.5 exactly -> lround rounds half away from zero -> 1.
		if (lighting::blockLightEmissionRGB4(static_cast<uint8_t>(GOLD_ORE)) !=
			lighting::packBlockLightRGB4(2, 1, 1))
			ok = fail("gold ore RGB4 emission must be (2, 1, 1)");
		if (lighting::blockLightEmissionRGB4(static_cast<uint8_t>(STONE)) != 0)
			ok = fail("stone RGB4 emission must be 0");
		for (const uint8_t type : {static_cast<uint8_t>(LAVA), static_cast<uint8_t>(MAGMA),
								   static_cast<uint8_t>(REDSTONE_ORE), static_cast<uint8_t>(LAPIS_ORE),
								   static_cast<uint8_t>(DIAMOND_ORE), static_cast<uint8_t>(EMERALD_ORE),
								   static_cast<uint8_t>(GOLD_ORE)})
		{
			uint8_t er = 0, eg = 0, eb = 0;
			lighting::unpackBlockLightRGB4(lighting::blockLightEmissionRGB4(type), er, eg, eb);
			if (er > 15 || eg > 15 || eb > 15)
				ok = fail("RGB4 emission channels must stay within 4 bits for type " +
						  std::to_string(static_cast<int>(type)));
		}

		// RGB4 pack/unpack round trip + 4-bit quantization bounds.
		{
			uint8_t r = 0, g = 0, b = 0;
			lighting::unpackBlockLightRGB4(lighting::packBlockLightRGB4(15, 7, 0), r, g, b);
			if (r != 15 || g != 7 || b != 0)
				ok = fail("RGB4 pack/unpack round-trip failed for (15, 7, 0)");
			lighting::unpackBlockLightRGB4(lighting::packBlockLightRGB4(1, 2, 3), r, g, b);
			if (r != 1 || g != 2 || b != 3)
				ok = fail("RGB4 pack/unpack round-trip failed for (1, 2, 3)");
			lighting::unpackBlockLightRGB4(lighting::packBlockLightRGB4(16, 255, 0), r, g, b);
			if (r != 0 || g != 15 || b != 0)
				ok = fail("RGB4 pack must mask channels to 4 bits (16 -> 0, 255 -> 15)");
		}

		// Overlap combination policy: per-channel max, order-independent.
		{
			const uint16_t a = lighting::packBlockLightRGB4(15, 5, 1);
			const uint16_t b = lighting::packBlockLightRGB4(3, 9, 0);
			uint8_t r = 0, g = 0, bch = 0;
			lighting::unpackBlockLightRGB4(lighting::maxBlockLightRGB4(a, b), r, g, bch);
			if (r != 15 || g != 9 || bch != 1)
				ok = fail("maxBlockLightRGB4 must combine per-channel maxima");
			if (lighting::maxBlockLightRGB4(a, b) != lighting::maxBlockLightRGB4(b, a))
				ok = fail("maxBlockLightRGB4 must be commutative");
			const uint16_t c = lighting::packBlockLightRGB4(0, 15, 7);
			if (lighting::maxBlockLightRGB4(lighting::maxBlockLightRGB4(a, b), c) !=
				lighting::maxBlockLightRGB4(a, lighting::maxBlockLightRGB4(b, c)))
				ok = fail("maxBlockLightRGB4 must be associative");
			if (lighting::maxBlockLightRGB4(a, 0) != a)
				ok = fail("maxBlockLightRGB4(v, 0) must be v");
		}

		// Attenuation: every channel decays by 1 toward 0, clamped at 0.
		{
			uint8_t r = 0, g = 0, b = 0;
			const uint16_t full = lighting::packBlockLightRGB4(15, 5, 1);
			lighting::unpackBlockLightRGB4(lighting::attenuateBlockLightRGB4(full), r, g, b);
			if (r != 14 || g != 4 || b != 0)
				ok = fail("attenuateBlockLightRGB4 must decay each channel by 1 toward 0");
			uint16_t small = lighting::packBlockLightRGB4(2, 1, 0);
			small = lighting::attenuateBlockLightRGB4(small);
			if (small != lighting::packBlockLightRGB4(1, 0, 0))
				ok = fail("attenuating (2, 1, 0) once must give (1, 0, 0)");
			small = lighting::attenuateBlockLightRGB4(small);
			if (small != 0)
				ok = fail("attenuating (2, 1, 0) twice must reach 0");
			if (lighting::attenuateBlockLightRGB4(small) != 0)
				ok = fail("attenuating zero light must stay zero");
		}

		// Scalar luminance proxy = max channel.
		{
			if (lighting::blockLightLumaRGB4(lighting::packBlockLightRGB4(15, 5, 1)) != 15)
				ok = fail("blockLightLumaRGB4 must return the max channel (R max)");
			if (lighting::blockLightLumaRGB4(lighting::packBlockLightRGB4(3, 9, 7)) != 9)
				ok = fail("blockLightLumaRGB4 must return the max channel (G max)");
			if (lighting::blockLightLumaRGB4(lighting::packBlockLightRGB4(3, 9, 12)) != 12)
				ok = fail("blockLightLumaRGB4 must return the max channel (B max)");
			if (lighting::blockLightLumaRGB4(0) != 0)
				ok = fail("blockLightLumaRGB4(0) must be 0");
		}

		// Vertex packedData bit placement (bits 14-17 sky, 18/22/26 block RGB).
		{
			const uint32_t raw = lighting::packLightBits(9, 1, 12, 15);
			uint8_t sky = 0, r = 0, g = 0, b = 0;
			lighting::unpackLightBits(raw, sky, r, g, b);
			if (sky != 9 || r != 1 || g != 12 || b != 15)
				ok = fail("pack/unpack light bits round-trip failed for (9, 1, 12, 15)");
			if (((raw >> 14) & 0xFu) != 9u)
				ok = fail("sky light nibble must sit at packedData bits 14-17");
			if (((raw >> 18) & 0xFu) != 1u)
				ok = fail("block R nibble must sit at packedData bits 18-21");
			if (((raw >> 22) & 0xFu) != 12u)
				ok = fail("block G nibble must sit at packedData bits 22-25");
			if (((raw >> 26) & 0xFu) != 15u)
				ok = fail("block B nibble must sit at packedData bits 26-29");
			if (((raw >> 30) & 0x3u) != 0u)
				ok = fail("top two packedData bits must stay spare (zero)");
		}
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
		// Horizon-AO estimator: composite uses a safety-only floor (0.62 was
		// compensating the old depth-difference estimator — no longer needed).
		if (std::abs(lighting::kSsaoAoFloor - 0.10f) > 1e-5f)
			ok = fail("kSsaoAoFloor must be 0.10 (safety-only clamp, must match composite.frag)");
		// Edge-aware SSAO normal axis selection (mirrors ssao.frag.glsl):
		// both valid -> smaller |dz| (tie -> A); single valid side wins;
		// none valid -> no delta (safe fallback). Guards the regression where
		// an invalid neighbor substituted with the center won with delta 0.
		using AP = lighting::SsaoAxisPick;
		if (lighting::ssaoPickAxisDelta(true, true, 0.3f, 0.1f) != AP::B)
			ok = fail("ssaoPickAxisDelta must pick the smaller delta (B)");
		if (lighting::ssaoPickAxisDelta(true, true, 0.1f, 0.3f) != AP::A)
			ok = fail("ssaoPickAxisDelta must pick the smaller delta (A)");
		if (lighting::ssaoPickAxisDelta(true, true, 0.2f, 0.2f) != AP::A)
			ok = fail("ssaoPickAxisDelta must break ties toward A");
		if (lighting::ssaoPickAxisDelta(true, false, 5.0f, 0.0f) != AP::A)
			ok = fail("ssaoPickAxisDelta must use the only valid side (A)");
		if (lighting::ssaoPickAxisDelta(false, true, 0.0f, 5.0f) != AP::B)
			ok = fail("ssaoPickAxisDelta must use the only valid side (B)");
		if (lighting::ssaoPickAxisDelta(false, false, 0.0f, 0.0f) != AP::None)
			ok = fail("ssaoPickAxisDelta must report None when no neighbor is valid");
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
		if (farFog > lighting::kAtmosphereFogAmountCap + 1e-4f)
			ok = fail("far fog must respect kAtmosphereFogAmountCap");
		if (!(farFog >= midFog))
			ok = fail("far fog should be ≥ mid fog");
		if (lighting::sunShadowWeight(0.f) > 1e-4f)
			ok = fail("sunShadowWeight(0) must mute CSM in caves");
		if (lighting::sunShadowWeight(1.f) < 0.99f)
			ok = fail("sunShadowWeight(1) must fully apply outdoor CSM");

		// --- Shared camera-to-surface aerial perspective (issue #159) ---
		// terrain.frag / water.frag / mob.frag all evaluate the ONE
		// atmosphere_fog.inc.glsl contract; these C++ mirrors pin the numeric
		// policy the shaders must not drift from.
		{
			const float fs = sp.fogStart, fe = sp.fogEnd, fd = sp.fogDensity;
			const float hf = sp.fogHeightFalloff, fb = sp.fogBaseY;
			const auto amount = [&](float dist, float worldY, float camY, float reach) {
				return lighting::atmosphereFogAmount(dist, worldY, camY, fs, fe, fd, hf, fb, reach);
			};

			// Fog amount is monotonic (non-decreasing) with air-path distance
			// for a fixed height, and strictly grows in the mid range.
			float prev = -1.f;
			bool monotone = true, grewMid = false;
			for (int d = 0; d <= 1000; d += 25)
			{
				const float a = amount(float(d), 80.f, 80.f, 1.0f);
				if (a < prev - 1e-6f)
					monotone = false;
				if (a > 0.01f && a > prev + 1e-4f)
					grewMid = grewMid || (d > 200 && d < 700);
				prev = a;
			}
			if (!monotone)
				ok = fail("atmosphere fog amount must be monotonic with air-path distance");
			if (!grewMid)
				ok = fail("atmosphere fog amount must grow with distance in the mid range");

			// Height falloff moves every caller in the same direction: at
			// equal distance, air above fogBaseY hazes less than air below it.
			// Probe just past fog start where the density (height-aware) term
			// dominates the height-independent linear term.
			const float dProbe = fs + 5.0f;
			if (!(sp.fogDensity > 0.0f && sp.fogHeightFalloff > 0.0f))
				ok = fail("height-falloff invariant needs nonzero density/falloff defaults");
			if (!(amount(dProbe, 200.f, 200.f, 1.0f) < amount(dProbe, 40.f, 40.f, 1.0f)))
				ok = fail("height falloff must reduce haze above fogBaseY");
			if (!(amount(dProbe, 110.f, 110.f, 1.0f) < amount(dProbe, 40.f, 40.f, 1.0f)))
				ok = fail("height falloff ordering broken near fogBaseY");

			// Sealed-cave gate: no local skylight → no outdoor haze at any
			// distance; open sky gets the full capped curve.
			if (amount(900.f, 80.f, 80.f, 0.0f) > 1e-6f)
				ok = fail("skyReach 0 must zero the outdoor atmosphere (sealed caves)");
			if (!(amount(900.f, 80.f, 80.f, 1.0f) > amount(900.f, 80.f, 80.f, 0.3f)))
				ok = fail("haze must increase with local skylight reach");

			// Caller independence: at an equal world position the amount, haze
			// color and composed color are one shared evaluation — terrain,
			// water and entity callers must produce identical numbers.
			{
				const float aTerrain = amount(450.f, 96.f, 80.f, 0.8f);
				const float aWater = amount(450.f, 96.f, 80.f, 0.8f);
				const float aEntity = amount(450.f, 96.f, 80.f, 0.8f);
				if (std::abs(aTerrain - aWater) > 0.f || std::abs(aTerrain - aEntity) > 0.f)
					ok = fail("fog amount at equal world position must be caller-independent");
			}

			// Amount stays under the shared cap for the whole sweep.
			for (int d = 0; d <= 2000; d += 50)
				if (amount(float(d), 80.f, 80.f, 1.0f) > lighting::kAtmosphereFogAmountCap + 1e-5f)
					ok = fail("atmosphere amount must respect kAtmosphereFogAmountCap");

			// Haze color policy: sunset haze trends warm (r > b), night haze is
			// a dim dark blue (b > r, dimmer than day), both shared across
			// callers.
			const glm::vec3 engineFog(0.60f, 0.70f, 0.80f);
			const glm::vec3 kLuma(0.2126f, 0.7152f, 0.0722f);
			const glm::vec3 sunsetHaze = lighting::atmosphereHazeColor(engineFog, 1.0f, 0.0f);
			const glm::vec3 nightHaze = lighting::atmosphereHazeColor(engineFog, 0.0f, 1.0f);
			const glm::vec3 dayHaze = lighting::atmosphereHazeColor(engineFog, 0.0f, 0.0f);
			if (!(sunsetHaze.r > sunsetHaze.b))
				ok = fail("sunset haze must trend warm (r > b)");
			if (!(nightHaze.b > nightHaze.r))
				ok = fail("night haze must be dark blue (b > r)");
			if (!(glm::dot(nightHaze, kLuma) < glm::dot(dayHaze, kLuma)))
				ok = fail("night haze must be dimmer than day haze");
			// Pinned reference: guards silent rewrites of the shared palette.
			// 0.45 * engineFog + 0.55 * dayAerial(0.40, 0.60, 0.90).
			if (glm::length(dayHaze - glm::vec3(0.4900f, 0.6450f, 0.8550f)) > 2e-3f)
				ok = fail("day haze palette drifted (shared aerial color contract)");

			// Composition: amount 0 is the identity, chroma fades monotonically
			// with amount (desaturation deepens), and the result converges
			// toward the haze color.
			const glm::vec3 surface(0.9f, 0.3f, 0.2f);
			const glm::vec3 haze = sunsetHaze;
			const glm::vec3 c0 = lighting::applyAerialPerspective(surface, 0.0f, haze);
			if (glm::length(c0 - surface) > 1e-5f)
				ok = fail("aerial perspective at amount 0 must be the identity");
			const auto chroma = [&](const glm::vec3 &c) {
				return glm::length(c - glm::vec3(glm::dot(c, kLuma)));
			};
			const glm::vec3 cNear = lighting::applyAerialPerspective(surface, 0.15f, haze);
			const glm::vec3 cFull = lighting::applyAerialPerspective(surface, lighting::kAtmosphereFogAmountCap, haze);
			if (!(chroma(cFull) < chroma(cNear) && chroma(cNear) < chroma(surface)))
				ok = fail("aerial desaturation must deepen monotonically with amount");
			if (!(glm::length(cFull - haze) < glm::length(cNear - haze)))
				ok = fail("composition must converge toward the haze color with amount");

			// Shader drift guard: the GLSL include must carry the same policy
			// constants as these C++ mirrors (same file-parse pattern as the
			// FrameUBO / cutout sync checks above).
			{
				namespace fs = std::filesystem;
				const char *candidates[] = {
					"ressources/shaders/vulkan/atmosphere_fog.inc.glsl",
					"../ressources/shaders/vulkan/atmosphere_fog.inc.glsl",
					"../../ressources/shaders/vulkan/atmosphere_fog.inc.glsl",
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
					ok = fail("atmosphere_fog.inc.glsl not found under ressources/shaders/vulkan");
				else
				{
					auto assertContains = [&](const std::string &needle, const char *what) {
						if (glsl.find(needle) == std::string::npos)
							ok = fail(std::string("atmosphere_fog.inc.glsl drifted: missing ") + what);
					};
					assertContains("const float kAtmosphereFogCap = 0.45;",
								   "kAtmosphereFogCap == lighting::kAtmosphereFogAmountCap");
					assertContains("const float kAtmosphereFogDensityScale = 0.0009;",
								   "kAtmosphereFogDensityScale == lighting::kAtmosphereFogDensityScale");
					assertContains("const float kAtmosphereDesatMin = 0.72;",
								   "kAtmosphereDesatMin == lighting::kAtmosphereDesatMin");
					assertContains("smoothstep(fogStart, max(fogStart + 1.0, fogEnd), dist)",
								   "shared linear-fog smoothstep");
					assertContains("dist - fogStart * 0.25", "shared density-fog onset");
					// Shared enclosure gate (lighting::sunShadowWeight policy).
					assertContains("clamp(skyReach, 0.0, 1.0)", "skylight gating of outdoor haze");
					// Consumers: all three material families must evaluate the
					// one contract (prevents future shader-family drift).
					const char *consumers[] = {
						"ressources/shaders/vulkan/terrain.frag.glsl",
						"ressources/shaders/vulkan/water.frag.glsl",
						"ressources/shaders/vulkan/mob.frag.glsl",
						"../ressources/shaders/vulkan/terrain.frag.glsl",
						"../ressources/shaders/vulkan/water.frag.glsl",
						"../ressources/shaders/vulkan/mob.frag.glsl",
						"../../ressources/shaders/vulkan/terrain.frag.glsl",
						"../../ressources/shaders/vulkan/water.frag.glsl",
						"../../ressources/shaders/vulkan/mob.frag.glsl",
					};
					for (const char *family : {"terrain", "water", "mob"})
					{
						std::string src;
						for (const char *c : consumers)
						{
							if (std::string(c).find(std::string(family) + ".frag.glsl") == std::string::npos)
								continue;
							std::ifstream in(c);
							if (in)
							{
								src.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
								break;
							}
						}
						if (src.empty())
						{
							ok = fail(std::string(family) + ".frag.glsl not found under ressources/shaders/vulkan");
							continue;
						}
						if (src.find("evaluateAtmosphereFog") == std::string::npos ||
							src.find("applyAtmosphereFog") == std::string::npos)
							ok = fail(std::string(family) +
									  ".frag.glsl must evaluate the shared atmosphere contract (issue #159)");
					}
				}
			}
		}
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
		if (sizeof(FrameUBO) != 624)
			ok = fail(std::string("FrameUBO sizeof must be 624 (got ") + std::to_string(sizeof(FrameUBO)) + ")");
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
									  "vec4 cascadeSplits", "vec4 moonAmbient", "vec4 cascadeBiasScales",
									  "vec4 cascadeTexelWorldSizes", "vec4 cascadeGridOffsets01",
									  "vec4 cascadeGridOffsets2"})
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

		// isSrgbFormat must cover the sRGB variants of every core-enum family
		// ft_vox may encounter, not only the 8-bit packs used today.
		if (!isSrgbFormat(VK_FORMAT_BC7_SRGB_BLOCK) ||
			!isSrgbFormat(VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK) ||
			!isSrgbFormat(VK_FORMAT_ASTC_4x4_SRGB_BLOCK) ||
			!isSrgbFormat(VK_FORMAT_ASTC_12x12_SRGB_BLOCK))
			ok = fail("isSrgbFormat must classify compressed sRGB variants (BC/ETC2/ASTC)");
		if (isSrgbFormat(VK_FORMAT_R8G8B8A8_UNORM) || isSrgbFormat(VK_FORMAT_R16G16B16A16_SFLOAT) ||
			isSrgbFormat(VK_FORMAT_D32_SFLOAT) || isSrgbFormat(VK_FORMAT_UNDEFINED))
			ok = fail("non-sRGB formats must not be classified as sRGB");

		// 5. Linear-encoding policy (depth, HDR color, SSAO… stay non-sRGB)
		if (!isLinearEncodingFormat(VK_FORMAT_D32_SFLOAT) ||
			!isLinearEncodingFormat(VK_FORMAT_R16G16B16A16_SFLOAT) ||
			!isLinearEncodingFormat(VK_FORMAT_R8_UNORM))
			ok = fail("Depth, HDR color buffer, and SSAO targets must be linear-encoded");
		if (isLinearEncodingFormat(VK_FORMAT_R8G8B8A8_SRGB))
			ok = fail("sRGB-encoded format cannot be a linear encoding");

		// 6. Swapchain output transfer policy — decided from {format, colorSpace}
		if (classifyOutputTransfer(VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) != OutputTransfer::HardwareSrgb ||
			classifyOutputTransfer(VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) != OutputTransfer::HardwareSrgb)
			ok = fail("sRGB format + SRGB_NONLINEAR must use the hardware attachment encode");
		if (classifyOutputTransfer(VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) != OutputTransfer::ShaderSrgb ||
			classifyOutputTransfer(VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) != OutputTransfer::ShaderSrgb)
			ok = fail("UNORM format + SRGB_NONLINEAR must require the shader encode");
		if (classifyOutputTransfer(VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT) != OutputTransfer::Unsupported ||
			classifyOutputTransfer(VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_DISPLAY_P3_NONLINEAR_EXT) != OutputTransfer::Unsupported ||
			classifyOutputTransfer(VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT) != OutputTransfer::Unsupported)
			ok = fail("non-SDR color spaces (HDR10/PQ, Display-P3, extended sRGB) must be refused");
		// Strict allowlist: with SRGB_NONLINEAR, only the B8G8R8A8/R8G8B8A8 sRGB
		// and UNORM pairs are supported — every other format must be refused,
		// including other sRGB-family members like A8B8G8R8_SRGB_PACK32.
		if (classifyOutputTransfer(VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) != OutputTransfer::Unsupported ||
			classifyOutputTransfer(VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) != OutputTransfer::Unsupported ||
			classifyOutputTransfer(VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) != OutputTransfer::Unsupported ||
			classifyOutputTransfer(VK_FORMAT_A8B8G8R8_SRGB_PACK32, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) != OutputTransfer::Unsupported)
			ok = fail("strict allowlist must reject non-allowlisted {format, SRGB_NONLINEAR} pairs as Unsupported");
		if (!outputTransferRequiresShaderEncode(OutputTransfer::ShaderSrgb) ||
			outputTransferRequiresShaderEncode(OutputTransfer::HardwareSrgb))
			ok = fail("only the ShaderSrgb path sets the composite encode flag");

		// 7. End-to-end simulation: hardware path vs shader-encode path parity
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
			const glm::vec3 outSrgb = simulateDisplayOutput(c, kNeutralGamma, OutputTransfer::HardwareSrgb);
			const glm::vec3 outUnorm = simulateDisplayOutput(c, kNeutralGamma, OutputTransfer::ShaderSrgb);
			if (glm::length(outSrgb - outUnorm) > 1e-4f)
				ok = fail("Discrepancy between sRGB swapchain and UNORM swapchain display output");

			// Verify that output matches exactly one sRGB encode:
			const glm::vec3 expected = linearToSrgb(c);
			if (glm::length(outSrgb - expected) > 1e-4f)
				ok = fail("Output does not match standard single sRGB transfer");
		}

		// 8. Creative gamma operator neutrality + CPU/GLSL epsilon contract
		const glm::vec3 sample(0.35f, 0.62f, 0.18f);
		if (glm::length(applyArtisticGamma(sample, 1.0f) - sample) > 1e-6f)
			ok = fail("applyArtisticGamma with gamma=1.0 must be exact identity");
		if (std::abs(applyArtisticGamma(0.5f, 1.0f) - 0.5f) > 1e-6f)
			ok = fail("applyArtisticGamma(float) with gamma=1.0 must be exact identity");
		// Must mirror composite.frag: |gamma - 1| <= 0.001 skips the grade
		// (a 1e-4 CPU threshold would diverge from the shader at e.g. 1.0005).
		if (std::abs(applyArtisticGamma(0.5f, 1.0005f) - 0.5f) > 1e-6f)
			ok = fail("applyArtisticGamma must be identity within the shader-neutral epsilon (1.0005)");
		if (std::abs(applyArtisticGamma(0.5f, 1.05f) - std::pow(0.5f, 1.0f / 1.05f)) > 1e-6f)
			ok = fail("applyArtisticGamma must apply the grade beyond the neutral epsilon");

		// Proves that old double-gamma bug significantly distorted output:
		// e.g. for linear mid-gray 0.21586, correct sRGB is 0.50196,
		// but double-transfer produced ~0.735 (46% brighter!)
		const float oldDoubleTransfer = linearToSrgb(std::pow(midGrayLinear, 1.0f / 2.2f));
		if (std::abs(oldDoubleTransfer - midGraySrgb) < 0.15f)
			ok = fail("Old transfer was not distinctively washed out (double gamma check)");
	}

	// --- Texture mip chains + cutout policy (issue #136) ---
	{
		using namespace texture_mips;

		// Covered-texel count of one mip level (alpha/255 >= kAlphaCutoutThreshold):
		// the quantity the quantized coverage rescale pins exactly on every
		// generated level.
		const auto coveredTexels = [](const uint8_t *pixels, uint32_t w) {
			uint64_t covered = 0;
			for (uint64_t i = 0; i < static_cast<uint64_t>(w) * w; ++i)
			{
				if (static_cast<float>(pixels[i * 4 + 3]) / 255.0f >= kAlphaCutoutThreshold)
					++covered;
			}
			return covered;
		};

		// 1. Mip level counts: floor(log2(size)) + 1
		{
			const struct
			{
				uint32_t size;
				uint32_t levels;
			} countCases[] = {
				{1, 1}, {2, 2}, {16, 5}, {64, 7}, {20, 5}, {3, 2},
			};
			for (const auto &c : countCases)
			{
				if (mipLevelCount(c.size) != c.levels)
					ok = fail("mipLevelCount(" + std::to_string(c.size) + ") must be " +
					          std::to_string(c.levels) + " (got " +
					          std::to_string(mipLevelCount(c.size)) + ")");
			}
		}

		// 2. Chain layout: 16 px -> 5 levels of (256+64+16+4+1) texels
		if (chainBytes(16) != (256 + 64 + 16 + 4 + 1) * 4)
			ok = fail("chainBytes(16) must be (256+64+16+4+1)*4");
		if (chainOffset(16, 1) != 256 * 4)
			ok = fail("chainOffset(16, 1) must be 256*4");
		if (chainOffset(16, 4) != (256 + 64 + 16 + 4) * 4)
			ok = fail("chainOffset(16, 4) must be (256+64+16+4)*4");
		if (mipLevelBytes(16, 2) != 16 * 4)
			ok = fail("mipLevelBytes(16, 2) must be 16*4");
		if (chainOffset(16, 5) != chainBytes(16))
			ok = fail("chainOffset past the last level must equal chainBytes");

		// 3. Opaque textures stay fully opaque through every level
		{
			const uint32_t base = 16;
			std::vector<uint8_t> layer(base * base * 4, 0);
			for (uint32_t y = 0; y < base; ++y)
			{
				for (uint32_t x = 0; x < base; ++x)
				{
					uint8_t *texel = &layer[(y * base + x) * 4];
					texel[0] = static_cast<uint8_t>((x * 17 + 11) & 0xff);
					texel[1] = static_cast<uint8_t>((y * 29 + 7) & 0xff);
					texel[2] = static_cast<uint8_t>((x * 7 + y * 13 + 3) & 0xff);
					texel[3] = 255;
				}
			}
			std::vector<uint8_t> chain(chainBytes(base), 0);
			generateLayerChain(base, layer.data(), chain.data());
			const uint32_t levels = mipLevelCount(base);
			for (uint32_t level = 0; level < levels; ++level)
			{
				const uint32_t w = std::max(base >> level, 1u);
				const uint8_t *pixels = chain.data() + chainOffset(base, level);
				for (uint32_t i = 0; i < w * w; ++i)
				{
					if (pixels[i * 4 + 3] != 255)
					{
						ok = fail("opaque texture must stay alpha 255 at level " +
						          std::to_string(level));
						break;
					}
				}
			}
		}

		// 4. Fringe: half-coverage 2x2 — opaque green over transparent magenta.
		// Premultiplied area filtering gives the transparent row zero color
		// weight, and the coverage search keeps the level's unscaled ~0.5 alpha
		// average (equal-error ties resolve toward the higher coverage), so the
		// 1x1 level stays ~50% alpha, green-dominant, with no magenta bleed.
		{
			const uint32_t base = 2;
			const std::vector<uint8_t> layer = {
				// top row: two opaque green texels
				0, 255, 0, 255, 0, 255, 0, 255,
				// bottom row: two fully transparent magenta texels (would fringe if bled)
				255, 0, 255, 0, 255, 0, 255, 0,
			};
			std::vector<uint8_t> chain(chainBytes(base), 0);
			generateLayerChain(base, layer.data(), chain.data());
			const uint8_t *level1 = chain.data() + chainOffset(base, 1); // 1x1
			const float a = static_cast<float>(level1[3]) / 255.0f;
			if (std::abs(a - 0.5f) > 2.0f / 255.0f)
				ok = fail("half-coverage 2x2 must keep alpha ~0.5 at level 1 (got " +
				          std::to_string(a) + ")");
			const float r = static_cast<float>(level1[0]) / 255.0f;
			const float g = static_cast<float>(level1[1]) / 255.0f;
			const float b = static_cast<float>(level1[2]) / 255.0f;
			if (!(g > r && g > b))
				ok = fail("fringe filter must stay green-dominant (G strictly max)");
			if (r >= 60.0f / 255.0f || b >= 60.0f / 255.0f)
				ok = fail("fringe filter must not bleed transparent magenta (R/B < 60/255)");
		}

		// 5. Sub-threshold soft alpha: every texel below the cutout threshold
		// gives the layer 0 coverage, so nothing may be rescaled or dilated —
		// the filtered level keeps the exact alpha byte and its RGB
		{
			const uint32_t base = 2;
			const std::vector<uint8_t> faint = {
				128, 0, 128, 100, 128, 0, 128, 100,
				128, 0, 128, 100, 128, 0, 128, 100,
			};
			std::vector<uint8_t> faintChain(chainBytes(base), 0);
			generateLayerChain(base, faint.data(), faintChain.data());
			const uint8_t *faint1 = faintChain.data() + chainOffset(base, 1);
			if (faint1[3] != 100)
				ok = fail("sub-threshold soft alpha (coverage 0) must keep its exact byte (got " +
				          std::to_string(faint1[3]) + ")");
			if (std::abs(int(faint1[0]) - 128) > 1 || faint1[1] > 1 || std::abs(int(faint1[2]) - 128) > 1)
				ok = fail("soft-alpha RGB must survive premultiplied filtering unchanged");
		}

		// 6. Coverage preservation matrix: for cutout layers (0 < coverage < 1)
		// every generated level with w >= 2 must hold the base level's own
		// measured covered-texel count EXACTLY: the per-level binary search only
		// finds the alpha scale, then the quantized tie resolution promotes and
		// demotes individual boundary texels until the covered count equals
		// lround(baseCoverage * w * w) (all count ratios in these fixtures are
		// dyadic, so the double math is exact). Loud magenta under the
		// transparent texels doubles as a no-bleed probe at every depth.
		{
			const uint32_t base = 8;
			struct MaskCase
			{
				const char *name;
				bool opaque[8][8];
			};
			std::vector<MaskCase> masks;

			MaskCase sparse{};
			sparse.name = "sparse ~25%";
			for (uint32_t y = 0; y < base; ++y)
				for (uint32_t x = 0; x < base; ++x)
					sparse.opaque[y][x] = ((x * 3 + y * 5) % 8) < 2;
			masks.push_back(sparse);

			MaskCase dense{};
			dense.name = "dense ~75%";
			for (uint32_t y = 0; y < base; ++y)
				for (uint32_t x = 0; x < base; ++x)
					dense.opaque[y][x] = ((x * 3 + y * 5) % 8) < 6;
			masks.push_back(dense);

			MaskCase blob{};
			blob.name = "leaf-like blob";
			{
				const char *rows[8] = {
					"00000000",
					"00111100",
					"01111110",
					"01101110",
					"01111110",
					"00111100",
					"00011000",
					"00000000",
				};
				for (uint32_t y = 0; y < base; ++y)
					for (uint32_t x = 0; x < base; ++x)
						blob.opaque[y][x] = rows[y][x] == '1';
			}
			masks.push_back(blob);

			MaskCase branches{};
			branches.name = "isolated branches";
			branches.opaque[1][1] = true;
			branches.opaque[2][5] = true;
			branches.opaque[5][3] = true;
			branches.opaque[6][6] = true;
			masks.push_back(branches);

			for (const auto &mask : masks)
			{
				std::vector<uint8_t> layer(base * base * 4, 0);
				for (uint32_t y = 0; y < base; ++y)
				{
					for (uint32_t x = 0; x < base; ++x)
					{
						uint8_t *texel = &layer[(y * base + x) * 4];
						if (mask.opaque[y][x])
						{
							texel[0] = 0;
							texel[1] = 255;
							texel[2] = 0;
							texel[3] = 255;
						}
						else
						{
							// Deliberately loud RGB under the transparent
							// texel: an alpha-unaware filter would bleed
							// magenta down the chain.
							texel[0] = 255;
							texel[1] = 0;
							texel[2] = 255;
							texel[3] = 0;
						}
					}
				}
				std::vector<uint8_t> chain(chainBytes(base), 0);
				generateLayerChain(base, layer.data(), chain.data());

				// The target is the layer's own measured mip-0 coverage, not a
				// nominal value.
				const uint64_t baseCovered = coveredTexels(chain.data(), base);
				const double baseCoverage =
					static_cast<double>(baseCovered) / static_cast<double>(base * base);
				if (baseCoverage <= 0.0 || baseCoverage >= 1.0)
					ok = fail(std::string("mask ") + mask.name +
					          " must be a cutout layer (0 < coverage < 1)");

				const uint32_t levels = mipLevelCount(base);
				for (uint32_t level = 1; level < levels; ++level)
				{
					const uint32_t w = std::max(base >> level, 1u);
					// A 1x1 level can only represent coverage 0 or 1 — skip it.
					if (w < 2)
						continue;
					const uint8_t *pixels = chain.data() + chainOffset(base, level);
					// The only covered color is pure green: no generated texel
					// may see R or B exceed G (magenta bleed).
					for (uint32_t i = 0; i < w * w; ++i)
					{
						if (pixels[i * 4] > pixels[i * 4 + 1] || pixels[i * 4 + 2] > pixels[i * 4 + 1])
							ok = fail(std::string("magenta bled into mask ") + mask.name +
							          " at level " + std::to_string(level));
					}
					const uint64_t coveredCount = coveredTexels(pixels, w);
					const uint64_t expected = static_cast<uint64_t>(std::lround(
						baseCoverage * static_cast<double>(w) * static_cast<double>(w)));
					if (expected > 0 && coveredCount == 0)
						ok = fail(std::string("non-zero representable cutout coverage vanished for mask ") +
						          mask.name + " at level " + std::to_string(level));
					if (coveredCount != expected)
						ok = fail(std::string("coverage drifted for mask ") + mask.name +
						          " at level " + std::to_string(level) + " (expected " +
						          std::to_string(expected) + " of " + std::to_string(w * w) +
						          " covered texels, got " + std::to_string(coveredCount) + ")");
				}

				// Focused regression (review P1): the isolated-branches mask is
				// four 1-texel features (6.25% coverage) whose 4x4 window
				// averages all land on 0 or 64/255 — a global alpha scale alone
				// could only give the level 0% or 25% coverage. The quantized
				// tie resolution promotes exactly one boundary texel,
				// preserving the representable 6.25% detail: coveredCount must
				// be exactly 1.
				if (std::string(mask.name) == "isolated branches")
				{
					const uint8_t *level1 = chain.data() + chainOffset(base, 1); // 4x4
					const uint64_t level1Covered = coveredTexels(level1, 4);
					if (level1Covered != 1)
						ok = fail("isolated branches must keep exactly 1 of 16 texels covered at level 1 (got " +
						          std::to_string(level1Covered) +
						          ") — quantized tie resolution lost the representable 6.25% detail");
				}
			}
		}

		// 7. Checkerboard pathology: every 2x2 window of a perfect checkerboard
		// averages to exactly 0.5 alpha, so each filtered level is uniform — all
		// of its texels share the single value 128/255 (just above the
		// threshold), and a global alpha scale can only move them all together:
		// coverage 0 or 1. The quantized tie resolution decides WHICH texels
		// cross instead: the 4x4 Bayer spread promotes/demotes exactly half of
		// each uniform level, so the 50% checkerboard keeps exactly 50% covered
		// at every generated level (8/16 at 4x4, 2/4 at 2x2) instead of
		// collapsing toward either extreme.
		{
			const uint32_t base = 8;
			std::vector<uint8_t> layer(base * base * 4, 0);
			for (uint32_t y = 0; y < base; ++y)
			{
				for (uint32_t x = 0; x < base; ++x)
				{
					uint8_t *texel = &layer[(y * base + x) * 4];
					if ((x + y) % 2 == 0)
					{
						texel[0] = 0;
						texel[1] = 255;
						texel[2] = 0;
						texel[3] = 255;
					}
					else
					{
						texel[0] = 255;
						texel[1] = 0;
						texel[2] = 255;
						texel[3] = 0;
					}
				}
			}
			std::vector<uint8_t> chain(chainBytes(base), 0);
			generateLayerChain(base, layer.data(), chain.data());
			const double baseCoverage =
				static_cast<double>(coveredTexels(chain.data(), base)) /
				static_cast<double>(base * base);
			const uint32_t levels = mipLevelCount(base);
			for (uint32_t level = 1; level < levels; ++level)
			{
				const uint32_t w = std::max(base >> level, 1u);
				// A 1x1 level can only represent coverage 0 or 1 — skip it.
				if (w < 2)
					continue;
				const uint8_t *pixels = chain.data() + chainOffset(base, level);
				const uint64_t coveredCount = coveredTexels(pixels, w);
				const uint64_t expected = static_cast<uint64_t>(std::lround(
					baseCoverage * static_cast<double>(w) * static_cast<double>(w)));
				if (coveredCount != expected)
					ok = fail("checkerboard must keep exactly " + std::to_string(expected) +
					          " of " + std::to_string(w * w) + " texels covered at level " +
					          std::to_string(level) + " (got " + std::to_string(coveredCount) + ")");
			}
		}

		// 8. Mip-0 edge dilation (review P1/P2): slight minification already
		// filters mip 0 with LINEAR, so the base level itself must carry border
		// colors — alpha-0 texels adjacent (8-neighborhood) to covered texels
		// take their linear-light average RGB on mip 0 too, alpha stays exactly
		// 0, and texels with no covered neighbor within one texel stay black.
		{
			const uint32_t base = 4;
			std::vector<uint8_t> layer(base * base * 4, 0);
			for (uint32_t y = 0; y < base; ++y)
			{
				for (uint32_t x = 0; x < base; ++x)
				{
					uint8_t *texel = &layer[(y * base + x) * 4];
					if (x == 0)
					{
						texel[0] = 0;
						texel[1] = 255;
						texel[2] = 0;
						texel[3] = 255;
					}
					else
					{
						texel[0] = 0;
						texel[1] = 0;
						texel[2] = 0;
						texel[3] = 0;
					}
				}
			}
			std::vector<uint8_t> chain(chainBytes(base), 0);
			generateLayerChain(base, layer.data(), chain.data());
			const uint8_t *mip0 = chain.data();
			for (uint32_t y = 0; y < base; ++y)
			{
				for (uint32_t x = 0; x < base; ++x)
				{
					const uint8_t *texel = mip0 + (y * base + x) * 4;
					// Alpha bytes are never touched by the dilation.
					if (texel[3] != (x == 0 ? 255 : 0))
						ok = fail("mip-0 dilation must keep alpha bytes unchanged (texel " +
						          std::to_string(y * base + x) + ")");
					if (x == 0)
					{
						// Covered texels keep their own RGB verbatim.
						if (texel[0] != 0 || texel[1] != 255 || texel[2] != 0)
							ok = fail("mip-0 dilation must leave covered texels' RGB unchanged (row " +
							          std::to_string(y) + ")");
					}
					else if (x == 1)
					{
						// Adjacent to the covered green column: dilated border color.
						const float r = static_cast<float>(texel[0]) / 255.0f;
						const float g = static_cast<float>(texel[1]) / 255.0f;
						const float b = static_cast<float>(texel[2]) / 255.0f;
						if (!(g > 100.0f / 255.0f && g > r && g > b) ||
							r >= 60.0f / 255.0f || b >= 60.0f / 255.0f)
							ok = fail("mip-0 texels adjacent to covered texels must carry the green border color (column 1, row " +
							          std::to_string(y) + ")");
					}
					else
					{
						// Columns 2-3 have no covered neighbor within one texel.
						if (texel[0] != 0 || texel[1] != 0 || texel[2] != 0)
							ok = fail("mip-0 texels without a covered neighbor must stay black (column " +
							          std::to_string(x) + ", row " + std::to_string(y) + ")");
					}
				}
			}
		}

		// 9. NPOT area sampling: on a 5x5 layer the level-1 texel (1,1) averages
		// the whole {2,3,4}² source window — the remainder is spread across the
		// edge texels, nothing is dropped — so the opaque red last row/column
		// keeps contributing down to the 1x1 level
		{
			const uint32_t base = 5;
			std::vector<uint8_t> layer(base * base * 4, 0);
			for (uint32_t y = 0; y < base; ++y)
			{
				for (uint32_t x = 0; x < base; ++x)
				{
					uint8_t *texel = &layer[(y * base + x) * 4];
					const bool border = (x == base - 1) || (y == base - 1);
					texel[0] = border ? 255 : 0;
					texel[1] = 0;
					texel[2] = 0;
					texel[3] = 255;
				}
			}
			std::vector<uint8_t> chain(chainBytes(base), 0);
			generateLayerChain(base, layer.data(), chain.data());
			if (mipLevelCount(base) != 3)
				ok = fail("mipLevelCount(5) must be 3 (levels 5, 2, 1)");
			const uint8_t *level1 = chain.data() + chainOffset(base, 1); // 2x2
			// The layer is fully opaque: the NPOT windows must keep alpha 255 too.
			if (level1[3] != 255 || level1[7] != 255 || level1[11] != 255 || level1[15] != 255)
				ok = fail("NPOT opaque layer must keep alpha 255 at level 1");
			// Texel (1,1) blends 5 red border texels of its 9-tap window.
			if (!(level1[12] > level1[13] && level1[12] > level1[14]))
				ok = fail("NPOT level 1 texel (1,1) must be red-dominant (R strictly max)");
			// Texel (0,0) blends only interior black texels.
			if (level1[0] >= 60 || level1[1] >= 60 || level1[2] >= 60)
				ok = fail("NPOT level 1 texel (0,0) must stay black (< 60/255 per channel)");
			// The red contribution must survive all the way to the 1x1 level.
			const uint8_t *level2 = chain.data() + chainOffset(base, 2); // 1x1
			if (!(level2[0] > level2[1] && level2[0] > level2[2]))
				ok = fail("NPOT red contribution must reach the 1x1 level (R strictly max)");
		}

		// 10. Edge dilation (dark-fringe fix): generated alpha-0 texels adjacent
		// to covered texels take the linear-light average of their covered
		// 8-neighborhood while their alpha stays exactly 0. The mip-0 input is
		// edge-dilated too now, but the premultiplied filter ignores alpha-0
		// RGB, so the level-1 results are unchanged.
		{
			const uint32_t base = 4;
			std::vector<uint8_t> layer(base * base * 4, 0);
			for (uint32_t y = 0; y < base; ++y)
			{
				for (uint32_t x = 0; x < base; ++x)
				{
					uint8_t *texel = &layer[(y * base + x) * 4];
					if (x == 0)
					{
						texel[0] = 0;
						texel[1] = 255;
						texel[2] = 0;
						texel[3] = 255;
					}
					else
					{
						texel[0] = 0;
						texel[1] = 0;
						texel[2] = 0;
						texel[3] = 0;
					}
				}
			}
			std::vector<uint8_t> chain(chainBytes(base), 0);
			generateLayerChain(base, layer.data(), chain.data());
			const uint8_t *level1 = chain.data() + chainOffset(base, 1); // 2x2
			for (uint32_t i = 0; i < 4; ++i)
			{
				const uint8_t *texel = level1 + i * 4;
				if (texel[3] != 0)
					continue;
				const float r = static_cast<float>(texel[0]) / 255.0f;
				const float g = static_cast<float>(texel[1]) / 255.0f;
				const float b = static_cast<float>(texel[2]) / 255.0f;
				if (!(g > 100.0f / 255.0f && g > r && g > b))
					ok = fail("dilated alpha-0 texel must carry the green border color (level 1, texel " +
					          std::to_string(i) + ")");
			}
		}

		// 11. Multi-layer atlas: layer-major / mip-minor buffer with per-layer
		// mip-0 copies and a spot-checked filtered value. Mip-0 contract: byte
		// exact EXCEPT the RGB of alpha-0 texels, which mip-0 edge dilation may
		// recolor — these test layers have no alpha-0 texels, so their mip 0
		// stays fully byte-exact.
		{
			const uint32_t base = 4;
			const uint32_t layers = 2;
			const uint32_t mip0Bytes = base * base * 4;
			std::vector<uint8_t> atlas(layers * mip0Bytes, 0);
			for (uint32_t i = 0; i < mip0Bytes; ++i)
				atlas[i] = static_cast<uint8_t>((i % 251) + 1); // layer 0 pattern
			for (uint32_t i = 0; i < mip0Bytes; ++i)
				atlas[mip0Bytes + i] = static_cast<uint8_t>((i * 37 + 11) & 0xff); // layer 1 differs

			const std::vector<uint8_t> out = buildMipChainAtlas(base, layers, atlas);
			if (out.size() != static_cast<size_t>(layers) * chainBytes(base))
				ok = fail("atlas output must hold one full chain per layer (got " +
				          std::to_string(out.size()) + " bytes)");
			// Each layer's mip 0 must be byte-exact (no alpha-0 texels in these
			// layers, so mip-0 edge dilation cannot recolor anything).
			for (uint32_t i = 0; i < mip0Bytes; ++i)
			{
				if (out[i] != atlas[i] || out[chainBytes(base) + i] != atlas[mip0Bytes + i])
					ok = fail("atlas layer mip-0 must be byte-exact (byte " +
					          std::to_string(i) + ")");
			}
			// Layer 1's level 1 starts at chainBytes + chainOffset and fits the buffer.
			if (chainBytes(base) + chainOffset(base, 1) + mipLevelBytes(base, 1) > out.size())
				ok = fail("layer 1 level 1 range must fit inside the atlas buffer");
			// Layer 0 level 1 (2x2) texel (0,0) alpha = rounded average of the
			// mip-0 {0,1}x{0,1} alpha bytes (layer 0's coverage is 0, so the
			// value is the plain area average, untouched by the rescale).
			const auto mip0Alpha = [&atlas, base](uint32_t x, uint32_t y) {
				return atlas[(y * base + x) * 4 + 3];
			};
			const uint32_t alphaSum =
				mip0Alpha(0, 0) + mip0Alpha(1, 0) + mip0Alpha(0, 1) + mip0Alpha(1, 1);
			const uint8_t *layer0Level1 = out.data() + chainOffset(base, 1);
			const int expectedAlpha = static_cast<int>(alphaSum / 4);
			if (std::abs(static_cast<int>(layer0Level1[3]) - expectedAlpha) > 1)
				ok = fail("layer 0 level 1 alpha must equal the 2x2 alpha average (got " +
				          std::to_string(static_cast<int>(layer0Level1[3])) + ", expected ~" +
				          std::to_string(expectedAlpha) + ")");
		}

		// 12. Synthetic alpha levels (review P2): a layer mixing all six
		// interesting alpha bytes {0, 64, 127, 128, 192, 255} with one fixed RGB
		// per level. Mip 0 must be byte-exact for alpha and for the RGB of every
		// alpha > 0 texel (only alpha-0 texels may be recolored by edge
		// dilation), every generated level must hold the exact covered count,
		// and a final alpha of 0 must always store RGB 0 (no hidden garbage
		// under transparency).
		{
			const uint32_t base = 16;
			const uint8_t alphaLevels[6] = {0, 64, 127, 128, 192, 255};
			const uint8_t rgbLevels[6][3] = {
				{255, 0, 0},   {0, 255, 0},   {0, 0, 255},
				{255, 255, 0}, {255, 0, 255}, {0, 255, 255},
			};
			std::vector<uint8_t> layer(base * base * 4, 0);
			for (uint32_t y = 0; y < base; ++y)
			{
				for (uint32_t x = 0; x < base; ++x)
				{
					uint8_t *texel = &layer[(y * base + x) * 4];
					const uint32_t k = (x + 3 * y) % 6;
					texel[0] = rgbLevels[k][0];
					texel[1] = rgbLevels[k][1];
					texel[2] = rgbLevels[k][2];
					texel[3] = alphaLevels[k];
				}
			}
			std::vector<uint8_t> chain(chainBytes(base), 0);
			generateLayerChain(base, layer.data(), chain.data());

			// Mip 0: alpha bytes byte-exact, RGB byte-exact wherever alpha > 0.
			for (uint32_t i = 0; i < base * base; ++i)
			{
				const uint8_t *src = &layer[i * 4];
				const uint8_t *out = &chain[i * 4];
				if (out[3] != src[3])
					ok = fail("synthetic alpha levels: mip-0 alpha must be byte-exact (texel " +
					          std::to_string(i) + ")");
				if (src[3] > 0 && (out[0] != src[0] || out[1] != src[1] || out[2] != src[2]))
					ok = fail("synthetic alpha levels: mip-0 RGB of alpha>0 texels must be byte-exact (texel " +
					          std::to_string(i) + ")");
			}

			const uint64_t baseCovered = coveredTexels(chain.data(), base);
			const double baseCoverage =
				static_cast<double>(baseCovered) / static_cast<double>(base * base);
			const uint32_t levels = mipLevelCount(base);
			for (uint32_t level = 1; level < levels; ++level)
			{
				const uint32_t w = std::max(base >> level, 1u);
				// A 1x1 level can only represent coverage 0 or 1 — skip it.
				if (w < 2)
					continue;
				const uint8_t *pixels = chain.data() + chainOffset(base, level);
				const uint64_t coveredCount = coveredTexels(pixels, w);
				const uint64_t expected = static_cast<uint64_t>(std::lround(
					baseCoverage * static_cast<double>(w) * static_cast<double>(w)));
				if (coveredCount != expected)
					ok = fail("synthetic alpha levels: covered count must be exactly " +
					          std::to_string(expected) + " of " + std::to_string(w * w) +
					          " at level " + std::to_string(level) + " (got " +
					          std::to_string(coveredCount) + ")");
				for (uint32_t i = 0; i < w * w; ++i)
				{
					if (pixels[i * 4 + 3] == 0 &&
						(pixels[i * 4] != 0 || pixels[i * 4 + 1] != 0 || pixels[i * 4 + 2] != 0))
						ok = fail("synthetic alpha levels: final alpha 0 must store RGB 0 (level " +
						          std::to_string(level) + ", texel " + std::to_string(i) + ")");
				}
			}
		}

		// 13. Bundled-pack glass/ice contract (review P2): the canonical cutout
		// threshold (alpha/255 >= 0.5) must classify the shipped pack's
		// translucency textures exactly as the mip generator assumes — glass is
		// a binary cutout (the threshold is an identity for it), ice is
		// uniformly translucent at alpha 190 (190/255 >= 0.5, so it is never
		// discarded, and a uniform plain alpha average keeps exactly 190 through
		// the chain), and the packed ices are fully opaque.
		{
			namespace fs = std::filesystem;
			const char *candidates[] = {
				"ressources/default-resource-pack.zip",
				"../ressources/default-resource-pack.zip",
				"../../ressources/default-resource-pack.zip",
			};
			std::string foundZip;
			for (const char *candidate : candidates)
			{
				if (fs::exists(candidate))
				{
					foundZip = candidate;
					break;
				}
			}
			if (foundZip.empty())
			{
				ok = fail("default-resource-pack.zip not found (bundled glass/ice mip contract untested)");
			}
			else
			{
				ResourcePackReader reader(foundZip);
				if (!reader.isValid())
					ok = fail("ResourcePackReader failed to open default-resource-pack.zip");

				// Decode a bundled block texture into a 16x16 RGBA8 layer.
				const auto decodeBlockLayer = [&](const char *basename, std::vector<uint8_t> &layer) {
					std::vector<uint8_t> png;
					if (!reader.readBlockTexture(basename, png) || png.empty())
					{
						ok = fail(std::string("failed to read ") + basename +
						          " from the default resource pack");
						return false;
					}
					int w = 0;
					int h = 0;
					int comp = 0;
					stbi_uc *decoded =
						stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &w, &h, &comp, 4);
					if (!decoded)
					{
						ok = fail(std::string("stb_image failed to decode ") + basename);
						return false;
					}
					layer.assign(decoded, decoded + static_cast<size_t>(w) * h * 4);
					stbi_image_free(decoded);
					if (w != 16 || h != 16)
					{
						ok = fail(std::string(basename) + " must decode to 16x16 (got " +
						          std::to_string(w) + "x" + std::to_string(h) + ")");
						return false;
					}
					return true;
				};

				// glass.png: binary cutout, alpha {0, 255} only.
				std::vector<uint8_t> glass;
				if (decodeBlockLayer("glass.png", glass))
				{
					for (uint32_t i = 0; i < 256; ++i)
					{
						const uint8_t a = glass[i * 4 + 3];
						if (a != 0 && a != 255)
							ok = fail("glass.png alpha must be a binary cutout {0, 255} (texel " +
							          std::to_string(i) + " has alpha " + std::to_string(static_cast<int>(a)) + ")");
					}
					// The 0.5 threshold is an identity for a binary cutout, so
					// the generated chain must hold the mip-0 covered count
					// exactly at every level.
					std::vector<uint8_t> chain(chainBytes(16), 0);
					generateLayerChain(16, glass.data(), chain.data());
					const double baseCoverage =
						static_cast<double>(coveredTexels(chain.data(), 16)) / 256.0;
					for (uint32_t level = 1; level < mipLevelCount(16); ++level)
					{
						const uint32_t w = std::max(16u >> level, 1u);
						// A 1x1 level can only represent coverage 0 or 1 — skip it.
						if (w < 2)
							continue;
						const uint64_t coveredCount =
							coveredTexels(chain.data() + chainOffset(16, level), w);
						const uint64_t expected = static_cast<uint64_t>(std::lround(
							baseCoverage * static_cast<double>(w) * static_cast<double>(w)));
						if (coveredCount != expected)
							ok = fail("glass.png chain must hold exactly " + std::to_string(expected) +
							          " of " + std::to_string(w * w) + " texels covered at level " +
							          std::to_string(level) + " (got " + std::to_string(coveredCount) + ")");
					}
				}

				// ice.png: uniformly translucent, alpha 190 everywhere —
				// coverage 1.0 skips the rescale and the uniform plain alpha
				// average is exact, so every texel at EVERY level stays 190.
				std::vector<uint8_t> ice;
				if (decodeBlockLayer("ice.png", ice))
				{
					for (uint32_t i = 0; i < 256; ++i)
					{
						if (ice[i * 4 + 3] != 190)
							ok = fail("ice.png must be uniformly translucent alpha 190 (texel " +
							          std::to_string(i) + " has alpha " +
							          std::to_string(static_cast<int>(ice[i * 4 + 3])) + ")");
					}
					std::vector<uint8_t> chain(chainBytes(16), 0);
					generateLayerChain(16, ice.data(), chain.data());
					for (uint32_t level = 0; level < mipLevelCount(16); ++level)
					{
						const uint32_t w = std::max(16u >> level, 1u);
						const uint8_t *pixels = chain.data() + chainOffset(16, level);
						for (uint32_t i = 0; i < w * w; ++i)
						{
							if (pixels[i * 4 + 3] != 190)
								ok = fail("ice.png alpha must stay exactly 190 through the whole chain (level " +
								          std::to_string(level) + ", texel " + std::to_string(i) + ")");
						}
					}
				}

				// blue_ice.png / packed_ice.png: fully opaque.
				for (const char *name : {"blue_ice.png", "packed_ice.png"})
				{
					std::vector<uint8_t> layer;
					if (decodeBlockLayer(name, layer))
					{
						for (uint32_t i = 0; i < 256; ++i)
						{
							if (layer[i * 4 + 3] != 255)
								ok = fail(std::string(name) + " must be fully opaque alpha 255 (texel " +
								          std::to_string(i) + " has alpha " +
								          std::to_string(static_cast<int>(layer[i * 4 + 3])) + ")");
						}
					}
				}
			}
		}

		// 14. Anisotropy policy: device-gated, clamped to the device limit, >= 1.0
		{
			const SamplerAnisotropy unsupported = resolveAnisotropy(false, 16.0f);
			if (unsupported.enabled || unsupported.maxAnisotropy != 1.0f)
				ok = fail("anisotropy must be disabled with maxAnisotropy 1.0 when unsupported");
			const SamplerAnisotropy full = resolveAnisotropy(true, 16.0f);
			if (!full.enabled || full.maxAnisotropy != 8.0f)
				ok = fail("anisotropy must honor the 8.0 request on capable devices");
			const SamplerAnisotropy clamp4 = resolveAnisotropy(true, 4.0f);
			if (!clamp4.enabled || clamp4.maxAnisotropy != 4.0f)
				ok = fail("anisotropy must clamp to the 4.0 device limit");
			const SamplerAnisotropy clamp2 = resolveAnisotropy(true, 2.0f);
			if (!clamp2.enabled || clamp2.maxAnisotropy != 2.0f)
				ok = fail("anisotropy must clamp to the 2.0 device limit");
		}

		// 15. Shader cutout threshold stays in sync with the mip generator
		{
			namespace fs = std::filesystem;
			const char *candidates[] = {
				"ressources/shaders/vulkan/cutout.inc.glsl",
				"../ressources/shaders/vulkan/cutout.inc.glsl",
				"../../ressources/shaders/vulkan/cutout.inc.glsl",
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
			{
				ok = fail("cutout.inc.glsl not found under ressources/shaders/vulkan");
			}
			else
			{
				const std::string key = "const float kAlphaCutoutThreshold = ";
				const auto at = glsl.find(key);
				if (at == std::string::npos)
					ok = fail("cutout.inc.glsl must define kAlphaCutoutThreshold = <float>");
				else
				{
					const float shaderThreshold = std::strtof(glsl.c_str() + at + key.size(), nullptr);
					if (std::abs(shaderThreshold - kAlphaCutoutThreshold) > 1e-6f)
						ok = fail("cutout.inc.glsl threshold must equal texture_mips::kAlphaCutoutThreshold");
				}
			}
		}

		// 16. Informational: CPU mip generation cost (no assertion)
		{
			const uint32_t base = 64;
			std::vector<uint8_t> layer(base * base * 4, 0);
			for (uint32_t y = 0; y < base; ++y)
			{
				for (uint32_t x = 0; x < base; ++x)
				{
					uint8_t *texel = &layer[(y * base + x) * 4];
					texel[0] = static_cast<uint8_t>(x * 4);
					texel[1] = static_cast<uint8_t>(y * 4);
					texel[2] = static_cast<uint8_t>((x + y) * 2);
					texel[3] = static_cast<uint8_t>((x + y) & 0xff);
				}
			}
			std::vector<uint8_t> chain(chainBytes(base), 0);
			const auto start = std::chrono::steady_clock::now();
			constexpr int kIterations = 1000;
			for (int i = 0; i < kIterations; ++i)
				generateLayerChain(base, layer.data(), chain.data());
			const auto elapsed = std::chrono::steady_clock::now() - start;
			const double msPerLayer =
				std::chrono::duration<double, std::milli>(elapsed).count() / kIterations;
			std::cout << "test_render_helpers: generateLayerChain(64) average "
			          << msPerLayer << " ms/layer over " << kIterations << " iterations\n";
		}
	}

	// --- Entity local voxel lighting (issue #128) ---------------------------
	{
		// 1. Pack and unpack voxel light (4-bit sky, 4-bit R, 4-bit G, 4-bit B = 16-bit uint16_t)
		for (uint8_t sky = 0; sky < 16; ++sky)
		{
			for (uint8_t r = 0; r < 16; ++r)
			{
				for (uint8_t g = 0; g < 16; ++g)
				{
					const uint8_t b = (sky + r + g) & 0xF;
					const uint16_t packed = lighting::packVoxelLight(sky, r, g, b);
					uint8_t uSky = 0, uR = 0, uG = 0, uB = 0;
					lighting::unpackVoxelLight(packed, uSky, uR, uG, uB);
					if (uSky != sky || uR != r || uG != g || uB != b)
					{
						ok = fail("packVoxelLight / unpackVoxelLight roundtrip failed");
						break;
					}

					const uint16_t blockRGB4 = lighting::packBlockLightRGB4(r, g, b);
					const uint16_t packed2 = lighting::packVoxelLightRGB4(sky, blockRGB4);
					if (packed != packed2)
					{
						ok = fail("packVoxelLightRGB4 mismatch with packVoxelLight");
						break;
					}
				}
			}
		}

		// 2. Unpack to LocalVoxelLight float representation
		const auto l0 = lighting::unpackLocalVoxelLight(lighting::packVoxelLight(0, 0, 0, 0));
		if (l0.skylight != 0.0f || l0.blockRgb != glm::vec3(0.0f))
			ok = fail("unpackLocalVoxelLight zero light failed");

		const auto lFull = lighting::unpackLocalVoxelLight(lighting::packVoxelLight(15, 15, 15, 15));
		if (lFull.skylight != 1.0f || lFull.blockRgb != glm::vec3(1.0f))
			ok = fail("unpackLocalVoxelLight full light failed");

		const auto lMixed = lighting::unpackLocalVoxelLight(lighting::packVoxelLight(15, 15, 0, 6));
		if (std::abs(lMixed.skylight - 1.0f) > 1e-6f ||
		    std::abs(lMixed.blockRgb.r - 1.0f) > 1e-6f ||
		    std::abs(lMixed.blockRgb.g - 0.0f) > 1e-6f ||
		    std::abs(lMixed.blockRgb.b - (6.0f / 15.0f)) > 1e-6f)
			ok = fail("unpackLocalVoxelLight normalized float scale failed");

		// 3. Lerp between two LocalVoxelLight samples
		lighting::LocalVoxelLight a{0.2f, glm::vec3(1.0f, 0.0f, 0.5f)};
		lighting::LocalVoxelLight b{0.8f, glm::vec3(0.0f, 1.0f, 0.5f)};

		const auto lerp0 = lighting::lerpLocalVoxelLight(a, b, 0.0f);
		if (std::abs(lerp0.skylight - 0.2f) > 1e-6f || lerp0.blockRgb != a.blockRgb)
			ok = fail("lerpLocalVoxelLight at t=0 must return a");

		const auto lerp1 = lighting::lerpLocalVoxelLight(a, b, 1.0f);
		if (std::abs(lerp1.skylight - 0.8f) > 1e-6f || lerp1.blockRgb != b.blockRgb)
			ok = fail("lerpLocalVoxelLight at t=1 must return b");

		const auto lerpMid = lighting::lerpLocalVoxelLight(a, b, 0.5f);
		if (std::abs(lerpMid.skylight - 0.5f) > 1e-6f ||
		    std::abs(lerpMid.blockRgb.r - 0.5f) > 1e-6f ||
		    std::abs(lerpMid.blockRgb.g - 0.5f) > 1e-6f ||
		    std::abs(lerpMid.blockRgb.b - 0.5f) > 1e-6f)
			ok = fail("lerpLocalVoxelLight at t=0.5 must return midpoint");
	}

	if (!ok)
	{
		std::cerr << "test_render_helpers: FAILED\n";
		return EXIT_FAILURE;
	}
	std::cout << "test_render_helpers: OK (cascades + fog + lighting + materials + block textures + FrameUBO + indirect batching + colorspace + entity lighting)\n";
	return EXIT_SUCCESS;
}
