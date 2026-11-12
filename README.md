# WuBuCompositor -- the WuBuOS compositor (slerm'd, no wlroots).
# Our own Wayland compositor on libwayland-server + Xlib + libxkbcommon,
# driving the wubu hosted desktop fullscreen. Rendering through XPutImage
# with present-on-change (no flashing). See src/wubucompositor.c.

## Build
Build the compositor. Requires libwayland-dev, libx11-dev, libxkbcommon-dev.
Run inside an X session (DISPLAY set); it opens a fullscreen 1024x768
window and auto-launches the wubu client (override with WUBU_START_CMD).

    make       # -> ./wubucompositor
    make run   # run it in the current X session

## The idea
weston is laggy and repaints everything; WuBuOS deserves its own
compositor. This one is hand-written on libwayland-server (the wire
protocol machinery, not a compositor) with Xlib for the output. All the
compositor logic is ours: the globals, wl_surface/shm bookkeeping, the
xdg-shell role, the seat, and the paint path.

The client is driven fullscreen and damage is tracked by the surface
commit path: the compositor copies the client's shm buffer and skips the
XPutImage when it is byte-identical to what it last showed, so an
unchanged desktop repaints nothing -- the flashing is gone by design.

## Protocols
`protocols/` holds wayland-scanner-generated server bindings for the
core wayland.xml and xdg-shell. `make` rebuilds them from
/usr/share/wayland and /usr/share/wayland-protocols.

## State
Working: connection, surface/shm rendering, output reporting, seat
pointer (motion/button) and keyboard pass-through, present-on-change.
Next leg: a DRM/KMS direct-to-panel backend (no X), and xkb-aware
keycode translation for the keyboard path.
