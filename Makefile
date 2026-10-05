# Build: make        Run: ./slime-wallpaper
# Arch deps: sudo pacman -S gcc make wayland wayland-protocols wlr-protocols

CC      ?= gcc
# [CHANGE 6] -march=native lets gcc use your CPU's SIMD instructions in the blur loops
CFLAGS  ?= -O3 -march=native -Wall
WAYLAND_PROTOCOLS := $(shell pkg-config --variable=pkgdatadir wayland-protocols)
WLR_PROTOCOLS     ?= /usr/share/wlr-protocols

GEN = xdg-shell-client-protocol.h xdg-shell-protocol.c \
      viewporter-client-protocol.h viewporter-protocol.c \
      wlr-layer-shell-unstable-v1-client-protocol.h wlr-layer-shell-unstable-v1-protocol.c

slime-wallpaper: slime_wallpaper.c $(GEN)
	$(CC) $(CFLAGS) -o $@ slime_wallpaper.c xdg-shell-protocol.c viewporter-protocol.c wlr-layer-shell-unstable-v1-protocol.c $(shell pkg-config --cflags --libs wayland-client) -lm

xdg-shell-client-protocol.h: $(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml
	wayland-scanner client-header $< $@
xdg-shell-protocol.c: $(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml
	wayland-scanner private-code $< $@

viewporter-client-protocol.h: $(WAYLAND_PROTOCOLS)/stable/viewporter/viewporter.xml
	wayland-scanner client-header $< $@
viewporter-protocol.c: $(WAYLAND_PROTOCOLS)/stable/viewporter/viewporter.xml
	wayland-scanner private-code $< $@

wlr-layer-shell-unstable-v1-client-protocol.h: $(WLR_PROTOCOLS)/unstable/wlr-layer-shell-unstable-v1.xml
	wayland-scanner client-header $< $@
wlr-layer-shell-unstable-v1-protocol.c: $(WLR_PROTOCOLS)/unstable/wlr-layer-shell-unstable-v1.xml
	wayland-scanner private-code $< $@

clean:
	rm -f slime-wallpaper $(GEN)

.PHONY: clean
