#include "Nearest.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace Expanse {

    std::optional<size_t> nearestBox(eDirection direction, const CBox& from, const std::vector<CBox>& boxes) {
        // Overlap of at least a third of the smaller span.
        const auto SHARED = [](double a0, double a1, double b0, double b1) { return std::min(a1, b1) - std::max(a0, b0) >= std::min(a1 - a0, b1 - b0) / 3.0; };
        const auto DISTANCE = [](const CBox& a, const CBox& b) { return a.middle().distance(b.middle()); };

        std::optional<std::pair<size_t, double>> inLine, inCone;
        for (size_t i = 0; i < boxes.size(); ++i) {
            const CBox&  r  = boxes[i];
            const double DX = r.middle().x - from.middle().x, DY = r.middle().y - from.middle().y;
            bool         ahead = false, sameLine = false, withinCone = false;
            double       gap   = 0.0;

            switch (direction) {
                case DIRECTION_RIGHT:
                    ahead      = DX > 1 && r.x > from.x;
                    gap        = r.x - (from.x + from.w);
                    sameLine   = SHARED(r.y, r.y + r.h, from.y, from.y + from.h);
                    withinCone = std::abs(DY) <= DX * 0.7;
                    break;
                case DIRECTION_LEFT:
                    ahead      = DX < -1 && r.x + r.w < from.x + from.w;
                    gap        = from.x - (r.x + r.w);
                    sameLine   = SHARED(r.y, r.y + r.h, from.y, from.y + from.h);
                    withinCone = std::abs(DY) <= -DX * 0.7;
                    break;
                case DIRECTION_DOWN:
                    ahead      = DY > 1 && r.y > from.y;
                    gap        = r.y - (from.y + from.h);
                    sameLine   = SHARED(r.x, r.x + r.w, from.x, from.x + from.w);
                    withinCone = std::abs(DX) <= DY * 0.7;
                    break;
                case DIRECTION_UP:
                    ahead      = DY < -1 && r.y + r.h < from.y + from.h;
                    gap        = from.y - (r.y + r.h);
                    sameLine   = SHARED(r.x, r.x + r.w, from.x, from.x + from.w);
                    withinCone = std::abs(DX) <= -DY * 0.7;
                    break;
            }

            if (!ahead)
                continue;

            if (sameLine) {
                // Equal gaps (two windows snapped below one): the better-aligned one wins.
                const double SCORE = gap + DISTANCE(r, from) / 1000.0;
                if (!inLine || SCORE < inLine->second)
                    inLine = {i, SCORE};
            } else if (withinCone) {
                const double D = DISTANCE(r, from);
                if (!inCone || D < inCone->second)
                    inCone = {i, D};
            }
        }

        if (inLine)
            return inLine->first;
        if (inCone)
            return inCone->first;
        return std::nullopt;
    }
}
