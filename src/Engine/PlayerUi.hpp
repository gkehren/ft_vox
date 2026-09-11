#pragma once

// UI model helpers for the Status Overlay and the Player / Gameplay panel
// (issue #184): the compact read-only player snapshot, motion-state display
// mapping, world -> chunk display math, the stable block-palette ordering and
// the overlay density policy. Pure and header-only so tests cover them
// without ImGui (tests/test_ui_shell).
//
// The snapshot is filled by Engine::drawUi once per frame; UI surfaces never
// reach into physics::PlayerController directly (issue #184 §6, snapshot
// direction from #179).

#include <utils.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace playerui
{

// --- Status overlay density (single state home: debugui::PanelState) -------

/// Density of the read-only Status Overlay (F1 cycles; View menu radios).
enum class StatusOverlayDensity : int
{
	Off = 0,
	Minimal = 1,
	Detailed = 2
};

/// F1 cycle order: Off -> Minimal -> Detailed -> Off.
inline constexpr StatusOverlayDensity nextStatusOverlayDensity(StatusOverlayDensity d)
{
	return static_cast<StatusOverlayDensity>(
		(static_cast<int>(d) + 1) % (static_cast<int>(StatusOverlayDensity::Detailed) + 1));
}

/// Field-selection policy: Minimal is the frame-health line only; Detailed
/// adds world/player context (position, chunk/biome, selection, target).
inline constexpr bool statusOverlayShowsWorld(StatusOverlayDensity d)
{
	return d == StatusOverlayDensity::Detailed;
}

/// True when a visible surface samples the player biome this frame. Kept
/// next to the density policy so the engine depends on this explicit rule
/// instead of a presentation detail (issue #184 review): only the Detailed
/// overlay shows the biome today, so every other state skips the query.
inline constexpr bool statusOverlayNeedsBiome(StatusOverlayDensity d)
{
	return d == StatusOverlayDensity::Detailed;
}

// --- Player snapshot --------------------------------------------------------

/// UI-facing camera view mode. Kept separate from Camera/CameraMode so this
/// header stays engine-decoupled (issue #184 review): Engine maps between
/// the two when filling the snapshot.
enum class CameraViewMode
{
	Perspective,
	Isometric
};

// --- Player snapshot --------------------------------------------------------

/// Read-only per-frame view of the player for UI surfaces. Value types only;
/// `status` points at Engine-owned text (string literals today). The raw
/// physics counters are NOT shown on gameplay surfaces — they feed the
/// developer console's Player Diagnostics window (issue #179/#184).
struct PlayerSnapshot
{
	glm::vec3 position{0.f};
	float yaw{0.f};
	float pitch{0.f};
	int chunkX{0};
	int chunkZ{0};
	/// BiomeType ordinal at the player column; -1 = not resolved this frame.
	int biome{-1};

	bool flight{false};
	bool grounded{false};
	bool swimming{false};
	bool waitingForTerrain{false};
	/// Horizontal+vertical velocity magnitude in blocks/s.
	float speed{0.f};
	/// Contextual transition/validation message; empty = nothing to show.
	const char *status{""};

	// Raw physics solver counters (diagnostics only).
	uint32_t physicsSteps{0};
	uint64_t queriedCells{0};
	uint64_t queryIterations{0};
	uint64_t droppedSteps{0};
	bool submergedWater{false};
	bool submergedLava{false};

	// Camera view state, snapshot-only (issue #184 review): UI surfaces
	// display these values and mutate through GameUIFrame commands instead
	// of reading/writing Camera directly.
	CameraViewMode cameraViewMode{CameraViewMode::Perspective};
	float cameraMovementSpeed{0.f};
	float mouseSensitivity{0.f};
	float isometricZoom{0.f};
};

/// Motion-state display label. Priority resolves meaningful overlaps the same
/// way the pre-#184 HUD did: flight > waiting for terrain > swimming >
/// grounded > airborne. Every returned view is backed by a string literal, so
/// data() stays valid and null-terminated for printf-style rendering.
inline std::string_view playerMotionLabel(bool flight, bool waitingForTerrain,
										  bool swimming, bool grounded)
{
	if (flight)
		return "Flight";
	if (waitingForTerrain)
		return "Waiting for terrain";
	if (swimming)
		return "Swimming";
	if (grounded)
		return "Grounded";
	return "Airborne";
}

inline std::string_view playerMotionLabel(const PlayerSnapshot &p)
{
	return playerMotionLabel(p.flight, p.waitingForTerrain, p.swimming, p.grounded);
}

/// One-axis world -> chunk display coordinate: floor division by CHUNK_SIZE,
/// mirroring the worldToVoxelColumn contract — floor in double, non-finite
/// inputs map to INT_MIN, and out-of-int-range results clamp instead of
/// invoking UB in the float->int conversion (issue #184 review).
inline int worldToChunkAxis(float world)
{
	if (!std::isfinite(world))
		return std::numeric_limits<int>::min();

	const double chunk = std::floor(static_cast<double>(world) /
									static_cast<double>(CHUNK_SIZE));
	constexpr double kIntMin = static_cast<double>(std::numeric_limits<int>::min());
	constexpr double kIntMax = static_cast<double>(std::numeric_limits<int>::max());
	if (chunk <= kIntMin)
		return std::numeric_limits<int>::min();
	if (chunk >= kIntMax)
		return std::numeric_limits<int>::max();
	return static_cast<int>(chunk);
}

/// Display chunk coordinates for a world position: floor division by
/// CHUNK_SIZE (negative-safe; matches the streaming grid convention).
inline glm::ivec2 worldToChunkCoord(float worldX, float worldZ)
{
	return {worldToChunkAxis(worldX), worldToChunkAxis(worldZ)};
}

/// Biome display name for a snapshot ordinal ("?" for the unresolved case).
inline const char *biomeDisplayName(int biome)
{
	if (biome < 0 || biome >= BIOME_COUNT)
		return "?";
	const std::string_view name = biomeTypeString[biome];
	return name.empty() ? "?" : name.data();
}

// --- Block palette -----------------------------------------------------------

struct BlockPaletteEntry
{
	int id; // TextureType ordinal
	std::string name;
};

/// Stable alphabetical block palette for the selection combo. Deterministic
/// (name, then id) so the list never re-orders between frames or builds; the
/// caller caches the result — this must not run per frame (issue #184 §7).
inline std::vector<BlockPaletteEntry> buildBlockPalette()
{
	std::vector<BlockPaletteEntry> palette;
	palette.reserve(textureTypeString.size());
	for (std::size_t i = 0; i < textureTypeString.size(); ++i)
		palette.push_back({static_cast<int>(i), std::string{textureTypeString[i]}});
	std::sort(palette.begin(), palette.end(), [](const BlockPaletteEntry &a, const BlockPaletteEntry &b) {
		if (a.name != b.name)
			return a.name < b.name;
		return a.id < b.id;
	});
	return palette;
}

} // namespace playerui
