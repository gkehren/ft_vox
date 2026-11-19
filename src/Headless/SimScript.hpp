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
//   give <item> <count>               grantItems admin seam (registry item token)
//   select <slot>                     hotbar selection (0..8)
//   drop <count>                      drop from selected slot, fixed +Z direction
//   break <x> <y> <z>                 gameplay breakBlock (drops registry item)
//   placeSelected <x> <y> <z>         gameplay place-from-hotbar (consumes 1)
//   interest <x> <y> <z>              pin the headless interest probe
//   interest follow                   interest follows the player again
//   interest radius <chunks>          set the selected player's radius
//   player create|select <id>|remove <id>   control the multiplayer sessions
//   save                              durable player-state capture + flush
//   expect voxel <x> <y> <z> <block|air|unknown>   immediate assertion
//   expect inventory <slot> <item|empty> <count>   immediate assertion (slot 0..35)
//   expect items <n>                  immediate live-entity-count assertion
//   expect item <id> <item> <count>    immediate shared item assertion
//   expect result <name>              check the last command result
//   expect revision <next-edit-id>    check the next logical edit id
//   expect pending on|off             check for deferred logical edits
//   expect digest <hex>               final-digest assertion (checked at end)
//
// <block> is a SINGLE token: a TextureType name with underscores
// (case-insensitive, e.g. Stone / OAK_LEAVES / oak_leaves), a numeric
// ordinal, or "air". Spaces are not token separators inside a name, so use
// the underscore form ("Oak Leaves" must be written OAK_LEAVES).
// <item> is a SINGLE token: an items::ItemId registry name with underscores
// (case-insensitive, e.g. Stone / COBBLESTONE / oak_log) or a numeric registry
// ordinal (issue #230). "empty" is only valid in `expect inventory`.
// Cell coordinates are integers; edits target the cell center.
// Auto-pickup is a tick rule, exercised via `wait` (break -> wait -> pickup).
// =============================================================================

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <Items/ItemTypes.hpp>
#include <Sim/PlayerId.hpp>
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
	Give,
	Select,
	Drop,
	Break,
	PlaceSelected,
	Interest,
	InterestFollow,
	InterestRadius,
	CreatePlayer,
	SelectPlayer,
	RemovePlayer,
	Save,
	ExpectVoxel,
	ExpectInventory,
	ExpectItems,
	ExpectItem,
	ExpectResult,
	ExpectRevision,
	ExpectPending,
	ExpectDigest
};

/// Sentinel for `expect voxel ... unknown`: the cell must be UNAVAILABLE
/// (unloaded / not generated). Any other expected value is a TextureType
/// ordinal (AIR included — it is a real stored type).
inline constexpr int kExpectUnknown = -2;

struct ScriptAction
{
	ScriptOp op{ScriptOp::Wait};
	std::string name;		   // normalized action name for traces
	int frames{0};			   // Wait
	glm::dvec2 move{0.0};	   // Move
	bool enabled{false};	   // Sprint / Flight
	int swimDirection{0};	   // Swim: -1 down, 0 off, +1 up
	glm::dvec3 position{0.0};  // Teleport / Interest
	glm::ivec3 cell{0};		   // Place / Delete / Break / PlaceSelected / ExpectVoxel
	TextureType block{AIR};	   // Place
	items::ItemId item{items::ItemId::None}; // Give / ExpectInventory
	int count{0};			   // Give / Drop / ExpectInventory
	int slot{0};			   // Select / ExpectInventory
	bool expectEmpty{false};   // ExpectInventory: slot must be empty (count 0)
	int expectedItems{0};	   // ExpectItems: live entity count
	int expectedVoxel{0};	   // ExpectVoxel: ordinal or kExpectUnknown
	sim::PlayerId player;	   // SelectPlayer / RemovePlayer
	int radiusChunks{0};	   // InterestRadius
	uint64_t expectedNumber{0}; // ExpectItem id / ExpectRevision
	std::string expectedResult;
	std::string expectedDigest; // ExpectDigest: lowercase hex, no 0x
	int line{0};			   // source line (diagnostics)
};

struct SimScript
{
	std::vector<ScriptAction> actions;
	bool multiplayer{false};
	// 64-bit: each `wait` is capped at kMaxWaitFrames, but a long script's
	// SUM must not overflow before the runner's > --ticks check (issue #210
	// review).
	int64_t totalWaitFrames{0};
};

/// Parse a script file. Returns false and fills `error` (with the offending
/// line number) on any I/O or grammar problem. Bounded: at most
/// kMaxScriptActions actions, each `wait` at most kMaxWaitFrames frames.
bool parseSimScript(const std::filesystem::path &path, SimScript &out, std::string &error);

/// Case-insensitive block-name lookup: enum-style identifiers / display
/// names (spaces are normalized to underscores), numeric ordinals, "air".
/// NOTE: the script tokenizer splits on whitespace, so a multi-word display
/// name must be written as a single underscore token (OAK_LEAVES) in scripts.
/// Returns false when the token names no block.
bool parseBlockName(const std::string &token, TextureType &out);

inline constexpr int kMaxScriptActions = 100000;
inline constexpr int kMaxWaitFrames = 1000000;

} // namespace headless
