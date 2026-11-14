/*
 * wcc_monitors.h -- what the compositor knows about attached displays.
 *
 * WHY THIS EXISTS
 *   The compositor advertised ONE hardcoded wl_output: geometry 0,0,0,0 and a single
 *   mode, regardless of what was plugged in.  The guest now has a real display model
 *   (hosted_display.c) ready to consume several outputs, so the missing half is this
 *   side: enumerate what is actually attached, where each sits, its physical size and
 *   its current mode.
 *
 * WHY IT IS A SEPARATE LEAF
 *   Discovery touches X11 and XRandR; everything downstream (advertising wl_output,
 *   choosing a surface size) works from plain descriptors.  Keeping the protocol out
 *   of the descriptor list means the list can be unit-tested without an X server, and
 *   a machine with no display reports an honest failure instead of invented geometry.
 *
 * COORDINATES
 *   x/y are the monitor's position in the X screen, which matches Wayland's virtual
 *   desktop space: a second monitor right of the first has x == first width.
 */
/*
 * HOSTED-ONLY SOURCE -- read this before using it on bare metal.
 *
 * This module reads displays from XRandR, i.e. from an X server running under Linux.
 * On the BOOTABLE WuBuOS image there is no Linux and no X server: the display
 * information has to come from the firmware's own interface (UEFI GOP mode list) plus
 * the monitor's EDID over the GPU bus, or from VBE on legacy BIOS.
 *
 * The descriptor list here is deliberately source-independent so a GOP/EDID reader can
 * produce the same structures without touching anything downstream.  Do NOT wire
 * XRandR discovery into the bootable kernel path -- it will always report "no display"
 * and, worse, would look like a real answer if anyone trusted it.
 */
#ifndef WCC_MONITORS_H
#define WCC_MONITORS_H

#include <stdbool.h>

#define WCC_MAX_MONITORS 8
#define WCC_MONITOR_NAME 64

#define WCC_MAX_MODES 32

typedef struct {
    int  w, h;
    int  refresh_mhz;
    bool current;               /* the mode the CRTC is actually running */
} wcc_mode_t;

typedef struct {
    char     name[WCC_MONITOR_NAME];
    int      x, y;              /* position in the virtual desktop */
    int      w, h;              /* current mode resolution */
    int      mm_w, mm_h;        /* physical size, for wl_output geometry */
    int      refresh_mhz;
    bool     connected;         /* disconnected ports must never be advertised */
    bool     primary;
    /* Every mode the output offers, best-first, with the active one flagged.  This is
     * what a resolution menu needs: advertising only the CURRENT mode leaves the guest
     * nothing to CHOOSE, which is not resolution support -- it is a readout. */
    int         nmodes;
    wcc_mode_t  modes[WCC_MAX_MODES];
} wcc_monitor_t;

typedef struct {
    wcc_monitor_t m[WCC_MAX_MONITORS];
    int count;
} wcc_monitors_t;

/* Enumerate attached displays via XRandR.
 *
 * Returns the number of CONNECTED monitors written, or -1 when no X display could be
 * opened (headless session, wayland-native with no X, ssh).  On -1 `out` is cleared:
 * callers must treat "no display" as "nothing to advertise", never as a single
 * zero-sized phantom output -- which is exactly what the old hardcoded path produced.
 *
 * Disconnected ports are skipped entirely.  Advertising them would give clients a
 * display they cannot show anything on.
 */
int wcc_monitors_discover(wcc_monitors_t *out);

#endif /* WCC_MONITORS_H */
