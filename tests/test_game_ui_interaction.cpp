#include <Engine/PlayerUiWidgets.hpp>

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
	const InputContext inventory{.inventory = true};
	const InputContext paused{.nativeScreen = true, .paused = true};
	const InputContext connected{.nativeScreen = true};
	const InputContext modal{.nativeScreen = true, .modal = true, .devKeyboard = true, .devPointer = true};
	const InputContext dialog{.nativeScreen = true, .devDialog = true};
	const InputContext unfocused{.interactive = false};
	const InputContext nativeTarget{.nativePointer = true};
	const InputContext coveredTarget{.devPointer = true, .nativePointer = true};
	const InputContext devText{.nativeScreen = true, .devText = true};
	const InputContext loading{.devPointer = true, .transition = true};
	CHECK(hud.choose(Key) == Gameplay && hud.choose(Pointer) == Gameplay && hud.choose(Text) == Blocked,
		"passive HUD leaves input to gameplay and ignores committed text");
	CHECK(developer.choose(Key) == DeveloperUi && developer.choose(Pointer) == DeveloperUi &&
		developer.choose(Text) == DeveloperUi && developer.choose(Key, true) == Global,
		"developer widgets capture actions while explicit globals bypass text");
	CHECK(nativeTarget.choose(Pointer) == PlayerUi && coveredTarget.choose(Pointer) == DeveloperUi,
		"visible HUD control has a pointer target only outside developer occlusion");
	CHECK(devText.choose(Text) == DeveloperUi && devText.choose(Key, false, true) == DeveloperUi,
		"active developer text takes one keyboard stream while a native nonmodal screen is visible");
	CHECK(inventory.choose(Key) == Blocked && inventory.choose(Key, false, true) == PlayerUi &&
		inventory.choose(Pointer) == Blocked && !inventory.allowsMovement(),
		"inventory blocks gameplay, offers Escape to the screen");
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

void screens()
{
	ScreenStack stack;
	CHECK(stack.top() == Screen::Hud && !stack.back() && !stack.blocksGameplay(), "HUD is passive root");
	CHECK(stack.push(Screen::Inventory) && !stack.pausesWorld(false) && stack.back(),
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

	set.press({50, 50}, focus, {}, 1.f);
	set.drag({98, 50}, 1.f);
	CHECK(firstSlider.value == 1.f && secondSlider.value == 0.25f, "pointer drag mutates only its bound slider");
	set.setEnabled(3, false, focus);
	CHECK(set.drag({8, 50}, 1.f).action == WidgetAction::None && firstSlider.value == 1.f,
		"disabled slider cancels active drag");
	set.setEnabled(3, true, focus);
	CHECK(set.release({8, 50}, 1.f).action == WidgetAction::None &&
		set.drag({8, 50}, 1.f).action == WidgetAction::None, "cancelled drag cannot resume when re-enabled");
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
} // namespace

int main()
{
	routing();
	pointerGestures();
	screens();
	layoutAndWidgets();
	reusableBindings();
	textInput();
	return testmain::exitCode();
}
