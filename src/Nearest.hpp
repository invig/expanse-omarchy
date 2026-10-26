#pragma once

#include <hyprutils/math/Box.hpp>

#include <cstdint>
#include <optional>
#include <vector>

using namespace Hyprutils::Math;

namespace Expanse {

    enum eDirection : uint8_t {
        DIRECTION_LEFT,
        DIRECTION_RIGHT,
        DIRECTION_UP,
        DIRECTION_DOWN,
    };

    // The nearest box in `direction` from `from`, as Expanse on the Mac picks
    // it: a box sharing the row (left / right) or column (up / down) wins,
    // nearest first; failing that, the closest within ~35° of straight that way.
    // Boxes mostly above or below never count as left or right.
    std::optional<size_t> nearestBox(eDirection direction, const CBox& from, const std::vector<CBox>& boxes);
}
