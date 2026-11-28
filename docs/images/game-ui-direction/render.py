"""Render the documentation-only Phase A concepts. See game-ui-visual-direction.md."""

import argparse
from functools import lru_cache
from itertools import combinations
from pathlib import Path
from random import Random

from PIL import Image, ImageColor, ImageDraw, ImageFont


PALETTES = {
    "slate": {
        "panel": "#202625", "rail": "#2D3531", "well": "#151B19",
        "button": "#303A35", "rim": "#70887D", "lip": "#46594F",
        "text": "#F4F6EF", "muted": "#B7C7BD", "disabled": "#96A69C", "accent": "#ABE5C8",
        "focus": "#FFFBEF", "danger": "#FFAD9C", "pending": "#C9BAED", "scrim": "#0C1310",
    },
    "strata": {
        "panel": "#172534", "rail": "#263A4E", "well": "#0F1925",
        "button": "#2B4056", "rim": "#728DA6", "lip": "#466078",
        "text": "#F2F5FA", "muted": "#BACBDD", "disabled": "#A1B3C5", "accent": "#F1CB87",
        "focus": "#FAFCFF", "danger": "#FFAD9C", "pending": "#CCB8F3", "scrim": "#090F18",
    },
}
SCRIM_OPACITY = {"slate": .78, "strata": .82}
INVENTORY_EXTENTS = ((960, 540), (1280, 720), (1920, 1080), (2560, 1080))
INVENTORY_SCALES = (1, 1.5, 2)
ITEMS = {
    "stone": ("#BEC6C9", "#818F97", "#525D6B"),
    "dirt": ("#C59B6A", "#9C6C46", "#634631"),
    "wood": ("#F1C28A", "#BD854D", "#885B38"),
    "grass": ("#B2D581", "#769649", "#53623B"),
    "sand": ("#F5DDAC", "#CFB37D", "#AA8D5C"),
}
HOTBAR = [("stone", 48), ("dirt", 16), ("wood", 24), ("grass", 8),
          ("sand", 32), None, None, None, None]
SUPPLIES = {0: ("wood", 32), 1: ("stone", 64), 2: ("grass", 12),
            9: ("dirt", 28), 10: ("sand", 18), 21: ("stone", 11)}


@lru_cache
def face(directory, size, strong):
    weight = "Semibold" if strong else "Regular"
    return ImageFont.truetype(directory / f"SourceSans3-{weight}.ttf", size)


def srgb_to_linear(channel):
    channel /= 255
    return channel / 12.92 if channel <= .04045 else ((channel + .055) / 1.055) ** 2.4


def linear_to_srgb(channel):
    srgb = channel * 12.92 if channel <= .0031308 else 1.055 * channel ** (1 / 2.4) - .055
    return round(srgb * 255)


@lru_cache
def scrim_lut(color, opacity):
    return tuple(linear_to_srgb(srgb_to_linear(value) * (1 - opacity)
                                + srgb_to_linear(channel) * opacity)
                 for channel in ImageColor.getrgb(color) for value in range(256))


def contains(outer, inner):
    return (outer[0] <= inner[0] <= inner[2] <= outer[2]
            and outer[1] <= inner[1] <= inner[3] <= outer[3])


def overlaps(a, b):
    return max(a[0], b[0]) < min(a[2], b[2]) and max(a[1], b[1]) < min(a[3], b[3])


def landscape(width, height, night):
    """Draw original voxel terraces from near slopes to a distant river opening."""
    sky = ((16, 25, 52), (85, 108, 146)) if night else ((101, 172, 209), (219, 231, 221))
    image = Image.new("RGB", (width, height))
    draw = ImageDraw.Draw(image)
    for y in range(height):
        fraction = min(y / (height * .75), 1)
        color = tuple(round(a + (b - a) * fraction) for a, b in zip(*sky))
        draw.line((0, y, width, y), fill=color)

    def polygon(points, color):
        draw.polygon([(round(x * width), round(y * height)) for x, y in points], fill=color)

    def shade(color, factor):
        return tuple(round(channel * factor) for channel in color)

    if night:
        stars = Random(326)
        for _ in range(34):
            x, y = stars.uniform(.08, .92), stars.uniform(.04, .33)
            draw.point((int(x * width), int(y * height)), fill="#E8EFF7")
    x, y = round(width * .68), round(height * .18)
    size = round(height * .062)
    draw.rectangle((x, y, x + size, y + size), fill="#E0EDFF" if night else "#FFF4CA")
    for x, y, length in ((.13, .2, .20), (.76, .32, .16), (.39, .36, .105)):
        polygon([(x, y), (x + length * .65, y), (x + length * .65, y + .018),
                 (x + length, y + .018), (x + length, y + .041), (x, y + .041)],
                "#647998" if night else "#E7EEE5")

    rng = Random(326)
    water = (41, 86, 121) if night else (102, 177, 190)
    polygon([(0, .55), (1, .55), (1, 1), (0, 1)], water)
    for _ in range(94):
        x, y = rng.uniform(.05, .95), rng.uniform(.55, 1)
        length = rng.uniform(.014, .17) * y
        polygon([(x, y), (x + length, y), (x + length, y + .003), (x, y + .003)],
                shade(water, rng.uniform(.82, 1.22)))

    rock = (81, 87, 105) if night else (111, 124, 114)
    grass = (58, 83, 89) if night else (112, 148, 94)
    for mirror in (False, True):
        for index in range(22):
            depth = (index + 1) / 22
            edge = .465 - depth ** 1.36 * (.30 if mirror else .22)
            top = .53 - depth * (.26 if mirror else .49) + rng.uniform(-.055, .055)
            base = .555 + depth ** 1.37 * .49
            step = .017 + depth * .069

            def terrace(points, color):
                polygon([(1 - x if mirror else x, y) for x, y in points], color)

            tone = rng.uniform(.70, 1.11)
            terrace([(0, top), (edge - step, top), (edge - step, base - .035),
                     (0, base - .035)], shade(rock, tone * .85))
            terrace([(edge - step, top), (edge, top + step * .32), (edge, base),
                     (edge - step, base - .035)], shade(rock, tone))
            terrace([(0, top - .023), (edge - step, top - .023), (edge, top + step * .32),
                     (edge - step, top + .015), (0, top + .015)], shade(grass, tone))
            for _ in range(4):
                y = rng.uniform(top + .025, base - .015)
                terrace([(edge - step, y), (edge, y + .02), (edge, y + .025),
                         (edge - step, y + .005)], shade(rock, tone * .78))
    return image


class Mockup:
    def __init__(self, font_dir, theme, width, height, scale, night=False):
        self.image = landscape(width, height, night)
        self.draw = ImageDraw.Draw(self.image)
        self.font_dir = font_dir
        self.layered = theme == "strata"
        self.colors = PALETTES[theme]
        self.scrim_opacity = SCRIM_OPACITY[theme]
        self.s = scale
        self.w = width / scale
        self.h = height / scale

    def point(self, x, y):
        return round(x * self.s), round(y * self.s)

    def box(self, x, y, w, h):
        return (*self.point(x, y), *self.point(x + w, y + h))

    def rect(self, x, y, w, h, color, outline=None, stroke=1):
        left, top = self.point(x, y)
        right, bottom = self.point(x + w, y + h)
        self.draw.rectangle((left, top, max(left, right - 1), max(top, bottom - 1)),
                            fill=self.colors.get(color, color),
                            outline=self.colors.get(outline, outline), width=max(1, round(stroke * self.s)))

    def text(self, x, y, value, size=15, color="text", strong=False, anchor="lm", bounds=None):
        font = face(self.font_dir, round(size * self.s), strong)
        position = self.point(x, y)
        if bounds is not None:
            ink = self.draw.textbbox(position, value, font=font, anchor=anchor)
            assert contains(bounds, ink), (value, bounds, ink)
        self.draw.text(position, value, font=font, fill=self.colors[color], anchor=anchor)

    def scrim(self):
        self.image.paste(self.image.point(scrim_lut(self.colors["scrim"], self.scrim_opacity)))

    def panel(self, x, y, w, h):
        if self.layered:
            self.rect(x + 3, y + 4, w, h, "scrim")
        self.rect(x, y, w, h, "panel", "rim")
        self.rect(x + 1, y + 1, w - 2, 3 if self.layered else 1, "lip")

    def heading(self, x, y, w, label, compact=False, bounds=None):
        if self.layered:
            self.rect(x + 1, y + 4, w - 2, 38 if compact else 46, "rail")
            self.rect(x + 16, y + 17, 3, 20, "accent")
        self.text(x + (27 if self.layered else 16), y + 28, label, 24, strong=True, bounds=bounds)

    def button(self, x, y, w, h, label, state="normal"):
        fill = {"primary": "accent", "danger": "danger", "quiet": "panel"}.get(state, "button")
        self.rect(x, y, w, h, fill, "rim" if state in ("normal", "quiet", "focus") else fill)
        if state == "focus":
            self.rect(x + 1, y + 1, w - 2, h - 2, None, "focus", 2)
        self.text(x + 10, y + h / 2, label, color="well" if state in ("primary", "danger") else "text",
                  strong=state in ("primary", "danger"), bounds=self.box(x + 6, y + 3, w - 12, h - 6))

    def item(self, x, y, size, kind, disabled=False):
        top, front, side = ITEMS[kind]
        # Original flat-colour silhouettes for the concepts. Runtime icons use the atlas.
        for points, color in (
            ([(0, 5), (12, 0), (24, 5), (12, 10)], top),
            ([(0, 5), (12, 10), (12, 24), (0, 19)], front),
            ([(12, 10), (24, 5), (24, 19), (12, 24)], side),
        ):
            if disabled:
                lut = scrim_lut(self.colors["well"], .35)
                color = tuple(lut[i * 256 + ch] for i, ch in enumerate(ImageColor.getrgb(color)))
            self.draw.polygon([self.point(x + dx * size / 24, y + dy * size / 24)
                               for dx, dy in points], fill=color)

    def slot(self, x, y, cell, stack, *, selected=False, focused=False, hover=False,
             pressed=False, pending=False, rejected=False, disabled=False, key=None):
        status = "danger" if rejected else "pending" if pending else None
        edge = status or ("accent" if selected else "rim")
        inner = self.box(x + 4, y + 4, cell - 8, cell - 8)
        regions = {
            "icon": self.box(x + cell - 20, y + 5, 15, 15),
            "count": self.box(x + 10, y + cell - 16, cell - 15, 12),
            "status": self.box(x + 5, y + 5, 10, 9),
            "interaction": self.box(x + 5, y + 14, 7, 6),
            "selection": self.box(x + 5, y + cell - 13, 3, 8),
        }
        assert all(contains(inner, box) for box in regions.values()), regions
        assert not any(overlaps(a, b) for a, b in combinations(regions.values(), 2)), regions
        self.rect(x, y, cell, cell, "button" if hover and not disabled else "well",
                  edge, 2 if status or selected else 1)
        if self.layered and not (focused and not disabled):
            self.rect(x + 2, y + 2, cell - 4, 1, "lip")
        if stack:
            kind, count = stack
            self.item(x + cell - 20, y + 5, 14, kind, disabled)
            self.rect(x + 10, y + cell - 16, cell - 15, 12, "well")
            self.text(x + cell - 5, y + cell - 10, str(count), 14,
                      "disabled" if disabled else "text", strong=True, anchor="rm", bounds=regions["count"])
        if selected:
            self.rect(x + 5, y + cell - 13, 3, 8, "accent")
        if rejected:
            self.rect(x + 8, y + 5, 2, 5, "danger")
            self.rect(x + 8, y + 11, 2, 2, "danger")
        elif pending:
            for offset in (5, 9, 13):
                self.rect(x + offset, y + 8, 2, 2, "pending")
        elif key:
            self.text(x + 6, y + 9, key, 11, "muted", bounds=regions["status"])
        if disabled:
            self.draw.line((*self.point(x + 6, y + 19), *self.point(x + 10, y + 15)),
                           fill=self.colors["disabled"], width=max(1, round(self.s)))
        elif pressed or hover:
            self.rect(x + 5, y + 17, 6 if pressed else 3, 2, "text")
        if focused and not disabled:
            self.rect(x + 3, y + 3, cell - 6, cell - 6, None, "focus")

    def drop_label(self, stack, enabled, pending, width, *, confirmed=True):
        if pending:
            return "Drop pending"
        if stack is None:
            return "Nothing to drop" if confirmed else "Drop unavailable"
        font = face(self.font_dir, round(15 * self.s), enabled)
        name, count = stack
        prefix, suffix = "Drop ", f" x{count}"
        name = name.title()
        if font.getlength(prefix + name + suffix) > width * self.s:
            while name and font.getlength(prefix + name + "…" + suffix) > width * self.s:
                name = name[:-1]
            name = name.rstrip() + "…"
        return prefix + name + suffix

    def inventory(self):
        self.scrim()
        standard_cell = 38 if self.layered else 36
        standard_height = 344 if self.layered else 340
        compact = self.h < 356
        cell = 36 if compact else standard_cell
        gap = 4
        stride = cell + gap
        grid_w = 9 * cell + 8 * gap
        width = grid_w + 32
        height = 254 if compact else standard_height
        x, y = (self.w - width) / 2, (self.h - height) / 2
        panel = self.box(x, y, width, height)
        margin = 8
        assert contains(self.box(margin, margin, self.w - 2 * margin, self.h - 2 * margin), panel)
        if self.layered:
            assert contains((0, 0, *self.image.size), self.box(x + 3, y + 4, width, height))
        self.panel(x, y, width, height)
        regions = []
        slots = []

        def region(left, top, w, h):
            box = self.box(left, top, w, h)
            regions.append(box)
            return box

        grid_x = x + 16
        self.heading(x, y, width, "Inventory", compact, region(grid_x, y + 9, width - 94, 35))
        region(x + width - 70, y + 13, 54, 29)
        self.button(x + width - 70, y + 13, 54, 29, "Close", "quiet")
        top = y + (46 if compact else 66)
        if self.layered and not compact:
            self.rect(grid_x - 5, top - 5, grid_w + 10, 3 * stride + 6, "well", "lip")
        for index in range(27):
            row, col = divmod(index, 9)
            sx, sy = grid_x + col * stride, top + row * stride
            slots.append(region(sx, sy, cell, cell))
            self.slot(sx, sy, cell, SUPPLIES.get(index),
                      hover=index == 1, focused=index == 10)
        hotbar = top + 3 * stride + (12 if compact else 28)
        self.text(grid_x, hotbar - 8, "Hotbar", 13, "muted", True,
                  bounds=region(grid_x, hotbar - 14, grid_w, 14))
        for index, stack in enumerate(HOTBAR):
            sx = grid_x + index * stride
            slots.append(region(sx, hotbar, cell, cell))
            self.slot(sx, hotbar, cell, stack, selected=index == 0)
        actions_y = hotbar + cell + (4 if compact else 8)
        self.text(grid_x, actions_y + 14, "Focused: Sand x18", 14, strong=True,
                  bounds=region(grid_x, actions_y, grid_w - 164, 28))
        drop_x = grid_x + grid_w - 156
        region(drop_x, actions_y, 156, 28)
        self.button(drop_x, actions_y, 156, 28, self.drop_label(HOTBAR[0], True, False, 140), "danger")
        if not compact:
            self.text(grid_x, actions_y + 43, "Click or Enter to move. Right-click or Shift+Enter for half.",
                      13, "muted", bounds=region(grid_x, actions_y + 34, grid_w, 17))
            self.text(grid_x, actions_y + 61, "Arrows / Tab focus. 1-9 / wheel select. Q drops one.",
                      13, "muted", bounds=region(grid_x, actions_y + 52, grid_w, 17))
        assert all(contains(panel, box) for box in regions), (panel, regions)
        assert not any(overlaps(a, b) for a, b in combinations(regions, 2)), regions
        assert all(abs((right - left) - (bottom - top)) <= 1
                   for left, top, right, bottom in slots)
        return "Compact" if compact else "Standard"

    def hud(self):
        cx, cy = self.w / 2, self.h / 2
        self.rect(cx - 6, cy - 1.5, 12, 3, "scrim")
        self.rect(cx - 1.5, cy - 6, 3, 12, "scrim")
        self.rect(cx - 5, cy - .5, 10, 1, "focus")
        self.rect(cx - .5, cy - 5, 1, 10, "focus")
        cell, gap = (38 if self.layered else 36), 4
        width = cell * 9 + 8 * gap
        x, y = cx - width / 2, self.h - cell - 14
        if self.layered:
            self.panel(x - 5, y - 5, width + 10, cell + 10)
        for index, stack in enumerate(HOTBAR):
            self.slot(x + index * (cell + gap), y, cell, stack, selected=index == 0,
                      key=str(index + 1) if self.layered else None)
        self.rect(cx - 27, y - 29, 54, 20, "well")
        self.text(cx, y - 19, "Stone", 14, strong=True, anchor="mm")
        self.rect(cx - 86, y - 55, 172, 20, "well")
        self.text(cx, y - 45, "1-9 / wheel select   E Inventory", 13, "text", anchor="mm")

    def menu(self):
        self.scrim()
        actions = ["Resume", "Save world", "Save and return to menu", "Multiplayer", "Settings",
                   "Widget showcase", "Exit to desktop", "Design system", "Hide HUD"]
        width, height = (474, 301) if self.layered else (306, 486)
        x, y = (self.w - width) / 2, (self.h - height) / 2
        self.panel(x, y, width, height)
        self.heading(x, y, width, "Game menu")
        if self.layered:
            self.button(x + 16, y + 63, width - 32, 36, actions[0], "primary")
            self.rect(x + width / 2 - 1, y + 111, 1, 146, "lip")
            for col, indices in enumerate(((1, 2, 3, 6), (4, 5, 7, 8))):
                for row, index in enumerate(indices):
                    self.button(x + 16 + col * 225, y + 114 + row * 39, 217, 32, actions[index],
                                "quiet" if index in (5, 7, 8) else "normal")
        else:
            self.button(x + 16, y + 54, width - 32, 34, actions[0], "primary")
            top = y + 103
            for label, indices in (("World", (1, 2, 3)), ("Preferences", (4,)), ("Utilities", (5, 7, 8))):
                self.rect(x + 16, top, width - 32, 22, "rail")
                self.text(x + 24, top + 11, label, 13, "muted", True)
                top += 26
                for index in indices:
                    self.button(x + 16, top, width - 32, 29, actions[index],
                                "quiet" if label == "Utilities" else "normal")
                    top += 33
                top += 6
            self.rect(x + 16, top, width - 32, 1, "rim")
            self.button(x + 16, top + 8, width - 32, 29, actions[6], "quiet")

    def dialog(self):
        self.scrim()
        width, height = (444, 242) if self.layered else (390, 222)
        x, y = (self.w - width) / 2, (self.h - height) / 2
        assert contains(self.box(8, 8, self.w - 16, self.h - 16), self.box(x, y, width, height))
        self.panel(x, y, width, height)
        self.heading(x, y, width, "Kick player")
        if self.layered:
            self.rect(x + 16, y + 61, width - 32, 120, "well", "lip")
        offset = 28 if self.layered else 16
        for top, label, size, color, strong in (
            (65, "Hosted world: native-menu-proof", 15, "text", True),
            (91, "Player #4  |  Admin", 15, "text", True),
            (117, "Rejoin blocked for 60 seconds.", 15, "text", False),
            (143, "This action can affect active players.", 14, "muted", False),
        ):
            self.text(x + offset, y + top + 12, label, size, color, strong,
                      bounds=self.box(x + offset, y + top, width - 2 * offset, 24))
        button_w = (width - 40) / 2
        self.button(x + 16, y + height - 44, button_w, 30, "Cancel", "focus")
        self.button(x + 24 + button_w, y + height - 44, button_w, 30, "Confirm", "danger")

    def states(self):
        self.draw.rectangle((0, 0, self.image.width, self.image.height), fill=self.colors["panel"])
        specimens = (
            ("Rest", {}), ("Hover", {"hover": True}), ("Pressed", {"hover": True, "pressed": True}),
            ("Selected", {"selected": True}), ("Focused", {"focused": True}),
            ("Pending", {"pending": True}), ("Rejected", {"rejected": True}), ("Disabled", {"disabled": True}),
            ("Selected + Focused", {"selected": True, "focused": True}),
            ("Selected + Pending", {"selected": True, "pending": True}),
            ("Selected + Disabled", {"selected": True, "disabled": True, "hover": True, "focused": True}),
            ("Pending + Focused", {"pending": True, "focused": True}),
            ("Rejected + Focused", {"rejected": True, "focused": True}),
            ("Selected + Pending\n+ Focused", {"selected": True, "pending": True, "focused": True}),
            ("Rejected + Pending", {"rejected": True, "pending": True}),
            ("Pending + Disabled", {"pending": True, "disabled": True}),
        )
        for index, (label, flags) in enumerate(specimens):
            row, col = divmod(index, 8)
            x, y, cell = 41 + col * 118, 17 + row * 100, 36
            self.slot(x, y, cell, HOTBAR[0], **flags)
            for line, text in enumerate(label.splitlines()):
                self.text(x + cell / 2, y + 54 + 15 * line, text, 13, "muted", anchor="mm",
                          bounds=self.box(x + cell / 2 - 59, y + 46 + 15 * line, 118, 16))


def validate_compositing():
    # A 50% linear-light blend of black and white encodes to 188, versus 128 in sRGB.
    assert scrim_lut("#000000", .5)[255] == 188
    assert scrim_lut("#FFFFFF", .5)[0] == 188
    # Check the approved Slate pairs against the Phase B floors.
    colors = PALETTES["slate"] | {"canvas": "#141A18"}
    luminance = {
        key: sum(weight * srgb_to_linear(channel)
                 for weight, channel in zip((.2126, .7152, .0722), ImageColor.getrgb(color)))
        for key, color in colors.items()
    }

    def contrast(a, b):
        low, high = sorted((a, b))
        return (high + .05) / (low + .05)

    surfaces = ("panel", "well", "rail", "button")
    for role in ("text", "muted", "disabled", "rim", "focus", "accent", "pending", "danger"):
        floor = 4.5 if role in ("text", "muted", "disabled") else 3
        for surface in surfaces:
            assert contrast(luminance[role], luminance[surface]) >= floor, (role, surface)
    assert contrast(luminance["focus"], luminance["rim"]) >= 3
    for role in ("rim", "accent", "danger"):
        assert contrast(luminance[role], luminance["canvas"]) >= 3
    for opacity in (.94, 1):
        for scene in (0, 1):
            plate = luminance["well"] * opacity + scene * (1 - opacity)
            for role in ("text", "muted"):
                assert contrast(luminance[role], plate) >= 4.5, (role, scene, opacity)


def validate(fonts):
    validate_compositing()
    layouts = 0
    for theme in PALETTES:
        for width, height in INVENTORY_EXTENTS:
            for scale in INVENTORY_SCALES:
                mock = Mockup(fonts, theme, width, height, scale)
                expected = "Compact" if (width, height, scale) == (960, 540, 2) else "Standard"
                assert mock.inventory() == expected, (theme, width, height, scale)
                layouts += 1
        for scale in INVENTORY_SCALES:
            Mockup(fonts, theme, round(950 * scale), round(204 * scale), scale).states()
            mock = Mockup(fonts, theme, 960, 540, scale, night=True)
            mock.dialog()
            label = mock.drop_label(("a very long nonbreaking material name", 64), True, False, 140)
            assert label.startswith("Drop ") and label.endswith("… x64"), label
            mock.button(10, 10, 156, 28, label, "danger")
            assert mock.drop_label(None, False, False, 140) == "Nothing to drop"
            assert mock.drop_label(None, False, False, 140, confirmed=False) == "Drop unavailable"
            assert mock.drop_label(HOTBAR[0], False, False, 140) == "Drop Stone x48"
            assert mock.drop_label(HOTBAR[0], False, True, 140) == "Drop pending"
            assert mock.drop_label(None, False, True, 140, confirmed=False) == "Drop pending"
            for status in ({"pending": True}, {"rejected": True, "pending": True}):
                mock.slot(10, 50, 36, HOTBAR[0], selected=True, focused=True, **status)
                left, top, right, bottom = mock.box(13, 53, 30, 30)
                edges = ([(x, y) for x in range(left, right) for y in (top, bottom - 1)]
                         + [(x, y) for y in range(top, bottom) for x in (left, right - 1)])
                assert all(mock.image.getpixel(point) == ImageColor.getrgb(mock.colors["focus"])
                           for point in edges), (theme, scale, status)
    print(f"{layouts} inventory layouts, compound states, compact confirmations, contrast and scrim witnesses passed.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fonts", type=Path, required=True)
    parser.add_argument("--out", type=Path, default=Path(__file__).parent)
    parser.add_argument("--validate", action="store_true", help="check layout and compositing without writing PNGs")
    options = parser.parse_args()
    if options.validate:
        validate(options.fonts)
        return
    options.out.mkdir(parents=True, exist_ok=True)
    for theme in PALETTES:
        cases = [
            ("inventory", "inventory", 1280, 720, 1, False),
            ("menu", "menu", 1280, 720, 1, False),
            ("dialog", "dialog", 1280, 720, 1, False),
            ("day", "hud", 1280, 720, 1, False),
            ("night", "hud", 1280, 720, 1, True),
            ("compact", "inventory", 960, 540, 2, False),
            ("ultrawide", "inventory", 2560, 1080, 1.5, False),
            ("states", "states", 950, 204, 1, False),
        ]
        for label, method, width, height, scale, night in cases:
            mock = Mockup(options.fonts, theme, width, height, scale, night)
            getattr(mock, method)()
            mock.image.save(options.out / f"{theme}-{label}.png", optimize=True)


if __name__ == "__main__":
    main()
