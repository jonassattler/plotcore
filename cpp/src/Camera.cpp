// SPDX-FileCopyrightText: 2026 Jonas Sattler
// SPDX-License-Identifier: BSD-3-Clause

#include "plotcore/Camera.hpp"

#include "plotcore/Format.hpp"
#include "plotcore/Ticks.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

// The functions of the same names in H5Scope's src/qml/PlotSurface.qml, with
// the state PlotSurface reads off its own properties passed in. The argument
// for each is written over it: the viewer's own, carried here from the QML
// when the viewer came to draw through Figure, and kept in its words -- so a
// "surface" in one is PlotSurface, and a property it names is one of its own
// that is an argument here.

namespace plotcore {

// Which axes a wheel event with these modifiers zooms.
//
// Shift alone is x, Ctrl alone is y, and everything else -- neither, both,
// or one of them with a third key held -- is both. Both is the default
// rather than nothing, because a modifier this file does not know about is
// a modifier some window manager put there and not an instruction.
ZoomAxes zoomAxesFor(bool shift, bool control)
{
    if (shift && !control) {
        return ZoomAxes::X;
    }
    if (control && !shift) {
        return ZoomAxes::Y;
    }
    return ZoomAxes::Both;
}

// Where a value sits along an axis, in the units the zoom and the pan are
// measured in -- and back again.
//
// The whole of what a logarithmic axis changes about the gestures, and
// the two are the only lines below that know which scale they are on.
// Zoom and pan are arithmetic over an axis's *span*, and on a logarithmic
// axis the only span that means anything is a count of decades: a wheel
// notch has to take the same bite out of a pane showing 1 to 10 as out of
// one showing 1e6 to 1e7, and a drag has to move by so many decades rather
// than by so many units. Measured in the data's own units instead, one
// notch at the low end of a six-decade axis would zoom past every float
// there is while the high end had not visibly moved at all.
//
// So `panX` and `panY` are in decades while their axis is logarithmic,
// and that is why switching a scale resets the view: a pan of 40 is four
// decades or it is forty units, and there is no reading of it that is both.
double axisPosition(double value, bool logarithmic, double base)
{
    return logarithmic ? logOf(value, base) : value;
}

double axisValue(double position, bool logarithmic, double base)
{
    return logarithmic ? std::pow(base, position) : position;
}

// The value `at` of the way from `low` to `high`. The inverse of
// PlotFrame.fractionOn, and the same arithmetic gui::AxisMapping::valueAt
// does -- which is what makes a pointer position resolve to the value the
// tick under it prints.
double valueAlong(double low, double high, double at, bool logarithmic, double base)
{
    const double from = axisPosition(low, logarithmic, base);
    const double to = axisPosition(high, logarithmic, base);
    return axisValue(from + at * (to - from), logarithmic, base);
}

// The window the axes are actually showing, worked out with the same
// arithmetic ValueAxis uses. The footer prints these, because once the
// view has been moved the range of the data is no longer the range on
// screen and only one of the two is worth reading.
double visibleLow(double low, double high, double zoom, double pan, bool logarithmic, double base)
{
    const double from = axisPosition(low, logarithmic, base);
    const double to = axisPosition(high, logarithmic, base);
    return axisValue((from + to) / 2.0 + pan - (to - from) / zoom / 2.0, logarithmic, base);
}

double visibleHigh(double low, double high, double zoom, double pan, bool logarithmic, double base)
{
    const double from = axisPosition(low, logarithmic, base);
    const double to = axisPosition(high, logarithmic, base);
    return axisValue((from + to) / 2.0 + pan + (to - from) / zoom / 2.0, logarithmic, base);
}

// `low`..`high` with a little air at each end, so a line at the extreme is
// a line and not part of the frame. A flat series has no span to take a
// share of, and gets a unit of room instead.
//
// In the axis's own scale, which is what makes it the same air on either
// one: a twentieth of a *decade* below the smallest value rather than a
// twentieth of the value itself, which on a trace running from 1e-9 to 1
// would be nine decades of margin over the top and none at all underneath.
//
// A flat line on a logarithmic axis runs between the powers either side
// of it -- see the branch below.
//
// A logarithmic axis with nothing above zero to draw between gets one
// empty decade. It draws nothing either way -- there is no value it could
// place -- and an axis with legible numbers on it is a better answer than
// one with none, which is what a span of zero would give.
//
// A flat line is widened to the powers either side of it,
// which is matplotlib's `LogLocator.nonsingular`: the power
// strictly below and the power strictly above, so a line at 5
// runs 1 to 10 and one at 10 runs 1 to 100. Then the air, as
// for any other line.
Span padded(double low, double high, bool logarithmic, double base)
{
    if (logarithmic) {
        if (!(low > 0.0) || !(high > 0.0) || !usableBase(base)) {
            return {1.0, base > 1.0 ? base : 10.0};
        }
        double from = logOf(low, base);
        double to = logOf(high, base);
        if (!(to > from)) {
            const double at = std::abs(from - jsRound(from)) < 1e-10 ? jsRound(from) : from;
            const bool whole = std::isfinite(at) && std::floor(at) == at;
            from = whole ? at - 1.0 : std::floor(at);
            to = whole ? at + 1.0 : std::ceil(at);
        }
        const double air = (to - from) * 0.05;
        return {std::pow(base, from - air), std::pow(base, to + air)};
    }
    const double air = high > low ? (high - low) * 0.05 : 1.0;
    return {low - air, high + air};
}

// The base a mode and its number resolve to.
//
// Ten for anything this build does not recognise, which is the stance the
// rest of the surface takes on a remembered setting it cannot read: a
// saved view naming a base from some later version costs the reader that
// base and not the whole of their view. It is also the last guard on the
// number itself -- everything downstream may assume this answered.
double baseFor(const char* mode, double custom)
{
    if (mode != nullptr && std::strcmp(mode, "2") == 0) {
        return 2.0;
    }
    if (mode != nullptr && std::strcmp(mode, "e") == 0) {
        return std::numbers::e;
    }
    if (mode != nullptr && std::strcmp(mode, "custom") == 0) {
        return usableBase(custom) ? custom : 10.0;
    }
    return 10.0;
}

// Pan clamped so that at least panKeep of the window stays over the data.
//
// It used to keep the whole window inside the data, so with the zoom at
// 1 the only legal pan was none and the view could never be dragged at
// all. In positions, like every gesture: the window is `full / zoom` wide
// and its centre may go as far past either end of the data as leaves
// `panKeep` of it on the near side. One rule for every path that moves
// the window -- the drag, the wheel, the band and the four boxes -- so a
// wheel notch after a drag off the edge zooms where the reader is rather
// than snapping them back inside.
double clampPan(double pan, double zoom, double low, double high, bool logarithmic, double base,
                double keep)
{
    const double full =
        axisPosition(high, logarithmic, base) - axisPosition(low, logarithmic, base);
    const double span = full / zoom;
    const double room = full / 2.0 + span * (0.5 - keep);
    return std::max(-room, std::min(room, pan));
}

// The largest zoom that keeps a logarithmic window at least `minimumSpan`
// wide in the data's own units, when the value at `held` -- a position,
// so a logarithm -- stays at `fraction` of the pane.
//
// The window's width grows with its span in decades, so the answer is a
// bisection of that span rather than a formula: the width is a difference
// of two powers, and solving it in closed form is a quadratic in one of
// them for no gain over fifty halvings of a number already in hand. A
// window that cannot be made that wide at all is the whole axis, which is
// a zoom of one.
double logZoomCeiling(double full, double held, double fraction, double minimumSpan, double base,
                      double maxZoom)
{
    if (!(full > 0.0) || !(minimumSpan > 0.0)) {
        return maxZoom;
    }
    const auto width = [&](double span) {
        return std::pow(base, held + (1.0 - fraction) * span) -
               std::pow(base, held - fraction * span);
    };
    if (!(width(full) > minimumSpan)) {
        return 1.0;
    }
    double narrow = 0.0;
    double wide = full;
    for (int i = 0; i < 60; ++i) {
        const double mid = (narrow + wide) / 2.0;
        if (width(mid) >= minimumSpan) {
            wide = mid;
        }
        else {
            narrow = mid;
        }
    }
    return std::max(1.0, full / wide);
}

// Zoom one axis by `factor` while holding the value at `fraction` of the
// visible span still -- which is what makes the wheel zoom into whatever
// the pointer is over rather than into the middle of the frame.
//
// Returns the new { zoom, pan } for the caller to assign, because QML has
// no out-parameters and two of these run per wheel tick.
//
// In positions rather than in values, which is the whole of what makes
// this work on either scale: "hold the value under the pointer still"
// is "hold its position still", and a position is a decade count on a
// logarithmic axis.
//
// The ceiling where the pointer is, on a logarithmic axis that has
// one -- see minimumSpanX -- and the flat one everywhere else.
//
// Never *out* past where the reader already is, though: a ceiling
// that fell below the zoom in hand -- the pointer moved to where the
// elements are sparser -- is not a reason to throw them back out.
AxisZoom zoomedAxis(AxisZoom view, double low, double high, double fraction, double factor,
                    bool logarithmic, double base, double minimumSpan, double maxZoom, double keep)
{
    const double from = axisPosition(low, logarithmic, base);
    const double to = axisPosition(high, logarithmic, base);
    const double full = to - from;
    const double span = full / view.zoom;
    const double held = (from + to) / 2.0 + view.pan - span / 2.0 + fraction * span;
    const double ceiling = logarithmic && minimumSpan > 0.0
                               ? logZoomCeiling(full, held, fraction, minimumSpan, base, maxZoom)
                               : maxZoom;
    const double next = std::max(1.0, std::min(std::max(ceiling, view.zoom), view.zoom * factor));
    const double nextSpan = full / next;
    const double centre = held - fraction * nextSpan + nextSpan / 2.0;
    return {next, clampPan(centre - (from + to) / 2.0, next, low, high, logarithmic, base, keep)};
}

// The zoom and pan that put `from`..`to` on an axis running `low`..`high`.
//
// The inverse of visibleLow/visibleHigh, which is what makes it exact:
// asked for the window those two report, it gives back the zoom and pan
// they were computed from. Clamped the way every other path here is --
// never below 1, never past maxZoom, never so far off the data that less
// than panKeep of it is on screen -- so a window nobody can be shown comes
// back as the nearest one that can be.
//
// Held about the middle of what was asked for, which is where a stated
// window is centred. See zoomedAxis for the ceiling itself.
AxisZoom viewedAxis(double low, double high, double from, double to, bool logarithmic, double base,
                    double minimumSpan, double maxZoom, double keep)
{
    const double axisFrom = axisPosition(low, logarithmic, base);
    const double axisTo = axisPosition(high, logarithmic, base);
    const double wantFrom = axisPosition(from, logarithmic, base);
    const double wantTo = axisPosition(to, logarithmic, base);
    const double full = axisTo - axisFrom;
    const double span = std::abs(wantTo - wantFrom);
    if (!(full > 0.0) || !(span > 0.0)) {
        return {1.0, 0.0};
    }
    const double centre = (wantFrom + wantTo) / 2.0;
    const double ceiling = logarithmic && minimumSpan > 0.0
                               ? logZoomCeiling(full, centre, 0.5, minimumSpan, base, maxZoom)
                               : maxZoom;
    const double zoom = std::max(1.0, std::min(ceiling, full / span));
    return {zoom,
            clampPan(centre - (axisFrom + axisTo) / 2.0, zoom, low, high, logarithmic, base, keep)};
}

// `from`..`to` cut down to the part of a logarithmic axis that exists.
//
// A reader can type a zero into the four boxes under Plot Settings, and a
// saved view can carry a window from before the scale was turned on;
// neither is a window on this axis, because there is no place at or below
// zero for it to reach.
//
// Clipped to the axis's own low end rather than refused, which is what
// matplotlib does with a limit it cannot use: what was asked for is
// mostly reachable, and the part that is not is not there to be shown.
// Without it such a window resolved to a logarithm of minus infinity, an
// infinite span, and a zoom of one -- so asking to look at 0 to 100 on an
// axis running to a million showed the whole million, which is the
// opposite of what was asked.
//
// A window with nothing above zero in it at all is the whole axis, which
// is what a request to look at nothing resolves to everywhere else here.
Span positiveSpan(double from, double to, double low, double high, bool logarithmic)
{
    if (!logarithmic) {
        return {from, to};
    }
    if (!(to > 0.0)) {
        return {low, high};
    }
    return {std::max(from, low), to};
}

double gridPositiveMin(double low, double high, double step)
{
    if (!(high > 0.0)) {
        return 0.0;
    }
    if (low > 0.0) {
        return low;
    }
    const double stride = std::abs(step);
    if (!(stride > 0.0)) {
        return high;
    }
    const double at = low + (std::floor(-low / stride) + 1.0) * stride;
    return at > 0.0 && at <= high ? at : high;
}

Span xBounds(double low, double high, double positiveMin, bool logarithmic, double base)
{
    if (!logarithmic) {
        return {low, high};
    }
    if (!(positiveMin > 0.0) || !(high > positiveMin)) {
        return {1.0, base};
    }
    return {positiveMin, high};
}

double maxZoomFor(long long length)
{
    return std::max(256.0, static_cast<double>(length) / 16.0);
}

double CameraAxis::viewLow() const
{
    return visibleLow(bounds.low, bounds.high, view.zoom, view.pan, logarithmic, base);
}

double CameraAxis::viewHigh() const
{
    return visibleHigh(bounds.low, bounds.high, view.zoom, view.pan, logarithmic, base);
}

double CameraAxis::fullSpan() const
{
    return axisPosition(bounds.high, logarithmic, base) -
           axisPosition(bounds.low, logarithmic, base);
}

double CameraAxis::viewSpan() const
{
    return fullSpan() / view.zoom;
}

bool Camera::zoomed() const
{
    return x.view.zoom != 1.0 || y.view.zoom != 1.0 || x.view.pan != 0.0 || y.view.pan != 0.0;
}

void Camera::reset()
{
    x.view = {};
    y.view = {};
}

// Zoom about (px, py) by `factor`, on the axes `axes` names: "x", "y" or
// "both" -- the *pane's* x and y, across it and up it, so that Shift
// stretches what runs across the pane whichever way round it is drawn.
//
// One axis at a time is what the modifiers ask for, and it is not a
// convenience. A plot of a long trace is read by stretching time without
// changing what an amplitude is worth, and a plot of a narrow band is read
// the other way round; a zoom that always takes both makes either of those
// a zoom followed by a correcting pan, done by eye. Shift for x and Ctrl
// for y, which is the pair every other plot in the field uses.
//
// y grows downward on screen and upward on the axis.
//
// Which data axis runs which way. On a flipped pane x runs up it, so
// the pointer's height is where it is along x, and a request for the
// pane's x is a request for data y.
//
// Where the pointer is, in the x the axis prints, before the zoom
// moves anything -- which is the value zoomedAxis() is about to
// hold still under it.
//
// This is the one thing the surface has always known and never
// said. Only the *resulting* range crossed into the plot object, so
// the runs it read ahead of a zoom were centred on the middle of
// the frame, and a reader zooming into one corner walked off them
// after a step or two and waited for the file each time. Told where
// the zoom is going, it reads that way instead -- and reads at once
// rather than after the gesture stops, because an inward run costs
// half the span of the one above it.
//
// Through valueAlong rather than by interpolating the two bounds,
// which is the same distinction: on a logarithmic axis the value
// halfway across the pane is the geometric mean of its ends and
// not the arithmetic one, and a focus taken the other way would
// send the read towards a place the pointer is nowhere near.
std::optional<GestureFocus> Camera::zoomAt(double px, double py, double factor, ZoomAxes axes)
{
    if (area.width <= 0.0 || area.height <= 0.0) {
        return std::nullopt;
    }
    const double across = std::max(0.0, std::min(1.0, (px - area.x) / area.width));
    const double up = 1.0 - std::max(0.0, std::min(1.0, (py - area.y) / area.height));
    const double fx = flipped ? up : across;
    const double fy = flipped ? across : up;
    ZoomAxes which = axes;
    if (flipped) {
        which = axes == ZoomAxes::X ? ZoomAxes::Y : axes == ZoomAxes::Y ? ZoomAxes::X : axes;
    }
    std::optional<GestureFocus> focus;
    if (which != ZoomAxes::Y) {
        focus = GestureFocus{valueAlong(viewMinX(), viewMaxX(), fx, x.logarithmic, x.base), factor};
        x.view = zoomedAxis(x.view, x.bounds.low, x.bounds.high, fx, factor, x.logarithmic, x.base,
                            x.minimumSpan, maxZoom, panKeep);
    }
    if (which != ZoomAxes::X) {
        y.view = zoomedAxis(y.view, y.bounds.low, y.bounds.high, fy, factor, y.logarithmic, y.base,
                            y.minimumSpan, maxZoom, panKeep);
    }
    return focus;
}

// Drag the view by a pointer movement. The content follows the pointer,
// so the axis moves the other way.
//
// A drag is not a zoom and has nowhere it is heading, so what is read
// ahead goes back to being measured from the run on screen. It also
// stops the read going out on every frame of the drag: only a focus
// skips the settle.
//
// In positions, so that a drag of so many pixels moves the view by the
// share of the axis those pixels are -- which is the same sentence on
// either scale and is a count of decades on one of them.
//
// The share of each axis the drag moved along it. Upright, x runs to
// the right and y up; flipped, x runs up and y to the right -- and up
// is minus dy either way, because the pane counts downwards.
void Camera::panBy(double dx, double dy)
{
    if (area.width <= 0.0 || area.height <= 0.0) {
        return;
    }
    const double spanX = x.viewSpan();
    const double spanY = y.viewSpan();
    const double alongX = flipped ? -dy / area.height : dx / area.width;
    const double alongY = flipped ? dx / area.width : -dy / area.height;
    x.view.pan = clampPan(x.view.pan - alongX * spanX, x.view.zoom, x.bounds.low, x.bounds.high,
                          x.logarithmic, x.base, panKeep);
    y.view.pan = clampPan(y.view.pan - alongY * spanY, y.view.zoom, y.bounds.low, y.bounds.high,
                          y.logarithmic, y.base, panKeep);
}

// Put the window at exactly these four numbers, in data coordinates.
//
// The one way in for everything that states a window rather than nudging
// one: the region band, and the four boxes under Plot Settings > View.
// Both of those are the reader saying where to look, which is the same
// thing said twice -- so it is arithmetic in one place and the boxes
// report the band's answer without either knowing about the other.
void Camera::setViewRange(double x0, double x1, double y0, double y1)
{
    const Span wantX = positiveSpan(std::min(x0, x1), std::max(x0, x1), x.bounds.low, x.bounds.high,
                                    x.logarithmic);
    const Span wantY = positiveSpan(std::min(y0, y1), std::max(y0, y1), y.bounds.low, y.bounds.high,
                                    y.logarithmic);
    x.view = viewedAxis(x.bounds.low, x.bounds.high, wantX.low, wantX.high, x.logarithmic, x.base,
                        x.minimumSpan, maxZoom, panKeep);
    y.view = viewedAxis(y.bounds.low, y.bounds.high, wantY.low, wantY.high, y.logarithmic, y.base,
                        y.minimumSpan, maxZoom, panKeep);
}

// What a pixel on the pane stands for, on each axis.
//
// Out here rather than inside zoomToRegion because the band's own readout
// needs exactly these two answers: a number written beside a rectangle
// and the window that rectangle resolves to must be one reading of one
// pixel, or the numbers are a promise the zoom does not keep. See
// PlotBand, which asks rather than reimplements.
//
// Clamped to the pane, because a drag that runs off the edge says "to the
// edge" -- which is what the band draws and what the zoom then takes.
//
// Each is read off the coordinate its axis runs along, which is the first
// argument while the pane is upright and the second once it is flipped:
// x up the pane, y across it. The second may be left off by anything that
// never asks about a flipped pane.
double Camera::dataXAt(double px, double py) const
{
    if (flipped) {
        if (area.height <= 0.0) {
            return viewMinX();
        }
        const double up = std::max(0.0, std::min(1.0, (py - area.y) / area.height));
        return valueAlong(viewMinX(), viewMaxX(), 1.0 - up, x.logarithmic, x.base);
    }
    if (area.width <= 0.0) {
        return viewMinX();
    }
    const double at = std::max(0.0, std::min(1.0, (px - area.x) / area.width));
    return valueAlong(viewMinX(), viewMaxX(), at, x.logarithmic, x.base);
}

// The same, down the other axis. y grows downward on screen and upward on
// the axis, so the top of the pane is the larger value.
double Camera::dataYAt(double py, double px) const
{
    if (flipped) {
        if (area.width <= 0.0) {
            return viewMinY();
        }
        const double across = std::max(0.0, std::min(1.0, (px - area.x) / area.width));
        return valueAlong(viewMinY(), viewMaxY(), across, y.logarithmic, y.base);
    }
    if (area.height <= 0.0) {
        return viewMinY();
    }
    const double at = std::max(0.0, std::min(1.0, (py - area.y) / area.height));
    return valueAlong(viewMinY(), viewMaxY(), 1.0 - at, y.logarithmic, y.base);
}

// Go to the region a right-drag has just drawn, in this item's pixels.
//
// Two things happen here and the order of them is the whole feature. The
// focus is pushed *first*, with the magnification the move amounts to, so
// that the closer look the model reads is read towards the region and
// goes out in the same turn rather than after the settle -- a stated
// region is not a gesture that might carry on, it is a reader who has
// already said where they are going. Then the window moves, and the
// bindings under it push the range.
//
// A band under a few pixels on a side it is about to take is a slip rather
// than a request: a two-pixel-tall window is a magnification of several
// hundred on an axis the reader never meant to touch, and the way out of
// it would be a reset. Refused whole. Which sides are taken is the wheel's
// choice: Shift the direction across the pane, Control the one up it, and
// either both of those or neither is both. A side that is not taken is the
// whole pane, so its length is not a slip and its window is left where it
// was. On a flipped pane the request is swapped the way zoomAt swaps it, so
// Shift is still across the pane whichever data axis that is.
//
// The band's top edge is the axis's larger value -- see dataYAt -- and
// its right edge too; on a flipped pane those are x's and y's the
// other way round, which dataXAt and dataYAt know.
//
// The middle of the band and the magnification it amounts to, both
// measured the way the axis measures: on a logarithmic one the middle
// of the band is the geometric mean of its ends, and a band a decade
// wide on a pane of six is six times in however many units that
// decade holds. Taken the linear way, a band over the first decades of
// a wide axis sent the read to the far end of it.
bool Camera::zoomToRegion(double px0, double py0, double px1, double py1, ZoomAxes axes,
                          std::optional<GestureFocus>* focus)
{
    if (area.width <= 0.0 || area.height <= 0.0) {
        return false;
    }
    const double left = std::min(px0, px1);
    const double right = std::max(px0, px1);
    const double top = std::min(py0, py1);
    const double bottom = std::max(py0, py1);
    const bool across = axes != ZoomAxes::Y;
    const bool up = axes != ZoomAxes::X;
    if ((across && right - left < minimumBand) || (up && bottom - top < minimumBand)) {
        return false;
    }
    // The pane's directions, named before the flip. Afterwards, which data
    // axis that is: across the pane is data y once x runs up it.
    ZoomAxes which = axes;
    if (flipped) {
        which = axes == ZoomAxes::X ? ZoomAxes::Y : axes == ZoomAxes::Y ? ZoomAxes::X : axes;
    }
    const double x0 = dataXAt(left, bottom);
    const double x1 = dataXAt(right, top);
    const double y0 = dataYAt(bottom, left);
    const double y1 = dataYAt(top, right);
    if (which != ZoomAxes::Y) {
        const double span = axisPosition(viewMaxX(), x.logarithmic, x.base) -
                            axisPosition(viewMinX(), x.logarithmic, x.base);
        const double band =
            axisPosition(x1, x.logarithmic, x.base) - axisPosition(x0, x.logarithmic, x.base);
        if (focus != nullptr && span > 0.0 && band > 0.0) {
            *focus = GestureFocus{valueAlong(x0, x1, 0.5, x.logarithmic, x.base), span / band};
        }
        const Span want = positiveSpan(std::min(x0, x1), std::max(x0, x1), x.bounds.low,
                                       x.bounds.high, x.logarithmic);
        x.view = viewedAxis(x.bounds.low, x.bounds.high, want.low, want.high, x.logarithmic, x.base,
                            x.minimumSpan, maxZoom, panKeep);
    }
    if (which != ZoomAxes::X) {
        const Span want = positiveSpan(std::min(y0, y1), std::max(y0, y1), y.bounds.low,
                                       y.bounds.high, y.logarithmic);
        y.view = viewedAxis(y.bounds.low, y.bounds.high, want.low, want.high, y.logarithmic, y.base,
                            y.minimumSpan, maxZoom, panKeep);
    }
    return true;
}

} // namespace plotcore
