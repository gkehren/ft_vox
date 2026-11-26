#pragma once

#include <Engine/PlayerUiPreferences.hpp>

#include <optional>
#include <string>
#include <variant>

namespace gameui
{
struct SetVSync { bool enabled; };
struct SetFxaa { bool enabled; };
struct SetSensitivity { int percent; };
struct SetInvertY { bool enabled; };
struct PreviewDisplay
{
	std::optional<WindowSize> window;
	std::optional<ScalePolicy> scale;
};
enum class SettingsAction { Keep, Revert, Reset, RetrySave };
using SettingsRequest = std::variant<PreviewDisplay, SetVSync, SetFxaa,
	SetSensitivity, SetInvertY, SettingsAction>;

enum class DisplayPhase { Applying, Confirming, Restoring };

struct DisplayView
{
	DisplayPhase phase{};
	std::optional<WindowSize> window;
	std::optional<ScalePolicy> scale;
	int seconds{};
};

struct SettingsView
{
	WindowSize window{};
	ScalePolicy scale{};
	bool canResize{};
	bool vsync{true};
	std::optional<bool> pendingVSync;
	bool vsyncOverridden{};
	bool fxaa{true};
	bool fxaaOverridden{};
	int sensitivity{kDefaultSensitivity};
	bool invertY{};
	bool saveFailed{};
	std::optional<DisplayView> display;
	std::string notice;
};
} // namespace gameui
