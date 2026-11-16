// SPDX-FileCopyrightText: 2026 Jonas Sattler
// SPDX-License-Identifier: BSD-3-Clause

// What a scatter costs to build, and a frame of it to draw.
//
// The clouds are the ones that tell the walk's cases apart:
//
// - **uniform**: every cell of the pane holds points; the walk stops at nodes
//   the size of a cell, and what it visits is the pane's size.
// - **blob**: a bell in each coordinate, dense in the middle and sparse at the
//   edge, where the leaves are opened point by point.
// - **clusters**: a few dozen tight clusters, with points that stand at one
//   place thousands of times over -- nodes of no size, which fit any cell.
// - **decades**: uniform in the logarithm over six decades, on logarithmic
//   axes, for the tree built in axis positions.
//
// The gestures are a pan (three device pixels a frame, which walks only what it
// uncovers), a wheel going in and out (a new grid at every notch, walked whole)
// and a zoom across alone, the walk's worst case: a node fits a cell only when
// it fits the narrower of its sides.
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

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

namespace {

using namespace plotcore;
using Clock = std::chrono::steady_clock;

class Noise
{
public:
    explicit Noise(unsigned seed) : state_(seed) {}
    double operator()()
    {
        state_ = state_ * 1103515245u + 12345u;
        return static_cast<double>((state_ >> 16) & 0x7fff) / 32767.0;
    }

private:
    unsigned state_;
};

struct Cloud
{
    std::vector<double> x;
    std::vector<double> y;
};

Cloud uniform(long long count, unsigned seed)
{
    Noise noise(seed);
    Cloud c;
    c.x.resize(static_cast<std::size_t>(count));
    c.y.resize(static_cast<std::size_t>(count));
    for (std::size_t i = 0; i < c.x.size(); ++i) {
        c.x[i] = noise();
        c.y[i] = noise();
    }
    return c;
}

Cloud blob(long long count, unsigned seed)
{
    Noise noise(seed);
    Cloud c;
    c.x.resize(static_cast<std::size_t>(count));
    c.y.resize(static_cast<std::size_t>(count));
    for (std::size_t i = 0; i < c.x.size(); ++i) {
        c.x[i] = (noise() + noise() + noise() + noise()) / 4.0;
        c.y[i] = (noise() + noise() + noise() + noise()) / 4.0;
    }
    return c;
}

Cloud clusters(long long count, unsigned seed)
{
    Noise noise(seed);
    double cx[40];
    double cy[40];
    double spread[40];
    for (int k = 0; k < 40; ++k) {
        cx[k] = noise();
        cy[k] = noise();
        spread[k] = 0.001 + 0.03 * noise() * noise();
    }
    Cloud c;
    c.x.resize(static_cast<std::size_t>(count));
    c.y.resize(static_cast<std::size_t>(count));
    for (std::size_t i = 0; i < c.x.size(); ++i) {
        const int k = static_cast<int>(noise() * 39.0);
        const bool centre = i % 100 == 0;
        c.x[i] = cx[k] + (centre ? 0.0 : spread[k] * (noise() + noise() - 1.0));
        c.y[i] = cy[k] + (centre ? 0.0 : spread[k] * (noise() + noise() - 1.0));
    }
    return c;
}

Cloud decades(long long count, unsigned seed)
{
    Noise noise(seed);
    Cloud c;
    c.x.resize(static_cast<std::size_t>(count));
    c.y.resize(static_cast<std::size_t>(count));
    for (std::size_t i = 0; i < c.x.size(); ++i) {
        c.x[i] = std::pow(10.0, -3.0 + 6.0 * noise());
        c.y[i] = std::pow(10.0, -3.0 + 6.0 * noise());
    }
    return c;
}

struct Kind
{
    const char* name;
    Cloud (*make)(long long, unsigned);
    bool logarithmic;
};
const Kind kClouds[] = {
    {"uniform", uniform, false},
    {"blob", blob, false},
    {"clusters", clusters, false},
    {"decades", decades, true},
};

enum class Gesture
{
    Pan,
    Wheel,
    Across,
};

const char* nameOf(Gesture gesture)
{
    switch (gesture) {
    case Gesture::Wheel:
        return "wheel";
    case Gesture::Across:
        return "across";
    case Gesture::Pan:
        break;
    }
    return "pan";
}

struct Result
{
    double sceneMean = 0.0;
    double sceneWorst = 0.0;
    double paintMean = 0.0;
    double drawn = 0.0;
    double nodes = 0.0;
    double images = 0.0;
};

Result run(Figure& figure, const std::function<void(const Scene&)>& paint, Gesture gesture,
           int frames)
{
    figure.resetView();
    (void)figure.scene();
    const double x0 = figure.camera().viewMinX();
    const double x1 = figure.camera().viewMaxX();
    const double y0 = figure.camera().viewMinY();
    const double y1 = figure.camera().viewMaxY();
    const bool log = figure.settings().xLog;
    const auto along = [&](double from, double to, double at) {
        return log ? from * std::pow(to / from, at) : from + (to - from) * at;
    };
    // A window of the middle half, so a pan has somewhere to go.
    double left = along(x0, x1, 0.25);
    double right = along(x0, x1, 0.75);
    const double bottom = along(y0, y1, 0.25);
    const double top = along(y0, y1, 0.75);
    if (gesture == Gesture::Across) {
        right = along(left, right, 1.0 / 16.0); // sixteen times across, once up
    }
    figure.setViewRange(left, right, bottom, top);
    const Scene opening = figure.scene();
    const double column = 1.0 / std::max(opening.area.width, 1.0);

    Result result;
    const PointWalk before = figure.pointsWalked();
    for (int i = 1; i <= frames; ++i) {
        switch (gesture) {
        case Gesture::Pan:
        case Gesture::Across: {
            const double step = 3.0 * static_cast<double>(i) * column;
            figure.setViewRange(along(left, right, step), along(left, right, 1.0 + step), bottom,
                                top);
            break;
        }
        case Gesture::Wheel: {
            const double scale = i % 2 == 0 ? 1.0 : 0.8;
            figure.setViewRange(left, along(left, right, scale), bottom, top);
            break;
        }
        }
        const auto start = Clock::now();
        const Scene scene = figure.scene();
        const auto built = Clock::now();
        paint(scene);
        const auto painted = Clock::now();
        const double took = std::chrono::duration<double, std::milli>(built - start).count();
        result.sceneMean += took;
        result.sceneWorst = std::max(result.sceneWorst, took);
        result.paintMean += std::chrono::duration<double, std::milli>(painted - built).count();
        for (const DrawnLine& line : scene.lines) {
            result.images += line.image.has_value() ? 1.0 : 0.0;
        }
    }
    const auto each = static_cast<double>(frames);
    result.sceneMean /= each;
    result.paintMean /= each;
    result.images /= each;
    result.drawn = static_cast<double>(figure.pointsWalked().drawn - before.drawn) / each;
    result.nodes = static_cast<double>(figure.pointsWalked().nodes - before.nodes) / each;
    return result;
}

void setUp(Figure& figure, const Kind& kind, PointDrawing drawing)
{
    FigureSettings settings = figure.settings();
    settings.pointDrawing = drawing;
    settings.xLog = kind.logarithmic;
    settings.yLog = kind.logarithmic;
    settings.cursor = false;
    settings.legend = false;
    settings.colourBar = false;
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
    const auto none = [](const Scene&) {};
    Figure figure;
    figure.setSize(800.0, 400.0);
    const Cloud cloud = uniform(1000000, 9);
    (void)figure.plot().addPoints(cloud.x.data(), cloud.y.data(), 1000000);
    setUp(figure, kClouds[0], PointDrawing::Adaptive);
    const Result wheel = run(figure, none, Gesture::Wheel, 10);
    const Result pan = run(figure, none, Gesture::Pan, 10);
    std::printf("uniform, a million points: %.0f nodes a frame zooming, %.0f panning\n",
                wheel.nodes, pan.nodes);
    const double cells = 800.0 * 400.0;
    expect(wheel.drawn <= cells, "no more points drawn than the pane has cells");
    expect(wheel.nodes <= 5.0 * cells, "a frame walked whole visits the pane, not the points");
    expect(pan.nodes * 4.0 < wheel.nodes, "a pan walks only what it uncovers");
    expect(pan.images == 1.0, "a dense cloud is an image");
    Figure few;
    few.setSize(800.0, 400.0);
    const Cloud sparse = uniform(1000, 11);
    (void)few.plot().addPoints(sparse.x.data(), sparse.y.data(), 1000);
    setUp(few, kClouds[0], PointDrawing::Adaptive);
    const Result dots = run(few, none, Gesture::Pan, 5);
    expect(dots.images == 0.0 && dots.drawn > 100.0, "a sparse one is markers");
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
    const int width = 1100;
    const int height = 480;
    const long long sizes[] = {1000000, 10000000};
    std::printf("A scatter on a pane %d by %d, in ms. build: the tree. nodes: what a frame\n"
                "visits. drawn: the points it draws. image: the share of frames drawn as one.\n\n",
                width, height);
    std::printf("%-9s %9s %-9s %-7s %8s %8s %8s %8s %8s %9s %9s %6s\n", "cloud", "points",
                "drawing", "gesture", "build", "scene", "worst", "paint", "total", "nodes", "drawn",
                "image");
    for (const Kind& kind : kClouds) {
        for (const long long size : sizes) {
            if (quick && size > 1000000) {
                continue;
            }
            const Cloud cloud = kind.make(size, 17);
#ifdef PLOTCORE_BENCH_QT
            plotcore::qt::PlotWidget host;
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
            const auto start = Clock::now();
            (void)figure.plot().addPoints(cloud.x.data(), cloud.y.data(), size);
            const double built =
                std::chrono::duration<double, std::milli>(Clock::now() - start).count();
            for (const PointDrawing drawing : {PointDrawing::Adaptive, PointDrawing::Markers}) {
                setUp(figure, kind, drawing);
                for (const Gesture gesture : {Gesture::Pan, Gesture::Wheel, Gesture::Across}) {
                    const Result r = run(figure, paint, gesture, quick ? 5 : 20);
                    std::printf("%-9s %9lld %-9s %-7s %8.1f %8.2f %8.2f %8.2f %8.2f %9.0f %9.0f "
                                "%6.2f\n",
                                kind.name, size, pointDrawingName(drawing), nameOf(gesture), built,
                                r.sceneMean, r.sceneWorst, r.paintMean, r.sceneMean + r.paintMean,
                                r.nodes, r.drawn, r.images);
                    std::fflush(stdout);
                }
            }
        }
    }
    return 0;
}
