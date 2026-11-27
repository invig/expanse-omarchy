#pragma once

// Subset of hyprutils 5a7b8cf (the revision Hyprland 0.56.2's flake.lock pins),
// with the operations Expanse calls. Bodies match that revision's Vector2D.cpp.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Hyprutils::Math {
    class Vector2D {
      public:
        constexpr Vector2D(double xx, double yy) : x(xx), y(yy) {}
        constexpr Vector2D(int xx, int yy) : x(xx), y(yy) {}
        constexpr Vector2D() = default;

        double x = 0;
        double y = 0;

        constexpr Vector2D operator+(const Vector2D& a) const { return {x + a.x, y + a.y}; }
        constexpr Vector2D operator-(const Vector2D& a) const { return {x - a.x, y - a.y}; }
        constexpr Vector2D operator-() const { return {-x, -y}; }
        constexpr Vector2D operator*(const double& a) const { return {x * a, y * a}; }
        constexpr Vector2D operator/(const double& a) const { return {x / a, y / a}; }
        constexpr bool     operator==(const Vector2D& a) const { return a.x == x && a.y == y; }
        constexpr bool     operator!=(const Vector2D& a) const { return !(*this == a); }
        constexpr Vector2D operator*(const Vector2D& a) const { return {x * a.x, y * a.y}; }
        constexpr Vector2D operator/(const Vector2D& a) const { return {x / a.x, y / a.y}; }

        // hyprutils Vector2D.cpp:39
        double size() const { return std::sqrt((x * x) + (y * y)); }
        // hyprutils Vector2D.cpp:23
        Vector2D round() const { return {std::round(x), std::round(y)}; }
        // hyprutils Vector2D.cpp:35
        double distanceSq(const Vector2D& other) const { return ((x - other.x) * (x - other.x)) + ((y - other.y) * (y - other.y)); }
        // hyprutils Vector2D.cpp:31
        double distance(const Vector2D& other) const { return std::sqrt(distanceSq(other)); }
    };
}
