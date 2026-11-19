# WuBuCompositor — the WuBuOS compositor (slerm'd, no wlroots)

## Why this exists: the WSL-replacement philosophy

WuBuOS is a **WSL replacement**: *develop in Linux, run in macOS* — the way
WSL lets you develop in Linux and run on Windows. The architecture that
makes that true has two halves:

1. **The guest is host-agnostic.** The WuBuOS desktop (`wubuos/src/hosted/wubu`)
   is a pure C11 **Wayland client** (xdg-shell, SHM double-buffering,
   xkbcommon keymap). It speaks the Wayland wire protocol and nothing
   host-specific: no Xlib, no Quartz, no Win32 in the client core. Where it
   runs is decided by whoever serves the protocol.

2. **The compositor is the host seam.** This compositor owns the host's
   display and input and serves Wayland to the guest. Today the host side
   is X11 (Xlib output, XShm/GLX present, XRandR monitor enumeration,
   XGrabKeyboard focus). To run the same desktop on macOS, a Quartz/Metal
   backend replaces the host side — the protocol logic, the guest, and
   everything above it are unchanged. That is why WuBuOS has its OWN
   compositor instead of weston: we control the seam.

Same shape as WSLg (a Wayland compositor bridging the guest to the host's
display), built as our own C11 code.

## Portability map (honest status)

Portable today (verified on Linux):
- All Wayland protocol logic: globals, wl_surface/shm, xdg-shell, seat,
  ownership (one client per output).
- Keymap: libxkbcommon (cross-platform).
- Keymap transport: POSIX `shm_open` (replaced Linux-only `memfd_create`).
- Guest/buddy launch paths: derived from the executable's own location —
  `/proc/self/exe` on Linux, `_NSGetExecutablePath` on macOS
  (`src/wcc_spawn.c`, and the buddy launcher in the guest).

Host-bound (the seam a second backend replaces):
- Xlib window/output, XShm fallback present, GLX GPU present
  (`src/wcc_gpu.c` — first extracted host leaf), XRandR enumeration
  (`src/wcc_monitors.c`), X event dispatch + input forwarding (still in
  `src/wubucompositor.c`, extraction mapped below).

macOS execution leg (guest binaries, separate from hosting): the VSL
Mach-O loader parses Mach-O (`test_vsl_macho`), and Metal compute shaders
translate to SPIR-V via metal2vulkan and run on a real Vulkan pipeline
(proven). Full Mach-O *process* execution is open (darling is a dead end).
A Quartz hosting backend is the next host backend; it cannot be built or
verified on this Linux box — stated plainly, not implied.

## Build & verify

    make            # -> ./wubucompositor
    make run        # nested in the current X session (DISPLAY set)
    make test-spawn # HEADLESS behaviour gate: guest-launch leaf (no X)
    make test       # live self-test: boot, capture PPM, assert real desktop

Deps: libwayland-dev, libx11-dev, libxkbcommon-dev, libxext-dev (XShm),
libgl-dev (GLX), libxrandr-dev. `--screenshot out.ppm` captures the
desktop after the first commits and exits 0.

Guest launch: `WUBU_START_CMD` overrides the command; `WUBU_NO_SPAWN=1`
waits for a client you start yourself (never run two clients on one seat).
The default guest path is derived from the binary's location — sibling
`wubuos/src/hosted/wubu` — never a hardcoded home directory.

## Present-on-change (anti-flashing)

The client is driven fullscreen; the commit path copies the client's shm
buffer into a compositor-owned copy and skips the present when it is
byte-identical to what was last shown, so an unchanged desktop repaints
nothing — the flashing is gone by design. The policy lives in the
PRESENT path, never in the render loop.

## Protocols

`protocols/` holds wayland-scanner-generated server bindings for the
core wayland.xml and xdg-shell. `make` rebuilds them from
/usr/share/wayland and /usr/share/wayland-protocols.

## Monolith split (in progress)

`src/wubucompositor.c` is being dissolved per the no-monolith law:

- done: `wcc_spawn.{c,h}` (guest launch), `wcc_internal.h` (shared state),
  `wcc_gpu.c` (GLX present).
- next: `wcc_present.c`/`wcc_capture.c` (XShm blit + PPM shot),
  `wcc_seat.c`, `wcc_surface.c`/`wcc_globals.c`, `wcc_output.c`,
  `wcc_xdg.c`, `wcc_x11.c` + fresh entry. Full map with line ranges:
  wubuos `docs/compendium/04-roadmap/hardcoding-census.md`.

`src/main.c` is the DEAD wlroots-era entry point (banner-marked LEGACY;
the Makefile builds only the slerm).

## Rules that bite (each cost a real crash — see the wubucompositor skill)

- Every cached wl_resource needs a destroy listener; refuse intruders by
  binding no-ops, never by destroying them mid-bind.
- Pointer coords are wl_fixed_t (24.8), not ints; send modifiers on every
  key event; send pointer enter on button press too.
- Choose the GLX visual BEFORE XCreateWindow; client bytes are B,G,R,00 →
  GL_BGRA, blending OFF.
- XRandR is hosted-only; bare-metal display info is UEFI GOP + EDID.

## State

Working: connection, surface/shm rendering, per-monitor output reporting,
seat pointer (motion/button) and keyboard pass-through, present-on-change,
GPU (GLX) present with XShm fallback. Next legs: finish the monolith split,
then a DRM/KMS direct-to-panel backend (no X) and the Quartz/Metal host
backend for the macOS side of the philosophy.