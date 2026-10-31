// SPDX-FileCopyrightText: 2026 Jonas Sattler
// SPDX-FileCopyrightText: 2026 Andreas Martin
// SPDX-License-Identifier: BSD-3-Clause

#include "plotcore/Projection.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>

namespace plotcore {
namespace {

// How far outside the pane a projected point is allowed to land.
//
// A sample does not stop existing because the reader zoomed past it: it is
// drawn where it falls and the pane clips it, which is what keeps the line
// entering and leaving at the frame instead of stopping at the last sample
// that happened to be visible. But a value of 1e300 in a window a unit wide
// projects to 1e302 pixels, and that is +/-inf once it is a float32 -- an
// infinity in a vertex buffer is not a point off screen, it is a triangle the
// rasteriser may do anything at all with.
//
// Ten million pixels is four orders of magnitude outside any pane and four
// short of where float32 starts losing whole numbers. Clamping there moves the
// point where such a segment crosses the frame by far less than a pixel, and
// it is the only place in this file where the arithmetic is allowed to lie.
constexpr double kFarAway = 1e7;

// Where drawn point `at` sits along the axis, before the axis makes it an x.
double positionOf(const PlotLine& line, long long at)
{
    if (line.positions != nullptr) {
        return line.positions[at] * line.positionScale;
    }
    return line.positionStart + static_cast<double>(at) * line.positionStep;
}

// Which drawn point of a time base `position` reads, when the time base's
// drawn points sit at `start + i * step`.
//
// The nearest, except past either end. Rounding puts a position more than half
// a step past the last drawn time one past the end, and the point is then
// dropped -- which is where every line drawn at its own elements ends: its last
// extreme is the line's last element, and the time base's last drawn point is
// half a bucket short of that. A position that has reached the time base's
// *next* point would be a line longer than its axis, and is still past the end.
double timeIndex(double position, double start, double step, long long count)
{
    if (!(std::abs(step) > 0.0) || count <= 0) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const double exact = (position - start) / step;
    double index = std::round(exact);
    const auto last = static_cast<double>(count - 1);
    if (index > last && exact < static_cast<double>(count)) {
        index = last;
    }
    if (index < 0.0 && exact > -1.0) {
        index = 0.0;
    }
    return index;
}

} // namespace

AxisMapping mappingOver(double low, double high, bool logarithmic, double base)
{
    AxisMapping mapping;
    mapping.logarithmic = logarithmic;
    mapping.base = base;
    if (logarithmic) {
        // Only a number above one is a base. At exactly one the logarithm is a
        // division by zero and every value on the axis sits in the same place;
        // below it the axis runs backwards, which is a different request from
        // the one this answers. Refused rather than corrected, for the reason
        // the bounds below are: there is no nearest legal base to fall back to,
        // because the legal ones are open at one.
        if (!std::isfinite(base) || !(base > 1.0)) {
            return mapping;
        }
        // A bound at or below zero is not a bound this axis can be drawn
        // between, and there is no nearest one worth falling back to: clamping
        // it to some tiny positive number would put the whole of the data in
        // the top few pixels of a pane whose lower half means nothing. The view
        // is refused instead, exactly as a span of zero is, and the caller that
        // chose the bounds is where a positive one has to come from.
        if (!(low > 0.0) || !(high > 0.0)) {
            return mapping;
        }
        mapping.low = logOf(low, base);
        mapping.high = logOf(high, base);
    }
    else {
        mapping.low = low;
        mapping.high = high;
    }
    // A span, running upwards. Both halves are refusals rather than
    // corrections: a collapsed axis has no place to put anything, and an
    // inverted one is not a window the surface can produce -- every path that
    // states a view sorts its bounds first (see PlotSurface.setViewRange). The
    // x axis has always been held to exactly this; the y axis used to be held
    // to the absolute span instead, which differed only in that it would have
    // drawn an upside-down picture for a caller there is none of.
    mapping.usable = std::isfinite(mapping.high - mapping.low) && mapping.high > mapping.low;
    return mapping;
}

AxisMapping xMappingOf(const PlotView& view)
{
    return mappingOver(view.xMin, view.xMax, view.xLog, view.xLogBase);
}

AxisMapping yMappingOf(const PlotView& view)
{
    return mappingOver(view.yMin, view.yMax, view.yLog, view.yLogBase);
}

PlotView lineView(const PlotLine& line, const PlotView& view)
{
    if (!line.ownY) {
        return view;
    }
    PlotView own = view;
    own.yMin = line.yMin;
    own.yMax = line.yMax;
    own.yLog = line.ownYLog;
    return own;
}

std::optional<PlotWindow> windowFor(double low, double high, long long length, long long buckets)
{
    if (length <= 0 || buckets <= 0) {
        return {};
    }
    if (!std::isfinite(low) || !std::isfinite(high) || !(high > low)) {
        return {};
    }

    // A bucket small enough to put `buckets` of them across twice the visible
    // span. A power of two, and ceiling rather than nearest, so that zooming
    // steps from one octave to the next -- an arbitrary bucket would shimmer,
    // because every pixel of zoom would change which samples each of them
    // holds. Bounded rather than open, because a span that has gone to infinity
    // under a degenerate view would otherwise not stop.
    const double wanted = 2.0 * (high - low) / static_cast<double>(buckets);
    long long bucket = 1;
    while (static_cast<double>(bucket) < wanted && bucket < (std::int64_t{1} << 40)) {
        bucket <<= 1;
    }

    const long long span = bucket * buckets;
    if (span >= length) {
        // The whole-line summary already covers this much at this bucket or
        // finer. There is nothing a second read could add.
        return {};
    }

    const long long stride = std::max<long long>(bucket, (buckets / 4) * bucket);
    // Clamped in double before the cast: a view a long way off the data gives a
    // bound that does not fit in a long long at all.
    const double aligned =
        std::floor(low / static_cast<double>(stride)) * static_cast<double>(stride);
    auto first = static_cast<long long>(std::clamp(aligned, 0.0, static_cast<double>(length - 1)));
    first -= first % stride; // back onto the alignment after the clamp

    const long long take = std::min(span, length - first);
    if (take <= 0) {
        return {};
    }
    const auto columns = static_cast<int>((take + bucket - 1) / bucket);
    if (columns <= 0) {
        return {};
    }
    return PlotWindow{first, take, bucket, columns};
}

double xOf(const PlotLine& line, const PlotAxis& axis, long long at)
{
    if (line.xs != nullptr) {
        // Stated by the line itself, which is what a fold onto a logarithmic
        // axis's columns does. See PlotLine::xs.
        const double x = line.xs[at];
        return std::isfinite(x) ? x : std::numeric_limits<double>::quiet_NaN();
    }
    const double position = positionOf(line, at);
    if (!axis.explicitX()) {
        return axis.start + position * axis.step;
    }
    // The time base has a value at each of its own positions and nowhere in
    // between, so a point that falls between two of them takes the nearer.
    //
    // At the share of the axis the position is, not at the position itself:
    // `values` holds the time base's *drawn* points and a position counts the
    // elements they were summarised from. See PlotAxis::valueStep -- with a
    // time base twenty thousand elements long summarised to two thousand
    // points, reading it at the position is reading ten times past its end.
    //
    // The run the reader is looking at first, where there is one and where it
    // reaches this position: it is the same time base read finer, so it is the
    // same answer only sharper. See PlotAxis::closerValues.
    if (axis.hasCloser() && std::abs(axis.closerStep) > 0.0) {
        const double at = timeIndex(position, axis.closerStart, axis.closerStep, axis.closerCount);
        if (at >= 0.0 && at < static_cast<double>(axis.closerCount)) {
            const double x = axis.closerValues[static_cast<std::size_t>(at)];
            return std::isfinite(x) ? x : std::numeric_limits<double>::quiet_NaN();
        }
    }
    if (!(std::abs(axis.valueStep) > 0.0)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const double index = timeIndex(position, 0.0, axis.valueStep, axis.count);
    if (!(index >= 0.0) || !(index < static_cast<double>(axis.count))) {
        // Past the end of the time base. The line stops here rather than being
        // drawn against an x that does not exist, which is what "align" means
        // when a line and its axis are different lengths.
        return std::numeric_limits<double>::quiet_NaN();
    }
    const double x = axis.values[static_cast<std::size_t>(index)];
    return std::isfinite(x) ? x : std::numeric_limits<double>::quiet_NaN();
}

std::vector<Point> samplesOf(const PlotLine& line, const PlotAxis& axis)
{
    std::vector<Point> out;
    if (line.values == nullptr || line.count <= 0) {
        return out;
    }
    out.reserve(static_cast<std::size_t>(line.count));
    for (long long i = 0; i < line.count; ++i) {
        // A value that would not read is a gap in the line, not a zero: an
        // invented number at the axis is a reading of the data, and a wrong
        // one.
        const double value = line.values[i];
        if (!std::isfinite(value)) {
            continue;
        }
        const double x = xOf(line, axis, i);
        if (!std::isfinite(x)) {
            continue;
        }
        out.emplace_back(x, value);
    }
    return out;
}

double yFractionOf(double value, const PlotView& view)
{
    return yMappingOf(view).fractionOf(value);
}

double xFractionOf(double x, const PlotView& view)
{
    return xMappingOf(view).fractionOf(x);
}

PlotView uprightView(const PlotView& view)
{
    if (!view.transposed) {
        return view;
    }
    PlotView upright = view;
    upright.width = view.height;
    upright.height = view.width;
    upright.transposed = false;
    return upright;
}

Point transposedPoint(const Point& upright, const PlotView& view)
{
    // Upright, x = fx * height and y = (1 - fy) * width. On the pane, x is to
    // be fy * width and y (1 - fx) * height -- which is each of those taken
    // away from the side it was measured against.
    return {view.width - upright.y(), view.height - upright.x()};
}

Point uprightPoint(const Point& onPane, const PlotView& view)
{
    return {view.height - onPane.y(), view.width - onPane.x()};
}

PlotProjected projectLine(const PlotLine& line, const PlotAxis& axis, const PlotView& view,
                          std::vector<Point>& points, std::vector<PlotRun>& runs)
{
    const auto startRuns = static_cast<int>(runs.size());
    const auto added = [&](bool decimated) {
        return PlotProjected{static_cast<int>(runs.size()) - startRuns, decimated};
    };
    if (line.values == nullptr || line.count <= 0) {
        return {};
    }
    const double w = view.width;
    const double h = view.height;
    if (!(w > 0.0) || !(h > 0.0)) {
        return {};
    }
    const AxisMapping xMap = xMappingOf(view);
    if (!xMap.usable) {
        return {};
    }
    const AxisMapping yMap = yMappingOf(view);
    if (!yMap.usable) {
        return {};
    }

    // Everything below is arithmetic in double, and only the finished pixel
    // coordinate is ever cast down. That is the entire trick behind an epoch
    // timestamp drawing as a line here and as a staircase anywhere that casts
    // first.
    const auto toY = [&](double value) { return h - yMap.fractionOf(value) * h; };
    const auto toX = [&](double x) { return xMap.fractionOf(x) * w; };

    // Where the current run started, as an index into `points`.
    int open = -1;
    const auto closeRun = [&]() {
        if (open < 0) {
            return;
        }
        const int length = static_cast<int>(points.size()) - open;
        // ...and so is a run of several stations that are all the same one.
        // A bucket's seam is the next bucket's first element, which is often
        // that bucket's first extreme as well, so a fold hands the one element
        // over twice in a row; between two gaps, that is a run of two stations
        // and no length. The test stops at the first station that differs, so
        // a run pays for it at its first two stations and not along its length.
        const auto onePoint = [&]() {
            const Point& at = points[static_cast<std::size_t>(open)];
            return std::all_of(points.begin() + open + 1, points.end(),
                               [&](const Point& p) { return p.x() == at.x() && p.y() == at.y(); });
        };
        if (length >= 2 && !onePoint()) {
            runs.push_back({open, length});
        }
        else if (length >= 1) {
            // A run of one station is a point, and a stroke needs a direction.
            // Dropping it keeps the counts honest rather than emitting a
            // vertex pair that rasterises to nothing.
            points.resize(static_cast<std::size_t>(open));
        }
        open = -1;
    };
    // The centre of the device pixel the point falls in, back in logical
    // pixels. A pan then moves the stroke by whole pixels: the same sample
    // keeps the same place inside its pixel, which is what stops the line
    // crawling across a pixel as the window slides.
    const double pixelRatio = std::max(view.pixelRatio, 1.0);
    const auto place = [&](double px, double py) {
        if (std::isfinite(px)) {
            // The pixel is floor of this. A logarithmic column edge that
            // falls on a pixel boundary comes back an ulp under that integer
            // from one libm and an ulp over it from the other, and floor gives
            // them different pixels. The golden sum of data x then misses its
            // relative 1e-9, while the sum of the pixels can still agree when
            // another edge steps the other way. A millionth of a pixel is
            // still that boundary. A real sample is nowhere near it.
            const double scaled = px * pixelRatio;
            const double nearest = std::nearbyint(scaled);
            if (std::abs(scaled - nearest) <= 1e-6) {
                px = nearest / pixelRatio;
            }
            px = (std::floor(px * pixelRatio) + 0.5) / pixelRatio;
        }
        if (open < 0) {
            open = static_cast<int>(points.size());
        }
        points.emplace_back(std::clamp(px, -kFarAway, kFarAway),
                            std::clamp(py, -kFarAway, kFarAway));
    };

    const auto count = static_cast<std::int64_t>(line.count);
    const auto xAt = [&](std::int64_t i) { return xOf(line, axis, i); };
    const auto drawRange = [&](std::int64_t from, std::int64_t to) {
        for (std::int64_t i = from; i <= to; ++i) {
            const double value = line.values[i];
            const double x = xAt(i);
            if (!yMap.draws(value) || !xMap.draws(x)) {
                closeRun();
                continue;
            }
            place(toX(x), toY(value));
        }
        closeRun();
    };

    // A stated x is already a fold, one or two points a column. A time base
    // that turns around, or that has a hole, is not a column of anything: the
    // samples in one pixel would come from all over the line. Both are drawn
    // sample for sample. An ascending time base is a column, below.
    const bool located = line.positions != nullptr || axis.explicitX();
    const auto ascending = [&]() {
        if (count < 2) {
            return true;
        }
        double previous = xAt(0);
        if (!std::isfinite(previous)) {
            return false;
        }
        for (std::int64_t i = 1; i < count; ++i) {
            const double x = xAt(i);
            if (!std::isfinite(x) || x < previous) {
                return false;
            }
            previous = x;
        }
        return true;
    };
    const bool ordered = !located || ascending();
    if (line.xs != nullptr || (axis.explicitX() && !ordered)) {
        drawRange(0, count - 1);
        // Nothing here was summarised by this. A time base does not make a
        // line of summaries into a line of samples: the line says what its
        // values are, and the axis has no bearing on that.
        return added(line.summarised);
    }

    // First index whose x is at least `edge`, and the first whose x is past
    // it. The line is ascending here, which is what makes the bisection an
    // answer. A sample that lands on an edge belongs to the column that
    // starts there.
    const auto lowerBound = [&](double edge) {
        std::int64_t lo = 0;
        std::int64_t hi = count;
        while (lo < hi) {
            const std::int64_t mid = lo + (hi - lo) / 2;
            if (!(xAt(mid) >= edge)) {
                lo = mid + 1;
            }
            else {
                hi = mid;
            }
        }
        return lo;
    };
    const auto upperBound = [&](double edge) {
        std::int64_t lo = 0;
        std::int64_t hi = count;
        while (lo < hi) {
            const std::int64_t mid = lo + (hi - lo) / 2;
            if (!(xAt(mid) > edge)) {
                lo = mid + 1;
            }
            else {
                hi = mid;
            }
        }
        return lo;
    };

    // x is affine in the sample index, so the window can be turned back into a
    // range of indices and the samples outside it never touched.
    //
    // Still true on a logarithmic x axis, and that is worth saying because it
    // looks as though it should not be: the window's bounds are in the data's
    // own units whichever scale the axis is drawn on -- the logarithm happens
    // between a value and a *pixel*, not between the reader and the data -- so
    // the inversion below is the same subtraction it always was.
    const double x0 = axis.start + line.positionStart * axis.step;
    const double dx = line.positionStep * axis.step;

    std::int64_t first = 0;
    std::int64_t last = count - 1;
    if (axis.explicitX()) {
        // One sample either side, as the affine window below. The bounds are
        // the times themselves: the index is whatever sample sits there.
        const double low = std::min(view.xMin, view.xMax);
        const double high = std::max(view.xMin, view.xMax);
        if (!std::isfinite(low) || !std::isfinite(high)) {
            return {};
        }
        const std::int64_t from = lowerBound(low);
        const std::int64_t to = upperBound(high);
        first = std::max<std::int64_t>(from - 1, 0);
        last = std::min<std::int64_t>(to, count - 1);
    }
    else if (line.positions != nullptr) {
        // Not evenly spaced, so the window is not a division: it is a range of
        // positions, and they are written in order. One point either side of
        // it, for the reason the even case below gives.
        if (!(std::abs(axis.step) > 0.0) || !(line.positionScale > 0.0)) {
            return {};
        }
        double low = (view.xMin - axis.start) / axis.step / line.positionScale;
        double high = (view.xMax - axis.start) / axis.step / line.positionScale;
        if (low > high) {
            std::swap(low, high);
        }
        if (std::isnan(low) || std::isnan(high)) {
            return {};
        }
        const double* const begin = line.positions;
        const double* const end = begin + count;
        const auto from = static_cast<std::int64_t>(std::lower_bound(begin, end, low) - begin);
        const auto to = static_cast<std::int64_t>(std::upper_bound(begin, end, high) - begin);
        first = std::max<std::int64_t>(from - 1, 0);
        last = std::min<std::int64_t>(to, count - 1);
    }
    else if (std::abs(dx) > 0.0) {
        // One sample either side of the window, so the line enters and leaves
        // the pane at its edges instead of stopping a pixel short of them.
        double lowAt = (view.xMin - x0) / dx;
        double highAt = (view.xMax - x0) / dx;
        if (lowAt > highAt) {
            std::swap(lowAt, highAt);
        }
        if (!std::isfinite(lowAt) || !std::isfinite(highAt)) {
            return {};
        }
        const double floorLow = std::floor(lowAt) - 1.0;
        const double ceilHigh = std::ceil(highAt) + 1.0;
        // Clamped in double before the cast: a window a long way off the data
        // gives a bound that does not fit in an int64 at all.
        first =
            static_cast<std::int64_t>(std::clamp(floorLow, 0.0, static_cast<double>(count - 1)));
        last = static_cast<std::int64_t>(std::clamp(ceilHigh, 0.0, static_cast<double>(count - 1)));
    }
    if (last < first) {
        return {};
    }

    // One column per device pixel. Rounded, not ceiled: that is the column
    // the pane has, and maxColumns is still the line's share of the vertices.
    // The grid is anchored at zero -- at one, on a logarithmic axis -- rather
    // than at the window, so a pan of less than a column leaves every sample
    // in the column it was in. That is what stops the line boiling as it
    // slides. The y at an edge is the chord between the two samples the edge
    // falls between, and it lengthens both columns; it is not another sample.
    // A column of more than one sample that stands at least a pixel tall is
    // the vertical in its middle, and the stroke ends there. Anything
    // shallower is the chord from edge to edge. No such column, and the
    // samples themselves are the line. See Levels.hpp.
    std::int64_t columns = std::llround(w * pixelRatio);
    if (view.maxColumns > 0) {
        columns = std::min<std::int64_t>(columns, view.maxColumns);
    }
    columns = std::max<std::int64_t>(columns, 1);

    const bool indexGrid = !located && std::abs(dx) > 0.0;
    const bool locatedGrid = located && ordered;
    const double xLowBound = std::min(view.xMin, view.xMax);
    const double xHighBound = std::max(view.xMin, view.xMax);
    if ((indexGrid || locatedGrid) && xHighBound > xLowBound &&
        !(view.xLog && !(xLowBound > 0.0))) {
        const double width = (xHighBound - xLowBound) / static_cast<double>(columns);
        std::vector<double> edges(static_cast<std::size_t>(columns) + 1);
        bool edgesOk = width > 0.0 && std::isfinite(width);
        if (edgesOk && !view.xLog) {
            const double k0 = std::floor(xLowBound / width + 0.5);
            for (std::int64_t i = 0; i <= columns; ++i) {
                edges[static_cast<std::size_t>(i)] = (k0 + static_cast<double>(i)) * width;
            }
        }
        else if (edgesOk) {
            const double p0 = std::log(xLowBound);
            const double p1 = std::log(xHighBound);
            const double logWidth = (p1 - p0) / static_cast<double>(columns);
            edgesOk = logWidth > 0.0 && std::isfinite(logWidth);
            if (edgesOk) {
                const double k0 = std::floor(p0 / logWidth + 0.5);
                for (std::int64_t i = 0; i <= columns; ++i) {
                    edges[static_cast<std::size_t>(i)] =
                        std::exp((k0 + static_cast<double>(i)) * logWidth);
                }
            }
        }
        if (edgesOk) {
            const auto accept = [&](std::int64_t i, double x) -> std::optional<double> {
                const double value = line.values[i];
                if (!yMap.draws(value) || !xMap.draws(x)) {
                    return {};
                }
                return value;
            };
            const auto chordIndex = [&](double t) -> std::optional<double> {
                const double lastIndex = static_cast<double>(count - 1);
                if (!std::isfinite(t) || t < 0.0) {
                    return {};
                }
                if (t >= lastIndex) {
                    return accept(count - 1, x0 + lastIndex * dx);
                }
                const double rounded = std::round(t);
                if (std::abs(t - rounded) <= 1e-9) {
                    const auto i = static_cast<std::int64_t>(rounded);
                    return accept(i, x0 + static_cast<double>(i) * dx);
                }
                const auto iNext = static_cast<std::int64_t>(std::ceil(t));
                const auto iPrev = iNext - 1;
                if (iPrev < 0 || iNext >= count) {
                    return {};
                }
                const auto yPrev = accept(iPrev, x0 + static_cast<double>(iPrev) * dx);
                const auto yNext = accept(iNext, x0 + static_cast<double>(iNext) * dx);
                if (!yPrev || !yNext) {
                    return {};
                }
                return *yPrev + (t - static_cast<double>(iPrev)) * (*yNext - *yPrev);
            };
            const auto chordAt = [&](double edge) -> std::optional<double> {
                if (indexGrid) {
                    return chordIndex((edge - x0) / dx);
                }
                const std::int64_t next = upperBound(edge);
                if (next <= 0) {
                    return {};
                }
                if (next >= count) {
                    const double x = xAt(count - 1);
                    if (edge > x) {
                        return {};
                    }
                    return accept(count - 1, x);
                }
                const std::int64_t prev = next - 1;
                const double xPrev = xAt(prev);
                const double xNext = xAt(next);
                const auto yPrev = accept(prev, xPrev);
                const auto yNext = accept(next, xNext);
                if (!yPrev || !yNext) {
                    return {};
                }
                if (xPrev == edge || !(xNext > xPrev)) {
                    return yPrev;
                }
                return *yPrev + (edge - xPrev) / (xNext - xPrev) * (*yNext - *yPrev);
            };
            const auto include = [](double& low, double& high, double y) {
                if (!std::isfinite(y)) {
                    return;
                }
                if (!std::isfinite(low)) {
                    low = high = y;
                    return;
                }
                if (y < low) {
                    low = y;
                }
                else if (y > high) {
                    high = y;
                }
            };

            const double quiet = std::numeric_limits<double>::quiet_NaN();
            std::vector<double> columnLow(static_cast<std::size_t>(columns), quiet);
            std::vector<double> columnHigh(static_cast<std::size_t>(columns), quiet);
            std::vector<int> columnCount(static_cast<std::size_t>(columns), 0);
            int most = 0;
            for (std::int64_t column = 0; column < columns; ++column) {
                const double leftEdge = edges[static_cast<std::size_t>(column)];
                const double rightEdge = edges[static_cast<std::size_t>(column + 1)];
                std::int64_t start = 0;
                std::int64_t end = 0;
                if (indexGrid) {
                    double a = (leftEdge - x0) / dx;
                    double b = (rightEdge - x0) / dx;
                    if (a > b) {
                        std::swap(a, b);
                    }
                    if (std::isfinite(a) && std::isfinite(b)) {
                        const double startD = std::ceil(a - 1e-9);
                        const double endD = std::floor(b - 1e-9) + 1.0;
                        start = static_cast<std::int64_t>(
                            std::clamp(startD, 0.0, static_cast<double>(count)));
                        end = static_cast<std::int64_t>(
                            std::clamp(endD, 0.0, static_cast<double>(count)));
                    }
                }
                else {
                    start = lowerBound(leftEdge);
                    end = lowerBound(rightEdge);
                }
                if (end < start) {
                    end = start;
                }
                double& low = columnLow[static_cast<std::size_t>(column)];
                double& high = columnHigh[static_cast<std::size_t>(column)];
                int& n = columnCount[static_cast<std::size_t>(column)];
                for (std::int64_t i = start; i < end; ++i) {
                    const double value = line.values[i];
                    if (!yMap.draws(value)) {
                        continue;
                    }
                    include(low, high, value);
                    ++n;
                }
                if (const std::optional<double> y = chordAt(leftEdge)) {
                    include(low, high, *y);
                }
                if (const std::optional<double> y = chordAt(rightEdge)) {
                    include(low, high, *y);
                }
                most = std::max(most, n);
            }
            if (most > 1) {
                bool linked = false;
                for (std::int64_t column = 0; column < columns; ++column) {
                    const double leftEdge = edges[static_cast<std::size_t>(column)];
                    const double rightEdge = edges[static_cast<std::size_t>(column + 1)];
                    const double low = columnLow[static_cast<std::size_t>(column)];
                    const double high = columnHigh[static_cast<std::size_t>(column)];
                    const bool tall = columnCount[static_cast<std::size_t>(column)] > 1 &&
                                       yMap.draws(low) && yMap.draws(high) &&
                                       std::abs(yMap.fractionOf(high) - yMap.fractionOf(low)) * h >=
                                           1.0;
                    if (tall) {
                        if (linked) {
                            closeRun();
                            linked = false;
                        }
                        const double mid = 0.5 * (leftEdge + rightEdge);
                        if (xMap.draws(mid)) {
                            place(toX(mid), toY(low));
                            place(toX(mid), toY(high));
                        }
                        closeRun();
                        continue;
                    }
                    const std::optional<double> yLeft = chordAt(leftEdge);
                    const std::optional<double> yRight = chordAt(rightEdge);
                    if (!yLeft || !yRight || !xMap.draws(leftEdge) || !xMap.draws(rightEdge)) {
                        if (linked) {
                            closeRun();
                            linked = false;
                        }
                        continue;
                    }
                    if (!linked) {
                        place(toX(leftEdge), toY(*yLeft));
                    }
                    place(toX(rightEdge), toY(*yRight));
                    linked = true;
                }
                closeRun();
                return added(true);
            }
        }
    }

    // No column held two samples, so these are the line: one either side of
    // the window, which is what `first` and `last` already reach. They are
    // samples only when the model did not summarise them on the way here.
    drawRange(first, last);
    return added(line.summarised);

}

std::optional<NearestSample> nearestSample(std::span<const PlotLine> lines,
                                           std::span<const PlotAxis> axes, const PlotView& pane,
                                           double px, double py)
{
    // Searched upright, as the lines were projected, with the pointer moved
    // there first and the answer moved back: on a transposed pane the pointer's
    // height is what says which x it is over. See PlotView::transposed.
    const PlotView upright = uprightView(pane);
    if (pane.transposed) {
        const Point at = uprightPoint(Point(px, py), pane);
        px = at.x();
        py = at.y();
    }
    const double w = upright.width;
    const double h = upright.height;
    const AxisMapping xMap = xMappingOf(pane);
    if (!(w > 0.0) || !(h > 0.0) || !xMap.usable || lines.empty() || axes.empty()) {
        return std::nullopt;
    }
    // In the data's own units, through the axis's own scale -- so on a
    // logarithmic axis the pointer resolves to the x it is actually over
    // rather than to the one it would be over if the axis were linear.
    const double wanted = xMap.valueAt(px / w);

    double bestDistance = std::numeric_limits<double>::infinity();
    int bestLine = -1;
    double bestX = 0.0;
    double bestY = 0.0;
    double bestPx = 0.0;
    double bestPy = 0.0;

    // Per line, because a line on an axis of its own is drawn through that
    // axis: snapping it through the common one would put the crosshair's dot
    // somewhere the stroke is not.
    AxisMapping yMap;
    const auto consider = [&](int index, long long at, double x, double value) {
        // A sample the axes cannot place is not a sample the crosshair may snap
        // to: it was not drawn, and a readout of it would name a point that is
        // not on the pane.
        if (!xMap.draws(x) || !yMap.draws(value)) {
            return false;
        }
        const double sx = xMap.fractionOf(x) * w;
        const double sy = h - yMap.fractionOf(value) * h;
        const double distance = (sx - px) * (sx - px) + (sy - py) * (sy - py);
        if (distance < bestDistance) {
            bestDistance = distance;
            bestLine = index;
            bestX = x;
            bestY = value;
            bestPx = sx;
            bestPy = sy;
        }
        (void)at;
        return true;
    };

    for (std::size_t slot = 0; slot < lines.size(); ++slot) {
        const auto index = static_cast<int>(slot);
        const PlotLine& line = lines[slot];
        const PlotAxis& axis =
            axes.size() == 1 ? axes.front() : axes[std::min(slot, axes.size() - 1)];
        if (line.values == nullptr || line.count <= 0) {
            continue;
        }
        yMap = yMappingOf(lineView(line, pane));

        if (axis.explicitX() || line.xs != nullptr ||
            !(std::abs(line.positionStep * axis.step) > 0.0)) {
            // A time base need not be monotonic, so there is no index to solve
            // for and the line is searched. Affordable because a line drawn
            // against one is a custom plot entry, and those are thinned on the
            // way out of the file like everything else.
            for (long long i = 0; i < line.count; ++i) {
                const double value = line.values[i];
                if (!std::isfinite(value)) {
                    continue;
                }
                const double x = xOf(line, axis, i);
                if (std::isfinite(x)) {
                    consider(index, i, x, value);
                }
            }
            continue;
        }

        // x is affine in the index, so the nearest one is arithmetic -- or, for
        // a line that names where each point is, a bisection of those, for
        // the reason projectLine bisects them: an envelope's extremes are at
        // their own elements, not half a bucket apart, and a guess from the
        // step can start the walk on the wrong one of a pair.
        long long centre = 0;
        if (line.positions != nullptr) {
            if (!(std::abs(axis.step) > 0.0) || !(line.positionScale > 0.0)) {
                continue;
            }
            const double position = (wanted - axis.start) / axis.step / line.positionScale;
            if (!std::isfinite(position)) {
                continue;
            }
            const double* const end = line.positions + line.count;
            long long after = std::lower_bound(line.positions, end, position) - line.positions;
            after = std::min(after, line.count - 1);
            centre = after;
            if (after > 0 &&
                position - line.positions[after - 1] < line.positions[after] - position) {
                centre = after - 1;
            }
        }
        else {
            const double x0 = axis.start + line.positionStart * axis.step;
            const double dx = line.positionStep * axis.step;
            const double exact = (wanted - x0) / dx;
            if (!std::isfinite(exact)) {
                continue;
            }
            centre = static_cast<long long>(
                std::clamp(std::llround(exact), 0LL, static_cast<long long>(line.count - 1)));
        }

        // Outward from there until a drawable sample turns up. A gap wider than
        // this is a gap the crosshair declines to reach across, which is the
        // same answer the line itself gives: there is nothing drawn there.
        constexpr long long kReach = 64;
        for (long long step = 0; step <= kReach; ++step) {
            bool found = false;
            for (const long long at : {centre - step, centre + step}) {
                if (at < 0 || at >= line.count) {
                    continue;
                }
                const double value = line.values[at];
                if (!std::isfinite(value)) {
                    continue;
                }
                // What the walk is looking for is a sample that could be
                // *taken*, not one that is merely finite. Stopping on a value
                // the axes declined to place -- every non-positive one on a
                // logarithmic axis -- ended the search having considered
                // nothing, and the crosshair then found no sample at all in a
                // pane full of them.
                found = consider(index, at, xOf(line, axis, at), value) || found;
            }
            if (found) {
                break;
            }
        }
    }

    if (bestLine < 0) {
        return std::nullopt;
    }
    if (pane.transposed) {
        const Point drawn = transposedPoint(Point(bestPx, bestPy), pane);
        bestPx = drawn.x();
        bestPy = drawn.y();
    }
    return NearestSample{bestLine, bestX, bestY, bestPx, bestPy};
}

void strokeRun(const Point* points, int count, double width, std::vector<Point>& out)
{
    strokeRunInto(points, count, width, [&out](double x, double y) { out.emplace_back(x, y); });
}

void markerAt(const Point& centre, double radius, std::vector<Point>& out)
{
    // The ring, once. Eight sines and cosines per marker would be sixteen
    // thousand of them on a line of two thousand points, which is a
    // measurable fraction of a frame spent recomputing a constant.
    static const std::vector<Point> ring = [] {
        std::vector<Point> unit;
        unit.reserve(kMarkerSides);
        for (int i = 0; i < kMarkerSides; ++i) {
            const double angle = 2.0 * std::numbers::pi_v<double> * static_cast<double>(i) /
                                 static_cast<double>(kMarkerSides);
            unit.emplace_back(std::cos(angle), std::sin(angle));
        }
        return unit;
    }();

    // Zig-zag around the ring -- 0, n-1, 1, n-2, ... -- which is the triangle
    // strip of a convex polygon and is why a marker costs its own side count
    // in vertices and not one more.
    int low = 0;
    int high = kMarkerSides - 1;
    bool fromLow = true;
    for (int i = 0; i < kMarkerSides; ++i) {
        const Point& at = ring[static_cast<std::size_t>(fromLow ? low++ : high--)];
        fromLow = !fromLow;
        out.emplace_back(centre.x() + at.x() * radius, centre.y() + at.y() * radius);
    }
}

} // namespace plotcore
