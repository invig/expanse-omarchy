CXX ?= g++

OUT     ?= expanse.so
SOURCES  = src/main.cpp src/Canvas.cpp src/CanvasInput.cpp src/CanvasStore.cpp src/Render.cpp src/Arrange.cpp src/Nearest.cpp src/Store.cpp src/Json.cpp src/HotCorner.cpp
HEADERS  = $(wildcard src/*.hpp)
# The Lua Hyprland itself links (5.5 for 0.56), found the way its CMake does.
LUA_PKG ?= $(shell for p in lua55 lua5.5 lua-55 lua-5.5 lua; do pkg-config --exists "$$p >= 5.5" && echo $$p && break; done)
PKGS     = pixman-1 libdrm hyprland pangocairo libinput libudev wayland-server xkbcommon $(LUA_PKG)

CXXFLAGS ?= -O2 -g
CXXFLAGS += -shared -fPIC -std=c++2b -Wno-narrowing
ifeq ($(CXX),g++)
    # Lets Hyprland unload and reload the plugin in one session.
    CXXFLAGS += -fno-gnu-unique
endif

.PHONY: all clean

all: $(OUT)

$(OUT): $(SOURCES) $(HEADERS)
	$(CXX) $(CXXFLAGS) $(SOURCES) -o $@.next `pkg-config --cflags $(PKGS)`
	mv -f $@.next $@

clean:
	rm -rf $(OUT) $(OUT).next .build

.PHONY: test
test: .build/nearest_test .build/store_test
	.build/nearest_test
	.build/store_test

.build/nearest_test: tests/nearest_test.cpp src/Nearest.cpp src/Nearest.hpp
	@mkdir -p .build
	$(CXX) -std=c++2b -O1 -g tests/nearest_test.cpp src/Nearest.cpp -o $@ `pkg-config --cflags --libs hyprutils`

.build/store_test: tests/store_test.cpp src/Store.cpp src/Store.hpp src/Json.cpp src/Json.hpp
	@mkdir -p .build
	$(CXX) -std=c++2b -O1 -g tests/store_test.cpp src/Store.cpp src/Json.cpp -o $@ `pkg-config --cflags --libs hyprutils`
