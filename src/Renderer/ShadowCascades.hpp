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

/// Frustum-slice cascade matrix: tight ortho around the view frustum slice in light space.
/// Texel-snaps the center to reduce swimming. shadowMapResolution used for snap quanta.
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

	// Center of frustum slice in world
	glm::vec3 center(0.f);
	for (const auto &c : corners)
		center += c;
	center *= (1.f / 8.f);

	// Light view: camera sits on the *sun side* looking at the slice center.
	// lightDir is "toward the sun" (same as terrain.frag N·L), so the light
	// arrives from +dir; place the cascade eye at center + dir * pullBack.
	// (center - dir put the camera on the anti-sun side → inverted self-shadow.)
	const float pullBack = 500.f;
	const glm::vec3 lightEye = center + dir * pullBack;
	out.lightView = glm::lookAt(lightEye, center, lightUp);

	// AABB of corners in light space
	float minX = std::numeric_limits<float>::max();
	float minY = std::numeric_limits<float>::max();
	float minZ = std::numeric_limits<float>::max();
	float maxX = std::numeric_limits<float>::lowest();
	float maxY = std::numeric_limits<float>::lowest();
	float maxZ = std::numeric_limits<float>::lowest();
	for (const auto &c : corners)
	{
		const glm::vec3 ls = glm::vec3(out.lightView * glm::vec4(c, 1.f));
		minX = std::min(minX, ls.x);
		minY = std::min(minY, ls.y);
		minZ = std::min(minZ, ls.z);
		maxX = std::max(maxX, ls.x);
		maxY = std::max(maxY, ls.y);
		maxZ = std::max(maxZ, ls.z);
	}

	// Pad XY (acne / filter radius) and extend Z for casters outside the slice
	const float padXY = std::max((maxX - minX), (maxY - minY)) * 0.08f + 4.f;
	minX -= padXY;
	maxX += padXY;
	minY -= padXY;
	maxY += padXY;
	// Pull Z back so casters between light and frustum still cast into the slice
	const float zPad = (maxZ - minZ) * 0.5f + 80.f;
	minZ -= zPad;
	maxZ += 16.f;

	// Texel snap: quantize ortho center so shadows don't swim
	const float worldUnitsPerTexelX = (maxX - minX) / static_cast<float>(std::max(1u, shadowMapResolution));
	const float worldUnitsPerTexelY = (maxY - minY) / static_cast<float>(std::max(1u, shadowMapResolution));
	if (worldUnitsPerTexelX > 1e-6f && worldUnitsPerTexelY > 1e-6f)
	{
		const float midX = 0.5f * (minX + maxX);
		const float midY = 0.5f * (minY + maxY);
		const float snappedX = std::floor(midX / worldUnitsPerTexelX) * worldUnitsPerTexelX;
		const float snappedY = std::floor(midY / worldUnitsPerTexelY) * worldUnitsPerTexelY;
		const float dx = snappedX - midX;
		const float dy = snappedY - midY;
		minX += dx;
		maxX += dx;
		minY += dy;
		maxY += dy;
	}

	out.halfExtentX = 0.5f * (maxX - minX);
	out.halfExtentY = 0.5f * (maxY - minY);

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

	out.lightProj = glm::ortho(minX, maxX, minY, maxY, zNearDist, zFarDist);
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
							std::array<float, kCascadeCount> *outHalfExtents = nullptr)
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
			camPos, f, r, u, lightDir, fovY, std::max(aspect, 0.1f), prev, splits[i], kShadowMapSize);
		outMatrices[i] = b.lightViewProj;
		if (outHalfExtents)
			(*outHalfExtents)[i] = std::max(b.halfExtentX, b.halfExtentY);
		prev = splits[i];
	}
	outSplits = glm::vec4(splits[0], splits[1], splits[2], static_cast<float>(kCascadeCount));
}

/// Convenience: build camera basis from position + front + worldUp (same as typical free-look).
inline void buildCascadeUBOFromFront(const glm::vec3 &camPos, const glm::vec3 &front,
									 const glm::vec3 &worldUp, const glm::vec3 &lightDir,
									 float nearPlane, float farPlane, float aspect, float fovYDegrees,
									 std::array<glm::mat4, kCascadeCount> &outMatrices, glm::vec4 &outSplits,
									 std::array<float, kCascadeCount> *outHalfExtents = nullptr)
{
	const glm::vec3 f = glm::normalize(front);
	glm::vec3 r = glm::cross(f, glm::normalize(worldUp));
	if (glm::dot(r, r) < 1e-8f)
		r = glm::cross(f, glm::vec3(0.f, 0.f, 1.f));
	r = glm::normalize(r);
	const glm::vec3 u = glm::normalize(glm::cross(r, f));
	buildCascadeUBO(camPos, f, r, u, lightDir, nearPlane, farPlane, aspect, fovYDegrees,
					outMatrices, outSplits, outHalfExtents);
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
inline float shadowDepthBias(float nDotL)
{
	const float ndl = std::clamp(nDotL, 0.0f, 1.0f);
	return std::max(0.012f * (1.0f - ndl), 0.0035f);
}

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
