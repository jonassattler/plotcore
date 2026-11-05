// SPDX-FileCopyrightText: 2026 Jonas Sattler
// SPDX-FileCopyrightText: 2026 Andreas Martin
// SPDX-License-Identifier: BSD-3-Clause

#include "plotcore/Levels.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace plotcore {

namespace {

/// The careful reading: every element tested for being drawable before it is
/// compared. Correct for anything, and the slow path of the one below.
[[nodiscard]] Extremes carefulExtremes(const double* values, long long from, long long to)
{
    Extremes found;
    for (long long i = from; i < to; ++i) {
        const double value = values[i];
        if (!std::isfinite(value)) {
            continue;
        }
        if (found.lowAt < 0 || value < found.lowest) {
            found.lowest = value;
            found.lowAt = i;
        }
        if (found.highAt < 0 || value > found.highest) {
            found.highest = value;
            found.highAt = i;
        }
    }
    return found;
}

} // namespace

Extremes extremesOf(const double* values, long long from, long long to)
{
    Extremes found;
    if (values == nullptr) {
        return found;
    }

    // Two comparisons an element and nothing else in the loop.
    //
    // This is the innermost loop of the whole plot: it reads every element of
    // a line on the way to the summary and folds every level of the pyramid
    // above it, which together are about half of what a ten-million-element
    // dataset costs between being clicked on and being drawn. The version this
    // replaces asked three further questions per element -- isfinite(), and
    // whether either extreme had been seen yet -- and only the *first* element
    // of a run ever answers the last two differently.
    //
    // What lets all three go: **NaN fails every comparison**. Seeded with the
    // infinities, the first drawable element takes both branches and a NaN
    // takes neither, so being drawable needs no test of its own. An actual
    // infinity in the data does pass one of them, and that is the one case this
    // cannot decide -- so it hands the run to carefulExtremes() instead. Which
    // is rare: an infinity in a dataset is unusual, a NaN is not, and the two
    // used to cost the same.
    double lowest = std::numeric_limits<double>::infinity();
    double highest = -std::numeric_limits<double>::infinity();
    long long lowAt = -1;
    long long highAt = -1;
    for (long long i = from; i < to; ++i) {
        const double value = values[i];
        // Both, not one or the other: a run that only descends would otherwise
        // never take the second branch and would report no highest at all.
        if (value < lowest) {
            lowest = value;
            lowAt = i;
        }
        if (value > highest) {
            highest = value;
            highAt = i;
        }
    }

    if (lowAt < 0 && highAt < 0) {
        return found; // nothing passed either comparison: all NaN, and a gap
    }
    if (!std::isfinite(lowest) || !std::isfinite(highest)) {
        return carefulExtremes(values, from, to);
    }
    found.lowest = lowest;
    found.highest = highest;
    found.lowAt = lowAt;
    found.highAt = highAt;
    return found;
}

void joinSeam(Extremes& found, double exit, double entry, long long entryAt)
{
    // No stroke through a value that is not one: a chord to or from a NaN is
    // a line across a gap, and a run with no extremes is a gap already.
    if (!found.found() || !std::isfinite(exit) || !std::isfinite(entry)) {
        return;
    }
    // Strict, as the scan is. The entry is the latest element there is, so an
    // equal value has an earlier occurrence, and the earlier is the one named.
    if (entry < found.lowest) {
        found.lowest = entry;
        found.lowAt = entryAt;
    }
    else if (entry > found.highest) {
        found.highest = entry;
        found.highAt = entryAt;
    }
}

void joinSeam(double* pair, double* pairAt, double exit, double entry, double entryAt)
{
    if (pair == nullptr) {
        return;
    }
    // The pair back as the extremes it was folded from. Without positions the
    // two are only known to be in order, which is all first() and second()
    // ask: 0 and 1 for them, and 2 for the entry, which comes after both.
    Extremes found;
    const bool rising = pair[0] <= pair[1];
    found.lowest = rising ? pair[0] : pair[1];
    found.highest = rising ? pair[1] : pair[0];
    if (std::isnan(found.lowest) || std::isnan(found.highest)) {
        return; // a gap, which has no seam
    }
    const double firstAt = pairAt != nullptr ? pairAt[0] : 0.0;
    const double secondAt = pairAt != nullptr ? pairAt[1] : 1.0;
    // Positions are elements, so they are whole and the cast is exact.
    found.lowAt = static_cast<long long>(rising ? firstAt : secondAt);
    found.highAt = static_cast<long long>(rising ? secondAt : firstAt);
    joinSeam(found, exit, entry, static_cast<long long>(pairAt != nullptr ? entryAt : 2.0));
    pair[0] = found.first();
    pair[1] = found.second();
    if (pairAt != nullptr) {
        pairAt[0] = static_cast<double>(found.firstAt());
        pairAt[1] = static_cast<double>(found.secondAt());
    }
}

long long wholeBuckets(long long length, long long bucket)
{
    return length > bucket ? (length / bucket) * bucket : length;
}

namespace {

/// The element after bucket `b`'s last one, for its seam: in the buffer for
/// every bucket but the last, and `next` for that one.
[[nodiscard]] double seamOf(const double* values, long long to, long long count, double next)
{
    return to < count ? values[to] : next;
}

/// The fold of reduceBucketsInto(), with each bucket's seam or without: with
/// for elements, without for a buffer of pairs, whose seams came up with them
/// (see coarsenEnvelope()). One loop, so the two cannot come apart.
void foldInto(const double* values, long long count, long long width, double* out, bool seams,
              double next)
{
    const long long taken = (count + width - 1) / width;
    const auto nothing = std::numeric_limits<double>::quiet_NaN();
    for (long long b = 0; b < taken; ++b) {
        const long long from = b * width;
        const long long to = std::min(from + width, count);
        Extremes found = extremesOf(values, from, to);
        if (seams) {
            joinSeam(found, values[to - 1], seamOf(values, to, count, next), to);
        }
        if (!found.found()) {
            // Nothing drawable in the whole bucket. A pair of NaN is a gap, and
            // a gap is what that is -- dropping the bucket instead would slide
            // every later one left and draw the line across the hole.
            out[b * 2] = nothing;
            out[b * 2 + 1] = nothing;
            continue;
        }
        out[b * 2] = found.first();
        out[b * 2 + 1] = found.second();
    }
}

} // namespace

void reduceBucketsInto(const double* values, long long count, long long bucket, double* out,
                       double next)
{
    if (values == nullptr || out == nullptr || count <= 0) {
        return;
    }
    foldInto(values, count, std::max<long long>(bucket, 1), out, true, next);
}

void reduceBuckets(const double* values, long long count, long long bucket,
                   std::vector<double>& out, double next)
{
    if (values == nullptr || count <= 0) {
        return;
    }
    const long long width = std::max<long long>(bucket, 1);
    const long long taken = (count + width - 1) / width;
    const std::size_t was = out.size();
    out.resize(was + static_cast<std::size_t>(taken) * 2);
    reduceBucketsInto(values, count, width, out.data() + was, next);
}

void reduceBucketsLocated(const double* values, long long count, long long bucket, long long origin,
                          std::vector<double>& out, std::vector<double>& positions, double next)
{
    if (values == nullptr || count <= 0) {
        return;
    }
    const long long width = std::max<long long>(bucket, 1);
    const long long taken = (count + width - 1) / width;
    const std::size_t was = out.size();
    const std::size_t wasAt = positions.size();
    out.resize(was + static_cast<std::size_t>(taken) * 2);
    positions.resize(wasAt + static_cast<std::size_t>(taken) * 2);
    reduceBucketsLocatedInto(values, count, width, origin, out.data() + was,
                             positions.data() + wasAt, next);
}

void reduceBucketsLocatedInto(const double* values, long long count, long long bucket,
                              long long origin, double* out, double* positions, double next)
{
    if (values == nullptr || out == nullptr || positions == nullptr || count <= 0) {
        return;
    }
    const long long width = std::max<long long>(bucket, 1);
    const long long taken = (count + width - 1) / width;
    for (long long b = 0; b < taken; ++b) {
        const long long from = b * width;
        const long long to = std::min(from + width, count);
        Extremes found = extremesOf(values, from, to);
        joinSeam(found, values[to - 1], seamOf(values, to, count, next), to);
        if (!found.found()) {
            out[2 * b] = out[2 * b + 1] = std::numeric_limits<double>::quiet_NaN();
            positions[2 * b] = positions[2 * b + 1] = static_cast<double>(origin + from);
            continue;
        }
        out[2 * b] = found.first();
        out[2 * b + 1] = found.second();
        positions[2 * b] = static_cast<double>(origin + found.firstAt());
        positions[2 * b + 1] = static_cast<double>(origin + found.secondAt());
    }
}

void coarsenEnvelopeInto(const double* pairs, long long buckets, long long factor, double* out)
{
    if (pairs == nullptr || out == nullptr || buckets <= 0) {
        return;
    }
    // See the header: the pair buffer is already a sequence in occurrence
    // order, so folding it by twice the factor is the same question
    // reduceBuckets() asks of the elements themselves -- with no seam, because
    // the pairs carry theirs.
    foldInto(pairs, buckets * 2, std::max<long long>(factor, 1) * 2, out, false,
             std::numeric_limits<double>::quiet_NaN());
}

void coarsenEnvelopeLocatedInto(const double* pairs, const double* pairAt, long long buckets,
                                long long factor, double* out, double* outAt)
{
    if (pairs == nullptr || pairAt == nullptr || out == nullptr || outAt == nullptr ||
        buckets <= 0) {
        return;
    }
    // The same fold coarsenEnvelopeInto() asks of the pair buffer, by twice the
    // factor; the index it finds is into that buffer, and `pairAt` says which
    // element sits there.
    const long long width = std::max<long long>(factor, 1) * 2;
    const long long count = buckets * 2;
    const long long taken = (count + width - 1) / width;
    for (long long b = 0; b < taken; ++b) {
        const long long from = b * width;
        const Extremes found = extremesOf(pairs, from, std::min(from + width, count));
        if (!found.found()) {
            out[2 * b] = out[2 * b + 1] = std::numeric_limits<double>::quiet_NaN();
            outAt[2 * b] = outAt[2 * b + 1] = pairAt[from];
            continue;
        }
        out[2 * b] = found.first();
        out[2 * b + 1] = found.second();
        outAt[2 * b] = pairAt[found.firstAt()];
        outAt[2 * b + 1] = pairAt[found.secondAt()];
    }
}

void coarsenEnvelope(const double* pairs, long long buckets, long long factor,
                     std::vector<double>& out)
{
    if (pairs == nullptr || buckets <= 0) {
        return;
    }
    const long long width = std::max<long long>(factor, 1) * 2;
    const std::size_t was = out.size();
    out.resize(was + static_cast<std::size_t>((buckets * 2 + width - 1) / width) * 2);
    foldInto(pairs, buckets * 2, width, out.data() + was, false,
             std::numeric_limits<double>::quiet_NaN());
}

namespace {

/// The visible range clamped to the data, which is what a run has to cover.
void drawnRange(const LevelView& view, double& low, double& high)
{
    low = std::max(view.low, 0.0);
    high = std::min(view.high, static_cast<double>(view.length));
}

/// Octaves between two buckets.
double octavesApart(long long bucket, long long from)
{
    return std::abs(std::log2(static_cast<double>(std::max<long long>(bucket, 1))) -
                    std::log2(static_cast<double>(std::max<long long>(from, 1))));
}

} // namespace

int drawnLevel(std::span<const HeldLevel> held, const LevelView& view)
{
    if (held.empty() || !view.usable()) {
        return -1;
    }
    double low = 0.0;
    double high = 0.0;
    drawnRange(view, low, high);

    // The finest run that covers the pane. They all hold about the same number
    // of points, so a finer one is more detail on screen for the same cost --
    // and the coarser ones are still here for the moment the reader zooms out
    // past this one, which is the whole reason there is more than one.
    //
    // Of two as fine, the one held last. It answered the view as it is now;
    // the other answered a view the reader has since left, and which of those
    // a ladder still holds is its history rather than the view's. Two ladders
    // asked about one view must draw from the same run when both hold it -- a
    // line and the time base its x is read from, which a host sharpens through
    // the views on the way to this one -- or the line runs past the end of its
    // axis's run, and every point out there is placed by the coarse whole.
    int best = -1;
    for (std::size_t i = 0; i < held.size(); ++i) {
        const HeldLevel& level = held[i];
        if (!level.complete || !level.window.covers(low, high)) {
            continue;
        }
        if (best < 0 || level.window.bucket <= held[static_cast<std::size_t>(best)].window.bucket) {
            best = static_cast<int>(i);
        }
    }
    return best;
}

bool served(std::span<const HeldLevel> held, const LevelView& view, double low, double high,
            long long bucket)
{
    low = std::max(low, 0.0);
    high = std::min(high, static_cast<double>(view.length));
    return std::any_of(held.begin(), held.end(), [&](const HeldLevel& level) {
        return level.complete && level.window.bucket <= bucket && level.window.covers(low, high);
    });
}

namespace {

/// One run the policy wants in hand: the range it has to cover, the coarsest
/// bucket that answers it, and the run to fold or read when none does.
struct Wanted
{
    double low = 0.0;
    double high = 0.0;
    long long bucket = 0;
    PlotWindow window;
};

/// Every run the policy wants held, in the order it asks for them: the one the
/// pane is waiting for, the octaves in towards the focus, and the octaves out
/// from the run being drawn.
///
/// One list for both halves of the policy. wantedLevel() asks for the first of
/// these nothing in hand answers, and coldestLevel() keeps whatever answers any
/// of them -- and they were two pieces of arithmetic until 0.7.2, which is how
/// a full ladder came to evict the fourth octave in towards the pointer the
/// moment it was folded, because by distance from the pane's bucket it was the
/// furthest run held. Asked for again on the next turn, evicted again, until
/// the loop ran out of turns and the run went to the file.
std::vector<Wanted> pathOf(std::span<const HeldLevel> held, const LevelView& view,
                           const PlotFocus& focus)
{
    std::vector<Wanted> path;
    if (!view.usable()) {
        return path;
    }
    const std::optional<PlotWindow> needed =
        windowFor(view.low, view.high, view.length, view.paneBuckets);
    if (!needed.has_value()) {
        // Zoomed out far enough that the whole-line summary is already at this
        // bucket or finer. There is nothing a second read could add.
        return path;
    }

    // What the pane is waiting for, an octave finer than it asked -- first,
    // because it is the only one the reader can see. A finer run that would
    // reach past the end of the line has no octave to take, but the pane still
    // wants a finer bucket than the whole-line summary has; only lines between
    // one and two panes' worth of buckets long land there.
    double low = 0.0;
    double high = 0.0;
    drawnRange(view, low, high);
    const std::optional<PlotWindow> detail =
        windowFor(view.low, view.high, view.length, view.detailBuckets);
    path.push_back(Wanted{low, high, needed->bucket, detail.value_or(*needed)});

    // Inward, and only towards a focus. A run costs its span and each octave in
    // is half the span of the one above, so the whole inward ladder is cheaper
    // than one step out -- which is why it can be four octaves deep and why it
    // is worth folding before the outward one. Without a focus there is nothing
    // to aim it at: the view's own centre is where detailBuckets() already
    // reads an octave finer, and guessing further in about a reader who may be
    // about to zoom out is spending memory on a coin toss.
    if (focus.active && focus.inward) {
        for (int octave = 1; octave <= kFocusOctavesIn; ++octave) {
            // Where the view goes if the reader keeps zooming about this point:
            // the pointer holds still and both edges come in towards it, which
            // is exactly what PlotSurface.zoomedAxis does.
            const double shrink = 1.0 / static_cast<double>(1LL << octave);
            const double wantLow = focus.position - (focus.position - view.low) * shrink;
            const double wantHigh = focus.position + (view.high - focus.position) * shrink;
            const std::optional<PlotWindow> in =
                windowFor(wantLow, wantHigh, view.length, view.paneBuckets);
            if (!in.has_value()) {
                break; // finer than the line has anything to say about
            }
            path.push_back(Wanted{wantLow, wantHigh, in->bucket, *in});
        }
    }

    // ...and then the octaves out, nearest first.
    //
    // Measured from the *run* the pane is being drawn from rather than from the
    // view, and that is not a detail. A run is twice the pane wide and steps by
    // a quarter of itself, so panning about inside one leaves this arithmetic
    // untouched -- which is what keeps "panning reads nothing" true of the
    // prefetch as well as of the picture. Taken from the view, every pan would
    // shift the octaves a little and eventually ask for one.
    const int at = drawnLevel(held, view);
    if (at < 0) {
        return path;
    }
    const PlotWindow base = held[static_cast<std::size_t>(at)].window;
    const double half = static_cast<double>(base.span) / 2.0;
    double centre = static_cast<double>(base.first) + half;
    if (focus.active) {
        // A zoom out about a pointer near one edge extends mostly the other
        // way, so the centre leans towards it -- halfway, not all the way,
        // because the run on screen still has to come back covered. Only a
        // wheel sets a focus, so a pan cannot move this.
        centre = (centre + focus.position) / 2.0;
    }
    for (int octave = 1; octave <= view.prefetchOctaves; ++octave) {
        const auto reach = static_cast<double>(1LL << octave);
        // Whatever the lean did, never less than the run it came from: a step
        // out that did not cover what is already on screen would be a step out
        // into a blank.
        const double wantLow = std::min(centre - half * reach, static_cast<double>(base.first));
        const double wantHigh =
            std::max(centre + half * reach, static_cast<double>(base.first + base.span));
        const std::optional<PlotWindow> out =
            windowFor(wantLow, wantHigh, view.length, view.paneBuckets);
        if (!out.has_value()) {
            // That far out the whole-line summary already covers it, and so
            // does everything past it.
            break;
        }
        path.push_back(Wanted{wantLow, wantHigh, out->bucket, *out});
    }
    return path;
}

} // namespace

std::optional<PlotWindow> wantedLevel(std::span<const HeldLevel> held, const LevelView& view,
                                      const PlotFocus& focus)
{
    // Asked about the range rather than about the window, because a run
    // already in hand may answer for a step without being the run this would
    // have read: one the reader zoomed in from is finer and wider than the
    // octave out of where they are now, and folding it again would be work
    // spent on nothing.
    for (const Wanted& want : pathOf(held, view, focus)) {
        if (!served(held, view, want.low, want.high, want.bucket)) {
            return want.window;
        }
    }
    return {};
}

std::size_t coldestLevel(std::span<const HeldLevel> held, const LevelView& view,
                         const PlotFocus& focus)
{
    if (held.empty()) {
        return 0;
    }
    const std::optional<PlotWindow> needed =
        windowFor(view.low, view.high, view.length, view.paneBuckets);
    const int at = drawnLevel(held, view);
    const long long drawing = at >= 0 ? held[static_cast<std::size_t>(at)].window.bucket
                                      : std::numeric_limits<long long>::max();

    // Abandoned speculation first: a run finer than the one on screen was read
    // on a guess about where the zoom was going, and a focus somewhere it does
    // not cover says the guess was wrong. Ranked above every distance, so it
    // goes before anything still on the reader's path.
    const auto forsaken = [&](const HeldLevel& level) {
        if (!focus.active || at < 0 || level.window.bucket >= drawing) {
            return false;
        }
        return !level.window.covers(focus.position, focus.position);
    };

    const auto distance = [&](const HeldLevel& level) {
        if (!needed.has_value()) {
            return 0.0;
        }
        return octavesApart(level.window.bucket, needed->bucket);
    };

    // What answers a step the policy still wants is kept while anything else
    // can go: for each run on the path, the coarsest held run that serves it,
    // which is the one served() would find. Only when every run in hand is one
    // of those -- a ladder too short for the path, which a small budget gives --
    // does the ranking below choose among them, and then the plot stops folding
    // what it cannot keep rather than asking for it again.
    std::vector<bool> kept(held.size(), false);
    if (at >= 0) {
        kept[static_cast<std::size_t>(at)] = true;
    }
    for (const Wanted& want : pathOf(held, view, focus)) {
        const double low = std::max(want.low, 0.0);
        const double high = std::min(want.high, static_cast<double>(view.length));
        int answer = -1;
        for (std::size_t i = 0; i < held.size(); ++i) {
            const HeldLevel& level = held[i];
            if (!level.complete || level.window.bucket > want.bucket ||
                !level.window.covers(low, high)) {
                continue;
            }
            if (answer < 0 ||
                level.window.bucket > held[static_cast<std::size_t>(answer)].window.bucket) {
                answer = static_cast<int>(i);
            }
        }
        if (answer >= 0) {
            kept[static_cast<std::size_t>(answer)] = true;
        }
    }
    const bool anySpare = std::find(kept.begin(), kept.end(), false) != kept.end();

    std::optional<std::size_t> worst;
    for (std::size_t i = 0; i < held.size(); ++i) {
        if (anySpare && kept[i]) {
            continue;
        }
        if (!worst.has_value()) {
            worst = i;
            continue;
        }
        const bool lost = forsaken(held[i]);
        const bool losing = forsaken(held[*worst]);
        if (lost != losing) {
            if (lost) {
                worst = i;
            }
            continue;
        }
        if (distance(held[i]) > distance(held[*worst])) {
            worst = i;
        }
    }
    return worst.value_or(0);
}

std::optional<LogColumns> logColumnsFor(double low, double high, int columns)
{
    if (columns <= 0 || !(low > 0.0) || !(high > low) || !std::isfinite(high)) {
        return {};
    }
    const double from = std::log2(low);
    const double to = std::log2(high);
    const double octaves = to - from;
    // Under an octave the linear path is the right one; see the header.
    if (!(octaves >= 1.0) || !std::isfinite(octaves)) {
        return {};
    }
    // Rounded up to a power of two, so between one and two edges land in each
    // pixel column. The bound is the pane: at an octave exactly it is
    // `columns` rounded up, and it only falls from there.
    const double wanted = static_cast<double>(columns) / octaves;
    long long density = 1;
    while (static_cast<double>(density) < wanted && density < (1LL << 30)) {
        density <<= 1;
    }
    const double margin = octaves / 2.0;
    LogColumns grid;
    grid.density = density;
    grid.first = static_cast<long long>(std::floor((from - margin) * static_cast<double>(density)));
    grid.last = static_cast<long long>(std::ceil((to + margin) * static_cast<double>(density)));
    return grid;
}

bool logColumnsServe(const LogColumns& held, double low, double high, int columns)
{
    const std::optional<LogColumns> wanted = logColumnsFor(low, high, columns);
    return wanted.has_value() && wanted->density == held.density && held.covers(low, high);
}

bool LogFoldGrid::serves(double atStart, double atStep, int atMode, int atBuckets, double low,
                         double high) const
{
    return columns.has_value() && start == atStart && step == atStep && mode == atMode &&
           buckets == atBuckets && logColumnsServe(*columns, low, high, atBuckets);
}

void LogFoldGrid::remake(double atStart, double atStep, int atMode, int atBuckets, double low,
                         double high)
{
    columns = logColumnsFor(low, high, atBuckets);
    start = atStart;
    step = atStep;
    mode = atMode;
    buckets = atBuckets;
}

void edgesAlong(const LogColumns& columns, double start, double step, std::vector<double>& out)
{
    out.clear();
    if (columns.density <= 0 || columns.last < columns.first || !std::isfinite(start) ||
        !std::isfinite(step) || !(std::abs(step) > 0.0)) {
        return;
    }
    out.reserve(static_cast<std::size_t>(columns.last - columns.first + 1));
    for (long long k = columns.first; k <= columns.last; ++k) {
        out.push_back((columns.edge(k) - start) / step);
    }
    if (step < 0.0) {
        std::reverse(out.begin(), out.end());
    }
}

PaneColumns::Step PaneColumns::request(int columns)
{
    // Down to the quantum, and never to nothing. See kColumnQuantum for why
    // down rather than to the nearest.
    const int quantised = std::clamp((std::max(columns, 0) / kColumnQuantum) * kColumnQuantum,
                                     kMinPoints / 2, kMaxPoints / 2);
    if (quantised == wanted) {
        return Step::Nothing;
    }
    wanted = quantised;
    if (wanted == applied) {
        return Step::Cancel;
    }
    if (!measured) {
        // The surface measuring itself for the first time. There is no gesture
        // to wait out: whatever has been read so far was read at an assumed
        // width, so waiting would open every plot at the wrong resolution and
        // re-read every line of it a fifth of a second later.
        measured = true;
        return Step::Apply;
    }
    // The read is at the end of the drag, not once per sixty-four pixels of it.
    return Step::Wait;
}

bool PaneColumns::apply()
{
    if (wanted == applied) {
        return false;
    }
    applied = wanted;
    return true;
}

void ZoomFocus::set(double atX, double factor)
{
    if (!std::isfinite(atX) || !std::isfinite(factor) || !(factor > 0.0)) {
        clear();
        return;
    }
    x = atX;
    inward = factor > 1.0;
    active = true;
}

double ColumnGrid::edge(long long k) const
{
    const double at = static_cast<double>(k) * perColumn;
    return logarithmic ? std::pow(base, at) : at;
}

std::optional<ColumnGrid> columnGridFor(double low, double high, bool logarithmic, double base,
                                        double columns)
{
    if (!std::isfinite(columns) || !(columns >= 1.0)) {
        return std::nullopt;
    }
    const AxisMapping map = mappingOver(low, high, logarithmic, base);
    if (!map.usable) {
        return std::nullopt;
    }
    const double width = (map.high - map.low) / columns;
    if (!std::isfinite(width) || !(width > 0.0)) {
        return std::nullopt;
    }
    int exponent = 0;
    const double fraction = std::frexp(width, &exponent);
    constexpr double kKept = 1 << 20;
    ColumnGrid grid;
    grid.perColumn = std::ldexp(std::round(fraction * kKept) / kKept, exponent);
    grid.logarithmic = logarithmic;
    grid.base = base;
    // Past 2^53 a double no longer counts columns one at a time, and two
    // neighbours would be the same column.
    const double first = map.low / grid.perColumn;
    if (!std::isfinite(first) || std::abs(first) > 9007199254740992.0) {
        return std::nullopt;
    }
    grid.first = std::llround(first);
    grid.count = static_cast<int>(std::min(std::ceil(columns), 1e9));
    return grid;
}

ColumnRule columnRuleFor(double pixelRatio)
{
    const double ratio = std::max(pixelRatio, 1.0);
    ColumnRule rule;
    rule.run = static_cast<int>(std::lround(kBarRunPixels * ratio));
    rule.gap = static_cast<int>(std::lround(kBarGapPixels * ratio));
    return rule;
}

void filledColumns(std::span<const double> tops, std::span<const double> bottoms,
                   std::span<const unsigned char> kinds, std::span<const unsigned char> prior,
                   double strokeWidth, std::vector<unsigned char>& filled)
{
    const std::size_t count = std::min(tops.size(), bottoms.size());
    filled.assign(count, 0);
    const auto drawn = [&](std::size_t c) {
        return std::isfinite(tops[c]) && std::isfinite(bottoms[c]);
    };
    const auto kind = [&](std::size_t c) -> unsigned char {
        return c < kinds.size() ? kinds[c] : static_cast<unsigned char>(kColumnWhole);
    };
    const auto thin = [&](std::size_t c) { return kind(c) == kColumnThin; };
    // What a column is judged against: a cut one has an accidental height.
    const auto neighbour = [&](std::size_t c) { return drawn(c) && kind(c) != kColumnCut; };
    for (std::size_t c = 0; c < count; ++c) {
        if (!neighbour(c)) {
            continue;
        }
        if ((c > 0 && thin(c - 1)) || (c + 1 < count && thin(c + 1))) {
            continue;
        }
        const bool was = c < prior.size() && prior[c] != 0;
        const double overlap = (was ? kBarLeaveOverlap : kBarEnterOverlap) * strokeWidth;
        const double heights = was ? kBarLeaveHeights : kBarEnterHeights;
        const auto together = [&](std::size_t other) {
            const double common =
                std::min(bottoms[c], bottoms[other]) - std::max(tops[c], tops[other]);
            const double mine = bottoms[c] - tops[c];
            const double theirs = bottoms[other] - tops[other];
            return common >= overlap && std::min(mine, theirs) >= heights * std::max(mine, theirs);
        };
        const bool left = c > 0 && neighbour(c - 1);
        const bool right = c + 1 < count && neighbour(c + 1);
        filled[c] =
            (left || right) && (!left || together(c - 1)) && (!right || together(c + 1)) ? 1 : 0;
    }
    for (std::size_t c = 0; c < count; ++c) {
        if (kind(c) == kColumnCut && drawn(c)) {
            filled[c] = (c > 0 && filled[c - 1] != 0) || (c + 1 < count && filled[c + 1] != 0);
        }
    }
}

void denseStretches(std::span<const unsigned char> filled, std::span<const unsigned char> kinds,
                    const ColumnRule& rule, std::vector<unsigned char>& dense)
{
    const auto thin = [&](std::size_t c) { return c < kinds.size() && kinds[c] == kColumnThin; };
    dense.assign(filled.begin(), filled.end());
    const std::size_t count = dense.size();
    // A gap only counts between two stretches: one running off either end of
    // the columns has an unknown width, and margin() puts that far from the pane.
    // Measured on `filled`, so one filled gap does not lengthen what the next is
    // measured against: an answer stays local however long a chain of them runs.
    std::size_t runStart = count;
    std::size_t runEnd = count;
    for (std::size_t c = 0; c < count;) {
        if (thin(c)) {
            runStart = runEnd = count;
            ++c;
            continue;
        }
        if (filled[c] == 0) {
            ++c;
            continue;
        }
        std::size_t end = c;
        while (end < count && filled[end] != 0) {
            ++end;
        }
        if (runEnd != count) {
            const std::size_t gap = c - runEnd;
            const std::size_t before = runEnd - runStart;
            const std::size_t after = end - c;
            if (gap <= static_cast<std::size_t>(rule.gap) && before >= gap && after >= gap) {
                std::fill(dense.begin() + static_cast<std::ptrdiff_t>(runEnd),
                          dense.begin() + static_cast<std::ptrdiff_t>(c), 1);
            }
        }
        runStart = c;
        runEnd = end;
        c = end;
    }
    // ...and a stretch only counts as short if both its ends are in the columns.
    for (std::size_t c = 0; c < count;) {
        if (dense[c] == 0) {
            ++c;
            continue;
        }
        std::size_t end = c;
        while (end < count && dense[end] != 0) {
            ++end;
        }
        if (c > 0 && end < count && end - c < static_cast<std::size_t>(rule.run)) {
            std::fill(dense.begin() + static_cast<std::ptrdiff_t>(c),
                      dense.begin() + static_cast<std::ptrdiff_t>(end), 0);
        }
        c = end;
    }
    // Dropping a stretch can leave its neighbours closer than the gaps above were
    // measured to be, and what survived is long enough to be filled between.
    std::size_t last = count;
    for (std::size_t c = 0; c < count; ++c) {
        if (thin(c)) {
            last = count;
        }
        if (dense[c] == 0) {
            continue;
        }
        if (last != count && c - last - 1 <= static_cast<std::size_t>(rule.gap)) {
            std::fill(dense.begin() + static_cast<std::ptrdiff_t>(last),
                      dense.begin() + static_cast<std::ptrdiff_t>(c), 1);
        }
        last = c;
    }
}

void carriedColumns(const ColumnGrid& from, std::span<const unsigned char> answers,
                    const ColumnGrid& to, std::vector<unsigned char>& out)
{
    out.assign(static_cast<std::size_t>(std::max(to.count, 0)), 0);
    if (from.logarithmic != to.logarithmic || from.base != to.base || !(from.perColumn > 0.0)) {
        return;
    }
    for (std::size_t c = 0; c < out.size(); ++c) {
        const double middle =
            (static_cast<double>(to.first) + static_cast<double>(c) + 0.5) * to.perColumn;
        const long long at = from.columnAt(middle);
        if (at >= 0 && static_cast<std::size_t>(at) < answers.size()) {
            out[c] = answers[static_cast<std::size_t>(at)];
        }
    }
}

} // namespace plotcore
