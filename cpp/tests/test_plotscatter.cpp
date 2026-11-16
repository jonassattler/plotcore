// SPDX-FileCopyrightText: 2026 Jonas Sattler
// SPDX-License-Identifier: BSD-3-Clause

// The point tree, asserted against the points themselves: every promise the
// head of Scatter.hpp makes, checked by brute force over clouds where each one
// matters -- uniform, piled up, with outliers, with duplicates, on a line, and
// over six decades on logarithmic axes.

#include "plotcore/Scatter.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <set>
#include <vector>

using namespace plotcore;

namespace {

/// The same stream on every machine.
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

Cloud uniform(std::size_t n, unsigned seed)
{
    Noise noise(seed);
    Cloud c;
    for (std::size_t i = 0; i < n; ++i) {
        c.x.push_back(noise());
        c.y.push_back(noise());
    }
    return c;
}

/// Piled up in the middle: the sum of four draws, about a bell.
Cloud blob(std::size_t n, unsigned seed)
{
    Noise noise(seed);
    Cloud c;
    for (std::size_t i = 0; i < n; ++i) {
        c.x.push_back((noise() + noise() + noise() + noise()) / 4.0);
        c.y.push_back((noise() + noise() + noise() + noise()) / 4.0);
    }
    return c;
}

PointTree treeOf(const Cloud& c, bool xLog = false, bool yLog = false)
{
    return pointTreeOf(c.x.data(), c.y.data(), nullptr, nullptr, static_cast<long long>(c.x.size()),
                       xLog, yLog);
}

/// A frame over `low`..`high` on both axes, `across` by `up` cells.
PointFrame frameOver(double xLow, double xHigh, double yLow, double yHigh, int across, int up)
{
    PointFrame frame;
    frame.xLow = xLow;
    frame.xHigh = xHigh;
    frame.yLow = yLow;
    frame.yHigh = yHigh;
    frame.cellX = (xHigh - xLow) / across;
    frame.cellY = (yHigh - yLow) / up;
    return frame;
}

std::vector<std::uint32_t> drawn(const PointTree& tree, const PointFrame& frame,
                                 PointWalk* walk = nullptr, const PointSelection* chosen = nullptr)
{
    PointScratch scratch;
    std::vector<std::uint32_t> out;
    drawnPoints(tree, chosen, frame, scratch, out, walk);
    return out;
}

/// Which cell a position is in, along one axis.
long long cellOf(double position, double cell)
{
    return static_cast<long long>(std::floor(position / cell));
}

} // namespace

TEST_CASE("a tree holds every point once and says which element each was", "[scatter]")
{
    const Cloud c = blob(20000, 7);
    const PointTree tree = treeOf(c);
    REQUIRE(tree.size() == 20000);
    REQUIRE(tree.drawable == 20000);
    std::vector<int> seen(20000, 0);
    for (std::size_t s = 0; s < tree.x.size(); ++s) {
        const std::uint32_t e = tree.element[s];
        ++seen[e];
        CHECK(tree.x[s] == c.x[e]);
        CHECK(tree.y[s] == c.y[e]);
    }
    CHECK(std::all_of(seen.begin(), seen.end(), [](int k) { return k == 1; }));
}

TEST_CASE("every node's last point is the last of the points under it, inside its box", "[scatter]")
{
    const Cloud c = blob(30000, 11);
    const PointTree tree = treeOf(c);
    REQUIRE_FALSE(tree.empty());
    CHECK(tree.nodes[0].count == tree.drawable);
    for (const PointNode& node : tree.nodes) {
        std::uint32_t last = node.first;
        for (std::uint32_t s = node.first; s < node.first + node.count; ++s) {
            CHECK(tree.x[s] >= node.xLow);
            CHECK(tree.x[s] <= node.xHigh);
            CHECK(tree.y[s] >= node.yLow);
            CHECK(tree.y[s] <= node.yHigh);
            if (tree.element[s] > tree.element[last]) {
                last = s;
            }
        }
        CHECK(node.last == last);
        if (node.children() > 0) {
            std::uint32_t held = 0;
            for (std::uint32_t k = node.child(); k < node.child() + node.children(); ++k) {
                CHECK(tree.nodes[k].first == node.first + held);
                held += tree.nodes[k].count;
            }
            CHECK(held == node.count);
        }
        else {
            const bool atOnePlace = node.xLow == node.xHigh && node.yLow == node.yHigh;
            CHECK((node.count <= static_cast<std::uint32_t>(kPointLeaf) || atOnePlace));
        }
    }
}

TEST_CASE("a point with no place on the scales is kept after the tree and never drawn", "[scatter]")
{
    Cloud c = uniform(1000, 3);
    c.x[10] = std::nan("");
    c.y[20] = -1.0;
    c.x[30] = 0.0;
    const PointTree linear = treeOf(c);
    CHECK(linear.drawable == 999); // the NaN
    const PointTree logs = treeOf(c, true, true);
    CHECK(logs.drawable == 997); // the NaN, the negative y and the zero x
    for (std::uint32_t s = logs.drawable; s < logs.size(); ++s) {
        const std::uint32_t e = logs.element[s];
        CHECK((e == 10 || e == 20 || e == 30));
    }
    const std::vector<std::uint32_t> out = drawn(logs, frameOver(-4.0, 0.1, -4.0, 0.1, 400, 400));
    for (const std::uint32_t s : out) {
        CHECK(s < logs.drawable);
    }
}

TEST_CASE("at most one point is drawn a cell, and it is drawn after every other there", "[scatter]")
{
    const Cloud c = blob(200000, 5);
    const PointTree tree = treeOf(c);
    const PointFrame frame = frameOver(0.0, 1.0, 0.0, 1.0, 300, 200);
    const std::vector<std::uint32_t> out = drawn(tree, frame);
    std::set<std::pair<long long, long long>> cells;
    std::uint32_t previous = 0;
    for (std::size_t i = 0; i < out.size(); ++i) {
        const std::uint32_t s = out[i];
        CHECK(
            cells.insert({cellOf(tree.x[s], frame.cellX), cellOf(tree.y[s], frame.cellY)}).second);
        if (i > 0) {
            CHECK(tree.element[s] > previous); // in the order they are drawn
        }
        previous = tree.element[s];
    }
}

TEST_CASE("a point with nobody within a cell of it is always drawn", "[scatter]")
{
    // A dense blob, and outliers far out in the corners, each alone.
    Cloud c = blob(100000, 9);
    const std::vector<std::pair<double, double>> alone{
        {0.001, 0.001}, {0.999, 0.002}, {0.003, 0.998}, {0.997, 0.996}, {0.5, 0.0005}};
    for (const auto& [x, y] : alone) {
        c.x.push_back(x);
        c.y.push_back(y);
    }
    const PointTree tree = treeOf(c);
    for (const int across : {100, 640, 3000}) {
        const PointFrame frame = frameOver(0.0, 1.0, 0.0, 1.0, across, across * 2 / 3);
        const std::vector<std::uint32_t> out = drawn(tree, frame);
        std::set<std::uint32_t> elements;
        for (const std::uint32_t s : out) {
            elements.insert(tree.element[s]);
        }
        for (std::uint32_t k = 0; k < alone.size(); ++k) {
            CHECK(elements.count(100000 + k) == 1);
        }
    }
}

TEST_CASE("every point lies within two cells of a drawn point at least as late as itself",
          "[scatter]")
{
    for (const Cloud& c : {uniform(60000, 1), blob(60000, 2)}) {
        const PointTree tree = treeOf(c);
        for (const int across : {80, 500, 2400}) {
            const PointFrame frame = frameOver(0.0, 1.0, 0.0, 1.0, across, across / 2);
            const std::vector<std::uint32_t> out = drawn(tree, frame);
            // The latest drawn element in each cell, to look up around a point.
            std::map<std::pair<long long, long long>, std::uint32_t> latest;
            for (const std::uint32_t s : out) {
                latest[{cellOf(tree.x[s], frame.cellX), cellOf(tree.y[s], frame.cellY)}] =
                    tree.element[s];
            }
            long long unmet = 0;
            for (std::size_t s = 0; s < tree.x.size(); ++s) {
                const long long cx = cellOf(tree.x[s], frame.cellX);
                const long long cy = cellOf(tree.y[s], frame.cellY);
                bool met = false;
                for (long long dx = -2; dx <= 2 && !met; ++dx) {
                    for (long long dy = -2; dy <= 2 && !met; ++dy) {
                        const auto found = latest.find({cx + dx, cy + dy});
                        met = found != latest.end() && found->second >= tree.element[s];
                    }
                }
                unmet += met ? 0 : 1;
            }
            CHECK(unmet == 0);
        }
    }
}

TEST_CASE("a pan changes none of the points drawn away from the pane's edges", "[scatter]")
{
    const Cloud c = blob(300000, 13);
    const PointTree tree = treeOf(c);
    const PointFrame at = frameOver(0.2, 0.6, 0.3, 0.7, 400, 400);
    PointFrame moved = at;
    // A pan by a third of a cell and a few cells, which is no whole number of
    // either: the cells are the data's, not the pane's.
    const double dx = 7.34 * at.cellX;
    const double dy = -3.71 * at.cellY;
    moved.xLow += dx;
    moved.xHigh += dx;
    moved.yLow += dy;
    moved.yHigh += dy;
    const auto inner = [&](const std::vector<std::uint32_t>& out) {
        std::set<std::uint32_t> kept;
        for (const std::uint32_t s : out) {
            const double x = tree.x[s];
            const double y = tree.y[s];
            if (x > moved.xLow + 4 * at.cellX && x < at.xHigh - 4 * at.cellX &&
                y > at.yLow + 4 * at.cellY && y < moved.yHigh - 4 * at.cellY) {
                kept.insert(tree.element[s]);
            }
        }
        return kept;
    };
    const std::set<std::uint32_t> before = inner(drawn(tree, at));
    const std::set<std::uint32_t> after = inner(drawn(tree, moved));
    REQUIRE(before.size() > 10000);
    CHECK(before == after);
}

TEST_CASE("what a frame visits is bounded by the pane, not by the points", "[scatter]")
{
    // At a few points a cell the walk opens the leaves; past that it stops at
    // nodes the size of a cell, however many points are under them. A node
    // that fits a cell has a quarter of its area at the least, so it is a few
    // nodes a cell -- four and a little here, where the cells are half again
    // as tall as they are wide.
    const int across = 600;
    const int up = 400;
    const long long cells = static_cast<long long>(across) * up;
    for (const std::size_t size :
         {std::size_t{250000}, std::size_t{1000000}, std::size_t{4000000}}) {
        const PointTree tree = treeOf(uniform(size, 17));
        PointWalk walk;
        (void)drawn(tree, frameOver(0.0, 1.0, 0.0, 1.0, across, up), &walk);
        CAPTURE(size, walk.nodes, walk.points, walk.drawn);
        CHECK(walk.drawn <= cells);
        CHECK(walk.nodes + walk.points <= 5 * cells);
    }
}

TEST_CASE("points that stand at one place are one node, whatever the zoom", "[scatter]")
{
    Cloud c = uniform(5000, 21);
    for (int i = 0; i < 50000; ++i) {
        c.x.push_back(0.25);
        c.y.push_back(0.75);
    }
    const PointTree tree = treeOf(c);
    // Zoomed in until a cell is a millionth of the cloud: the pile is still
    // one node of no size, and its last point is the one drawn.
    PointWalk walk;
    const std::vector<std::uint32_t> out =
        drawn(tree, frameOver(0.25 - 1e-4, 0.25 + 1e-4, 0.75 - 1e-4, 0.75 + 1e-4, 200, 200), &walk);
    CHECK(walk.points < 100);
    bool lastDrawn = false;
    for (const std::uint32_t s : out) {
        lastDrawn = lastDrawn || tree.element[s] == 54999;
    }
    CHECK(lastDrawn);
}

TEST_CASE("a cloud over six decades is drawn by its logarithms on logarithmic axes", "[scatter]")
{
    Noise noise(23);
    Cloud c;
    for (int i = 0; i < 400000; ++i) {
        c.x.push_back(std::pow(10.0, -3.0 + 6.0 * noise()));
        c.y.push_back(std::pow(10.0, -3.0 + 6.0 * noise()));
    }
    const PointTree tree = treeOf(c, true, true);
    PointWalk walk;
    const std::vector<std::uint32_t> out =
        drawn(tree, frameOver(-3.0, 3.0, -3.0, 3.0, 300, 300), &walk);
    // Spread evenly over the decades, about four to a cell: most cells draw
    // one, and the walk is the pane's size, not the cloud's.
    CHECK(walk.drawn > 300 * 300 * 3 / 4);
    CHECK(walk.nodes + walk.points <= 5 * 300 * 300);
    CHECK(walk.nodes + walk.points < static_cast<long long>(c.x.size()));
    // The same tree over linear scales is the tree again, a different one.
    PointTree again = tree;
    rebuildPointTree(again, false, false);
    CHECK(again.generation != tree.generation);
    CHECK(again.drawable == tree.drawable);
}

TEST_CASE("a selection is drawn after everything else and wins its cell", "[scatter]")
{
    const Cloud c = blob(100000, 29);
    const PointTree tree = treeOf(c);
    std::vector<std::uint64_t> bits((100000 + 63) / 64, 0);
    for (std::uint32_t e = 0; e < 100000; e += 97) {
        bits[e >> 6] |= std::uint64_t{1} << (e & 63);
    }
    const PointSelection selection = selectionOn(tree, bits);
    CHECK(selection.count == (100000 + 96) / 97);
    const PointFrame frame = frameOver(0.0, 1.0, 0.0, 1.0, 200, 200);
    const std::vector<std::uint32_t> out = drawn(tree, frame, nullptr, &selection);
    bool chosenSeen = false;
    std::set<std::pair<long long, long long>> chosenCells;
    for (const std::uint32_t s : out) {
        const bool chosen = selection.has(s);
        CHECK((chosen || !chosenSeen)); // every chosen point after every other
        chosenSeen = chosenSeen || chosen;
        if (chosen) {
            chosenCells.insert({cellOf(tree.x[s], frame.cellX), cellOf(tree.y[s], frame.cellY)});
        }
    }
    // A cell holding a chosen point draws a chosen point.
    for (std::size_t s = 0; s < tree.x.size(); ++s) {
        if (selection.has(static_cast<std::uint32_t>(s))) {
            const long long cx = cellOf(tree.x[s], frame.cellX);
            const long long cy = cellOf(tree.y[s], frame.cellY);
            bool near = false;
            for (long long dx = -1; dx <= 1 && !near; ++dx) {
                for (long long dy = -1; dy <= 1 && !near; ++dy) {
                    near = chosenCells.count({cx + dx, cy + dy}) > 0;
                }
            }
            CHECK(near);
        }
    }
}

TEST_CASE("the point picked is the last whose dot covers the pointer, or else the nearest",
          "[scatter]")
{
    const Cloud c = blob(40000, 31);
    const PointTree tree = treeOf(c);
    std::vector<std::uint64_t> bits((40000 + 63) / 64, 0);
    for (std::uint32_t e = 5; e < 40000; e += 501) {
        bits[e >> 6] |= std::uint64_t{1} << (e & 63);
    }
    const PointSelection selection = selectionOn(tree, bits);
    const double per = 2000.0; // device pixels a position
    const double radius = 2.0;
    const double reach = 12.0;
    Noise noise(37);
    for (int trial = 0; trial < 300; ++trial) {
        const double px = noise();
        const double py = noise();
        for (const PointSelection* chosen :
             {static_cast<const PointSelection*>(nullptr), &selection}) {
            // By hand: the latest covering, by (chosen, element); else the nearest.
            const auto keyOf = [&](std::size_t s) {
                const bool picked = chosen != nullptr && chosen->has(static_cast<std::uint32_t>(s));
                return (std::uint64_t{picked ? 1u : 0u} << 32) | tree.element[s];
            };
            std::uint32_t expected = kNoSlot;
            double nearest = reach * reach;
            bool covering = false;
            for (std::size_t s = 0; s < tree.x.size(); ++s) {
                const double dx = (tree.x[s] - px) * per;
                const double dy = (tree.y[s] - py) * per;
                const double d = dx * dx + dy * dy;
                if (d <= radius * radius) {
                    if (!covering || keyOf(s) > keyOf(expected)) {
                        expected = static_cast<std::uint32_t>(s);
                    }
                    covering = true;
                }
                else if (!covering && (d < nearest || (d == nearest && expected != kNoSlot &&
                                                       keyOf(s) > keyOf(expected)))) {
                    nearest = d;
                    expected = static_cast<std::uint32_t>(s);
                }
            }
            const std::optional<PointPick> pick =
                pickPoint(tree, chosen, px, py, per, per, radius, reach);
            if (expected == kNoSlot) {
                CHECK_FALSE(pick.has_value());
                continue;
            }
            REQUIRE(pick.has_value());
            CHECK(pick->covers == covering);
            CHECK(tree.element[pick->slot] == tree.element[expected]);
        }
    }
}

TEST_CASE("every point inside a box is found, and no other", "[scatter]")
{
    const Cloud c = blob(50000, 41);
    const PointTree tree = treeOf(c);
    std::vector<std::uint64_t> bits;
    const long long found = pointsInside(tree, 0.55, 0.3, 0.4, 0.62, bits); // either way round
    long long expected = 0;
    for (std::size_t e = 0; e < c.x.size(); ++e) {
        const bool in = c.x[e] >= 0.3 && c.x[e] <= 0.55 && c.y[e] >= 0.4 && c.y[e] <= 0.62;
        expected += in ? 1 : 0;
        const bool set = ((bits[e >> 6] >> (e & 63)) & 1u) != 0;
        CHECK(set == in);
    }
    CHECK(found == expected);
    CHECK(expected > 1000);
}

TEST_CASE("values and colours handed over after the points follow them", "[scatter]")
{
    const Cloud c = blob(5000, 43);
    PointTree tree = treeOf(c);
    std::vector<double> values(5000);
    std::vector<std::uint8_t> rgba(4 * 5000);
    for (std::size_t e = 0; e < 5000; ++e) {
        values[e] = static_cast<double>(e) * 0.5;
        for (std::size_t k = 0; k < 4; ++k) {
            rgba[4 * e + k] = static_cast<std::uint8_t>((e + k) & 0xff);
        }
    }
    setTreeValues(tree, values);
    setTreeColours(tree, rgba);
    rebuildPointTree(tree, true, false);
    for (std::size_t s = 0; s < tree.x.size(); ++s) {
        const std::uint32_t e = tree.element[s];
        CHECK(tree.values[s] == values[e]);
        CHECK(tree.rgba[4 * s + 2] == rgba[4 * e + 2]);
        CHECK(tree.x[s] == c.x[e]);
    }
}

TEST_CASE("a disc is painted as an antialiased circle covers its pixels", "[scatter]")
{
    std::vector<std::uint8_t> image;
    const std::vector<Point> centres{{20.3, 15.7}};
    const std::vector<Rgba> colours{Rgba::of(1.0, 0.5, 0.0, 1.0)};
    paintDiscs(centres, colours, 6.0, 40, 30, image);
    REQUIRE(image.size() == 4u * 40 * 30);
    double ink = 0.0;
    for (std::size_t p = 0; p < 40 * 30; ++p) {
        ink += image[4 * p + 3] / 255.0;
        // Premultiplied: no channel above its alpha.
        CHECK(image[4 * p] <= image[4 * p + 3]);
    }
    // The area of a disc of radius six, its edge spread over the pixel either
    // side of the circle, to within a percent.
    const double area = std::numbers::pi * 6.0 * 6.0;
    CHECK(std::abs(ink - area) < 0.01 * area);
    const std::size_t middle = 4 * (15 * 40 + 20);
    CHECK(image[middle] == 255);
    CHECK(image[middle + 1] == 128);
    CHECK(image[middle + 3] == 255);
}

TEST_CASE("discs are painted over one another in the order given", "[scatter]")
{
    std::vector<std::uint8_t> image;
    const std::vector<Point> centres{{10.0, 10.0}, {10.0, 10.0}};
    const std::vector<Rgba> colours{Rgba::of(1.0, 0.0, 0.0), Rgba::of(0.0, 0.0, 1.0, 0.5)};
    paintDiscs(centres, colours, 3.0, 20, 20, image);
    const std::size_t middle = 4 * (10 * 20 + 10);
    CHECK(image[middle] == 128);     // half of the red shows through
    CHECK(image[middle + 2] == 128); // half of the blue
    CHECK(image[middle + 3] == 255);
}

TEST_CASE("a pan from what the last frame knew whole draws what a whole walk draws", "[scatter]")
{
    const Cloud c = blob(400000, 47);
    const PointTree tree = treeOf(c);
    std::vector<std::uint64_t> bits((400000 + 63) / 64, 0);
    for (std::uint32_t e = 3; e < 400000; e += 1013) {
        bits[e >> 6] |= std::uint64_t{1} << (e & 63);
    }
    const PointSelection selection = selectionOn(tree, bits);
    const PointFrame start = frameOver(0.25, 0.75, 0.3, 0.7, 500, 400);
    // Pans of a part of a cell, of a few, across and up at once, of more than
    // the window, and none.
    const std::vector<std::pair<double, double>> pans{{0.3, 0.0},   {3.0, 0.0},   {-7.25, 4.5},
                                                      {0.0, -12.0}, {800.0, 0.0}, {0.0, 0.0}};
    for (const PointSelection* chosen : {static_cast<const PointSelection*>(nullptr), &selection}) {
        PointMemory memory;
        PointScratch scratch;
        std::vector<std::uint32_t> out;
        PointWalk walk;
        drawnPoints(tree, chosen, start, scratch, out, &walk, &memory, chosen != nullptr ? 1 : 0);
        REQUIRE(memory.valid);
        const long long wholeVisits = walk.nodes + walk.points;
        PointFrame at = start;
        for (const auto& [dx, dy] : pans) {
            at.xLow += dx * start.cellX;
            at.xHigh += dx * start.cellX;
            at.yLow += dy * start.cellY;
            at.yHigh += dy * start.cellY;
            drawnPoints(tree, chosen, at, scratch, out, &walk, &memory, chosen != nullptr ? 1 : 0);
            const long long panVisits = walk.nodes + walk.points;
            // Fresh, with nothing remembered.
            PointScratch fresh;
            std::vector<std::uint32_t> expected;
            drawnPoints(tree, chosen, at, fresh, expected);
            CAPTURE(dx, dy, panVisits, wholeVisits);
            CHECK(out == expected);
            if (std::abs(dx) < 50 && std::abs(dy) < 50) {
                CHECK(panVisits * 4 < wholeVisits); // a strip, not the pane
            }
        }
        // Another selection is another picture: nothing is taken.
        drawnPoints(tree, chosen, start, scratch, out, &walk, &memory, chosen != nullptr ? 1 : 0);
        drawnPoints(tree, chosen, start, scratch, out, &walk, &memory, 99);
        CHECK(walk.nodes + walk.points > wholeVisits / 2);
    }
}
