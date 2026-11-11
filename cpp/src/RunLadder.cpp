// SPDX-FileCopyrightText: 2026 Jonas Sattler
// SPDX-License-Identifier: BSD-3-Clause

#include "plotcore/RunLadder.hpp"

#include <algorithm>

namespace plotcore {

RunLadder::~RunLadder()
{
    // Given up through the host on the way out too: a ladder destroyed while a
    // renderer is still drawing one of its runs is the case the retire store
    // exists for.
    clear();
}

RunLadder& RunLadder::operator=(RunLadder&& other) noexcept
{
    if (this != &other) {
        clear();
        retire_ = std::move(other.retire_);
        lines_ = std::move(other.lines_);
        view_ = other.view_;
        focus_ = other.focus_;
        held_ = other.held_;
        runs_ = std::move(other.runs_);
        other.runs_.clear();
    }
    return *this;
}

void RunLadder::setLines(std::vector<int> lines)
{
    if (lines == lines_) {
        return;
    }
    // A line no longer drawn is no longer held: the runs exist to spare a read
    // of a line on screen, not to accumulate every line the reader has ever
    // ticked. Through retire, because the renderer may be drawing it until the
    // host hands it what replaces it.
    std::vector<int> sorted = lines;
    std::ranges::sort(sorted);
    for (Run& run : runs_) {
        for (auto it = run.lines.begin(); it != run.lines.end();) {
            if (std::ranges::binary_search(sorted, it->first)) {
                ++it;
                continue;
            }
            retire(it->second.values);
            retire(it->second.positions);
            it = run.lines.erase(it);
        }
    }
    lines_ = std::move(lines);
}

void RunLadder::setView(const LevelView& view, const PlotFocus& focus)
{
    view_ = view;
    focus_ = focus;
}

std::optional<PlotWindow> RunLadder::window(int buckets) const
{
    if (!view_.usable() || buckets <= 0) {
        return {};
    }
    return windowFor(view_.low, view_.high, view_.length, buckets);
}

bool RunLadder::complete(const Run& run) const
{
    // Every line of the group: a run read before a line was ticked back on is
    // a run that cannot draw it. And at least one with something in it: a line
    // that came back empty is held so it is not asked for again, and is drawn
    // from its whole-line summary -- so a run of nothing else is not a closer
    // look, and a coarser one that is should be drawn instead.
    if (lines_.empty()) {
        return false;
    }
    bool any = false;
    for (const int line : lines_) {
        const auto found = run.lines.find(line);
        if (found == run.lines.end()) {
            return false;
        }
        any = any || !found->second.values.empty();
    }
    return any;
}

std::span<const HeldLevel> RunLadder::held() const
{
    ladder_.clear();
    ladder_.reserve(runs_.size());
    for (const Run& run : runs_) {
        ladder_.push_back(HeldLevel{run.window, complete(run)});
    }
    return ladder_;
}

const RunLadder::Run* RunLadder::drawn() const
{
    if (!view_.usable()) {
        return nullptr;
    }
    const int at = drawnLevel(held(), view_);
    return at >= 0 ? &runs_[static_cast<std::size_t>(at)] : nullptr;
}

std::optional<PlotWindow> RunLadder::wanted() const
{
    if (!view_.usable()) {
        return {};
    }
    return wantedLevel(held(), view_, focus_);
}

bool RunLadder::folds(const PlotWindow& window, const PyramidOf& pyramidOf) const
{
    if (lines_.empty() || !pyramidOf) {
        return false;
    }
    return std::ranges::all_of(lines_, [&](int line) {
        const LinePyramid* pyramid = pyramidOf(line);
        return pyramid != nullptr && !pyramid->empty() && window.bucket >= pyramid->baseBucket();
    });
}

bool RunLadder::holds(const PlotWindow& window) const
{
    return std::ranges::any_of(runs_, [&window](const Run& run) { return run.window == window; });
}

RunLadder::Run* RunLadder::runAt(const PlotWindow& window)
{
    const auto found =
        std::ranges::find_if(runs_, [&window](const Run& run) { return run.window == window; });
    return found != runs_.end() ? &*found : nullptr;
}

bool RunLadder::fold(const PlotWindow& window, const PyramidOf& pyramidOf)
{
    if (!folds(window, pyramidOf)) {
        return false;
    }
    Run* run = runAt(window);
    bool any = false;
    for (const int line : lines_) {
        if (run != nullptr && run->lines.contains(line)) {
            continue; // already held at this run
        }
        LineRun folded;
        if (!fillWindow(*pyramidOf(line), window, folded.values, &folded.positions)) {
            continue;
        }
        if (run == nullptr) {
            // What a read of this run would have reported: a pair per bucket,
            // each at the element it is, or the elements themselves when the
            // bucket is one. This may reallocate the runs while a renderer is
            // drawing one of them, which is why a Run is move-only.
            run = &runs_.emplace_back(
                window, window.bucket == 1 ? 1.0 : static_cast<double>(window.bucket) / 2.0);
        }
        run->lines[line] = std::move(folded);
        any = true;
    }
    return any;
}

bool RunLadder::refresh(const PyramidOf& pyramidOf)
{
    if (!view_.usable()) {
        return false;
    }
    // Everything the pyramids can answer, now, in this call.
    //
    // A run at or above a line's base bucket is a fold of a buffer already in
    // hand, so there is nothing to wait for and no reply to arm the next step
    // with -- which is why this loops where a read returns. The whole ladder
    // the policy wants is usually memory: the run on screen, the octaves in
    // towards the pointer and the ones out, all of them folded before the
    // frame that asked is drawn. That is the difference between a zoom that
    // resolves as the reader turns the wheel and one that answers a tenth of a
    // second after they stop.
    bool filled = false;

    // The pane's own preferred run first, whether or not something coarser in
    // hand would already have covered it.
    //
    // wantedLevel() stops asking once *some* held run covers the view at a
    // bucket no coarser than the pane strictly needs, and that was the right
    // rule while every run cost a round trip: a run read on the way in is
    // usually a little finer than the next view out needs, and re-reading the
    // file to gain a fraction of an octave would have been a round trip spent
    // on almost nothing. The cost of settling for it is that the pane can be
    // drawn at up to an octave coarser than it asked -- about one drawn station
    // per column where it asked for two, which a column of two stations
    // absorbs.
    //
    // Out of a held line that trade has no second side. The finer run is a fold
    // of a buffer already in hand, so there is nothing to weigh against it, and
    // the pane gets the resolution it asked for on every frame rather than on
    // the frames where the ladder happens to line up.
    if (const std::optional<PlotWindow> own = window(view_.detailBuckets);
        own.has_value() && fold(*own, pyramidOf)) {
        filled = true;
        trim();
    }
    // ...then the ladder, every rung the pyramids can answer, in this call.
    //
    // Bounded by the count held rather than trusted to end on its own. It
    // does end -- wantedLevel() never names a run that one held already
    // answers, and trim() keeps the ladder at the count -- but a loop whose
    // exit is a policy in another file is a loop worth bounding here. One that
    // ran to its bound stopped because it ran out of turns rather than because
    // the ladder was full, and in a release build that would not look like a
    // bug: it would look like the plot had become slow again, a read for a run
    // memory could answer. test_plotruns holds it to the second half: after one
    // refresh out of pyramids that answer everything, nothing is left to read.
    for (int step = 0; step <= held_; ++step) {
        const std::optional<PlotWindow> next = wanted();
        if (!next.has_value() || !fold(*next, pyramidOf)) {
            break;
        }
        filled = true;
        trim();
        if (!holds(*next)) {
            // The trim gave up what was just folded: the ladder is full of
            // runs nearer the reader, and asking again would fold it again.
            break;
        }
    }
    trim();
    return filled;
}

std::optional<RunRead> RunLadder::read(const PyramidOf& pyramidOf) const
{
    const std::optional<PlotWindow> want = wanted();
    // A run the pyramids can answer is never read, even when refresh() could
    // not keep it -- the ladder had no room for it, and the file would answer
    // it no differently: the reply would be trimmed away as the fold was. This
    // used to go to the file, which is how a zoom into 7777777 of a
    // ten-million-element line read once where a zoom into the middle of it
    // read nothing.
    if (!want.has_value() || folds(*want, pyramidOf)) {
        return {};
    }
    // Every line not yet held at it. Usually all of them -- the run is one
    // never held -- and sometimes one, which is a line ticked in the legend
    // while the reader was already looking closely.
    RunRead read{*want, {}};
    const auto run =
        std::ranges::find_if(runs_, [&want](const Run& held) { return held.window == *want; });
    for (const int line : lines_) {
        if (run == runs_.end() || !run->lines.contains(line)) {
            read.lines.push_back(line);
        }
    }
    if (read.lines.empty()) {
        return {};
    }
    return read;
}

void RunLadder::land(const PlotWindow& window, int line, double step, std::vector<double> values,
                     std::vector<double> positions)
{
    Run* run = runAt(window);
    if (run == nullptr) {
        // A window not held before. Nothing is given up for it here -- the
        // runs already held are the ones the reader zoomed through, and are
        // exactly what makes going back free; trim() is the host's to call.
        run = &runs_.emplace_back(window, step);
    }
    else if (!values.empty()) {
        run->step = step;
    }
    // Retired, not overwritten: a renderer may be drawing what is there now,
    // and goes on drawing it until it is handed what replaces it.
    if (const auto held = run->lines.find(line); held != run->lines.end()) {
        retire(held->second.values);
        retire(held->second.positions);
    }
    run->lines[line] = LineRun{std::move(values), std::move(positions)};
}

void RunLadder::trim()
{
    while (static_cast<int>(runs_.size()) > std::max(held_, 0)) {
        // Asked again each time round, because giving one up changes which of
        // the rest is coldest.
        const std::size_t worst = view_.usable() ? coldestLevel(held(), view_, focus_) : 0;
        retire(runs_[worst]);
        runs_.erase(runs_.begin() + static_cast<std::ptrdiff_t>(worst));
    }
}

void RunLadder::clear()
{
    for (Run& run : runs_) {
        retire(run);
    }
    runs_.clear();
}

long long RunLadder::heldDoubles() const
{
    long long doubles = 0;
    for (const Run& run : runs_) {
        for (const auto& [line, held] : run.lines) {
            doubles += static_cast<long long>(held.values.size() + held.positions.size());
        }
    }
    return doubles;
}

void RunLadder::retire(Run& run)
{
    for (auto& [line, held] : run.lines) {
        retire(held.values);
        retire(held.positions);
    }
}

void RunLadder::retire(std::vector<double>& values)
{
    if (retire_ && !values.empty()) {
        retire_(values);
    }
    values.clear();
}

} // namespace plotcore
