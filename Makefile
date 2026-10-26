CXX ?= g++

OUT     ?= expanse.so
SOURCES  = src/main.cpp src/Canvas.cpp
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
	rm -f $(OUT) $(OUT).next
