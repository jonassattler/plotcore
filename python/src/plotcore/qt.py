# SPDX-FileCopyrightText: 2026 Jonas Sattler
# SPDX-FileCopyrightText: 2026 Andreas Martin
# SPDX-License-Identifier: BSD-3-Clause

"""A figure in a PyQt6 widget.

What plotcore::qt::PlotWidget does in C++, in Python: tell the figure the
widget's size and how its fonts measure, hand it every wheel, press, move and
release, and paint the Scene that comes back. Nothing here decides where a tick
goes or what a gesture does.

    pip install plotcore[qt]
"""

from __future__ import annotations

import ctypes
from typing import Dict, Optional

import numpy as np

try:
    from PyQt6.QtCore import QPointF, QRectF, QSize, Qt, pyqtSignal
    from PyQt6.QtGui import (
        QColor,
        QCursor,
        QFont,
        QFontMetricsF,
        QImage,
        QPainter,
        QPen,
        QPolygonF,
    )
    from PyQt6.QtWidgets import QApplication, QWidget
except ImportError as error:  # pragma: no cover -- said once, plainly
    raise ImportError("plotcore.qt needs PyQt6: pip install plotcore[qt]") from error

from .figure import Button, Figure, Modifiers
from .scene import FORM_RUNS, Align, Mark, MarkKind, Scene, TextRole

def _qcolor(colour) -> QColor:
    return QColor.fromRgbF(*(float(c) for c in colour))


def _envelope(run: np.ndarray) -> bool:
    # A fold draws the minimum and the maximum of a column one after the other,
    # so the stroke reverses the height of the pane at every sample.
    if len(run) < 64:
        return False
    dy = np.abs(np.diff(run[:, 1]))
    return dy.size > 0 and float(dy.mean()) > 20.0


def _polygon(points: np.ndarray) -> QPolygonF:
    # A QPointF is two doubles, which is already how a run is stored. Building
    # one Python QPointF per sample is what made a hover cost more than the
    # fold: five thousand points, and the pane missed the next move.
    array = np.ascontiguousarray(points, dtype=np.float64)
    polygon = QPolygonF()
    count = int(array.shape[0])
    if count == 0:
        return polygon
    polygon.resize(count)
    ctypes.memmove(int(polygon.data()), array.ctypes.data, count * 16)
    return polygon


def _modifiers(keys) -> Modifiers:
    out = Modifiers.NONE
    if keys & Qt.KeyboardModifier.ShiftModifier:
        out |= Modifiers.SHIFT
    if keys & Qt.KeyboardModifier.ControlModifier:
        out |= Modifiers.CONTROL
    if keys & Qt.KeyboardModifier.AltModifier:
        out |= Modifiers.ALT
    return out


def _held(event) -> Modifiers:
    # The event carries the modifiers Qt last heard. A Shift held down before
    # this widget had the keyboard is not among them, so the gesture would
    # ignore it until a click gave the plot the focus. The platform is asked
    # as well, which is the keys actually down.
    return _modifiers(event.modifiers() | QApplication.queryKeyboardModifiers())


_MODIFIER_KEYS = (Qt.Key.Key_Shift, Qt.Key.Key_Control, Qt.Key.Key_Alt)


def _button(button) -> Optional[Button]:
    return {
        Qt.MouseButton.LeftButton: Button.LEFT,
        Qt.MouseButton.MiddleButton: Button.MIDDLE,
        Qt.MouseButton.RightButton: Button.RIGHT,
    }.get(button)


class PlotWidget(QWidget):
    """A QWidget over a plotcore Figure. `changed` is emitted after anything
    the reader did moved the view, a line or the reading."""

    changed = pyqtSignal()

    def __init__(self, figure: Optional[Figure] = None, parent: Optional[QWidget] = None):
        super().__init__(parent)
        self.figure = figure if figure is not None else Figure()
        self.last_scene: Optional[Scene] = None
        self._fonts: Dict[TextRole, QFont] = {}
        self.setMouseTracking(True)
        self.setFocusPolicy(Qt.FocusPolicy.WheelFocus)
        self._took_focus = False
        self.setAttribute(Qt.WidgetAttribute.WA_OpaquePaintEvent)
        self.setMinimumSize(160, 120)
        self._metrics: Dict[TextRole, QFontMetricsF] = {}
        self._body: Optional[QImage] = None
        self._body_ratio = 0.0
        self._body_scene: Optional[Scene] = None
        self.figure.set_text_measure(self._text_width, self._text_height)

    def sizeHint(self) -> QSize:  # noqa: N802 -- Qt's name
        return QSize(800, 500)

    # --- fonts ----------------------------------------------------------------

    def set_role_font(self, role: TextRole, font: QFont) -> None:
        self._fonts[role] = font
        self._metrics.pop(role, None)
        self.update()

    def role_font(self, role: TextRole) -> QFont:
        if role in self._fonts:
            return self._fonts[role]
        font = QFont(self.font())
        sizes = {
            TextRole.TICK: "tickPixels",
            TextRole.TICK_MINOR: "tickMinorPixels",
            TextRole.TITLE: "titlePixels",
            TextRole.AXIS_NAME: "axisNamePixels",
            TextRole.LEGEND: "legendPixels",
            TextRole.NOTE: "notePixels",
        }
        font.setPixelSize(max(1, round(self.figure.style[sizes[role]])))
        if role == TextRole.TITLE:
            font.setWeight(QFont.Weight.DemiBold)
        return font

    def _font_metrics(self, role: TextRole) -> QFontMetricsF:
        if role not in self._metrics:
            self._metrics[role] = QFontMetricsF(self.role_font(role))
        return self._metrics[role]

    def _text_width(self, text: str, role: TextRole) -> float:
        return self._font_metrics(role).horizontalAdvance(text)

    def _text_height(self, role: TextRole) -> float:
        return self._font_metrics(role).height()

    def changeEvent(self, event) -> None:  # noqa: N802
        # Also called while the widget is still being built, before there is
        # anything measured to forget.
        metrics = getattr(self, "_metrics", None)
        if metrics is not None:
            metrics.clear()
        super().changeEvent(event)

    # --- painting ---------------------------------------------------------------

    def paintEvent(self, _event) -> None:  # noqa: N802
        # Runs only: this host strokes polylines, and asking for the triangles
        # and the values as well built them and copied them out on every move.
        ratio = self.devicePixelRatioF()
        self.figure.set_size(self.width(), self.height(), ratio)
        scene = self.figure.scene(forms=FORM_RUNS)
        self.last_scene = scene
        painter = QPainter(self)
        try:
            # The crosshair moves and the lines do not. Stroking them again on
            # every move is what made the pane lag the pointer once a line was
            # long, so the ground, the grid and the lines stay and are blitted.
            if not self._body_matches(scene, ratio):
                self._store_body(scene, ratio)
            painter.drawImage(QPointF(0, 0), self._body)
            self._paint_over(painter, scene)
        finally:
            painter.end()

    def _body_matches(self, scene: Scene, ratio: float) -> bool:
        kept = self._body_scene
        if kept is None or self._body is None or self._body_ratio != ratio:
            return False
        if (kept.width, kept.height, kept.ground, kept.view, kept.area) != (
                scene.width, scene.height, scene.ground, scene.view, scene.area):
            return False
        if kept.under != scene.under or len(kept.lines) != len(scene.lines):
            return False
        for old, new in zip(kept.lines, scene.lines):
            if (old.colour, old.width, old.marker_radius) != (
                    new.colour, new.width, new.marker_radius):
                return False
            if len(old.runs) != len(new.runs) or old.markers.shape != new.markers.shape:
                return False
            if not np.array_equal(old.markers, new.markers):
                return False
            for old_run, new_run in zip(old.runs, new.runs):
                if old_run.shape != new_run.shape or not np.array_equal(old_run, new_run):
                    return False
        return True

    def _store_body(self, scene: Scene, ratio: float) -> None:
        # The ratio is applied by the painter, not by the image. Setting it on
        # the image first makes Qt stroke an envelope for seconds; see
        # _paint_body. It is set afterwards so the blit is still the right size.
        image = QImage(max(1, round(scene.width * ratio)), max(1, round(scene.height * ratio)),
                       QImage.Format.Format_ARGB32_Premultiplied)
        image.fill(Qt.GlobalColor.transparent)
        painter = QPainter(image)
        try:
            painter.scale(ratio, ratio)
            self._paint_body(painter, scene, ratio)
        finally:
            painter.end()
        image.setDevicePixelRatio(ratio)
        self._body = image
        self._body_ratio = ratio
        self._body_scene = scene

    def render(self, width: int, height: int, scale: float = 1.0) -> QImage:
        """The picture at `width` x `height` logical pixels, drawn at `scale`
        device pixels each -- laid out again at that size, not magnified."""
        image = QImage(round(width * scale), round(height * scale),
                       QImage.Format.Format_ARGB32_Premultiplied)
        image.fill(Qt.GlobalColor.transparent)
        scene = self.figure.scene(forms=FORM_RUNS, picture=(width, height), pixel_ratio=scale)
        painter = QPainter(image)
        try:
            painter.scale(scale, scale)
            self.paint_scene(painter, scene, scale)
        finally:
            painter.end()
        image.setDevicePixelRatio(scale)
        return image

    def _draw_text(self, painter: QPainter, text: Mark) -> None:
        font = self.role_font(text.role)
        raised = self.role_font(TextRole.TICK_MINOR)
        metrics = self._font_metrics(text.role)
        raised_metrics = self._font_metrics(TextRole.TICK_MINOR)
        base = text.base or text.text
        base_width = metrics.horizontalAdvance(base)
        width = base_width + (raised_metrics.horizontalAdvance(text.exponent)
                              if text.exponent else 0.0)
        height = metrics.height()

        def offset(align: Align, extent: float) -> float:
            return {Align.START: 0.0, Align.MIDDLE: -extent / 2.0, Align.END: -extent}[align]

        painter.save()
        painter.translate(QPointF(text.x0, text.y0))
        painter.rotate(text.turn)
        left = offset(text.across, width)
        top = offset(text.down, height)
        painter.setPen(_qcolor(text.colour))
        painter.setFont(font)
        painter.drawText(QPointF(left, top + metrics.ascent()), base)
        if text.exponent:
            painter.setFont(raised)
            painter.drawText(QPointF(left + base_width, top + metrics.ascent() - text.rise),
                             text.exponent)
        painter.restore()

    def _paint_mark(self, painter: QPainter, mark: Mark, area: QRectF) -> None:
        painter.save()
        if mark.clipped:
            painter.setClipRect(area)
        if mark.kind == MarkKind.TEXT:
            painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
            self._draw_text(painter, mark)
            painter.restore()
            return
        if mark.width > 0.0:
            pen = QPen(_qcolor(mark.stroke), mark.width)
            pen.setCapStyle(Qt.PenCapStyle.FlatCap)
            if mark.dashed:
                pen.setStyle(Qt.PenStyle.DashLine)
            painter.setPen(pen)
        else:
            painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(_qcolor(mark.fill) if mark.fill[3] > 0.0 else Qt.BrushStyle.NoBrush)
        if mark.kind == MarkKind.SEGMENT:
            # Crisp rather than smeared over two rows.
            painter.setRenderHint(QPainter.RenderHint.Antialiasing, False)
            painter.drawLine(QPointF(mark.x0, mark.y0), QPointF(mark.x1, mark.y1))
        elif mark.kind == MarkKind.BOX:
            # Filled to its bounds, and edged inside them.
            painter.setRenderHint(QPainter.RenderHint.Antialiasing, mark.radius > 0.0)
            box = QRectF(QPointF(mark.x0, mark.y0), QPointF(mark.x1, mark.y1))
            edge = painter.pen()
            painter.setPen(Qt.PenStyle.NoPen)
            painter.drawRoundedRect(box, mark.radius, mark.radius)
            if mark.width > 0.0:
                half = mark.width / 2.0
                radius = max(0.0, mark.radius - half)
                painter.setPen(edge)
                painter.setBrush(Qt.BrushStyle.NoBrush)
                painter.drawRoundedRect(box.adjusted(half, half, -half, -half), radius, radius)
        elif mark.kind == MarkKind.CIRCLE:
            painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
            painter.drawEllipse(QPointF(mark.x0, mark.y0), mark.radius, mark.radius)
        painter.restore()

    def _paint_body(self, painter: QPainter, scene: Scene, ratio: float = 1.0) -> None:
        """The ground, the grid and the lines: what does not move with the pointer."""
        painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
        if scene.ground[3] > 0.0:
            painter.fillRect(QRectF(0, 0, scene.width, scene.height), _qcolor(scene.ground))
        area = QRectF(scene.area.x, scene.area.y, scene.area.width, scene.area.height)
        for mark in scene.under:
            self._paint_mark(painter, mark, area)

        painter.save()
        painter.setClipRect(area)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
        for line in scene.lines:
            pen = QPen(_qcolor(line.colour), line.width)
            pen.setJoinStyle(Qt.PenJoinStyle.RoundJoin)
            pen.setCapStyle(Qt.PenCapStyle.RoundCap)
            painter.setPen(pen)
            painter.setBrush(Qt.BrushStyle.NoBrush)
            for run in line.runs:
                if len(run) == 1:
                    painter.drawPoint(QPointF(run[0, 0], run[0, 1]))
                    continue
                # Qt's stroker takes seconds on an envelope once a device pixel
                # ratio turns the hairline into a wider pen, or the ratio is a
                # transform on the stroke. One device pixel, with the ratio
                # already in the coordinates, is the same line and stays quick.
                if _envelope(run) and (ratio != 1.0 or line.width != 1.0):
                    painter.save()
                    painter.resetTransform()
                    painter.setClipRect(QRectF(area.x() * ratio, area.y() * ratio,
                                               area.width() * ratio, area.height() * ratio))
                    painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
                    hairline = QPen(_qcolor(line.colour), 1.0)
                    hairline.setJoinStyle(Qt.PenJoinStyle.RoundJoin)
                    hairline.setCapStyle(Qt.PenCapStyle.RoundCap)
                    painter.setPen(hairline)
                    painter.drawPolyline(_polygon(run * ratio))
                    painter.restore()
                else:
                    painter.drawPolyline(_polygon(run))
            if len(line.markers):
                painter.setPen(Qt.PenStyle.NoPen)
                painter.setBrush(_qcolor(line.colour))
                for x, y in line.markers:
                    painter.drawEllipse(QPointF(x, y), line.marker_radius, line.marker_radius)
                painter.setBrush(Qt.BrushStyle.NoBrush)
        painter.restore()

    def _paint_over(self, painter: QPainter, scene: Scene) -> None:
        area = QRectF(scene.area.x, scene.area.y, scene.area.width, scene.area.height)
        for mark in scene.over:
            self._paint_mark(painter, mark, area)

    def paint_scene(self, painter: QPainter, scene: Scene, ratio: float = 1.0) -> None:
        """Paint `scene`: all a host other than this widget needs to draw a
        figure with QPainter. Every colour, width and place is the scene's."""
        self._paint_body(painter, scene, ratio)
        self._paint_over(painter, scene)

    # --- events ---------------------------------------------------------------

    def showEvent(self, event) -> None:  # noqa: N802
        super().showEvent(event)
        # A window that opens under the pointer never gets an enter. The plot
        # takes the keyboard then, so the dock's first box does not keep it.
        if not self._took_focus:
            self._took_focus = True
            self.setFocus(Qt.FocusReason.OtherFocusReason)

    def enterEvent(self, event) -> None:  # noqa: N802
        # Shift and Ctrl are this plot's gestures. They arrive only at the
        # widget that has the keyboard, so the pointer arriving is enough and
        # a click is not required first.
        self.setFocus(Qt.FocusReason.MouseFocusReason)
        super().enterEvent(event)

    def _follow_modifiers(self) -> None:
        if QApplication.mouseButtons() == Qt.MouseButton.NoButton:
            return
        at = self.mapFromGlobal(QCursor.pos())
        if not self.rect().contains(at):
            return
        self.figure.move(float(at.x()), float(at.y()),
                         _modifiers(QApplication.queryKeyboardModifiers()))
        self.update()

    def keyPressEvent(self, event) -> None:  # noqa: N802
        if not event.isAutoRepeat() and event.key() in _MODIFIER_KEYS:
            self._follow_modifiers()
        super().keyPressEvent(event)

    def keyReleaseEvent(self, event) -> None:  # noqa: N802
        if not event.isAutoRepeat() and event.key() in _MODIFIER_KEYS:
            self._follow_modifiers()
        super().keyReleaseEvent(event)

    def wheelEvent(self, event) -> None:  # noqa: N802
        at = event.position()
        delta = event.angleDelta()
        # Qt reports a vertical wheel with Alt held as a horizontal one.
        angle = delta.y() if delta.y() != 0 else delta.x()
        self.figure.wheel(at.x(), at.y(), angle, _held(event))
        event.accept()
        self.update()
        self.changed.emit()

    def mousePressEvent(self, event) -> None:  # noqa: N802
        button = _button(event.button())
        if button is None:
            super().mousePressEvent(event)
            return
        self.setFocus(Qt.FocusReason.MouseFocusReason)
        at = event.position()
        self.figure.press(at.x(), at.y(), button, _held(event))
        event.accept()
        self.update()

    def mouseMoveEvent(self, event) -> None:  # noqa: N802
        at = event.position()
        self.figure.move(at.x(), at.y(), _held(event))
        event.accept()
        self.update()
        if event.buttons() != Qt.MouseButton.NoButton:
            self.changed.emit()

    def mouseReleaseEvent(self, event) -> None:  # noqa: N802
        button = _button(event.button())
        if button is None:
            super().mouseReleaseEvent(event)
            return
        at = event.position()
        self.figure.release(at.x(), at.y(), button, _held(event))
        event.accept()
        self.update()
        self.changed.emit()

    def mouseDoubleClickEvent(self, event) -> None:  # noqa: N802
        button = _button(event.button())
        if button is None:
            super().mouseDoubleClickEvent(event)
            return
        at = event.position()
        self.figure.double_click(at.x(), at.y(), button)
        event.accept()
        self.update()
        self.changed.emit()

    def leaveEvent(self, event) -> None:  # noqa: N802
        self.figure.leave()
        self.update()
        super().leaveEvent(event)

