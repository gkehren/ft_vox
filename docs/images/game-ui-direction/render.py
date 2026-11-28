"""Render the documentation-only Phase A concepts. See game-ui-visual-direction.md."""

import argparse
from functools import lru_cache
from pathlib import Path
from random import Random

from PIL import Image, ImageDraw, ImageFont


PALETTES = {
    "slate": {
        "panel": "#202625", "rail": "#2D3531", "well": "#151B19",
        "button": "#303A35", "rim": "#627970", "lip": "#46594F",
        "text": "#F4F6EF", "muted": "#B7C7BD", "accent": "#ABE5C8",
        "focus": "#FFFBEF", "danger": "#FFAD9C", "pending": "#C9BAED", "scrim": "#0C1310",
    },
    "strata": {
        "panel": "#172534", "rail": "#263A4E", "well": "#0F1925",
        "button": "#2B4056", "rim": "#637E96", "lip": "#466078",
        "text": "#F2F5FA", "muted": "#BACBDD", "accent": "#F1CB87",
        "focus": "#FAFCFF", "danger": "#FFAD9C", "pending": "#CCB8F3", "scrim": "#090F18",
    },
}
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
        self.s = scale
        self.w = width / scale
        self.h = height / scale

    def point(self, x, y):
        return round(x * self.s), round(y * self.s)

    def rect(self, x, y, w, h, color, outline=None, stroke=1):
        left, top = self.point(x, y)
        right, bottom = self.point(x + w, y + h)
        self.draw.rectangle((left, top, max(left, right - 1), max(top, bottom - 1)),
                            fill=self.colors.get(color, color),
                            outline=self.colors.get(outline, outline), width=max(1, round(stroke * self.s)))

    def text(self, x, y, value, size=15, color="text", strong=False, anchor="lm"):
        font = face(self.font_dir, round(size * self.s), strong)
        self.draw.text(self.point(x, y), value, font=font, fill=self.colors[color], anchor=anchor)

    def scrim(self):
        color = self.colors["scrim"]
        overlay = Image.new("RGB", self.image.size, color)
        self.image.paste(Image.blend(self.image, overlay, .60 if self.layered else .54))

    def panel(self, x, y, w, h):
        if self.layered:
            self.rect(x + 3, y + 4, w, h, "scrim")
        self.rect(x, y, w, h, "panel", "rim")
        self.rect(x + 1, y + 1, w - 2, 3 if self.layered else 1, "lip")

    def heading(self, x, y, w, label, compact=False):
        if self.layered:
            self.rect(x + 1, y + 4, w - 2, 38 if compact else 46, "rail")
            self.rect(x + 16, y + 17, 3, 20, "accent")
        self.text(x + (27 if self.layered else 16), y + 28, label, 24, strong=True)

    def button(self, x, y, w, h, label, state="normal"):
        fill = {"primary": "accent", "danger": "danger", "quiet": "panel"}.get(state, "button")
        self.rect(x, y, w, h, fill, "rim" if state in ("normal", "quiet", "focus") else fill)
        if state == "focus":
            self.rect(x + 1, y + 1, w - 2, h - 2, None, "focus", 2)
        self.text(x + 10, y + h / 2, label, color="well" if state in ("primary", "danger") else "text",
                  strong=state in ("primary", "danger"))

    def item(self, x, y, cell, kind):
        top, front, side = ITEMS[kind]
        # Original flat-colour silhouettes for the concepts. Runtime icons use the atlas.
        for points, color in (
            ([(0, 5), (12, 0), (24, 5), (12, 10)], top),
            ([(0, 5), (12, 10), (12, 24), (0, 19)], front),
            ([(12, 10), (24, 5), (24, 19), (12, 24)], side),
        ):
            size = min(24, cell * .62) / 24
            self.draw.polygon([self.point(x + (cell - 24 * size) / 2 + dx * size, y + 5 + dy * size)
                               for dx, dy in points], fill=color)

    def slot(self, x, y, cell, stack, selected=False, focused=False, hover=False, key=None):
        self.rect(x, y, cell, cell, "button" if hover else "well", "accent" if selected else "rim",
                  2 if selected else 1)
        if self.layered:
            self.rect(x + 2, y + 2, cell - 4, 2, "lip")
        if selected:
            self.rect(x + cell / 2 - 5, y + cell - 4, 10, 3, "accent")
        if focused:
            self.rect(x + 3, y + 3, cell - 6, cell - 6, None, "focus", 2)
        if stack:
            kind, count = stack
            self.item(x, y, cell, kind)
            label = str(count)
            font = face(self.font_dir, round(14 * self.s), True)
            label_w = font.getlength(label) / self.s + 4
            self.rect(x + cell - label_w - 4, y + cell - 19, label_w, 15, "well")
            self.text(x + cell - 6, y + cell - 12, label, 14, strong=True, anchor="rm")
        if key:
            self.text(x + 5, y + 9, key, 12, "muted", anchor="lm")

    def inventory(self):
        self.scrim()
        standard_cell = 38 if self.layered else 36
        standard_height = 4 * standard_cell + 196
        compact = self.h < standard_height + 16
        cell = 36 if compact else standard_cell
        gap = 4
        stride = cell + gap
        grid_w = 9 * cell + 8 * gap
        width = grid_w + 32
        height = 254 if compact else standard_height
        x, y = (self.w - width) / 2, (self.h - height) / 2
        assert x >= 8 and y >= 8
        self.panel(x, y, width, height)
        grid_x = x + 16
        self.heading(x, y, width, "Inventory", compact)
        self.button(x + width - 70, y + 13, 54, 29, "Close", "quiet")
        top = y + (46 if compact else 66)
        if self.layered and not compact:
            self.rect(grid_x - 5, top - 5, grid_w + 10, 3 * stride + 6, "well", "lip")
        for index in range(27):
            row, col = divmod(index, 9)
            self.slot(grid_x + col * stride, top + row * stride, cell, SUPPLIES.get(index),
                      hover=index == 1, focused=index == 10)
        hotbar = top + 3 * stride + (12 if compact else 28)
        self.text(grid_x, hotbar - (8 if compact else 13), "Hotbar", 13, "muted", True)
        for index, stack in enumerate(HOTBAR):
            self.slot(grid_x + index * stride, hotbar, cell, stack, selected=index == 0)
        actions_y = hotbar + cell + (4 if compact else 10)
        self.text(grid_x, actions_y + 14, "Sand x18", 14, strong=True)
        self.button(x + width - 167, actions_y, 151, 28, "Drop selected stack")
        if not compact:
            self.text(grid_x, actions_y + 45, "Click or Enter to move. Right-click or Shift+Enter for half.", 13, "muted")
            self.text(grid_x, actions_y + 63, "Arrows / Tab focus. 1-9 / wheel select. Q drops one.", 13, "muted")
        else:
            self.text(grid_x + 84, actions_y + 14, "Click / Enter to move", 13, "muted")
        assert actions_y + (28 if compact else 72) <= y + height - 5

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
        width, height = (474, 301) if self.layered else (286, 422)
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
            for index, action in enumerate(actions):
                self.button(x + 16, y + 54 + index * 39, width - 32, 33, action,
                            "primary" if index == 0 else "quiet" if index in (5, 7, 8) else "normal")

    def dialog(self):
        self.scrim()
        width, height = (444, 224) if self.layered else (390, 204)
        x, y = (self.w - width) / 2, (self.h - height) / 2
        self.panel(x, y, width, height)
        self.heading(x, y, width, "Confirm server action")
        if self.layered:
            self.rect(x + 16, y + 64, width - 32, 102, "well", "lip")
        offset = 28 if self.layered else 16
        self.text(x + offset, y + 79, "Kick", 18, "danger", True)
        self.text(x + offset, y + 104, "Player #4  |  Admin", strong=True)
        self.text(x + offset, y + 127, "Rejoin blocked for 60 seconds.")
        self.text(x + offset, y + 150, "This action can affect active players.", 14, "muted")
        button_w = (width - 40) / 2
        self.button(x + 16, y + height - 44, button_w, 30, "Cancel", "focus")
        self.button(x + 24 + button_w, y + height - 44, button_w, 30, "Confirm", "danger")

    def states(self):
        self.draw.rectangle((0, 0, self.image.width, self.image.height), fill=self.colors["panel"])
        for index, label in enumerate(("Rest", "Hover", "Pressed", "Selected", "Focused",
                                       "Pending", "Rejected", "Disabled")):
            x, y, cell = 42 + index * 88, 18, 38
            self.slot(x, y, cell, None if label == "Disabled" else HOTBAR[0],
                      selected=label == "Selected", focused=label in ("Focused", "Pending"),
                      hover=label in ("Hover", "Pressed"))
            if label == "Pressed":
                self.rect(x + 5, y + cell - 4, cell - 10, 2, "text")
            elif label == "Pending":
                self.rect(x, y, cell, cell, None, "pending", 2)
                self.rect(x + 5, y + 6, 13, 7, "well")
                for dot in range(3):
                    self.rect(x + 6 + 4 * dot, y + 8, 2, 2, "pending")
            elif label == "Rejected":
                self.rect(x, y, cell, cell, None, "danger", 2)
                self.rect(x + 3, y + 3, 10, 15, "well")
                self.text(x + 8, y + 10, "!", 14, "danger", True, anchor="mm")
            elif label == "Disabled":
                self.rect(x + 4, y + 4, cell - 8, cell - 8, "panel")
                self.draw.line((*self.point(x + 12, y + 26), *self.point(x + 26, y + 12)),
                               fill=self.colors["muted"], width=2)
            self.text(x + cell / 2, 74, label, 14, "muted", anchor="mm")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fonts", type=Path, required=True)
    parser.add_argument("--out", type=Path, default=Path(__file__).parent)
    options = parser.parse_args()
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
            ("states", "states", 740, 98, 1, False),
        ]
        for label, method, width, height, scale, night in cases:
            mock = Mockup(options.fonts, theme, width, height, scale, night)
            getattr(mock, method)()
            mock.image.save(options.out / f"{theme}-{label}.png", optimize=True)


if __name__ == "__main__":
    main()
