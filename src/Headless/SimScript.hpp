#pragma once

// =============================================================================
// SimScript — the bounded, deterministic action script of the headless
// simulation runner (issue #210, Epic #204 Phase F).
//
// Line-based plain text; '#' starts a comment, blank lines are ignored.
// Actions execute sequentially at host-frame granularity: `wait N` advances
// N fixed-dt frames with the current player command, every other action is
// instantaneous between frames. The parser is pure (no world/simulation
// dependency) so it is unit-testable and fail-fast: any malformed line is an
// error carrying the source line number, never a silent skip.
//
// Canonical grammar (one action per line):
//   wait <frames>                     advance N host frames (1..1000000)
//   move <x> <z>                      set the world-XZ movement direction
//   sprint on|off                     sprint toggle
//   jump                              one-frame jump press (next frame)
//   swim up|down|off                  vertical swim input
//   flight on|off                     flight toggle at the current feet
//   teleport <x> <y> <z>              host-driven reposition (doubles)
//   place <x> <y> <z> <block>         cell edit, gated by canPlaceVoxel
//   delete <x> <y> <z>                cell edit (authorized local edit)
//   interest <x> <y> <z>              pin the headless interest probe
//   interest follow                   interest follows the player again
//   save                              durable player-state capture + flush
//   expect voxel <x> <y> <z> <block|air|unknown>   immediate assertion
//   expect digest <hex>               final-digest assertion (checked at end)
//
// <block> is a TextureType display name (case-insensitive, spaces or
// underscores, e.g. "Oak Leaves" / OAK_LEAVES) or a numeric ordinal.
// Cell coordinates are integers; edits target the cell center.
// =============================================================================

#include <filesystem>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <utils.hpp>

namespace headless
{

enum class ScriptOp
{
	Wait,
	Move,
	Sprint,
	Jump,
	Swim,
	Flight,
	Teleport,
	Place,
	Delete,
	Interest,
	InterestFollow,
	Save,
	ExpectVoxel,
	ExpectDigest
};

/// Sentinel for `expect voxel ... unknown`: the cell must be UNAVAILABLE
/// (unloaded / not generated). Any other expected value is a TextureType
/// ordinal (AIR included — it is a real stored type).
inline constexpr int kExpectUnknown = -2;

struct ScriptAction
{
	ScriptOp op{ScriptOp::Wait};
	int frames{0};			   // Wait
	glm::dvec2 move{0.0};	   // Move
	bool enabled{false};	   // Sprint / Flight
	int swimDirection{0};	   // Swim: -1 down, 0 off, +1 up
	glm::dvec3 position{0.0};  // Teleport / Interest
	glm::ivec3 cell{0};		   // Place / Delete / ExpectVoxel
	TextureType block{AIR};	   // Place
	int expectedVoxel{0};	   // ExpectVoxel: ordinal or kExpectUnknown
	std::string expectedDigest; // ExpectDigest: lowercase hex, no 0x
	int line{0};			   // source line (diagnostics)
};

struct SimScript
{
	std::vector<ScriptAction> actions;
	int totalWaitFrames{0};
};

/// Parse a script file. Returns false and fills `error` (with the offending
/// line number) on any I/O or grammar problem. Bounded: at most
/// kMaxScriptActions actions, each `wait` at most kMaxWaitFrames frames.
bool parseSimScript(const std::filesystem::path &path, SimScript &out, std::string &error);

/// Case-insensitive block-name lookup (display names with spaces or
/// underscores, enum-style identifiers, numeric ordinals, "air").
/// Returns false when the token names no block.
bool parseBlockName(const std::string &token, TextureType &out);

inline constexpr int kMaxScriptActions = 100000;
inline constexpr int kMaxWaitFrames = 1000000;

} // namespace headless
