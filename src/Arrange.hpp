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

    // The standard gap between windows, and how far apart windows can be and
    // still count as one group.
    constexpr double GAP           = 20.0;
    constexpr double CLUSTER_REACH = 240.0;

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

    // Within each cluster, close gaps: each frame slides left (then up) until
    // it sits `gap` from its nearest neighbour that way that shares its rows
    // (or columns), so it never creates overlaps.
    void gravity(std::vector<CBox>& frames, const std::vector<size_t>& clusters, double gap);

    // Origins for windows of `sizes` that keep the `home` layout's gaps: each
    // window keeps its distance to its nearest window on the left (and above),
    // so shrinking windows pull the layout in.
    std::vector<Vector2D> derive(const std::vector<CBox>& home, const std::vector<Vector2D>& sizes);

    // The nearest origin for a window of `size` that overlaps none of
    // `others`: beside, below, left of or above them, closest to `target`.
    // Spots inside `visible` win.
    std::optional<Vector2D> freeSpot(const Vector2D& size, const std::vector<CBox>& others, const Vector2D& target, const CBox& visible, bool onScreenOnly);
}
