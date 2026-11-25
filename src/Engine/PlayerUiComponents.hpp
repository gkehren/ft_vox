#pragma once

// Reusable player (Game UI) components — issue #302, Epic #298 Phase D.
//
// Every visual the player interface needs, painted from the PlayerUiStyle
// tokens and nothing else: card/panel, section header, button variants, list
// row, form field with validation, status indicator, progress/loading,
// notification toast, tooltip, confirmation dialog and the inventory slot /
// slot grid contract.
//
// Split of responsibilities
//   * Interaction (focus, press, drag, keyboard) stays in WidgetSet
//     (PlayerUiWidgets.hpp): one owner for input state.
//   * Presentation is here: geometry helpers are free functions (pure,
//     unit-tested), painting lives on `Surface` (one DrawList + one FontView +
//     one theme + one resolved player UI scale + one clip).
//   * Screens own layout and state; a component never queries the host, never
//     mutates gameplay state and never allocates.
//
// The inventory-slot contract is presentation-only on purpose (issue #302
// scope): an icon, a count and the visual states, plus OPTIONAL durability and
// progress indicators for future gameplay. No item metadata, no save or wire
// change, no authoritative read: the caller fills SlotView from a snapshot.

#include <Engine/PlayerUiStyle.hpp>
#include <Engine/PlayerUiText.hpp>
#include <Renderer/GameUiDrawList.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace gameui
{
// --- Inventory slot contract ----------------------------------------------

/// One slot's presentation. Everything here is derived from a snapshot; the
/// state flags map onto the shared tokens so a disabled/pending/selected slot
/// looks the same in the HUD, the inventory panel and the gallery.
struct SlotView
{
	AtlasIcon icon{};                // generation-tagged atlas layer; layer 0 = empty
	uint32_t count{};                // 0/1 paint no badge
	style::State state{};            // Selected/Disabled/Focused/Error/Pending
	std::string_view label{};        // tooltip/accessibility text, never painted inside
	std::optional<float> durability; // 0..1 — future tool metadata, presentation only
	std::optional<float> progress;   // 0..1 — pending command feedback

	bool empty() const { return icon.layer == 0; }
};

/// Grid geometry for a hotbar strip or an inventory grid. Cell edges derive
/// from the bounds so a screen can hand the whole area to the grid.
struct SlotGrid
{
	Clip bounds{};
	int columns{1};
	int rows{1};
	float cell{};
	float gap{};
};

SlotGrid slotGrid(Clip bounds, int columns, int rows, float gap);

/// Slot size that fills a row of `count` cells with `gap` inside `bounds`,
/// leaving the rest to the caller (the HUD hotbar centres the result).
float centeredSlotSize(Clip bounds, int count, float gap, float maxCell);

Clip slotCell(const SlotGrid &grid, int column, int row);

/// Strip of `count` cells of `cell` size, horizontally centred on `bounds`
/// and bottom-aligned to it (the HUD hotbar shape).
Clip centeredSlotStrip(Clip bounds, int count, float cell, float gap);

/// Row-major hit test. Returns -1 outside the grid.
int slotIndexAt(const SlotGrid &grid, glm::vec2 point);

/// Index of the slot at `point` inside a centred strip of `count` cells.
int centeredSlotIndexAt(Clip bounds, int count, float cell, float gap, glm::vec2 point);

/// "99+" style stack badge: no allocation, caller-owned buffer.
std::string_view formatCount(uint32_t count, char (&buffer)[8]);

// --- Tooltip ---------------------------------------------------------------

struct TooltipLayout
{
	Clip rect{};
	bool below{};
};

/// Bounded tooltip placement: prefers above-right of the pointer, flips side
/// and clamps so the box never leaves `bounds`.
TooltipLayout tooltipLayout(Clip bounds, glm::vec2 pointer, glm::vec2 extent, float gap);

/// Extent of a tooltip for a label at the current role/scale.
glm::vec2 tooltipExtent(FontView font, std::string_view text, const style::Theme &theme, float scale,
						float fontScale = 1.f);

// --- Notifications / toasts -----------------------------------------------

struct Toast
{
	std::string_view title{};
	std::string_view body{};
	style::Status status{style::Status::Info};
	float lifetime{4.f}; // seconds; <= 0 = sticky until dismissed
	float age{};
	uint32_t id{};
};

/// Bounded newest-first toast queue with deterministic expiry. Presentation
/// state only: the host pushes session/world notifications and the queue owns
/// what is on screen and when it leaves.
class ToastQueue
{
public:
	static constexpr size_t kCapacity = 4;

	/// Push a toast. Returns false and drops the oldest entry when full.
	bool push(Toast toast);
	/// Age the queue by `dt` seconds and retire expired entries.
	void advance(float dt);
	void clear();
	size_t size() const { return m_count; }
	const Toast *newest() const { return m_count ? &m_toasts[0] : nullptr; }
	const Toast *at(size_t index) const { return index < m_count ? &m_toasts[index] : nullptr; }
	bool dismiss(uint32_t id);

private:
	std::array<Toast, kCapacity> m_toasts{};
	size_t m_count{};
};

// --- Confirmation ----------------------------------------------------------

struct ConfirmationLayout
{
	Clip panel{};
	Clip title{};
	Clip body{};
	Clip cancel{};
	Clip confirm{};
};

/// Fixed-structure confirmation dialog: title, body, cancel left, primary
/// action right. `bounds` is the viewport; the panel is centred and clamped.
ConfirmationLayout confirmationLayout(Clip bounds, glm::vec2 preferred, bool destructive,
									  const style::Theme &theme, float scale);

/// Dialog convention: a destructive confirmation initially focuses Cancel so a
/// stray Enter cannot delete anything.
inline constexpr bool confirmationFocusesCancel(bool destructive)
{
	return destructive;
}

// --- Progress --------------------------------------------------------------

inline float progressFraction(float value)
{
	if (!(value > 0.f))
		return 0.f;
	return std::min(value, 1.f);
}

/// Bar inner geometry for a track rect and a border inset.
Clip progressFill(Clip track, float value);

// --- Form fields ------------------------------------------------------------

/// Authored height of a field group: caption label band, control well and —
/// when the field reports feedback — the message line under it. Callers size
/// the group with this helper so a message can never be painted over the well
/// border; `Surface::formField` drops a message that a shorter rect cannot hold.
float formFieldHeight(const style::Theme &theme, float scale, bool hasMessage = false);

// --- Painting surface ------------------------------------------------------

/// One screen's painter: DrawList + font + theme + resolved scale + clip. All
/// geometry tokens are multiplied by the scale here, so callers position with
/// 100% design units and never re-derive the DPI scale.
class Surface
{
public:
	/// `fontScale` maps glyph-table pixels back to logical units when the face is
	/// baked at device resolution (the ImGuiLayer::FontGlobalScale equivalent).
	Surface(DrawList &out, FontView font, const style::Theme &theme, float scale, Clip clip,
			uint32_t atlasLayers = 0, float fontScale = 1.f)
		: m_out(out), m_font(font), m_theme(theme), m_scale(scale > 0.f ? scale : 1.f), m_clip(clip),
		  m_atlasLayers(atlasLayers), m_fontScale(fontScale > 0.f ? fontScale : 1.f)
	{
	}

	float scale() const { return m_scale; }
	float fontScale() const { return m_fontScale; }
	/// Glyph-space multiplier for a role at the current player scale.
	float glyphScale(style::TextRole role) const
	{
		return style::typeScale(role) * m_scale * m_fontScale;
	}
	const style::Theme &theme() const { return m_theme; }
	FontView font() const { return m_font; }
	Clip clip() const { return m_clip; }
	uint32_t atlasLayers() const { return m_atlasLayers; }

	/// Token -> pixels.
	float px(float value) const { return value * m_scale; }

	// Containers
	void backdrop(Clip bounds, glm::vec4 color) const;
	void card(Clip rect, bool raised = false) const;
	Clip cardContent(Clip rect) const;
	void sectionHeader(Clip rect, std::string_view title) const;
	void divider(Clip rect) const;

	// Controls
	void button(Clip rect, std::string_view label, style::ButtonVariant variant, style::State state) const;
	void listRow(Clip rect, std::string_view label, std::string_view trailing, style::State state) const;
	/// Label band, sunk well and optional validation message inside `rect`.
	/// Size the rect with `formFieldHeight` so both fit; a rect that cannot hold
	/// the well plus its message drops the message rather than overlapping.
	void formField(Clip rect, std::string_view label, std::string_view value, style::Validation validation,
				   style::State state, std::string_view message = {}) const;
	void statusIndicator(Clip rect, style::Status status, std::string_view text) const;
	void progress(Clip rect, float value, bool pending, float phase) const;
	void toast(Clip rect, const Toast &toast) const;
	void tooltip(Clip bounds, glm::vec2 pointer, std::string_view text) const;
	/// Paints the dialog card, title and body only: the two actions are real
	/// controls the screen places at `layout.cancel` / `layout.confirm`, so the
	/// focus ring and activation come from the one input owner.
	void confirmation(const ConfirmationLayout &layout, std::string_view title,
					  std::string_view body) const;

	// Inventory
	void slot(Clip rect, const SlotView &slot) const;
	void slotGrid(const SlotGrid &grid, std::span<const SlotView> slots, int focused = -1) const;

	// HUD
	void crosshair(Clip viewport, glm::vec4 color) const;

	// Primitives (exposed so screens can compose without re-deriving tokens)
	void fill(Clip rect, glm::vec4 color, float radius) const;
	void ring(Clip rect, glm::vec4 color, float width, float radius) const;
	void frame(Clip rect, glm::vec4 face, glm::vec4 border, float width, float radius) const;

	// Text
	void label(Clip rect, std::string_view text, style::TextRole role, glm::vec4 color) const;
	void labelAligned(Clip rect, std::string_view text, style::TextRole role, glm::vec4 color,
					  float alignX) const;
	/// Label fitted into `rect` (ellipsis when needed).
	void fittedLabel(Clip rect, std::string_view text, style::TextRole role, glm::vec4 color,
					 float alignX) const;

private:
	/// Painted width of a fitted label including the ellipsis it needs.
	float fittedWidth(std::string_view text, style::TextRole role, float available, FittedText &fitted) const;

	DrawList &m_out;
	FontView m_font;
	const style::Theme &m_theme;
	float m_scale;
	Clip m_clip;
	uint32_t m_atlasLayers;
	float m_fontScale{1.f};
};
} // namespace gameui
