#include <Engine/PlayerUiText.hpp>
#include <Engine/PlayerUiWidgets.hpp>
#include <Engine/UiWorkspace.hpp>

#include <cmath>

#include "TestMain.hpp"

using namespace gameui;

namespace
{
using enum InputOwner;
using enum InputKind;

void routing()
{
	const InputContext hud;
	const InputContext developer{.devKeyboard = true, .devPointer = true};
	const InputContext inventory{.nativeScreen = true, .inventory = true};
	const InputContext paused{.nativeScreen = true, .paused = true};
	const InputContext connected{.nativeScreen = true};
	const InputContext modal{.nativeScreen = true, .modal = true, .devKeyboard = true, .devPointer = true};
	const InputContext dialog{.nativeScreen = true, .devDialog = true};
	const InputContext unfocused{.interactive = false};
	const InputContext nativeTarget{.nativePointer = true};
	const InputContext coveredTarget{.devPointer = true, .nativePointer = true};
	const InputContext passiveCover{.devOccludes = true, .nativePointer = true};
	const InputContext devText{.nativeScreen = true, .devText = true};
	const InputContext loading{.devPointer = true, .transition = true};
	CHECK(loading.choose(InputKind::Key, false, true) == InputOwner::Gameplay,
		"Escape can open the player menu while waiting for a remote connection");
	CHECK(hud.choose(Key) == Gameplay && hud.choose(Pointer) == Gameplay && hud.choose(Text) == Blocked,
		"passive HUD leaves input to gameplay and ignores committed text");
	CHECK(developer.choose(Key) == DeveloperUi && developer.choose(Pointer) == DeveloperUi &&
		developer.choose(Text) == DeveloperUi && developer.choose(Key, true) == Global,
		"developer widgets capture actions while explicit globals bypass text");
	CHECK(nativeTarget.choose(Pointer) == PlayerUi && coveredTarget.choose(Pointer) == DeveloperUi,
		"visible HUD control has a pointer target only outside developer occlusion");
	CHECK(passiveCover.choose(Pointer) == Blocked && passiveCover.choose(Key) == Gameplay &&
		passiveCover.choose(Text) == Blocked, "an occluding passive overlay claims no keyboard or text");
	CHECK(devText.choose(Text) == DeveloperUi && devText.choose(Key, false, true) == DeveloperUi,
		"active developer text takes one keyboard stream while a native nonmodal screen is visible");
	CHECK(inventory.choose(Key) == PlayerUi && inventory.choose(Key, false, true) == PlayerUi &&
		inventory.choose(Pointer) == PlayerUi && !inventory.allowsMovement(),
		"native inventory owns pointer and keyboard and blocks movement");
	// PR #316 F01: explicit Developer UI keyboard ownership binds an open
	// inventory. Navigation keys reach only the owning interface, and the
	// policy hands everything — back keys included — to the developer layer.
	const InputContext inventoryFocus{.nativeScreen = true, .inventory = true, .devKeyboard = true};
	CHECK(inventoryFocus.choose(Key) == DeveloperUi && inventoryFocus.choose(Key, false, true) == DeveloperUi &&
		inventoryFocus.choose(Key, true) == Global,
		"developer keyboard ownership wins over an open inventory without touching globals");
	CHECK(inventoryFocus.choose(Pointer) == PlayerUi && !inventoryFocus.allowsMovement(),
		"developer keyboard ownership leaves the native pointer surface to the inventory");
	// After the player clicks a slot the host neutralizes the retained passive
	// focus and the same keys return to the native screen.
	const InputContext inventoryRestored{.nativeScreen = true, .inventory = true};
	CHECK(inventoryRestored.choose(Key) == PlayerUi && inventoryRestored.choose(Key, false, true) == PlayerUi,
		"with developer ownership released, native navigation returns to the inventory");
	const InputContext hudFocus{.devKeyboard = true};
	CHECK(hudFocus.choose(Key) == DeveloperUi && hudFocus.choose(Key, false, false, true) == DeveloperUi,
		"developer keyboard ownership answers the inventory shortcut too");
	CHECK(paused.choose(Key) == PlayerUi && connected.choose(Pointer) == PlayerUi &&
		!paused.allowsMovement() && !connected.allowsMovement(), "both menus stop local gameplay input");
	CHECK(modal.choose(Key, true) == PlayerUi && modal.choose(Text) == PlayerUi &&
		modal.choose(Pointer) == PlayerUi, "modal shields both underlying interfaces and view shortcuts");
	CHECK(dialog.choose(Pointer) == DeveloperUi && dialog.choose(Key, false, true) == DeveloperUi &&
		!dialog.allowsMovement(), "existing file dialog retains priority outside a native modal");
	CHECK(unfocused.choose(Key, true) == Blocked && unfocused.choose(Pointer) == Blocked &&
		!unfocused.allowsMovement(), "unfocused windows reject new actions");
	CHECK(loading.choose(Key) == Blocked && loading.choose(Pointer) == DeveloperUi &&
		!loading.allowsMovement(), "session transition leaves developer recovery controls while blocking gameplay");

	InputRouter router;
	CHECK(router.button(1, true, PlayerUi) == PlayerUi &&
		router.button(1, true, Gameplay) == Blocked &&
		router.button(1, false, DeveloperUi) == PlayerUi, "mouse press and release use one owner");
	CHECK(router.button(1, true, DeveloperUi) == DeveloperUi &&
		router.button(1, false, PlayerUi) == DeveloperUi, "developer gesture stays paired across the viewport");
	CHECK(router.button(1, false, Gameplay) == Blocked, "orphan release does not reach gameplay");
	CHECK(router.key(4, true, false, Gameplay) == Gameplay && router.gameplayKey(4),
		"polling requires a gameplay-owned key");
	router.cancelGameplay();
	CHECK(!router.gameplayKey(4) && router.key(4, true, true, Gameplay) == Blocked &&
		router.key(4, false, false, PlayerUi) == Blocked, "capture change cancels held movement until release");
	CHECK(router.key(4, true, false, Gameplay) == Gameplay, "fresh movement press after release");
	CHECK(router.key(5, true, false, Global) == Global && router.key(5, true, true, Global) == Blocked,
		"global shortcut fires once");
	CHECK(router.key(6, true, false, PlayerUi) == PlayerUi &&
		router.key(6, true, true, PlayerUi) == PlayerUi &&
		router.key(6, true, true, DeveloperUi) == Blocked, "editor repeat stays with its owner");
	router.cancelAll();
	router.reconcileKey(4, true);
	router.reconcileKey(6, false);
	CHECK(!router.gameplayKey(4) && router.key(6, true, false, Gameplay) == Gameplay,
		"focus regain reconciles an omitted release and quarantines held input");
}

void sessionLifecycle()
{
	ScreenStack stack;
	stack.reset(Screen::MainMenu);
	const auto context = [&](bool connected, bool interactive = true) {
		return InputContext{.interactive = interactive, .nativeScreen = stack.nativeScreen(),
			.modal = stack.suppressDeveloper(), .paused = stack.pausesWorld(connected)};
	};
	const auto workspaceAvailable = [&] {
		return ui::developerUiAvailable(true, stack.suppressDeveloper(), ui::Workspace::Visible);
	};
	CHECK(stack.push(Screen::Worlds) && stack.push(Screen::CreateWorld) && stack.push(Screen::Loading),
		"new game navigation reaches the loading screen");
	CHECK(stack.pausesWorld(false) && !workspaceAvailable() && !context(false).allowsMovement(),
		"loading suspends solo play and owns the viewport");
	CHECK(stack.push(Screen::SessionError) && context(false).choose(Key, false, true) == PlayerUi &&
		!workspaceAvailable(), "a failed open gives its recovery screen the Back key");
	stack.reset();

	InputRouter router;
	constexpr size_t movementKey = 26;
	CHECK(router.key(movementKey, true, false, context(false).choose(Key)) == Gameplay,
		"a fresh world accepts movement");
	CHECK(stack.push(Screen::Pause) && stack.push(Screen::Settings) && !workspaceAvailable() &&
		stack.pausesWorld(false) && !stack.pausesWorld(true),
		"settings over Pause suspends solo simulation while a remote world stays live");
	router.cancelGameplay();
	CHECK(stack.back() && workspaceAvailable() && stack.back(), "Back restores Pause and then the HUD");
	CHECK(!router.gameplayKey(movementKey) &&
		router.key(movementKey, false, false, context(false).choose(Key)) == Blocked,
		"returning to the HUD does not replay a held movement key");

	CHECK(stack.push(Screen::Pause) && stack.push(Screen::Multiplayer) &&
		stack.push(Screen::MultiplayerAdmin), "multiplayer administration has its own route from Pause");
	stack.focus(42);
	CHECK(stack.push(Screen::MultiplayerConfirm) && !workspaceAvailable() &&
		context(true).choose(Pointer) == PlayerUi &&
		context(true, false).choose(Pointer) == Blocked,
		"focused confirmation owns input, and focus loss blocks it");
	CHECK(stack.back() && stack.focus() == 42, "cancelling confirmation restores the administration focus");
	stack.reset();
	CHECK(stack.top() == Screen::Hud && stack.focus() == 0 && !stack.contains(Screen::MultiplayerAdmin) &&
		context(false).allowsMovement(), "returning to solo starts with a clean HUD stack");
}

// Workspace visibility and input ownership (issue #303 F01/F04).
void workspaceVisibility()
{
	using enum ui::Workspace;
	CHECK(ui::toggled(Visible) == Hidden && ui::toggled(Hidden) == Visible,
		"the workspace toggle is reversible without restarting the application");

	// Hidden workspace, inventory closed: no developer surface owns input, so
	// gameplay keeps the pointer, the keyboard and movement.
	const bool hidden = ui::developerUiAvailable(true, false, Hidden);
	CHECK(!hidden, "a hidden workspace has no developer input");
	const InputContext gameplay{.devKeyboard = hidden, .devPointer = hidden};
	CHECK(gameplay.choose(Key) == Gameplay && gameplay.choose(Pointer) == Gameplay &&
		gameplay.allowsMovement(), "hidden workspace leaves gameplay fully in charge");

	const InputContext inventory{.nativeScreen = true, .inventory = true, .devKeyboard = hidden,
		.devPointer = hidden};
	CHECK(inventory.choose(Key, false, true) == PlayerUi &&
		inventory.choose(Key) == PlayerUi && !inventory.allowsMovement(),
		"inventory stays interactive with the workspace hidden");

	// Visible workspace: developer widgets own input; explicit globals bypass the
	// text field and still reach the host (the panel shortcuts restore it).
	const bool shown = ui::developerUiAvailable(true, false, Visible);
	CHECK(shown, "a visible workspace can own developer input");
	const InputContext developer{.devKeyboard = shown, .devPointer = shown};
	CHECK(developer.choose(Key) == DeveloperUi && developer.choose(Key, true) == Global &&
		!developer.allowsMovement(),
		"the workspace owns keys, while explicit globals still reach the host");

	// A native player screen (pause menu, loading, exit confirmation) outranks
	// visibility: it removes the developer layer entirely.
	CHECK(!ui::developerUiAvailable(true, true, Visible),
		"a native player screen removes developer input even with the workspace visible");

	const InputContext waiting{.nativeScreen = true, .modal = true, .pointerGeometryPending = true};
	CHECK(waiting.choose(Pointer) == Blocked && waiting.choose(Key, false, true) == PlayerUi,
		"geometry refresh blocks pointers while native modal keyboard routing stays live");
	InputRouter transition;
	CHECK(transition.button(1, true, waiting.choose(Pointer)) == Blocked &&
		transition.button(1, false, Gameplay) == Blocked &&
		transition.button(1, true, Gameplay) == Gameplay,
		"a press during refresh keeps its blocked release after composition resumes");

	// Hiding ends developer-owned input: a held developer key is quarantined and
	// still has to be released, so nothing leaks back into the hidden state.
	CHECK(ui::endsDeveloperInput(Visible, Hidden) && !ui::endsDeveloperInput(Hidden, Visible) &&
		!ui::endsDeveloperInput(Visible, Visible),
		"only hiding the workspace ends developer input");
	InputRouter router;
	CHECK(router.key(7, true, false, DeveloperUi) == DeveloperUi, "a developer key is held");
	router.cancelDeveloper();
	CHECK(router.key(7, true, true, DeveloperUi) == Blocked &&
		router.key(7, false, false, DeveloperUi) == Blocked,
		"hiding the workspace cancels the press and swallows its orphan release");
}

// Issue #303 F03/F12: the HUD's multiplayer recovery line tells the player to
// release the captured mouse with C before Multiplayer > Reconnect, so C (and
// Escape) must survive a session transition while every other gameplay command
// stays blocked.
void transitionRecovery()
{
	using enum InputOwner;
	const InputContext blocked{.transition = true};
	CHECK(blocked.choose(Key, false, false, false, true) == Gameplay,
		"C reaches the host during a session transition so the captured mouse can be freed");
	CHECK(blocked.choose(Key, false, true) == Gameplay,
		"Escape keeps opening the player menu during a session transition");
	CHECK(blocked.choose(Key) == Blocked && blocked.choose(Pointer) == Blocked &&
			blocked.choose(Text) == Blocked && !blocked.allowsMovement(),
		"movement, gameplay keys, pointer and text stay blocked during a session transition");
	CHECK(blocked.choose(Key, false, false, true) == Gameplay,
		"E can open the disabled inventory during a session transition");

	// The exception sits after every other ownership rule, so a benchmark lock, a
	// native screen, a modal, a developer field and the inventory all keep their
	// own policy for the same key.
	const InputContext benchmark{.locked = true, .transition = true};
	CHECK(benchmark.choose(Key, false, true) == Blocked &&
			benchmark.choose(Key, false, false, false, true) == Blocked,
		"a locked benchmark keeps both shortcuts out of gameplay");
	const InputContext native{.nativeScreen = true, .transition = true};
	CHECK(native.choose(Key, false, true) == PlayerUi &&
			native.choose(Key, false, false, false, true) == PlayerUi,
		"a native screen keeps the shortcuts under its own input policy");
	const InputContext modal{.modal = true, .transition = true};
	CHECK(modal.choose(Key, false, false, false, true) == PlayerUi,
		"a modal keeps the cursor shortcut under the native policy");
	const InputContext devText{.devKeyboard = true, .devText = true, .transition = true};
	CHECK(devText.choose(Key, false, false, false, true) == DeveloperUi,
		"a developer text field keeps C as typed text");
	const InputContext devKeyboard{.devKeyboard = true, .transition = true};
	CHECK(devKeyboard.choose(Key, false, false, false, true) == DeveloperUi &&
			devKeyboard.choose(Key, false, true) == DeveloperUi,
		"developer keyboard ownership outranks both transition exceptions");
	const InputContext dialog{.devDialog = true, .transition = true};
	CHECK(dialog.choose(Key, false, false, false, true) == DeveloperUi,
		"an open developer dialog outranks the transition exception");
	const InputContext inventory{.nativeScreen = true, .inventory = true, .transition = true};
	CHECK(inventory.choose(Key, false, false, false, true) == PlayerUi &&
			inventory.choose(Key, false, false, true) == PlayerUi,
		"an open inventory keeps its own policy for C and for its close key");

	// Outside a transition the shortcut is unchanged, including the capture retry
	// the failure flag already allowed.
	const InputContext hud;
	CHECK(hud.choose(Key, false, false, false, true) == Gameplay &&
			InputContext{.captureFailed = true}.choose(Key, false, false, false, true) == Gameplay,
		"C stays a gameplay shortcut in normal play and after a failed capture");
}

void screens()
{	ScreenStack stack;
	CHECK(stack.top() == Screen::Hud && !stack.back() && !stack.blocksGameplay(), "HUD is passive root");
	CHECK(stack.push(Screen::Inventory) && stack.nativeScreen() && !stack.pausesWorld(false) && stack.back(),
		"inventory is dismissible and does not pause solo");
	CHECK(stack.push(Screen::Pause) && stack.pausesWorld(false) && !stack.pausesWorld(true),
		"same pause menu suspends solo only");
	stack.focus(2);
	CHECK(stack.push(Screen::Showcase) && stack.focus() == 0, "child has independent focus");
	stack.focus(8);
	CHECK(stack.push(Screen::ConfirmExit) && stack.suppressDeveloper() && stack.pausesWorld(false),
		"confirmation shields developer drawing and retains solo suspension");
	CHECK(stack.back() && stack.focus() == 8 && !stack.suppressDeveloper() &&
		stack.back() && stack.focus() == 2 && stack.back() && stack.top() == Screen::Hud,
		"back closes in stack order and restores opener focus");
	stack.reset(Screen::Loading);
	CHECK(stack.suppressDeveloper() && stack.blocksGameplay() && !stack.back(), "loading owns input");
	stack.reset(Screen::MainMenu);
	CHECK(!stack.pausesWorld(false) && stack.focus() == 0, "reset clears stale session focus");
}

void pointerGestures()
{
	InputRouter router;
	CHECK(router.button(3, true, DeveloperUi) == DeveloperUi &&
		router.button(1, true, PlayerUi) == Blocked &&
		router.activeButton(3) && !router.activeButton(1) &&
		router.motionOwner(Gameplay) == DeveloperUi && router.wheelOwner(PlayerUi) == Blocked,
		"first live owner wins independent of SDL button number");
	CHECK(router.button(1, false, Gameplay) == Blocked &&
		router.button(3, false, PlayerUi) == DeveloperUi && router.pointerOwner() == None,
		"cross-surface releases can arrive in reverse order");
	CHECK(router.button(1, true, PlayerUi) == PlayerUi &&
		router.button(3, true, PlayerUi) == PlayerUi &&
		router.button(1, false, DeveloperUi) == PlayerUi && router.motionOwner(DeveloperUi) == PlayerUi,
		"same-surface buttons share the gesture until the last release");
	router.cancelPointer(PlayerUi);
	CHECK(router.pointerOwner() == None && router.motionOwner(DeveloperUi) == DeveloperUi &&
		!router.activeButton(3) && router.button(2, true, DeveloperUi) == DeveloperUi &&
		router.motionOwner(PlayerUi) == DeveloperUi,
		"a quarantined button cannot mask or claim a fresh gesture");
	CHECK(router.button(3, false, Gameplay) == Blocked &&
		router.button(2, false, PlayerUi) == DeveloperUi && router.pointerOwner() == None,
		"late cancelled release cannot cross or disrupt the new owner");
	CHECK(router.button(1, true, PlayerUi) == PlayerUi, "fresh press after cancellation owns motion");
	router.cancelAll();
	CHECK(router.button(1, true, PlayerUi) == Blocked && router.button(1, false, PlayerUi) == Blocked,
		"cancelled button must return up before accepting a new down");
}

void layoutAndWidgets()
{
	const Clip scene{{240, 50, 1680, 950}};
	const Clip panel = anchor(scene, {600, 1200}, {0.5f, 0.5f});
	CHECK((panel.rect == glm::vec4{660, 50, 1260, 950}), "anchor measures inside the scene viewport");
	const Clip rows{{0, 0, 120, 200}};
	StackLayout column(rows, true, 4.f);
	CHECK((column.next(40).rect == glm::vec4{0, 0, 120, 40}) &&
		(column.next(300).rect == glm::vec4{0, 44, 120, 200}) && column.next(2).empty(),
		"column applies spacing and bounds");
	const Clip cell = gridCell({{10, 20, 110, 100}}, 2, 2, 1, 1, 4);
	CHECK((cell.rect == glm::vec4{62, 62, 110, 100}), "grid reserves gutters on both axes");
	const auto scale = framebufferScale({800, 600}, {1200, 1200});
	CHECK((pixelClip({{1.2f, 1.2f, 801.f, 5.8f}}, scale, {1200, 1200}).rect == glm::vec4{2, 3, 1200, 11}),
		"fractional and asymmetric DPI use bounded inward rounding");
	CHECK(hit({{0, 0, 10, 10}}, {0, 0}) && !hit({{0, 0, 10, 10}}, {10, 10}), "hit clipping is half-open");

	ControlId focus{};
	WidgetSet set;
	set.reset({{0, 0, 120, 80}}, 200, 0);
	set.add({1, {{0, 0, 120, 30}}, "first", {}, Button{}});
	set.add({2, {{0, 40, 120, 70}}, "disabled", {}, Button{}, false});
	set.add({3, {{0, 150, 120, 180}}, "below viewport", {}, Button{}});
	CHECK(!set.at({5, 151}) && set.step(1, false)->id == 3 && set.step(1, true)->id == 3,
		"pointer clips hidden controls, keyboard skips disabled controls");
	set.press({5, 45}, focus, {}, 1.f);
	CHECK(set.release({5, 45}, 1.f).action == WidgetAction::None && !focus, "disabled control does not press or focus");
	set.press({5, 5}, focus, {}, 1.f);
	CHECK(set.release({121, 5}, 1.f).action == WidgetAction::None, "release outside viewport does not activate");
	set.press({5, 5}, focus, {}, 1.f);
	set.cancelPress();
	CHECK(set.release({5, 5}, 1.f).action == WidgetAction::None, "cancelled press cannot be replayed");
	set.focus(focus, 3, true);
	CHECK(set.viewport().offset == 100 && set.at({5, 65})->id == 3, "scroll reveal makes keyboard target visible");
	set.press({5, 65}, focus, {}, 1.f);
	CHECK(set.release({5, 65}, 1.f).id == 3 && focus == 3, "painted hit and release use the same geometry");
	set.scroll({5, 65}, -500.f, 1.f, focus);
	CHECK(set.viewport().offset == 120.f, "scrolling clamps to content bounds");
	ListSelection list;
	list.scroll(99, 8, 3);
	CHECK(list.first == 5 && list.selected == 0, "list wheel is bounded and does not select");
	list.select(2, 8, 3);
	CHECK(list.first == 2 && list.selected == 2, "selection reveals its row");
	// Correction F11: the administrative roster decouples the selection from
	// the scroll window — wheel browsing works without a selection, the
	// offset survives unchanged views, shrinkage clamps it, and the explicit
	// unselected state never implicitly picks a row.
	ListSelection roster;
	roster.clear();
	CHECK(!roster.any() && roster.selected == -1, "the explicit unselected state is no row");
	roster.scroll(3, 10, 4);
	CHECK(roster.first == 3 && !roster.any(), "the wheel scrolls without a selection");
	roster.select(8, 10, 4);
	CHECK(roster.selected == 8 && roster.first == 5, "an explicit selection reveals its row");
	roster.scroll(-2, 10, 4);
	CHECK(roster.first == 3 && roster.selected == 8, "the offset moves freely around a selection");
	roster.scroll(0, 6, 4);
	CHECK(roster.first == 2, "roster shrinkage clamps the scroll offset");
	roster.clear();
	CHECK(!roster.any() && roster.first == 2, "clearing the selection preserves the scroll window");
	roster.select(roster.selected + 1, 10, 4);
	CHECK(roster.selected == 0 && roster.first == 0, "keyboard navigation starts from the first row and reveals it");
	// Correction F17: bounds reconciliation keeps the wheel offset — the
	// shared list helper no longer reveals the selected row every frame, and
	// select() stays reserved for explicit interaction.
	ListSelection shared;
	shared.select(4, 6, 5);
	shared.scroll(1, 6, 5);
	CHECK(shared.first == 1 && shared.selected == 4, "the wheel browses beyond the selected row");
	shared.reconcile(6, 5);
	CHECK(shared.first == 1 && shared.selected == 4, "reconciliation preserves the offset and selection");
	shared.reconcile(3, 5);
	CHECK(shared.first == 0 && shared.selected == 2, "shrinkage clamps the offset and the selection");
	shared.reconcile(0, 5);
	CHECK(shared.first == 0 && shared.selected == 0, "an empty collection is safe");
	ListSelection fresh;
	fresh.reconcile(4, 3);
	CHECK(fresh.selected == 0 && fresh.first == 0, "a fresh list keeps its default first-row selection");
	ListSelection unselected;
	unselected.clear();
	unselected.reconcile(4, 3);
	CHECK(!unselected.any(), "the explicit unselected state survives reconciliation");
}

void reusableBindings()
{
	TextField firstField, secondField;
	Toggle firstToggle, secondToggle;
	Slider firstSlider{0.5f}, secondSlider{0.25f};
	constexpr std::array<std::string_view, 3> items{"a", "b", "c"};
	ListBox firstList{items, 1}, secondList{items, 2};
	WidgetSet set;
	ControlId focus{};
	set.reset({{0, 0, 240, 200}}, 200, 0);
	set.add({1, {{0, 0, 100, 36}}, "first field", {}, &firstField});
	set.add({2, {{120, 0, 220, 36}}, "second field", {}, &secondField});
	set.add({3, {{0, 40, 100, 72}}, "first slider", {}, &firstSlider});
	set.add({4, {{120, 40, 220, 72}}, "second slider", {}, &secondSlider});
	set.add({5, {{0, 76, 100, 104}}, "first toggle", {}, &firstToggle});
	set.add({6, {{120, 76, 220, 104}}, "second toggle", {}, &secondToggle});
	set.add({7, {{0, 108, 100, 188}}, "first list", {}, &firstList});
	set.add({8, {{120, 108, 220, 188}}, "second list", {}, &secondList});

	set.focus(focus, 1, false);
	set.editor(focus)->insert("left");
	set.editor(focus)->preedit("pending", 2, 3);
	set.focus(focus, 2, false);
	set.editor(focus)->insert("right");
	CHECK(firstField.value() == "left" && firstField.composition().empty() && secondField.value() == "right",
		"independent fields and composition cleared on editor switch");
	secondField.preedit("pending");
	set.setEnabled(2, false, focus);
	CHECK(!focus && !set.editor(2) && secondField.composition().empty() && secondField.value() == "right",
		"disabling a focused editor clears preedit and eligibility immediately");
	set.setEnabled(2, true, focus);
	CHECK(!focus && set.key(focus, NavKey::Enter, false, false, false).action == WidgetAction::None,
		"re-enabling does not restore focus or replay input");

	// F13: pointer -> value uses the painted track, so the visible ends are 0 and
	// 1 and the midpoints land on the same values as the keyboard steps.
	const auto slider = sliderGeometry({{0, 40, 100, 72}}, style::baseline(), 1.f);
	const auto sliderX = [&](float fraction) {
		return slider.track.rect.x + size(slider.track).x * fraction;
	};
	set.press({sliderX(0.5f), 50}, focus, {}, 1.f);
	set.drag({sliderX(1.f), 50}, 1.f);
	CHECK(firstSlider.value == 1.f && secondSlider.value == 0.25f, "pointer drag mutates only its bound slider");
	set.setEnabled(3, false, focus);
	CHECK(set.drag({sliderX(0.f), 50}, 1.f).action == WidgetAction::None && firstSlider.value == 1.f,
		"disabled slider cancels active drag");
	set.setEnabled(3, true, focus);
	CHECK(set.release({sliderX(0.f), 50}, 1.f).action == WidgetAction::None &&
		set.drag({sliderX(0.f), 50}, 1.f).action == WidgetAction::None,
		"cancelled drag cannot resume when re-enabled");
	CHECK(set.press({sliderX(0.25f), 50}, focus, {}, 1.f).action == WidgetAction::Change &&
		std::abs(firstSlider.value - 0.25f) < 0.001f,
		"a click at 25% of the visible track selects a quarter");
	CHECK(set.drag({sliderX(0.75f), 50}, 1.f).action == WidgetAction::Change &&
		std::abs(firstSlider.value - 0.75f) < 0.001f,
		"dragging to 75% of the visible track selects three quarters");
	set.release({sliderX(0.75f), 50}, 1.f);
	set.focus(focus, 4, false);
	CHECK(set.key(focus, NavKey::Right, false, false, false).id == 4 && secondSlider.value == 0.30f,
		"second slider has independent keyboard behavior and change action");

	set.press({150, 85}, focus, {}, 1.f);
	set.setEnabled(6, false, focus);
	set.setEnabled(6, true, focus);
	CHECK(set.release({150, 85}, 1.f).action == WidgetAction::None && !secondToggle.value,
		"disabling an armed toggle prevents even a later eligible release");
	set.focus(focus, 5, false);
	CHECK(set.key(focus, NavKey::Space, false, false, false).id == 5 && firstToggle.value && !secondToggle.value,
		"toggle updates its binding and emits the composing screen's control ID");
	set.scroll({50, 145}, -1.f, 1.f, focus);
	set.focus(focus, 8, false);
	set.key(focus, NavKey::Down, false, false, false);
	CHECK(firstList.selection.first == 1 && firstList.selection.selected == 0 &&
		secondList.selection.first == 0 && secondList.selection.selected == 1, "list selection and scroll are independent");

	set.focus(focus, 1, false);
	firstField.preedit("will be clipped");
	set.reset({{0, 85, 240, 200}}, 200, 0);
	set.add({1, {{0, 0, 100, 36}}, "first field", {}, &firstField});
	set.revalidate(focus);
	CHECK(!focus && !set.editor(1) && firstField.composition().empty(),
		"recomposition removes eligibility for fully clipped fields");
}

void textMetrics()
{
	// F01: one metric for painting, the caret, hit testing and the IME anchor. At
	// every supported player scale and framebuffer density, the caret sits on a
	// character boundary and a click on a character selects that boundary.
	std::array<Glyph, 128> glyphs{};
	for (auto &glyph : glyphs)
	{
		glyph.advance = 6.f;
		glyph.visible = true;
	}
	const FontView font{glyphs};
	const std::string value = "abcdef";
	for (const float scale : {1.f, 1.5f, 2.f})
		for (const float fontScale : {1.f, 0.5f})
		{
			TextField field;
			field.insert(value);
			WidgetSet set;
			ControlId focus{};
			set.reset({{0.f, 0.f, 600.f, 600.f}}, 600.f, 0.f);
			set.add({1, {{0.f, 0.f, 400.f * scale, 46.f * scale}}, "value", {}, &field});
			set.focus(focus, 1, false);
			const auto metric = textStyle(style::TextRole::Body, scale, fontScale);
			const auto geometry = set.editorGeometry(focus, font, scale, fontScale);
			CHECK(geometry.glyphScale == metric.glyphScale() && geometry.lineHeight == metric.lineHeight(),
				"the editor geometry reports the one text metric");
			const float advance = font.measure("a", metric.glyphScale());
			CHECK(std::abs(geometry.caretX - (geometry.origin.x + advance * float(value.size()))) < 0.01f,
				"the caret x is the painted advance of every character before it");
			const float click = geometry.origin.x + advance * 2.5f;
			set.press({click, geometry.origin.y + 1.f}, focus, font, scale, false, fontScale);
			CHECK(field.caret() == 3,
				"a click at a character centre selects exactly that character boundary");
			set.press({geometry.origin.x - 5.f, geometry.origin.y + 1.f}, focus, font, scale, false,
				fontScale);
			CHECK(field.caret() == 0, "a click left of the text selects the first boundary");
		}
}

void textInput()
{
	TextField field;
	field.insert("a\xc3\xa9\xf0\x9f\x8c\xb2");
	CHECK(field.value().size() == 7 && field.caret() == 7, "committed UTF-8 stored");
	field.move(NavKey::Left, true);
	CHECK(field.selection().size() == 4 && field.caret() == 3, "shift movement selects a complete scalar");
	field.erase(NavKey::Delete);
	CHECK(field.value() == "a\xc3\xa9", "delete removes the selection");
	field.erase(NavKey::Backspace);
	CHECK(field.value() == "a" && field.caret() == 1, "backspace removes one UTF-8 scalar");
	field.preedit("\xc3\xa9");
	CHECK(field.composition().size() == 2 && field.value() == "a", "IME preedit does not commit");
	field.insert("b\n\xc0\x80");
	CHECK(field.value() == "ab" && field.composition().empty(), "commit filters invalid and multiline input");
	field.selectAll();
	field.insert(std::string(TextField::kMaxBytes - 1, 'x') + "\xc3\xa9");
	CHECK(field.value().size() == TextField::kMaxBytes - 1, "bounded field never truncates a scalar");
	field.preedit("a\xc3\xa9\xf0\x9f\x8c\xb2", 1, 1);
	CHECK(field.compositionCursor() == 1 && field.compositionEnd() == 3,
		"SDL scalar range becomes a complete multibyte selection");
	field.preedit("a\xc3\xa9", INT32_MAX, INT32_MAX);
	CHECK(field.compositionCursor() == 3 && field.compositionEnd() == 3,
		"out-of-range composition offsets clamp safely");
	field.preedit("a\n\xc3\xa9", 2, 1);
	CHECK(field.compositionCursor() == 1 && field.compositionEnd() == 3,
		"filtered composition keeps offsets mapped to sanitized bytes");
	// F09: SDL does not normalize composition units — the Windows backend sends
	// UTF-16 code units, the Linux backends Unicode scalars. Both normalize to
	// one scalar range, and a supplementary character is never split.
	// U+1F332 twice, built numerically so no source encoding can interfere.
	std::string trees;
	for (int i = 0; i < 2; ++i)
	{
		trees.push_back(char(0xf0));
		trees.push_back(char(0x9f));
		trees.push_back(char(0x8c));
		trees.push_back(char(0xb2));
	}
	const auto utf16Range = compositionRange(trees, 1, 2, CompositionUnit::Utf16);
	const auto scalarRange = compositionRange(trees, 1, 2, CompositionUnit::Scalar);
	// Both ends round down, so the range covers exactly the scalars that lie
	// fully inside the requested UTF-16 range: units [1,3) -> scalar [0,1).
	CHECK(utf16Range.start == 0 && utf16Range.length == 1,
		"a UTF-16 range starting inside a surrogate pair rounds down to the pair");
	CHECK(scalarRange.start == 1 && scalarRange.length == 1,
		"the same numeric range on a scalar backend selects the second character");
	CHECK(compositionRange(trees, -1, -1, CompositionUnit::Utf16).start == -1 &&
			compositionRange(trees, -1, -1, CompositionUnit::Utf16).length == -1,
		"an unset composition range stays unset on both backends");
	CHECK(compositionRange(trees, 1, INT32_MAX, CompositionUnit::Utf16).length == 2,
		"an end-of-text composition range clamps instead of overflowing");
	TextField supplementary;
	supplementary.insert("x");
	supplementary.preedit(trees, 2, 2, CompositionUnit::Utf16);
	CHECK(supplementary.composition().size() == 8 && supplementary.compositionCursor() == 4 &&
			supplementary.compositionEnd() == 8,
		"a supplementary composition range maps to byte offsets in the sanitized preedit");

	std::array<Glyph, 128> glyphs{};
	for (auto &glyph : glyphs)
		glyph.advance = 8.f;
	const FontView font{glyphs};
	WidgetSet set;
	set.reset({{10, 0, 80, 40}}, 40, 0);
	set.add({1, {{0, 0, 90, 40}}, "long field", {}, &field});
	ControlId focus{};
	set.focus(focus, 1, false);
	const auto geometry = set.editorGeometry(focus, font, 1.f);
	CHECK(geometry.caretX == geometry.clip.rect.z - 1.f && geometry.origin.x < 0.f,
		"the long-field IME anchor uses the same horizontally scrolled geometry as painting");
}

void selectionAndSliderClipping()
{
	// R01: a scrolled editor still highlights its visible selection, and no
	// editor primitive escapes the field's content rect.
	std::array<Glyph, 128> glyphs{};
	for (auto &glyph : glyphs)
	{
		glyph.rect = {0.f, 0.f, 6.f, 10.f};
		glyph.advance = 6.f;
		glyph.visible = true;
	}
	const FontView font{glyphs};
	const auto &theme = style::baseline();
	const auto &p = theme.palette;
	const glm::vec4 selection{p.accent.r, p.accent.g, p.accent.b, 0.32f};
	const auto insideClip = [](glm::vec4 rect, Clip clip) {
		return rect.x >= clip.rect.x - 0.01f && rect.y >= clip.rect.y - 0.01f &&
			rect.z <= clip.rect.z + 0.01f && rect.w <= clip.rect.w + 0.01f;
	};
	for (const float scale : {1.f, 1.5f, 2.f})
		for (const float fontScale : {1.f, 0.5f})
		{
			TextField field;
			field.insert("abcdefghijklmnop");
			field.selectAll();
			WidgetSet set;
			ControlId focus{};
			set.reset({{0.f, 0.f, 600.f, 600.f}}, 600.f, 0.f);
			set.add({1, {{0.f, 0.f, 26.f * scale, 46.f * scale}}, "value", {}, &field});
			set.focus(focus, 1, false);
			DrawList out;
			out.reset({600.f, 600.f}, 4);
			set.paint(out, font, scale, focus, {-20.f, -20.f}, theme, 4, fontScale);
			const auto geometry = set.editorGeometry(focus, font, scale, fontScale);
			size_t highlights = 0, overflowing = 0, carets = 0;
			for (const auto &quad : out.quads())
			{
				if (quad.color == selection)
				{
					++highlights;
					overflowing += !insideClip(quad.rect, geometry.clip);
				}
				else if (quad.color == p.accent && quad.layer == -1)
					++carets;
			}
			CHECK(geometry.origin.x < geometry.clip.rect.x,
				"the value is horizontally scrolled at " << scale * 100.f << "% scale, density "
					<< fontScale);
			CHECK(highlights == 1 && overflowing == 0,
				"a scrolled selection stays highlighted and never overflows the editor at "
					<< scale * 100.f << "% scale, density " << fontScale);
			CHECK(carets == 1, "the caret is still painted for a scrolled selection");
		}

	// R04: the slider label and the percentage readout own disjoint rectangles,
	// so a long label elides instead of painting over the value.
	for (const float scale : {1.f, 2.f})
		for (const float width : {240.f, 80.f, 12.f})
		{
			const auto geometry = sliderGeometry({{0.f, 0.f, width * scale, 60.f * scale}}, theme, scale);
			CHECK(geometry.label.rect.z <= geometry.value.rect.x,
				"the slider label rect ends before the value rect begins at " << width * scale
					<< " px");
			CHECK(geometry.label.rect.x >= -0.01f && geometry.value.rect.z <= width * scale + 0.01f,
				"the label and value rects stay inside the slider bounds");
		}
	{
		Slider slider{0.5f};
		WidgetSet set;
		set.reset({{0.f, 0.f, 400.f, 120.f}}, 120.f, 0.f);
		set.add({1, {{0.f, 0.f, 200.f, 40.f}}, "An extremely long slider label", {}, &slider});
		DrawList out;
		out.reset({400.f, 120.f}, 4);
		set.paint(out, font, 1.f, {}, {-20.f, -20.f}, theme);
		const auto geometry = sliderGeometry({{0.f, 0.f, 200.f, 40.f}}, theme, 1.f);
		size_t labelGlyphs = 0, straddling = 0;
		for (const auto &quad : out.quads())
			if (quad.layer == -2)
			{
				labelGlyphs += quad.rect.x < geometry.value.rect.x;
				straddling += quad.rect.x < geometry.value.rect.x && quad.rect.z > geometry.value.rect.x;
			}
		CHECK(labelGlyphs > 0 && straddling == 0,
			"a long slider label elides instead of overlapping the percentage readout");
	}
}

void changeNotifications()
{
	TextField first, second;
	WidgetSet set;
	set.reset({{0, 0, 200, 100}}, 100, 0);
	set.add({1, {{0, 0, 190, 36}}, "first", {}, &first});
	set.add({2, {{0, 40, 190, 76}}, "second", {}, &second});
	ControlId focus{};
	set.focus(focus, 1, false);
	const auto checkEdit = [&](WidgetEvent edit) {
		CHECK(edit.action == WidgetAction::Change && edit.id == focus, "effective text edit identifies the focused binding");
	};
	const auto key = [&](NavKey key, bool shortcut = false) { return set.key(focus, key, false, shortcut, false); };
	CHECK(key(NavKey::Backspace).action == WidgetAction::None, "deleting from empty text is a no-op");
	first.preedit("pending", 1, 2);
	CHECK(key(NavKey::Backspace).action == WidgetAction::None && first.value().empty() &&
		first.composition() == "pending", "preedit and composition editing do not change committed text");
	checkEdit(set.commitText(focus, "a\xc3\xa9"));
	CHECK(first.composition().empty() && key(NavKey::Left).action == WidgetAction::None,
		"caret movement and clearing preedit are not change actions");
	checkEdit(key(NavKey::Delete));
	CHECK(first.value() == "a" && key(NavKey::Delete).action == WidgetAction::None, "delete at end has no event");
	CHECK(key(NavKey::A, true).action == WidgetAction::None &&
		key(NavKey::C, true).action == WidgetAction::Copy &&
		set.commitText(focus, "a").action == WidgetAction::None, "selection, copy and identical replacement do not change text");
	CHECK(set.completeCut(focus).action == WidgetAction::None && first.value() == "a", "cut requires a selection");
	first.selectAll();
	CHECK(key(NavKey::X, true).action == WidgetAction::Cut && first.value() == "a",
		"cut does not mutate until the clipboard adapter completes it");
	checkEdit(set.completeCut(focus));
	CHECK(first.value().empty() && set.completeCut(focus).action == WidgetAction::None, "successful cut emits once");
	set.focus(focus, 2, false);
	CHECK(key(NavKey::V, true).action == WidgetAction::Paste && second.value().empty(), "paste asks the clipboard adapter");
	checkEdit(set.commitText(focus, "paste"));
	CHECK(second.value() == "paste" && first.value().empty() &&
		set.commitText(focus, "\n").action == WidgetAction::None, "paste and SDL commits share sanitization and notifications");
	set.setEnabled(2, false, focus);
	CHECK(set.commitText(2, "stale").action == WidgetAction::None &&
		set.completeCut(2).action == WidgetAction::None && second.value() == "paste", "disabled binding rejects delayed completion");
}
} // namespace

int main()
{
	routing();
	sessionLifecycle();
	pointerGestures();
	workspaceVisibility();
	transitionRecovery();
	screens();
	layoutAndWidgets();
	reusableBindings();
	textMetrics();
	selectionAndSliderClipping();
	textInput();
	changeNotifications();
	return testmain::exitCode();
}
