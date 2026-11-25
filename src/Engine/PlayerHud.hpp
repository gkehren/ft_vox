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

struct HudView
{
	HudHotbar hotbar;
	std::string_view status, activity, creativeBlock;
	style::Status statusKind{style::Status::Info};
	bool aim{}, target{}, creative{}, hints{true}, captured{};
};

struct HudLayout
{
	Clip view, status, activity, hint, selection, reticle;
	SlotGrid hotbar;
	float scale{};
};

// The render projection spans scene. The developer dockspace can cover parts
// of it, so the bar uses safeArea and the reticle stays on the scene's axis.
HudLayout hudLayout(Clip scene, Clip safeArea, const style::Theme &theme, float scale);
void paintHud(const Surface &surface, const HudLayout &layout, Clip scene, const HudView &view);
} // namespace gameui
