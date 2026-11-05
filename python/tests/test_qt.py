# SPDX-FileCopyrightText: 2026 Jonas Sattler
# SPDX-License-Identifier: BSD-3-Clause

"""The PyQt6 widget paints what the scene says and hands the figure what the
reader did; and the example walks every feature without a failure."""

import importlib.util
from pathlib import Path

import numpy as np
import pytest

pytest.importorskip("PyQt6")

from PyQt6.QtCore import QPoint, QPointF, Qt  # noqa: E402
from PyQt6.QtGui import QColor, QMouseEvent, QWheelEvent  # noqa: E402
from PyQt6.QtWidgets import QApplication  # noqa: E402

import plotcore  # noqa: E402
from plotcore.qt import PlotWidget  # noqa: E402

EXAMPLE = Path(__file__).resolve().parents[1] / "pyqt6-demo" / "demo.py"


def test_the_widget_paints_each_line_in_its_colour(qapp):
    widget = PlotWidget()
    widget.figure.add_line(np.full(1000, 1.0), "flat")
    widget.figure.add_line(np.arange(1000.0), "ramp")
    widget.figure.settings["legend"] = False
    # Three pixels wide, so the middle of each stroke is its colour outright
    # rather than shared with the ground by the antialiasing of a hairline.
    widget.figure.style["lineWidth"] = 3.0
    widget.resize(640, 400)
    image = widget.render(640, 400, 1.0)
    scene = widget.figure.scene()
    for line in scene.lines:
        x, y = line.runs[0][len(line.runs[0]) // 2]
        want = QColor.fromRgbF(*line.colour)
        found = False
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                got = image.pixelColor(int(round(x)) + dx, int(round(y)) + dy)
                if (abs(got.redF() - want.redF()) + abs(got.greenF() - want.greenF())
                        + abs(got.blueF() - want.blueF())) < 0.1:
                    found = True
        assert found, f"line {line.line} is painted in its colour at its own point"
    ground = image.pixelColor(2, 2)
    assert (ground.redF(), ground.greenF(), ground.blueF()) == pytest.approx(scene.ground[:3])


def test_bars_fill_what_a_stroke_leaves_slits_in(qapp):
    # A band of noise a line's width of pixels never quite covers as a zigzag,
    # and the same band as bars: a row through the middle of it should be the
    # line's own colour in every column.
    rng = np.random.default_rng(11)
    values = rng.standard_normal(2_000_000)
    with plotcore.Figure() as figure:
        figure.add_line(values, "noise")
        widget = PlotWidget(figure)
        widget.resize(800, 400)
        figure.settings["cursor"] = False
        figure.settings["legend"] = False
        figure.settings["dark"] = False

        def pale_columns() -> int:
            image = widget.render(800, 400, 1.0)
            scene = figure.scene()
            row = int(scene.area.y + scene.area.height / 2)
            ground = image.pixelColor(2, 2)
            first = int(scene.area.x) + 1
            last = int(scene.area.x + scene.area.width) - 1
            inks = [image.pixelColor(x, row) for x in range(first, last)]
            ink = max(set(c.name() for c in inks), key=[c.name() for c in inks].count)
            assert ink != ground.name()
            return sum(1 for c in inks if c.name() != ink)

        assert pale_columns() > 0, "or this data does not show what bars are for"
        figure.settings["drawing"] = "bars"
        assert pale_columns() == 0
        figure.settings["drawing"] = "adaptive"
        assert pale_columns() == 0, "dense noise is bars when each form is chosen, too"


def test_the_widget_hands_the_figure_its_wheel_and_its_drag(qapp):
    widget = PlotWidget()
    widget.figure.add_line(np.arange(10_000.0), "ramp")
    widget.resize(640, 400)
    widget.show()
    widget.repaint()
    QApplication.processEvents()
    before = widget.last_scene.view
    middle = QPointF(320, 200)
    wheel = QWheelEvent(middle, widget.mapToGlobal(middle), QPoint(), QPoint(0, 240),
                        Qt.MouseButton.NoButton, Qt.KeyboardModifier.NoModifier,
                        Qt.ScrollPhase.NoScrollPhase, False)
    QApplication.sendEvent(widget, wheel)
    after = widget.figure.view
    assert after[1] - after[0] < before[1] - before[0]
    for kind, at, held in ((QMouseEvent.Type.MouseButtonPress, middle, Qt.MouseButton.LeftButton),
                           (QMouseEvent.Type.MouseMove, middle + QPointF(60, 0),
                            Qt.MouseButton.LeftButton),
                           (QMouseEvent.Type.MouseButtonRelease, middle + QPointF(60, 0),
                            Qt.MouseButton.NoButton)):
        event = QMouseEvent(kind, at, widget.mapToGlobal(at), Qt.MouseButton.LeftButton, held,
                            Qt.KeyboardModifier.NoModifier)
        QApplication.sendEvent(widget, event)
    assert widget.figure.view[0] < after[0]
    widget.close()


def test_the_example_walks_every_feature(qapp, tmp_path):
    pytest.importorskip("matplotlib")
    spec = importlib.util.spec_from_file_location("plotcore_pyqt6_demo", EXAMPLE)
    demo = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(demo)
    window = demo.DemoWindow()
    window.show()
    QApplication.processEvents()
    picture = tmp_path / "demo.png"
    assert window.self_test(str(picture)) == 0
    assert picture.stat().st_size > 10_000
    window.close()
