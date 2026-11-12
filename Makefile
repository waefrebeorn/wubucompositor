# WuBuCompositor -- our own Wayland compositor for WuBuOS. No wlroots.
# Hand-written compositor on libwayland-server (the protocol machinery) +
# Xlib for the output + libxkbcommon for the keymap.
#
#   make            build ./wubucompositor
#   make run        run it nested in the current DISPLAY (X11 output)
#   make run-drm    (next leg: DRM/KMS direct to the panel)
#
CC      ?= gcc
CFLAGS  ?= -O2
CFLAGS  += -std=c11 -Wall -Wextra -Iprotocols -D_POSIX_C_SOURCE=200809L
CFLAGS  += $(shell pkg-config --cflags wayland-server xkbcommon x11)
LIBS    := $(shell pkg-config --libs wayland-server xkbcommon x11) -lm

SRC := src/wubucompositor.c protocols/wayland-server-protocol.c \
       protocols/xdg-shell-server-protocol.c
BIN := wubucompositor

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LIBS)

# Nested under the current X session: safe on the shared laptop.
run: $(BIN)
	./$(BIN)

# Self-test: boot, capture, assert the PPM is a real desktop (not mono).
test: $(BIN)
	./wcc_test.sh

clean:
	rm -f $(BIN)

.PHONY: all run test clean
