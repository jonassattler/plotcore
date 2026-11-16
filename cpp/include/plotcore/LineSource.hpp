// SPDX-FileCopyrightText: 2026 Jonas Sattler
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

// What a figure asks of the lines it draws, and nothing more.
//
// A Figure draws the lines of its own Plot unless it is handed a source of
// the host's. The viewer is the host that needs one: its lines are read out
// of a file on a thread of their own, a run at a time, and held under a
// budget shared between every plot in the window -- none of which a Figure
// should know about, and all of which ends in the same two answers a Plot
// gives: the line as it is drawn now, and the axis it is drawn against.
//
// So the questions are exactly the ones a Figure asks of its Plot, and a
// source answers them for whatever it holds. Two of them are not questions
// but news: the window the reader is looking at (setView, setPaneColumns),
// which is what lets a source decide what is worth folding or reading, and
// where a zoom is heading (focus), which is what lets it read that way before
// the reader gets there. A source that holds every line whole, as a Plot
// does, can take the news and do nothing with it.
//
// Lines are numbered from zero in the order they are drawn. A source need not
// hand over every line it has -- the viewer's Plot tab draws the first
// sixty-four rows of ten thousand -- so a line also says which *series* it
// is: the number the palette colours it by and the legend names it by, which
// does not move when a line before it is left out. See series().
//
// A line may also be a scatter: points given by an x and a y each, drawn as
// dots (Scatter.hpp). It is a line of another kind and not a second sort of
// thing, so it is numbered with the lines, coloured and named by its series,
// moved, scaled, stacked and hidden as a line is, and the axes are drawn over
// both. What a figure asks of one is its extent, as of any line, and its points
// (drawPoints); a source with no scatters answers neither, and has none.

#include "plotcore/Camera.hpp"
#include "plotcore/Levels.hpp"
#include "plotcore/Projection.hpp"
#include "plotcore/Scatter.hpp"

#include <string>

namespace plotcore {

/// The values of a line: its finite extent, and the smallest of it above zero,
/// which is where a logarithmic y axis starts.
struct LineExtent
{
    bool finite = false;
    double minimum = 0.0;
    double maximum = 0.0;
    bool positive = false;
    double positiveMinimum = 0.0;
};

/// A line folded onto the columns of a grid: each column's smallest and largest
/// value, the stroke out of it included, or NaN for both where the column holds
/// nothing finite or lies past either end of the line. `count` of each, the
/// source's, on the terms of LineSource::drawLine.
struct DrawnColumns
{
    const double* lows = nullptr;
    const double* highs = nullptr;
    /// What is unusual about each of them, as a ColumnKind, or null when nothing
    /// is.
    const unsigned char* kinds = nullptr;
    int count = 0;
};

/// What a line is: elements along an x the plot gives them, or points that
/// each carry an x of their own.
enum class LineKind
{
    Line,
    Points,
};

class LineSource
{
public:
    LineSource() = default;
    LineSource(const LineSource&) = default;
    LineSource(LineSource&&) noexcept = default;
    LineSource& operator=(const LineSource&) = default;
    LineSource& operator=(LineSource&&) noexcept = default;
    virtual ~LineSource() = default;

    // --- the lines ---------------------------------------------------------
    [[nodiscard]] virtual int lineCount() const = 0;
    /// Which line this is to the palette and the legend: its place among every
    /// line the host has, drawn or not. A palette colours a line by it, so a
    /// line keeps its colour when one before it is hidden -- PlotSurface's
    /// promise, asked for by `series` and not by position there too. The
    /// line's own number when a source hands over every line it has.
    [[nodiscard]] virtual int series(int line) const { return line; }
    [[nodiscard]] virtual std::string name(int line) const = 0;
    /// Elements of the line. The zoom's ceiling follows the longest.
    [[nodiscard]] virtual long long length(int line) const = 0;
    /// Whether it can be drawn yet: a line still arriving is not.
    [[nodiscard]] virtual bool ready(int line) const = 0;

    // --- what the axes are drawn between -------------------------------------
    /// The whole line's values: the y axis is drawn against these and not
    /// against what is on screen, so it does not rescale as a closer look
    /// arrives. Asked once per line per frame.
    [[nodiscard]] virtual LineExtent extent(int line) const = 0;
    /// The finite x the line spans.
    [[nodiscard]] virtual Span xExtent(int line) const = 0;
    /// The smallest x of the line above zero, or 0.
    [[nodiscard]] virtual double xPositiveMinimum(int line) const = 0;

    // --- what is being looked at ---------------------------------------------
    /// Device-pixel columns of the pane: what a line is folded to.
    virtual void setPaneColumns(int columns) = 0;
    /// The window of x on screen, and whether the axis is logarithmic -- which
    /// decides what a column is.
    virtual void setView(double xLow, double xHigh, bool xLog) = 0;
    /// Where a zoom of x is heading: the x under the pointer and the factor.
    /// Told before the window that zoom makes is, so whatever is read for it
    /// is read towards the pointer rather than towards the middle of the pane.
    virtual void focus(const GestureFocus& focus) { (void)focus; }
    /// A pan, a reset: nowhere in particular any more.
    virtual void clearFocus() {}

    // --- the picture -----------------------------------------------------------
    /// The line as it is drawn now, folded for the view and the pane, with the
    /// element of each extreme. Its arrays are the source's and must stay
    /// valid until the source is next asked something that is not const.
    [[nodiscard]] virtual PlotLine drawLine(int line) const = 0;
    /// The x axis that line is drawn against, on the same terms.
    [[nodiscard]] virtual PlotAxis drawAxis(int line) const = 0;
    /// The line on the columns of `grid`, for a line drawn as bars -- or nothing,
    /// and the figure then strokes it as usual, when a column inside the line
    /// holds no element or would have to be cut finer than what is held. With
    /// `grid.partial` such a column is NaN like any other with nothing to draw,
    /// and only a line that has no columns at all is refused.
    ///
    /// A question and not news: it may be asked for a picture's grid as well as
    /// the pane's, and must read nothing to answer. A source with no way of
    /// answering it says nothing, which is every source until it chooses to.
    [[nodiscard]] virtual DrawnColumns drawColumns(int line, const ColumnGrid& grid) const
    {
        (void)line;
        (void)grid;
        return {};
    }

    // --- points ------------------------------------------------------------------
    /// Which kind of line this is. Every line of a source that has no scatters
    /// is a line.
    [[nodiscard]] virtual LineKind kind(int line) const
    {
        (void)line;
        return LineKind::Line;
    }
    /// What a scatter's points are coloured by, as an extent: the colour axis
    /// is drawn against these, as the y axis is against `extent`. Asked once
    /// per scatter per frame; not finite for one with no values.
    [[nodiscard]] virtual LineExtent valueExtent(int line) const
    {
        (void)line;
        return {};
    }
    /// A scatter's points, in a tree over the scales asked, or null for a line
    /// that is not one. The tree is the source's and stays valid until the
    /// source is next asked something that is not const.
    ///
    /// A question and not news, as drawColumns is: it may order the points
    /// again for scales it was not ordered for, which is arithmetic over what
    /// is held, and reads nothing.
    [[nodiscard]] virtual const PointTree* drawPoints(int line, bool xLog, bool yLog) const
    {
        (void)line;
        (void)xLog;
        (void)yLog;
        return nullptr;
    }
};

} // namespace plotcore
