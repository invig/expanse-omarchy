#pragma once

#include <hyprutils/math/Box.hpp>

#include <cstddef>
#include <optional>
#include <set>
#include <utility>
#include <vector>

using namespace Hyprutils::Math;

// Laying windows out on the canvas, ported from Expanse on the Mac so both
// arrange alike. Canvas coordinates, y down. Rectangles follow Core Graphics:
// two that only touch don't intersect, and one inset past nothing is null.
namespace Expanse::Arrange {

    // The standard gap between windows; how far apart windows can be and
    // still count as one group (Tidy Up tidies each group where it is, and
    // keeps groups at least this far apart); how close edges come to lining
    // up for Tidy Up to line them up.
    constexpr double GAP           = 20.0;
    constexpr double CLUSTER_REACH = 240.0;
    constexpr double ALIGN_REACH   = 60.0;

    struct SGuide {
        Vector2D from, to;
    };

    struct SSnap {
        CBox                box;
        std::vector<SGuide> guides;
    };

    // Snap a dragged box (one window, or a group's bounds) to the others.
    // Edges and the standard gap snap firmly; centres snap softly (half the
    // reach, and they lose ties) so they guide without fighting. The middle of
    // the view snaps too, and a box above or below a row of windows can centre
    // on the whole row. `scale` is the camera's, so the reach is the same on
    // screen at every zoom.
    SSnap snap(const CBox& box, const std::vector<CBox>& others, double scale, const Vector2D& viewCentre);

    using IndexPairs = std::set<std::pair<size_t, size_t>>;

    // Push overlapping frames apart in reading order, only ever right or down,
    // so it always finishes. `keep`: pairs left overlapping.
    void separate(std::vector<CBox>& frames, double gap, bool onlyOverlaps = false, const IndexPairs& keep = {});

    // Pairs of frames that overlap.
    IndexPairs overlappingPairs(const std::vector<CBox>& frames);

    // A cluster number for each frame: frames within `reach` of each other,
    // directly or through a chain of neighbours, share one.
    std::vector<size_t> clusters(const std::vector<CBox>& frames, double reach);

    // Close gaps: each frame slides left (then up) until it sits `gap` from
    // its nearest neighbour that way that shares its rows (or columns), so it
    // never creates overlaps.
    void gravity(std::vector<CBox>& frames, double gap);

    // Line up edges that nearly line up (within `reach`): a frame's top with
    // its neighbour on the left, then its left edge with its neighbour above.
    // Skipped where the move would bring it closer than `gap` to another frame
    // (windows of different widths can't line up both sides).
    void align(std::vector<CBox>& frames, double reach, double gap);

    // Keep groups `reach` apart, so they stay separate groups: a group that
    // another has grown into is pushed away as a whole, right or down, in
    // reading order.
    void separateGroups(std::vector<CBox>& frames, const std::vector<std::vector<size_t>>& groups, double reach);

    // Tidy Up, on the home layout. Each group of windows is tidied where it
    // is: overlaps pushed apart, edges that nearly line up lined up, and gaps
    // closed to the standard gap. A group only moves (as a whole) when another
    // group has grown into its space.
    void tidyUp(std::vector<CBox>& frames);

    // Origins for windows of `sizes` that keep the `home` layout's gaps: each
    // window keeps its distance to its nearest window on the left (and above),
    // so shrinking windows pull the layout in.
    std::vector<Vector2D> derive(const std::vector<CBox>& home, const std::vector<Vector2D>& sizes);

    // An origin for a window of `size` that overlaps none of `others`. First
    // anywhere along an edge of `anchor`, the window it came from: right,
    // below, left, then above, lined up with the anchor's top or left edge,
    // then its bottom or right, then centred. Only when every edge is taken,
    // beside the windows nearest `target`, the closest first. Spots inside
    // `visible` win.
    std::optional<Vector2D> freeSpot(const Vector2D& size, const std::vector<CBox>& others, const std::optional<CBox>& anchor, const Vector2D& target, const CBox& visible,
                                     bool onScreenOnly);
}
