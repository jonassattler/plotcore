// SPDX-FileCopyrightText: 2026 Jonas Sattler
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

// Points given by an x and a y each, held once, and drawn at the cost of the
// pane rather than of the points.
//
// A line is elements along an x that only goes one way, so a window of x is a
// window of elements and an envelope folds it into a pane of columns
// (Levels.hpp). A scatter has no such order. Its x is anything, two points
// beside each other on the pane can be a million elements apart, and a window
// of the view is a region of the plane, not a run of the line. So the
// reduction a scatter needs is spatial, and this file is to a scatter what
// Pyramid.hpp is to a line: a structure built once, aligned in the data so a
// pan moves the picture rigidly, from which any view is drawn by visiting
// about as many nodes as the pane has pixels however many points there are.
//
// It rests on one property, as the pyramid rests on the envelope's:
//
//   **The last point of a region is the last of the last points of its parts.**
//
// "Last" is the order the points are drawn in: by element, the order they were
// handed over in, as a scatter of every point would draw them, with a selected
// point after every point that is not. Drawn opaque, the dot on top at a pixel
// is the dot of the last point within a dot's reach of it -- so the last point
// of a region is the one point of it a reader can see at its place, and a node
// that keeps its last point answers for everything under it. The last of a
// union is the last of the lasts of its parts, exactly, so every node of the
// tree answers with a copy and no walk, as PyramidLevel::at keeps each
// extreme's element rather than finding it again.
//
// The tree is a quadtree over the points' **axis positions** -- the value, or
// its logarithm on a logarithmic axis -- because a pixel is a fixed share of
// a position and not of a value. Over linear positions a cloud spanning six
// decades drawn on a logarithmic axis lands in one column of leaves at the
// left, and every frame would visit all of it. So a tree is built for one pair
// of scales and built again when either changes, which resets the view anyway
// (Figure::setSettings). The base of a logarithm does not matter: it cancels
// out of every place a point is drawn (PlotView::xLogBase), so the tree's
// logarithms are decimal whatever the axis is numbered in.
//
// A frame draws through cells **aligned in the data**: one device pixel each,
// a ColumnGrid along each axis, so cell k covers the same run of positions
// whatever the reader is looking at. A pan then changes which cells are on
// screen and nothing about what is in them, for the reason the buckets are
// aligned (PlotWindow). The walk stops at a node that fits inside a cell on
// both axes and takes its last point, and each cell keeps the last of what
// lands in it. What that promises, measured in cells:
//
// - At most one point is drawn a cell, and it is the last that landed there.
// - A point with no other point within a cell of it, along both axes, is
//   always drawn: an outlier is never lost, as an envelope never loses a
//   spike.
// - Every point lies within two cells, along each axis, of a drawn point at
//   least as late as itself -- under the dot of one, at the size dots are
//   drawn at.
//
// Held against every point painted, in the order handed over and coloured by
// a value through a ramp, what the walk draws differs by more than an eighth
// in any channel in 0.86% of the covered pixels of a 640 by 400 pane at forty
// thousand points, and in 0.47% at four hundred thousand: where a cell holds
// two points of different colours and the later one stood a cell over.
//
// What a frame costs is the nodes it visits. In a view zoomed alike on both
// axes that is a few for every cell the points occupy, and so bounded by the
// pane. A view zoomed on one axis far more than on the other is the worst
// case: a node fits a cell only when it fits the narrower of its sides, so it
// costs about as many nodes a cell as the one axis is zoomed past the other,
// and never more than the points in view.

#include "plotcore/Projection.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace plotcore {

/// Points a leaf holds at most, unless they all stand at one place.
///
/// A leaf wider than a cell is visited a point at a time, so this is what a
/// frame costs a cell at the worst density there is -- the one at which each
/// leaf is just wider than a cell. At sixty-four that density was a uniform ten
/// million on a pane of six hundred thousand cells, and every frame visited
/// every point. At eight it is eight visits a cell, and a node costs 48 bytes,
/// so the tree costs about eight bytes a point.
inline constexpr int kPointLeaf = 8;

/// The most points one frame draws as markers, over every scatter together.
///
/// A marker is a dot a host paints, and the slowest host paints one in 1.7
/// microseconds: QPainter's antialiased ellipse, measured with
/// plotcore-bench-scatter-qt over 200 000 of them. So eight thousand are about
/// fourteen milliseconds, which is a frame; past that a scatter is drawn as an
/// image of the pane instead (PaneImage), which costs the host one blit --
/// under a millisecond through the same painter -- however many points it
/// holds. Shared between the scatters as kMaxVertices is shared between the
/// lines (maxMarkersFor).
inline constexpr int kMaxMarkers = 1 << 13;

/// Each scatter's share of kMaxMarkers.
[[nodiscard]] inline int maxMarkersFor(int scatters)
{
    return std::max(1, kMaxMarkers / std::max(scatters, 1));
}

/// No slot: a node with nothing selected in it, a point that is not there.
inline constexpr std::uint32_t kNoSlot = 0xffffffffu;

/// The most points one scatter holds: an element and a slot each fit in 31
/// bits, so a point's place in the drawing order and its slot are one word,
/// and keeping the last of what lands in a cell is a maximum with no branch in
/// it -- which, in the order the tree visits points, is three times faster
/// than one that mispredicts half the time.
inline constexpr long long kMaxScatterPoints = 0x7ffffffeLL;

/// One node of a PointTree.
struct PointNode
{
    /// The box its points stand in, in axis positions -- the points', not the
    /// quadrant's, so a node of points that all stand at one place is a node
    /// of no size, which fits any cell.
    double xLow = 0.0;
    double xHigh = 0.0;
    double yLow = 0.0;
    double yHigh = 0.0;
    /// The slots it holds: `[first, first + count)`.
    std::uint32_t first = 0;
    std::uint32_t count = 0;
    /// Its first child in the low 29 bits and how many it has in the high
    /// three, which is none for a leaf: children are consecutive.
    std::uint32_t family = 0;
    /// The slot of its last point.
    std::uint32_t last = kNoSlot;

    [[nodiscard]] std::uint32_t child() const { return family & 0x1fffffffu; }
    [[nodiscard]] std::uint32_t children() const { return family >> 29; }
};

/// A scatter's points, in the order of a quadtree over their axis positions.
///
/// The order *is* the copy, as a pyramid's base is a line's: the points are
/// held once, in the tree's order, and everything a point carries is moved
/// with it. Points that have no place on the tree's scales -- a coordinate
/// that is not finite, or not above zero on a logarithmic axis -- are held
/// after the tree, in the order they came, and never drawn.
struct PointTree
{
    /// The scales the tree is over.
    bool xLog = false;
    bool yLog = false;
    /// Each slot's point, in the data's own units.
    std::vector<double> x;
    std::vector<double> y;
    /// The element each slot holds: where the point was in what was handed
    /// over, which is its place in the order points are drawn in.
    std::vector<std::uint32_t> element;
    /// What each slot's point is coloured by, when the points carry values;
    /// empty otherwise.
    std::vector<double> values;
    /// Each slot's own colour, four bytes a point (red, green, blue, alpha, not
    /// premultiplied), when the points carry colours; empty otherwise.
    std::vector<std::uint8_t> rgba;
    /// The root first. Empty when no point has a place on these scales.
    std::vector<PointNode> nodes;
    /// Slots `[0, drawable)` are in the tree.
    std::uint32_t drawable = 0;
    /// Which build this is. Two trees never share one, so whatever was worked
    /// out from a tree -- the selection's nodes (PointSelection) -- can ask
    /// whether it was this one.
    std::uint64_t generation = 0;

    [[nodiscard]] long long size() const { return static_cast<long long>(x.size()); }
    [[nodiscard]] bool empty() const { return nodes.empty(); }
    /// What it costs, in bytes, for a budget to count.
    [[nodiscard]] long long bytes() const;
};

/// The axis position of `value`: itself, or its decimal logarithm. NaN where
/// it has none, which is no place.
[[nodiscard]] inline double pointPosition(double value, bool logarithmic)
{
    if (!std::isfinite(value) || (logarithmic && !(value > 0.0))) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return logarithmic ? std::log10(value) : value;
}

/// A tree over `count` points for the scales asked. `values` (a double a
/// point) and `rgba` (four bytes a point) are null when the points carry
/// none. Everything is copied.
[[nodiscard]] PointTree pointTreeOf(const double* x, const double* y, const double* values,
                                    const std::uint8_t* rgba, long long count, bool xLog,
                                    bool yLog);

/// The same points over other scales, reordered where they are held. Nothing
/// happens when the tree is already over them.
void rebuildPointTree(PointTree& tree, bool xLog, bool yLog);

/// Values or colours handed over after the points, in element order, put in
/// the tree's order. Empty clears them.
void setTreeValues(PointTree& tree, std::span<const double> values);
void setTreeColours(PointTree& tree, std::span<const std::uint8_t> rgba);

/// A selection as one tree sees it: which slots are in it, and the last
/// selected slot of every node -- worked out once from a selection kept by
/// element, so the walk can draw a selection of millions as it draws the
/// points, by nodes.
struct PointSelection
{
    std::uint64_t generation = 0;
    /// A bit a slot.
    std::vector<std::uint64_t> slots;
    /// A slot a node, or kNoSlot where nothing under it is selected.
    std::vector<std::uint32_t> last;
    long long count = 0;

    [[nodiscard]] bool has(std::uint32_t slot) const
    {
        return count > 0 && ((slots[slot >> 6] >> (slot & 63)) & 1u) != 0;
    }
};

/// `elements`, a bit an element, as `tree` sees it.
[[nodiscard]] PointSelection selectionOn(const PointTree& tree,
                                         std::span<const std::uint64_t> elements);

/// Where a frame draws, in the tree's axis positions.
struct PointFrame
{
    /// The window, widened on every side by as far as a dot reaches, so a dot
    /// centred just off the pane still draws the part of it on the pane.
    double xLow = 0.0;
    double xHigh = 1.0;
    double yLow = 0.0;
    double yHigh = 1.0;
    /// Positions a cell along each axis: a device pixel, from ColumnGrid, so
    /// that two frames of one zoom have the same cells.
    double cellX = 1.0;
    double cellY = 1.0;
};

/// What drawing a frame cost: what the tests hold the walk to.
struct PointWalk
{
    long long nodes = 0;
    long long points = 0;
    long long candidates = 0;
    long long drawn = 0;
};

/// What `drawnPoints` keeps between frames: a word a cell -- the last point
/// that landed there, as whether it is selected, its element and its slot plus
/// one, or nothing -- and what sorts them.
struct PointScratch
{
    std::vector<std::uint64_t> cells;
    std::vector<std::uint64_t> order;
    std::vector<std::uint64_t> spare;
};

/// What one frame of a scatter drew, kept for the next on the same grid: the
/// last point of every cell wholly inside its window.
///
/// Such a cell is known whole -- every node that holds a point in it meets the
/// window, and was walked -- and what it drew depends on nothing but the grid,
/// the tree and the selection. So a pan, which keeps the grid, takes those cells
/// as they are and walks only the cells it uncovers, as a pan folds only the
/// columns it uncovers for bars (Plot::drawColumns); and what it draws is what
/// a frame walked whole would, cell for cell.
struct PointMemory
{
    bool valid = false;
    double cellX = 0.0;
    double cellY = 0.0;
    std::uint64_t generation = 0;
    /// The caller's word for the selection it drew with: another selection is
    /// another picture.
    std::uint64_t chosen = 0;
    /// The cells known whole, `[x0, x1)` across by `[y0, y1)` up, counted from
    /// position zero; and a word for each, row by row, nothing for an empty one.
    long long x0 = 0;
    long long x1 = 0;
    long long y0 = 0;
    long long y1 = 0;
    std::vector<std::uint64_t> words;
};

/// The points a frame draws, as slots, in the order they are drawn: at most
/// one a cell, the last of what lands in it, and every point the header
/// promises. `selection` is null, or the selection as this tree sees it.
///
/// With `memory`, what the last frame on this grid knew whole is taken from it
/// and not walked again, and what this frame knows whole is left in it.
/// `chosen` names the selection, so a memory of another is not taken.
void drawnPoints(const PointTree& tree, const PointSelection* selection, const PointFrame& frame,
                 PointScratch& scratch, std::vector<std::uint32_t>& out, PointWalk* walk = nullptr,
                 PointMemory* memory = nullptr, std::uint64_t chosen = 0);

/// The point under a pointer, as a slot, and how far it is from it.
struct PointPick
{
    std::uint32_t slot = kNoSlot;
    /// In device pixels.
    double distance = 0.0;
    /// Whether its dot covers the pointer.
    bool covers = false;
};

/// The point under (`x`, `y`), which are axis positions: the last point whose
/// dot of `radius` covers it, as the one drawn on top there -- or, where no dot
/// does, the nearest within `reach`. `perX` and `perY` are pixels a position,
/// which is how a distance on the pane is measured, and `radius` and `reach`
/// are in the same pixels. `within`, when it is given, is the window a point
/// must be in to be taken at all: the one on screen, so a crosshair asked for
/// the nearest point anywhere is not handed one off the pane. Nothing when no
/// point is within reach.
[[nodiscard]] std::optional<PointPick> pickPoint(const PointTree& tree,
                                                 const PointSelection* selection, double x,
                                                 double y, double perX, double perY, double radius,
                                                 double reach, const PointFrame* within = nullptr);

/// Every point inside the box between `xLow` and `xHigh` and `yLow` and
/// `yHigh`, in axis positions, as the element of each, into `out` -- a bit an
/// element, which is set and never cleared. How many there were.
long long pointsInside(const PointTree& tree, double xLow, double xHigh, double yLow, double yHigh,
                       std::vector<std::uint64_t>& out);

/// Discs `radius` device pixels across the middle, at `centres` in device
/// pixels from the image's top left, each in its colour -- `colours` holds one
/// a disc, or one for them all -- composited in the order given into an image
/// `width` by `height`, premultiplied RGBA, eight bits a channel, the top row
/// first.
///
/// A pixel is covered by a disc by `r + 1/2 - d`, its distance `d` from the
/// centre, clamped to 0..1: the antialiased edge a host's own circle has, so a
/// scatter drawn as an image is the scatter drawn as markers. The centre is
/// taken to the nearest sixteenth of a pixel, from which the coverage is
/// stamped rather than worked out again. Spread over the
/// pool in bands of rows, each band taking its discs in the order given, so
/// the image is the same whatever the number of threads.
void paintDiscs(std::span<const Point> centres, std::span<const Rgba> colours, double radius,
                int width, int height, std::vector<std::uint8_t>& out);

} // namespace plotcore
