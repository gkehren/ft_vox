# Player design system (Game UI)

Issue #302, Epic #298 Phase D. This document is the visual and behavioral
specification of the player-facing design system: the one baseline theme, the
audited face, the scale policy, the reusable components, the inventory-slot
contract and the evidence that backs them.

Scope: the player interface only. Dear ImGui remains the Developer UI and keeps
`Engine/UiTheme.hpp` / `UiScale.hpp`; the two layers share no style state.

```text
Engine/UiTheme.hpp      -> Developer UI (ImGui) style, docking, F1..F7 panels
Engine/PlayerUiStyle.hpp-> player tokens: palette, type, spacing, sizing, states
Engine/PlayerUiText.hpp -> roles, fitting/ellipsis, glyph + icon fallback policy
Engine/PlayerUiScale.hpp-> player scale policy + frame-boundary controller
Engine/PlayerUiPreferences.* -> persisted override (settings seam)
Engine/PlayerUiComponents.*  -> reusable components + inventory slot contract
Engine/PlayerUiGallery.*     -> gallery/HUD-mock layout (pure) + section table
Engine/PlayerUiFonts.*       -> the audited face bake (graphics-scoped TU)
```

## 1. One baseline theme

`gameui::style::baseline()` is the only theme. There is no second palette: the
epic asks for one coherent baseline, and every component reads these tokens.

| Token | Value (sRGB) | Used for |
| --- | --- | --- |
| `ink` | 0.941, 0.969, 1.000 | primary text on player surfaces |
| `inkMuted` | 0.702, 0.769, 0.812 | captions, secondary values, trailing text |
| `inkDisabled` | 0.408, 0.451, 0.478 | disabled labels and glyphs |
| `inkOnAccent` | 0.031, 0.055, 0.071 | text on an accent-filled control |
| `canvas` | 0.027, 0.039, 0.050 | opaque backdrop behind a screen |
| `surface` | 0.052, 0.083, 0.103 | card face |
| `surfaceRaised` | 0.192, 0.341, 0.369 | headers, hovered/selected rows |
| `surfaceSunken` | 0.033, 0.055, 0.070 | wells, slots, list bodies |
| `border` | 0.157, 0.226, 0.259 | resting outline |
| `borderStrong` / `accent` | 0.310, 0.878, 0.760 | selection, primary action, progress |
| `focus` | 0.941, 0.969, 1.000 | keyboard focus ring |
| `scrim` | 0.016, 0.024, 0.031 @ 0.68 | modal scrim |
| `hudScrim` | 0.020, 0.031, 0.047 @ 0.62 | HUD text backdrop over the scene |
| `ok` / `warning` / `error` / `info` / `pending` | 0.478,0.847,0.541 / 0.961,0.769,0.353 / 0.929,0.443,0.396 / 0.522,0.757,0.949 / 0.694,0.639,0.910 | status semantics |

Typography (measured from the baked face, `kFontPixels = 13`):

| Role | Pixels | Line height | Where |
| --- | --- | --- | --- |
| `Caption` | 11 | 14 | slot counts, section captions, tooltips, validation messages |
| `Body` | 13 | 17 | labels, buttons, list rows, HUD text |
| `Heading` | 18 | 23 | section headers, dialog titles |
| `Title` | 26 | 32 | gallery specimen and future screen titles |

Spacing (`xxs` 2, `xs` 4, `sm` 6, `md` 8, `lg` 12, `xl` 16), sizing (control 28,
row 24, slot 36, slot gap 4, hotbar bottom margin 10, crosshair 5, border 1,
strong border 2, focus ring 2) and radii (2 / 4 / 6) are the only literals a
component may use, scaled by the resolved player scale.

### State precedence

`style::State` is a flag set; `style::interactiveState(enabled, hovered,
pressed, focused, selected)` is the single producer. Resolution:

| Component | Order |
| --- | --- |
| Button | disabled → pressed → primary/danger accent → hover → rest. Focus never changes a fill; it replaces the border with `focus` at ring width. |
| List row | disabled → selected (ink text + accent marker) → hovered (raised face, muted text) |
| Form field | validation (`error`, `pending`) → focused (`focus` border) → rest (`border`). Disabled keeps muted text and its validation border. |
| Slot | `style::slotTokens` resolves the whole appearance, one rule each: disabled clears focus/hover/press and dims the icon; validation (`error` > `pending`) owns the border colour; focus owns the border width (ring); selection owns the resting border strength. Focus therefore widens an error/pending border without hiding it. |

### Readability contract

Contrast is asserted, not eyeballed (`tests/test_game_ui_style.cpp`):

- `ink` and `inkMuted` on `surface` / `surfaceRaised` reach WCAG AA (4.5:1 body,
  3:1 large/secondary).
- HUD text is measured over `hudScrim` composited on the two extreme scene
  tokens (`kSceneBackdrop.bright` = sunlit sky/snow, `.dark` = night/cave):
  `ink` ≥ 6.2:1 and `inkMuted` ≥ 3.7:1 on **both**, which is why HUD text is
  always painted on the scrim and never directly on the world.
- `inkOnAccent` on `accent` ≈ 11.8:1 (primary button labels).
- `inkDisabled` stays perceivable but below the interactive floor (2:1 ≤ r <
  4.5:1) so a disabled control reads as disabled without vanishing.

## 2. The audited face and icons

| Asset | Source | License | Notice |
| --- | --- | --- | --- |
| ProggyClean bitmap face | `src/imgui/imgui_draw.cpp` (Dear ImGui 1.91.9 WIP embedded face) | MIT | `src/imgui/LICENSE.txt` |

`PlayerUiFonts.cpp` bakes one independent copy of that face into the player UI's
own alpha atlas at `kFontPixels × framebufferScale` (`bakeFace`), so:

- no new asset, download, package or license obligation is introduced;
- glyphs are rasterized at device resolution and mapped back to logical units
  through `fontScale = 1 / framebufferScale` (the same trick ImGuiLayer uses
  with `io.FontGlobalScale`);
- the coverage bitmap, extents and glyph table are owned by the player UI, not
  by ImGui's atlas.

Missing-glyph policy: the baked table covers printable ASCII. Any scalar without
a glyph renders as the documented `?` fallback, **once per Unicode scalar** (a
2-byte `é` is one `?`, not two), and nothing is ever silently dropped. Labels
are fitted through `fitText`, which returns the prefix that fits plus an
ellipsis request, so an over-long label elides instead of colliding with the
next control (`Caption - clipped: an intention...` in the gallery).

Missing-icon policy: item icons are generation-tagged atlas handles
(`AtlasIcon{generation, layer}`). `iconStatus` classifies them as `Valid`,
`StaleGeneration` (the atlas was reloaded — the handle must not be sampled) or
`MissingLayer`. A slot with a non-drawable icon keeps its geometry, frame,
count, selection and focus ring and paints a muted placeholder with the fallback
glyph, so an atlas reload never blanks the HUD.

## 3. Player UI scale

`PlayerUiScale.hpp` owns a scale that is **independent of the ImGui scale**
(`UiScale.hpp`) and of the developer docking layout.

| Mode | Rule |
| --- | --- |
| `Auto` (default) | `base = snapDown(contentScale)`, `fit = min(width / 900, height / 480)`, `scale = snapDown(min(base, fit))` — continuous across the fit box, snapped down onto `{0.75, 1, 1.25, 1.5, 1.75, 2}` |
| `Fixed` | an explicit user override snapped onto `{1, 1.25, 1.5, 1.75, 2}` (100–200%); ignores the window size, which is the point of an override |

The accepted Auto table (asserted in `GameUiStyle`):

| Logical resolution | Content scale | Auto scale |
| --- | --- | --- |
| 1920 × 1080 | 200% | 200% |
| 1600 × 900 | 200% | 175% |
| 1280 × 720 | 200% | 125% |
| 1160 × 680 | 125% | 125% |
| 900 × 480 | 200% | 100% |
| 800 × 480 | 150% | 75% |
| 0 × 0 (minimized) | invalid | last valid Auto scale |

Details:

- There is no special case at `fit >= 1`: the cap is `min(base, fit)` everywhere,
  so crossing 900×480 moves one step instead of jumping to the raw content
  scale.
- Degenerate inputs (0×0 while minimized, a non-finite or zero content scale)
  keep the last valid **Auto** scale without bumping the layout revision; a
  fresh controller falls back to the snapped content scale. Restoring the window
  re-resolves to the same value, so no layout is invalidated.
- A failed DPI query is passed through as **invalid**, never laundered into a
  valid 100%: `ScaleInputs::contentScaleValid` (produced by `scaleInputs`) tells
  the controller the truth, Auto keeps the last valid value, `Fixed` stays
  independent of the query, and the host skips the font-atlas refresh while the
  framebuffer density is unknown (the previous face stays active).
- 75% is auto-only: an override may not make text smaller than 100%.
- `ScaleController::resolve(inputs)` runs **once per frame, before any screen is
  built**. A changed value cancels the in-flight press and rebuilds the layout on
  the next frame; nothing invalidates the layout currently being painted and
  hit-test rectangles always match the painted geometry.

### Persisted override (settings seam)

The Engine loads the scale policy from `<saves-root>/.client-settings/player-settings.txt`
and passes it to the scale controller. **Settings** previews the scale before
saving it. The gallery's scale selector changes the presentation for that visit.
Going back or resetting the screen stack restores the prior player scale policy.
See [Player settings](game-ui-migration.md#player-settings-307) for
validation, legacy scale migration, and precedence.

## 4. Components

All painting goes through `gameui::Surface` (one `DrawList`, one `FontView`, one
theme, one resolved scale, one clip), and `WidgetSet` paints its interactive
controls through the same surface, so a real button and a gallery sample are
indistinguishable by construction.

| Component | API | Notes |
| --- | --- | --- |
| Panel / card | `card`, `cardContent`, `backdrop` | card face at `overlay.card`, modal scrim from `scrim` |
| Section / header | `sectionHeader`, `divider`, `galleryHeaderHeight` | heading line + rule; sections reserve the band so content never overlaps a title |
| Button | `button(rect, label, ButtonVariant, State)` | `Primary`, `Secondary`, `Quiet`, `Danger` |
| List row | `listRow(rect, title, trailing, State)` | selection marker, hover face, trailing value |
| Form field | `formField(rect, label, value, Validation, State, message)` + `formFieldHeight` | sunk well, validation border + message; the height helper reserves the label band, the control well and the message line, and a squeezed rect drops the message instead of drawing it over the well border |
| Status indicator | `statusIndicator(rect, Status, text)` | token-colored dot + label; the HUD paints it on the `hudScrim` plate, so it stays readable over bright and dark scenes |
| Bounded multiline | `wrapText` (PlayerUiText) + `wrappedLabel(rect, text, role, color, maxLines)` + `wrappedLineHeight` | greedy word wrap measured with the painting metric, capped at the caller's line count, last line elided; the HUD feedback block reserves its lines up front, so session copy cannot spill out of its band (issue #303 F03) |
| Progress / loading | `progress(rect, value, pending, phase)` | determinate fill, deterministic indeterminate band |
| Notification / toast | `ToastQueue` + `toast(rect, Toast)` | bounded (4) newest-first queue, age-based expiry, sticky entries |
| Tooltip | `tooltipLayout` + `tooltip` | prefers above-right, flips at edges, clamps inside the viewport |
| Confirmation | `confirmationLayout` + `confirmation` | centered panel, title/body, cancel left + primary right (the buttons are real controls); destructive dialogs focus Cancel |
| Inventory slot / grid | `SlotView`, `SlotGrid`, `slot`, `slotGrid`, `slotCell`, `slotIndexAt`, `centeredSlotStrip` | see below |

Corner treatment: `DrawList` only emits quads, so `Surface::fill` approximates
the radius token with a two-step octagon and `ring` with four edge bands. A true
rounded/SDF primitive is a renderer follow-up (recorded in §7).

### Inventory-slot contract

```cpp
struct SlotView {
    AtlasIcon icon;                 // generation-tagged atlas layer (0 = empty)
    uint32_t count;                 // 0/1 paint no badge, > 99 paints "99+"
    style::State state;             // Selected / Disabled / Focused / Pending / Error
    std::string_view label;         // tooltip/accessibility text
    std::optional<float> durability;// 0..1, presentation only
    std::optional<float> progress;  // 0..1, presentation only
};
```

Deliberately presentation-only: no item metadata is added, no save or wire field
changes, and nothing here reads authoritative state. Durability/progress are
optional indicators for future gameplay tooling; the slot renders them from the
existing status tokens. `#304` wires real inventory interaction to the same
`SlotView` (the gallery's strip cycles its sample selection through one
focusable target; per-slot presses are #304).

## 5. Gallery and HUD mock

Open **Design system** from the game menu. Launch with `--game-ui` to also show the
**HUD mock** entry. Both use the same components and input owner as the live HUD.

- `Screen::Gallery` — sections: design tokens, typography roles, four button
  variants, list rows (painted states + a real `ListBox`), form field with
  pending/rejected validation next to the live editor, status + determinate and
  indeterminate progress, notifications/tooltip, a confirmation with real
  actions, the inventory slot strip and the player-UI scale selector. One column
  in a narrow window, two columns once the card is ≥ 900 scaled units wide.
- `Screen::HudMock` — the #303 preview composed from real components: centered
  crosshair, nine-slot hotbar (selection, counts), status line on the HUD scrim,
  toast slot, interaction prompt. Passive: it holds no gameplay state.

The gallery layout is pure data (`PlayerUiGallery.hpp`), so "usable at small
windows, 16:9, 21:9 and 100–200%" is asserted headlessly, not just observed.

## 6. Evidence

Executed on this branch (Windows 11, MSVC 19.52, Vulkan 1.4.357, NVIDIA RTX
4070 Ti, `-DFT_VOX_GRAPHICS=ON`, Release):

```bash
ctest -C Release -R "GameUiStyle|GameUiComponents" --output-on-failure
test_engine_reload --game-ui-design-system docs/images/game-ui-design-system
test_engine_reload --game-ui-showcase-proof     # existing interaction proof
```

- `GameUiStyle` (170 checks) — palette contrast on bright/dark scenes, state
  precedence, type/spacing scales, auto/fixed scale policy incl. degenerate
  windows and invalid DPI inputs, controller frame boundary, resolved slot /
  toggle / slider tokens, preference round-trip, concurrent readers vs writers,
  an injected replacement failure, repair of a corrupt record and every
  corruption case.
- `GameUiComponents` (219 checks) — slot grid geometry/hit testing, tooltip
  placement and flipping, toast capacity/expiry/sticky/dismissal, confirmation
  geometry and destructive focus, progress fractions, text fitting and glyph
  fallback, icon fallback, owned/bounded notifications, toast timing at
  30–240 Hz, the field-group height contract, slot occupancy vs icon validity,
  label/border/band clamping, component painting/scissoring, a full HUD mock at
  eight size × scale combinations inside the 8-draw budget, the gallery layout
  at 16:9, 21:9, small windows, 100% and 200%, in-place notification ageing,
  degenerate (empty/fractional/narrow/flat) component geometry, clamped
  composite decorations (status, toast, slot, toggle, slider) and glyph clipping
  between 50% and 100% of a line height at 100/150/200%.
- `GameUiInteraction` — routing, focus, and the geometry contracts behind them:
  one text metric for painting/caret/hit testing at 100–200% with 1×/2×
  framebuffer density, scrolled-selection highlight visibility and clipping at
  the same scales, slider track mapping at 0/25/50/75/100%, disjoint slider
  label/value rects, and the IME composition-unit normalization (UTF-16 on
  Windows, scalars on Linux).
- `--game-ui-design-system` (49 checks, Vulkan validation enabled) — captures
  the gallery and HUD mock through the production composition path at
  1920×1080 / 2560×1080 / 1160×680 and 100% / 150% / 200%, checks the resolved
  scale against the requested policy, the destructive confirmation sample on the
  danger variant, the ≤ 32-draw menu budget with zero dropped quads, and the
  validation counters. `--game-ui-showcase-proof` (39 checks) additionally
  asserts that the real exit confirmation styles its action as danger and that
  one Enter right after opening it activates Cancel. The same proof exercises a
  failed and a successful font-atlas replacement on the GPU (metrics, descriptor
  footprint and resource round-trip) and the Engine preference seam (missing,
  corrupt, repaired, failed write and retry) on an isolated temporary directory.

Screenshots (canonical physical GPU, Vulkan validation on):
[gallery 1920×1080 100%](images/game-ui-design-system/gallery-1920x1080-100.png),
[gallery 2560×1080 (21:9)](images/game-ui-design-system/gallery-2560x1080-100.png),
[gallery 1160×680](images/game-ui-design-system/gallery-1160x680-100.png),
[gallery 1920×1080 200%](images/game-ui-design-system/gallery-1920x1080-200.png),
[HUD mock 1920×1080](images/game-ui-design-system/hud-1920x1080-100.png),
[HUD mock 2560×1080 150%](images/game-ui-design-system/hud-2560x1080-150.png),
[HUD mock 960×540](images/game-ui-design-system/hud-960x540-100.png).

The gallery captures above were regenerated after the round-2 corrections
(danger confirmation action, shared slider geometry, slot tokens).

Not executed here: a physical cross-monitor DPI change, macOS/MoltenVK capture,
an OS-driven IME session (still tracked in [#314](https://github.com/gkehren/ft_vox/issues/314))
and hands-on multiplayer/save flows (unchanged by this issue).

## 7. Review corrections

The first review of the design system found eight functional and nine
robustness issues; the second found three functional and seven robustness
issues. All P2 items are fixed in this PR; the tables are the map from finding to
code and to the check that pins it.

### Round 1 (F01–F17)

| # | Correction | Where | Evidence |
| --- | --- | --- | --- |
| F01 | One text metric (`TextStyle`: role × player scale × framebuffer) drives rendering, caret placement, mouse hit testing, selection, horizontal scrolling and the IME anchor; the SDL text-input area no longer carries a hardcoded line height and no geometry depends on a paint having run. | `PlayerUiText.hpp`, `PlayerUiWidgets.*`, `GameUiShowcase.cpp` | `GameUiInteraction::textMetrics` (100/150/200% × 1×/2×), backend proof caret/candidate check |
| F02 | Selection, preedit and caret primitives are clipped to the editor content rect (`fieldGeometry` returns the well inset ∩ viewport). | `PlayerUiWidgets.cpp` | paint path + `GameUiInteraction` long-field caret test |
| F03 | List rows use `listGeometry`/`listRowRect`/`listRowAt` for paint, hover and activation; clipped rows keep their visible part interactive and cannot take input outside the row window. | `PlayerUiWidgets.*` | `GameUiInteraction` hit/release parity |
| F04 | Auto scale is `snapDown(min(snapDown(contentScale), fit))`; the fit-box discontinuity is gone and invalid inputs keep the last valid Auto scale. | `PlayerUiScale.hpp` | `GameUiStyle::scaling` (accepted table + continuity) |
| F05 | The whole confirmation layout (panel, title, body, both buttons) is placed by the viewport transform that the widgets use. | `GameUiGallery.cpp`, `PlayerUiComponents.*` | `GameUiComponents::confirmations` placement invariance |
| F06 | Unique staging file per write, `MoveFileExW(REPLACE_EXISTING)` on Windows / `rename` elsewhere, bounded retry on sharing violations, only our staging file is ever deleted, and serialization/parsing is fully locale-independent. | `PlayerUiPreferences.cpp` | `GameUiStyle::preferences` (4 concurrent writers, failed write keeps the record, comma/period cases) |
| F07 | `Toast` owns `std::string` title/body, bounded to 96 bytes on a scalar boundary; painting allocates nothing. | `PlayerUiComponents.*` | `GameUiComponents::notifications` (source destroyed, over-long text) |
| F08 | `SlotView::icon` is `std::optional<AtlasIcon>`; occupancy is `count != 0`, atlas layer 0 is a real texture, and an occupied slot with a missing/stale icon keeps its count and frame. | `PlayerUiComponents.*`, `GameUiGallery.cpp` | `GameUiComponents::slotPaintingAndFallbacks` (layer 0, no icon, empty) |
| F09 | Composition units are normalized: UTF-16 code units on Windows, scalars on Linux; an index inside a surrogate pair rounds down and end-of-text ranges clamp. | `PlayerUiWidgets.*` | `GameUiInteraction` composition-range matrix; real OS IME pass tracked separately |
| F10 | Notifications and the indeterminate phase advance from a monotonic clock (`presentationStep`, 250 ms cap); toast ids are monotonic, not phase-derived. | `GameUiSample.cpp`, `PlayerUiComponents.hpp` | `GameUiComponents::notifications` (30/60/144/240 Hz) |
| F11 | The face is re-baked only when the framebuffer density changes; the renderer swaps the atlas after `waitIdle`, and every text path (standalone proofs included) applies the compensation. | `GameUiSample.cpp`, `GameUiFonts`, `GameUiRenderer.*` | code review + the GPU proofs on a 1× display; a physical DPI move is not reproducible on this workstation and is called out as such in the PR |
| F12 | Labels are fitted into their own rects, borders clamp to the box, the indeterminate band clamps to its track, and the gallery card is clipped by the viewport while its content is clipped by the body. | `PlayerUiComponents.cpp`, `GameUiGallery.cpp` | `GameUiComponents::painting` (long label, 4×4 ring, 6×4 bar) |
| F13 | `sliderGeometry`/`sliderValueAt` are shared by painting and dragging; the legacy interaction margin is gone. | `PlayerUiWidgets.*` | `GameUiInteraction` 0/25/50/75/100% track mapping |
| F14 | The quantity badge paints a contrast backing, owns a band above the durability/progress track, and never overlaps it. | `PlayerUiComponents.cpp` | `GameUiComponents` slot painting + captures |
| F15 | `settleState`/`slotState` centralize precedence: disabled clears focus/hover/press but keeps selection; toggle and slider faces are token functions; the exit confirmation is a Danger action whose initial focus is Cancel. | `PlayerUiStyle.hpp`, `PlayerUiWidgets.cpp`, `GameUiShowcase.cpp` | `GameUiStyle::states`, `GameUiComponents::confirmations` |
| F16 | Screen entry (`m_screenRevision`) is separate from layout rebuild (`m_layoutRevision`); a scale/resolution change keeps focus, selection, notifications, field values, list selection and the clamped scroll offset, and only screen entry resets them or cancels composition. | `GameUiSample.*`, `GameUiShowcase.cpp`, `GameUiGallery.cpp` | design-system proof: an in-session scale change keeps focus, the selected slot and the notification queue |
| F17 | The Engine tracks a dirty/failed persist state: selecting the same value retries a failed write, the error clears only on success, and a failed write is never re-attempted on every frame. | `EngineUiInput.cpp`, `Engine.hpp` | manual: read-only settings path, then repair + reselect |

### Round 2 (R01–R10)

Every round-2 finding is fixed with a regression test that fails without the
correction.

| # | Correction | Where | Evidence |
| --- | --- | --- | --- |
| R01 | The selection highlight is built **with** its horizontal scroll offset and intersected with the editor clip *before* the visibility test, then that same rect is painted — for committed text and for IME preedit selections. | `PlayerUiWidgets.cpp` | `GameUiInteraction::selectionAndSliderClipping`: a scrolled field at 100/150/200% × 1×/0.5× density still emits exactly one highlight quad, inside `editorClip`, plus its caret |
| R02 | A failed DPI query stays invalid end to end: `scaleInputs` reports `contentScaleValid=false`, the controller keeps the last valid Auto scale (no revision bump), `Fixed` ignores the failure, an identical restore is a no-op, and the host skips the atlas refresh while the density is unknown. | `PlayerUiScale.hpp`, `GameUiSample.*` | `GameUiStyle::scaling` conversion → controller matrix (0/NaN/0-density, restore, Fixed, Auto-after-Fixed) |
| R03 | `PlayerUiSettings::load` reads a bounded record from one open handle, rejects oversized records and distinguishes missing, corrupt or unreadable, and Windows sharing contention. | `PlayerUiPreferences.cpp` | `GameUiStyle::preferences`: two readers and 40 alternating writes check complete records. A blocked replacement preserves the previous record and cleans its staging file. |
| R04 | `sliderGeometry` gives the label and the percentage readout disjoint rects with a token gap; the label elides instead of painting over the value. | `PlayerUiWidgets.cpp` | `GameUiInteraction::selectionAndSliderClipping` (label `z` ≤ value `x` at 100/200% and 240/80/12 px, no glyph straddles the boundary) |
| R05 | `fill`/`ring`/`sectionHeader`/`divider`/`fittedLabel`/`labelAligned`/`progress` clamp to their own rect: no inverted band, border width clamped to the box, header rule clamped to `rect.w`, and a rect that cannot show half a line paints no text. | `PlayerUiComponents.cpp` | `GameUiComponents::degenerateGeometry` (empty, inverted, fractional, 3 px wide, 0.5 px tall) |
| R06 | `style::slotTokens` is the one resolver for slot visuals (face, border colour, border width, tint, indicator); `toggleFace`/`sliderFill`/`sliderTrackFace` give toggles and sliders pressed and hover feedback. Precedence documented above. | `PlayerUiStyle.hpp`, `PlayerUiComponents.cpp` | `GameUiStyle::states` asserts resolved colours, widths and the dim tint for `Error\|Focused`, `Pending\|Focused`, `Selected\|Disabled\|Focused`, `Disabled\|Error` |
| R07 | `Renderer::setFont` is transactional: it validates the replacement, creates and uploads the new atlas, `waitIdle()`s, rebinds the descriptors and only then destroys the old image and publishes `m_face`/`m_facePixels`/`m_fontScale`; a failure destroys the partial image and keeps the previous face, and an unchanged invalid density is not retried every frame. | `GameUiRenderer.*`, `GameUiSample.*` | GPU proofs (design-system, showcase, present) run on the production swap path with validation enabled |
| R08 | `ToastQueue::advance` ages entries in place and compacts with a move only when an entry actually expires (never self-moves, retired slots cleared). | `PlayerUiComponents.cpp` | `GameUiComponents::notificationLifetime` asserts stable `std::string` buffers/capacity across 20 steps, ordering, expiry, stickiness and capacity |
| R09 | A corrupt or unreadable preference record marks the settings dirty so an explicit selection of the current default rewrites it; a missing record stays a fresh install and nothing is overwritten at startup. | `EngineUiInput.cpp` | `GameUiStyle::preferences` corrupt → explicit save → valid record; missing stays `Missing` |
| R10 | The destructive action uses `ButtonVariant::Danger` in the gallery sample and in the real exit confirmation, Cancel stays first so it takes the initial focus, and Enter on a freshly opened dialog activates Cancel. | `GameUiShowcase.cpp`, `GameUiGallery.cpp` | design-system proof (gallery sample variants), showcase proof (danger styling + `ConfirmExit` → Enter → `Pause`) |

### Round 3 (S01–S04)

| # | Correction | Where | Evidence |
| --- | --- | --- | --- |
| S01 | `clipGlyph` intersects one glyph quad with its component rect and remaps the UV rectangle proportionally; `FontView::paint` takes an optional bound and skips fully clipped glyphs. `label`, `labelAligned` and `fittedLabel` all funnel into the clipped path, the field editor clips to `editorClip`, the surface scissor is unchanged (no extra batches), and the advance still moves so alignment, ellipsis and framebuffer compensation are untouched. | `PlayerUiText.hpp`, `PlayerUiWidgets.*`, `PlayerUiComponents.cpp` | `GameUiComponents`: 60/75/90% of a line height × 100/150/200% → every glyph quad inside the rect, valid non-inverted UVs, ≤ 2 batches |
| S02 | Composite decorations clamp or skip: `statusIndicator` shrinks its marker and text band, `toast` clamps the accent strip and body, `slot` clamps the icon inset, progress/durability band and badge (and paints no bar when there is no room), the toggle clamps its switch and label, the slider clamps label/value height and track. Empty and inverted rects return early. | `PlayerUiComponents.cpp`, `PlayerUiWidgets.cpp` | `GameUiComponents::degenerateGeometry` (empty, inverted, fractional, narrow, flat) plus a toggle and a slider at 90×40, 24×40, 90×14 and 12×12 |
| S03 | `GameUiSample::refreshFaceForDensity` is the explicit density seam (`setFaceDensityOverride` lets a host or a proof pick the target); the GPU proof refuses an invalid replacement (zero extent, mismatched coverage), keeps rendering on the working atlas, then performs a real 2× replacement, asserts the published CPU metrics match the GPU image, round-trips three times and restores the original footprint. | `GameUiSample.*`, `GameUiBackendProof.cpp` | `--game-ui-design-system`: `fontBytes`, `descriptorSets`, `m_face.width·height`, `m_fontScale`, `dropped == 0`, validation counters |
| S04 | The gallery scale preview restores the previous policy on Back and screen-stack reset. Settings handles preference loading, repair and explicit save retry. | `GameUiGallery.cpp`, `EnginePlayerSettings.cpp` | `--game-ui-design-system` and `--player-settings-proof` |

The only visible capture difference is a handful of ±1 pixels at label edges:
glyphs that used to extend a pixel past their component are now cut by the same
rect, which is exactly what S01 guarantees.

### Windows preferences contention (#317)

#318 changed Windows preference reads from a CRT stream to `CreateFileW` with
`FILE_SHARE_DELETE`. A CRT reader without that share flag can fail to open a
valid record while another handle has `DELETE` access. The Windows regression
test reproduces that conflict with an actual CRT stream and verifies that the
shared reader can still read the complete record.

On Windows 11 with MSVC, 20 instrumented runs of the pre-#318 code recorded
151 sharing-violation opens during concurrent saves. The latest failed open
was on attempt three, so the historical CI exhaustion was not reproduced
locally. The current test passed 40 traced runs. It recorded failed replacement
attempts followed by successful retries, and no failed opens during the
ordinary reader/writer race.

Windows reads and atomic replacements share a 40-attempt retry policy. A read
that exhausts a sharing, byte-lock or delete-pending conflict reports `Busy`.
Startup shows a busy notice and uses defaults for the launch without falling
back to the legacy record or marking settings for repair. The reader decodes
bytes from a single successful handle. Every successful load in the race check
must match one of the two complete records, and every write must succeed.

To diagnose a recurrence, set `FT_VOX_PLAYER_SETTINGS_TRACE=1` in the Windows
test process. It records the open, read or rename phase, Win32 error, attempt
number and elapsed microseconds for each failure. The existing `cpu-windows`
job enables traces on test failure. Set the repository Actions variable
`FT_VOX_GAME_UI_STYLE_REPEAT` to a count from 1 to 100 to opt in to its stress
step. The equivalent local PowerShell run is:

```powershell
$env:FT_VOX_PLAYER_SETTINGS_TRACE = '1'
ctest --test-dir build-cpu -C Release -R '^GameUiStyle$' --repeat until-fail:40 --output-on-failure
```

Use `-V` in place of `--output-on-failure` to see traces from passing runs too.

### IME platform facts (F09)

`SDL_EVENT_TEXT_EDITING::start/length` are **not** normalized by SDL. In the
bundled SDL 3.4.18:

- `src/video/windows/SDL_windowskeyboard.c` (`IME_SendEditingEvent`) passes
  `ime_cursor`, `ime_selected_start/length` and `SDL_wcslen(readingstring)` —
  UTF-16 **code units** — while `text` is UTF-8.
- `src/core/linux/SDL_fcitx.c` / `SDL_ibus.c` convert with `SDL_utf8strnlen`,
  i.e. **Unicode scalars**.

The player UI therefore normalizes with `CompositionUnit` (`kPlatformCompositionUnit`
selects UTF-16 on Windows, scalars elsewhere). A UTF-16 index inside a surrogate
pair rounds down, so a supplementary character can never be split into invalid
bytes. The font displays `?` for missing glyphs while the editor retains the
original UTF-8, including supplementary characters.

#### Preedit, candidate UI, and focus

SDL uses OS-managed composition by default where the backend supports it, so
both Game UI and Developer UI can use the native composition window. On backends
that deliver `SDL_EVENT_TEXT_EDITING`, the player field paints the reported
preedit. SDL receives the visible editor rect and caret offset in window
coordinates. The OS owns candidate selection and presentation in both modes.

To experiment with app-rendered preedit, set the SDL environment variable
`SDL_IME_IMPLEMENTED_UI=composition` before launching the game. SDL 3.4.18
reads the hint during Windows IME initialization, so the choice applies to the
whole process. In this mode, Developer ImGui fields display committed text and
lack inline preedit for composition that SDL delegates to the app. The Game UI
uses its fallback glyph where the atlas lacks a character, including much of
CJK. Use the default for regular play and Developer UI work.
[SDL IME hint](https://wiki.libsdl.org/SDL3/SDL_HINT_IME_IMPLEMENTED_UI) ·
[SDL 3.4.18 Windows backend](https://github.com/libsdl-org/SDL/blob/release-3.4.18/src/video/windows/SDL_windowskeyboard.c)

A text commit delivered before a focus change stays in the originating field.
Tab, clicking away, leaving the screen, or losing window focus cancels remaining
preedit. Hiding and minimizing also retire the editor. Clicking inside a field,
selecting all, cutting and pasting cancel native and local preedit while keeping
the editor's SDL session active. Copy reads the committed selection and leaves
preedit in place. SDL text commits insert normally. Resize and display-scale
changes keep the same focused SDL session while layout is rebuilt. Text events
reaching that editor during the rebuild still apply, and the next frame sends
the new anchor.

#### Validation for [#314](https://github.com/gkehren/ft_vox/issues/314)

`GameUiInteraction` covers range conversion, text geometry and editing
shortcuts. `test_engine_reload --game-ui-showcase-proof` uses the normal SDL
configuration. Add `--inline-ime` to explicitly request app-rendered preedit for
the fixture process before SDL starts. Both runs inject SDL events through the
production router and Vulkan presentation path. They check caret anchoring,
supplementary-plane selection and single commits at 100% and 200%, clipboard
commands during preedit, and existing preedit across each resize or display
event. The focus, hide and minimize cases cover same-batch and separate-batch
recovery. The showcase also verifies committed text handoff to and from a real
Developer UI ImGui input field.

On Windows build 26300 with SDL 3.4.18, the showcase passed 115 checks in each
mode and the design-system proof passed 103 checks. Vulkan validation reported
only the [known RTSS VUIDs](vulkan-validation.md). All 55 graphics-off CTest
cases passed on Windows, and the seven affected ASan/UBSan tests passed on
Debian WSL2.

Live Japanese, Chinese and Korean candidate placement, supplementary-character
composition, focus changes during OS composition, and moving an active
composition between different-DPI monitors remain pending. The Windows test
session exposed French and US English keyboard profiles. No IBus or Fcitx desktop
IME was available in the Linux test environment.

For the desktop pass, start `ft_vox --seed 42` and open **Pause → Widget
showcase**. Set **Player UI scale** in **Settings** before opening the showcase.

1. Use Microsoft Japanese IME, Microsoft Pinyin and Microsoft Korean IME in
   turn. Fill **Name** with a long ASCII prefix, move to its end and compose a
   phrase. Open the candidate list and check that it opens near the visible
   caret in the horizontally scrolled field.
2. At fixed 100% and 200%, use an IME that can compose U+1F332 or U+20000.
   Inspect preedit selection, commit once and copy the field into a Unicode-
   capable editor to verify the exact character behind the fallback glyph.
3. With fresh preedit each time, Tab away, click **Secondary name**, and return
   to Pause with Escape. Check that the old field contains only text already
   committed and that revisiting it displays no preedit. Record any keystroke
   the IME consumes before the game can navigate.
4. In windowed mode, drag the game between monitors with different OS scale
   factors while a candidate list is open. Verify that the preedit survives and
   the candidate list follows the caret, then commit and verify the result.
5. Record the OS, SDL version, IME, player scale, display scales and result for
   each case on the issue. Repeat with an available IBus or Fcitx desktop IME.

## 8. Limitations and follow-ups

- Rounded corners are a stepped octagon; a rounded/SDF UI primitive is a
  renderer follow-up, not a design-system one.
- One bitmap face means the display roles (`Heading`/`Title`) soften when scaled;
  a rasterized TTF face remains a future, separately reviewed dependency.
- The #303 live HUD uses the same slot components. #304 wires inventory slot
  interaction, and #307 brings the scale selector and preferences to Settings.
  [Live HUD controls and evidence](game-ui-migration.md#live-player-hud-303)
- The pause-menu previews are additive; the existing solo/multiplayer/hosting
  and developer flows are unchanged.

## Crafting composition preview

Pause → Design system includes a crafting table preview built from the same
confirmation card, slot grid, slot states and tooltip used by player screens.
The 3×3 input grid shows sample stacks and the output slot is disabled.
Hover a populated input to inspect its name. A future crafting or container
screen can supply a bounded snapshot of its own slots and route typed intents
through Engine. `SlotView` carries display state, including the atlas
generation, quantity and optional progress. Recipes, output validation and
inventory transfers belong to the simulation or server.
