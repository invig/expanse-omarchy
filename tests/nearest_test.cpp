// The nearest-window rule, case by case. `make test` (needs hyprutils).
#include "../src/Nearest.hpp"

#include <cstdio>
#include <optional>
#include <vector>

using namespace Expanse;

static int failures = 0;

static void expect(const char* name, eDirection direction, const CBox& from, const std::vector<CBox>& boxes, std::optional<size_t> want) {
    const auto GOT = nearestBox(direction, from, boxes);
    if (GOT == want)
        return;
    ++failures;
    std::printf("FAIL %s: want %ld, got %ld\n", name, want ? (long)*want : -1L, GOT ? (long)*GOT : -1L);
}

int main() {
    const CBox FROM{0, 0, 100, 100};

    expect("a window sharing the row beats a nearer one off it", DIRECTION_RIGHT, FROM, {{500, 20, 100, 100}, {200, 90, 100, 100}}, 0);
    expect("off the row, the closest within the cone", DIRECTION_RIGHT, FROM, {{200, 90, 100, 100}, {400, 150, 100, 100}}, 0);
    expect("mostly below never counts as right", DIRECTION_RIGHT, FROM, {{150, 300, 100, 100}}, std::nullopt);
    expect("but it is down", DIRECTION_DOWN, FROM, {{150, 300, 100, 100}}, 0);
    expect("in a row, the nearest gap first", DIRECTION_RIGHT, FROM, {{300, 0, 100, 100}, {150, 0, 100, 100}}, 1);
    expect("equal gaps: the better-aligned one", DIRECTION_DOWN, FROM, {{50, 200, 100, 100}, {0, 200, 100, 100}}, 1);
    expect("left mirrors right", DIRECTION_LEFT, FROM, {{-300, 0, 100, 100}, {-150, 10, 100, 100}, {200, 0, 100, 100}}, 1);
    expect("up mirrors down", DIRECTION_UP, FROM, {{0, -300, 100, 100}, {20, -150, 100, 100}}, 1);
    expect("nothing that way", DIRECTION_UP, FROM, {{0, 200, 100, 100}, {300, 0, 100, 100}}, std::nullopt);
    expect("an overlapping window isn't ahead", DIRECTION_RIGHT, FROM, {{-20, 0, 100, 100}}, std::nullopt);

    if (failures == 0)
        std::printf("nearest: all cases pass\n");
    return failures == 0 ? 0 : 1;
}
