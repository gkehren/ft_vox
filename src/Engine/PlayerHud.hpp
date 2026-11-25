#pragma once

#include <Engine/PlayerUiComponents.hpp>
#include <Items/ItemTypes.hpp>

#include <array>
#include <span>
#include <string_view>

namespace gameui
{
struct HudHotbar
{
	std::array<SlotView, items::kHotbarSize> slots{};
	bool available{};
	bool canPlace{};
	std::string_view selectedName;
};

// Only this frame's snapshot contributes counts, selection or icons.
HudHotbar makeHudHotbar(std::span<const items::ItemStack> slots, int selected,
	uint64_t atlasGeneration, bool pending);

/// Session copy the HUD shows over the live scene (issue #303 F03/F07): a
/// one-line headline, a bounded detail and a separate recovery instruction.
/// Every string is host-owned, so the HUD never allocates and a temporary can
/// never dangle mid-frame. `status` also accepts an empty headline, which
/// leaves the whole feedback block unpainted.
struct HudView
{
	HudHotbar hotbar;
	std::string_view status, activity, recovery, creativeBlock;
	style::Status statusKind{style::Status::Info};
	bool aim{}, target{}, creative{}, hints{true}, captured{};
};

/// Body lines the HUD reserves for the status detail (bounded multiline).
inline constexpr int kHudDetailLines = 2;

/// Session health as the HUD understands it. The host maps its transport status
/// onto these four rows of the issue #303 F07 table, so the copy, the bar and
/// the reticle follow one policy instead of drifting per call site.
enum class HudSessionState : uint8_t
{
	Connected = 0, // synchronized and playable
	Degraded,      // playable, but snapshots or acknowledgments are late
	Pending,       // connecting, syncing terrain, resyncing or leaving
	Blocked,       // the session ended: timed out, rejected, disconnected
};

/// The copy and the surface a session observation resolves to.
struct HudSession
{
	std::string_view status, activity, recovery;
	style::Status kind{style::Status::Info};
	/// The confirmed private inventory may be presented. False while the
	/// session is not ready, so a resync never shows a stale bar.
	bool showHotbar{};
	/// Gameplay is synchronized: aiming is permitted.
	bool allowAim{};
	/// The session ended. The host fills `recovery` with the instruction that
	/// matches the current presentation state (`recoveryInstruction`), because
	/// only it knows whether the developer menu is reachable.
	bool blocked{};
};

/// Recovery instruction for a session that ended (issue #303 F03). It only names
/// actions that exist: the developer menu bar owns `Multiplayer > Reconnect`,
/// and reaching it needs the workspace visible and the mouse released. The
/// wording follows the current state, so a player is never told to toggle
/// something that is already in the right position.
std::string_view recoveryInstruction(bool workspaceVisible, bool mouseCaptured);

/// Session presentation policy (issue #303 F03/F07). `label` and `detail` come
/// from the host's transport status (stable storage); every other string is a
/// literal, so the result never points at a temporary. A degraded session that
/// is still ready keeps the normal HUD with one warning line; a degraded (or
/// connected) session that is not ready shows synchronization and hides both
/// the bar and the reticle.
HudSession hudSession(HudSessionState state, bool ready, std::string_view label,
	std::string_view detail);

/// Pure geometry: where each band WOULD go. Whether it is painted, and whether
/// the reticle survives a collision, is `resolveHud`'s job — an empty band never
/// blocks anything (issue #303 F02).
struct HudLayout
{
	Clip view;   // unobstructed gameplay rectangle: the bar + feedback live here
	Clip scene;  // full-window camera projection: the one reticle anchor
	Clip status, activity, recovery, hint, selection;
	Clip reticle; // aim geometry at the scene centre, content-independent
	SlotGrid hotbar;
	int detailLines{}; // 1..kHudDetailLines that actually fit
	float scale{};
};

/// `scene` is the full-window camera projection, `safeArea` the rectangle not
/// covered by developer docks. The hotbar and the feedback block use the safe
/// area; the reticle keeps the scene's centre and is NEVER recentred into the
/// safe area (an asymmetric dock must not move the aim point).
HudLayout hudLayout(Clip scene, Clip safeArea, const style::Theme &theme, float scale);

/// What a frame actually paints. The reticle outranks optional copy: a band that
/// would sit on the aim point is dropped, never the reticle. The mandatory
/// hotbar instead suppresses the reticle, because a bar over the aim point means
/// the world there is not aimable.
struct HudPaint
{
	bool status{}, activity{}, recovery{}, selection{}, hint{}, reticle{};
};

HudPaint resolveHud(const HudLayout &layout, const HudView &view);

/// True when the HUD has real session copy to show. The opt-in showcase strip
/// yields to it, so a connection error or a pending action never shares the
/// band above the HUD with a demonstration widget (issue #303 F05).
inline bool hudHasSessionFeedback(const HudView &view)
{
	return !view.status.empty() || !view.activity.empty() || !view.recovery.empty();
}

/// "Creative: Stone" — the creative block selection stays explicit and
/// distinguishable from the authoritative inventory selection, independent of
/// the optional hint line (issue #303 F06). Writes into `buffer`, no allocation.
std::string_view creativeLabel(std::string_view block, char (&buffer)[48]);

/// Paint the resolved content only: copy on one contrast plate, the reticle on
/// the scene's axis, then the hotbar and its selection.
void paintHud(const Surface &surface, const HudLayout &layout, const HudView &view);
} // namespace gameui
