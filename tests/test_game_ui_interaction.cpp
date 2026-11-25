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

	ScreenStack stack;
	WidgetSet set;
	set.reset({{0, 0, 120, 80}}, 200, 0);
	set.add({1, WidgetKind::Button, {{0, 0, 120, 30}}, "first", {}});
	set.add({2, WidgetKind::Button, {{0, 40, 120, 70}}, "disabled", {}, false});
	set.add({3, WidgetKind::Button, {{0, 150, 120, 180}}, "below viewport", {}});
	CHECK(!set.at({5, 151}) && set.step(1, false)->id == 3 && set.step(1, true)->id == 3,
		"pointer clips hidden controls, keyboard skips disabled controls");
	set.press({5, 45}, stack);
	CHECK(!set.release({5, 45}) && !stack.focus(), "disabled control does not press or focus");
	set.press({5, 5}, stack);
	CHECK(!set.release({121, 5}), "release outside viewport does not activate");
	set.press({5, 5}, stack);
	set.cancelPress();
	CHECK(!set.release({5, 5}), "cancelled press cannot be replayed");
	set.viewport().reveal(set.find(3)->bounds);
	CHECK(set.viewport().offset == 100 && set.at({5, 65})->id == 3, "scroll reveal makes keyboard target visible");
	set.press({5, 65}, stack);
	CHECK(set.release({5, 65})->id == 3 && stack.focus() == 3, "painted hit and release use the same geometry");
	set.viewport().setOffset(500.f);
	CHECK(set.viewport().offset == 120.f, "scrolling clamps to content bounds");
	ListSelection list;
	list.scroll(99, 8, 3);
	CHECK(list.first == 5 && list.selected == 0, "list wheel is bounded and does not select");
	list.select(2, 8, 3);
	CHECK(list.first == 2 && list.selected == 2, "selection reveals its row");
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
}
} // namespace

int main()
{
	routing();
	screens();
	layoutAndWidgets();
	textInput();
	return testmain::exitCode();
}
