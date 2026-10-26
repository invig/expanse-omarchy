#include "Arrange.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace Expanse::Arrange {

    namespace {
        double minX(const CBox& r) {
            return r.x;
        }
        double maxX(const CBox& r) {
            return r.x + r.w;
        }
        double midX(const CBox& r) {
            return r.x + r.w / 2.0;
        }
        double minY(const CBox& r) {
            return r.y;
        }
        double maxY(const CBox& r) {
            return r.y + r.h;
        }
        double midY(const CBox& r) {
            return r.y + r.h / 2.0;
        }
        Vector2D centre(const CBox& r) {
            return {midX(r), midY(r)};
        }

        bool isNull(const CBox& r) {
            return r.w < 0 || r.h < 0;
        }

        // CGRect.insetBy: negative grows.
        CBox inset(const CBox& r, double dx, double dy) {
            return {r.x + dx, r.y + dy, r.w - 2.0 * dx, r.h - 2.0 * dy};
        }

        // CGRect.intersects: a shared edge isn't enough, a null rect never does.
        bool intersects(const CBox& a, const CBox& b) {
            if (isNull(a) || isNull(b))
                return false;
            return minX(a) < maxX(b) && minX(b) < maxX(a) && minY(a) < maxY(b) && minY(b) < maxY(a);
        }

        bool contains(const CBox& outer, const CBox& inner) {
            return minX(inner) >= minX(outer) && maxX(inner) <= maxX(outer) && minY(inner) >= minY(outer) && maxY(inner) <= maxY(outer);
        }

        CBox unite(const CBox& a, const CBox& b) {
            const double X0 = std::min(minX(a), minX(b)), Y0 = std::min(minY(a), minY(b));
            const double X1 = std::max(maxX(a), maxX(b)), Y1 = std::max(maxY(a), maxY(b));
            return {X0, Y0, X1 - X0, Y1 - Y0};
        }

        struct SCandidate {
            double              score = std::numeric_limits<double>::infinity();
            double              delta = 0.0;
            std::optional<double> line;
            std::optional<CBox> span;
            bool                found = false;
        };
    }

    SSnap snap(const CBox& f, const std::vector<CBox>& others, double scale, const Vector2D& viewCentre) {
        const double FIRM = 10.0 / scale;
        const double SOFT = FIRM / 2.0;

        SCandidate   bestX, bestY;

        const auto consider = [&](double a, double b, bool centre, std::optional<double> line, std::optional<CBox> span, SCandidate& best) {
            const double D = b - a;
            if (!(std::abs(D) < (centre ? SOFT : FIRM)))
                return;
            const double SCORE = std::abs(D) + (centre ? SOFT / 2.0 : 0.0);
            if (SCORE < best.score)
                best = {SCORE, D, line, span, true};
        };

        for (const auto& r : others) {
            for (const double A : {minX(f), maxX(f)})
                for (const double B : {minX(r), maxX(r)})
                    consider(A, B, false, B, std::nullopt, bestX);
            consider(midX(f), midX(r), true, midX(r), std::nullopt, bestX);
            consider(minX(f), maxX(r) + GAP, false, std::nullopt, std::nullopt, bestX);
            consider(maxX(f), minX(r) - GAP, false, std::nullopt, std::nullopt, bestX);
            for (const double A : {minY(f), maxY(f)})
                for (const double B : {minY(r), maxY(r)})
                    consider(A, B, false, B, std::nullopt, bestY);
            consider(midY(f), midY(r), true, midY(r), std::nullopt, bestY);
            consider(minY(f), maxY(r) + GAP, false, std::nullopt, std::nullopt, bestY);
            consider(maxY(f), minY(r) - GAP, false, std::nullopt, std::nullopt, bestY);
        }

        // The middle of the view: a window dragged near it snaps to dead centre.
        consider(midX(f), viewCentre.x, false, viewCentre.x, CBox{viewCentre.x, minY(f), 0, f.h}, bestX);
        consider(midY(f), viewCentre.y, false, viewCentre.y, CBox{minX(f), viewCentre.y, f.w, 0}, bestY);

        // Centre on a whole neighbouring row (above / below) or column (left / right).
        const double      REACH = CLUSTER_REACH;
        const CBox        WIDE = inset(f, -REACH, 0), TALL = inset(f, 0, -REACH);
        std::vector<CBox> below, above, left, right;
        for (const auto& r : others) {
            if (minY(r) >= maxY(f) - 1 && minY(r) - maxY(f) <= REACH && intersects(r, inset(WIDE, 0, -REACH)))
                below.push_back(r);
            if (maxY(r) <= minY(f) + 1 && minY(f) - maxY(r) <= REACH && intersects(r, inset(WIDE, 0, -REACH)))
                above.push_back(r);
            if (maxX(r) <= minX(f) + 1 && minX(f) - maxX(r) <= REACH && intersects(r, inset(TALL, -REACH, 0)))
                left.push_back(r);
            if (minX(r) >= maxX(f) - 1 && minX(r) - maxX(f) <= REACH && intersects(r, inset(TALL, -REACH, 0)))
                right.push_back(r);
        }
        for (const auto* row : {&below, &above}) {
            if (row->size() <= 1)
                continue;
            const CBox U = std::accumulate(row->begin() + 1, row->end(), row->front(), unite);
            consider(midX(f), midX(U), true, midX(U), U, bestX);
        }
        for (const auto* column : {&left, &right}) {
            if (column->size() <= 1)
                continue;
            const CBox U = std::accumulate(column->begin() + 1, column->end(), column->front(), unite);
            consider(midY(f), midY(U), true, midY(U), U, bestY);
        }

        CBox out = f;
        if (bestX.found)
            out.x += bestX.delta;
        if (bestY.found)
            out.y += bestY.delta;

        std::vector<SGuide> guides;
        if (bestX.found && bestX.line) {
            const double X    = *bestX.line;
            CBox         span = out;
            if (bestX.span)
                span = unite(*bestX.span, out);
            else
                for (const auto& r : others)
                    if (std::abs(minX(r) - X) < 0.5 || std::abs(midX(r) - X) < 0.5 || std::abs(maxX(r) - X) < 0.5)
                        span = unite(span, r);
            guides.push_back({{X, minY(span)}, {X, maxY(span)}});
        }
        if (bestY.found && bestY.line) {
            const double Y    = *bestY.line;
            CBox         span = out;
            if (bestY.span)
                span = unite(*bestY.span, out);
            else
                for (const auto& r : others)
                    if (std::abs(minY(r) - Y) < 0.5 || std::abs(midY(r) - Y) < 0.5 || std::abs(maxY(r) - Y) < 0.5)
                        span = unite(span, r);
            guides.push_back({{minX(span), Y}, {maxX(span), Y}});
        }

        return {out, guides};
    }

    void separate(std::vector<CBox>& frames, double gap, bool onlyOverlaps, const IndexPairs& keep) {
        std::vector<size_t> order(frames.size());
        std::iota(order.begin(), order.end(), 0);
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            return std::pair{minY(frames[a]), minX(frames[a])} < std::pair{minY(frames[b]), minX(frames[b])};
        });

        std::vector<std::pair<size_t, CBox>> settled;
        for (const size_t I : order) {
            CBox r = frames[I];
            for (int step = 0; step < 200; ++step) {
                const double REACH = onlyOverlaps ? 0.5 : -gap + 0.5;
                const CBox*  hit   = nullptr;
                for (const auto& [index, frame] : settled) {
                    if (keep.contains({std::min(index, I), std::max(index, I)}))
                        continue;
                    if (intersects(inset(frame, REACH, REACH), r)) {
                        hit = &frame;
                        break;
                    }
                }
                if (!hit)
                    break;
                const double RIGHT = maxX(*hit) + gap - minX(r);
                const double DOWN  = maxY(*hit) + gap - minY(r);
                if (RIGHT <= DOWN)
                    r.x += RIGHT;
                else
                    r.y += DOWN;
            }
            frames[I] = r;
            settled.emplace_back(I, r);
        }
    }

    IndexPairs overlappingPairs(const std::vector<CBox>& frames) {
        IndexPairs pairs;
        for (size_t i = 0; i < frames.size(); ++i)
            for (size_t j = i + 1; j < frames.size(); ++j)
                if (intersects(inset(frames[i], 0.5, 0.5), frames[j]))
                    pairs.insert({i, j});
        return pairs;
    }

    std::vector<size_t> clusters(const std::vector<CBox>& frames, double reach) {
        std::vector<size_t> parent(frames.size());
        std::iota(parent.begin(), parent.end(), 0);
        const auto root = [&](size_t i) {
            while (parent[i] != i)
                i = parent[i];
            return i;
        };
        for (size_t i = 0; i < frames.size(); ++i)
            for (size_t j = i + 1; j < frames.size(); ++j)
                if (intersects(inset(frames[i], -reach / 2.0, -reach / 2.0), inset(frames[j], -reach / 2.0, -reach / 2.0)))
                    parent[root(j)] = root(i);

        std::vector<size_t> out(frames.size());
        for (size_t i = 0; i < frames.size(); ++i)
            out[i] = root(i);
        return out;
    }

    void gravity(std::vector<CBox>& frames, const std::vector<size_t>& clusters, double gap) {
        const auto OVERLAPY = [](const CBox& a, const CBox& b) { return std::min(maxY(a), maxY(b)) - std::max(minY(a), minY(b)) > 0; };
        const auto OVERLAPX = [](const CBox& a, const CBox& b) { return std::min(maxX(a), maxX(b)) - std::max(minX(a), minX(b)) > 0; };

        std::vector<size_t> order(frames.size());
        for (int pass = 0; pass < 3; ++pass) {
            std::iota(order.begin(), order.end(), 0);
            std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return minX(frames[a]) < minX(frames[b]); });
            for (const size_t I : order) {
                const CBox            R = frames[I];
                std::optional<double> nearest;
                for (size_t j = 0; j < frames.size(); ++j)
                    if (j != I && clusters[j] == clusters[I] && maxX(frames[j]) <= minX(R) + 0.5 && OVERLAPY(frames[j], R))
                        nearest = std::max(nearest.value_or(-std::numeric_limits<double>::infinity()), maxX(frames[j]));
                if (nearest && minX(R) - *nearest > gap)
                    frames[I].x = *nearest + gap;
            }

            std::iota(order.begin(), order.end(), 0);
            std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return minY(frames[a]) < minY(frames[b]); });
            for (const size_t I : order) {
                const CBox            R = frames[I];
                std::optional<double> nearest;
                for (size_t j = 0; j < frames.size(); ++j)
                    if (j != I && clusters[j] == clusters[I] && maxY(frames[j]) <= minY(R) + 0.5 && OVERLAPX(frames[j], R))
                        nearest = std::max(nearest.value_or(-std::numeric_limits<double>::infinity()), maxY(frames[j]));
                if (nearest && minY(R) - *nearest > gap)
                    frames[I].y = *nearest + gap;
            }
        }
    }

    std::vector<Vector2D> derive(const std::vector<CBox>& home, const std::vector<Vector2D>& sizes) {
        // Distance between ranges; 0 when they overlap.
        const auto SPAN = [](double a0, double a1, double b0, double b1) { return std::max(0.0, std::max(a0, b0) - std::min(a1, b1)); };

        std::vector<double> x(home.size()), y(home.size());
        for (size_t i = 0; i < home.size(); ++i) {
            x[i] = minX(home[i]);
            y[i] = minY(home[i]);
        }

        std::vector<size_t> order(home.size());
        std::iota(order.begin(), order.end(), 0);
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return minX(home[a]) < minX(home[b]); });
        for (const size_t I : order) {
            // Nearest: the horizontal gap, with rows that don't line up counting extra.
            std::optional<size_t> best;
            double                bestCost = 0;
            for (size_t j = 0; j < home.size(); ++j) {
                if (j == I || !(maxX(home[j]) <= minX(home[I]) + 0.5))
                    continue;
                const double COST = (minX(home[I]) - maxX(home[j])) + 2 * SPAN(minY(home[j]), maxY(home[j]), minY(home[I]), maxY(home[I]));
                if (!best || COST < bestCost) {
                    best     = j;
                    bestCost = COST;
                }
            }
            if (best)
                x[I] = x[*best] + sizes[*best].x + (minX(home[I]) - maxX(home[*best]));
        }

        std::iota(order.begin(), order.end(), 0);
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return minY(home[a]) < minY(home[b]); });
        for (const size_t I : order) {
            std::optional<size_t> best;
            double                bestCost = 0;
            for (size_t j = 0; j < home.size(); ++j) {
                if (j == I || !(maxY(home[j]) <= minY(home[I]) + 0.5))
                    continue;
                const double COST = (minY(home[I]) - maxY(home[j])) + 2 * SPAN(minX(home[j]), maxX(home[j]), minX(home[I]), maxX(home[I]));
                if (!best || COST < bestCost) {
                    best     = j;
                    bestCost = COST;
                }
            }
            if (best)
                y[I] = y[*best] + sizes[*best].y + (minY(home[I]) - maxY(home[*best]));
        }

        std::vector<Vector2D> out(home.size());
        for (size_t i = 0; i < home.size(); ++i)
            out[i] = {x[i], y[i]};
        return out;
    }

    std::optional<Vector2D> freeSpot(const Vector2D& size, const std::vector<CBox>& others, const Vector2D& target, const CBox& visible, bool onScreenOnly) {
        if (others.empty())
            return std::nullopt;

        // Neighbours of the windows nearest the target; nudged by direction so ties go right, then down.
        std::vector<size_t> nearest(others.size());
        std::iota(nearest.begin(), nearest.end(), 0);
        std::stable_sort(nearest.begin(), nearest.end(), [&](size_t a, size_t b) { return centre(others[a]).distance(target) < centre(others[b]).distance(target); });
        nearest.resize(std::min<size_t>(nearest.size(), 12));

        std::vector<std::pair<Vector2D, double>> candidates;
        for (const size_t I : nearest) {
            const CBox& O = others[I];
            candidates.push_back({{maxX(O) + GAP, minY(O)}, 0});
            candidates.push_back({{minX(O), maxY(O) + GAP}, 40});
            candidates.push_back({{minX(O) - GAP - size.x, minY(O)}, 80});
            candidates.push_back({{minX(O), minY(O) - GAP - size.y}, 120});
        }

        std::optional<std::pair<Vector2D, double>> best;
        for (const auto& [origin, bias] : candidates) {
            const CBox R{origin.x, origin.y, size.x, size.y};
            if (std::ranges::any_of(others, [&](const CBox& o) { return intersects(inset(o, -GAP / 2.0, -GAP / 2.0), R); }))
                continue;
            const bool SHOWN = contains(visible, R);
            if (onScreenOnly && !SHOWN)
                continue;
            const double SCORE = centre(R).distance(target) + bias + (SHOWN ? 0 : 1500);
            if (!best || SCORE < best->second)
                best = {origin, SCORE};
        }

        if (!best)
            return std::nullopt;
        return Vector2D{std::round(best->first.x), std::round(best->first.y)};
    }
}
