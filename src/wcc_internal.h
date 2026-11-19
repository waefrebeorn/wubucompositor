/*
 * wcc_internal.h -- shared compositor state (step 1 of the monolith split).
 *
 * The protocol state (Wayland globals, surfaces, seat) and the HOST state
 * (X11/GLX today; a Quartz/Metal backend tomorrow) live in one struct for
 * now; the host fields are the seam a second backend replaces. Leaves
 * include this; nothing outside the compositor does.
 */
#ifndef WCC_INTERNAL_H
#define WCC_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <X11/Xlib.h>
#include <X11/extensions/XShm.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include "wcc_monitors.h"

/* Output size: set from the real screen before the window is created. */
extern int WC_OUTPUT_W, WC_OUTPUT_H;

struct wcc_server;

struct wcc_surface {
    struct wcc_server *server;
    struct wl_resource *resource;   /* wl_surface */
    struct wl_resource *toplevel;   /* xdg_toplevel role resource */
    bool configured;                /* xdg configure acked */
    struct wl_resource *pending_buffer;
    struct wl_shm_buffer *buf;
    struct wl_resource *frame_cb;
};

/* One per advertised display.  A wl_output is a GLOBAL: binding it must yield THAT
 * output, not whichever one a single shared global happens to describe.  Each global
 * therefore carries its own context naming the monitor it belongs to. */
typedef struct {
    struct wcc_server *server;
    wcc_monitor_t     *monitor;
} wcc_output_ctx;

struct wcc_server {
    struct wl_display *display;
    struct wl_event_loop *loop;
    struct wl_resource *compositor_res, *seat_res, *wm_res;
    /* Per-client bound output resources, one per advertised monitor.  A single cached
     * pointer could only ever track one of them; after a client disconnect the rest
     * would dangle and the next event would dereference freed memory. */
    struct wl_resource *output_res[WCC_MAX_MONITORS];
    int                 n_output_res;
    struct wcc_surface *app;

    /* What is actually attached.  Replaces the previous single hardcoded output
     * (geometry 0,0,0,0 and one fixed mode) with the real display's geometry, name,
     * physical size and full mode list -- which is what resolution support needs:
     * a guest can only CHOOSE from modes it was told about. */
    wcc_monitors_t monitors;
    wcc_output_ctx output_ctx[WCC_MAX_MONITORS];

    /* This compositor drives exactly one desktop.  The owner is remembered so
     * a second client is refused instead of silently sharing the seat -- two
     * clients on one seat produced "object from one client used for another"
     * errors and an undefined pointer/keyboard state. */
    struct wl_client *client;
    struct wl_listener client_destroy;

    Display *xd; Window xw;
    int xfd;
    int xd_focused;
    XImage *ximg;
    unsigned char *ximg_owner;
    XShmSegmentInfo shm;
    bool shm_ok;

    /* GPU present (GLX).  When ok, the client's buffer is uploaded to a
     * texture and drawn by the GPU; the XShm path becomes the fallback. */
    struct {
        bool ok;
        XVisualInfo *vi;
        Visual *visual;
        int depth;
        GLXContext ctx;
        GLuint tex;
        unsigned char *last;   /* last presented frame, for change detect */
    } gpu;
    /* self-screenshot: --screenshot PATH captures the window as PPM after
     * the desktop's first commits, then exits 0 (headless-safe verify). */
    const char *shot_path;
    int shot_done;
    int shot_commits;
    int shot_delay_ms;
    uint32_t shot_start_ms;

    char *xkb_keymap_str;

    struct {
        struct wl_resource *pointer_res, *keyboard_res;
        struct wcc_surface *focused;
        double px, py;
        uint32_t mods_depressed;   /* xkb modifier mask (wl_keyboard.modifiers) */
        uint32_t mods_locked;      /* caps lock */
    } seat;
};

/* xkb modifier mask bits, matching wl_keyboard.modifiers. */
#define WCC_MOD_SHIFT   (1u << 0)
#define WCC_MOD_LOCK    (1u << 1)
#define WCC_MOD_CTRL    (1u << 2)
#define WCC_MOD_ALT     (1u << 3)
#define WCC_MOD_NUM     (1u << 4)
#define WCC_MOD_MOD3    (1u << 5)
#define WCC_MOD_SUPER   (1u << 6)
#define WCC_MOD_MOD5    (1u << 7)

/* Shared helpers (defined in wubucompositor.c until the util leaf). */
uint32_t wcc_now_ms(void);
uint32_t wcc_next_serial(void);

/* GPU present leaf (wcc_gpu.c): GLX texture upload + fullscreen quad. */
void wcc_gpu_destroy(struct wcc_server *s);
void wcc_gpu_setup(struct wcc_server *s);
void wcc_gpu_present(struct wcc_server *s, const void *data,
                     int w, int h, int stride);

#endif /* WCC_INTERNAL_H */
