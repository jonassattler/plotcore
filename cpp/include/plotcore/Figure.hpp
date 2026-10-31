// SPDX-FileCopyrightText: 2026 Jonas Sattler
// SPDX-FileCopyrightText: 2026 Andreas Martin
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

// A whole plot: the lines, the view onto them, what the reader has chosen, and
// the picture all of that makes.
//
// What the viewer spreads across DatasetPlot, PlotSurface.qml, PlotFrame.qml
// and PlotItem, in one object a host can drive without knowing any of them.
// The host does three things and nothing else: it hands over the pane's size
// and how to measure text, it forwards the pointer and the wheel, and it
// paints the Scene that comes back -- the lines, and the marks to paint
// around them in order, in pane pixels with y down. Every decision between
// those is plotcore's, so a plot drawn from Qt, from Python and from Rust is
// the same plot: the same ticks at the same places, the same gestures moving
// the same windows, the same line in the same colour (golden/ holds the three
// to one another).
//
// The viewer's rules hold here as they do there, because the arithmetic is the
// viewer's -- Ticks.hpp and Camera.hpp are PlotFrame's and PlotSurface's,
// held to them by tst_plotcore_parity.qml. Three things are this file's own,
// all of them from #30, all of them about a line rather than the axes:
//
// - **A line may be moved and scaled on its axis** (LineSettings::scale and
//   shift): its window is the common window's share of a window of its own,
//   so one gesture still moves every line and a moved line stays moved.
// - **Lines may be stacked**, each in an equal band of the pane: the same
//   arithmetic, with the scale and shift that put each line's own extent in
//   its band.
// - **A line may carry a time of its own** (Plot::setLineTime), against the
//   one x axis every line shares.
//
// A line on an axis of its own, or stacked, is numbered beside the pane in
// its colour, as the viewer draws a separate axis -- and, as there, only while
// a second line is drawn. A line that is only moved or scaled is not: it is a
// reading in the common axis's units, the crosshair reads it in its own, and
// an axis appearing beside the pane on the first pixel of a drag would widen
// the gutter and move every x under the pointer that is dragging.

#include "plotcore/Camera.hpp"
#include "plotcore/LineSource.hpp"
#include "plotcore/Palettes.hpp"
#include "plotcore/Plot.hpp"
#include "plotcore/Ticks.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace plotcore {

/// The faces text is set in. A host maps each to a font.
enum class TextRole
{
    /// A number on an axis: Theme.readout.
    Tick,
    /// A raised exponent, and a digit between two numbers: Theme.readoutMinor.
    TickMinor,
    /// The title over the pane: Theme.bodyStrong.
    Title,
    /// An axis's name: Theme.bodySmall.
    AxisName,
    /// A line's name in the legend: Theme.monoSmall.
    Legend,
    /// What is said in passing: the legend's count of the lines it leaves
    /// out. Theme.micro.
    Note,
};

/// How a host measures text. Both are in pane pixels.
struct TextMeasure
{
    std::function<double(const std::string& text, TextRole role)> width;
    /// The height of a line of type in `role`.
    std::function<double(TextRole role)> height;
};

/// What the frame is laid out with: Theme's numbers, as a host's own. The
/// defaults are the viewer's, so a figure laid out by any host is the picture
/// the viewer draws.
struct FigureStyle
{
    /// The faces' sizes: Theme.readout, readoutMinor, bodyStrong, bodySmall,
    /// monoSmall and micro.
    double tickPixels = 10.0;
    double tickMinorPixels = 8.0;
    double titlePixels = 14.0;
    double axisNamePixels = 13.0;
    double legendPixels = 12.0;
    double notePixels = 10.0;
    double gapXS = 2.0;
    double gapS = 6.0;
    double gapM = 8.0;
    double gapL = 12.0;
    double s3 = 4.0;
    double s9 = 24.0;
    /// The inset where the axis labels live, on the side of a y label: a
    /// number of several digits written sideways-on to the axis, where an x
    /// label is a column index under it (Theme.plotLabelMargin).
    double labelMargin = 32.0;
    /// Theme.borderWidthAccent, and the hairline every rule is drawn at.
    double accentWidth = 2.0;
    double ruleWidth = 1.0;
    double tickLength = 4.0;
    /// How far an exponent on a logarithmic axis sits above the line of the
    /// number it raises: the `3` of `10³` (Theme.plotExponentRaise).
    ///
    /// matplotlib's mathtext puts a superscript's baseline at a little under
    /// half the height of the type it raises. The tick face is ten pixels,
    /// and four is that on the spacing ladder -- enough that the exponent
    /// reads as raised rather than as a smaller digit beside the power, and
    /// not so much that it leaves the line of type the gutter was measured
    /// for.
    double exponentRaise = 4.0;
    /// A line is a hairline like every other stroke in the viewer's design;
    /// the marker is the smallest dot that still reads as one
    /// (Theme.plotLineWidth, plotMarkerSize).
    double lineWidth = 1.0;
    double markerSize = 4.0;
    /// Theme.radiusS: the corners of a veil.
    double radius = 2.0;
    /// How much of the pane a pointer covers, down and to the right of its
    /// hotspot (Theme.pointerSize): what the band's numbers stay off.
    ///
    /// Not a measurement a host can make: the cursor belongs to the window
    /// system and its size is the reader's own setting. This is the common
    /// one -- a 24-pixel arrow whose hotspot is its tip -- and it exists so
    /// that a readout which follows the pointer can be drawn beside it rather
    /// than underneath it. Wrong by a few pixels on a reader who has scaled
    /// their cursor up; wrong by the whole of it if nothing tries.
    double pointerSize = 24.0;
    /// The legend's swatch: a stroke of line, not a block of colour, because
    /// what it stands for is a stroke (Theme.plotLegendSwatch).
    double legendSwatch = 12.0;
    /// How many lines the legend on the plot names before it stops and says
    /// how many are left (Theme.plotLegendRows). A plot of ten thousand lines
    /// has a legend ten thousand rows tall, which is not a legend -- it is the
    /// picture replaced by a list of what is in it. Twelve is about a third of
    /// a pane at the viewer's row height, which is as much of the drawing as
    /// a caption may take.
    int legendRows = 12;
    /// How far the pointer travels, across or down, before a press becomes a
    /// drag: QStyleHints::startDragDistance, which a host sets from its
    /// platform. A band is drawn, and the crosshair put away for it, only
    /// once a right press has gone that far -- as the viewer's DragHandler
    /// waits for the platform's threshold before it is a band at all.
    double dragDistance = 10.0;
    int tickTarget = 8;
    /// How much finer the dense grid is than the numbered ticks
    /// (Theme.plotGridDenseFactor). Four, which after niceStep's rounding to
    /// 1, 2 or 5 is usually five minor rules between two labels and sometimes
    /// two. It is the one quantity that decides what "dense" means, and both
    /// axes read it.
    int denseFactor = 4;

    /// A measure that needs no font: three fifths of the face's size a
    /// character, and six fifths of it a line. What a headless run and the
    /// golden digests use, so they are about the arithmetic and not a font.
    [[nodiscard]] TextMeasure approximateMeasure() const;
    [[nodiscard]] double pixelsOf(TextRole role) const;
};

enum class Corner
{
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
};

/// Everything a reader chooses about the plot as a whole -- PlotSurface's
/// drawing settings.
struct FigureSettings
{
    GridMode grid = GridMode::Loose;
    double gridStepX = 0.0;
    double gridStepY = 0.0;
    bool xLog = false;
    bool yLog = false;
    double xLogBase = 10.0;
    double yLogBase = 10.0;
    /// x up the pane and y across it, as `plot(y, x)` draws it.
    bool flipped = false;
    bool minorNumbers = false;
    bool markers = false;
    bool cursor = true;
    std::string title;
    std::string xLabel;
    std::string yLabel;
    bool legend = true;
    Corner legendCorner = Corner::TopRight;
    LineColours colours = LineColours::defaults(true);
    int highlighted = -1;
    /// Lines drawn under one another, as a bundle of rows is.
    bool bundle = false;
    /// Whether the reading under the pointer is written in the pane, in a
    /// box in its corner. A host that writes it somewhere of its own -- the
    /// viewer prints it in the bar under the plot -- turns it off and reads
    /// Scene::reading.
    bool readout = true;
    /// Whether a band being drawn writes its corners and its lengths beside
    /// it (PlotBand.qml).
    bool bandReadout = true;
    /// Whether the figure says over the pane why it cannot draw. A host with
    /// a way of its own of saying that a view is empty -- the viewer has one
    /// for every view it has -- turns it off and reads Scene::message.
    bool paneMessage = true;
    /// Whether a line can be moved and scaled on its own: the middle button
    /// or Alt and a drag, Alt and Control and the wheel, and Control and a
    /// click to put it back (#30).
    bool lineGestures = true;
    /// The ground the picture is for.
    bool dark = true;
    /// The picture a page would draw: the light scope, whatever is on screen.
    bool paper = false;
    /// Every drawn line in an equal band of the pane.
    bool stacked = false;
};

/// What a reader chooses about one line.
struct LineSettings
{
    bool visible = true;
    /// A colour of its own, over whatever cycle is in force.
    std::optional<Rgba> colour;
    /// On a y axis of its own: the line alone, linear, zoomed by the common
    /// axis's share. See H5Scope's CLAUDE.md, "A line of a custom tab may also be drawn
    /// against a y axis of its own".
    bool ownAxis = false;
    /// ...and left at its whole extent whatever the zoom.
    bool axisFixed = false;
    std::string axisLabel;
    /// Moved and scaled on its axis: its whole window is the window it would
    /// have, shrunk by `scale` about its centre and moved by `shift` -- in the
    /// axis's positions, decades on a logarithmic one.
    double scale = 1.0;
    double shift = 0.0;
};

/// Which button a press came from.
enum class Button
{
    Left,
    Middle,
    Right,
};

struct Modifiers
{
    bool shift = false;
    bool control = false;
    bool alt = false;
};

/// A piece of text, at a point and an alignment. `base` is set in `role`'s
/// face, `exponent` raised beside it in TickMinor's; `text` is the whole of it
/// in one string with the exponent in superscript digits, for a host that sets
/// one string.
struct TextItem
{
    enum class Align
    {
        Start,
        Middle,
        End,
    };

    std::string text;
    std::string base;
    std::string exponent;
    Point at;
    Align across = Align::Start;
    Align down = Align::Start;
    TextRole role = TextRole::Tick;
    Rgba colour;
    /// Turned about `at` by this many degrees, clockwise on the pane, and
    /// aligned in the turned frame: -90 runs up the side, as an axis name does.
    double turn = 0.0;
    /// How far the exponent's baseline sits above the base's.
    double raise = 0.0;
};

/// One thing to paint, in pane pixels with y down.
///
/// A scene lists them in the order they are painted, and a host translates
/// each into its toolkit's call and decides nothing about it: every colour,
/// width and radius is here. That is what makes a plot drawn from Qt, from
/// Python and from Rust the same plot -- the four hosts used to make their own
/// crosshair, legend and band, and no two of them made the same one.
struct Mark
{
    enum class Kind
    {
        Segment,
        Box,
        Circle,
        Text,
    };

    /// What a mark is part of, for a host that paints only some of it: one
    /// whose axes are its own -- matplotlib's -- paints the overlay and draws
    /// the frame its own way.
    enum class Layer
    {
        /// The rules across the pane, under the lines.
        Grid,
        /// The axes, their numbers and names, the title and the legend.
        Frame,
        /// What answers the reader: the crosshair and its reading, the band,
        /// and why the pane cannot draw.
        Overlay,
    };

    Kind kind = Kind::Segment;
    Layer layer = Layer::Frame;
    /// Painted only inside the pane (Scene::area).
    bool clipped = false;
    /// A segment's two ends; a box's top left and bottom right; a circle's
    /// centre, in `from`.
    Point from;
    Point to;
    /// The inside of a box or a circle. Not painted where it is clear.
    Rgba fill = Rgba::of(0.0, 0.0, 0.0, 0.0);
    /// A segment, and the edge of a box or a circle. Not painted at a width
    /// of 0. A box's edge lies inside the box, as a QML Rectangle's border
    /// does, so a box is exactly as big as it says; a circle's is centred on
    /// its radius.
    Rgba stroke;
    double width = 0.0;
    /// A circle's radius, and the radius a box's corners are rounded to.
    double radius = 0.0;
    /// The edge in dashes rather than whole.
    bool dashed = false;
    TextItem text;
};

/// One line, as it is drawn.
struct DrawnLine
{
    int line = -1;
    Rgba colour;
    double width = 1.0;
    /// The strokes as polylines, for a host that strokes them itself.
    std::vector<std::vector<Point>> runs;
    /// ...and as triangles, three points each, for one that fills them.
    std::vector<Point> triangles;
    /// ...and as one triangle strip a run, two points a sample: the same
    /// triangles at a third of the points, for a host that draws strips.
    std::vector<std::vector<Point>> strips;
    /// Where a marker goes, when markers are on and the line is its samples.
    std::vector<Point> markers;
    double markerRadius = 2.0;
    /// The runs again in the data's own units, point for point: x as the x
    /// axis reads it and y as `y` reads it. For a host whose axes are its own
    /// -- matplotlib's -- and which is handed values to draw rather than
    /// pixels.
    std::vector<std::vector<Point>> values;
    /// The window y is read in: the common axis's, or the line's own when it
    /// is on an axis of its own, moved, scaled or stacked.
    Span y;
    bool yLogarithmic = false;
    bool own = false;
    /// Whether that window is numbered beside the pane.
    bool numbered = false;
};

/// One row of the legend: a swatch from `from` to `to`, and the name set in
/// TextRole::Legend starting at `textAt` and centred on it vertically.
struct LegendEntry
{
    int line = -1;
    std::string name;
    Rgba colour;
    Point from;
    Point to;
    Point textAt;
};

/// One of the numbers a band being drawn writes beside itself: a corner
/// ("start", "end") or a length ("widthAbove", "widthBelow", "heightLeft",
/// "heightRight"). A band that takes one axis writes "width" or "height"
/// for the span that has to stand on the band, because the band fills the
/// pane the other way and there is no outside left to stand in. In the box
/// it is drawn in and turned by `turn` degrees. What PlotBand.labels was, so
/// a test can hold the layout to its rules.
struct BandLabel
{
    std::string key;
    std::string text;
    PaneRect box;
    double turn = 0.0;
};

/// The sample under the pointer, when there is one.
struct Reading
{
    bool valid = false;
    int line = -1;
    double x = 0.0;
    double y = 0.0;
    /// Where it was drawn.
    Point at;
    /// The two numbers, written as the axes write theirs.
    std::string xText;
    std::string yText;
};

/// One axis as it was numbered and ruled, in its own values.
///
/// The marks say where the numbers and the rules were painted, in whole
/// pixels; this says what they were. A host that draws its frame in its own
/// toolkit takes the numbers from here rather than reading them back off the
/// marks, and so does a test that asks which values an axis was ruled at
/// rather than which pixels: a rule rounded to a pixel is a value known to
/// within half of one, and the viewer's suite held its axes to the exact
/// fraction for as long as the frame was QML it could ask.
struct SceneAxis
{
    /// The line this is the axis of, or -1 for a common one.
    int line = -1;
    /// What it runs between, and on which scale.
    Span window;
    bool logarithmic = false;
    double base = 10.0;
    /// Its numbers: the values, the label written at each, and the ticks.
    TickSet ticks;
    /// The rules across the pane from it, each at a fraction of the axis.
    std::vector<Rule> grid;
    /// The digits between two of its numbers, where they are asked for.
    std::vector<MinorNumber> minor;
    /// For an axis of a line's own: the colour it is drawn in, which is its
    /// line's; its name; and where it stands, in pane pixels -- a column left
    /// of the pane, or a row under it when the plot is flipped.
    Rgba colour;
    std::string label;
    PaneRect box;
};

/// One frame.
struct Scene
{
    double width = 0.0;
    double height = 0.0;
    Rgba ground;
    /// What everything that is not a line is drawn in -- the ink of the text,
    /// the legend's border, the crosshair -- so a host paints the frame in
    /// the scope it was laid out for without working it out again.
    Chrome chrome;
    /// The pane inside the gutters, where the lines are -- clipped to it.
    PaneRect area;
    /// What is painted under the lines, in order: the grid.
    std::vector<Mark> under;
    std::vector<DrawnLine> lines;
    /// What is painted over them, in order: everything else.
    std::vector<Mark> over;
    /// The legend's rows and where they were laid out, for a host or a test
    /// that asks which line is which; `over` paints them.
    std::vector<LegendEntry> legend;
    PaneRect legendRect;
    Reading reading;
    /// The rectangle a right-drag is drawing, while it is, and what is
    /// written beside it.
    std::optional<PaneRect> band;
    std::vector<BandLabel> bandLabels;
    /// Why nothing can be drawn, said over the pane; empty when it can. The
    /// title is the same thing said in a few words.
    std::string message;
    std::string messageTitle;
    Span x;
    Span y;
    /// The common axes as they were numbered and ruled -- y's without numbers
    /// or rules when no line is drawn against it -- and each axis of a line's
    /// own, in the order they stand beside the pane. Not carried across the C
    /// interface yet: its hosts paint the marks.
    SceneAxis xAxis;
    SceneAxis yAxis;
    std::vector<SceneAxis> sideAxes;

    /// The texts painted over the lines, in order: what a test reads the
    /// numbers off.
    [[nodiscard]] std::vector<TextItem> texts() const;
    /// The marks of one kind over the lines -- or under them -- in order.
    [[nodiscard]] std::vector<Mark> marks(Mark::Kind kind, bool underLines = false) const;
};

/// Which forms of the lines a scene carries. Each is the whole of every line
/// again, and a scene of ten thousand lines is a great many points, so a host
/// asks for the forms it draws and goes without the rest.
struct SceneOptions
{
    bool runs = true;
    bool triangles = true;
    bool strips = false;
    bool values = true;
};

/// A sample to draw the crosshair on, named rather than pointed at: the line,
/// and the sample in the data's own units.
struct PinnedSample
{
    int line = -1;
    double x = 0.0;
    double y = 0.0;
};

/// What a scene is asked for. The default is the pane as it stands.
///
/// A *picture* is the same figure laid out again for somewhere else -- the
/// viewer's copied plot, at a size and in colours of the reader's choosing
/// (PlotPicture.qml). It is not a second figure, because what it draws is
/// exactly what the pane draws; it is the pane's lines in a frame of another
/// size. So a picture asks the lines for nothing -- no new window, no new
/// width, nothing for a source to fold or read on the picture's account --
/// and is drawn from what they hold for the pane. Nobody is pointing at a
/// picture and nobody is dragging across it: it has no band, and a reading
/// only when one is pinned.
struct SceneRequest
{
    SceneOptions geometry;
    bool picture = false;
    /// The picture's size in pane pixels and its pixel ratio; zero for the
    /// pane's own.
    double width = 0.0;
    double height = 0.0;
    double pixelRatio = 0.0;
    /// The light scope's picture, whatever the settings say: the page's.
    bool paper = false;
    /// No ground at all, so a page's own colour shows through.
    bool transparent = false;
    /// The crosshair on this sample, placed by the picture's own axes
    /// (PlotFrame.givenReading).
    std::optional<PinnedSample> reading;
};

class Figure
{
public:
    Figure();

    /// The figure's own lines, which it draws unless it is handed a source.
    [[nodiscard]] Plot& plot() { return plot_; }
    [[nodiscard]] const Plot& plot() const { return plot_; }
    /// Draw the host's lines instead of its own -- or its own again, given
    /// null. The source is the host's and must outlive the figure's use of it.
    /// What is chosen about each line (lineSettings) is kept by the line's
    /// number, which is the host's to keep in step with its lines.
    void setSource(LineSource* source);
    [[nodiscard]] LineSource& source()
    {
        return source_ != nullptr ? *source_ : static_cast<LineSource&>(plot_);
    }
    [[nodiscard]] const LineSource& source() const
    {
        return source_ != nullptr ? *source_ : static_cast<const LineSource&>(plot_);
    }
    [[nodiscard]] Camera& camera() { return camera_; }
    [[nodiscard]] const Camera& camera() const { return camera_; }

    [[nodiscard]] const FigureSettings& settings() const { return settings_; }
    /// Change the settings. A change of either axis's scale or of the flip
    /// resets the view, for PlotSurface's reason: a pan in decades and a pan
    /// in units have no reading that is both.
    void setSettings(const FigureSettings& settings);

    /// Take a line of the figure's own plot away, and what was chosen about it
    /// with it: the lines after it move down one, settings and all. With a
    /// source of the host's, the host has taken its line away already, and
    /// this moves what was chosen about the lines after it down with them.
    void removeLine(int line);
    /// Every line of its own plot, and everything chosen about any line.
    void clear();

    /// One line's settings; a line that has none yet gets the defaults.
    [[nodiscard]] LineSettings lineSettings(int line) const;
    void setLineSettings(int line, const LineSettings& settings);

    [[nodiscard]] const FigureStyle& style() const { return style_; }
    void setStyle(const FigureStyle& style);
    /// How the host measures text; one without both halves goes back to the
    /// style's approximation.
    void setMeasure(TextMeasure measure);

    /// The whole picture's size in pane pixels, and how many device pixels
    /// each one is.
    void setSize(double width, double height, double pixelRatio = 1.0);

    // --- gestures, in pane pixels with y down -----------------------------
    /// A wheel turned by `angleDelta` eighths of a degree, as Qt counts them:
    /// 120 is one notch. Shift is x, Control is y; Alt and Control together
    /// scale the line under the pointer and nothing else.
    void wheel(double px, double py, double angleDelta, Modifiers modifiers);
    /// Left drags the view; right draws a band to zoom to. Shift on that drag
    /// zooms across the pane only and Control up it only, the wheel's choice,
    /// and the band fills the pane the other way. Middle, or left with Alt,
    /// moves the line under the pointer up and down its axis. Control and a
    /// left click puts that line back.
    void press(double px, double py, Button button, Modifiers modifiers);
    void move(double px, double py, Modifiers modifiers);
    void release(double px, double py, Button button, Modifiers modifiers);
    /// Left puts the view back to the whole of every line.
    void doubleClick(double px, double py, Button button);
    void hover(double px, double py);
    void leave();
    void resetView();
    /// The window named in values -- the viewer's four boxes -- clipped to
    /// what the axes can draw.
    void setViewRange(double x0, double x1, double y0, double y1);

    // --- the picture -------------------------------------------------------
    [[nodiscard]] Scene scene() { return scene(SceneRequest{}); }
    [[nodiscard]] Scene scene(const SceneRequest& request);

    /// A value on an axis, written as that axis writes its ticks -- to the
    /// decimals the span on screen asks for, logarithmically where the axis
    /// is numbered so (PlotSurface.xNumber and yNumber). What the band's
    /// corners are written in, and what a host writing a position in words of
    /// its own asks.
    [[nodiscard]] std::string xNumber(double value);
    [[nodiscard]] std::string yNumber(double value);

    /// The line drawn nearest (`px`, `py`), within `reach` pixels, or -1.
    [[nodiscard]] int lineAt(double px, double py, double reach = 12.0);

    // --- a view, written down ------------------------------------------------
    /// The settings, every line's settings and the window, as JSON.
    [[nodiscard]] std::string state() const;
    /// Put back what state() wrote. A setting this build does not know is
    /// passed over and costs that setting, not the view. False when `json` is
    /// not a view at all.
    bool restore(const std::string& json);

private:
    struct Layout;
    struct LineWindow;

    /// Ask the source which lines are drawn and what they span, once, and set
    /// the axes' bounds from the answers. Every entry point that reads the
    /// lines starts here, and nothing after it asks the source again for what
    /// it holds: the viewer's Plot tab is ten thousand lines, and a question
    /// asked per line inside a loop over the lines is a hundred million.
    void refresh();
    /// Whether the line drawn at `position` is drawn on the common window, and
    /// whether its extent is part of that window's whole -- which a moved
    /// line's still is.
    [[nodiscard]] bool commonLine(int position) const;
    [[nodiscard]] bool spansCommon(int position) const;
    /// The window the line drawn at `position` is drawn in; with `always`, the
    /// one it would have if it were moved by nothing, which for a line on the
    /// common axis is that axis's view.
    [[nodiscard]] LineWindow windowOf(int position, bool always = false) const;
    /// Where a line is among those drawn, or -1.
    [[nodiscard]] int positionOf(int line) const;
    [[nodiscard]] std::string reason() const;
    [[nodiscard]] double commonFraction(double value) const;
    [[nodiscard]] Layout layout() const;
    [[nodiscard]] PlotView paneView(const Layout& layout) const;
    [[nodiscard]] Scene compose(const SceneRequest& request);
    /// The numbers a band being drawn writes beside itself (PlotBand.qml).
    void bandReadout(Scene& scene, const Layout& layout) const;
    /// The rectangle a band in progress draws. Both axes: the pointer's
    /// corners. One axis: the pane's whole extent the other way.
    [[nodiscard]] PaneRect bandRectangle(const PaneRect& area) const;
    /// `text` cut to `width` in `role`'s face, with an ellipsis at its end
    /// or, `middle`, in its middle -- as Qt elides a Text.
    [[nodiscard]] std::string elided(const std::string& text, double width, TextRole role,
                                     bool middle = false) const;
    /// `text` broken into lines no wider than `width` in `role`'s face.
    [[nodiscard]] std::vector<std::string> wrapped(const std::string& text, double width,
                                                   TextRole role) const;
    void scaleLine(int line, double py, double factor);
    void shiftLine(int line, double dy);
    void ensureLine(int line);

    Plot plot_;
    LineSource* source_ = nullptr;
    /// What refresh() found about each line drawn, in drawing order.
    struct Drawn
    {
        int line = -1;
        LineExtent extent;
        Span x;
        double xPositive = 0.0;
    };
    std::vector<Drawn> drawn_;
    Camera camera_;
    FigureSettings settings_;
    std::vector<LineSettings> lines_;
    FigureStyle style_;
    TextMeasure measure_;
    bool approximate_ = true;
    double width_ = 640.0;
    double height_ = 400.0;
    double pixelRatio_ = 1.0;

    // What a gesture in progress is doing.
    enum class Drag
    {
        None,
        Pan,
        Band,
        Line,
    };
    Drag drag_ = Drag::None;
    Point pressed_;
    Point last_;
    int dragLine_ = -1;
    bool moved_ = false;
    /// Whether a right press has gone far enough to be a band.
    bool banding_ = false;
    /// Which pane directions the band in progress will zoom. The wheel's
    /// choice, from the modifiers of the latest move.
    ZoomAxes bandAxes_ = ZoomAxes::Both;
    bool controlClick_ = false;
    std::optional<Point> pointer_;
};

} // namespace plotcore
