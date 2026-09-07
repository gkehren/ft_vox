#pragma once

/// Cascaded shadow map math: splits, frustum-slice light matrices, bias/blend helpers.
/// Used by WorldRenderer / ShadowPass and unit tests.

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace shadow
{

inline constexpr int kCascadeCount = 3;
inline constexpr float kCascadeLambda = 0.65f;
inline constexpr float kDefaultFovYDegrees = 80.0f;
inline constexpr float kCascadeBlendFraction = 0.12f; // soft blend band as fraction of split gap
inline constexpr uint32_t kShadowMapSize = 1024;

/// Practical cascade split distances in view space (near → far).
/// cascadeSplits[i] is the far bound of cascade i; cascade 0 starts at nearPlane.
inline std::array<float, kCascadeCount> computeCascadeSplits(float nearPlane, float farPlane,
															 float lambda = kCascadeLambda)
{
	std::array<float, kCascadeCount> splits{};
	const float n = std::max(nearPlane, 0.01f);
	const float f = std::max(farPlane, n + 1.0f);
	const float ratio = f / n;
	for (int i = 0; i < kCascadeCount; ++i)
	{
		const float p = static_cast<float>(i + 1) / static_cast<float>(kCascadeCount);
		const float logSplit = n * std::pow(ratio, p);
		const float uniSplit = n + (f - n) * p;
		splits[i] = lambda * logSplit + (1.0f - lambda) * uniSplit;
	}
	for (int i = 1; i < kCascadeCount; ++i)
		splits[i] = std::max(splits[i], splits[i - 1] + 1.0f);
	splits[kCascadeCount - 1] = f;
	return splits;
}

/// Stable light-space up vector (avoids lookAt singularity when sun is near vertical).
inline glm::vec3 stableLightUp(const glm::vec3 &lightDir)
{
	const glm::vec3 dir = glm::normalize(lightDir);
	glm::vec3 up(0.0f, 1.0f, 0.0f);
	if (std::abs(glm::dot(dir, up)) > 0.95f)
		up = glm::vec3(0.0f, 0.0f, 1.0f);
	// Orthonormalize against light dir
	const glm::vec3 right = glm::normalize(glm::cross(up, dir));
	return glm::normalize(glm::cross(dir, right));
}

/// Build 8 corners of a view-frustum slice [sliceNear, sliceFar] in world space.
/// Camera basis: position + front (view -Z), right, up. fovY in radians.
inline void frustumSliceCornersWorld(const glm::vec3 &camPos, const glm::vec3 &front,
									 const glm::vec3 &right, const glm::vec3 &up,
									 float fovYRadians, float aspect,
									 float sliceNear, float sliceFar,
									 std::array<glm::vec3, 8> &outCorners)
{
	const float tanHalf = std::tan(fovYRadians * 0.5f);
	const float hn = sliceNear * tanHalf;
	const float wn = hn * aspect;
	const float hf = sliceFar * tanHalf;
	const float wf = hf * aspect;

	const glm::vec3 fc = camPos + front * sliceFar;
	const glm::vec3 nc = camPos + front * sliceNear;

	// Near plane: TL, TR, BR, BL
	outCorners[0] = nc + up * hn - right * wn;
	outCorners[1] = nc + up * hn + right * wn;
	outCorners[2] = nc - up * hn + right * wn;
	outCorners[3] = nc - up * hn - right * wn;
	// Far plane
	outCorners[4] = fc + up * hf - right * wf;
	outCorners[5] = fc + up * hf + right * wf;
	outCorners[6] = fc - up * hf + right * wf;
	outCorners[7] = fc - up * hf - right * wf;
}

/// Light-space ortho extent (half-width of XY AABB) for testing coverage progression.
struct CascadeBounds
{
	glm::mat4 lightView{1.0f};
	glm::mat4 lightProj{1.0f};
	glm::mat4 lightViewProj{1.0f};
	float halfExtentX{0.f};
	float halfExtentY{0.f};
	float zNear{0.f};
	float zFar{0.f};
	bool finite{false};
};

/// Frustum-slice cascade matrix with a WORLD-STABLE texel grid (issue #137).
///
/// Stability comes from three properties:
/// 1. The light rotation is built WITHOUT a camera-following translation, so
///    corner coordinates can be expressed in an ABSOLUTE light frame.
/// 2. The ortho extent is a slice bounding-sphere radius — it depends only on
///    the slice range and fov, so the box does not breathe when the camera
///    rotates (a tight AABB would resize continuously).
/// 3. The view anchor is the sphere center snapped to the texel grid IN THE
///    ABSOLUTE light frame, so the view (and the texel grid it defines) only
///    ever moves in whole-texel steps: a fixed world point keeps its
///    fractional texel position under camera translation AND rotation.
inline CascadeBounds computeFrustumSliceCascade(const glm::vec3 &camPos, const glm::vec3 &front,
												const glm::vec3 &right, const glm::vec3 &up,
												const glm::vec3 &lightDir,
												float fovYRadians, float aspect,
												float sliceNear, float sliceFar,
												uint32_t shadowMapResolution = kShadowMapSize)
{
	CascadeBounds out{};
	const glm::vec3 dir = glm::normalize(lightDir);
	const glm::vec3 lightUp = stableLightUp(dir);

	std::array<glm::vec3, 8> corners{};
	frustumSliceCornersWorld(camPos, front, right, up, fovYRadians, aspect, sliceNear, sliceFar, corners);

	// Absolute light frame: pure rotation (lookAt from the origin), no
	// camera-following translation.
	const glm::mat3 lightRot(glm::lookAt(dir, glm::vec3(0.f), lightUp));

	// Slice bounding sphere centered on the view axis: enclosing radius of
	// the eight slice corners. Rotation-invariant by construction.
	const glm::vec3 sphereCenter = camPos + front * (0.5f * (sliceNear + sliceFar));
	float radius = 0.f;
	for (const auto &c : corners)
		radius = std::max(radius, glm::length(c - sphereCenter));
	radius += 4.f; // filter-radius margin

	const float texel = (2.f * radius) / static_cast<float>(std::max(1u, shadowMapResolution));

	// Snap the sphere center to the texel grid in the ABSOLUTE light frame,
	// then map it back to a world anchor for the view.
	const glm::vec3 absCenter = lightRot * sphereCenter;
	const glm::vec3 snappedAbs(std::floor(absCenter.x / texel) * texel,
							   std::floor(absCenter.y / texel) * texel,
							   absCenter.z);
	const glm::vec3 worldAnchor = glm::transpose(lightRot) * snappedAbs;

	// Light view: eye on the *sun side* of the anchor. lightDir is "toward
	// the sun" (same as terrain.frag N·L), so the light arrives from +dir.
	// (anchor - dir put the camera on the anti-sun side → inverted self-shadow.)
	const float pullBack = 500.f;
	out.lightView = glm::lookAt(worldAnchor + dir * pullBack, worldAnchor, lightUp);
	out.halfExtentX = radius;
	out.halfExtentY = radius;

	// Depth bounds from the corners in the final view frame (Z is not part
	// of the XY texel grid; pads keep inter-slice casters).
	float minZ = std::numeric_limits<float>::max();
	float maxZ = std::numeric_limits<float>::lowest();
	for (const auto &c : corners)
	{
		const float z = (out.lightView * glm::vec4(c, 1.f)).z;
		minZ = std::min(minZ, z);
		maxZ = std::max(maxZ, z);
	}
	const float zPad = (maxZ - minZ) * 0.5f + 80.f;
	minZ -= zPad;
	maxZ += 16.f;

	// GLM RH lookAt: view looks down -Z, so scene points have negative eye Z.
	// glm::ortho(zNear, zFar) expects positive distances (maps eye.z ∈ [-zFar,-zNear]).
	// Closest corner → least-negative Z (maxZ); farthest → most-negative Z (minZ).
	float zNearDist = -maxZ;
	float zFarDist = -minZ;
	if (zNearDist > zFarDist)
		std::swap(zNearDist, zFarDist);
	zNearDist = std::max(zNearDist, 0.1f);
	if (zFarDist <= zNearDist + 1.f)
		zFarDist = zNearDist + 1.f;
	out.zNear = zNearDist;
	out.zFar = zFarDist;

	out.lightProj = glm::ortho(-radius, radius, -radius, radius, zNearDist, zFarDist);
	out.lightViewProj = out.lightProj * out.lightView;
	out.finite = std::isfinite(out.halfExtentX) && std::isfinite(out.halfExtentY) &&
				 std::isfinite(out.lightViewProj[0][0]) && std::isfinite(out.lightViewProj[3][3]);
	return out;
}

/// Fill cascade matrices + splits for FrameUBO. Uses frustum-slice orthos (not camera spheres).
/// front/right/up must be orthonormal camera basis; fovYDegrees matches Camera perspective (80°).
inline void buildCascadeUBO(const glm::vec3 &camPos, const glm::vec3 &front, const glm::vec3 &right,
							const glm::vec3 &up, const glm::vec3 &lightDir,
							float nearPlane, float farPlane, float aspect, float fovYDegrees,
							std::array<glm::mat4, kCascadeCount> &outMatrices, glm::vec4 &outSplits,
							std::array<float, kCascadeCount> *outHalfExtents = nullptr,
							uint32_t shadowMapResolution = kShadowMapSize,
							std::array<float, kCascadeCount> *outDepthSpans = nullptr)
{
	const auto splits = computeCascadeSplits(nearPlane, farPlane);
	const float fovY = glm::radians(fovYDegrees);
	const glm::vec3 f = glm::normalize(front);
	const glm::vec3 r = glm::normalize(right);
	const glm::vec3 u = glm::normalize(up);
	float prev = nearPlane;
	for (int i = 0; i < kCascadeCount; ++i)
	{
		const CascadeBounds b = computeFrustumSliceCascade(
			camPos, f, r, u, lightDir, fovY, std::max(aspect, 0.1f), prev, splits[i],
			shadowMapResolution);
		outMatrices[i] = b.lightViewProj;
		if (outHalfExtents)
			(*outHalfExtents)[i] = std::max(b.halfExtentX, b.halfExtentY);
		if (outDepthSpans)
			(*outDepthSpans)[i] = std::max(b.zFar - b.zNear, 1.0f);
		prev = splits[i];
	}
	outSplits = glm::vec4(splits[0], splits[1], splits[2], static_cast<float>(kCascadeCount));
}

/// Convenience: build camera basis from position + front + worldUp (same as typical free-look).
inline void buildCascadeUBOFromFront(const glm::vec3 &camPos, const glm::vec3 &front,
									 const glm::vec3 &worldUp, const glm::vec3 &lightDir,
									 float nearPlane, float farPlane, float aspect, float fovYDegrees,
									 std::array<glm::mat4, kCascadeCount> &outMatrices, glm::vec4 &outSplits,
									 std::array<float, kCascadeCount> *outHalfExtents = nullptr,
									 uint32_t shadowMapResolution = kShadowMapSize,
									 std::array<float, kCascadeCount> *outDepthSpans = nullptr)
{
	const glm::vec3 f = glm::normalize(front);
	glm::vec3 r = glm::cross(f, glm::normalize(worldUp));
	if (glm::dot(r, r) < 1e-8f)
		r = glm::cross(f, glm::vec3(0.f, 0.f, 1.f));
	r = glm::normalize(r);
	const glm::vec3 u = glm::normalize(glm::cross(r, f));
	buildCascadeUBO(camPos, f, r, u, lightDir, nearPlane, farPlane, aspect, fovYDegrees,
					outMatrices, outSplits, outHalfExtents, shadowMapResolution, outDepthSpans);
}

/// Legacy overload kept for older call sites / tests — builds a default forward camera.
inline void buildCascadeUBO(const glm::vec3 &cameraPos, const glm::vec3 &lightDir,
							float nearPlane, float farPlane,
							std::array<glm::mat4, kCascadeCount> &outMatrices,
							glm::vec4 &outSplits)
{
	buildCascadeUBOFromFront(cameraPos, glm::vec3(0.f, 0.f, -1.f), glm::vec3(0.f, 1.f, 0.f), lightDir,
							 nearPlane, farPlane, 16.f / 9.f, kDefaultFovYDegrees, outMatrices, outSplits,
							 nullptr);
}

/// Shadow depth bias matching terrain.frag (for unit tests / docs).
/// DEPRECATED form: absolute normalized-depth constants that ignore the
/// cascade footprint. Kept only to document what the receiver bias was
/// calibrated against; the live contract is the dimensionless
/// kReceiverBiasSlope/kReceiverBiasBase pair applied to
/// FrameUBO::cascadeBiasScales (worldUnitsPerTexel / depthSpan) in
/// csm.inc.glsl — see the BIAS POLICY header there.
inline float shadowDepthBias(float nDotL)
{
	const float ndl = std::clamp(nDotL, 0.0f, 1.0f);
	return std::max(0.012f * (1.0f - ndl), 0.0035f);
}

/// Dimensionless receiver-bias factors (issue #137): the shader multiplies
/// them by the cascade's normalized-depth-per-texel footprint
/// (worldUnitsPerTexel / light-space depth span), so the bias halves when
/// the shadow map resolution doubles and grows with cascade footprint.
/// Calibrated so cascade 0 @1024 reproduces the legacy shadowDepthBias
/// magnitudes. Keep in sync with csm.inc.glsl.
inline constexpr float kReceiverBiasSlope = 22.0f;
inline constexpr float kReceiverBiasBase = 6.5f;

/// Cascade blend weight in [0,1] for soft transition near split (matches terrain.frag).
/// viewDepth in same space as splits; returns 1 = fully this cascade, lower = blend toward next.
inline float cascadeBlendWeight(float viewDepth, float splitEnd, float splitStart, float blendFraction)
{
	const float gap = std::max(splitEnd - splitStart, 1.0f);
	const float band = gap * std::clamp(blendFraction, 0.01f, 0.5f);
	const float edge = splitEnd - band;
	if (viewDepth <= edge)
		return 1.0f;
	if (viewDepth >= splitEnd)
		return 0.0f;
	return 1.0f - (viewDepth - edge) / band;
}


} // namespace shadow
