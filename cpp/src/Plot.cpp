// SPDX-FileCopyrightText: 2026 Jonas Sattler
// SPDX-FileCopyrightText: 2026 Andreas Martin
// SPDX-License-Identifier: BSD-3-Clause

#include "plotcore/Plot.hpp"

#include "plotcore/Parallel.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>

namespace plotcore {
namespace {

/// Which way a time base runs, if it runs one way at all. A NaN or a turn
/// anywhere is "neither": a bisection of it would answer nothing.
void orderOf(const std::vector<double>& time, bool& sorted, bool& ascending)
{
    bool up = true;
    bool down = true;
    for (std::size_t i = 0; i < time.size(); ++i) {
        if (!std::isfinite(time[i])) {
            up = down = false;
            break;
        }
        if (i > 0) {
            up = up && time[i - 1] <= time[i];
            down = down && time[i - 1] >= time[i];
        }
        if (!up && !down) {
            break;
        }
    }
    sorted = up || down;
    ascending = up;
}

/// Elements of `time` (its first `count`) before `x` in the order it runs.
long long before(const std::vector<double>& time, long long count, bool ascending, double x)
{
    long long low = 0;
    long long high = count;
    while (low < high) {
        const long long mid = low + (high - low) / 2;
        const double t = time[static_cast<std::size_t>(mid)];
        if (ascending ? t < x : t >= x) {
            low = mid + 1;
        }
        else {
            high = mid;
        }
    }
    return low;
}

/// The finite extent of `values`, and the smallest of them above zero.
LineExtent extentOf(const std::vector<double>& values)
{
    LineExtent found;
    double low = std::numeric_limits<double>::infinity();
    double high = -low;
    double positive = 0.0;
    for (const double v : values) {
        if (!std::isfinite(v)) {
            continue;
        }
        low = std::min(low, v);
        high = std::max(high, v);
        if (v > 0.0 && (positive == 0.0 || v < positive)) {
            positive = v;
        }
    }
    found.finite = low <= high;
    found.minimum = found.finite ? low : 0.0;
    found.maximum = found.finite ? high : 0.0;
    found.positive = positive > 0.0;
    found.positiveMinimum = positive;
    return found;
}

} // namespace

struct Plot::Line
{
    std::string name;
    long long length = 0;
    bool finished = false;
    LineKind kind = LineKind::Line;
    std::unique_ptr<PyramidBuilder> builder;
    LinePyramid pyramid;
    LineReader reader;
    std::vector<double> time;
    bool timeSorted = false;
    bool timeAscending = true;

    // The folds, kept until what they were folded for changes. Mutable,
    // because drawing is const and folding is how it is answered.
    mutable int summaryColumns = -1;
    mutable std::vector<double> summary;
    mutable std::vector<double> summaryAt;
    mutable long long stride = 1;
    mutable double step = 1.0;

    mutable std::optional<PlotWindow> window;
    mutable std::vector<double> closer;
    mutable std::vector<double> closerAt;

    mutable LogFoldGrid grid;
    mutable bool foldArmed = false;
    mutable const std::vector<double>* foldTime = nullptr;
    mutable std::vector<double> foldValues;
    mutable std::vector<double> foldXs;
    mutable bool foldSummarised = false;

    // What the axes are drawn between, kept for the reason the folds are, and
    // for a sharper one: a figure asks for all three once per line per frame.
    // They were worked out on every asking. smallestPositive() costs a walk of
    // the base when a line crosses zero throughout, and a time base costs a
    // walk of itself twice over, so a frame of one line of ten million elements
    // spent 20.7 ms on a signal around zero against 0.13 ms on the same signal
    // above it, and 45.8 ms against a time base -- in a pan, where nothing any
    // of them depends on had moved. forget() is called by everything that can
    // change them.
    mutable std::optional<LineExtent> extent;
    mutable std::optional<Span> timeSpan;
    mutable std::optional<double> timePositive;

    // The columns a line was last folded onto, kept so that a pan folds only
    // what it uncovers. See ColumnGrid.
    mutable std::optional<ColumnGrid> barGrid;
    mutable std::vector<double> barLows;
    mutable std::vector<double> barHighs;
    mutable std::vector<unsigned char> barKinds;

    // A scatter's points: the tree, which is the copy, and is ordered again
    // for other scales when it is asked for them; the points still arriving;
    // and what they span, worked out once when they are finished -- nothing
    // the plot's x or a time base does changes where a point is.
    mutable PointTree points;
    std::vector<double> arrivingX;
    std::vector<double> arrivingY;
    LineExtent pointY;
    Span pointX{0.0, 0.0};
    double pointXPositive = 0.0;
    LineExtent values;

    void forget() const
    {
        barGrid.reset();
        extent.reset();
        timeSpan.reset();
        timePositive.reset();
        summaryColumns = -1;
        window.reset();
        closer.clear();
        closerAt.clear();
        foldArmed = false;
        grid.clear();
    }
};

Plot::Plot() = default;
Plot::~Plot() = default;
Plot::Plot(Plot&&) noexcept = default;
Plot& Plot::operator=(Plot&&) noexcept = default;

Plot::Line& Plot::at(int line)
{
    if (line < 0 || line >= lineCount()) {
        throw std::out_of_range("plotcore::Plot: no such line");
    }
    return *lines_[static_cast<std::size_t>(line)];
}

const Plot::Line& Plot::at(int line) const
{
    if (line < 0 || line >= lineCount()) {
        throw std::out_of_range("plotcore::Plot: no such line");
    }
    return *lines_[static_cast<std::size_t>(line)];
}

int Plot::lineCount() const
{
    return static_cast<int>(lines_.size());
}

long long Plot::budgetDoubles() const
{
    const long long bytes =
        budgetBytes_ > 0 ? budgetBytes_ : allowance(appetite_, physicalMemory());
    return bytes / static_cast<long long>(sizeof(double));
}

int Plot::lineKindCount() const
{
    int count = 0;
    for (const std::unique_ptr<Line>& line : lines_) {
        count += line->kind == LineKind::Line ? 1 : 0;
    }
    return count;
}

long long Plot::pointDoubles() const
{
    long long held = 0;
    for (const std::unique_ptr<Line>& line : lines_) {
        if (line->kind == LineKind::Points) {
            held += (line->points.bytes() + static_cast<long long>(sizeof(double)) - 1) /
                    static_cast<long long>(sizeof(double));
        }
    }
    return held;
}

long long Plot::lineBudget() const
{
    // A scatter is held whole and cannot give anything back, so the lines
    // share what the scatters leave -- and never less than a sixteenth of the
    // budget between them, or one large scatter would hold every line beside
    // it at its coarsest.
    const long long budget = budgetDoubles();
    const long long points = pointDoubles();
    return points > 0 ? std::max(budget - points, budget / 16) : budget;
}

long long Plot::share() const
{
    return lineBudget() / std::max(lineKindCount(), 1);
}

long long Plot::heldDoubles() const
{
    long long held = 0;
    for (const std::unique_ptr<Line>& line : lines_) {
        held += static_cast<long long>(line->pyramid.doubles() + line->time.size());
    }
    return held + pointDoubles();
}

void Plot::fitAll()
{
    // The free direction only. A share that has shrunk is honoured at once and
    // exactly; one that has grown would need elements no pyramid still has,
    // which only a reader can give back -- and a reader reaches below the base
    // anyway, so the picture is the same either way.
    const long long each = share();
    for (const std::unique_ptr<Line>& line : lines_) {
        if (!line->finished || line->pyramid.empty() || line->kind != LineKind::Line) {
            continue;
        }
        const long long was = line->pyramid.baseBucket();
        (void)fitToBudget(line->pyramid, each);
        if (line->pyramid.baseBucket() != was) {
            line->forget();
        }
    }
}

int Plot::addLine(const double* values, long long count, std::string name)
{
    auto line = std::make_unique<Line>();
    line->name = std::move(name);
    const long long each = lineBudget() / (lineKindCount() + 1);
    if (values != nullptr && count > 0) {
        line->pyramid = pyramidOf(values, count, baseBucketFor(count, each));
        line->length = line->pyramid.length;
    }
    line->finished = true;
    lines_.push_back(std::move(line));
    fitAll();
    return lineCount() - 1;
}

int Plot::beginLine(long long count, std::string name)
{
    auto line = std::make_unique<Line>();
    line->name = std::move(name);
    const long long length = std::max<long long>(count, 0);
    const long long each = lineBudget() / (lineKindCount() + 1);
    line->builder = std::make_unique<PyramidBuilder>(length, baseBucketFor(length, each));
    line->length = length;
    lines_.push_back(std::move(line));
    return lineCount() - 1;
}

void Plot::appendToLine(int line, const double* values, long long count)
{
    Line& held = at(line);
    if (held.builder != nullptr) {
        held.builder->add(values, count);
    }
}

void Plot::finishLine(int line)
{
    Line& held = at(line);
    if (held.kind == LineKind::Points) {
        if (held.finished) {
            return;
        }
        held.points = pointTreeOf(held.arrivingX.data(), held.arrivingY.data(), nullptr, nullptr,
                                  static_cast<long long>(held.arrivingX.size()), false, false);
        held.arrivingX = {};
        held.arrivingY = {};
        held.length = held.points.size();
        held.finished = true;
        held.pointY = extentOf(held.points.y);
        const LineExtent xs = extentOf(held.points.x);
        held.pointX = xs.finite ? Span{xs.minimum, xs.maximum} : Span{0.0, 0.0};
        held.pointXPositive = xs.positive ? xs.positiveMinimum : 0.0;
        fitAll();
        return;
    }
    if (held.builder == nullptr) {
        return;
    }
    held.pyramid = held.builder->finish();
    held.length = held.pyramid.length;
    held.builder.reset();
    held.finished = true;
    held.forget();
    fitAll();
}

void Plot::setLineReader(int line, LineReader reader)
{
    Line& held = at(line);
    if (held.kind == LineKind::Points) {
        throw std::logic_error("plotcore::Plot: a scatter is held whole and reads nothing");
    }
    held.reader = std::move(reader);
    held.forget();
}

int Plot::addPoints(const double* x, const double* y, long long count, std::string name)
{
    if (count > kMaxScatterPoints) {
        throw std::length_error("plotcore::Plot: a scatter holds at most 2^31 - 2 points");
    }
    const int line = beginPoints(count, std::move(name));
    appendPoints(line, x, y, count);
    finishLine(line);
    return line;
}

int Plot::beginPoints(long long count, std::string name)
{
    if (count > kMaxScatterPoints) {
        throw std::length_error("plotcore::Plot: a scatter holds at most 2^31 - 2 points");
    }
    auto line = std::make_unique<Line>();
    line->name = std::move(name);
    line->kind = LineKind::Points;
    line->length = std::max<long long>(count, 0);
    line->arrivingX.reserve(static_cast<std::size_t>(line->length));
    line->arrivingY.reserve(static_cast<std::size_t>(line->length));
    lines_.push_back(std::move(line));
    return lineCount() - 1;
}

void Plot::appendPoints(int line, const double* x, const double* y, long long count)
{
    Line& held = at(line);
    if (held.kind != LineKind::Points || held.finished) {
        throw std::logic_error("plotcore::Plot: points go to a scatter still arriving");
    }
    if (count <= 0) {
        return;
    }
    if (x == nullptr || y == nullptr) {
        throw std::invalid_argument("plotcore::Plot: points need an x and a y each");
    }
    if (static_cast<long long>(held.arrivingX.size()) + count > kMaxScatterPoints) {
        throw std::length_error("plotcore::Plot: a scatter holds at most 2^31 - 2 points");
    }
    held.arrivingX.insert(held.arrivingX.end(), x, x + count);
    held.arrivingY.insert(held.arrivingY.end(), y, y + count);
}

void Plot::setPointValues(int line, const double* values, long long count)
{
    Line& held = at(line);
    if (held.kind != LineKind::Points || !held.finished) {
        throw std::logic_error("plotcore::Plot: values go to a finished scatter");
    }
    if (values == nullptr || count <= 0) {
        setTreeValues(held.points, {});
        held.values = {};
        return;
    }
    if (count != held.points.size()) {
        throw std::invalid_argument("plotcore::Plot: a value a point");
    }
    const std::span<const double> given(values, static_cast<std::size_t>(count));
    setTreeValues(held.points, given);
    held.values = extentOf(held.points.values);
}

void Plot::setPointColours(int line, const std::uint8_t* rgba, long long count)
{
    Line& held = at(line);
    if (held.kind != LineKind::Points || !held.finished) {
        throw std::logic_error("plotcore::Plot: colours go to a finished scatter");
    }
    if (rgba == nullptr || count <= 0) {
        setTreeColours(held.points, {});
        return;
    }
    if (count != held.points.size()) {
        throw std::invalid_argument("plotcore::Plot: a colour a point");
    }
    setTreeColours(held.points,
                   std::span<const std::uint8_t>(rgba, 4 * static_cast<std::size_t>(count)));
}

LineKind Plot::kind(int line) const
{
    return at(line).kind;
}

LineExtent Plot::valueExtent(int line) const
{
    // Points that carry colours of their own are not coloured by their values,
    // and run no colour axis over them.
    const Line& held = at(line);
    return held.kind == LineKind::Points && !held.points.values.empty() && held.points.rgba.empty()
               ? held.values
               : LineExtent{};
}

const PointTree* Plot::drawPoints(int line, bool xLog, bool yLog) const
{
    const Line& held = at(line);
    if (held.kind != LineKind::Points || !held.finished) {
        return nullptr;
    }
    rebuildPointTree(held.points, xLog, yLog);
    return &held.points;
}

void Plot::removeLine(int line)
{
    (void)at(line);
    lines_.erase(lines_.begin() + line);
}

void Plot::clear()
{
    lines_.clear();
}

std::string Plot::name(int line) const
{
    return at(line).name;
}

void Plot::setName(int line, std::string name)
{
    at(line).name = std::move(name);
}

long long Plot::length(int line) const
{
    return at(line).length;
}

bool Plot::ready(int line) const
{
    return at(line).finished;
}

void Plot::setIndexAxis(double start, double step)
{
    if (!std::isfinite(start) || !std::isfinite(step) || step == 0.0) {
        return;
    }
    indexStart_ = start;
    indexStep_ = step;
    for (const std::unique_ptr<Line>& line : lines_) {
        line->forget();
    }
}

void Plot::setTime(const double* values, long long count)
{
    time_.assign(values, values + std::max<long long>(values == nullptr ? 0 : count, 0));
    orderOf(time_, timeSorted_, timeAscending_);
    for (const std::unique_ptr<Line>& line : lines_) {
        line->forget();
    }
}

void Plot::setLineTime(int line, const double* values, long long count)
{
    Line& held = at(line);
    if (held.kind == LineKind::Points) {
        throw std::logic_error("plotcore::Plot: a scatter's x is its own");
    }
    held.time.assign(values, values + std::max<long long>(values == nullptr ? 0 : count, 0));
    orderOf(held.time, held.timeSorted, held.timeAscending);
    held.forget();
}

bool Plot::hasTime(int line) const
{
    return timeOf(at(line)) != nullptr;
}

const std::vector<double>* Plot::timeOf(const Line& line) const
{
    if (!line.time.empty()) {
        return &line.time;
    }
    if (!time_.empty()) {
        return &time_;
    }
    return nullptr;
}

void Plot::setAppetite(Appetite appetite)
{
    appetite_ = appetite;
    fitAll();
}

void Plot::setBudgetBytes(long long bytes)
{
    budgetBytes_ = std::max<long long>(bytes, 0);
    fitAll();
}

long long Plot::baseBucket(int line) const
{
    return at(line).pyramid.baseBucket();
}

void Plot::setPaneColumns(int columns)
{
    columns_ = std::max(columns, 1);
}

void Plot::setView(double xLow, double xHigh, bool xLog)
{
    if (!std::isfinite(xLow) || !std::isfinite(xHigh) || !(xHigh > xLow)) {
        viewSet_ = false;
        return;
    }
    viewLow_ = xLow;
    viewHigh_ = xHigh;
    xLog_ = xLog;
    viewSet_ = true;
}

LineExtent Plot::extent(int line) const
{
    const Line& held = at(line);
    if (held.kind == LineKind::Points) {
        return held.finished ? held.pointY : LineExtent{};
    }
    LineExtent found;
    if (!held.finished || held.pyramid.empty()) {
        return found;
    }
    if (held.extent.has_value()) {
        return *held.extent;
    }
    ++wholeLineWalks_;
    const Extremes all = extremesOver(held.pyramid, 0, held.pyramid.length);
    found.finite = all.found();
    found.minimum = all.lowest;
    found.maximum = all.highest;
    found.positive = smallestPositive(held.pyramid, found.positiveMinimum);
    held.extent = found;
    return found;
}

Span Plot::xExtent(int line) const
{
    const Line& held = at(line);
    if (held.kind == LineKind::Points) {
        return held.pointX;
    }
    const long long count = held.length;
    if (const std::vector<double>* time = timeOf(held)) {
        if (held.timeSpan.has_value()) {
            return *held.timeSpan;
        }
        ++wholeLineWalks_;
        const auto drawn = static_cast<std::size_t>(std::min<long long>(count, time->size()));
        double low = std::numeric_limits<double>::infinity();
        double high = -std::numeric_limits<double>::infinity();
        for (std::size_t i = 0; i < drawn; ++i) {
            const double t = (*time)[i];
            if (std::isfinite(t)) {
                low = std::min(low, t);
                high = std::max(high, t);
            }
        }
        held.timeSpan = low <= high ? Span{low, high} : Span{0.0, 0.0};
        return *held.timeSpan;
    }
    const double first = indexStart_;
    const double last =
        indexStart_ + static_cast<double>(std::max<long long>(count - 1, 0)) * indexStep_;
    return {std::min(first, last), std::max(first, last)};
}

double Plot::xPositiveMinimum(int line) const
{
    const Line& held = at(line);
    if (held.kind == LineKind::Points) {
        return held.pointXPositive;
    }
    if (const std::vector<double>* time = timeOf(held)) {
        if (held.timePositive.has_value()) {
            return *held.timePositive;
        }
        ++wholeLineWalks_;
        const auto drawn = static_cast<std::size_t>(std::min<long long>(held.length, time->size()));
        double best = 0.0;
        for (std::size_t i = 0; i < drawn; ++i) {
            const double t = (*time)[i];
            if (t > 0.0 && std::isfinite(t) && (best == 0.0 || t < best)) {
                best = t;
            }
        }
        held.timePositive = best;
        return best;
    }
    const Span span = xExtent(line);
    return gridPositiveMin(span.low, span.high, indexStep_);
}

bool Plot::positionsOf(const Line& line, double xLow, double xHigh, double& low, double& high) const
{
    if (const std::vector<double>* time = timeOf(line)) {
        const bool sorted = time == &line.time ? line.timeSorted : timeSorted_;
        const bool ascending = time == &line.time ? line.timeAscending : timeAscending_;
        if (!sorted) {
            return false;
        }
        const long long count =
            std::min<long long>(line.length, static_cast<long long>(time->size()));
        const long long a = before(*time, count, ascending, ascending ? xLow : xHigh);
        const long long b = before(*time, count, ascending, ascending ? xHigh : xLow);
        // One element either side, so the line enters and leaves the pane at
        // its edges.
        low = static_cast<double>(std::max<long long>(a - 1, 0));
        high = static_cast<double>(std::min<long long>(b + 1, count));
        return true;
    }
    low = (xLow - indexStart_) / indexStep_;
    high = (xHigh - indexStart_) / indexStep_;
    if (low > high) {
        std::swap(low, high);
    }
    return std::isfinite(low) && std::isfinite(high);
}

void Plot::foldSummary(Line& line) const
{
    if (line.summaryColumns == columns_) {
        return;
    }
    line.summary.clear();
    line.summaryAt.clear();
    if (!fillWhole(line.pyramid, columns_, line.summary, line.stride, line.step, &line.summaryAt)) {
        line.stride = 1;
        line.step = 1.0;
    }
    line.summaryColumns = columns_;
}

void Plot::foldCloser(Line& line) const
{
    double low = 0.0;
    double high = 0.0;
    if (!viewSet_ || !positionsOf(line, viewLow_, viewHigh_, low, high)) {
        line.window.reset();
        return;
    }
    const auto length = static_cast<double>(line.length);
    low = std::clamp(low, 0.0, length);
    high = std::clamp(high, 0.0, length);
    // The drawn run's resolution, as the viewer's: a run twice the pane, at
    // the bucket that puts one of them on each column of it.
    const int buckets = 2 * columns_;
    std::optional<PlotWindow> want = windowFor(low, high, line.length, buckets);
    if (!want.has_value()) {
        line.window.reset();
        return;
    }
    const long long base = line.pyramid.baseBucket();
    if (want->bucket < base && !line.reader) {
        // Finer than anything held, and nothing to read it from: the finest
        // run the pyramid can fold, which is a run at its base. Asked for as
        // the span that puts one base bucket in each of `buckets` -- windowFor
        // rounds its bucket up to a power of two, and the base is one, so that
        // is the base exactly. Dividing the bucket count by how far short the
        // wanted bucket fell is not: the rounding can leave it short again,
        // and the closer look then fell all the way back to the summary --
        // two points across a window of a hundred elements.
        want =
            windowFor(low, low + static_cast<double>(base) * buckets / 2.0, line.length, buckets);
        if (!want.has_value() || want->bucket != base) {
            line.window.reset();
            return;
        }
    }
    if (line.window == want) {
        return;
    }
    line.closer.clear();
    line.closerAt.clear();
    if (want->bucket >= base) {
        if (!fillWindow(line.pyramid, *want, line.closer, &line.closerAt)) {
            line.window.reset();
            return;
        }
    }
    else {
        // Below the base: the reader, for this run and nothing else -- and the
        // element after it, where the line has one, because that is the seam
        // of the run's last bucket (see Levels.hpp). A run drawn sample for
        // sample has no buckets and needs none.
        const long long take = std::min(want->span, line.length - want->first);
        const bool seamed = want->bucket > 1 && want->first + take < line.length;
        std::vector<double> raw(static_cast<std::size_t>(std::max<long long>(take, 0)) +
                                (seamed ? 1 : 0));
        if (take <= 0 ||
            !line.reader(want->first, static_cast<long long>(raw.size()), raw.data())) {
            line.window.reset();
            return;
        }
        if (want->bucket == 1) {
            line.closer = std::move(raw);
        }
        else {
            reduceBucketsLocated(raw.data(), take, want->bucket, want->first, line.closer,
                                 line.closerAt,
                                 seamed ? raw.back() : std::numeric_limits<double>::quiet_NaN());
        }
    }
    line.window = want;
}

bool Plot::foldLog(Line& line) const
{
    if (!xLog_ || !viewSet_) {
        return false;
    }
    const std::vector<double>* time = timeOf(line);
    const int mode = time != nullptr ? 1 : 0;
    if (line.foldArmed && line.foldTime == time &&
        line.grid.serves(indexStart_, indexStep_, mode, columns_, viewLow_, viewHigh_)) {
        return true;
    }
    line.foldArmed = false;
    line.grid.remake(indexStart_, indexStep_, mode, columns_, viewLow_, viewHigh_);
    if (!line.grid.columns.has_value()) {
        return false;
    }
    std::vector<double> edges;
    if (time == nullptr) {
        edgesAlong(*line.grid.columns, indexStart_, indexStep_, edges);
    }
    else {
        const bool sorted = time == &line.time ? line.timeSorted : timeSorted_;
        const bool ascending = time == &line.time ? line.timeAscending : timeAscending_;
        if (!sorted) {
            return false;
        }
        const long long count =
            std::min<long long>(line.length, static_cast<long long>(time->size()));
        for (long long k = line.grid.columns->first; k <= line.grid.columns->last; ++k) {
            edges.push_back(
                static_cast<double>(before(*time, count, ascending, line.grid.columns->edge(k))));
        }
        if (!ascending) {
            std::reverse(edges.begin(), edges.end());
        }
    }
    ColumnFold fold;
    foldColumns(line.pyramid, edges, fold);
    line.foldValues = std::move(fold.values);
    line.foldXs.resize(fold.positions.size());
    for (std::size_t i = 0; i < fold.positions.size(); ++i) {
        const double p = fold.positions[i];
        if (time == nullptr) {
            line.foldXs[i] = indexStart_ + p * indexStep_;
        }
        else {
            const auto index = static_cast<std::size_t>(std::max(p, 0.0));
            line.foldXs[i] =
                index < time->size() ? (*time)[index] : std::numeric_limits<double>::quiet_NaN();
        }
    }
    line.foldSummarised = fold.summarised;
    line.foldTime = time;
    line.foldArmed = true;
    return true;
}

PlotLine Plot::drawLine(int line) const
{
    const Line& held = at(line);
    PlotLine drawn;
    if (!held.finished || held.pyramid.empty() || held.kind != LineKind::Line) {
        return drawn;
    }
    auto& line_ = const_cast<Line&>(held);
    if (foldLog(line_)) {
        drawn.values = held.foldValues.data();
        drawn.count = static_cast<long long>(held.foldValues.size());
        drawn.xs = held.foldXs.data();
        drawn.summarised = held.foldSummarised;
        return drawn;
    }
    foldSummary(line_);
    foldCloser(line_);
    if (held.window.has_value()) {
        double low = 0.0;
        double high = 0.0;
        if (positionsOf(held, viewLow_, viewHigh_, low, high) &&
            held.window->covers(std::clamp(low, 0.0, static_cast<double>(held.length)),
                                std::clamp(high, 0.0, static_cast<double>(held.length)))) {
            drawn.values = held.closer.data();
            drawn.count = static_cast<long long>(held.closer.size());
            drawn.positionStart = static_cast<double>(held.window->first);
            drawn.positionStep =
                held.window->bucket == 1 ? 1.0 : static_cast<double>(held.window->bucket) / 2.0;
            if (!held.closerAt.empty() && held.closerAt.size() == held.closer.size()) {
                drawn.positions = held.closerAt.data();
            }
            drawn.summarised = held.window->bucket > 1;
            return drawn;
        }
    }
    drawn.values = held.summary.data();
    drawn.count = static_cast<long long>(held.summary.size());
    drawn.positionStep = held.step;
    if (!held.summaryAt.empty() && held.summaryAt.size() == held.summary.size()) {
        drawn.positions = held.summaryAt.data();
    }
    drawn.summarised = held.stride > 1;
    return drawn;
}

DrawnColumns Plot::drawColumns(int line, const ColumnGrid& grid) const
{
    const Line& held = at(line);
    if (!held.finished || held.pyramid.empty() || grid.count <= 0 || held.kind != LineKind::Line) {
        return {};
    }
    const std::vector<double>* time = timeOf(held);
    bool ascending = true;
    long long count = held.length;
    if (time != nullptr) {
        // A time base that doubles back has no column a run of elements could
        // be: the samples of one pixel are from all over the line.
        if (!(time == &held.time ? held.timeSorted : timeSorted_)) {
            return {};
        }
        ascending = time == &held.time ? held.timeAscending : timeAscending_;
        count = std::min<long long>(held.length, static_cast<long long>(time->size()));
    }
    // The element an edge falls before. Each element is in exactly one column,
    // whichever way the line runs along x: a line running right to left has
    // its elements in descending order across the columns, and the same run of
    // them lies between two edges.
    const auto elementAt = [&](long long k) {
        const double x = grid.edge(k);
        if (time != nullptr) {
            return before(*time, count, ascending, x);
        }
        const double position = std::ceil((x - indexStart_) / indexStep_);
        return static_cast<long long>(std::clamp(position, 0.0, static_cast<double>(count)));
    };

    // What is kept, moved to where this grid wants it, and the run of columns
    // that is left to fold.
    int from = 0;
    int to = grid.count;
    if (held.barGrid.has_value() && held.barGrid->sameColumns(grid)) {
        const long long along = grid.first - held.barGrid->first;
        const auto kept = static_cast<long long>(grid.count) - std::abs(along);
        if (kept > 0) {
            const auto move = [&](auto& values) {
                if (along > 0) {
                    std::copy(values.begin() + along, values.end(), values.begin());
                }
                else if (along < 0) {
                    std::copy_backward(values.begin(), values.end() + along, values.end());
                }
            };
            move(held.barLows);
            move(held.barHighs);
            move(held.barKinds);
            from = along > 0 ? static_cast<int>(kept) : 0;
            to = along > 0 ? grid.count : static_cast<int>(-along);
        }
    }
    held.barGrid.reset();
    held.barLows.resize(static_cast<std::size_t>(grid.count));
    held.barHighs.resize(static_cast<std::size_t>(grid.count));
    held.barKinds.resize(static_cast<std::size_t>(grid.count));

    // Asked of every column before any is folded, because one that cannot be
    // drawn means a stroke and not a bar. A column with no element inside the
    // line has nothing to draw and a stroke crossing it: that is every column
    // under a sample a pixel, the sparse end of a logarithmic axis, and a
    // pause in a time base. One narrower than a base bucket would come out as
    // that bucket, wider than itself, and its neighbours would too.
    const long long base = held.pyramid.baseBucket();
    const auto fresh = static_cast<std::size_t>(to - from);
    std::vector<long long> edges(fresh + 1);
    for (std::size_t i = 0; i <= fresh; ++i) {
        edges[i] = elementAt(grid.first + from + static_cast<long long>(i));
    }
    const auto undrawable = [&](long long first, long long last) {
        return first > 0 && last < count && last - first < base;
    };
    if (!grid.partial) {
        for (std::size_t i = 0; i < fresh; ++i) {
            if (undrawable(std::min(edges[i], edges[i + 1]), std::max(edges[i], edges[i + 1]))) {
                return {};
            }
        }
    }

    const auto foldOf = [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            const long long first = std::min(edges[i], edges[i + 1]);
            const long long last = std::max(edges[i], edges[i + 1]);
            const auto column = static_cast<std::size_t>(from) + i;
            double low = std::numeric_limits<double>::quiet_NaN();
            double high = low;
            held.barKinds[column] =
                undrawable(first, last)
                    ? kColumnThin
                    : (first < last && (first == 0 || last == count) ? kColumnCut : kColumnWhole);
            if (first < last && held.barKinds[column] != kColumnThin) {
                const Extremes found = extremesOver(held.pyramid, first, last);
                if (found.found()) {
                    low = found.lowest;
                    high = found.highest;
                }
            }
            held.barLows[column] = low;
            held.barHighs[column] = high;
        }
    };
    // Over the pool when a zoom has to fold a whole pane of them, which on one
    // thread is a sizeable part of a frame once there are a few dozen lines. A
    // pan folds a column or two, and is not worth waking anything for.
    constexpr std::size_t kAlone = 256;
    if (fresh < kAlone) {
        foldOf(0, fresh);
    }
    else {
        const long long pieces =
            std::min<long long>(poolThreads(), static_cast<long long>(fresh / (kAlone / 2)));
        parallelFor(pieces, [&](long long piece) {
            const auto at = static_cast<std::size_t>(piece);
            const auto of = static_cast<std::size_t>(pieces);
            foldOf(fresh * at / of, fresh * (at + 1) / of);
        });
    }
    columnsFolded_ += static_cast<long long>(fresh);
    held.barGrid = grid;

    DrawnColumns drawn;
    drawn.lows = held.barLows.data();
    drawn.highs = held.barHighs.data();
    drawn.kinds = held.barKinds.data();
    drawn.count = grid.count;
    return drawn;
}

PlotAxis Plot::drawAxis(int line) const
{
    const Line& held = at(line);
    PlotAxis axis;
    axis.start = indexStart_;
    axis.step = indexStep_;
    if (const std::vector<double>* time = timeOf(held)) {
        // Held whole, so a position is an element and its x is that element's
        // time, exactly.
        axis.values = time->data();
        axis.count = static_cast<long long>(time->size());
        axis.valueStep = 1.0;
    }
    return axis;
}

} // namespace plotcore
