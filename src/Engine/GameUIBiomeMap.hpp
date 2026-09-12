#pragma once

#include <Chunk/BiomeRegionGrid.hpp>
#include <Chunk/TerrainGenerator.hpp>
#include <utils.hpp>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <vector>
#include <glm/glm.hpp>

/// Biome map colors indexed by BiomeType (from legacy UIManager).
inline constexpr unsigned char kBiomeColors[BIOME_COUNT][3] = {
	{100, 150, 200}, // FROZEN_OCEAN
	{220, 230, 240}, // SNOWY_TUNDRA
	{130, 160, 180}, // SNOWY_TAIGA
	{160, 210, 230}, // ICE_SPIKES
	{30, 100, 180},	 // OCEAN
	{210, 200, 150}, // BEACH
	{140, 200, 90},	 // PLAINS
	{50, 130, 50},	 // FOREST
	{100, 170, 80},	 // BIRCH_FOREST
	{30, 80, 30},	 // DARK_FOREST
	{80, 100, 60},	 // SWAMP
	{60, 130, 200},	 // RIVER
	{220, 200, 100}, // DESERT
	{190, 170, 80},	 // SAVANNA
	{40, 160, 40},	 // JUNGLE
	{200, 100, 30},	 // BADLANDS
	{150, 150, 150}, // MOUNTAINS
	{220, 220, 230}, // SNOWY_MOUNTAINS
	{150, 215, 95},	 // FLOWER_MEADOW
	{230, 130, 165}, // CHERRY_GROVE
	{190, 105, 45},	 // AUTUMN_FOREST
	{45, 85, 50},	 // REDWOOD_FOREST
	{55, 85, 55},	 // MANGROVE_SWAMP
	{70, 180, 65},	 // BAMBOO_JUNGLE
	{105, 105, 70},	 // MOOR
	{155, 210, 225}, // GLACIER
	{125, 185, 220}, // FROZEN_RIVER
	{70, 55, 50},	 // VOLCANIC
	{80, 185, 85},	 // OASIS
	{145, 90, 150},	 // MUSHROOM_FIELDS
	{45, 175, 185},	 // CORAL_REEF
};

struct BiomeMapRequest
{
	uint64_t requestId{0};
	uint64_t worldGenerationId{0};
	int seed{0};
	glm::vec2 center{0.f, 0.f};
	int size{256};
	float zoom{0.5f};
	std::shared_ptr<std::atomic<bool>> cancelToken;
	/// Optional injectable test seam hook invoked before sampling.
	std::function<void()> onCheckpoint;
	/// Owner-controlled scratch for the sequential/small-map path. Parallel
	/// tiles never share it; each worker uses bounded thread-local scratch.
	/// When set, the
	/// job reuses it across refreshes (retention bounded by the scratch's
	/// retainedPointsCap); when null, getBiomeRegion falls back to an
	/// internal thread-local scratch. Shared ownership keeps the scratch
	/// alive for the duration of the job even if the owner goes away.
	/// Access relies on the single-flight biome-map job contract (at most
	/// one in-flight request at a time), so no synchronization is needed.
	std::shared_ptr<TerrainGenerator::BiomeRegionScratch> scratch;
};

struct BiomeMapResult
{
	uint64_t requestId{0};
	uint64_t worldGenerationId{0};
	int seed{0};
	glm::vec2 center{0.f, 0.f};
	float zoom{1.f};
	/// Canonical grid describing pixel <-> world <-> voxel-column mapping
	/// for this map (step == 1/zoom, center == request center). The player
	/// marker is placed through it instead of reconstructing the mapping.
	BiomeRegionGrid grid;
	int size{0};
	std::vector<unsigned char> rgba;
	bool valid{false};
	double elapsedMs{0.0}; // scheduled request to completed CPU result, including queue time
};

/// Pending GPU upload request for streamed biome map data. The grid travels
/// with the pixels ON PURPOSE (issue #191 review): pixels and their mapping
/// are one atomic logical unit, so the World panel's published grid can only
/// ever describe the texture that was actually recorded to the GPU.
struct BiomeMapUpload
{
	std::vector<uint8_t> rgba;
	uint32_t width{0};
	uint32_t height{0};
	uint64_t requestId{0};
	/// Canonical grid of these pixels (validated upstream by
	/// isBiomeMapResultAcceptable before staging). Invalid while empty.
	BiomeRegionGrid grid;
};

/// True when a staged upload must be dropped because a newer request
/// superseded it while it waited for staging space (issue #191 review).
/// Dropping must never touch the PUBLISHED grid: the older texture stays on
/// screen and keeps its own mapping.
inline bool isBiomeMapUploadSuperseded(const BiomeMapUpload &pending, uint64_t currentRequestId)
{
	return pending.requestId != 0 && pending.requestId != currentRequestId;
}

/// Validate that a biome map upload request contains well-formed pixel data
/// AND a matching canonical grid: pixels and their mapping are one atomic
/// logical unit (issue #191 review round 2) — an upload without a valid,
/// dimension-matching grid can never be published.
inline bool isBiomeMapUploadValid(const BiomeMapUpload &upload)
{
	return upload.width > 0 &&
		   upload.height > 0 &&
		   upload.requestId > 0 &&
		   upload.grid.valid() &&
		   upload.grid.width == static_cast<int>(upload.width) &&
		   upload.grid.height == static_cast<int>(upload.height) &&
		   upload.rgba.size() == static_cast<size_t>(upload.width) * static_cast<size_t>(upload.height) * 4;
}

/// Frame-order-safe presentation state for the biome map (issue #191 review
/// round 2). Captures the two moments that must never desync:
///  - `publishedGrid`/`hasTexture` describe what ImGui samples in the
///    CURRENT frame — the UI build (drawWorld) reads them;
///  - `pending` carries a freshly accepted result until its GPU upload is
///    recorded AFTER the ImGui pass (postImGuiRecord). Publication therefore
///    lands between frames: the next UI build reads the new grid with the
///    new pixels. No double buffering needed — each frame builds from the
///    last publication.
struct BiomeMapPresentationState
{
	BiomeRegionGrid publishedGrid{};
	bool hasTexture{false};
	BiomeMapUpload pending{};

	/// True while an upload waits for its post-ImGui recording.
	bool hasPending() const { return !pending.rgba.empty(); }

	/// Stage a freshly accepted CPU result (pixels + grid atomically).
	void stage(BiomeMapUpload upload) { pending = std::move(upload); }

	/// Supersede: drop the pending upload; the published pair is untouched
	/// (the older texture remains on screen with its own mapping).
	void dropPending() { pending = {}; }

	/// World/seed invalidation: nothing on screen stays semantically valid.
	void invalidate()
	{
		pending = {};
		publishedGrid = {};
		hasTexture = false;
	}

	/// Called after the copy commands for `pending` were recorded (post-ImGui):
	/// publishes pixels + grid together for the NEXT frame. Returns false when
	/// there was nothing publishable. Last safety barrier: the FULL upload
	/// validity is re-checked here, so an invalid pending can never mutate the
	/// published state even if a caller skipped the earlier validation
	/// (issue #191 review round 3).
	bool publishPending()
	{
		if (!isBiomeMapUploadValid(pending))
			return false;
		publishedGrid = pending.grid;
		hasTexture = true;
		pending = {};
		return true;
	}
};

/// Check if a biome map result matches active world generation, seed, request ID,
/// and internal invariant checks before GPU publication.
inline bool isBiomeMapResultAcceptable(const BiomeMapResult &result,
										  uint64_t currentWorldGenId,
										  int currentSeed,
										  uint64_t currentRequestId)
{
	return result.valid &&
		   result.worldGenerationId == currentWorldGenId &&
		   result.seed == currentSeed &&
		   result.requestId == currentRequestId &&
		   result.size > 0 &&
		   result.zoom > 0.0f &&
		   result.grid.valid() &&
		   result.grid.width == result.size &&
		   result.grid.height == result.size &&
		   result.rgba.size() == static_cast<size_t>(result.size) * static_cast<size_t>(result.size) * 4;
}

/// Check whether the existing GPU backing resources can be reused without destruction/recreation.
/// Semantic content validity (m_mapHasTexture) does not affect backing resource reuse.
inline bool canReuseBiomeTexture(bool imageExists,
								 bool descriptorExists,
								 int existingSize,
								 int requestedSize)
{
	return imageExists && descriptorExists && (existingSize == requestedSize) && (requestedSize > 0);
}

/// Determine if player movement should trigger superseding the active biome map request.
inline bool shouldSupersedeBiomeMap(glm::vec2 player,
									glm::vec2 lastPlayer,
									bool follow)
{
	if (!follow)
		return false;
	return glm::length(player - lastPlayer) > 8.0f;
}

/// Whether the World panel should show its "Updating map..." indicator
/// (issue #191 review round 3): a job in flight, an upload awaiting its
/// post-ImGui recording, or a refresh already requested for the next tick
/// (the small window between supersede and the next dispatch). Pure so the
/// indicator rule is testable without ImGui.
inline bool biomeMapUpdatePending(bool jobRunning, bool uploadPending, bool needsUpdate)
{
	return jobRunning || uploadPending || needsUpdate;
}

/// Continuous (float) map-pixel coordinates for a world position on a
/// published biome grid — the pixel-center convention of BiomeRegionGrid
/// without the display rounding: row 0 is the min-Z edge, `center` falls at
/// pixel size*0.5 (even sizes). Pure so tests and the World-panel draw-list
/// overlays share one mapping (issue #186); callers map this into screen
/// space with the drawn image rect.
inline glm::vec2 biomeMapContinuousPixel(const BiomeRegionGrid &grid, glm::vec2 world)
{
	return {(world.x - grid.center.x) / grid.step + static_cast<float>(grid.width) * 0.5f,
			(world.y - grid.center.y) / grid.step + static_cast<float>(grid.height) * 0.5f};
}

/// View center after panning the map by a drag delta in screen pixels
/// (issue #192): the content follows the cursor, so the center moves the
/// OPPOSITE way — screen +X is world +X, but screen +Y (down) is world -Y.
/// `screenPxPerMapPx` converts drag pixels into map pixels through the
/// drawn image rect; an invalid grid or non-positive scale is a no-op.
/// Pure so tests and the World-panel drag handler share one mapping.
inline glm::vec2 biomeMapPanCenter(const BiomeRegionGrid &grid,
								   glm::vec2 center,
								   glm::vec2 dragPx,
								   float screenPxPerMapPx)
{
	if (!grid.valid() || !(screenPxPerMapPx > 0.f))
		return center;
	const float worldPerPx = grid.step / screenPxPerMapPx;
	return {center.x - dragPx.x * worldPerPx, center.y - dragPx.y * worldPerPx};
}

// ---------------------------------------------------------------------------
// Drag-pan state machine (issue #192 review): Idle -> Pressed -> Dragging.
//
// A plain RMB/MMB press must be a STRICT no-op (no center change, no request
// supersede), so the pan only arms on the press and starts translating once
// ImGui's drag threshold is exceeded. The initiating button stays sticky: a
// pan started with RMB ends when RMB is released, even if MMB is still held.
// Nothing here touches ImGui — drawWorld() translates io state into
// BiomeMapPanInput and applies the returned translation — so every
// transition is unit-testable.

/// Pan-initiating mouse buttons in priority order (RMB wins a simultaneous
/// press). Values mirror ImGuiMouseButton_Right / _Middle; kept as plain
/// ints so this header stays ImGui-free. LMB is deliberately excluded for
/// future map interactions.
inline constexpr int kBiomeMapPanButtons[2] = {1, 2};
/// Sentinel: no initiating button.
inline constexpr int kBiomeMapPanButtonNone = -1;

enum class BiomeMapPanState
{
	Idle,	 ///< Not panning; may still carry a preview offset from a
			 ///< finished drag that the shown texture does not reflect yet.
	Pressed, ///< Initiating button down over the map, below the drag
			 ///< threshold. Translates nothing.
	Dragging ///< Past the drag threshold: every frame's mouse delta moves
			 ///< the view center and the preview offset.
};

/// Per-button ImGui snapshot for one frame.
struct BiomeMapPanButtonInput
{
	bool clicked{false};  ///< Pressed this frame.
	bool down{false};	  ///< Currently held.
	bool dragging{false}; ///< ImGui::IsMouseDragging(button, threshold).
	/// ImGui::GetMouseDragDelta(button, 0.f): movement since the mouse-down
	/// position, regardless of the threshold. SINGLE SOURCE OF TRUTH for the
	/// first drag frame — whether the threshold was crossed one frame or ten
	/// frames after the press, this is exactly everything that must be
	/// applied, because nothing is ever translated before Dragging.
	glm::vec2 dragFromClick{0.f, 0.f};
};

struct BiomeMapPanInput
{
	bool mapActive{false}; ///< Map drawn with a valid published grid.
	bool hovered{false};   ///< Cursor over the map this frame.
	bool follow{false};	 ///< Follow player enabled: pan disabled, state resets.
	BiomeMapPanButtonInput buttons[2]; ///< Parallel to kBiomeMapPanButtons.
	glm::vec2 mouseDelta{0.f}; ///< io.MouseDelta for this frame.
};

/// Pan controller state owned by GameUI across frames.
struct BiomeMapPan
{
	BiomeMapPanState state{BiomeMapPanState::Idle};
	int button{kBiomeMapPanButtonNone}; ///< Initiating button while armed.
	/// Screen px the SHOWN texture stays shifted by while the async
	/// generation catches up with m_mapCenter. Accumulates while dragging,
	/// survives the drag (the texture must not snap back), and is cleared
	/// only when a publication represents the current view, on follow
	/// re-enable, or on map invalidation.
	glm::vec2 previewOffset{0.f, 0.f};
};

/// One step's outcome: the new controller state plus what the caller must
/// apply this frame.
struct BiomeMapPanStep
{
	BiomeMapPan pan;			///< New controller state.
	glm::vec2 dragDelta{0.f};   ///< NEW screen-px translation this frame
								/// (non-zero only while dragging).
	bool dragStarted{false};	///< Pressed -> Dragging this frame.
	bool dragEnded{false};		///< Dragging -> Idle this frame (button release).
};

inline const BiomeMapPanButtonInput &biomeMapPanButtonInput(const BiomeMapPanInput &in,
															int button)
{
	return in.buttons[button == kBiomeMapPanButtons[1] ? 1 : 0];
}

/// Advance the pan state machine by one frame. Pure.
inline BiomeMapPanStep stepBiomeMapPan(const BiomeMapPan &pan, const BiomeMapPanInput &in)
{
	BiomeMapPanStep out;
	out.pan = pan;

	// Hard resets: follow owns the center again, or there is no map to pan
	// (invalidation / world change). The preview goes with it — there is
	// nothing on screen left to bridge.
	if (in.follow || !in.mapActive)
	{
		out.pan = BiomeMapPan{};
		return out;
	}

	switch (pan.state)
	{
	case BiomeMapPanState::Idle:
		// Arm only on a press OVER the map. No translation, no supersede —
		// except a fast flick where clicked and past-the-threshold land in
		// the SAME frame: the whole movement since the mouse-down is then
		// applied immediately, straight to Dragging (never a lost Pressed
		// frame, never a rebuilt delta).
		if (in.hovered)
		{
			for (int i = 0; i < 2; ++i)
			{
				if (!in.buttons[i].clicked)
					continue;
				out.pan.button = kBiomeMapPanButtons[i];
				if (in.buttons[i].dragging)
				{
					out.pan.state = BiomeMapPanState::Dragging;
					out.dragStarted = true;
					out.dragDelta = in.buttons[i].dragFromClick;
					out.pan.previewOffset += out.dragDelta;
				}
				else
				{
					out.pan.state = BiomeMapPanState::Pressed;
				}
				break;
			}
		}
		break;

	case BiomeMapPanState::Pressed:
	{
		const BiomeMapPanButtonInput &init = biomeMapPanButtonInput(in, pan.button);
		if (!init.down)
		{
			// Released before the threshold: a plain click, strictly a
			// no-op. Any sub-threshold movement was never applied (it only
			// ever lived in ImGui's drag delta); the preview offset from an
			// EARLIER drag is kept — the shown texture is still offset
			// until its replacement publishes.
			out.pan.state = BiomeMapPanState::Idle;
			out.pan.button = kBiomeMapPanButtonNone;
			break;
		}
		if (init.dragging)
		{
			// Threshold crossed: the drag becomes real. dragFromClick is
			// the WHOLE movement since the mouse-down — exactly everything
			// to apply, since nothing was translated before Dragging — so
			// the map lands exactly under the cursor with no catch-up jump.
			out.pan.state = BiomeMapPanState::Dragging;
			out.dragStarted = true;
			out.dragDelta = init.dragFromClick;
			out.pan.previewOffset += out.dragDelta;
		}
		break;
	}

	case BiomeMapPanState::Dragging:
	{
		const BiomeMapPanButtonInput &init = biomeMapPanButtonInput(in, pan.button);
		if (!init.down)
		{
			// Initiating button released (even with the other button still
			// held). The drag ends; the preview offset PERSISTS until the
			// texture for the dragged-to view is published.
			out.pan.state = BiomeMapPanState::Idle;
			out.pan.button = kBiomeMapPanButtonNone;
			out.dragEnded = true;
			break;
		}
		// Dragging continues regardless of hover: the cursor may leave the
		// map rect mid-drag without aborting the pan.
		out.dragDelta = in.mouseDelta;
		out.pan.previewOffset += in.mouseDelta;
		break;
	}
	}
	return out;
}

/// Clear the pan preview once the published texture represents the CURRENT
/// view (accepted result actually published). Called ONLY from the
/// publication path — never when a stale result is rejected: a rejection
/// means the screen still shows the old view and the offset is what keeps
/// it visually aligned with m_mapCenter.
inline void clearBiomeMapPanPreview(BiomeMapPan &pan)
{
	pan.previewOffset = {0.f, 0.f};
}

/// Screen-px offset that puts `targetCenter` exactly at the viewport center
/// while the SHOWN texture still corresponds to `publishedGrid` (issue #192
/// review). The sign mirrors biomeMapPanCenter: the view center moving east
/// means the shown pixels must shift west, and panning by drag delta D from
/// a grid-centered view yields exactly offset D back. Zero on an invalid
/// grid or non-positive scale.
inline glm::vec2 biomeMapPreviewOffsetForCenter(const BiomeRegionGrid &publishedGrid,
												glm::vec2 targetCenter,
												float screenPxPerMapPx)
{
	if (!publishedGrid.valid() || !(screenPxPerMapPx > 0.f))
		return {0.f, 0.f};
	const glm::vec2 mapPx =
		(targetCenter - publishedGrid.center) / publishedGrid.step;
	return -mapPx * screenPxPerMapPx;
}

/// Re-anchor the pan controller after the Center button re-targets the view
/// center to `targetCenter` (issue #192 review): the shown map jumps
/// straight to the new target — even while a pending pan preview toward an
/// older target is on screen — and any armed/dragging interaction is
/// cancelled so a still-held button cannot keep moving the center. Pure.
inline void reanchorBiomeMapPanForCenter(BiomeMapPan &pan,
										 const BiomeRegionGrid &publishedGrid,
										 glm::vec2 targetCenter,
										 float screenPxPerMapPx)
{
	pan.state = BiomeMapPanState::Idle;
	pan.button = kBiomeMapPanButtonNone;
	pan.previewOffset = biomeMapPreviewOffsetForCenter(publishedGrid, targetCenter,
													   screenPxPerMapPx);
}

/// Paint the player indicator dot (black outline with white center) into the
/// RGBA buffer. The dot pixel is grid.pixelForWorld(playerXZ) (nearest display
/// pixel); when it falls outside the grid nothing is painted.
void paintBiomeMapPlayerDot(std::vector<unsigned char> &rgba,
							const BiomeRegionGrid &grid,
							glm::vec2 playerXZ);

/// Sample biomes and convert them to an RGBA pixel buffer using thread-local generator.
/// Fully self-contained: owns its buffers and does not retain raw pointers to Engine state.
/// Cancellation is checked before sampling, during region sampling (between
/// tiles), and after sampling — an interrupted build returns an invalid
/// result and never publishes partial pixel data.
BiomeMapResult generateBiomeMap(const BiomeMapRequest &req);

class ThreadPool;
/// Engine-owned Low-priority tile scheduling. No worker waits for children.
/// Request data lives until every tile retires; the last tile publishes.
/// The pool must outlive submission/execution. GameUI enforces single-flight.
std::future<BiomeMapResult> submitBiomeMap(ThreadPool &pool, BiomeMapRequest req);

