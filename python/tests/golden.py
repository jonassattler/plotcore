# SPDX-FileCopyrightText: 2026 Jonas Sattler
# SPDX-License-Identifier: BSD-3-Clause

"""golden/script.txt, run through the Python binding.

The language is golden/README.md's, and so is the digest: test_capi.c writes
it from C, this writes it from Python, and the two must agree token for token
-- numbers within a relative 1e-9, text exactly.
"""

from __future__ import annotations

import math
from pathlib import Path
from typing import List, Optional, Tuple

import numpy as np

import plotcore
from plotcore import Button, Figure, Layer, LineKind, MarkKind, Modifiers

GOLDEN = Path(__file__).resolve().parents[2] / "golden"


def generate(words: List[str]) -> Tuple[np.ndarray, int]:
    """A generator of README.md and how many words it took."""
    kind, n = words[0], int(words[1])
    i = np.arange(n, dtype=np.float64)
    if kind == "ramp":
        return float(words[2]) + i * float(words[3]), 4
    if kind == "quadratic":
        k = np.arange(1, n + 1, dtype=np.float64)
        return float(words[2]) * k * k, 3
    if kind == "square":
        period, low, high = int(words[2]), float(words[3]), float(words[4])
        steps = np.arange(n) % period
        return np.where(steps < period // 2, low, high).astype(np.float64), 5
    if kind == "burst":
        from_, to = int(words[2]), int(words[3])
        index = np.arange(n)
        inside = (index >= from_) & (index < to)
        return np.where(inside, np.where(index % 2 == 0, -100.0, 100.0), i * 0.0001), 4
    if kind == "holes":
        every = int(words[2])
        return np.where(np.arange(n) % every < 3, np.nan, i), 3
    if kind == "cloud":
        # The x of every point and then its y: each the mean of two draws of
        # the walk's stream.
        state = int(words[2]) & 0xFFFFFFFF
        out = np.empty(2 * n)
        for k in range(2 * n):
            state = (state * 1103515245 + 12345) & 0xFFFFFFFF
            a = ((state >> 16) & 0x7FFF) / 32767.0
            state = (state * 1103515245 + 12345) & 0xFFFFFFFF
            b = ((state >> 16) & 0x7FFF) / 32767.0
            out[k] = (a + b) / 2.0
        return out, 3
    if kind == "walk":
        state = int(words[2]) & 0xFFFFFFFF
        at = 0.0
        out = np.empty(n)
        for k in range(n):
            state = (state * 1103515245 + 12345) & 0xFFFFFFFF
            at += ((state >> 16) & 0x7FFF) / 32767.0 - 0.5
            out[k] = at + 500.0 if k % 997 == 996 else at
        return out, 3
    raise ValueError(f"no generator {kind}")


def _modifiers(word: str) -> Modifiers:
    out = Modifiers.NONE
    if "shift" in word:
        out |= Modifiers.SHIFT
    if "control" in word:
        out |= Modifiers.CONTROL
    if "alt" in word:
        out |= Modifiers.ALT
    return out


def _button(word: str) -> Button:
    return {"middle": Button.MIDDLE, "right": Button.RIGHT}.get(word, Button.LEFT)


def _g(value: float) -> str:
    return repr(float(value))


def _colour(colour) -> str:
    return " " + " ".join(_g(c) for c in colour)


def _marks(out: List[str], what: str, marks) -> None:
    """The segments of each layer as a count and sums, and every other mark on
    a line of its own, in the order it is painted -- test_capi's marks()."""
    for layer in (Layer.GRID, Layer.FRAME, Layer.OVERLAY):
        segments = [m for m in marks if m.kind == MarkKind.SEGMENT and m.layer == layer]
        if not segments:
            continue
        sx = sy = width = 0.0
        for m in segments:
            sx += m.x0
            sx += m.x1
            sy += m.y0
            sy += m.y1
            width += m.width
        out.append(f"{what} segments {int(layer)} {len(segments)} {_g(sx)} {_g(sy)} {_g(width)}")
    for m in marks:
        if m.kind == MarkKind.BOX:
            out.append(f"{what} box {int(m.layer)} {int(m.clipped)} {int(m.dashed)}"
                       f" {_g(m.x0)} {_g(m.y0)} {_g(m.x1)} {_g(m.y1)}" + _colour(m.fill)
                       + _colour(m.stroke) + f" {_g(m.width)} {_g(m.radius)}"
                       + (_colour(m.fill_to) if m.fill_to[3] > 0 else ""))
        elif m.kind == MarkKind.CIRCLE:
            out.append(f"{what} circle {int(m.layer)} {int(m.clipped)} {_g(m.x0)} {_g(m.y0)}"
                       f" {_g(m.radius)}" + _colour(m.fill) + _colour(m.stroke)
                       + f" {_g(m.width)}")
        elif m.kind == MarkKind.TEXT:
            out.append(f"{what} text {int(m.layer)} {int(m.role)} {int(m.across)} {int(m.down)}"
                       f" {_g(m.turn)} {_g(m.x0)} {_g(m.y0)}" + _colour(m.colour)
                       + f" |{m.text}")


def digest(figure: Figure, scene: plotcore.Scene, index: int) -> List[str]:
    out = [f"scene {index}", f"size {_g(scene.width)} {_g(scene.height)}"]
    a = scene.area
    out.append(f"area {_g(a.x)} {_g(a.y)} {_g(a.width)} {_g(a.height)}")
    out.append("view " + " ".join(_g(v) for v in scene.view))
    out.append("ground" + _colour(scene.ground))
    c = scene.chrome
    out.append("chrome" + _colour(c.ink) + _colour(c.axis_rule) + _colour(c.cursor_ink))
    out.append(f"message |{scene.message}")
    for line in scene.lines:
        sx = sy = vx = vy = 0.0
        points = 0
        for run, values in zip(line.runs, line.values):
            for (x, y), (u, v) in zip(run.tolist(), values.tolist()):
                sx += x
                sy += y
                vx += u
                vy += v
            points += len(run)
        out.append(f"line {line.line}" + _colour(line.colour)
                   + f" {_g(line.width)} {len(line.runs)} {points} {len(line.triangles)}"
                     f" {len(line.markers)} {_g(sx)} {_g(sy)}")
        out.append(f"window {_g(line.y_low)} {_g(line.y_high)} {int(line.y_log)} {int(line.own)}"
                   f" {int(line.numbered)} {_g(vx)} {_g(vy)}")
        if len(line.bars):
            bx, by = line.bars.sum(axis=0).tolist()
            bu, bv = line.bar_values.sum(axis=0).tolist()
            out.append(f"bars {len(line.bars) // 2} {_g(bx)} {_g(by)} {_g(bu)} {_g(bv)}")
        if line.kind == LineKind.POINTS:
            mx = my = me = ux = uy = 0.0
            rgba = [0.0, 0.0, 0.0, 0.0]
            for k in range(len(line.markers)):
                mx += float(line.markers[k][0])
                my += float(line.markers[k][1])
                me += float(line.marker_elements[k])
                if len(line.marker_colours):
                    for c in range(4):
                        rgba[c] += float(line.marker_colours[k][c])
                if len(line.marker_values):
                    ux += float(line.marker_values[k][0])
                    uy += float(line.marker_values[k][1])
            out.append(f"points {len(line.markers)} {_g(line.marker_radius)} {_g(mx)} {_g(my)}"
                       f" {_g(me)} " + " ".join(_g(c) for c in rgba) + f" {_g(ux)} {_g(uy)}")
            if line.image is not None:
                height, width = line.image.pixels.shape[:2]
                covered = int((line.image.pixels[:, :, 3] >= 128).sum())
                r = line.image.rect
                out.append(f"image {width} {height} {_g(r.x)} {_g(r.y)} {_g(r.width)}"
                           f" {_g(r.height)} {covered}")
    for what, marks in (("under", scene.under), ("over", scene.over)):
        _marks(out, what, marks)
    for e in scene.legend:
        out.append(f"legend {e.line}" + _colour(e.colour) + " "
                   + " ".join(_g(v) for v in e.swatch + e.text_at) + f" |{e.name}")
    if scene.legend:
        r = scene.legend_rect
        out.append(f"legend_rect {_g(r.x)} {_g(r.y)} {_g(r.width)} {_g(r.height)}")
    r = scene.reading
    out.append(f"reading {int(r.valid)} {r.line} {_g(r.x)} {_g(r.y)} {_g(r.px)} {_g(r.py)}"
               f" |{r.x_text}|{r.y_text}")
    if r.valid and (r.element >= 0 or r.value_text):
        out.append(f"reading_element {r.element} |{r.value_text}")
    axis = scene.colour_axis
    if axis is not None:
        out.append(f"colour_axis {_g(axis.low)} {_g(axis.high)} {int(axis.log)} {_g(axis.bar.x)}"
                   f" {_g(axis.bar.y)} {_g(axis.bar.width)} {_g(axis.bar.height)}")
    b = scene.band
    out.append("band 0 0 0 0 0" if b is None else
               f"band 1 {_g(b.x)} {_g(b.y)} {_g(b.width)} {_g(b.height)}")
    for line in range(len(figure)):
        chosen = figure.selection(line) if figure.line(line).kind == LineKind.POINTS else []
        if len(chosen):
            out.append(f"selection {line} {len(chosen)} {int(np.sum(chosen))}")
    return out


def _rest(line: str, skip: int) -> str:
    return line.split(" ", skip)[skip] if line.count(" ") >= skip else ""


def run(script: Path) -> List[str]:
    """Every scene the script asks for, digested, as lines."""
    figure = Figure()
    out: List[str] = []
    last: Optional[plotcore.Scene] = None
    kept: Optional[str] = None
    pointer = [0.0, 0.0]
    scenes = 0

    def marker(line: int, index: int) -> Tuple[float, float]:
        for drawn in last.lines if last else []:
            if drawn.line == line and index < len(drawn.markers):
                x, y = drawn.markers[index]
                return float(x), float(y)
        raise IndexError(f"no marker {index} of scatter {line}")

    def sample(line: int, index: int) -> Tuple[float, float]:
        for drawn in last.lines if last else []:
            if drawn.line == line and drawn.runs and index < len(drawn.runs[0]):
                x, y = drawn.runs[0][index]
                return float(x), float(y)
        raise IndexError(f"no sample {index} of line {line}")

    for raw in script.read_text(encoding="utf-8").splitlines():
        line = raw.rstrip("\r")
        if not line or line.startswith("#"):
            continue
        w = line.split(" ")
        command = w[0]
        if command == "size":
            figure.set_size(float(w[1]), float(w[2]), float(w[3]))
        elif command == "line":
            values, used = generate(w[1:])
            figure.add_line(values, _rest(line, 1 + used))
        elif command == "points":
            xy, used = generate(w[1:])
            half = len(xy) // 2
            figure.add_points(xy[:half], xy[half:], _rest(line, 1 + used))
        elif command == "point_values":
            figure.set_point_values(int(w[1]), generate(w[2:])[0])
        elif command == "point_colours":
            target = int(w[1])
            n = figure.line(target).length
            state = int(w[2]) & 0xFFFFFFFF
            rgba = np.empty(4 * n, dtype=np.uint8)
            for k in range(4 * n):
                state = (state * 1103515245 + 12345) & 0xFFFFFFFF
                rgba[k] = 255 if k % 4 == 3 else (state >> 16) & 0xFF
            figure.set_point_colours(target, rgba.reshape(n, 4))
        elif command == "select":
            figure.set_selection(int(w[1]), [int(v) for v in w[2:]])
        elif command == "clear_selection":
            figure.clear_selection()
        elif command == "click_point":
            pointer = list(marker(int(w[1]), int(w[2])))
            figure.press(pointer[0], pointer[1], Button.LEFT, _modifiers(w[3]))
            figure.release(pointer[0], pointer[1], Button.LEFT, _modifiers(w[3]))
        elif command == "pick":
            found = figure.point_at(float(w[1]), float(w[2]), 12.0)
            if found is None:
                out.append("pick 0 -1 -1 0 0")
            else:
                out.append(f"pick 1 {found[0]} {found[1]} {_g(found[2])} {_g(found[3])}")
        elif command == "remove":
            figure.remove_line(int(w[1]))
        elif command == "clear":
            figure.clear()
        elif command == "time":
            figure.set_time(None if w[1] == "none" else generate(w[1:])[0])
        elif command == "line_time":
            figure.set_line_time(int(w[1]), generate(w[2:])[0])
        elif command == "budget":
            figure.budget_bytes = int(w[1])
        elif command == "set_bool":
            figure.settings[w[1]] = w[2] != "0"
        elif command == "set_number":
            figure.settings[w[1]] = float(w[2])
        elif command == "set_text":
            figure.settings[w[1]] = _rest(line, 2)
        elif command == "set_colour":
            figure.settings[w[1]] = tuple(float(v) for v in w[2:6])
        elif command == "line_bool":
            figure.line(int(w[1])).settings[w[2]] = w[3] != "0"
        elif command == "line_number":
            figure.line(int(w[1])).settings[w[2]] = float(w[3])
        elif command == "line_text":
            figure.line(int(w[1])).settings[w[2]] = _rest(line, 3)
        elif command == "line_colour":
            figure.line(int(w[1])).colour = tuple(float(v) for v in w[2:6])
        elif command == "wheel":
            pointer = [float(w[1]), float(w[2])]
            figure.wheel(pointer[0], pointer[1], float(w[3]), _modifiers(w[4]))
        elif command == "press":
            pointer = [float(w[1]), float(w[2])]
            figure.press(pointer[0], pointer[1], _button(w[3]), _modifiers(w[4]))
        elif command == "move":
            pointer = [float(w[1]), float(w[2])]
            figure.move(pointer[0], pointer[1], _modifiers(w[3]))
        elif command == "release":
            pointer = [float(w[1]), float(w[2])]
            figure.release(pointer[0], pointer[1], _button(w[3]), _modifiers(w[4]))
        elif command == "double":
            figure.double_click(float(w[1]), float(w[2]), _button(w[3]))
        elif command == "hover":
            pointer = [float(w[1]), float(w[2])]
            figure.hover(*pointer)
        elif command == "leave":
            figure.leave()
        elif command == "reset":
            figure.reset_view()
        elif command == "view":
            figure.set_view(*(float(v) for v in w[1:5]))
        elif command in ("press_line", "wheel_line", "click_line"):
            pointer = list(sample(int(w[1]), int(w[2])))
            if command == "press_line":
                figure.press(pointer[0], pointer[1], Button.LEFT, _modifiers(w[3]))
            elif command == "wheel_line":
                figure.wheel(pointer[0], pointer[1], float(w[3]), _modifiers(w[4]))
            else:
                figure.press(pointer[0], pointer[1], Button.LEFT, _modifiers(w[3]))
                figure.release(pointer[0], pointer[1], Button.LEFT, _modifiers(w[3]))
        elif command == "move_by":
            pointer = [pointer[0] + float(w[1]), pointer[1] + float(w[2])]
            figure.move(pointer[0], pointer[1], _modifiers(w[3]))
        elif command == "release_here":
            figure.release(pointer[0], pointer[1], _button(w[1]), _modifiers(w[2]))
        elif command == "state":
            kept = figure.state()
            out.append(f"state |{kept}")
        elif command == "restore":
            figure.restore(kept or "")
        elif command == "scene":
            last = figure.scene()
            out.extend(digest(figure, last, scenes))
            scenes += 1
        else:
            raise ValueError(f"no command {command}")
    figure.close()
    return out


def same_line(expected: str, actual: str) -> bool:
    """README.md's comparison: numbers within a relative 1e-9, everything after
    the first '|' character for character."""
    head_e, bar_e, tail_e = expected.partition("|")
    head_a, bar_a, tail_a = actual.partition("|")
    if bar_e != bar_a or tail_e != tail_a:
        return False
    words_e = head_e.split()
    words_a = head_a.split()
    if len(words_e) != len(words_a):
        return False
    for e, a in zip(words_e, words_a):
        try:
            x, y = float(e), float(a)
        except ValueError:
            if e != a:
                return False
            continue
        if math.isnan(x) or math.isnan(y):
            if not (math.isnan(x) and math.isnan(y)):
                return False
            continue
        if abs(x - y) > 1e-9 * max(1.0, abs(x), abs(y)):
            return False
    return True
