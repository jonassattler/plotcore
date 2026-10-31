// SPDX-FileCopyrightText: 2026 Jonas Sattler
// SPDX-FileCopyrightText: 2026 Andreas Martin
// SPDX-License-Identifier: BSD-3-Clause

#include "plotcore/Figure.hpp"

#include "Json.hpp"
#include "plotcore/Format.hpp"
#include "plotcore/Projection.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

// The frame is PlotFrame.qml's, the gestures PlotSurface.qml's; where this
// file lays something out it names the property of the QML it follows. The
// numbers are FigureStyle's, which are Theme's.

namespace plotcore {
namespace {

constexpr double kInfinity = std::numeric_limits<double>::infinity();

/// What a pane `pixels` device pixels across is read against: down to the
/// quantum, as PaneColumns rounds it, and within what a line may be thinned to.
// Tell it how wide the pane is, in columns.
//
// A bucket is a column: what a line is thinned to is a property of the
// pane it is drawn in and not a constant, and the number used to be a
// constant in both directions -- fewer buckets than pixels on a wide
// screen, which draws an envelope as a hatch of separated teeth instead of
// a band, and more points than anyone can tell apart on a narrow one.
//
// The frame rather than the plot area inside it, which is not a rounding:
// the area's left gutter is measured off the widest y tick label, that
// label is measured off the extent of the data, and the extent moves when
// the resolution does -- a finer bucket can find a more extreme value. So
// a resolution taken from the area is a resolution that decides the gutter
// that decides the resolution, which is exactly the binding loop Qt
// reported the first time this was written that way. The frame's own width
// depends on nothing the plot draws.
//
// It overestimates by the gutters, which is what the renderer's tolerance
// for a few points per column is for: a column is two stations or a chord.
// Quantised on the other side, so dragging the window's edge does not
// re-read the file once a pixel.
//
// In *device* pixels, which is the other half of "a bucket is a column".
// A pane is laid out in logical pixels and drawn into a framebuffer with
// devicePixelRatio of them for each one, so thinning to the logical width
// on a HiDPI screen hands the renderer one bucket per two or three
// physical columns -- a band drawn at half or a third of the resolution
// the display has, which is the whole of what oversampling would have
// bought and is free to ask for correctly instead.
//
// Along x, which on a flipped pane is up it: the frame's height, less the
// title's line, which is as independent of what the plot draws as the
// width's gutter is.
int columnsOf(double pixels)
{
    const int columns = static_cast<int>(std::max(pixels, 0.0));
    return std::clamp((columns / kColumnQuantum) * kColumnQuantum, kMinPoints / 2, kMaxPoints / 2);
}

const char* gridName(GridMode mode)
{
    switch (mode) {
    case GridMode::None:
        return "none";
    case GridMode::Dense:
        return "dense";
    case GridMode::Custom:
        return "custom";
    case GridMode::Loose:
        break;
    }
    return "loose";
}

GridMode gridOf(const std::string& name)
{
    if (name == "none") {
        return GridMode::None;
    }
    if (name == "dense") {
        return GridMode::Dense;
    }
    if (name == "custom") {
        return GridMode::Custom;
    }
    return GridMode::Loose;
}

const char* cornerName(Corner corner)
{
    switch (corner) {
    case Corner::TopLeft:
        return "top-left";
    case Corner::BottomLeft:
        return "bottom-left";
    case Corner::BottomRight:
        return "bottom-right";
    case Corner::TopRight:
        break;
    }
    return "top-right";
}

Corner cornerOf(const std::string& name)
{
    if (name == "top-left") {
        return Corner::TopLeft;
    }
    if (name == "bottom-left") {
        return Corner::BottomLeft;
    }
    if (name == "bottom-right") {
        return Corner::BottomRight;
    }
    return Corner::TopRight;
}

/// A triangle strip, as strokeRunInto() emits it, as a list of triangles.
void trianglesOf(const std::vector<Point>& strip, std::vector<Point>& out)
{
    for (std::size_t i = 0; i + 2 < strip.size(); ++i) {
        out.push_back(strip[i]);
        out.push_back(strip[i + 1]);
        out.push_back(strip[i + 2]);
    }
}

} // namespace

// --- the style ----------------------------------------------------------------

double FigureStyle::pixelsOf(TextRole role) const
{
    switch (role) {
    case TextRole::TickMinor:
        return tickMinorPixels;
    case TextRole::Title:
        return titlePixels;
    case TextRole::AxisName:
        return axisNamePixels;
    case TextRole::Legend:
        return legendPixels;
    case TextRole::Note:
        return notePixels;
    case TextRole::Tick:
        break;
    }
    return tickPixels;
}

TextMeasure FigureStyle::approximateMeasure() const
{
    const FigureStyle style = *this;
    TextMeasure measure;
    measure.width = [style](const std::string& text, TextRole role) {
        return 0.6 * style.pixelsOf(role) * textLength(text);
    };
    measure.height = [style](TextRole role) { return 1.2 * style.pixelsOf(role); };
    return measure;
}

// --- the scene ------------------------------------------------------------------

std::vector<TextItem> Scene::texts() const
{
    std::vector<TextItem> found;
    for (const Mark& mark : over) {
        if (mark.kind == Mark::Kind::Text) {
            found.push_back(mark.text);
        }
    }
    return found;
}

std::vector<Mark> Scene::marks(Mark::Kind kind, bool underLines) const
{
    std::vector<Mark> found;
    for (const Mark& mark : underLines ? under : over) {
        if (mark.kind == kind) {
            found.push_back(mark);
        }
    }
    return found;
}

// --- one line's own y ---------------------------------------------------------

/// The window a line is drawn in, when it is not the common one: in the
/// values, and on which scale.
struct Figure::LineWindow
{
    bool own = false;
    /// Whether it is numbered beside the pane: a line asked onto an axis of
    /// its own, or stacked. A moved or scaled line is not -- see Figure.hpp.
    bool axis = false;
    double low = 0.0;
    double high = 1.0;
    bool logarithmic = false;
    bool fixed = false;
    /// The window the line's scale and shift are measured against, in the
    /// axis's positions: the common axis's whole, the line's own padded
    /// extent, or its band.
    double baseLow = 0.0;
    double baseHigh = 1.0;
};

/// Everything the frame is laid out from.
struct Figure::Layout
{
    PaneRect area;
    TickSet leftTicks;
    TickSet footTicks;
    TickSet xTicks;
    TickSet yTicks;
    bool common = true;
    double tickHeight = 0.0;
    double minorHeight = 0.0;
    double nameHeight = 0.0;
    double titleHeight = 0.0;
    double sideWidth = 0.0;
    double commonWidth = 0.0;
    double footHeight = 0.0;

    struct Side
    {
        int line = -1;
        LineWindow window;
        Rgba colour;
        std::string label;
        TickSet ticks;
        double at = 0.0;
        double extent = 0.0;
        double widest = 0.0;
    };
    std::vector<Side> sides;
};

Figure::Figure()
{
    measure_ = style_.approximateMeasure();
}

void Figure::ensureLine(int line)
{
    if (line >= 0 && static_cast<std::size_t>(line) >= lines_.size()) {
        lines_.resize(static_cast<std::size_t>(line) + 1);
    }
}

LineSettings Figure::lineSettings(int line) const
{
    if (line < 0 || static_cast<std::size_t>(line) >= lines_.size()) {
        return {};
    }
    return lines_[static_cast<std::size_t>(line)];
}

void Figure::setLineSettings(int line, const LineSettings& settings)
{
    if (line < 0) {
        return;
    }
    ensureLine(line);
    LineSettings clean = settings;
    if (!(clean.scale > 0.0) || !std::isfinite(clean.scale)) {
        clean.scale = 1.0;
    }
    if (!std::isfinite(clean.shift)) {
        clean.shift = 0.0;
    }
    lines_[static_cast<std::size_t>(line)] = clean;
}

void Figure::setSettings(const FigureSettings& settings)
{
    FigureSettings next = settings;
    if (!usableBase(next.xLogBase)) {
        next.xLogBase = settings_.xLogBase;
    }
    if (!usableBase(next.yLogBase)) {
        next.yLogBase = settings_.yLogBase;
    }
    const bool reset = next.xLog != settings_.xLog || next.yLog != settings_.yLog ||
                       next.flipped != settings_.flipped ||
                       (next.xLog && next.xLogBase != settings_.xLogBase) ||
                       (next.yLog && next.yLogBase != settings_.yLogBase);
    settings_ = next;
    if (reset) {
        resetView();
    }
}

void Figure::setStyle(const FigureStyle& style)
{
    style_ = style;
    if (approximate_) {
        // A measure that needs no font is the style's own, so it follows it.
        measure_ = style_.approximateMeasure();
    }
}

void Figure::setMeasure(TextMeasure measure)
{
    approximate_ = !(measure.width && measure.height);
    measure_ = approximate_ ? style_.approximateMeasure() : std::move(measure);
}

void Figure::setSize(double width, double height, double pixelRatio)
{
    width_ = std::max(width, 1.0);
    height_ = std::max(height, 1.0);
    pixelRatio_ = pixelRatio > 0.0 ? pixelRatio : 1.0;
}

void Figure::resetView()
{
    camera_.reset();
    // The whole of every line, which is where nothing is heading.
    source().clearFocus();
}

void Figure::setSource(LineSource* source)
{
    source_ = source;
    // A drag holds a line by its number, and the numbers are the old source's.
    drag_ = Drag::None;
    banding_ = false;
    dragLine_ = -1;
}

void Figure::setViewRange(double x0, double x1, double y0, double y1)
{
    refresh();
    camera_.area = layout().area;
    camera_.setViewRange(x0, x1, y0, y1);
}

void Figure::removeLine(int line)
{
    if (source_ == nullptr) {
        plot_.removeLine(line);
    }
    if (static_cast<std::size_t>(line) < lines_.size()) {
        lines_.erase(lines_.begin() + line);
    }
    if (settings_.highlighted == line) {
        settings_.highlighted = -1;
    }
    else if (settings_.highlighted > line) {
        --settings_.highlighted;
    }
    if (dragLine_ == line) {
        drag_ = Drag::None;
        banding_ = false;
        dragLine_ = -1;
    }
    else if (dragLine_ > line) {
        --dragLine_;
    }
}

void Figure::clear()
{
    plot_.clear();
    lines_.clear();
    settings_.highlighted = -1;
    drag_ = Drag::None;
    banding_ = false;
    dragLine_ = -1;
    camera_.reset();
}

int Figure::positionOf(int line) const
{
    for (std::size_t k = 0; k < drawn_.size(); ++k) {
        if (drawn_[k].line == line) {
            return static_cast<int>(k);
        }
    }
    return -1;
}

bool Figure::commonLine(int position) const
{
    // A line on an axis of its own needs a second line to be beside: the
    // request is kept, so a second line brings the axis back. See H5Scope's CLAUDE.md.
    if (drawn_.size() < 2) {
        return true;
    }
    const LineSettings own = lineSettings(drawn_[static_cast<std::size_t>(position)].line);
    return !(own.ownAxis || settings_.stacked || own.scale != 1.0 || own.shift != 0.0);
}

bool Figure::spansCommon(int position) const
{
    // A moved or scaled line is still a reading on the common axis's units --
    // its window is a share of that axis's whole -- so it stays in the whole.
    // Taking it out would move the whole under it, and the line with it, on
    // the gesture that was meant to hold the value under the pointer still.
    if (drawn_.size() < 2) {
        return true;
    }
    return !(lineSettings(drawn_[static_cast<std::size_t>(position)].line).ownAxis ||
             settings_.stacked);
}

void Figure::refresh()
{
    const LineSource& lines = source();
    drawn_.clear();
    long long longest = 1;
    const int count = lines.lineCount();
    for (int i = 0; i < count; ++i) {
        if (!lineSettings(i).visible || !lines.ready(i)) {
            continue;
        }
        const long long length = lines.length(i);
        if (length <= 0) {
            continue;
        }
        longest = std::max(longest, length);
        drawn_.push_back(Drawn{i, lines.extent(i), lines.xExtent(i), lines.xPositiveMinimum(i)});
    }

    double xMin = kInfinity;
    double xMax = -kInfinity;
    double xPositive = 0.0;
    double yMin = kInfinity;
    double yMax = -kInfinity;
    double yPositive = 0.0;
    bool anyCommon = false;
    for (std::size_t k = 0; k < drawn_.size(); ++k) {
        const Drawn& line = drawn_[k];
        xMin = std::min(xMin, line.x.low);
        xMax = std::max(xMax, line.x.high);
        if (line.xPositive > 0.0 && (xPositive == 0.0 || line.xPositive < xPositive)) {
            xPositive = line.xPositive;
        }
        if (!spansCommon(static_cast<int>(k))) {
            continue;
        }
        const LineExtent& extent = line.extent;
        if (extent.finite) {
            yMin = std::min(yMin, extent.minimum);
            yMax = std::max(yMax, extent.maximum);
            anyCommon = true;
        }
        if (extent.positive && (yPositive == 0.0 || extent.positiveMinimum < yPositive)) {
            yPositive = extent.positiveMinimum;
        }
    }
    if (!(xMax >= xMin)) {
        xMin = 0.0;
        xMax = 1.0;
    }
    if (xMax == xMin) {
        // One x has no span to be an axis over; the unit either side of it is
        // the honest window, as `padded` gives a flat line one.
        xMin -= 0.5;
        xMax += 0.5;
    }

    camera_.flipped = settings_.flipped;
    camera_.maxZoom = maxZoomFor(longest);
    camera_.x.logarithmic = settings_.xLog;
    camera_.x.base = settings_.xLogBase;
    camera_.x.bounds = xBounds(xMin, xMax, xPositive, settings_.xLog, settings_.xLogBase);
    camera_.x.minimumSpan = std::abs(xMax - xMin) / camera_.maxZoom;

    camera_.y.logarithmic = settings_.yLog;
    camera_.y.base = settings_.yLogBase;
    if (!anyCommon) {
        // No line on the common axis: a nominal one for the gestures to run on,
        // which the frame numbers, names and rules none of.
        camera_.y.bounds = settings_.yLog ? Span{1.0, settings_.yLogBase} : Span{0.0, 1.0};
    }
    else {
        camera_.y.bounds =
            padded(settings_.yLog ? yPositive : yMin, yMax, settings_.yLog, settings_.yLogBase);
    }
    camera_.y.minimumSpan = 0.0;
}

// Where the window on the common axis sits within the whole of it, as
// a fraction from the bottom -- in the axis's own positions, so on a
// logarithmic common axis a zoom into the top decade is a zoom into the
// top of every separate axis too.
double Figure::commonFraction(double value) const
{
    const CameraAxis& y = camera_.y;
    const double from = axisPosition(y.bounds.low, y.logarithmic, y.base);
    const double to = axisPosition(y.bounds.high, y.logarithmic, y.base);
    if (!(to > from)) {
        return 0.0;
    }
    return (axisPosition(value, y.logarithmic, y.base) - from) / (to - from);
}

Figure::LineWindow Figure::windowOf(int position, bool always) const
{
    LineWindow window;
    if (!always && commonLine(position)) {
        return window;
    }
    const Drawn& drawnLine = drawn_[static_cast<std::size_t>(position)];
    const LineSettings own = lineSettings(drawnLine.line);
    const bool yLog = settings_.yLog;
    const double base = settings_.yLogBase;
    const LineExtent& extent = drawnLine.extent;
    const auto drawn = static_cast<double>(drawn_.size());

    double low = 0.0;
    double high = 1.0;
    if (own.ownAxis && !settings_.stacked) {
        // The line alone, and linear: the axis it would have if it were the
        // only one on the plot.
        const Span whole =
            extent.finite ? padded(extent.minimum, extent.maximum, false, 10.0) : Span{0.0, 1.0};
        window.logarithmic = false;
        low = whole.low;
        high = whole.high;
    }
    else if (settings_.stacked) {
        // Its own extent, on the common scale, in an equal band of the pane:
        // the first line drawn at the top.
        Span whole = Span{0.0, 1.0};
        if (yLog && extent.positive) {
            whole = padded(extent.positiveMinimum, extent.maximum, true, base);
        }
        else if (!yLog && extent.finite) {
            whole = padded(extent.minimum, extent.maximum, false, base);
        }
        else if (yLog) {
            whole = Span{1.0, base};
        }
        window.logarithmic = yLog;
        const double from = axisPosition(whole.low, yLog, base);
        const double to = axisPosition(whole.high, yLog, base);
        const double bandLow = 1.0 - static_cast<double>(position + 1) / drawn;
        const double bandHigh = 1.0 - static_cast<double>(position) / drawn;
        const double span = (to - from) / (bandHigh - bandLow);
        low = axisValue(from - bandLow * span, yLog, base);
        high = axisValue(from - bandLow * span + span, yLog, base);
    }
    else {
        // Moved or scaled on the common axis: a share of the common window,
        // on its scale.
        window.logarithmic = yLog;
        low = camera_.y.bounds.low;
        high = camera_.y.bounds.high;
    }

    const bool log = window.logarithmic;
    window.baseLow = axisPosition(low, log, base);
    window.baseHigh = axisPosition(high, log, base);
    const double centre = (window.baseLow + window.baseHigh) / 2.0;
    const double half = (window.baseHigh - window.baseLow) / 2.0 / own.scale;
    const double homeLow = centre - half + own.shift;
    const double homeSpan = 2.0 * half;
    window.fixed = own.axisFixed;
    const double bottom = window.fixed ? 0.0 : commonFraction(camera_.viewMinY());
    const double top = window.fixed ? 1.0 : commonFraction(camera_.viewMaxY());
    window.own = true;
    window.axis = own.ownAxis || settings_.stacked;
    window.low = axisValue(homeLow + bottom * homeSpan, log, base);
    window.high = axisValue(homeLow + top * homeSpan, log, base);
    return window;
}

// --- the frame ----------------------------------------------------------------

Figure::Layout Figure::layout() const
{
    Layout lay;
    const FigureStyle& s = style_;
    TickStyle ticks;
    ticks.target = s.tickTarget;
    ticks.fontPixels = s.tickPixels;
    ticks.denseFactor = s.denseFactor;

    // Numbered while any line is a reading on it -- a moved one included,
    // whose window is a share of the common one's units.
    lay.common = drawn_.empty();
    for (std::size_t k = 0; k < drawn_.size() && !lay.common; ++k) {
        lay.common = spansCommon(static_cast<int>(k));
    }
    lay.tickHeight = measure_.height(TextRole::Tick);
    lay.minorHeight = measure_.height(TextRole::TickMinor);
    lay.nameHeight = measure_.height(TextRole::AxisName);
    lay.titleHeight = measure_.height(TextRole::Title);

    const bool flipped = settings_.flipped;
    const bool leftNumbered = flipped || lay.common;
    const bool footNumbered = !flipped || lay.common;
    const std::string& leftName = flipped ? settings_.xLabel : lay.common ? settings_.yLabel : "";
    const std::string& footName = !flipped ? settings_.xLabel : lay.common ? settings_.yLabel : "";

    // The side axes: every line drawn on a window of its own.
    const auto count = static_cast<int>(drawn_.size());
    for (int k = 0; k < count; ++k) {
        const LineWindow window = windowOf(k);
        if (!window.own || !window.axis) {
            continue;
        }
        const int line = drawn_[static_cast<std::size_t>(k)].line;
        Layout::Side side;
        side.line = line;
        side.window = window;
        side.colour = seriesColor(settings_.colours, source().series(line), k, count,
                                  settings_.paper, settings_.dark, lineSettings(line).colour);
        side.label = lineSettings(line).axisLabel;
        lay.sides.push_back(side);
    }

    // PlotFrame.gutterTop, footCommonHeight, sideHeight and gutterBottom --
    // none of which depends on a number up the side, which is what lets the
    // height be settled before the left gutter is.
    const double top =
        s.gapS + (settings_.title.empty() ? 0.0 : std::ceil(lay.titleHeight) + s.gapS);
    lay.footHeight = (footNumbered ? std::ceil(lay.tickHeight) + s.gapS : s.gapS) +
                     (footName.empty() ? 0.0 : std::ceil(lay.nameHeight) + s.gapXS);
    double sideHeight = 0.0;
    if (flipped && !lay.sides.empty()) {
        const double numbers = s.accentWidth + s.s3 + std::ceil(lay.tickHeight);
        double at = 0.0;
        for (Layout::Side& side : lay.sides) {
            side.at = at;
            side.extent =
                numbers + (side.label.empty() ? 0.0 : s.gapXS + std::ceil(lay.nameHeight));
            at += side.extent + s.gapM;
        }
        sideHeight = std::ceil(lay.sides.back().at + lay.sides.back().extent + s.gapS);
    }
    const double areaHeight = std::max(1.0, height_ - top - lay.footHeight - sideHeight);

    // The numbers up the side, measured up the pane -- PlotFrame.xValuesUp and
    // yValuesUp, which are what keep the left gutter out of a cycle with them.
    const CameraAxis& x = camera_.x;
    const CameraAxis& y = camera_.y;
    if (flipped) {
        lay.xTicks =
            tickValues(x.viewLow(), x.viewHigh(), x.logarithmic, x.base, areaHeight, true, ticks);
        lay.leftTicks = lay.xTicks;
    }
    else if (lay.common) {
        lay.yTicks =
            tickValues(y.viewLow(), y.viewHigh(), y.logarithmic, y.base, areaHeight, true, ticks);
        lay.leftTicks = lay.yTicks;
    }

    // Each side column, from the outside in: its name, its numbers, a gap, a
    // tick and a rule (PlotFrame.sideColumns).
    double sideWidth = 0.0;
    if (!flipped && !lay.sides.empty()) {
        double at = 0.0;
        for (Layout::Side& side : lay.sides) {
            side.ticks = tickValues(side.window.low, side.window.high, side.window.logarithmic,
                                    y.base, areaHeight, true, ticks);
            for (const TickLabel& label : side.ticks.texts) {
                side.widest = std::max(
                    side.widest, measure_.width(label.base, TextRole::Tick) +
                                     (label.exponent.empty()
                                          ? 0.0
                                          : measure_.width(label.exponent, TextRole::TickMinor)));
            }
            side.at = at;
            side.extent = (side.label.empty() ? 0.0 : std::ceil(lay.nameHeight) + s.gapXS) +
                          std::ceil(side.widest) + s.gapS + s.s3 + s.accentWidth;
            at += side.extent + s.gapM;
        }
        sideWidth = std::ceil(lay.sides.back().at + lay.sides.back().extent + s.gapM);
    }
    lay.sideWidth = sideWidth;

    double widest = 0.0;
    for (const TickLabel& label : lay.leftTicks.texts) {
        widest = std::max(widest, measure_.width(label.base, TextRole::Tick) +
                                      (label.exponent.empty()
                                           ? 0.0
                                           : measure_.width(label.exponent, TextRole::TickMinor)));
    }
    lay.commonWidth = !leftNumbered
                          ? s.gapS
                          : std::max(s.labelMargin, std::ceil(widest) + s.gapM) +
                                (leftName.empty() ? 0.0 : std::ceil(lay.nameHeight) + s.gapXS);
    const double left = sideWidth + lay.commonWidth;
    const double right = s.s9;
    lay.area = PaneRect{left, top, std::max(1.0, width_ - left - right), areaHeight};

    // The numbers along the foot, measured across it.
    if (flipped) {
        if (lay.common) {
            lay.yTicks = tickValues(y.viewLow(), y.viewHigh(), y.logarithmic, y.base,
                                    lay.area.width, false, ticks);
        }
        lay.footTicks = lay.yTicks;
        for (Layout::Side& side : lay.sides) {
            side.ticks = tickValues(side.window.low, side.window.high, side.window.logarithmic,
                                    y.base, lay.area.width, false, ticks);
        }
    }
    else {
        lay.xTicks = tickValues(x.viewLow(), x.viewHigh(), x.logarithmic, x.base, lay.area.width,
                                false, ticks);
        lay.footTicks = lay.xTicks;
    }
    return lay;
}

PlotView Figure::paneView(const Layout& lay) const
{
    PlotView pane;
    pane.xMin = camera_.viewMinX();
    pane.xMax = camera_.viewMaxX();
    pane.yMin = camera_.viewMinY();
    pane.yMax = camera_.viewMaxY();
    pane.xLog = settings_.xLog;
    pane.yLog = settings_.yLog;
    pane.xLogBase = settings_.xLogBase;
    pane.yLogBase = settings_.yLogBase;
    pane.width = lay.area.width;
    pane.height = lay.area.height;
    pane.pixelRatio = pixelRatio_;
    pane.transposed = settings_.flipped;
    pane.maxColumns = maxColumnsFor(static_cast<int>(drawn_.size()));
    return pane;
}

std::string Figure::reason() const
{
    // PlotSurface.logReason: asked of what the axes were offered, not of what
    // they settled on, because both settle on a nominal power.
    if (drawn_.empty()) {
        return {};
    }
    bool commonPositive = false;
    bool anyCommon = false;
    double positiveX = 0.0;
    double highestX = -kInfinity;
    for (std::size_t k = 0; k < drawn_.size(); ++k) {
        const Drawn& line = drawn_[k];
        highestX = std::max(highestX, line.x.high);
        if (line.xPositive > 0.0 && (positiveX == 0.0 || line.xPositive < positiveX)) {
            positiveX = line.xPositive;
        }
        if (spansCommon(static_cast<int>(k))) {
            anyCommon = true;
            commonPositive = commonPositive || (line.extent.finite && line.extent.maximum > 0.0);
        }
    }
    if (settings_.yLog && anyCommon && !commonPositive) {
        return "Nothing here is above zero, and a logarithmic y axis has no place to draw a "
               "value that is not.";
    }
    if (settings_.xLog && !(positiveX > 0.0 && highestX > positiveX)) {
        return "No x here is above zero, and a logarithmic x axis has no place to draw a value "
               "that is not.";
    }
    return {};
}

Scene Figure::scene(const SceneRequest& request)
{
    refresh();
    // A picture is laid out at its own size and in its own scope, and leaves
    // the pane's as they were: the figure goes on being the pane's, and the
    // next gesture is measured against the pane and not against the picture.
    struct Held
    {
        Figure& figure;
        double width;
        double height;
        double pixelRatio;
        bool paper;
        PaneRect area;
        ~Held()
        {
            figure.width_ = width;
            figure.height_ = height;
            figure.pixelRatio_ = pixelRatio;
            figure.settings_.paper = paper;
            figure.camera_.area = area;
        }
    };
    Held held{*this, width_, height_, pixelRatio_, settings_.paper, camera_.area};
    if (request.picture) {
        width_ = request.width > 0.0 ? request.width : width_;
        height_ = request.height > 0.0 ? request.height : height_;
        pixelRatio_ = request.pixelRatio > 0.0 ? request.pixelRatio : pixelRatio_;
    }
    settings_.paper = settings_.paper || request.paper;
    Scene composed = compose(request);
    if (!request.picture) {
        // The pane's own layout, which the gestures are measured against.
        held.area = camera_.area;
    }
    return composed;
}

Scene Figure::compose(const SceneRequest& request)
{
    const Layout lay = layout();
    camera_.area = lay.area;
    const FigureStyle& s = style_;
    const Chrome chrome = chromeFor(settings_.dark, settings_.paper);
    const SceneOptions& want = request.geometry;

    Scene scene;
    scene.width = width_;
    scene.height = height_;
    scene.ground = chrome.ground;
    if (request.transparent) {
        scene.ground.alpha = 0.0;
    }
    scene.chrome = chrome;
    scene.area = lay.area;
    scene.x = {camera_.viewMinX(), camera_.viewMaxX()};
    scene.y = {camera_.viewMinY(), camera_.viewMaxY()};
    scene.message = reason();

    // --- the lines ---------------------------------------------------------
    const PlotView pane = paneView(lay);
    const PlotView upright = uprightView(pane);
    if (!request.picture) {
        // The news, for a source that folds or reads by it. A picture asks for
        // nothing: it is drawn from what the lines hold for the pane.
        const double columns = (settings_.flipped ? lay.area.height : lay.area.width) * pixelRatio_;
        source().setPaneColumns(columnsOf(columns));
        source().setView(camera_.viewMinX(), camera_.viewMaxX(), settings_.xLog);
    }

    const auto count = static_cast<int>(drawn_.size());
    std::vector<PlotLine> projected;
    std::vector<PlotAxis> axes;
    std::vector<LineWindow> windows;
    for (int k = 0; k < count; ++k) {
        const int index = drawn_[static_cast<std::size_t>(k)].line;
        const int series = source().series(index);
        PlotLine line = source().drawLine(index);
        const Rgba colour = seriesColor(settings_.colours, series, k, count, settings_.paper,
                                        settings_.dark, lineSettings(index).colour);
        line.colour = colour;
        // A line on paper is drawn solid, whatever is highlighted and however
        // many there are -- PlotPicture's one deliberate departure from the
        // light theme it otherwise draws in. On screen a bundle separates by
        // piling up; on a page a line drawn at part strength is a line
        // somebody will complain about (Theme.paperSeriesOpacity).
        line.opacity = settings_.paper ? 1.0
                                       : seriesOpacity(series, count, settings_.highlighted,
                                                       settings_.bundle, settings_.dark);
        line.width = seriesWidth(series, settings_.highlighted, s.lineWidth);
        const LineWindow window = windowOf(k);
        if (window.own) {
            line.ownY = true;
            line.yMin = window.low;
            line.yMax = window.high;
            line.ownYLog = window.logarithmic;
        }
        const PlotAxis axis = source().drawAxis(index);
        projected.push_back(line);
        axes.push_back(axis);
        windows.push_back(window);

        std::vector<Point> points;
        std::vector<PlotRun> runs;
        const PlotView view = lineView(line, upright);
        const PlotProjected result = projectLine(line, axis, view, points, runs);
        // The way back to the values, through the view that projected them,
        // before the points are turned onto a flipped pane.
        const double xFrom = axisPosition(view.xMin, view.xLog, view.xLogBase);
        const double xTo = axisPosition(view.xMax, view.xLog, view.xLogBase);
        const double yFrom = axisPosition(view.yMin, view.yLog, view.yLogBase);
        const double yTo = axisPosition(view.yMax, view.yLog, view.yLogBase);
        const auto valueOf = [&](const Point& p) {
            const double across = view.width > 0.0 ? p.x() / view.width : 0.0;
            const double up = view.height > 0.0 ? 1.0 - p.y() / view.height : 0.0;
            return Point(axisValue(xFrom + across * (xTo - xFrom), view.xLog, view.xLogBase),
                         axisValue(yFrom + up * (yTo - yFrom), view.yLog, view.yLogBase));
        };
        std::vector<Point> values;
        if (want.values) {
            values.reserve(points.size());
            for (const Point& point : points) {
                values.push_back(valueOf(point));
            }
        }
        if (pane.transposed) {
            for (Point& point : points) {
                point = transposedPoint(point, pane);
            }
        }
        DrawnLine drawnLine;
        drawnLine.line = index;
        Rgba ink = colour;
        ink.alpha *= line.opacity;
        drawnLine.colour = ink;
        drawnLine.width = line.width;
        drawnLine.markerRadius = s.markerSize / 2.0;
        drawnLine.y = Span{view.yMin, view.yMax};
        drawnLine.yLogarithmic = view.yLog;
        drawnLine.own = window.own;
        drawnLine.numbered = window.own && window.axis;
        const bool marks = settings_.markers && !line.summarised && !result.decimated;
        for (const PlotRun& run : runs) {
            std::vector<Point> stroke;
            stroke.reserve(static_cast<std::size_t>(run.count));
            for (int i = 0; i < run.count; ++i) {
                const Point& p = points[static_cast<std::size_t>(run.first + i)];
                stroke.emplace_back(p.x() + lay.area.x, p.y() + lay.area.y);
            }
            if (want.triangles || want.strips) {
                std::vector<Point> strip;
                strokeRunInto(stroke.data(), static_cast<int>(stroke.size()), line.width,
                              [&](double px, double py) { strip.emplace_back(px, py); });
                if (want.triangles) {
                    trianglesOf(strip, drawnLine.triangles);
                }
                if (want.strips) {
                    drawnLine.strips.push_back(std::move(strip));
                }
            }
            if (marks) {
                drawnLine.markers.insert(drawnLine.markers.end(), stroke.begin(), stroke.end());
            }
            if (want.runs) {
                drawnLine.runs.push_back(std::move(stroke));
            }
            if (want.values) {
                drawnLine.values.emplace_back(values.begin() + run.first,
                                              values.begin() + run.first + run.count);
            }
        }
        scene.lines.push_back(std::move(drawnLine));
        scene.legend.push_back(LegendEntry{index, source().name(index), colour, {}, {}, {}});
    }

    // --- what is painted, as marks -----------------------------------------------
    // Positions are whole pixels wherever the viewer's frame rounds them
    // (Math.round, which is jsRound), and every rule is a filled hairline
    // rather than a segment: a box of one pixel is one pixel in every toolkit,
    // and a stroke one pixel wide is two half-covered rows in some of them.
    const auto written = [&](TextItem item, Mark::Layer layer = Mark::Layer::Frame,
                             bool clipped = false) {
        Mark mark;
        mark.kind = Mark::Kind::Text;
        mark.layer = layer;
        mark.clipped = clipped;
        item.raise = s.exponentRaise;
        mark.from = item.at;
        mark.text = std::move(item);
        return mark;
    };
    const auto boxed = [](const PaneRect& rect, Rgba fill, Rgba edge, double width,
                          Mark::Layer layer, double radius = 0.0, bool clipped = false) {
        Mark mark;
        mark.kind = Mark::Kind::Box;
        mark.layer = layer;
        mark.clipped = clipped;
        mark.from = Point(rect.x, rect.y);
        mark.to = Point(rect.x + rect.width, rect.y + rect.height);
        mark.fill = fill;
        mark.stroke = edge;
        mark.width = width;
        mark.radius = radius;
        return mark;
    };
    const Rgba clear = Rgba::of(0.0, 0.0, 0.0, 0.0);
    const auto filled = [&](double x, double y, double width, double height, Rgba colour,
                            Mark::Layer layer = Mark::Layer::Frame, bool clipped = false) {
        return boxed(PaneRect{x, y, width, height}, colour, clear, 0.0, layer, 0.0, clipped);
    };
    const double hairline = s.ruleWidth;

    // --- the rules, under everything ----------------------------------------
    const PaneRect& a = lay.area;
    const auto alongX = [&](double fraction) { return jsRound(a.x + fraction * a.width); };
    const auto upY = [&](double fraction) { return jsRound(a.y + (1.0 - fraction) * a.height); };
    const CameraAxis& cx = camera_.x;
    const CameraAxis& cy = camera_.y;
    TickStyle tickStyle;
    tickStyle.target = s.tickTarget;
    tickStyle.fontPixels = s.tickPixels;
    tickStyle.denseFactor = s.denseFactor;
    const std::vector<double> xGrid = gridValues(settings_.grid, cx.viewLow(), cx.viewHigh(),
                                                 settings_.gridStepX, lay.xTicks, tickStyle);
    const std::vector<double> yGrid = lay.common
                                          ? gridValues(settings_.grid, cy.viewLow(), cy.viewHigh(),
                                                       settings_.gridStepY, lay.yTicks, tickStyle)
                                          : std::vector<double>{};
    // PlotFrame.acrossRules and downRules: a hairline the width or the height
    // of the pane, at the rule's place rounded to a pixel.
    const auto rule = [&](double fraction, bool ofX, bool major) {
        const Rgba colour = major ? chrome.ruleMajor : chrome.ruleMinor;
        if (ofX != settings_.flipped) {
            return filled(alongX(fraction), a.y, hairline, a.height, colour, Mark::Layer::Grid);
        }
        return filled(a.x, upY(fraction), a.width, hairline, colour, Mark::Layer::Grid);
    };
    // What each common axis is numbered and ruled at, kept on the scene in
    // its own values as well as painted (SceneAxis).
    const auto commonAxis = [](const CameraAxis& axis, const TickSet& ticks) {
        SceneAxis out;
        out.window = {axis.viewLow(), axis.viewHigh()};
        out.logarithmic = axis.logarithmic;
        out.base = axis.base;
        out.ticks = ticks;
        return out;
    };
    scene.xAxis = commonAxis(cx, lay.xTicks);
    scene.yAxis = commonAxis(cy, lay.yTicks);
    scene.xAxis.grid = gridAt(settings_.grid, xGrid, lay.xTicks.values, cx.viewLow(), cx.viewHigh(),
                              cx.logarithmic, cx.base);
    scene.yAxis.grid = gridAt(settings_.grid, yGrid, lay.yTicks.values, cy.viewLow(), cy.viewHigh(),
                              cy.logarithmic, cy.base);
    for (const Rule& found : scene.xAxis.grid) {
        if (found.at >= 0.0 && found.at <= 1.0) {
            scene.under.push_back(rule(found.at, true, found.major));
        }
    }
    for (const Rule& found : scene.yAxis.grid) {
        if (found.at >= 0.0 && found.at <= 1.0) {
            scene.under.push_back(rule(found.at, false, found.major));
        }
    }

    // --- the frame ------------------------------------------------------------------
    // All four sides, a hairline wider and taller than the pane: the left rule
    // on the pane's first column and the bottom one on the row under its last
    // -- PlotFrame's bordered rectangle, which is what gives the pane an edge
    // a trace leaving the window stops at.
    const Rgba axisColour = chrome.axisRule;
    scene.over.push_back(filled(a.x, a.y, a.width + hairline, hairline, axisColour));
    scene.over.push_back(filled(a.x, a.y + a.height, a.width + hairline, hairline, axisColour));
    scene.over.push_back(filled(a.x, a.y, hairline, a.height + hairline, axisColour));
    scene.over.push_back(filled(a.x + a.width, a.y, hairline, a.height + hairline, axisColour));

    // --- the numbers on the common axes ------------------------------------------
    // Beside the pane with a gap and no tick: the grid is what a number is
    // read against, and a tick under a rule would be the rule drawn twice.
    const bool leftOfX = settings_.flipped;
    const auto leftFraction = [&](double value) {
        return leftOfX ? fractionOn(value, cx.viewLow(), cx.viewHigh(), cx.logarithmic, cx.base)
                       : fractionOn(value, cy.viewLow(), cy.viewHigh(), cy.logarithmic, cy.base);
    };
    const auto footFraction = [&](double value) {
        return leftOfX ? fractionOn(value, cy.viewLow(), cy.viewHigh(), cy.logarithmic, cy.base)
                       : fractionOn(value, cx.viewLow(), cx.viewHigh(), cx.logarithmic, cx.base);
    };
    const auto addText = [&](const TickLabel& label, Point at, TextItem::Align across,
                             TextItem::Align down, TextRole role, Rgba colour) {
        TextItem item;
        item.text = label.text;
        item.base = label.base;
        item.exponent = label.exponent;
        item.at = at;
        item.across = across;
        item.down = down;
        item.role = role;
        item.colour = colour;
        scene.over.push_back(written(std::move(item)));
    };
    const auto addName = [&](const std::string& text, Point at, TextItem::Align down, double room,
                             double turn, Rgba colour) {
        TextItem name;
        name.text = name.base = elided(text, room, TextRole::AxisName);
        name.at = at;
        name.across = TextItem::Align::Middle;
        name.down = down;
        name.role = TextRole::AxisName;
        name.colour = colour;
        name.turn = turn;
        scene.over.push_back(written(std::move(name)));
    };
    const double bottom = a.y + a.height;
    for (std::size_t i = 0; i < lay.leftTicks.values.size(); ++i) {
        const double f = leftFraction(lay.leftTicks.values[i]);
        if (f < -1e-9 || f > 1.0 + 1e-9) {
            continue;
        }
        addText(lay.leftTicks.texts[i], Point(a.x - s.gapS, upY(f)), TextItem::Align::End,
                TextItem::Align::Middle, TextRole::Tick, chrome.ink);
    }
    const bool footNumbered = !settings_.flipped || lay.common;
    if (footNumbered) {
        for (std::size_t i = 0; i < lay.footTicks.values.size(); ++i) {
            const double f = footFraction(lay.footTicks.values[i]);
            if (f < -1e-9 || f > 1.0 + 1e-9) {
                continue;
            }
            addText(lay.footTicks.texts[i], Point(alongX(f), bottom + s.s3),
                    TextItem::Align::Middle, TextItem::Align::Start, TextRole::Tick, chrome.ink);
        }
    }

    // The digits between two numbers, in the smallest face (minorNumbersOf).
    const double minorWidth = measure_.width("0", TextRole::TickMinor);
    const auto minors = [&](const std::vector<double>& grid, const TickSet& found,
                            const CameraAxis& axis, bool up, double extent) {
        return minorNumbersOf(settings_.minorNumbers, settings_.grid, grid, found, axis.viewLow(),
                              axis.viewHigh(), axis.base, extent,
                              up ? std::ceil(lay.minorHeight) : s.gapS, up ? 0.0 : minorWidth);
    };
    // By axis rather than by side, so that the scene can say which are x's:
    // the axis numbered up the left is y's, or x's when the plot is flipped.
    scene.xAxis.minor =
        minors(xGrid, lay.xTicks, cx, settings_.flipped, settings_.flipped ? a.height : a.width);
    scene.yAxis.minor =
        minors(yGrid, lay.yTicks, cy, !settings_.flipped, settings_.flipped ? a.width : a.height);
    const std::vector<MinorNumber>& leftMinor =
        settings_.flipped ? scene.xAxis.minor : scene.yAxis.minor;
    const std::vector<MinorNumber>& footMinor =
        settings_.flipped ? scene.yAxis.minor : scene.xAxis.minor;
    if (settings_.flipped || lay.common) {
        for (const MinorNumber& number : leftMinor) {
            addText(TickLabel{number.text, "", number.text}, Point(a.x - s.gapS, upY(number.at)),
                    TextItem::Align::End, TextItem::Align::Middle, TextRole::TickMinor, chrome.ink);
        }
    }
    if (footNumbered) {
        for (const MinorNumber& number : footMinor) {
            addText(TickLabel{number.text, "", number.text},
                    Point(alongX(number.at), bottom + s.s3), TextItem::Align::Middle,
                    TextItem::Align::Start, TextRole::TickMinor, chrome.ink);
        }
    }

    // The names and the title, each cut to the side it names.
    const std::string& leftName = settings_.flipped ? settings_.xLabel
                                  : lay.common      ? settings_.yLabel
                                                    : std::string();
    const std::string& footName = !settings_.flipped ? settings_.xLabel
                                  : lay.common       ? settings_.yLabel
                                                     : std::string();
    if (!leftName.empty()) {
        addName(leftName,
                Point(lay.sideWidth + std::ceil(lay.nameHeight) / 2.0, a.y + a.height / 2.0),
                TextItem::Align::Middle, a.height, -90.0, chrome.ink);
    }
    if (!footName.empty()) {
        addName(footName,
                Point(a.x + a.width / 2.0,
                      bottom + (footNumbered ? s.s3 + std::ceil(lay.tickHeight) : 0.0) + s.gapXS),
                TextItem::Align::Start, a.width, 0.0, chrome.ink);
    }
    if (!settings_.title.empty()) {
        TextItem title;
        title.text = title.base = elided(settings_.title, a.width, TextRole::Title);
        title.at = Point(a.x + a.width / 2.0, s.gapS);
        title.across = TextItem::Align::Middle;
        title.down = TextItem::Align::Start;
        title.role = TextRole::Title;
        title.colour = chrome.ink;
        scene.over.push_back(written(std::move(title)));
    }

    // --- the axes of their own ------------------------------------------------
    // In the colour of the line each is the axis of, which is the whole of how
    // a reader tells which axis a line is read against; the rule at the
    // accent's weight, because a hairline of colour on black is a colour
    // nobody can name.
    for (const Layout::Side& side : lay.sides) {
        const double base = cy.base;
        SceneAxis own;
        own.line = side.line;
        own.window = {side.window.low, side.window.high};
        own.logarithmic = side.window.logarithmic;
        own.base = base;
        own.ticks = side.ticks;
        own.colour = side.colour;
        own.label = side.label;
        own.box = settings_.flipped
                      ? PaneRect{a.x, bottom + lay.footHeight + side.at, a.width, side.extent}
                      : PaneRect{side.at, a.y, side.extent, a.height};
        scene.sideAxes.push_back(std::move(own));
        if (!settings_.flipped) {
            // A column: its name, its numbers, a gap, a tick and a rule, right
            // to left from the rule.
            const double right = side.at + side.extent;
            scene.over.push_back(filled(right - s.accentWidth, a.y, s.accentWidth,
                                        a.height + hairline, side.colour));
            for (std::size_t i = 0; i < side.ticks.values.size(); ++i) {
                const double f = fractionOn(side.ticks.values[i], side.window.low, side.window.high,
                                            side.window.logarithmic, base);
                if (f < -1e-9 || f > 1.0 + 1e-9) {
                    continue;
                }
                const double at = a.y + jsRound((1.0 - f) * a.height);
                const double tick = right - s.accentWidth - s.s3;
                scene.over.push_back(filled(tick, at, s.s3, hairline, side.colour));
                addText(side.ticks.texts[i], Point(tick - s.gapS, at), TextItem::Align::End,
                        TextItem::Align::Middle, TextRole::Tick, side.colour);
            }
            if (!side.label.empty()) {
                addName(side.label,
                        Point(side.at + std::ceil(lay.nameHeight) / 2.0, a.y + a.height / 2.0),
                        TextItem::Align::Middle, a.height, -90.0, side.colour);
            }
            continue;
        }
        // A row under the foot: a rule, a tick, its numbers and its name, top
        // to bottom.
        const double top = bottom + lay.footHeight + side.at;
        scene.over.push_back(filled(a.x, top, a.width + hairline, s.accentWidth, side.colour));
        for (std::size_t i = 0; i < side.ticks.values.size(); ++i) {
            const double f = fractionOn(side.ticks.values[i], side.window.low, side.window.high,
                                        side.window.logarithmic, base);
            if (f < -1e-9 || f > 1.0 + 1e-9) {
                continue;
            }
            const double at = a.x + jsRound(f * a.width);
            scene.over.push_back(filled(at, top + s.accentWidth, hairline, s.s3, side.colour));
            addText(side.ticks.texts[i], Point(at, top + s.accentWidth + s.s3),
                    TextItem::Align::Middle, TextItem::Align::Start, TextRole::Tick, side.colour);
        }
        if (!side.label.empty()) {
            addName(side.label,
                    Point(a.x + a.width / 2.0,
                          top + s.accentWidth + s.s3 + std::ceil(lay.tickHeight) + s.gapXS),
                    TextItem::Align::Start, a.width, 0.0, side.colour);
        }
    }

    // --- the reading under the pointer -----------------------------------------
    // Which line, which sample, and where it was drawn in the pane: taken from
    // the pointer on the pane, or named outright for a picture nobody is
    // pointing at.
    //
    // Off while a band is being drawn (PlotSurface.selecting), and only then:
    // what the reader is doing during a band is choosing an interval, the band
    // writes that interval out, and a second readout snapped to whichever
    // sample is nearest would be a second answer to a question nobody asked
    // twice. A pan is not that, and reads as it goes.
    struct Taken
    {
        int position = -1;
        double x = 0.0;
        double y = 0.0;
        double px = 0.0;
        double py = 0.0;
    };
    std::optional<Taken> taken;
    if (!request.picture && settings_.cursor && pointer_.has_value() && !banding_ &&
        scene.message.empty()) {
        const double px = pointer_->x() - a.x;
        const double py = pointer_->y() - a.y;
        if (px >= 0.0 && py >= 0.0 && px <= a.width && py <= a.height && !projected.empty()) {
            double bestDistance = kInfinity;
            // One line at a time, each with its own x axis -- a line may carry
            // a time of its own -- and the nearest of all of them taken.
            for (std::size_t k = 0; k < projected.size(); ++k) {
                const std::optional<NearestSample> found =
                    nearestSample(std::span<const PlotLine>(&projected[k], 1),
                                  std::span<const PlotAxis>(&axes[k], 1), pane, px, py);
                if (!found.has_value()) {
                    continue;
                }
                // A sample drawn off the pane is a clip neighbour, kept so the
                // stroke reaches the edge, and not something on screen to read.
                const double along = settings_.flipped ? found->py : found->px;
                if (along < 0.0 || along > (settings_.flipped ? a.height : a.width)) {
                    continue;
                }
                const double distance =
                    (found->px - px) * (found->px - px) + (found->py - py) * (found->py - py);
                if (distance < bestDistance) {
                    bestDistance = distance;
                    taken = Taken{static_cast<int>(k), found->x, found->y, found->px, found->py};
                }
            }
        }
    }
    else if (request.reading.has_value() && scene.message.empty()) {
        // PlotFrame.placeReading: the sample is what crosses over, not the
        // place it was drawn, and where it lands is this layout's own
        // arithmetic -- through the line's own axis when it has one, which is
        // the only place its curve is.
        const PinnedSample& sample = *request.reading;
        const int position = positionOf(sample.line);
        if (position >= 0) {
            const LineWindow& window = windows[static_cast<std::size_t>(position)];
            const double along =
                fractionOn(sample.x, cx.viewLow(), cx.viewHigh(), cx.logarithmic, cx.base);
            const double up =
                window.own
                    ? fractionOn(sample.y, window.low, window.high, window.logarithmic, cy.base)
                    : fractionOn(sample.y, cy.viewLow(), cy.viewHigh(), cy.logarithmic, cy.base);
            const bool flipped = settings_.flipped;
            // The same device-pixel centre the stroke was drawn on, so the
            // crosshair sits on the sample rather than half a pixel beside it.
            const double ratio = std::max(pixelRatio_, 1.0);
            const auto centre = [&](double px) {
                return (std::floor(px * ratio) + 0.5) / ratio;
            };
            double px = (flipped ? up : along) * a.width;
            double py = a.height - (flipped ? along : up) * a.height;
            if (flipped) {
                py = centre(py);
            }
            else {
                px = centre(px);
            }
            taken = Taken{position, sample.x, sample.y, px, py};
        }
    }
    if (taken.has_value()) {
        Reading& reading = scene.reading;
        reading.valid = true;
        reading.line = drawn_[static_cast<std::size_t>(taken->position)].line;
        reading.x = taken->x;
        reading.y = taken->y;
        reading.at = Point(a.x + taken->px, a.y + taken->py);
        reading.xText = readingText(taken->x);
        reading.yText = readingText(taken->y);
        // Clipped to the pane, which answers every way the sample can be off
        // it at once: the rule through an x on screen stays, and the rule and
        // the ring at a y above the pane go, because what they mark is not
        // there to be marked.
        scene.over.push_back(filled(a.x, a.y + jsRound(taken->py), a.width, hairline, axisColour,
                                    Mark::Layer::Overlay, true));
        scene.over.push_back(filled(a.x + jsRound(taken->px), a.y, hairline, a.height, axisColour,
                                    Mark::Layer::Overlay, true));
        // The sample itself, ringed in the accent so the reader can see which
        // one was taken: as wide as a marker and its ring, the ring inside.
        Mark ring;
        ring.kind = Mark::Kind::Circle;
        ring.layer = Mark::Layer::Overlay;
        ring.clipped = true;
        ring.from = reading.at;
        ring.radius = (s.markerSize + s.accentWidth) / 2.0 - s.accentWidth / 2.0;
        ring.stroke = chrome.cursorInk;
        ring.width = s.accentWidth;
        scene.over.push_back(ring);
    }

    // --- the legend on the plot ------------------------------------------------
    // PlotOverlayLegend: a caption in a corner, part of the picture that
    // leaves -- so it names the lines rather than merely showing that there
    // are some. It names the first plotLegendRows of them and counts the
    // rest: a legend ten thousand rows tall is the picture replaced by a list
    // of what is in it. No wider than half the pane, with a name too long for
    // it cut in the middle, where a path keeps its two ends.
    if (settings_.legend && !scene.legend.empty()) {
        const auto named = std::min<std::size_t>(
            scene.legend.size(), static_cast<std::size_t>(std::max(s.legendRows, 0)));
        const std::size_t unnamed = scene.legend.size() - named;
        std::string longest;
        for (std::size_t i = 0; i < named; ++i) {
            if (textLength(scene.legend[i].name) > textLength(longest)) {
                longest = scene.legend[i].name;
            }
        }
        const double row = std::ceil(measure_.height(TextRole::Legend)) + s.gapXS;
        const double wanted = s.gapM * 2.0 + s.legendSwatch + s.gapS +
                              std::ceil(measure_.width(longest, TextRole::Legend));
        const double width = std::min(wanted, std::max(0.0, a.width * 0.5));
        const double rows = static_cast<double>(named) + (unnamed > 0 ? 1.0 : 0.0);
        const double height = row * rows + s.gapS * 2.0;
        const bool atLeft = settings_.legendCorner == Corner::TopLeft ||
                            settings_.legendCorner == Corner::BottomLeft;
        const bool atTop =
            settings_.legendCorner == Corner::TopLeft || settings_.legendCorner == Corner::TopRight;
        scene.legendRect =
            PaneRect{atLeft ? a.x + s.gapM : a.x + a.width - width - s.gapM,
                     atTop ? a.y + s.gapM : a.y + a.height - height - s.gapM, width, height};
        const PaneRect& box = scene.legendRect;
        scene.over.push_back(
            boxed(box, chrome.veil, chrome.legendEdge, hairline, Mark::Layer::Frame, s.radius));
        const double left = box.x + s.gapM;
        const double room = width - s.gapM * 2.0 - s.legendSwatch - s.gapS;
        for (std::size_t i = 0; i < named; ++i) {
            LegendEntry& entry = scene.legend[i];
            const double middle = box.y + s.gapS + row * static_cast<double>(i) + row / 2.0;
            entry.from = Point(left, middle);
            entry.to = Point(left + s.legendSwatch, middle);
            entry.textAt = Point(left + s.legendSwatch + s.gapS, middle);
            // A stroke of line rather than a block of colour, because what it
            // stands for is a stroke.
            scene.over.push_back(
                boxed(PaneRect{left, middle - s.accentWidth / 2.0, s.legendSwatch, s.accentWidth},
                      entry.colour, clear, 0.0, Mark::Layer::Frame, s.radius));
            TextItem name;
            name.text = name.base = elided(entry.name, room, TextRole::Legend, true);
            name.at = entry.textAt;
            name.down = TextItem::Align::Middle;
            name.role = TextRole::Legend;
            name.colour = chrome.text;
            scene.over.push_back(written(std::move(name)));
        }
        scene.legend.resize(named);
        if (unnamed > 0) {
            TextItem more;
            more.text = more.base = "+" + std::to_string(unnamed) + " more";
            more.at = Point(left, box.y + s.gapS + row * static_cast<double>(named) + row / 2.0);
            more.down = TextItem::Align::Middle;
            more.role = TextRole::Note;
            more.colour = chrome.faint;
            scene.over.push_back(written(std::move(more)));
        }
    }
    else {
        scene.legend.clear();
    }

    // --- what the reading says, in the pane ------------------------------------
    // For a host with nowhere else to say it: the viewer prints it in the bar
    // under its plot and turns this off. On the veil the band's numbers stand
    // on, and in their face.
    if (scene.reading.valid && settings_.readout) {
        TextItem said;
        said.text = said.base = "x " + scene.reading.xText + "   y " + scene.reading.yText;
        said.role = TextRole::Tick;
        said.colour = chrome.text;
        const double width = std::ceil(measure_.width(said.text, TextRole::Tick)) + s.gapXS * 2.0;
        const double height = std::ceil(measure_.height(TextRole::Tick)) + s.gapXS * 2.0;
        const PaneRect ground{a.x + s.gapM, a.y + s.gapM, width, height};
        said.at = Point(ground.x + width / 2.0, ground.y + height / 2.0);
        said.across = TextItem::Align::Middle;
        said.down = TextItem::Align::Middle;
        scene.over.push_back(
            boxed(ground, chrome.veil, clear, 0.0, Mark::Layer::Overlay, s.radius, true));
        scene.over.push_back(written(std::move(said), Mark::Layer::Overlay, true));
    }

    // --- the band ------------------------------------------------------------------
    if (!request.picture && drag_ == Drag::Band && banding_) {
        // The band is clamped to the pane rather than merely clipped, because
        // its corners are what the numbers are read out of as well as what the
        // rectangle is drawn from: the band, the numbers beside it and the
        // window they get on release are one statement. A modifier takes one
        // direction, and that statement is then the pane's whole extent the
        // other way: the numbers name the edges the zoom will keep.
        scene.band = bandRectangle(a);
        scene.over.push_back(boxed(*scene.band, chrome.bandFill, chrome.bandEdge, s.accentWidth,
                                   Mark::Layer::Overlay, 0.0, true));
        if (settings_.bandReadout) {
            bandReadout(scene, lay);
        }
    }

    // --- why there is nothing to see ---------------------------------------------
    if (!scene.message.empty()) {
        scene.messageTitle = "nothing on this scale";
    }
    if (!scene.message.empty() && settings_.paneMessage) {
        const std::vector<std::string> lines =
            wrapped(scene.message, std::max(1.0, a.width - 2.0 * s.gapL), TextRole::AxisName);
        const double line = measure_.height(TextRole::AxisName);
        const double top = a.y + a.height / 2.0 - line * static_cast<double>(lines.size()) / 2.0;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            TextItem said;
            said.text = said.base = lines[i];
            said.at = Point(a.x + a.width / 2.0, top + line * static_cast<double>(i));
            said.across = TextItem::Align::Middle;
            said.role = TextRole::AxisName;
            said.colour = chrome.ink;
            scene.over.push_back(written(std::move(said), Mark::Layer::Overlay));
        }
    }
    return scene;
}

PaneRect Figure::bandRectangle(const PaneRect& area) const
{
    const double left = std::clamp(std::min(pressed_.x(), last_.x()), area.x, area.x + area.width);
    const double right = std::clamp(std::max(pressed_.x(), last_.x()), area.x, area.x + area.width);
    const double top = std::clamp(std::min(pressed_.y(), last_.y()), area.y, area.y + area.height);
    const double bottom =
        std::clamp(std::max(pressed_.y(), last_.y()), area.y, area.y + area.height);
    if (bandAxes_ == ZoomAxes::X) {
        return {left, area.y, right - left, area.height};
    }
    if (bandAxes_ == ZoomAxes::Y) {
        return {area.x, top, area.width, bottom - top};
    }
    return {left, top, right - left, bottom - top};
}

void Figure::bandReadout(Scene& scene, const Layout& lay) const
{
    // PlotBand.qml. The band on its own says *where* the reader is about to
    // look and nothing about *what* they are about to see. Somebody dragging
    // one over the second half of a pulse is choosing an interval, and the
    // interval is the thing they want written down: where the drag started,
    // where it has reached, and how wide and how tall it has become by now.
    // Those are the four numbers the axes are about to be set to. A modifier
    // takes one of those directions, the same choice the wheel makes. The
    // rectangle is then the pane's whole extent the other way, and the numbers
    // are that axis's two edges and the span between them: a second number
    // would name an axis the zoom is about to leave alone.
    //
    // They are asked of the camera rather than worked out again -- dataXAt
    // and dataYAt are what zoomToRegion resolves the band itself with. A
    // second copy of that mapping would be a readout that can disagree with
    // the window it names.
    //
    // Where the numbers go is the rest of this, and one rule carries it:
    // **nothing is drawn over the band, over the pane's edge, over another
    // number, or under the pointer.** A coordinate is drawn at the corner it
    // names, pushed away from the band -- and past the cursor where that push
    // would go under it, because the corner that follows the pointer is the
    // one the reader is covering with their own hand. A length is drawn
    // against its own edge only where there is room for one, and the two that
    // measure the sides read along them. A length that does not fit is left
    // out rather than squeezed in: the corners are the reading, the lengths
    // are the subtraction between them, and the subtraction is what a crowded
    // pane can afford to lose.
    const FigureStyle& s = style_;
    const Chrome chrome = chromeFor(settings_.dark, settings_.paper);
    const PaneRect& a = lay.area;
    const CameraAxis& cx = camera_.x;
    const CameraAxis& cy = camera_.y;

    // The band in the pane's own pixels, clamped as the zoom clamps.
    const auto paneX = [&](double px) { return std::clamp(px - a.x, 0.0, a.width); };
    const auto paneY = [&](double py) { return std::clamp(py - a.y, 0.0, a.height); };
    const double startX = paneX(pressed_.x());
    const double startY = paneY(pressed_.y());
    const double endX = paneX(last_.x());
    const double endY = paneY(last_.y());
    // The rectangle that is drawn, in the pane's own pixels. On one axis it
    // fills the pane the other way, and the numbers are kept off that.
    const PaneRect absolute = bandRectangle(a);
    const PaneRect bandRect{absolute.x - a.x, absolute.y - a.y, absolute.width, absolute.height};

    // ...and in the axes' own numbers. Both coordinates of each corner go to
    // both questions, because on a flipped pane x is read off the height.
    const double startValueX = camera_.dataXAt(pressed_.x(), pressed_.y());
    const double startValueY = camera_.dataYAt(pressed_.y(), pressed_.x());
    const double endValueX = camera_.dataXAt(last_.x(), last_.y());
    const double endValueY = camera_.dataYAt(last_.y(), last_.x());
    // Each number written by the axis it is on, which takes its decimals off
    // the span on screen: so these say what the ticks beneath them say, to
    // the same precision, and a reader zooming in is given digits as the
    // ticks close up.
    const auto xNumber = [&](double value) {
        return axisLabel(value, cx.viewLow(), cx.viewHigh(), lay.xTicks.logarithmic);
    };
    const auto yNumber = [&](double value) {
        return axisLabel(value, cy.viewLow(), cy.viewHigh(), lay.yTicks.logarithmic);
    };
    // A corner is two numbers and a comma -- only the x when there is no
    // common y axis, whose y is a different number on every line's own.
    const auto coordinate = [&](double x, double y) {
        return lay.common ? xNumber(x) + ", " + yNumber(y) : xNumber(x);
    };
    // A length carries U+2206 INCREMENT, so a lone number against an edge
    // does not read as one more coordinate -- and not U+0394, which the mono
    // face the viewer ships does not carry.
    const std::string increment = "\xE2\x88\x86";
    std::string startText = coordinate(startValueX, startValueY);
    std::string endText = coordinate(endValueX, endValueY);
    const std::string xSpan = increment + xNumber(std::abs(endValueX - startValueX));
    const std::string ySpan =
        lay.common ? increment + yNumber(std::abs(endValueY - startValueY)) : std::string();
    // The band's width is a length of whichever axis runs across the pane,
    // which on a flipped one is y.
    std::string widthText = settings_.flipped ? ySpan : xSpan;
    std::string heightText = settings_.flipped ? xSpan : ySpan;
    if (bandAxes_ == ZoomAxes::X) {
        startText = settings_.flipped ? yNumber(startValueY) : xNumber(startValueX);
        endText = settings_.flipped ? yNumber(endValueY) : xNumber(endValueX);
        heightText.clear();
    }
    else if (bandAxes_ == ZoomAxes::Y) {
        startText = settings_.flipped ? xNumber(startValueX) : yNumber(startValueY);
        endText = settings_.flipped ? xNumber(endValueX) : yNumber(endValueY);
        widthText.clear();
    }

    // The air between a number and whatever it is kept off: one distance for
    // the band, the pane's edge and another number, because they are one rule.
    const double gap = s.gapS;
    const auto boxFor = [&](const std::string& text) {
        return PaneRect{0.0, 0.0, std::ceil(measure_.width(text, TextRole::Tick)) + s.gapXS * 2.0,
                        std::ceil(measure_.height(TextRole::Tick)) + s.gapXS * 2.0};
    };
    // Clear of one another with the gap between them -- a little more than
    // not overlapping: two numbers a pixel apart read as one number twice as
    // long. Satisfied exactly at the gap.
    const auto clears = [&](const PaneRect& p, const PaneRect& q) {
        return p.x + p.width + gap <= q.x || q.x + q.width + gap <= p.x ||
               p.y + p.height + gap <= q.y || q.y + q.height + gap <= p.y;
    };
    const auto inside = [&](const PaneRect& box) {
        return box.x >= 0.0 && box.y >= 0.0 && box.x + box.width <= a.width &&
               box.y + box.height <= a.height;
    };
    // A cursor is drawn from its hotspot down and to the right, so a number
    // in that quadrant is one the reader is covering with what they are
    // pointing with -- the end corner, every time.
    const PaneRect pointer{endX, endY, s.pointerSize, s.pointerSize};
    const auto stands = [&](const PaneRect& box, std::initializer_list<PaneRect> avoid) {
        if (!inside(box)) {
            return false;
        }
        return std::all_of(avoid.begin(), avoid.end(),
                           [&](const PaneRect& other) { return clears(box, other); });
    };
    const auto clampTo = [](double value, double size, double extent) {
        return std::max(0.0, std::min(extent - size, value));
    };
    // A corner's number is pushed away from the band on both axes; where that
    // push is into the pointer's quadrant it is a pointer's width and the gap.
    // Four placements are tried in order and the first that stands clear is
    // taken: the preferred one, the same sliding the other way, and the two
    // that fold back over the corner -- which at the pane's edge is where one
    // of the pushes has room. A band drawn into the corner of the pane leaves
    // its own corner nowhere outside it to stand, and the number then stands
    // on the band, which is a veil and not a cover.
    const auto place = [&](double atX, double atY, double awayX, double awayY,
                           const PaneRect& size) {
        const double outX = awayX > 0.0 && awayY > 0.0 ? s.pointerSize + gap : gap;
        const double wantX = awayX < 0.0 ? atX - gap - size.width : atX + outX;
        const double wantY = awayY < 0.0 ? atY - gap - size.height : atY + gap;
        const double backX = awayX < 0.0 ? atX + gap : atX - gap - size.width;
        const double backY = awayY < 0.0 ? atY + gap : atY - gap - size.height;
        const auto at = [&](double x, double y) { return PaneRect{x, y, size.width, size.height}; };
        const PaneRect tries[] = {at(clampTo(wantX, size.width, a.width), wantY),
                                  at(wantX, clampTo(wantY, size.height, a.height)),
                                  at(clampTo(wantX, size.width, a.width), backY),
                                  at(backX, clampTo(wantY, size.height, a.height))};
        for (const PaneRect& tried : tries) {
            if (stands(tried, {bandRect, pointer})) {
                return tried;
            }
        }
        return at(clampTo(wantX, size.width, a.width), clampTo(wantY, size.height, a.height));
    };

    struct Placed
    {
        BandLabel label;
        PaneRect box;
    };
    std::vector<Placed> placed;
    // Diagonally opposite corners, so the two pushes are exact opposites --
    // which holds the two coordinates apart when the band is small enough
    // that they would otherwise meet. One axis has no diagonal: its two
    // edges are the two ends of the side being taken, and each number is
    // pushed off that side.
    PaneRect startBox;
    PaneRect endBox;
    if (bandAxes_ == ZoomAxes::X) {
        const bool startLeft = startX <= endX;
        startBox = place(startX, bandRect.y, startLeft ? -1.0 : 1.0, 1.0, boxFor(startText));
        endBox = place(endX, bandRect.y, startLeft ? 1.0 : -1.0, 1.0, boxFor(endText));
    }
    else if (bandAxes_ == ZoomAxes::Y) {
        const bool startAbove = startY <= endY;
        startBox = place(bandRect.x, startY, 1.0, startAbove ? -1.0 : 1.0, boxFor(startText));
        endBox = place(bandRect.x, endY, 1.0, startAbove ? 1.0 : -1.0, boxFor(endText));
    }
    else {
        const double runsRight = endX >= startX ? 1.0 : -1.0;
        const double runsDown = endY >= startY ? 1.0 : -1.0;
        startBox = place(startX, startY, -runsRight, -runsDown, boxFor(startText));
        endBox = place(endX, endY, runsRight, runsDown, boxFor(endText));
    }
    placed.push_back(Placed{BandLabel{"start", startText, {}, 0.0}, startBox});
    placed.push_back(Placed{BandLabel{"end", endText, {}, 0.0}, endBox});

    // The lengths, on both of the band's x lines and both of its y lines,
    // because a reader measuring an edge is already looking at it -- offered
    // only where one fits between the two corners it measures, and kept only
    // where it is inside the pane and clear of the corners, which are placed
    // first and never give way.
    std::vector<Placed> candidates;
    const PaneRect widthSize = boxFor(widthText);
    const PaneRect written = boxFor(heightText);
    // The height reads along the side it measures, turned the quarter the y
    // axis's name is turned by -- and the two turned opposite ways, so each
    // letter's top faces away from the band: a pair on either side of one
    // rectangle reads as one when each is the other reflected. Turned, it
    // costs a line's height beside the band and its length along it.
    const PaneRect heightSize{0.0, 0.0, written.height, written.width};
    const double midX = bandRect.x + bandRect.width / 2.0;
    const double midY = bandRect.y + bandRect.height / 2.0;
    if (!widthText.empty() && widthSize.width <= bandRect.width) {
        candidates.push_back(
            Placed{BandLabel{"widthAbove", widthText, {}, 0.0},
                   PaneRect{midX - widthSize.width / 2.0, bandRect.y - gap - widthSize.height,
                            widthSize.width, widthSize.height}});
        candidates.push_back(
            Placed{BandLabel{"widthBelow", widthText, {}, 0.0},
                   PaneRect{midX - widthSize.width / 2.0, bandRect.y + bandRect.height + gap,
                            widthSize.width, widthSize.height}});
    }
    if (!heightText.empty() && heightSize.height <= bandRect.height) {
        candidates.push_back(
            Placed{BandLabel{"heightLeft", heightText, {}, -90.0},
                   PaneRect{bandRect.x - gap - heightSize.width, midY - heightSize.height / 2.0,
                            heightSize.width, heightSize.height}});
        candidates.push_back(
            Placed{BandLabel{"heightRight", heightText, {}, 90.0},
                   PaneRect{bandRect.x + bandRect.width + gap, midY - heightSize.height / 2.0,
                            heightSize.width, heightSize.height}});
    }
    bool placedWidth = false;
    bool placedHeight = false;
    for (const Placed& candidate : candidates) {
        if (inside(candidate.box) && clears(candidate.box, startBox) &&
            clears(candidate.box, endBox)) {
            placedWidth = placedWidth || candidate.label.key.starts_with("width");
            placedHeight = placedHeight || candidate.label.key.starts_with("height");
            placed.push_back(candidate);
        }
    }
    // A one-axis band fills the pane the other way, so the span has no
    // outside to stand in. It stands on the band, which is a veil, where it
    // is still clear of the two edges and of the pointer.
    const auto onBand = [&](const std::string& key, const std::string& text, const PaneRect& spot,
                            double turn) {
        if (inside(spot) && clears(spot, startBox) && clears(spot, endBox) &&
            clears(spot, pointer)) {
            placed.push_back(Placed{BandLabel{key, text, {}, turn}, spot});
            return true;
        }
        return false;
    };
    if (bandAxes_ == ZoomAxes::X && !placedWidth && !widthText.empty() &&
        widthSize.width <= bandRect.width) {
        const PaneRect size = widthSize;
        const PaneRect along{midX - size.width / 2.0, bandRect.y + gap, size.width, size.height};
        const PaneRect mid{midX - size.width / 2.0, midY - size.height / 2.0, size.width,
                           size.height};
        if (!onBand("width", widthText, along, 0.0)) {
            onBand("width", widthText, mid, 0.0);
        }
    }
    if (bandAxes_ == ZoomAxes::Y && !placedHeight && !heightText.empty() &&
        heightSize.height <= bandRect.height) {
        const PaneRect beside{bandRect.x + gap, midY - heightSize.height / 2.0, heightSize.width,
                              heightSize.height};
        const PaneRect mid{midX - heightSize.width / 2.0, midY - heightSize.height / 2.0,
                           heightSize.width, heightSize.height};
        if (!onBand("height", heightText, beside, -90.0)) {
            onBand("height", heightText, mid, -90.0);
        }
    }

    // Drawn on the veil, in whole pixels: a line of ten-pixel type laid out
    // on a half pixel is a line of blurred type.
    for (Placed& one : placed) {
        BandLabel label = one.label;
        label.box = PaneRect{a.x + one.box.x, a.y + one.box.y, one.box.width, one.box.height};
        const PaneRect drawn{a.x + jsRound(one.box.x), a.y + jsRound(one.box.y), one.box.width,
                             one.box.height};
        Mark ground;
        ground.kind = Mark::Kind::Box;
        ground.layer = Mark::Layer::Overlay;
        ground.clipped = true;
        ground.from = Point(drawn.x, drawn.y);
        ground.to = Point(drawn.x + drawn.width, drawn.y + drawn.height);
        ground.fill = chrome.veil;
        ground.stroke = Rgba::of(0.0, 0.0, 0.0, 0.0);
        ground.radius = s.radius;
        scene.over.push_back(ground);
        Mark text;
        text.kind = Mark::Kind::Text;
        text.layer = Mark::Layer::Overlay;
        text.clipped = true;
        text.text.text = text.text.base = label.text;
        text.text.at = Point(drawn.x + drawn.width / 2.0, drawn.y + drawn.height / 2.0);
        text.text.across = TextItem::Align::Middle;
        text.text.down = TextItem::Align::Middle;
        text.text.role = TextRole::Tick;
        text.text.colour = chrome.text;
        text.text.turn = label.turn;
        text.text.raise = s.exponentRaise;
        text.from = text.text.at;
        scene.over.push_back(std::move(text));
        scene.bandLabels.push_back(std::move(label));
    }
}

namespace {

/// A UTF-8 string cut into its code points, so an ellipsis is never put in the
/// middle of one.
std::vector<std::string> codePoints(const std::string& text)
{
    std::vector<std::string> points;
    for (std::size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i]);
        const std::size_t length = lead < 0x80        ? 1
                                   : lead >> 5 == 0x6 ? 2
                                   : lead >> 4 == 0xE ? 3
                                                      : 4;
        points.push_back(text.substr(i, length));
        i += length;
    }
    return points;
}

} // namespace

std::string Figure::elided(const std::string& text, double width, TextRole role, bool middle) const
{
    if (measure_.width(text, role) <= width) {
        return text;
    }
    // As many code points as fit with the ellipsis, from the start -- or half
    // from each end, the start's half the larger, as Qt keeps a path's two
    // ends.
    const std::string ellipsis = "\xE2\x80\xA6";
    const std::vector<std::string> points = codePoints(text);
    const auto keep = [&](std::size_t count) {
        std::string out;
        const std::size_t front = middle ? (count + 1) / 2 : count;
        for (std::size_t i = 0; i < front; ++i) {
            out += points[i];
        }
        out += ellipsis;
        if (middle) {
            for (std::size_t i = points.size() - (count - front); i < points.size(); ++i) {
                out += points[i];
            }
        }
        return out;
    };
    std::size_t low = 0;
    std::size_t high = points.size();
    while (low < high) {
        const std::size_t mid = (low + high + 1) / 2;
        if (measure_.width(keep(mid), role) <= width) {
            low = mid;
        }
        else {
            high = mid - 1;
        }
    }
    return keep(low);
}

std::vector<std::string> Figure::wrapped(const std::string& text, double width, TextRole role) const
{
    // Word by word, as a toolkit wraps a paragraph: a word that fits on the
    // line goes on it, and one that does not starts the next.
    std::vector<std::string> lines;
    std::string line;
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t space = text.find(' ', at);
        const std::size_t ends = space == std::string::npos ? text.size() : space;
        const std::string word = text.substr(at, ends - at);
        at = ends + (space == std::string::npos ? 0 : 1);
        if (word.empty()) {
            continue;
        }
        const std::string longer = line.empty() ? word : line + " " + word;
        if (!line.empty() && measure_.width(longer, role) > width) {
            lines.push_back(line);
            line = word;
        }
        else {
            line = longer;
        }
    }
    if (!line.empty()) {
        lines.push_back(line);
    }
    return lines;
}

// --- gestures -------------------------------------------------------------------

std::string Figure::xNumber(double value)
{
    refresh();
    const Layout lay = layout();
    return axisLabel(value, camera_.x.viewLow(), camera_.x.viewHigh(), lay.xTicks.logarithmic);
}

std::string Figure::yNumber(double value)
{
    refresh();
    const Layout lay = layout();
    return axisLabel(value, camera_.y.viewLow(), camera_.y.viewHigh(), lay.yTicks.logarithmic);
}

int Figure::lineAt(double px, double py, double reach)
{
    refresh();
    const Layout lay = layout();
    camera_.area = lay.area;
    const PlotView pane = paneView(lay);
    source().setPaneColumns(
        columnsOf((settings_.flipped ? lay.area.height : lay.area.width) * pixelRatio_));
    source().setView(camera_.viewMinX(), camera_.viewMaxX(), settings_.xLog);
    int best = -1;
    double bestDistance = reach * reach;
    for (std::size_t k = 0; k < drawn_.size(); ++k) {
        const int index = drawn_[k].line;
        PlotLine line = source().drawLine(index);
        const LineWindow window = windowOf(static_cast<int>(k));
        if (window.own) {
            line.ownY = true;
            line.yMin = window.low;
            line.yMax = window.high;
            line.ownYLog = window.logarithmic;
        }
        const PlotAxis axis = source().drawAxis(index);
        const std::optional<NearestSample> found =
            nearestSample(std::span<const PlotLine>(&line, 1), std::span<const PlotAxis>(&axis, 1),
                          pane, px - lay.area.x, py - lay.area.y);
        if (!found.has_value()) {
            continue;
        }
        const double dx = found->px - (px - lay.area.x);
        const double dy = found->py - (py - lay.area.y);
        if (dx * dx + dy * dy <= bestDistance) {
            bestDistance = dx * dx + dy * dy;
            best = index;
        }
    }
    return best;
}

void Figure::scaleLine(int line, double py, double factor)
{
    // The value under the pointer stays under it, and the next pan, wheel or
    // band takes the same share of this line's window as of every other.
    const int position = positionOf(line);
    if (position < 0 || !(factor > 0.0) || !std::isfinite(factor)) {
        return;
    }
    if (drawn_.size() < 2) {
        return; // a lone line's axis is the common one, and the wheel moves it
    }
    // A line on the common axis is drawn in exactly the window a moved one
    // with a scale of one and no shift would have, so that is what is scaled.
    const LineWindow window = windowOf(position, true);
    ensureLine(line);
    LineSettings& held = lines_[static_cast<std::size_t>(line)];
    const double base = settings_.yLogBase;
    const double low = axisPosition(window.low, window.logarithmic, base);
    const double high = axisPosition(window.high, window.logarithmic, base);
    const PaneRect& a = camera_.area;
    const double up = settings_.flipped ? (py - a.x) / a.width : 1.0 - (py - a.y) / a.height;
    const double held_ = low + std::clamp(up, 0.0, 1.0) * (high - low);
    const double span = (high - low) / factor;
    const double nextLow = held_ - std::clamp(up, 0.0, 1.0) * span;
    const double bottom = window.fixed ? 0.0 : commonFraction(camera_.viewMinY());
    const double top = window.fixed ? 1.0 : commonFraction(camera_.viewMaxY());
    if (!(top > bottom)) {
        return;
    }
    const double homeSpan = span / (top - bottom);
    const double homeLow = nextLow - bottom * homeSpan;
    const double baseSpan = window.baseHigh - window.baseLow;
    const double centre = (window.baseLow + window.baseHigh) / 2.0;
    if (!(homeSpan > 0.0) || !(baseSpan > 0.0)) {
        return;
    }
    held.scale = std::clamp(baseSpan / homeSpan, 1e-12, 1e12);
    held.shift = homeLow + homeSpan / 2.0 - centre;
}

void Figure::shiftLine(int line, double pixels)
{
    const int position = positionOf(line);
    if (position < 0 || drawn_.size() < 2) {
        return;
    }
    const LineWindow window = windowOf(position, true);
    const double base = settings_.yLogBase;
    const double span = axisPosition(window.high, window.logarithmic, base) -
                        axisPosition(window.low, window.logarithmic, base);
    const PaneRect& a = camera_.area;
    const double extent = settings_.flipped ? a.width : a.height;
    if (!(extent > 0.0)) {
        return;
    }
    ensureLine(line);
    LineSettings& held = lines_[static_cast<std::size_t>(line)];
    // Down the pane moves the line down, which is its window moving up; across
    // a flipped one, right moves it right.
    held.shift += (settings_.flipped ? -pixels : pixels) / extent * span;
}

void Figure::wheel(double px, double py, double angleDelta, Modifiers modifiers)
{
    refresh();
    const Layout lay = layout();
    camera_.area = lay.area;
    if (!reason().empty()) {
        return; // a pane explaining why it cannot draw takes no gestures
    }
    const double factor = std::pow(1.25, angleDelta / 120.0);
    if (modifiers.alt && modifiers.control && settings_.lineGestures) {
        const int line = lineAt(px, py);
        if (line >= 0) {
            scaleLine(line, settings_.flipped ? px : py, factor);
        }
        return;
    }
    // Where the zoom is heading goes to the lines before the window it makes
    // does (PlotSurface.pushZoomFocus), so that whatever is read for it is read
    // towards the pointer.
    if (const std::optional<GestureFocus> focus =
            camera_.zoomAt(px, py, factor, zoomAxesFor(modifiers.shift, modifiers.control))) {
        source().focus(*focus);
    }
}

void Figure::press(double px, double py, Button button, Modifiers modifiers)
{
    refresh();
    camera_.area = layout().area;
    pressed_ = last_ = Point(px, py);
    pointer_ = Point(px, py);
    moved_ = false;
    banding_ = false;
    controlClick_ = false;
    dragLine_ = -1;
    if (button == Button::Right) {
        drag_ = Drag::Band;
        bandAxes_ = zoomAxesFor(modifiers.shift, modifiers.control);
        return;
    }
    if (settings_.lineGestures &&
        (button == Button::Middle || (button == Button::Left && modifiers.alt))) {
        dragLine_ = lineAt(px, py);
        drag_ = dragLine_ >= 0 ? Drag::Line : Drag::None;
        return;
    }
    controlClick_ = modifiers.control && settings_.lineGestures;
    drag_ = Drag::Pan;
}

void Figure::move(double px, double py, Modifiers modifiers)
{
    const Point at(px, py);
    pointer_ = at;
    if (drag_ == Drag::None) {
        return;
    }
    if (drag_ == Drag::Band) {
        bandAxes_ = zoomAxesFor(modifiers.shift, modifiers.control);
    }
    const double dx = px - last_.x();
    const double dy = py - last_.y();
    moved_ = moved_ || std::abs(px - pressed_.x()) > 2.0 || std::abs(py - pressed_.y()) > 2.0;
    banding_ =
        banding_ || (drag_ == Drag::Band && (std::abs(px - pressed_.x()) > style_.dragDistance ||
                                             std::abs(py - pressed_.y()) > style_.dragDistance));
    if (drag_ == Drag::Pan) {
        camera_.panBy(dx, dy);
        // A drag has nowhere it is heading (PlotSurface.panBy).
        source().clearFocus();
    }
    else if (drag_ == Drag::Line) {
        refresh();
        shiftLine(dragLine_, settings_.flipped ? dx : dy);
    }
    last_ = at;
}

void Figure::release(double px, double py, Button button, Modifiers modifiers)
{
    last_ = Point(px, py);
    if (drag_ == Drag::Band && button == Button::Right) {
        drag_ = Drag::None;
        banding_ = false;
        // A stated region is a reader who has already said where they are
        // going, so it is said to the lines too (PlotSurface.zoomToRegion).
        // The keys at release are the ones that choose the axis, as they are
        // for the wheel.
        std::optional<GestureFocus> focus;
        const ZoomAxes axes = zoomAxesFor(modifiers.shift, modifiers.control);
        if (camera_.zoomToRegion(pressed_.x(), pressed_.y(), px, py, axes, &focus) &&
            focus.has_value()) {
            source().focus(*focus);
        }
        return;
    }
    if (drag_ == Drag::Pan && controlClick_ && !moved_) {
        drag_ = Drag::None;
        banding_ = false;
        const int line = lineAt(px, py);
        if (line >= 0) {
            // Put back: on the common axis, unmoved and unscaled.
            ensureLine(line);
            LineSettings& held = lines_[static_cast<std::size_t>(line)];
            held.scale = 1.0;
            held.shift = 0.0;
        }
        return;
    }
    drag_ = Drag::None;
    banding_ = false;
}

void Figure::doubleClick(double, double, Button button)
{
    if (button == Button::Left) {
        drag_ = Drag::None;
        banding_ = false;
        resetView();
    }
}

void Figure::hover(double px, double py)
{
    pointer_ = Point(px, py);
}

void Figure::leave()
{
    pointer_.reset();
}

// --- state ------------------------------------------------------------------------

std::string Figure::state() const
{
    using json::Value;
    Value colours = Value::object();
    colours.set("mode", Value::of(settings_.colours.mode))
        .set("single", Value::of(hexOf(settings_.colours.single)))
        .set("rangeFrom", Value::of(hexOf(settings_.colours.rangeFrom)))
        .set("rangeTo", Value::of(hexOf(settings_.colours.rangeTo)))
        .set("reversed", Value::of(settings_.colours.reversed))
        .set("from", Value::of(settings_.colours.from))
        .set("to", Value::of(settings_.colours.to))
        .set("custom", Value::of(settings_.colours.custom));
    Value lines = Value::array();
    for (const LineSettings& line : lines_) {
        Value one = Value::object();
        one.set("visible", Value::of(line.visible))
            .set("colour", line.colour.has_value() ? Value::of(hexOf(*line.colour)) : Value{})
            .set("ownAxis", Value::of(line.ownAxis))
            .set("axisFixed", Value::of(line.axisFixed))
            .set("axisLabel", Value::of(line.axisLabel))
            .set("scale", Value::of(line.scale))
            .set("shift", Value::of(line.shift));
        lines.items.push_back(std::move(one));
    }
    Value view = Value::object();
    view.set("zoomX", Value::of(camera_.x.view.zoom))
        .set("panX", Value::of(camera_.x.view.pan))
        .set("zoomY", Value::of(camera_.y.view.zoom))
        .set("panY", Value::of(camera_.y.view.pan));
    Value root = Value::object();
    root.set("plotcore", Value::of(1.0))
        .set("grid", Value::of(std::string(gridName(settings_.grid))))
        .set("gridStepX", Value::of(settings_.gridStepX))
        .set("gridStepY", Value::of(settings_.gridStepY))
        .set("xLog", Value::of(settings_.xLog))
        .set("yLog", Value::of(settings_.yLog))
        .set("xLogBase", Value::of(settings_.xLogBase))
        .set("yLogBase", Value::of(settings_.yLogBase))
        .set("flipped", Value::of(settings_.flipped))
        .set("minorNumbers", Value::of(settings_.minorNumbers))
        .set("markers", Value::of(settings_.markers))
        .set("cursor", Value::of(settings_.cursor))
        .set("readout", Value::of(settings_.readout))
        .set("bandReadout", Value::of(settings_.bandReadout))
        .set("paneMessage", Value::of(settings_.paneMessage))
        .set("lineGestures", Value::of(settings_.lineGestures))
        .set("title", Value::of(settings_.title))
        .set("xLabel", Value::of(settings_.xLabel))
        .set("yLabel", Value::of(settings_.yLabel))
        .set("legend", Value::of(settings_.legend))
        .set("legendCorner", Value::of(std::string(cornerName(settings_.legendCorner))))
        .set("colours", std::move(colours))
        .set("highlighted", Value::of(static_cast<double>(settings_.highlighted)))
        .set("bundle", Value::of(settings_.bundle))
        .set("dark", Value::of(settings_.dark))
        .set("paper", Value::of(settings_.paper))
        .set("stacked", Value::of(settings_.stacked))
        .set("lines", std::move(lines))
        .set("view", std::move(view));
    return json::write(root);
}

bool Figure::restore(const std::string& text)
{
    const std::optional<json::Value> parsed = json::parse(text);
    if (!parsed.has_value() || parsed->kind != json::Value::Kind::Object ||
        parsed->find("plotcore") == nullptr) {
        return false;
    }
    const json::Value& root = *parsed;
    FigureSettings next = settings_;
    next.grid = gridOf(root.textOr("grid", gridName(next.grid)));
    next.gridStepX = root.numberOr("gridStepX", next.gridStepX);
    next.gridStepY = root.numberOr("gridStepY", next.gridStepY);
    next.xLog = root.boolOr("xLog", next.xLog);
    next.yLog = root.boolOr("yLog", next.yLog);
    next.xLogBase = root.numberOr("xLogBase", next.xLogBase);
    next.yLogBase = root.numberOr("yLogBase", next.yLogBase);
    next.flipped = root.boolOr("flipped", next.flipped);
    next.minorNumbers = root.boolOr("minorNumbers", next.minorNumbers);
    next.markers = root.boolOr("markers", next.markers);
    next.cursor = root.boolOr("cursor", next.cursor);
    next.readout = root.boolOr("readout", next.readout);
    next.bandReadout = root.boolOr("bandReadout", next.bandReadout);
    next.paneMessage = root.boolOr("paneMessage", next.paneMessage);
    next.lineGestures = root.boolOr("lineGestures", next.lineGestures);
    next.title = root.textOr("title", next.title);
    next.xLabel = root.textOr("xLabel", next.xLabel);
    next.yLabel = root.textOr("yLabel", next.yLabel);
    next.legend = root.boolOr("legend", next.legend);
    next.legendCorner = cornerOf(root.textOr("legendCorner", cornerName(next.legendCorner)));
    if (const json::Value* colours = root.find("colours"); colours != nullptr) {
        next.colours.mode = colours->textOr("mode", next.colours.mode);
        const auto colourOr = [&](const char* key, const Rgba& fallback) {
            const json::Value* found = colours->find(key);
            return found != nullptr && found->kind == json::Value::Kind::String
                       ? colourOf(found->text)
                       : fallback;
        };
        next.colours.single = colourOr("single", next.colours.single);
        next.colours.rangeFrom = colourOr("rangeFrom", next.colours.rangeFrom);
        next.colours.rangeTo = colourOr("rangeTo", next.colours.rangeTo);
        next.colours.reversed = colours->boolOr("reversed", next.colours.reversed);
        next.colours.from = colours->numberOr("from", next.colours.from);
        next.colours.to = colours->numberOr("to", next.colours.to);
        if (colours->find("custom") != nullptr) {
            next.colours.custom = colours->boolOr("custom", next.colours.custom);
        }
        else {
            // A view written before a colour could be left to the ground: its
            // colours are a choice wherever one of them is a colour neither
            // ground would have put there -- the viewer's migration.
            const LineColours dark = LineColours::defaults(true);
            const LineColours light = LineColours::defaults(false);
            const auto chosen = [](const Rgba& c, const Rgba& a, const Rgba& b) {
                const auto same = [](const Rgba& x, const Rgba& y) { return hexOf(x) == hexOf(y); };
                return !same(c, a) && !same(c, b);
            };
            next.colours.custom = chosen(next.colours.single, dark.single, light.single) ||
                                  chosen(next.colours.rangeFrom, dark.rangeFrom, light.rangeFrom) ||
                                  chosen(next.colours.rangeTo, dark.rangeTo, light.rangeTo);
        }
    }
    next.highlighted = static_cast<int>(root.numberOr("highlighted", next.highlighted));
    next.bundle = root.boolOr("bundle", next.bundle);
    next.dark = root.boolOr("dark", next.dark);
    next.paper = root.boolOr("paper", next.paper);
    next.stacked = root.boolOr("stacked", next.stacked);
    setSettings(next);

    if (const json::Value* lines = root.find("lines");
        lines != nullptr && lines->kind == json::Value::Kind::Array) {
        for (std::size_t i = 0; i < lines->items.size(); ++i) {
            const json::Value& one = lines->items[i];
            LineSettings line = lineSettings(static_cast<int>(i));
            line.visible = one.boolOr("visible", line.visible);
            if (const json::Value* colour = one.find("colour"); colour != nullptr) {
                line.colour = colour->kind == json::Value::Kind::String
                                  ? std::optional<Rgba>(colourOf(colour->text))
                                  : std::nullopt;
            }
            line.ownAxis = one.boolOr("ownAxis", line.ownAxis);
            line.axisFixed = one.boolOr("axisFixed", line.axisFixed);
            line.axisLabel = one.textOr("axisLabel", line.axisLabel);
            line.scale = one.numberOr("scale", line.scale);
            line.shift = one.numberOr("shift", line.shift);
            setLineSettings(static_cast<int>(i), line);
        }
    }
    if (const json::Value* view = root.find("view"); view != nullptr) {
        const auto zoomOf = [](double zoom) {
            return zoom >= 1.0 && std::isfinite(zoom) ? zoom : 1.0;
        };
        camera_.x.view = {zoomOf(view->numberOr("zoomX", 1.0)), view->numberOr("panX", 0.0)};
        camera_.y.view = {zoomOf(view->numberOr("zoomY", 1.0)), view->numberOr("panY", 0.0)};
    }
    return true;
}

} // namespace plotcore
