# SPDX-FileCopyrightText: 2026 Jonas Sattler
# SPDX-License-Identifier: BSD-3-Clause

"""One frame of a figure, as plain Python and numpy.

Copied out of the C scene in one go, so nothing here refers to memory the
library owns: a Scene outlives the call that made it, and the figure it was
taken from, and can be handed to another thread.
"""

from __future__ import annotations

import ctypes
import enum
from dataclasses import dataclass, field
from typing import List, Optional, Tuple

import numpy as np

from . import _native

Colour = Tuple[float, float, float, float]


class TextRole(enum.IntEnum):
    """The faces text is set in; a host maps each to a font."""

    TICK = 0
    TICK_MINOR = 1
    TITLE = 2
    AXIS_NAME = 3
    LEGEND = 4
    #: What is said in passing: the legend's count of the lines it leaves out.
    NOTE = 5


class Align(enum.IntEnum):
    START = 0
    MIDDLE = 1
    END = 2


class MarkKind(enum.IntEnum):
    SEGMENT = 0
    BOX = 1
    CIRCLE = 2
    TEXT = 3


class Layer(enum.IntEnum):
    """What a mark is part of, for a host that paints only some of it."""

    #: The rules across the pane, under the lines.
    GRID = 0
    #: The axes, their numbers and names, the title and the legend.
    FRAME = 1
    #: The crosshair and its reading, the band, and why the pane cannot draw.
    OVERLAY = 2


#: The forms of a line a scene can carry; `Figure.scene(forms=...)` takes
#: them or-ed together.
FORM_RUNS = 1
FORM_TRIANGLES = 2
FORM_STRIPS = 4
FORM_VALUES = 8
FORM_BARS = 16
FORM_ALL = 31


@dataclass(frozen=True)
class Rect:
    x: float
    y: float
    width: float
    height: float


@dataclass(frozen=True)
class Chrome:
    """What everything that is not a line is drawn in."""

    ground: Colour
    ink: Colour
    rule_minor: Colour
    rule_major: Colour
    axis_rule: Colour
    cursor_ink: Colour


@dataclass
class Line:
    """One drawn line. Points are pane pixels with y down; `values` are the
    same points in the data's own units, for a host whose axes are its own.

    `bars` is the line as one bar a device column, when the setting `bars` is on
    and the line is dense enough, and it then has no runs, strips or triangles:
    two corners a bar, its top left and its bottom right, to fill antialiased in
    the line's colour. `bar_values` is the same corners in the data's units."""

    line: int
    colour: Colour
    width: float
    marker_radius: float
    runs: List[np.ndarray]
    values: List[np.ndarray]
    strips: List[np.ndarray]
    triangles: np.ndarray
    markers: np.ndarray
    bars: np.ndarray
    bar_values: np.ndarray
    y_low: float
    y_high: float
    y_log: bool
    own: bool
    numbered: bool


@dataclass
class Mark:
    """One thing to paint, in pane pixels with y down, which a host translates
    into its own call and decides nothing about.

    A SEGMENT runs from (x0, y0) to (x1, y1) in `stroke` at `width`. A BOX
    runs from its top left (x0, y0) to its bottom right (x1, y1), filled with
    `fill` and edged in `stroke` at `width`, its corners rounded to `radius`
    and its edge in dashes when `dashed`. A CIRCLE is about (x0, y0) at
    `radius`, edged inside its bounds. A TEXT is at (x0, y0): `base` in `role`'s face and `exponent`
    raised `rise` above it in TICK_MINOR's, `text` both in one string, turned
    `turn` degrees clockwise about that point and aligned in the turned frame.
    A clear fill is not painted, nor an edge of no width; a `clipped` mark is
    painted only inside the pane."""

    kind: MarkKind
    layer: Layer
    clipped: bool
    dashed: bool
    x0: float
    y0: float
    x1: float
    y1: float
    fill: Colour
    stroke: Colour
    width: float
    radius: float
    role: TextRole = TextRole.TICK
    across: Align = Align.START
    down: Align = Align.START
    colour: Colour = (0.0, 0.0, 0.0, 0.0)
    turn: float = 0.0
    rise: float = 0.0
    text: str = ""
    base: str = ""
    exponent: str = ""


@dataclass
class LegendEntry:
    line: int
    colour: Colour
    name: str
    swatch: Tuple[float, float, float, float]
    text_at: Tuple[float, float]


@dataclass
class Reading:
    valid: bool = False
    line: int = -1
    x: float = 0.0
    y: float = 0.0
    px: float = 0.0
    py: float = 0.0
    x_text: str = ""
    y_text: str = ""


@dataclass
class Scene:
    width: float
    height: float
    ground: Colour
    chrome: Chrome
    area: Rect
    view: Tuple[float, float, float, float]
    message: str
    #: The message in a few words.
    message_title: str
    #: Painted under the lines, in order: the grid.
    under: List[Mark] = field(default_factory=list)
    #: The lines, clipped to `area`.
    lines: List[Line] = field(default_factory=list)
    #: Painted over the lines, in order: everything else.
    over: List[Mark] = field(default_factory=list)
    #: Which line is which, and where its row was laid out; `over` paints it.
    legend: List[LegendEntry] = field(default_factory=list)
    legend_rect: Optional[Rect] = None
    reading: Reading = field(default_factory=Reading)
    band: Optional[Rect] = None

    @property
    def texts(self) -> List[Mark]:
        """The texts painted over the lines, in order."""
        return [mark for mark in self.over if mark.kind == MarkKind.TEXT]

    @property
    def rules(self) -> List[Mark]:
        """The grid's rules: everything painted under the lines."""
        return list(self.under)


def _colour(rgba: _native.Rgba) -> Colour:
    return (rgba.red, rgba.green, rgba.blue, rgba.alpha)


def _chrome(chrome: _native.Chrome) -> Chrome:
    return Chrome(_colour(chrome.ground), _colour(chrome.ink), _colour(chrome.rule_minor),
                  _colour(chrome.rule_major), _colour(chrome.axis_rule),
                  _colour(chrome.cursor_ink))


def _rect(rect: _native.Rect) -> Rect:
    return Rect(rect.x, rect.y, rect.width, rect.height)


def _text(raw: Optional[bytes]) -> str:
    return raw.decode("utf-8") if raw else ""


def _points(pointer, count: int) -> np.ndarray:
    if count <= 0 or not pointer:
        return np.zeros((0, 2))
    return np.ctypeslib.as_array(pointer, shape=(count * 2,)).reshape(count, 2).copy()


def _marks(handle, count: int, at) -> List[Mark]:
    raw = _native.Mark()
    out = []
    for index in range(count):
        at(handle, index, ctypes.byref(raw))
        kind = MarkKind(raw.kind)
        mark = Mark(
            kind=kind,
            layer=Layer(raw.layer),
            clipped=bool(raw.clipped),
            dashed=bool(raw.dashed),
            x0=raw.x0,
            y0=raw.y0,
            x1=raw.x1,
            y1=raw.y1,
            fill=_colour(raw.fill),
            stroke=_colour(raw.stroke),
            width=raw.width,
            radius=raw.radius,
        )
        if kind == MarkKind.TEXT:
            mark.role = TextRole(raw.role)
            mark.across = Align(raw.across)
            mark.down = Align(raw.down)
            mark.colour = _colour(raw.colour)
            mark.turn = raw.turn
            mark.rise = raw.raise_
            mark.text = _text(raw.text)
            mark.base = _text(raw.base)
            mark.exponent = _text(raw.exponent)
        out.append(mark)
    return out


def copy_scene(handle) -> Scene:
    """Everything in the C scene `handle`, which the caller still frees."""
    lib = _native.lib
    width = ctypes.c_double()
    height = ctypes.c_double()
    lib.plotcore_scene_size(handle, ctypes.byref(width), ctypes.byref(height))
    x0, x1, y0, y1 = (ctypes.c_double() for _ in range(4))
    lib.plotcore_scene_view(handle, ctypes.byref(x0), ctypes.byref(x1), ctypes.byref(y0),
                            ctypes.byref(y1))
    scene = Scene(
        width=width.value,
        height=height.value,
        ground=_colour(lib.plotcore_scene_ground(handle)),
        chrome=_chrome(lib.plotcore_scene_chrome(handle)),
        area=_rect(lib.plotcore_scene_area(handle)),
        view=(x0.value, x1.value, y0.value, y1.value),
        message=_text(lib.plotcore_scene_message(handle)),
        message_title=_text(lib.plotcore_scene_message_title(handle)),
    )
    info = _native.LineInfo()
    for index in range(lib.plotcore_scene_line_count(handle)):
        lib.plotcore_scene_line(handle, index, ctypes.byref(info))
        runs = []
        values = []
        strips = []
        for run in range(info.run_count):
            count = lib.plotcore_scene_line_run_length(handle, index, run)
            if count:
                runs.append(_points(lib.plotcore_scene_line_run(handle, index, run), count))
                pointer = lib.plotcore_scene_line_run_values(handle, index, run)
                if pointer:
                    values.append(_points(pointer, count))
            strip = lib.plotcore_scene_line_strip_length(handle, index, run)
            if strip:
                strips.append(_points(lib.plotcore_scene_line_strip(handle, index, run), strip))
        bars = lib.plotcore_scene_line_bar_count(handle, index)
        scene.lines.append(
            Line(
                line=info.line,
                colour=_colour(info.colour),
                width=info.width,
                marker_radius=info.marker_radius,
                runs=runs,
                values=values,
                strips=strips,
                triangles=_points(lib.plotcore_scene_line_triangles(handle, index),
                                  info.triangle_points),
                markers=_points(lib.plotcore_scene_line_markers(handle, index),
                                info.marker_count),
                bars=_points(lib.plotcore_scene_line_bars(handle, index), 2 * bars),
                bar_values=_points(lib.plotcore_scene_line_bar_values(handle, index), 2 * bars),
                y_low=info.y_low,
                y_high=info.y_high,
                y_log=bool(info.y_log),
                own=bool(info.own),
                numbered=bool(info.numbered),
            )
        )
    scene.under = _marks(handle, lib.plotcore_scene_under_count(handle), lib.plotcore_scene_under)
    scene.over = _marks(handle, lib.plotcore_scene_over_count(handle), lib.plotcore_scene_over)
    entry = _native.LegendEntry()
    for index in range(lib.plotcore_scene_legend_count(handle)):
        lib.plotcore_scene_legend(handle, index, ctypes.byref(entry))
        scene.legend.append(
            LegendEntry(
                line=entry.line,
                colour=_colour(entry.colour),
                name=_text(entry.name),
                swatch=(entry.x0, entry.y0, entry.x1, entry.y1),
                text_at=(entry.text_x, entry.text_y),
            )
        )
    if scene.legend:
        scene.legend_rect = _rect(lib.plotcore_scene_legend_rect(handle))
    reading = lib.plotcore_scene_reading(handle)
    scene.reading = Reading(
        valid=bool(reading.valid),
        line=reading.line,
        x=reading.x,
        y=reading.y,
        px=reading.px,
        py=reading.py,
        x_text=_text(reading.x_text),
        y_text=_text(reading.y_text),
    )
    band = _native.Rect()
    if lib.plotcore_scene_band(handle, ctypes.byref(band)):
        scene.band = _rect(band)
    return scene
