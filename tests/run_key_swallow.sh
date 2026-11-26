#!/bin/sh
# Builds and runs the zoomed-out key-swallow cases. Exits non-zero while
# Ctrl/Alt/Super chords pressed on the canvas are delivered to the focused client.
set -eu
cd "$(dirname "$0")/.."
mkdir -p .build
g++ -std=c++2b -O1 -g -ffunction-sections -fdata-sections \
    -I src -I tests/stubs -I tests/third_party/hyprutils/include \
    tests/key_swallow_test.cpp tests/key_swallow_link.cpp src/CanvasInput.cpp \
    tests/third_party/hyprutils/src/math/Box.cpp tests/third_party/hyprutils/src/math/Vector2D.cpp \
    -o .build/key_swallow_test -Wl,--gc-sections
.build/key_swallow_test
