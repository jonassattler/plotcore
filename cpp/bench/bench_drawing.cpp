// SPDX-FileCopyrightText: 2026 Jonas Sattler
// SPDX-License-Identifier: BSD-3-Clause

// What each way of drawing a line costs, and how still it holds.
//
// Time is half of it. A way of drawing that is fast and changes its mind under
// the pointer is no good, so each gesture also reports what the reader would
// see change: the share of the line that went from one form to the other between
// two frames, and how many borders between the forms there were. A pan should
// change none of it. A wobble of a zoom should settle.
//
// The signals are the ones that have told the forms apart:
//
// - **chirp**: the demos' ten million, slow to very fast, on which a stroke
//   leaves slits and bars leave stripes.
// - **walk**: a spike every 997 elements, whose stripes as bars were what made
//   a drag look dark.
// - **noise**: a band, where bars should win outright.
// - **smooth**: a line, where a stroke should win outright.
// - **burst**: a line with a band in the middle, for the borders.
//
// `--smoke` runs a little of it and fails on a count that is wrong. Durations
// are printed and never asserted.

#include "plotcore/Figure.hpp"

#ifdef PLOTCORE_BENCH_QT
#include "plotcore/qt/PlotWidget.hpp"

#include <QApplication>
#include <QImage>
#include <QPainter>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace {

using namespace plotcore;
using Clock = std::chrono::steady_clock;

constexpr double kTau = 6.283185307179586;

/// The same stream on every machine, so two runs draw the same picture.
class Noise
{
public:
    explicit Noise(unsigned seed) : state_(seed) {}
    double operator()()
    {
        state_ = state_ * 1103515245u + 12345u;
        return static_cast<double>((state_ >> 16) & 0x7fff) / 32767.0 - 0.5;
    }

private:
    unsigned state_;
};

std::vector<double> chirp(long long count, unsigned seed)
{
    Noise noise(seed);
    std::vector<double> values(static_cast<std::size_t>(count));
    for (long long i = 0; i < count; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(count);
        values[static_cast<std::size_t>(i)] =
            std::sin(kTau * (20.0 * t + 2000.0 * t * t)) + 0.2 * noise();
    }
    return values;
}

std::vector<double> walk(long long count, unsigned seed)
{
    Noise noise(seed);
    std::vector<double> values(static_cast<std::size_t>(count));
    double at = 0.0;
    for (long long i = 0; i < count; ++i) {
        at += noise();
        values[static_cast<std::size_t>(i)] = i % 997 == 996 ? at + 500.0 : at;
    }
    return values;
}

std::vector<double> band(long long count, unsigned seed)
{
    Noise noise(seed);
    std::vector<double> values(static_cast<std::size_t>(count));
    for (long long i = 0; i < count; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(count);
        values[static_cast<std::size_t>(i)] = std::sin(kTau * 3.0 * t) + 0.3 * noise();
    }
    return values;
}

std::vector<double> smooth(long long count, unsigned /*seed*/)
{
    std::vector<double> values(static_cast<std::size_t>(count));
    for (long long i = 0; i < count; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(count);
        values[static_cast<std::size_t>(i)] =
            std::sin(kTau * 2.3 * t) * std::exp(-1.2 * t) + 0.3 * std::sin(kTau * 9.0 * t);
    }
    return values;
}

std::vector<double> burst(long long count, unsigned seed)
{
    std::vector<double> values = smooth(count, seed);
    Noise noise(seed);
    for (long long i = count * 2 / 5; i < count * 3 / 5; ++i) {
        values[static_cast<std::size_t>(i)] += noise();
    }
    return values;
}

struct Signal
{
    const char* name;
    std::vector<double> (*make)(long long, unsigned);
};
const Signal kSignals[] = {
    {"chirp", chirp}, {"walk", walk}, {"noise", band}, {"smooth", smooth}, {"burst", burst},
};

struct Load
{
    int lines;
    long long elements;
};

/// Which way a gesture goes at each frame, given the window it started from.
enum class Gesture
{
    Pan,
    YZoom,
    Wheel,
};

const char* nameOf(Gesture gesture)
{
    switch (gesture) {
    case Gesture::YZoom:
        return "y zoom";
    case Gesture::Wheel:
        return "wheel";
    case Gesture::Pan:
        break;
    }
    return "pan";
}

/// One bin per this share of the line. A form's share of the line is read off
/// these rather than off columns, which a zoom replaces at every notch.
constexpr std::size_t kBins = 1 << 14;

/// Which bins of the whole line are under a bar of the first line, and which of
/// them the pane reaches.
struct Coverage
{
    std::vector<unsigned char> barred = std::vector<unsigned char>(kBins, 0);
    std::vector<unsigned char> seen = std::vector<unsigned char>(kBins, 0);
};

Coverage coverageOf(const Scene& scene, long long elements)
{
    Coverage coverage;
    const double span = scene.x.high - scene.x.low;
    const auto binOf = [&](double x) {
        return static_cast<long long>(std::floor(x / static_cast<double>(elements) * kBins));
    };
    const auto mark = [&](std::vector<unsigned char>& bins, double from, double to) {
        const long long first = std::max<long long>(binOf(from), 0);
        const long long last = std::min<long long>(binOf(to), kBins - 1);
        for (long long bin = first; bin <= last; ++bin) {
            bins[static_cast<std::size_t>(bin)] = 1;
        }
    };
    mark(coverage.seen, scene.x.low, scene.x.high);
    if (!scene.lines.empty()) {
        const std::vector<Point>& bars = scene.lines[0].bars;
        for (std::size_t c = 0; c + 1 < bars.size(); c += 2) {
            const double left = (bars[c].x() - scene.area.x) / scene.area.width;
            const double right = (bars[c + 1].x() - scene.area.x) / scene.area.width;
            mark(coverage.barred, scene.x.low + left * span, scene.x.low + right * span);
        }
    }
    return coverage;
}

/// The share of what both frames show that changed form, as a percentage.
double changed(const Coverage& a, const Coverage& b)
{
    std::size_t both = 0;
    std::size_t differ = 0;
    for (std::size_t bin = 0; bin < kBins; ++bin) {
        if (a.seen[bin] != 0 && b.seen[bin] != 0) {
            ++both;
            differ += a.barred[bin] != b.barred[bin] ? 1 : 0;
        }
    }
    return both > 0 ? 100.0 * static_cast<double>(differ) / static_cast<double>(both) : 0.0;
}

int stretchesOf(const Scene& scene)
{
    if (scene.lines.empty() || scene.lines[0].bars.empty()) {
        return 0;
    }
    const std::vector<Point>& bars = scene.lines[0].bars;
    int stretches = 1;
    for (std::size_t c = 2; c + 1 < bars.size(); c += 2) {
        stretches += bars[c].x() > bars[c - 1].x() + 1e-6 ? 1 : 0;
    }
    return stretches;
}

struct Result
{
    double sceneMean = 0.0;
    double sceneWorst = 0.0;
    double paintMean = 0.0;
    double bars = 0.0;
    double points = 0.0;
    double stretches = 0.0;
    double flipMean = 0.0;
    double flipWorst = 0.0;
    /// What changed between a frame and the one two before it, over the last half
    /// of them. A gesture going back and forth is back where it was by then, so
    /// anything here is a picture that has not settled.
    double flipSettled = 0.0;
    double folded = 0.0;
};

#ifdef PLOTCORE_BENCH_QT
using Host = plotcore::qt::PlotWidget;
#endif

Result run(Figure& figure, const std::function<void(const Scene&)>& paint, Gesture gesture,
           long long elements, int frames)
{
    figure.resetView();
    (void)figure.scene();
    const double x0 = figure.camera().viewMinX();
    const double x1 = figure.camera().viewMaxX();
    const double y0 = figure.camera().viewMinY();
    const double y1 = figure.camera().viewMaxY();
    const double span = (x1 - x0) / 3.0;
    const double start = x0 + span;

    SceneRequest request;
    request.geometry.runs = true;
    request.geometry.bars = true;
    request.geometry.triangles = false;
    request.geometry.values = false;

    figure.setViewRange(start, start + span, y0, y1);
    const Scene opening = figure.scene(request);
    const double column = span / std::max(opening.area.width, 1.0);
    const long long foldedBefore = figure.plot().columnsFolded();

    Result result;
    Coverage last = coverageOf(opening, elements);
    Coverage twoAgo = last;
    double flips = 0.0;
    double flipsLate = 0.0;
    int late = 0;
    for (int i = 1; i <= frames; ++i) {
        switch (gesture) {
        case Gesture::Pan: {
            const double at = start + 3.0 * static_cast<double>(i) * column;
            figure.setViewRange(at, at + span, y0, y1);
            break;
        }
        case Gesture::YZoom: {
            const double half = (y1 - y0) / 2.0 * (i % 2 == 0 ? 1.0 : 0.96);
            const double middle = (y0 + y1) / 2.0;
            figure.setViewRange(start, start + span, middle - half, middle + half);
            break;
        }
        case Gesture::Wheel: {
            const double scale = i % 2 == 0 ? 1.0 : 1.25;
            figure.setViewRange(start, start + span / scale, y0, y1);
            break;
        }
        }
        const auto before = Clock::now();
        const Scene scene = figure.scene(request);
        const auto built = Clock::now();
        paint(scene);
        const auto painted = Clock::now();

        const double took = std::chrono::duration<double, std::milli>(built - before).count();
        result.sceneMean += took;
        result.sceneWorst = std::max(result.sceneWorst, took);
        result.paintMean += std::chrono::duration<double, std::milli>(painted - built).count();
        if (!scene.lines.empty()) {
            result.bars += static_cast<double>(scene.lines[0].bars.size() / 2);
            for (const std::vector<Point>& stroke : scene.lines[0].runs) {
                result.points += static_cast<double>(stroke.size());
            }
        }
        result.stretches += stretchesOf(scene);
        Coverage now = coverageOf(scene, elements);
        const double flip = changed(last, now);
        flips += flip;
        result.flipWorst = std::max(result.flipWorst, flip);
        if (i > frames / 2) {
            flipsLate += changed(twoAgo, now);
            ++late;
        }
        twoAgo = std::move(last);
        last = std::move(now);
    }
    const auto each = static_cast<double>(frames);
    result.sceneMean /= each;
    result.paintMean /= each;
    result.bars /= each;
    result.points /= each;
    result.stretches /= each;
    result.flipMean = flips / each;
    result.flipSettled = late > 0 ? flipsLate / static_cast<double>(late) : 0.0;
    result.folded = static_cast<double>(figure.plot().columnsFolded() - foldedBefore) / each;
    return result;
}

void fill(Figure& figure, const Signal& signal, const Load& load)
{
    for (int line = 0; line < load.lines; ++line) {
        const std::vector<double> values =
            signal.make(load.elements, 17u + static_cast<unsigned>(line));
        (void)figure.plot().addLine(values.data(), load.elements);
    }
}

void setUp(Figure& figure, LineDrawing drawing)
{
    FigureSettings settings = figure.settings();
    settings.drawing = drawing;
    settings.cursor = false;
    settings.legend = false;
    figure.setSettings(settings);
}

/// A little of it, failing on a count that is wrong.
int smoke()
{
    int failures = 0;
    const auto expect = [&](bool ok, const char* what) {
        std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
        failures += ok ? 0 : 1;
    };
    Figure figure;
    figure.setSize(800.0, 400.0);
    fill(figure, kSignals[4], Load{1, 400000});
    const auto none = [](const Scene&) {};
    for (const LineDrawing drawing :
         {LineDrawing::Stroke, LineDrawing::Bars, LineDrawing::Adaptive}) {
        setUp(figure, drawing);
        std::printf("%s\n", drawingName(drawing));
        const Result pan = run(figure, none, Gesture::Pan, 400000, 20);
        const Result wheel = run(figure, none, Gesture::Wheel, 400000, 20);
        const Result yZoom = run(figure, none, Gesture::YZoom, 400000, 20);
        expect(pan.flipWorst == 0.0, "a pan changes no column's form");
        expect(wheel.flipSettled == 0.0, "a zoom going back and forth settles");
        expect(yZoom.flipSettled == 0.0, "and so does a y zoom");
        if (drawing == LineDrawing::Stroke) {
            expect(pan.bars == 0.0 && pan.points > 0.0, "a stroke and nothing else");
        }
        if (drawing == LineDrawing::Bars) {
            expect(pan.bars > 0.0 && pan.points == 0.0, "bars and nothing else");
            expect(pan.folded <= 3.0, "a pan folds only the columns it uncovers");
        }
        if (drawing == LineDrawing::Adaptive) {
            expect(pan.bars > 0.0 && pan.points > 0.0, "both, on a line with a band in it");
            expect(pan.folded <= 3.0, "a pan folds only the columns it uncovers");
            expect(pan.stretches >= 1.0 && pan.stretches <= 2.0, "and one border to speak of");
        }
    }
    std::printf(failures == 0 ? "all counts as they should be\n" : "%d counts wrong\n", failures);
    return failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
#ifdef PLOTCORE_BENCH_QT
    QApplication application(argc, argv);
#endif
    bool quick = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--smoke") == 0) {
            return smoke();
        }
        quick = quick || std::strcmp(argv[i], "--quick") == 0;
    }

    const Load loads[] = {{1, 10000000}, {8, 2000000}, {64, 200000}, {400, 20000}};
    const int width = 1100;
    const int height = 480;

    std::printf("One frame of a pane %d by %d, in ms. flips: the %% of the first line on screen\n"
                "that changed form since the last frame, on average and at worst. again: since\n"
                "the frame before that, over the last half of them, which for a gesture going\n"
                "back and forth is what has not settled. strtch: stretches of bars on that line.\n"
                "\n",
                width, height);
    std::printf("%-7s %5s %9s %-9s %-7s %8s %8s %8s %8s %7s %7s %6s %6s %6s %6s\n", "signal",
                "lines", "elements", "drawing", "gesture", "scene", "worst", "paint", "total",
                "bars", "points", "strtch", "flips", "worst", "again");
    for (const Signal& signal : kSignals) {
        for (const Load& load : loads) {
            // One line of each shape shows everything but the cost of many;
            // only the first signal is worth the data for all of them.
            if (&signal != &kSignals[0] && load.lines != 1) {
                continue;
            }
            const int frames = quick ? 10 : (load.lines >= 64 ? 30 : 60);
#ifdef PLOTCORE_BENCH_QT
            Host host;
            Figure& figure = host.figure();
            QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
            const std::function<void(const Scene&)> paint = [&](const Scene& scene) {
                QPainter painter(&image);
                host.paintScene(painter, scene);
            };
#else
            Figure figure;
            const std::function<void(const Scene&)> paint = [](const Scene&) {};
#endif
            figure.setSize(width, height, 1.0);
            fill(figure, signal, load);
            for (const LineDrawing drawing :
                 {LineDrawing::Stroke, LineDrawing::Bars, LineDrawing::Adaptive}) {
                setUp(figure, drawing);
                for (const Gesture gesture : {Gesture::Pan, Gesture::YZoom, Gesture::Wheel}) {
                    const Result r = run(figure, paint, gesture, load.elements, frames);
                    std::printf("%-7s %5d %9lld %-9s %-7s %8.3f %8.3f %8.3f %8.3f %7.0f %7.0f "
                                "%6.1f %6.2f %6.2f %6.2f\n",
                                signal.name, load.lines, load.elements, drawingName(drawing),
                                nameOf(gesture), r.sceneMean, r.sceneWorst, r.paintMean,
                                r.sceneMean + r.paintMean, r.bars, r.points, r.stretches,
                                r.flipMean, r.flipWorst, r.flipSettled);
                    std::fflush(stdout);
                }
            }
        }
    }
    return 0;
}
