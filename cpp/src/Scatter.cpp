// SPDX-FileCopyrightText: 2026 Jonas Sattler
// SPDX-License-Identifier: BSD-3-Clause

#include "plotcore/Scatter.hpp"

#include "plotcore/Parallel.hpp"

#include <array>
#include <atomic>
#include <stdexcept>
#include <type_traits>

namespace plotcore {
namespace {

/// Bits of a position the sort quantises to, along each axis: sixteen, so a
/// Morton code and a slot share one 64-bit key and the sort carries nothing
/// beside it. Deeper than that a node is split by its own box (refine).
constexpr int kQuantumBits = 16;

/// How deep a node may be split by its box. A box that two neighbouring
/// doubles bound cannot be halved any further, and well before that the
/// points in it stand at one place to every pane there is.
constexpr int kMaxDepth = 60;

/// Points a piece of a pass over every point, on one thread.
constexpr std::size_t kPiece = std::size_t{1} << 16;

std::atomic<std::uint64_t> nextGeneration{1};

long long piecesOf(std::size_t n)
{
    return static_cast<long long>((n + kPiece - 1) / kPiece);
}

/// `floor(value)`, without the library call a baseline x86-64 build makes of
/// std::floor: the walk asks it of every point it lands, and the call was a
/// measurable share of a frame.
long long floorOf(double value)
{
    const auto whole = static_cast<long long>(value);
    return whole - (value < static_cast<double>(whole) ? 1 : 0);
}

/// `x`'s low sixteen bits, spread to every other bit.
std::uint32_t spread(std::uint32_t x)
{
    x &= 0xffffu;
    x = (x | (x << 8)) & 0x00ff00ffu;
    x = (x | (x << 4)) & 0x0f0f0f0fu;
    x = (x | (x << 2)) & 0x33333333u;
    x = (x | (x << 1)) & 0x55555555u;
    return x;
}

/// Sort by the high 32 bits, stably, the low 32 riding along -- which is what a
/// key of a code, or an element, and a slot is for. Three passes of eleven,
/// eleven and ten bits, each counted and scattered by pieces over the pool.
void sortHigh(std::vector<std::uint64_t>& keys, std::vector<std::uint64_t>& spare)
{
    const std::size_t n = keys.size();
    if (n < 2) {
        return;
    }
    if (n < 4096) {
        std::stable_sort(keys.begin(), keys.end(),
                         [](std::uint64_t a, std::uint64_t b) { return (a >> 32) < (b >> 32); });
        return;
    }
    spare.resize(n);
    const long long pieces = piecesOf(n);
    std::vector<std::size_t> counts;
    int shift = 32;
    for (const int bits : {11, 11, 10}) {
        const std::size_t buckets = std::size_t{1} << bits;
        const std::uint64_t mask = buckets - 1;
        counts.assign(buckets * static_cast<std::size_t>(pieces), 0);
        parallelFor(pieces, [&](long long piece) {
            std::size_t* count = counts.data() + buckets * static_cast<std::size_t>(piece);
            const std::size_t from = static_cast<std::size_t>(piece) * kPiece;
            const std::size_t to = std::min(n, from + kPiece);
            for (std::size_t i = from; i < to; ++i) {
                ++count[(keys[i] >> shift) & mask];
            }
        });
        // Where each piece's run of each bucket starts: bucket by bucket, and
        // piece by piece within one, which is what keeps the sort stable.
        std::size_t total = 0;
        bool trivial = false;
        for (std::size_t b = 0; b < buckets && !trivial; ++b) {
            std::size_t inBucket = 0;
            for (long long p = 0; p < pieces; ++p) {
                std::size_t& count = counts[buckets * static_cast<std::size_t>(p) + b];
                const std::size_t held = count;
                count = total;
                total += held;
                inBucket += held;
            }
            trivial = inBucket == n; // every key has the same bits here
        }
        if (!trivial) {
            parallelFor(pieces, [&](long long piece) {
                std::size_t* at = counts.data() + buckets * static_cast<std::size_t>(piece);
                const std::size_t from = static_cast<std::size_t>(piece) * kPiece;
                const std::size_t to = std::min(n, from + kPiece);
                for (std::size_t i = from; i < to; ++i) {
                    spare[at[(keys[i] >> shift) & mask]++] = keys[i];
                }
            });
            keys.swap(spare);
        }
        shift += bits;
    }
}

/// Two slots exchanged, with everything each carries.
void swapSlots(PointTree& tree, std::uint32_t a, std::uint32_t b)
{
    std::swap(tree.x[a], tree.x[b]);
    std::swap(tree.y[a], tree.y[b]);
    std::swap(tree.element[a], tree.element[b]);
    if (!tree.values.empty()) {
        std::swap(tree.values[a], tree.values[b]);
    }
    if (!tree.rgba.empty()) {
        for (std::size_t c = 0; c < 4; ++c) {
            std::swap(tree.rgba[4 * std::size_t{a} + c], tree.rgba[4 * std::size_t{b} + c]);
        }
    }
}

/// Everything a slot carries, moved to the order `order` names: slot `i`
/// becomes what slot `order[i]` was.
void reorder(PointTree& tree, const std::vector<std::uint32_t>& order)
{
    const std::size_t n = order.size();
    const auto moved = [&](auto& values, std::size_t width) {
        if (values.empty()) {
            return;
        }
        std::remove_reference_t<decltype(values)> out(values.size());
        parallelFor(piecesOf(n), [&](long long piece) {
            const std::size_t from = static_cast<std::size_t>(piece) * kPiece;
            const std::size_t to = std::min(n, from + kPiece);
            for (std::size_t i = from; i < to; ++i) {
                for (std::size_t c = 0; c < width; ++c) {
                    out[width * i + c] = values[width * order[i] + c];
                }
            }
        });
        values.swap(out);
    };
    moved(tree.x, 1);
    moved(tree.y, 1);
    moved(tree.element, 1);
    moved(tree.values, 1);
    moved(tree.rgba, 4);
}

/// Splits nodes into the vector it is given, and closes them.
struct Builder
{
    PointTree& tree;
    /// The sorted codes, a slot each, for the quantised levels.
    const std::vector<std::uint32_t>& codes;

    [[nodiscard]] double px(std::uint32_t slot) const
    {
        return pointPosition(tree.x[slot], tree.xLog);
    }
    [[nodiscard]] double py(std::uint32_t slot) const
    {
        return pointPosition(tree.y[slot], tree.yLog);
    }

    /// Split `nodes[index]`, at `depth`, one level: into the children its codes
    /// have, or into the quadrants of its own box once the codes run out. A
    /// level that divides nothing is a level of nothing, and the next one down
    /// is tried instead. The depth its children are at, or -1 for a leaf.
    int splitOnce(std::vector<PointNode>& nodes, std::uint32_t index, int depth)
    {
        const PointNode node = nodes[index];
        while (node.count > static_cast<std::uint32_t>(kPointLeaf) && depth < kMaxDepth) {
            std::array<std::uint32_t, 5> edges{};
            if (depth < kQuantumBits) {
                // The two bits of this level, x in the low one: the children are
                // the runs of codes that agree on them, in order.
                const int shift = 2 * (kQuantumBits - 1 - depth);
                const std::uint64_t above = ~((std::uint64_t{1} << (shift + 2)) - 1);
                const auto prefix = static_cast<std::uint32_t>(codes[node.first] & above);
                const std::uint32_t end = node.first + node.count;
                edges[0] = node.first;
                edges[4] = end;
                for (std::uint32_t q = 1; q < 4; ++q) {
                    const std::uint32_t at = prefix | (q << shift);
                    if (node.count <= 32) {
                        std::uint32_t s = edges[q - 1];
                        while (s < end && codes[s] < at) {
                            ++s;
                        }
                        edges[q] = s;
                    }
                    else {
                        edges[q] = static_cast<std::uint32_t>(
                            std::lower_bound(codes.begin() + edges[q - 1], codes.begin() + end,
                                             at) -
                            codes.begin());
                    }
                }
            }
            else if (!refine(node, edges)) {
                return -1;
            }
            std::uint32_t children = 0;
            for (std::size_t q = 0; q < 4; ++q) {
                children += edges[q + 1] > edges[q] ? 1 : 0;
            }
            if (children < 2) {
                ++depth;
                continue;
            }
            const auto first = static_cast<std::uint32_t>(nodes.size());
            if (first > 0x1fffffffu) {
                throw std::length_error("plotcore: a scatter of more nodes than a tree counts");
            }
            for (std::size_t q = 0; q < 4; ++q) {
                if (edges[q + 1] > edges[q]) {
                    PointNode child;
                    child.first = edges[q];
                    child.count = edges[q + 1] - edges[q];
                    nodes.push_back(child);
                }
            }
            nodes[index].family = first | (children << 29);
            return depth + 1;
        }
        return -1;
    }

    /// `nodes[index]` and everything under it.
    void split(std::vector<PointNode>& nodes, std::uint32_t index, int depth)
    {
        const int below = splitOnce(nodes, index, depth);
        if (below < 0) {
            return;
        }
        const PointNode node = nodes[index];
        for (std::uint32_t c = node.child(); c < node.child() + node.children(); ++c) {
            split(nodes, c, below);
        }
    }

    /// A node past the quantised levels, cut at the middle of its points' box
    /// into the quadrants its points fall in, in place: `edges` are where each
    /// quadrant starts. False when the points stand at one place.
    bool refine(const PointNode& node, std::array<std::uint32_t, 5>& edges)
    {
        double xl = std::numeric_limits<double>::infinity();
        double xh = -xl;
        double yl = xl;
        double yh = -xl;
        for (std::uint32_t s = node.first; s < node.first + node.count; ++s) {
            xl = std::min(xl, px(s));
            xh = std::max(xh, px(s));
            yl = std::min(yl, py(s));
            yh = std::max(yh, py(s));
        }
        if (!(xh > xl) && !(yh > yl)) {
            return false;
        }
        const double mx = xl + (xh - xl) / 2.0;
        const double my = yl + (yh - yl) / 2.0;
        const auto part = [&](std::uint32_t from, std::uint32_t to, auto&& high) {
            std::uint32_t low = from;
            std::uint32_t top = to;
            while (low < top) {
                if (high(low)) {
                    swapSlots(tree, low, --top);
                }
                else {
                    ++low;
                }
            }
            return low;
        };
        const std::uint32_t end = node.first + node.count;
        const std::uint32_t ySplit =
            part(node.first, end, [&](std::uint32_t s) { return yh > yl && py(s) > my; });
        edges[0] = node.first;
        edges[1] = part(node.first, ySplit, [&](std::uint32_t s) { return xh > xl && px(s) > mx; });
        edges[2] = ySplit;
        edges[3] = part(ySplit, end, [&](std::uint32_t s) { return xh > xl && px(s) > mx; });
        edges[4] = end;
        return true;
    }

    /// The box and the last point of `nodes[k]`, its children's being known.
    void close(std::vector<PointNode>& nodes, std::size_t k) const
    {
        PointNode& node = nodes[k];
        node.xLow = node.yLow = std::numeric_limits<double>::infinity();
        node.xHigh = node.yHigh = -std::numeric_limits<double>::infinity();
        node.last = kNoSlot;
        std::uint32_t lastElement = 0;
        const auto take = [&](std::uint32_t slot) {
            if (node.last == kNoSlot || tree.element[slot] > lastElement) {
                node.last = slot;
                lastElement = tree.element[slot];
            }
        };
        if (node.children() == 0) {
            for (std::uint32_t s = node.first; s < node.first + node.count; ++s) {
                const double x = px(s);
                const double y = py(s);
                node.xLow = std::min(node.xLow, x);
                node.xHigh = std::max(node.xHigh, x);
                node.yLow = std::min(node.yLow, y);
                node.yHigh = std::max(node.yHigh, y);
                take(s);
            }
            return;
        }
        for (std::uint32_t c = node.child(); c < node.child() + node.children(); ++c) {
            const PointNode& child = nodes[c];
            node.xLow = std::min(node.xLow, child.xLow);
            node.xHigh = std::max(node.xHigh, child.xHigh);
            node.yLow = std::min(node.yLow, child.yLow);
            node.yHigh = std::max(node.yHigh, child.yHigh);
            take(child.last);
        }
    }
};

/// Order the tree's points for its scales and build its nodes.
void build(PointTree& tree)
{
    tree.nodes.clear();
    tree.drawable = 0;
    tree.generation = nextGeneration.fetch_add(1);
    const std::size_t n = tree.x.size();
    if (n == 0) {
        return;
    }
    const long long pieces = piecesOf(n);

    // Which points have a place, and the box of the ones that do, piece by
    // piece: the ones that have none are few, and listed rather than marked.
    struct Piece
    {
        double xl = std::numeric_limits<double>::infinity();
        double xh = -std::numeric_limits<double>::infinity();
        double yl = std::numeric_limits<double>::infinity();
        double yh = -std::numeric_limits<double>::infinity();
        std::size_t placed = 0;
        std::vector<std::uint32_t> unplaced;
    };
    std::vector<Piece> parts(static_cast<std::size_t>(pieces));
    parallelFor(pieces, [&](long long p) {
        Piece& part = parts[static_cast<std::size_t>(p)];
        const std::size_t from = static_cast<std::size_t>(p) * kPiece;
        const std::size_t to = std::min(n, from + kPiece);
        for (std::size_t i = from; i < to; ++i) {
            const double x = pointPosition(tree.x[i], tree.xLog);
            const double y = pointPosition(tree.y[i], tree.yLog);
            if (std::isfinite(x) && std::isfinite(y)) {
                ++part.placed;
                part.xl = std::min(part.xl, x);
                part.xh = std::max(part.xh, x);
                part.yl = std::min(part.yl, y);
                part.yh = std::max(part.yh, y);
            }
            else {
                part.unplaced.push_back(static_cast<std::uint32_t>(i));
            }
        }
    });
    double xl = std::numeric_limits<double>::infinity();
    double xh = -xl;
    double yl = xl;
    double yh = -xl;
    std::vector<std::size_t> offsets(parts.size() + 1, 0);
    for (std::size_t p = 0; p < parts.size(); ++p) {
        xl = std::min(xl, parts[p].xl);
        xh = std::max(xh, parts[p].xh);
        yl = std::min(yl, parts[p].yl);
        yh = std::max(yh, parts[p].yh);
        offsets[p + 1] = offsets[p] + parts[p].placed;
    }
    const std::size_t placed = offsets.back();

    // A code a placed point, its slot beside it, sorted by code: the slots in
    // the order of the quantised quadtree. Two points that share a code keep
    // the order they were in, which is the order they were handed over in.
    const double quanta = static_cast<double>(1u << kQuantumBits);
    const double xScale = xh > xl ? quanta / (xh - xl) : 0.0;
    const double yScale = yh > yl ? quanta / (yh - yl) : 0.0;
    const auto quantum = [&](double position, double low, double scale) {
        const double q = (position - low) * scale;
        return static_cast<std::uint32_t>(std::clamp(q, 0.0, quanta - 1.0));
    };
    std::vector<std::uint64_t> keys(placed);
    parallelFor(pieces, [&](long long p) {
        std::size_t at = offsets[static_cast<std::size_t>(p)];
        const std::size_t from = static_cast<std::size_t>(p) * kPiece;
        const std::size_t to = std::min(n, from + kPiece);
        for (std::size_t i = from; i < to; ++i) {
            const double x = pointPosition(tree.x[i], tree.xLog);
            const double y = pointPosition(tree.y[i], tree.yLog);
            if (std::isfinite(x) && std::isfinite(y)) {
                const std::uint32_t code =
                    spread(quantum(x, xl, xScale)) | (spread(quantum(y, yl, yScale)) << 1);
                keys[at++] = (std::uint64_t{code} << 32) | i;
            }
        }
    });
    {
        std::vector<std::uint64_t> spare;
        sortHigh(keys, spare);
    }
    tree.drawable = static_cast<std::uint32_t>(placed);

    std::vector<std::uint32_t> order(n);
    std::vector<std::uint32_t> codes(placed);
    parallelFor(piecesOf(placed), [&](long long p) {
        const std::size_t from = static_cast<std::size_t>(p) * kPiece;
        const std::size_t to = std::min(placed, from + kPiece);
        for (std::size_t i = from; i < to; ++i) {
            order[i] = static_cast<std::uint32_t>(keys[i] & 0xffffffffu);
            codes[i] = static_cast<std::uint32_t>(keys[i] >> 32);
        }
    });
    keys = {};
    std::size_t at = placed;
    for (const Piece& part : parts) {
        for (const std::uint32_t i : part.unplaced) {
            order[at++] = i;
        }
    }
    parts = {};
    reorder(tree, order);
    order = {};
    if (placed == 0) {
        return;
    }

    // The top of the tree on this thread, level by level, until there are
    // enough subtrees to share out; then each subtree split and closed on its
    // own, into nodes of its own, and spliced in after the top.
    Builder builder{tree, codes};
    std::vector<PointNode>& nodes = tree.nodes;
    PointNode root;
    root.first = 0;
    root.count = tree.drawable;
    nodes.push_back(root);
    struct Open
    {
        std::uint32_t index;
        int depth;
    };
    std::vector<Open> frontier{{0, 0}};
    const auto wanted = static_cast<std::size_t>(8 * poolThreads());
    for (int level = 0; level < 8 && frontier.size() < wanted; ++level) {
        std::vector<Open> next;
        bool split = false;
        for (const Open& open : frontier) {
            const int below = builder.splitOnce(nodes, open.index, open.depth);
            if (below < 0) {
                next.push_back(open); // a leaf already: a subtree of one
                continue;
            }
            split = true;
            const PointNode node = nodes[open.index];
            for (std::uint32_t c = node.child(); c < node.child() + node.children(); ++c) {
                next.push_back({c, below});
            }
        }
        frontier.swap(next);
        if (!split) {
            break;
        }
    }
    const std::size_t top = nodes.size();
    std::vector<std::vector<PointNode>> subtrees(frontier.size());
    parallelFor(static_cast<long long>(frontier.size()), [&](long long k) {
        const Open& open = frontier[static_cast<std::size_t>(k)];
        std::vector<PointNode>& own = subtrees[static_cast<std::size_t>(k)];
        own.push_back(nodes[open.index]);
        builder.split(own, 0, open.depth);
        for (std::size_t j = own.size(); j-- > 0;) {
            builder.close(own, j);
        }
    });
    for (std::size_t k = 0; k < frontier.size(); ++k) {
        std::vector<PointNode>& own = subtrees[k];
        // The subtree's first node is the frontier node itself, and the rest
        // go after everything so far: a child index moves by that, less one.
        const auto shift = static_cast<std::uint32_t>(nodes.size()) - 1;
        const auto moved = [&](PointNode node) {
            if (node.children() > 0) {
                const std::uint64_t at = std::uint64_t{node.child()} + shift;
                if (at > 0x1fffffffu) {
                    throw std::length_error("plotcore: a scatter of more nodes than a tree counts");
                }
                node.family = static_cast<std::uint32_t>(at) | (node.children() << 29);
            }
            return node;
        };
        nodes[frontier[k].index] = moved(own[0]);
        for (std::size_t j = 1; j < own.size(); ++j) {
            nodes.push_back(moved(own[j]));
        }
        own = {};
    }
    // The top, bottom up; its frontier is closed already, with its subtrees.
    std::vector<unsigned char> closed(top, 0);
    for (const Open& open : frontier) {
        closed[open.index] = 1;
    }
    for (std::size_t k = top; k-- > 0;) {
        if (closed[k] == 0) {
            builder.close(nodes, k);
        }
    }
    nodes.shrink_to_fit();
}

} // namespace

long long PointTree::bytes() const
{
    return static_cast<long long>(x.size() * sizeof(double) + y.size() * sizeof(double) +
                                  element.size() * sizeof(std::uint32_t) +
                                  values.size() * sizeof(double) + rgba.size() +
                                  nodes.size() * sizeof(PointNode));
}

PointTree pointTreeOf(const double* x, const double* y, const double* values,
                      const std::uint8_t* rgba, long long count, bool xLog, bool yLog)
{
    if (count > kMaxScatterPoints) {
        throw std::length_error("plotcore: a scatter holds at most 2^31 - 2 points");
    }
    PointTree tree;
    tree.xLog = xLog;
    tree.yLog = yLog;
    const auto n = static_cast<std::size_t>(std::max<long long>(count, 0));
    if (n > 0 && (x == nullptr || y == nullptr)) {
        throw std::invalid_argument("plotcore: points need an x and a y each");
    }
    tree.x.assign(x, x + n);
    tree.y.assign(y, y + n);
    tree.element.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        tree.element[i] = static_cast<std::uint32_t>(i);
    }
    if (values != nullptr) {
        tree.values.assign(values, values + n);
    }
    if (rgba != nullptr) {
        tree.rgba.assign(rgba, rgba + 4 * n);
    }
    build(tree);
    return tree;
}

void rebuildPointTree(PointTree& tree, bool xLog, bool yLog)
{
    if (tree.xLog == xLog && tree.yLog == yLog && tree.generation != 0) {
        return;
    }
    tree.xLog = xLog;
    tree.yLog = yLog;
    build(tree);
}

void setTreeValues(PointTree& tree, std::span<const double> values)
{
    if (values.empty()) {
        tree.values.clear();
        return;
    }
    if (values.size() != tree.x.size()) {
        throw std::invalid_argument("plotcore: a value a point");
    }
    tree.values.resize(values.size());
    for (std::size_t s = 0; s < values.size(); ++s) {
        tree.values[s] = values[tree.element[s]];
    }
}

void setTreeColours(PointTree& tree, std::span<const std::uint8_t> rgba)
{
    if (rgba.empty()) {
        tree.rgba.clear();
        return;
    }
    if (rgba.size() != 4 * tree.x.size()) {
        throw std::invalid_argument("plotcore: four bytes of colour a point");
    }
    tree.rgba.resize(rgba.size());
    for (std::size_t s = 0; s < tree.x.size(); ++s) {
        for (std::size_t c = 0; c < 4; ++c) {
            tree.rgba[4 * s + c] = rgba[4 * std::size_t{tree.element[s]} + c];
        }
    }
}

PointSelection selectionOn(const PointTree& tree, std::span<const std::uint64_t> elements)
{
    PointSelection selection;
    selection.generation = tree.generation;
    const std::size_t n = tree.x.size();
    selection.slots.assign((n + 63) / 64, 0);
    selection.last.assign(tree.nodes.size(), kNoSlot);
    const auto chosen = [&](std::uint32_t element) {
        const std::size_t word = element >> 6;
        return word < elements.size() && ((elements[word] >> (element & 63)) & 1u) != 0;
    };
    for (std::size_t s = 0; s < n; ++s) {
        if (chosen(tree.element[s])) {
            selection.slots[s >> 6] |= std::uint64_t{1} << (s & 63);
            ++selection.count;
        }
    }
    if (selection.count == 0) {
        return selection;
    }
    // Children are always after their parents, so backwards is bottom up.
    for (std::size_t k = tree.nodes.size(); k-- > 0;) {
        const PointNode& node = tree.nodes[k];
        std::uint32_t best = kNoSlot;
        const auto take = [&](std::uint32_t slot) {
            if (slot != kNoSlot && (best == kNoSlot || tree.element[slot] > tree.element[best])) {
                best = slot;
            }
        };
        if (node.children() == 0) {
            for (std::uint32_t s = node.first; s < node.first + node.count; ++s) {
                if (selection.has(s)) {
                    take(s);
                }
            }
        }
        else {
            for (std::uint32_t c = node.child(); c < node.child() + node.children(); ++c) {
                take(selection.last[c]);
            }
        }
        selection.last[k] = best;
    }
    return selection;
}

void drawnPoints(const PointTree& tree, const PointSelection* selection, const PointFrame& frame,
                 PointScratch& scratch, std::vector<std::uint32_t>& out, PointWalk* walk,
                 PointMemory* memory, std::uint64_t chosenKey)
{
    out.clear();
    if (walk != nullptr) {
        *walk = {};
    }
    if (tree.empty() || !(frame.cellX > 0.0) || !(frame.cellY > 0.0) ||
        !std::isfinite(frame.cellX) || !std::isfinite(frame.cellY) ||
        !(frame.xHigh >= frame.xLow) || !(frame.yHigh >= frame.yLow)) {
        if (memory != nullptr) {
            memory->valid = false;
        }
        return;
    }
    const PointSelection* selected =
        selection != nullptr && selection->count > 0 && selection->generation == tree.generation
            ? selection
            : nullptr;

    // Which cell a position is in: `floor(position / cell)`, worked out the
    // same way in every frame of one zoom, so a cell is the same run of
    // positions however the window has moved.
    const double perX = 1.0 / frame.cellX;
    const double perY = 1.0 / frame.cellY;
    const double xCells = (frame.xHigh - frame.xLow) * perX;
    const double yCells = (frame.yHigh - frame.yLow) * perY;
    // A window is a pane's worth of cells; anything else is not a frame.
    if (!((xCells + 3.0) * (yCells + 3.0) <= 4.0e7) || !std::isfinite(frame.xLow * perX) ||
        !std::isfinite(frame.yLow * perY) || std::abs(frame.xHigh * perX) > 4.0e18 ||
        std::abs(frame.yHigh * perY) > 4.0e18) {
        if (memory != nullptr) {
            memory->valid = false;
        }
        return;
    }
    // The cells the window covers, and one more on every side: a node that
    // fits a cell and crosses the window's edge has its last point within a
    // cell of it, and that point is taken wherever it stands.
    const long long x0 = floorOf(frame.xLow * perX) - 1;
    const long long y0 = floorOf(frame.yLow * perY) - 1;
    const long long nx = floorOf(frame.xHigh * perX) - x0 + 2;
    const long long ny = floorOf(frame.yHigh * perY) - y0 + 2;
    const auto cellCount = static_cast<std::size_t>(nx * ny);
    if (scratch.cells.size() < cellCount) {
        scratch.cells.assign(cellCount, 0);
    }
    std::uint64_t* cells = scratch.cells.data();
    // A point as one word, ordered as it is drawn: whether it is selected,
    // then its element; and its slot, plus one so no point is the empty cell.
    const auto wordOf = [&](std::uint32_t slot) {
        const std::uint64_t chosen = selected != nullptr && selected->has(slot) ? 1u : 0u;
        return (chosen << 63) | (std::uint64_t{tree.element[slot]} << 32) | (slot + 1u);
    };

    // Runs of cells, counted from position zero: `[x0, x1)` by `[y0, y1)`.
    struct Cells
    {
        long long x0 = 0;
        long long x1 = 0;
        long long y0 = 0;
        long long y1 = 0;
        [[nodiscard]] bool empty() const { return x1 <= x0 || y1 <= y0; }
    };
    const Cells buffer{x0, x0 + nx, y0, y0 + ny};
    // Wholly inside the window: the buffer less its ring, and less the
    // window's first and last cells, which it may only cross.
    const Cells whole{x0 + 2, x0 + nx - 2, y0 + 2, y0 + ny - 2};

    // What the last frame knew whole of the cells this one knows whole, taken
    // as it is.
    Cells kept;
    if (memory != nullptr && memory->valid && memory->cellX == frame.cellX &&
        memory->cellY == frame.cellY && memory->generation == tree.generation &&
        memory->chosen == chosenKey) {
        kept = Cells{std::max(whole.x0, memory->x0), std::min(whole.x1, memory->x1),
                     std::max(whole.y0, memory->y0), std::min(whole.y1, memory->y1)};
        if (!kept.empty()) {
            const long long width = memory->x1 - memory->x0;
            for (long long y = kept.y0; y < kept.y1; ++y) {
                const std::uint64_t* from =
                    memory->words.data() + (y - memory->y0) * width + (kept.x0 - memory->x0);
                std::copy(from, from + (kept.x1 - kept.x0), cells + (y - y0) * nx + (kept.x0 - x0));
            }
        }
    }

    // The rest of the buffer, walked: whole when nothing was kept, or the
    // strips around what was -- above it, below it, and either side of it.
    std::vector<Cells> regions;
    if (kept.empty()) {
        regions.push_back(buffer);
    }
    else {
        regions.push_back(Cells{buffer.x0, buffer.x1, buffer.y0, kept.y0});
        regions.push_back(Cells{buffer.x0, buffer.x1, kept.y1, buffer.y1});
        regions.push_back(Cells{buffer.x0, kept.x0, kept.y0, kept.y1});
        regions.push_back(Cells{kept.x1, buffer.x1, kept.y0, kept.y1});
    }

    PointWalk counted;
    for (const Cells& region : regions) {
        if (region.empty()) {
            continue;
        }
        // What can land in the region meets the window within a cell of it:
        // a node that stops fits a cell, and holds its last point.
        const double xLow = std::max(frame.xLow, static_cast<double>(region.x0 - 1) * frame.cellX);
        const double xHigh =
            std::min(frame.xHigh, static_cast<double>(region.x1 + 1) * frame.cellX);
        const long long rows = region.y1 - region.y0;
        // In bands of rows of cells, a band to a piece of the pool. A band owns
        // its cells, so no two pieces ever land in one; each walks the tree as
        // far as it meets the band, and the top few levels are walked by every
        // band, which is nothing beside the rest. How many there are is the
        // pane's to say and not the machine's, so what a frame visits is the
        // same count everywhere.
        const long long bands = std::clamp<long long>(rows / 32, 1, 64);
        std::vector<PointWalk> walks(static_cast<std::size_t>(bands));
        parallelFor(bands, [&](long long b) {
            PointWalk& part = walks[static_cast<std::size_t>(b)];
            const long long r0 = region.y0 + rows * b / bands;
            const long long r1 = region.y0 + rows * (b + 1) / bands;
            const double yLow = std::max(frame.yLow, static_cast<double>(r0 - 1) * frame.cellY);
            const double yHigh = std::min(frame.yHigh, static_cast<double>(r1 + 1) * frame.cellY);
            const auto land = [&](std::uint32_t slot, double x, double y) {
                ++part.candidates;
                const long long row = floorOf(y * perY);
                const long long column = floorOf(x * perX);
                if (row < r0 || row >= r1 || column < region.x0 || column >= region.x1) {
                    return;
                }
                std::uint64_t& held = cells[(row - y0) * nx + (column - x0)];
                held = std::max(held, wordOf(slot));
            };
            const auto landSlot = [&](std::uint32_t slot) {
                land(slot, pointPosition(tree.x[slot], tree.xLog),
                     pointPosition(tree.y[slot], tree.yLog));
            };
            std::vector<std::uint32_t> stack{0};
            while (!stack.empty()) {
                const std::uint32_t index = stack.back();
                stack.pop_back();
                const PointNode& node = tree.nodes[index];
                ++part.nodes;
                if (node.xHigh < xLow || node.xLow > xHigh || node.yHigh < yLow ||
                    node.yLow > yHigh) {
                    continue;
                }
                if (node.xHigh - node.xLow <= frame.cellX &&
                    node.yHigh - node.yLow <= frame.cellY) {
                    landSlot(node.last);
                    if (selected != nullptr && selected->last[index] != kNoSlot &&
                        selected->last[index] != node.last) {
                        landSlot(selected->last[index]);
                    }
                    continue;
                }
                if (node.children() == 0) {
                    for (std::uint32_t s = node.first; s < node.first + node.count; ++s) {
                        ++part.points;
                        const double x = pointPosition(tree.x[s], tree.xLog);
                        const double y = pointPosition(tree.y[s], tree.yLog);
                        if (x >= frame.xLow && x <= frame.xHigh && y >= frame.yLow &&
                            y <= frame.yHigh) {
                            land(s, x, y);
                        }
                    }
                    continue;
                }
                for (std::uint32_t c = node.child() + node.children(); c-- > node.child();) {
                    stack.push_back(c);
                }
            }
        });
        for (const PointWalk& part : walks) {
            counted.nodes += part.nodes;
            counted.points += part.points;
            counted.candidates += part.candidates;
        }
    }

    // What every cell kept, emptied for the next frame as it goes, and what
    // this frame knows whole left for the next: an element and a slot, which
    // are one key, an element being unique.
    const bool remember = memory != nullptr && !whole.empty();
    if (remember) {
        memory->words.assign(
            static_cast<std::size_t>((whole.x1 - whole.x0) * (whole.y1 - whole.y0)), 0);
    }
    const long long bands = std::clamp<long long>(ny / 32, 1, 64);
    struct Kept
    {
        std::vector<std::uint64_t> plain;
        std::vector<std::uint64_t> chosen;
    };
    std::vector<Kept> parts(static_cast<std::size_t>(bands));
    parallelFor(bands, [&](long long b) {
        Kept& part = parts[static_cast<std::size_t>(b)];
        for (long long row = ny * b / bands; row < ny * (b + 1) / bands; ++row) {
            const long long y = y0 + row;
            const bool inWhole = remember && y >= whole.y0 && y < whole.y1;
            for (long long column = 0; column < nx; ++column) {
                std::uint64_t& cell = cells[row * nx + column];
                const std::uint64_t word = cell;
                if (word == 0) {
                    continue;
                }
                cell = 0;
                const long long x = x0 + column;
                if (inWhole && x >= whole.x0 && x < whole.x1) {
                    memory->words[static_cast<std::size_t>((y - whole.y0) * (whole.x1 - whole.x0) +
                                                           (x - whole.x0))] = word;
                }
                const std::uint64_t key = (word & ~(std::uint64_t{1} << 63)) - 1;
                ((word >> 63) != 0 ? part.chosen : part.plain).push_back(key);
            }
        }
    });
    if (memory != nullptr) {
        memory->valid = remember;
        memory->cellX = frame.cellX;
        memory->cellY = frame.cellY;
        memory->generation = tree.generation;
        memory->chosen = chosenKey;
        memory->x0 = whole.x0;
        memory->x1 = whole.x1;
        memory->y0 = whole.y0;
        memory->y1 = whole.y1;
    }

    // In the order it is drawn: the points nobody chose, by element, then the
    // chosen ones, by element.
    std::vector<std::uint64_t>& plain = scratch.order;
    std::vector<std::uint64_t> chosen;
    plain.clear();
    for (Kept& part : parts) {
        plain.insert(plain.end(), part.plain.begin(), part.plain.end());
        chosen.insert(chosen.end(), part.chosen.begin(), part.chosen.end());
    }
    sortHigh(plain, scratch.spare);
    sortHigh(chosen, scratch.spare);
    out.reserve(plain.size() + chosen.size());
    for (const std::uint64_t key : plain) {
        out.push_back(static_cast<std::uint32_t>(key & 0xffffffffu));
    }
    for (const std::uint64_t key : chosen) {
        out.push_back(static_cast<std::uint32_t>(key & 0xffffffffu));
    }
    counted.drawn = static_cast<long long>(out.size());
    if (walk != nullptr) {
        *walk = counted;
    }
}

std::optional<PointPick> pickPoint(const PointTree& tree, const PointSelection* selection, double x,
                                   double y, double perX, double perY, double radius, double reach,
                                   const PointFrame* within)
{
    if (tree.empty() || !std::isfinite(x) || !std::isfinite(y) || !(perX > 0.0) || !(perY > 0.0)) {
        return std::nullopt;
    }
    const PointSelection* selected =
        selection != nullptr && selection->count > 0 && selection->generation == tree.generation
            ? selection
            : nullptr;
    const auto keyOf = [&](std::uint32_t slot) {
        const std::uint64_t chosen = selected != nullptr && selected->has(slot) ? 1u : 0u;
        return (chosen << 32) | tree.element[slot];
    };
    const auto squared = [&](double dx, double dy) {
        const double sx = dx * perX;
        const double sy = dy * perY;
        return sx * sx + sy * sy;
    };
    // The nearest and the farthest a node's box is from the pointer, squared,
    // in device pixels.
    const auto nearest = [&](const PointNode& node) {
        const double dx = x < node.xLow ? node.xLow - x : (x > node.xHigh ? x - node.xHigh : 0.0);
        const double dy = y < node.yLow ? node.yLow - y : (y > node.yHigh ? y - node.yHigh : 0.0);
        return squared(dx, dy);
    };
    const auto farthest = [&](const PointNode& node) {
        return squared(std::max(std::abs(x - node.xLow), std::abs(x - node.xHigh)),
                       std::max(std::abs(y - node.yLow), std::abs(y - node.yHigh)));
    };
    const auto distanceTo = [&](std::uint32_t slot) {
        return squared(pointPosition(tree.x[slot], tree.xLog) - x,
                       pointPosition(tree.y[slot], tree.yLog) - y);
    };
    // Whether a node is wholly outside the window, wholly inside it, and
    // whether a point is in it at all.
    const auto outside = [&](const PointNode& node) {
        return within != nullptr && (node.xHigh < within->xLow || node.xLow > within->xHigh ||
                                     node.yHigh < within->yLow || node.yLow > within->yHigh);
    };
    const auto wholly = [&](const PointNode& node) {
        return within == nullptr || (node.xLow >= within->xLow && node.xHigh <= within->xHigh &&
                                     node.yLow >= within->yLow && node.yHigh <= within->yHigh);
    };
    const auto placed = [&](std::uint32_t slot) {
        if (within == nullptr) {
            return true;
        }
        const double px = pointPosition(tree.x[slot], tree.xLog);
        const double py = pointPosition(tree.y[slot], tree.yLog);
        return px >= within->xLow && px <= within->xHigh && py >= within->yLow &&
               py <= within->yHigh;
    };

    // The last dot that covers the pointer. A node wholly inside the dot's
    // reach answers with its last point; it is the last of everything under it.
    const double covered = radius * radius;
    std::uint32_t top = kNoSlot;
    const auto offer = [&](std::uint32_t slot) {
        if (slot != kNoSlot && (top == kNoSlot || keyOf(slot) > keyOf(top))) {
            top = slot;
        }
    };
    std::vector<std::uint32_t> stack{0};
    while (radius > 0.0 && !stack.empty()) {
        const std::uint32_t index = stack.back();
        stack.pop_back();
        const PointNode& node = tree.nodes[index];
        if (nearest(node) > covered || outside(node)) {
            continue;
        }
        if (farthest(node) <= covered && wholly(node)) {
            offer(node.last);
            if (selected != nullptr) {
                offer(selected->last[index]);
            }
            continue;
        }
        if (node.children() == 0) {
            for (std::uint32_t s = node.first; s < node.first + node.count; ++s) {
                if (distanceTo(s) <= covered && placed(s)) {
                    offer(s);
                }
            }
            continue;
        }
        for (std::uint32_t c = node.child(); c < node.child() + node.children(); ++c) {
            stack.push_back(c);
        }
    }
    if (top != kNoSlot) {
        return PointPick{top, std::sqrt(distanceTo(top)), true};
    }

    // ...or the nearest within reach, the nearer children first so the bound
    // closes quickly. Of two as near, the later.
    double best = reach * reach;
    std::uint32_t found = kNoSlot;
    stack.assign(1, 0);
    while (!stack.empty()) {
        const std::uint32_t index = stack.back();
        stack.pop_back();
        const PointNode& node = tree.nodes[index];
        if (nearest(node) > best || outside(node)) {
            continue;
        }
        if (node.children() == 0) {
            for (std::uint32_t s = node.first; s < node.first + node.count; ++s) {
                if (!placed(s)) {
                    continue;
                }
                const double d = distanceTo(s);
                if (d < best || (d == best && (found == kNoSlot || keyOf(s) > keyOf(found)))) {
                    best = d;
                    found = s;
                }
            }
            continue;
        }
        std::array<std::pair<double, std::uint32_t>, 4> order{};
        const std::uint32_t children = std::min<std::uint32_t>(node.children(), 4);
        for (std::uint32_t c = 0; c < children; ++c) {
            order[c] = {nearest(tree.nodes[node.child() + c]), node.child() + c};
        }
        // Farthest first, by insertion: four at most.
        for (std::uint32_t i = 1; i < children; ++i) {
            for (std::uint32_t j = i; j > 0 && order[j - 1].first < order[j].first; --j) {
                std::swap(order[j - 1], order[j]);
            }
        }
        for (std::uint32_t c = 0; c < children; ++c) {
            stack.push_back(order[c].second); // the nearest is pushed last, taken first
        }
    }
    if (found == kNoSlot) {
        return std::nullopt;
    }
    return PointPick{found, std::sqrt(best), false};
}

long long pointsInside(const PointTree& tree, double xLow, double xHigh, double yLow, double yHigh,
                       std::vector<std::uint64_t>& out)
{
    if (tree.empty()) {
        return 0;
    }
    if (xLow > xHigh) {
        std::swap(xLow, xHigh);
    }
    if (yLow > yHigh) {
        std::swap(yLow, yHigh);
    }
    const std::size_t words = (tree.x.size() + 63) / 64;
    if (out.size() < words) {
        out.resize(words, 0);
    }
    long long found = 0;
    const auto take = [&](std::uint32_t slot) {
        const std::uint32_t element = tree.element[slot];
        std::uint64_t& word = out[element >> 6];
        const std::uint64_t bit = std::uint64_t{1} << (element & 63);
        found += (word & bit) == 0 ? 1 : 0;
        word |= bit;
    };
    std::vector<std::uint32_t> stack{0};
    while (!stack.empty()) {
        const PointNode& node = tree.nodes[stack.back()];
        stack.pop_back();
        if (node.xHigh < xLow || node.xLow > xHigh || node.yHigh < yLow || node.yLow > yHigh) {
            continue;
        }
        if (node.xLow >= xLow && node.xHigh <= xHigh && node.yLow >= yLow && node.yHigh <= yHigh) {
            for (std::uint32_t s = node.first; s < node.first + node.count; ++s) {
                take(s);
            }
            continue;
        }
        if (node.children() == 0) {
            for (std::uint32_t s = node.first; s < node.first + node.count; ++s) {
                const double x = pointPosition(tree.x[s], tree.xLog);
                const double y = pointPosition(tree.y[s], tree.yLog);
                if (x >= xLow && x <= xHigh && y >= yLow && y <= yHigh) {
                    take(s);
                }
            }
            continue;
        }
        for (std::uint32_t c = node.child(); c < node.child() + node.children(); ++c) {
            stack.push_back(c);
        }
    }
    return found;
}

void paintDiscs(std::span<const Point> centres, std::span<const Rgba> colours, double radius,
                int width, int height, std::vector<std::uint8_t>& out)
{
    const auto w = static_cast<std::size_t>(std::max(width, 0));
    const auto h = static_cast<std::size_t>(std::max(height, 0));
    out.assign(4 * w * h, 0);
    if (w == 0 || h == 0 || centres.empty() || colours.empty()) {
        return;
    }
    const double r = std::max(radius, 0.5);
    const double reach = r + 0.5;

    // A disc's coverage worked out once for each sixteenth of a pixel its
    // centre can stand at, across and down, with each row's run of covered
    // pixels: a disc is then the stamp its centre asks for, copied. Worked out
    // a pixel at a time, with a root for every pixel on an edge and a span for
    // every row, a disc of two pixels cost two and a half times as much. Where
    // the centre is rounded to, a thirty-second of a pixel at most, moves the
    // edge by that and nothing else. A large dot is stamped from four steps a
    // pixel, so the stamps stay a few megabytes.
    const int steps = r > 16.0 ? 4 : 16;
    const int around = static_cast<int>(std::ceil(reach));
    const int side = 2 * around + 2;
    const auto stampSize = static_cast<std::size_t>(side) * static_cast<std::size_t>(side);
    std::vector<float> stamps(static_cast<std::size_t>(steps * steps) * stampSize, 0.0F);
    std::vector<std::int16_t> spans(static_cast<std::size_t>(steps * steps * side) * 2, 0);
    for (int qy = 0; qy < steps; ++qy) {
        for (int qx = 0; qx < steps; ++qx) {
            const std::size_t stamp = static_cast<std::size_t>(qy * steps + qx);
            float* cover = &stamps[stamp * stampSize];
            std::int16_t* span = &spans[stamp * static_cast<std::size_t>(side) * 2];
            const double ox = (qx + 0.5) / steps;
            const double oy = (qy + 0.5) / steps;
            for (int j = 0; j < side; ++j) {
                int from = side;
                int to = -1;
                const double dy = static_cast<double>(j - around) + 0.5 - oy;
                for (int i = 0; i < side; ++i) {
                    const double dx = static_cast<double>(i - around) + 0.5 - ox;
                    const double c = std::clamp(reach - std::sqrt(dx * dx + dy * dy), 0.0, 1.0);
                    cover[j * side + i] = static_cast<float>(c);
                    if (c > 0.0) {
                        from = std::min(from, i);
                        to = std::max(to, i);
                    }
                }
                span[2 * j] = static_cast<std::int16_t>(from);
                span[2 * j + 1] = static_cast<std::int16_t>(to);
            }
        }
    }

    // Which discs touch each band of rows, in the order given.
    constexpr std::size_t kBand = 32;
    const std::size_t bands = (h + kBand - 1) / kBand;
    std::vector<std::uint32_t> start(bands + 1, 0);
    const auto rowsOf = [&](const Point& c, long long& top, long long& bottom) {
        if (!std::isfinite(c.x()) || !std::isfinite(c.y()) || c.x() + reach < 0.0 ||
            c.x() - reach >= static_cast<double>(w) || c.y() + reach < 0.0 ||
            c.y() - reach >= static_cast<double>(h)) {
            return false;
        }
        top = std::max<long long>(floorOf(c.y() - reach), 0);
        bottom = std::min<long long>(floorOf(c.y() + reach), static_cast<long long>(h) - 1);
        return true;
    };
    for (const Point& c : centres) {
        long long top = 0;
        long long bottom = 0;
        if (rowsOf(c, top, bottom)) {
            for (auto b = static_cast<std::size_t>(top) / kBand;
                 b <= static_cast<std::size_t>(bottom) / kBand; ++b) {
                ++start[b + 1];
            }
        }
    }
    for (std::size_t b = 1; b <= bands; ++b) {
        start[b] += start[b - 1];
    }
    std::vector<std::uint32_t> members(start[bands]);
    {
        std::vector<std::uint32_t> at(start.begin(), start.end() - 1);
        for (std::size_t i = 0; i < centres.size(); ++i) {
            long long top = 0;
            long long bottom = 0;
            if (rowsOf(centres[i], top, bottom)) {
                for (auto b = static_cast<std::size_t>(top) / kBand;
                     b <= static_cast<std::size_t>(bottom) / kBand; ++b) {
                    members[at[b]++] = static_cast<std::uint32_t>(i);
                }
            }
        }
    }

    // Each band painted front to back: the last disc first, each one after it
    // under what is there already. It composites to what painting them in
    // order over one another does, and a pixel that is opaque takes nothing
    // more -- so where a pane is thick with dots, a disc over pixels that are
    // all covered already is passed over whole, by the tiles of eight it
    // touches, rather than a pixel at a time. On a dense pane at a pixel ratio
    // of two that was most of the painting.
    constexpr float kOpaque = 1.0F - 1.0F / 1024.0F;
    constexpr std::size_t kTile = 8;
    const std::size_t tilesAcross = (w + kTile - 1) / kTile;
    parallelFor(static_cast<long long>(bands), [&](long long band) {
        const std::size_t rowFrom = static_cast<std::size_t>(band) * kBand;
        const std::size_t rowTo = std::min(h, rowFrom + kBand);
        const std::size_t rows = rowTo - rowFrom;
        std::vector<float> ink(4 * w * rows, 0.0F);
        const std::size_t tilesDown = (rows + kTile - 1) / kTile;
        std::vector<std::uint8_t> full(tilesAcross * tilesDown, 0);
        std::vector<std::uint8_t> opaque(tilesAcross * tilesDown, 0);
        for (std::size_t ty = 0; ty < tilesDown; ++ty) {
            for (std::size_t tx = 0; tx < tilesAcross; ++tx) {
                full[ty * tilesAcross + tx] = static_cast<std::uint8_t>(
                    std::min(kTile, w - tx * kTile) * std::min(kTile, rows - ty * kTile));
            }
        }
        for (std::uint32_t m = start[static_cast<std::size_t>(band) + 1];
             m-- > start[static_cast<std::size_t>(band)];) {
            const std::uint32_t i = members[m];
            const Point& c = centres[i];
            const Rgba& colour = colours.size() == 1 ? colours[0] : colours[i];
            const auto alpha = static_cast<float>(std::clamp(colour.alpha, 0.0, 1.0));
            const long long cx = floorOf(c.x());
            const long long cy = floorOf(c.y());
            const int qx =
                std::min(steps - 1, static_cast<int>((c.x() - static_cast<double>(cx)) * steps));
            const int qy =
                std::min(steps - 1, static_cast<int>((c.y() - static_cast<double>(cy)) * steps));
            const std::size_t stamp = static_cast<std::size_t>(qy * steps + qx);
            const float* cover = &stamps[stamp * stampSize];
            const std::int16_t* span = &spans[stamp * static_cast<std::size_t>(side) * 2];
            const long long left = cx - around;
            const long long top = cy - around;
            const long long x0 = std::max<long long>(left, 0);
            const long long x1 =
                std::min<long long>(left + side - 1, static_cast<long long>(w) - 1);
            const long long y0 = std::max<long long>(top, static_cast<long long>(rowFrom));
            const long long y1 =
                std::min<long long>(top + side - 1, static_cast<long long>(rowTo) - 1);
            if (x1 < x0 || y1 < y0) {
                continue;
            }
            bool hidden = true;
            const auto tx0 = static_cast<std::size_t>(x0) / kTile;
            const auto tx1 = static_cast<std::size_t>(x1) / kTile;
            const std::size_t ty0 = (static_cast<std::size_t>(y0) - rowFrom) / kTile;
            const std::size_t ty1 = (static_cast<std::size_t>(y1) - rowFrom) / kTile;
            for (std::size_t ty = ty0; hidden && ty <= ty1; ++ty) {
                for (std::size_t tx = tx0; hidden && tx <= tx1; ++tx) {
                    hidden = opaque[ty * tilesAcross + tx] == full[ty * tilesAcross + tx];
                }
            }
            if (hidden) {
                continue;
            }
            const auto red = static_cast<float>(colour.red);
            const auto green = static_cast<float>(colour.green);
            const auto blue = static_cast<float>(colour.blue);
            for (long long py = y0; py <= y1; ++py) {
                const auto j = static_cast<int>(py - top);
                const long long from = std::max<long long>(left + span[2 * j], x0);
                const long long to = std::min<long long>(left + span[2 * j + 1], x1);
                const std::size_t row = static_cast<std::size_t>(py) - rowFrom;
                const float* covered = cover + j * side - left;
                float* pixel = &ink[4 * (row * w + static_cast<std::size_t>(std::max(from, 0LL)))];
                for (long long px = from; px <= to; ++px, pixel += 4) {
                    if (pixel[3] >= kOpaque) {
                        continue;
                    }
                    const float a = alpha * covered[px] * (1.0F - pixel[3]);
                    pixel[0] += red * a;
                    pixel[1] += green * a;
                    pixel[2] += blue * a;
                    pixel[3] += a;
                    if (pixel[3] >= kOpaque) {
                        ++opaque[(row / kTile) * tilesAcross +
                                 static_cast<std::size_t>(px) / kTile];
                    }
                }
            }
        }
        std::uint8_t* to = out.data() + 4 * w * rowFrom;
        for (std::size_t k = 0; k < ink.size(); ++k) {
            to[k] = static_cast<std::uint8_t>(std::clamp(ink[k], 0.0F, 1.0F) * 255.0F + 0.5F);
        }
    });
}

} // namespace plotcore
