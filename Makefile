.POSIX:

VERSION = 0.1

PKG_CONFIG = pkg-config

WAYLAND_SCANNER   != $(PKG_CONFIG) --variable=wayland_scanner wayland-scanner
WAYLAND_PROTOCOLS != $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols

XWAYLAND = -DXWAYLAND
XLIBS    = xcb xcb-icccm

PKGS  = wlroots-0.19 wayland-server xkbcommon libinput $(XLIBS)
INCS != $(PKG_CONFIG) --cflags $(PKGS)
LIBS != $(PKG_CONFIG) --libs $(PKGS)

CPPFLAGS = -I. -D_POSIX_C_SOURCE=200809L -DWLR_USE_UNSTABLE -DVERSION=\"$(VERSION)\" $(XWAYLAND)
# CFLAGS   = -Wall -Wextra -Werror -Wshadow -Wno-unused-parameter $(INCS)
CFLAGS   = $(INCS)
LDFLAGS  = $(LIBS) 

all: compy

compy: compy.o

compy.o: compy.c \
	xdg-shell-protocol.h \
	wlr-layer-shell-unstable-v1-protocol.h \

xdg-shell-protocol.h:
	$(WAYLAND_SCANNER) server-header $(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@

wlr-layer-shell-unstable-v1-protocol.h:
	$(WAYLAND_SCANNER) enum-header protocols/wlr-layer-shell-unstable-v1.xml $@

config.h:
	cp config.def.h $@

clean:
	rm -f compy *.o *-protocol.h
