CXX := g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -pedantic
CC := gcc
CFLAGS := -std=c11 -O2 -Wall -Wextra -pedantic
TARGET := tuipaper
BG_TARGET := tuipaper-bg

WLR_LAYER_SHELL_H := wlr-layer-shell-unstable-v1-client-protocol.h
WLR_LAYER_SHELL_C := wlr-layer-shell-unstable-v1-protocol.c

.PHONY: all clean

all: $(TARGET) $(BG_TARGET)

$(TARGET): main.cpp tui.hpp browser.hpp preview.hpp wallpaper.hpp config.hpp
	$(CXX) $(CXXFLAGS) -o $@ main.cpp

$(WLR_LAYER_SHELL_H): protocols/wlr-layer-shell-unstable-v1.xml
	wayland-scanner client-header $< $@

$(WLR_LAYER_SHELL_C): protocols/wlr-layer-shell-unstable-v1.xml
	wayland-scanner private-code $< $@

$(BG_TARGET): tuipaper-bg.c $(WLR_LAYER_SHELL_H) $(WLR_LAYER_SHELL_C) stb_image.h
	$(CC) $(CFLAGS) -o $@ tuipaper-bg.c $(WLR_LAYER_SHELL_C) \
		$$(pkg-config --cflags --libs wayland-client) -lm

clean:
	rm -f $(TARGET) $(BG_TARGET) $(WLR_LAYER_SHELL_H) $(WLR_LAYER_SHELL_C)
