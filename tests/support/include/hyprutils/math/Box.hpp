#pragma once

// Subset of hyprutils 5a7b8cf Box, with the operations Expanse calls.
// Bodies match that revision's Box.cpp.

#include "Vector2D.hpp"

#include <algorithm>
#include <cmath>

namespace Hyprutils::Math {
    class CBox {
      public:
        CBox(double x_, double y_, double w_, double h_) : x(x_), y(y_), w(w_), h(h_) {}
        CBox() = default;
        CBox(const Vector2D& pos, const Vector2D& size_) : x(pos.x), y(pos.y), w(size_.x), h(size_.y) {}

        // hyprutils Box.cpp:32
        CBox& translate(const Vector2D& vec) {
            x += vec.x;
            y += vec.y;
            return *this;
        }
        // hyprutils Box.cpp:136
        CBox& expand(const double& value) {
            constexpr double EPSILON = 1e-9;
            constexpr double DOUBLE  = 2.0;
            x -= value;
            y -= value;
            w += value * DOUBLE;
            h += value * DOUBLE;
            if (w <= EPSILON || h <= EPSILON) {
                w = 0;
                h = 0;
            }
            return *this;
        }
        // hyprutils Box.cpp:51
        CBox& round() {
            const double roundedX = std::round(x);
            const double roundedY = std::round(y);
            const double newW     = x + w - roundedX;
            const double newH     = y + h - roundedY;
            x                     = roundedX;
            y                     = roundedY;
            w                     = std::round(newW);
            h                     = std::round(newH);
            return *this;
        }
        // hyprutils Box.cpp:190
        CBox copy() const { return *this; }
        // hyprutils Box.cpp:157
        CBox intersection(const CBox& other) const {
            constexpr double EPSILON   = 1e-9;
            const double     newX      = std::max(x, other.x);
            const double     newY      = std::max(y, other.y);
            const double     newBottom = std::min(y + h, other.y + other.h);
            const double     newRight  = std::min(x + w, other.x + other.w);
            double           newW      = newRight - newX;
            double           newH      = newBottom - newY;
            if (newW <= EPSILON || newH <= EPSILON) {
                newW = 0;
                newH = 0;
            }
            return {newX, newY, newW, newH};
        }
        // hyprutils Box.cpp:39
        Vector2D middle() const { return {x + (w * 0.5), y + (h * 0.5)}; }
        // hyprutils Box.cpp:194
        Vector2D pos() const { return {x, y}; }
        // hyprutils Box.cpp:198
        Vector2D size() const { return {w, h}; }
        // hyprutils Box.cpp:43, half-open
        bool containsPoint(const Vector2D& vec) const { return vec.x >= x && vec.x < x + w && vec.y >= y && vec.y < y + h; }
        // hyprutils Box.cpp:47
        bool empty() const {
            constexpr double EPSILON = 1e-9;
            return std::fabs(w) < EPSILON || std::fabs(h) < EPSILON;
        }
        bool operator==(const CBox& rhs) const { return x == rhs.x && y == rhs.y && w == rhs.w && h == rhs.h; }

        double x = 0, y = 0;
        double w = 0, h = 0;
    };
}

using namespace Hyprutils::Math;
