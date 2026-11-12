/*
 * WuBuCompositor -- the WuBuOS compositor, slerm'd, no wlroots.
 *
 * A hand-written Wayland compositor on libwayland-server (the wire
 * protocol machinery -- like a socket library, not a compositor) plus
 * Xlib for the output and libxkbcommon for the keyboard map. The
 * compositor logic itself -- the globals, surface bookkeeping, the
 * xdg-shell role, the seat, and the paint to the output -- is ours,
 * written for exactly one client: the wubu hosted desktop, fullscreen.
 *
 * The client binds wl_compositor, wl_shm, xdg_wm_base, wl_seat (pointer
 * + keyboard) and wl_output; this compositor provides precisely those.
 *
 * Output backend: an X11 window. It renders the client's committed
 * wl_shm buffer with XPutImage and forwards X input events to the
 * seat. (A DRM/KMS direct-to-panel path is the next leg.)
 *
 * C11.
 */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/memfd.h>
#include <time.h>

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include "xdg-shell-server-protocol.h"

#include <X11/Xlib.h>
#include <X11/cursorfont.h>
#include <xkbcommon/xkbcommon.h>

#define WC_OUTPUT_W 1024
#define WC_OUTPUT_H 768
#define BTN_LEFT 0x110
#define BTN_MIDDLE 0x112
#define BTN_RIGHT 0x111

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
static uint32_t next_serial(void)
{
    static uint32_t s;
    return ++s;
}

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

/* impls referenced across record boundaries */
static const struct xdg_surface_interface xdg_surface_impl;
static const struct xdg_toplevel_interface xdg_toplevel_impl;

struct wcc_server {
    struct wl_display *display;
    struct wl_event_loop *loop;
    struct wl_resource *compositor_res, *seat_res, *output_res, *wm_res;
    struct wcc_surface *app;

    Display *xd; Window xw;
    int xfd;
    int xd_focused;
    XImage *ximg;
    unsigned char *ximg_owner;

    char *xkb_keymap_str;

    struct {
        struct wl_resource *pointer_res, *keyboard_res;
        struct wcc_surface *focused;
        double px, py;
    } seat;
};

/* =================== wl_compositor / wl_surface ========================= */

static void surface_destroy(struct wl_client *c, struct wl_resource *r)
{
    (void)c; wl_resource_destroy(r);
}
static void surface_attach(struct wl_client *c, struct wl_resource *r,
                           struct wl_resource *buffer, int32_t x, int32_t y)
{
    (void)c; (void)x; (void)y;
    struct wcc_surface *surf = wl_resource_get_user_data(r);
    surf->pending_buffer = buffer;
}
static void surface_damage(struct wl_client *c, struct wl_resource *r,
                           int32_t x, int32_t y, int32_t w, int32_t h)
{
    (void)c;(void)r;(void)x;(void)y;(void)w;(void)h;
}
static void surface_frame(struct wl_client *c, struct wl_resource *r,
                          uint32_t id)
{
    struct wcc_surface *surf = wl_resource_get_user_data(r);
    if (surf->frame_cb) wl_resource_destroy(surf->frame_cb);
    surf->frame_cb = wl_resource_create(c, &wl_callback_interface, 1, id);
}
static void surface_set_opaque(struct wl_client *c, struct wl_resource *r,
                               struct wl_resource *region)
{ (void)c;(void)r;(void)region; }
static void surface_set_input(struct wl_client *c, struct wl_resource *r,
                              struct wl_resource *region)
{ (void)c;(void)r;(void)region; }

static void blit(struct wcc_server *s, struct wcc_surface *surf)
{
    if (!surf->buf || !s->xw) return;
    void *data = wl_shm_buffer_get_data(surf->buf);
    int w = wl_shm_buffer_get_width(surf->buf);
    int h = wl_shm_buffer_get_height(surf->buf);
    int stride = wl_shm_buffer_get_stride(surf->buf);
    size_t len = (size_t)w * h * 4;

    /* Present-on-change: if the buffer is byte-identical to what we last
     * showed, skip the XPutImage -- a repaint of an unchanged surface is
     * what reads as flashing on the panel. Copy into our own backing
     * store first so we never hand the X server a pointer into the
     * client's shared pool (which it may reuse once released). */
    if (memcmp(data, s->ximg_owner, len) == 0)
        return;
    memcpy(s->ximg_owner, data, len);

    s->ximg->width = w;
    s->ximg->height = h;
    s->ximg->bytes_per_line = stride;
    s->ximg->data = (char *)s->ximg_owner;
    XPutImage(s->xd, s->xw, DefaultGC(s->xd, 0), s->ximg,
              0, 0, 0, 0, w, h);
    XSync(s->xd, False);
}

static void surface_commit(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    struct wcc_surface *surf = wl_resource_get_user_data(r);
    struct wcc_server *s = surf->server;
    if (!surf->configured || !surf->pending_buffer) {
        if (surf->frame_cb) { wl_callback_send_done(surf->frame_cb, now_ms());
            wl_resource_destroy(surf->frame_cb); surf->frame_cb = NULL; }
        return;
    }
    struct wl_resource *buf_res = surf->pending_buffer;
    struct wl_shm_buffer *buf = wl_shm_buffer_get(buf_res);
    if (buf) {
        surf->buf = buf;
        blit(s, surf);
        wl_buffer_send_release(buf_res);
    }
    surf->pending_buffer = NULL;
    if (surf->frame_cb) { wl_callback_send_done(surf->frame_cb, now_ms());
        wl_resource_destroy(surf->frame_cb); surf->frame_cb = NULL; }
}

static const struct wl_surface_interface surface_impl = {
    .destroy = surface_destroy,
    .attach = surface_attach,
    .damage = surface_damage,
    .frame = surface_frame,
    .set_opaque_region = surface_set_opaque,
    .set_input_region = surface_set_input,
    .commit = surface_commit,
};

static void region_destroy(struct wl_client *c, struct wl_resource *r)
{ (void)c; wl_resource_destroy(r); }
static void region_add(struct wl_client *c, struct wl_resource *r,
                       int32_t x, int32_t y, int32_t w, int32_t h)
{ (void)c;(void)r;(void)x;(void)y;(void)w;(void)h; }
static void region_subtract(struct wl_client *c, struct wl_resource *r,
                            int32_t x, int32_t y, int32_t w, int32_t h)
{ (void)c;(void)r;(void)x;(void)y;(void)w;(void)h; }
static const struct wl_region_interface region_impl = {
    .destroy = region_destroy, .add = region_add, .subtract = region_subtract,
};

static void compositor_create_region(struct wl_client *c,
                                     struct wl_resource *r, uint32_t id)
{
    struct wl_resource *reg = wl_resource_create(c, &wl_region_interface,
                                                 wl_resource_get_version(r), id);
    wl_resource_set_implementation(reg, &region_impl, NULL, NULL);
}

static void compositor_create_surface(struct wl_client *c,
                                      struct wl_resource *r, uint32_t id)
{
    struct wcc_server *s = wl_resource_get_user_data(r);
    struct wcc_surface *surf = calloc(1, sizeof(*surf));
    if (!surf) { wl_resource_post_no_memory(r); return; }
    surf->server = s;
    surf->resource = wl_resource_create(c, &wl_surface_interface,
                                        wl_resource_get_version(r), id);
    wl_resource_set_implementation(surf->resource, &surface_impl, surf, NULL);
    if (!s->app) s->app = surf;
}

static const struct wl_compositor_interface compositor_impl = {
    .create_surface = compositor_create_surface,
    .create_region = compositor_create_region,
};
static void compositor_bind(struct wl_client *c, void *data,
                            uint32_t version, uint32_t id)
{
    struct wcc_server *s = data;
    s->compositor_res = wl_resource_create(c, &wl_compositor_interface,
                                           version, id);
    wl_resource_set_implementation(s->compositor_res, &compositor_impl, s, NULL);
}

/* =============================== wl_seat ================================ */

static void pointer_set_cursor(struct wl_client *c, struct wl_resource *r,
                               uint32_t serial, struct wl_resource *surface,
                               int32_t hx, int32_t hy)
{ (void)c;(void)r;(void)serial;(void)surface;(void)hx;(void)hy; }
static void pointer_release(struct wl_client *c, struct wl_resource *r)
{ (void)c; wl_resource_destroy(r); }
static const struct wl_pointer_interface pointer_impl = {
    .set_cursor = pointer_set_cursor, .release = pointer_release,
};
static void keyboard_release(struct wl_client *c, struct wl_resource *r)
{ (void)c; wl_resource_destroy(r); }
static const struct wl_keyboard_interface keyboard_impl = {
    .release = keyboard_release,
};
static void touch_release(struct wl_client *c, struct wl_resource *r)
{ (void)c; wl_resource_destroy(r); }
static const struct wl_touch_interface touch_impl = { .release = touch_release };

static void seat_get_pointer(struct wl_client *c, struct wl_resource *r,
                             uint32_t id)
{
    struct wcc_server *s = wl_resource_get_user_data(r);
    s->seat.pointer_res = wl_resource_create(c, &wl_pointer_interface,
                                             wl_resource_get_version(r), id);
    wl_resource_set_implementation(s->seat.pointer_res, &pointer_impl, s, NULL);
}
static void seat_get_keyboard(struct wl_client *c, struct wl_resource *r,
                              uint32_t id)
{
    struct wcc_server *s = wl_resource_get_user_data(r);
    s->seat.keyboard_res = wl_resource_create(c, &wl_keyboard_interface,
                                              wl_resource_get_version(r), id);
    wl_resource_set_implementation(s->seat.keyboard_res, &keyboard_impl, s, NULL);

    if (s->xkb_keymap_str) {
        size_t len = strlen(s->xkb_keymap_str);
        int fd = syscall(SYS_memfd_create, "wcc-keymap", MFD_CLOEXEC);
        if (fd >= 0) {
            ftruncate(fd, (off_t)len);
            void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (p != MAP_FAILED) {
                memcpy(p, s->xkb_keymap_str, len);
                munmap(p, len);
            }
            wl_keyboard_send_keymap(s->seat.keyboard_res,
                                    WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd,
                                    (uint32_t)len);
            /* Autorepeat: the client generates repeats from this info
             * (both fields since wl_keyboard v1). 25 rate = 40 ms repeat. */
            wl_keyboard_send_repeat_info(s->seat.keyboard_res, 25, 600);
            struct wl_array empty = { 0 };
            if (s->app)
                wl_keyboard_send_enter(s->seat.keyboard_res, next_serial(),
                                       s->app->resource, &empty);
            close(fd);
        }
    }
}
static void seat_get_touch(struct wl_client *c, struct wl_resource *r,
                           uint32_t id)
{
    (void)r; (void)c;
}
static void seat_release(struct wl_client *c, struct wl_resource *r)
{ (void)c; wl_resource_destroy(r); }
static const struct wl_seat_interface seat_impl = {
    .get_pointer = seat_get_pointer,
    .get_keyboard = seat_get_keyboard,
    .get_touch = seat_get_touch,
    .release = seat_release,
};
static void seat_bind(struct wl_client *c, void *data,
                      uint32_t version, uint32_t id)
{
    struct wcc_server *s = data;
    s->seat_res = wl_resource_create(c, &wl_seat_interface, version, id);
    wl_resource_set_implementation(s->seat_res, &seat_impl, s, NULL);
    wl_seat_send_capabilities(s->seat_res,
                              WL_SEAT_CAPABILITY_POINTER |
                              WL_SEAT_CAPABILITY_KEYBOARD);
}

/* ============================= wl_output ================================ */

static const struct wl_output_interface output_impl = { .release = NULL };
static void output_bind(struct wl_client *c, void *data,
                        uint32_t version, uint32_t id)
{
    struct wcc_server *s = data;
    int v = version > 2 ? 2 : version;
    s->output_res = wl_resource_create(c, &wl_output_interface, v, id);
    wl_resource_set_implementation(s->output_res, &output_impl, NULL, NULL);
    wl_output_send_geometry(s->output_res, 0, 0, 0, 0,
                            WL_OUTPUT_SUBPIXEL_UNKNOWN, "", "",
                            WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(s->output_res, WL_OUTPUT_MODE_CURRENT |
                        WL_OUTPUT_MODE_PREFERRED,
                        WC_OUTPUT_W, WC_OUTPUT_H, 60000);
    if (v >= 2) wl_output_send_scale(s->output_res, 1);
    if (v >= 3) wl_output_send_done(s->output_res);
}

/* ============================ xdg shell ================================= */

static void wm_destroy(struct wl_client *c, struct wl_resource *r)
{ (void)c; wl_resource_destroy(r); }
static void pos_destroy(struct wl_client *c, struct wl_resource *r)
{ (void)c; wl_resource_destroy(r); }
static void pos_ignore(struct wl_client *c, struct wl_resource *r,
                       int32_t a, int32_t b)
{ (void)c;(void)r;(void)a;(void)b; }
static void pos_ignore_uint(struct wl_client *c, struct wl_resource *r,
                            uint32_t a)
{ (void)c;(void)r;(void)a; }
static void pos_ignore_uint2(struct wl_client *c, struct wl_resource *r,
                             struct wl_resource *a, uint32_t b)
{ (void)c;(void)r;(void)a;(void)b; }
static void pos_set_reactive(struct wl_client *c, struct wl_resource *r)
{ (void)c;(void)r; }
static const struct xdg_positioner_interface positioner_impl = {
    .destroy = pos_destroy,
    .set_size = pos_ignore,
    .set_anchor_rect = pos_ignore,
    .set_anchor = pos_ignore_uint,
    .set_gravity = pos_ignore_uint,
    .set_constraint_adjustment = pos_ignore_uint,
    .set_offset = pos_ignore,
    .set_reactive = pos_set_reactive,
    .set_parent_size = pos_ignore_uint2,
    .set_parent_configure = pos_ignore_uint,
};
static void wm_create_positioner(struct wl_client *c, struct wl_resource *r,
                                 uint32_t id)
{
    struct wl_resource *pr = wl_resource_create(c, &xdg_positioner_interface,
                                                wl_resource_get_version(r), id);
    wl_resource_set_implementation(pr, &positioner_impl, NULL, NULL);
}
static void wm_pong(struct wl_client *c, struct wl_resource *r, uint32_t serial)
{ (void)c;(void)r;(void)serial; }

static void wm_get_xdg_surface(struct wl_client *c, struct wl_resource *r,
                               uint32_t id, struct wl_resource *surface)
{
    struct wcc_server *s = wl_resource_get_user_data(r);
    struct wcc_surface *surf = wl_resource_get_user_data(surface);

    struct wl_resource *res = wl_resource_create(c, &xdg_surface_interface,
                                                 wl_resource_get_version(r), id);
    wl_resource_set_implementation(res, &xdg_surface_impl, surf, NULL);
    /* First configure: the client acks then may attach a buffer. */
    surf->toplevel = NULL;   /* cleared until get_toplevel */
    (void)s;
}

static const struct xdg_wm_base_interface wm_impl = {
    .destroy = wm_destroy,
    .create_positioner = wm_create_positioner,
    .get_xdg_surface = wm_get_xdg_surface,
    .pong = wm_pong,
};
static void wm_bind(struct wl_client *c, void *data,
                    uint32_t version, uint32_t id)
{
    struct wcc_server *s = data;
    s->wm_res = wl_resource_create(c, &xdg_wm_base_interface, version, id);
    wl_resource_set_implementation(s->wm_res, &wm_impl, s, NULL);
    xdg_wm_base_send_ping(s->wm_res, next_serial());
}

/* --- xdg_surface --- */

static void xdg_surface_destroy(struct wl_client *c, struct wl_resource *r)
{ (void)c; wl_resource_destroy(r); }
static void xdg_surface_get_toplevel(struct wl_client *c,
                                     struct wl_resource *r, uint32_t id)
{
    struct wcc_surface *surf = wl_resource_get_user_data(r);
    surf->toplevel = wl_resource_create(c, &xdg_toplevel_interface,
                                        wl_resource_get_version(r), id);
    wl_resource_set_implementation(surf->toplevel, &xdg_toplevel_impl, surf, NULL);
    /* configure: fullscreen-sized, empty states; the client acks the
     * corresponding xdg_surface.configure, enabling attach */
    struct wl_array states = { 0 };
    xdg_toplevel_send_configure(surf->toplevel, WC_OUTPUT_W, WC_OUTPUT_H,
                                &states);
    /* the xdg_surface.configure must follow; use an incremented serial */
    xdg_surface_send_configure(r, next_serial());
}
static void xdg_surface_get_popup(struct wl_client *c, struct wl_resource *r,
                                  uint32_t id, struct wl_resource *parent,
                                  struct wl_resource *positioner)
{ (void)c;(void)r;(void)id;(void)parent;(void)positioner; }
static void xdg_surface_set_window_geometry(struct wl_client *c,
                                            struct wl_resource *r,
                                            int32_t x, int32_t y,
                                            int32_t w, int32_t h)
{ (void)c;(void)r;(void)x;(void)y;(void)w;(void)h; }
static void xdg_surface_ack_configure(struct wl_client *c,
                                      struct wl_resource *r, uint32_t serial)
{
    (void)c; (void)serial;
    struct wcc_surface *surf = wl_resource_get_user_data(r);
    surf->configured = true;
    if (surf->server) {
        struct wcc_surface *a = surf->server->app;
        if (a == surf && surf->toplevel)
            xdg_surface_send_configure(r, next_serial()); /* steady-state */
    }
}
static const struct xdg_surface_interface xdg_surface_impl = {
    .destroy = xdg_surface_destroy,
    .get_toplevel = xdg_surface_get_toplevel,
    .get_popup = xdg_surface_get_popup,
    .set_window_geometry = xdg_surface_set_window_geometry,
    .ack_configure = xdg_surface_ack_configure,
};

/* --- xdg_toplevel --- */

//static const struct xdg_surface_interface xdg_surface_impl;

static void toplevel_destroy(struct wl_client *c, struct wl_resource *r)
{ (void)c; wl_resource_destroy(r); }
#define TL_IGNORE(name) \
    static void toplevel_##name(struct wl_client *c, struct wl_resource *r, uint32_t serial) \
    { (void)c; (void)r; (void)serial; }
static void tl_set_parent(struct wl_client *c, struct wl_resource *r,
                          struct wl_resource *parent)
{ (void)c;(void)r;(void)parent; }
static void tl_set_title(struct wl_client *c, struct wl_resource *r,
                         const char *title)
{ (void)c;(void)r;(void)title; }
static void tl_set_app_id(struct wl_client *c, struct wl_resource *r,
                          const char *app_id)
{ (void)c;(void)r;(void)app_id; }
static void tl_show_window_menu(struct wl_client *c, struct wl_resource *r,
                                struct wl_resource *seat, uint32_t serial,
                                int32_t x, int32_t y)
{ (void)c;(void)r;(void)seat;(void)serial;(void)x;(void)y; }
static void tl_move(struct wl_client *c, struct wl_resource *r,
                    struct wl_resource *seat, uint32_t serial)
{ (void)c;(void)r;(void)seat;(void)serial; }
static void tl_resize(struct wl_client *c, struct wl_resource *r,
                      struct wl_resource *seat, uint32_t serial,
                      uint32_t edges)
{ (void)c;(void)r;(void)seat;(void)serial;(void)edges; }
static void tl_set_max_size(struct wl_client *c, struct wl_resource *r,
                            int32_t w, int32_t h)
{ (void)c;(void)r;(void)w;(void)h; }
static void tl_set_min_size(struct wl_client *c, struct wl_resource *r,
                            int32_t w, int32_t h)
{ (void)c;(void)r;(void)w;(void)h; }
static void tl_set_maximized(struct wl_client *c, struct wl_resource *r)
{ (void)c; (void)r; }
static void tl_unset_maximized(struct wl_client *c, struct wl_resource *r)
{ (void)c; (void)r; }
static void tl_set_fullscreen(struct wl_client *c, struct wl_resource *r,
                              struct wl_resource *output)
{ (void)c;(void)r;(void)output; }
static void tl_unset_fullscreen(struct wl_client *c, struct wl_resource *r)
{ (void)c;(void)r; }
static void tl_set_minimized(struct wl_client *c, struct wl_resource *r)
{ (void)c;(void)r; }
static const struct xdg_toplevel_interface xdg_toplevel_impl = {
    .destroy = toplevel_destroy,
    .set_parent = tl_set_parent,
    .set_title = tl_set_title,
    .set_app_id = tl_set_app_id,
    .show_window_menu = tl_show_window_menu,
    .move = tl_move,
    .resize = tl_resize,
    .set_max_size = tl_set_max_size,
    .set_min_size = tl_set_min_size,
    .set_maximized = tl_set_maximized,
    .unset_maximized = tl_unset_maximized,
    .set_fullscreen = tl_set_fullscreen,
    .unset_fullscreen = tl_unset_fullscreen,
    .set_minimized = tl_set_minimized,
};

/* ==================== X11 backend: input dispatch ======================= */

static int wcc_x_error(Display *d, XErrorEvent *e)
{
    char buf[128];
    XGetErrorText(d, e->error_code, buf, sizeof(buf));
    fprintf(stderr, "[wcc] X error (req %d): %s\n", e->request_code, buf);
    return 0;   /* recover; never abort the compositor on an X error */
}

static int x11_dispatch(int fd, uint32_t mask, void *data)
{
    struct wcc_server *s = data;
    (void)fd; (void)mask;

    while (XPending(s->xd)) {
        XEvent ev;
        XNextEvent(s->xd, &ev);
        switch (ev.type) {
        case MotionNotify:
            s->seat.px = ev.xmotion.x;
            s->seat.py = ev.xmotion.y;
            if (s->app && s->seat.pointer_res) {
                if (s->seat.focused != s->app) {
                    wl_pointer_send_enter(s->seat.pointer_res, next_serial(),
                                          s->app->resource, s->seat.px, s->seat.py);
                    s->seat.focused = s->app;
                }
                wl_pointer_send_motion(s->seat.pointer_res, now_ms(),
                                       s->seat.px, s->seat.py);
                wl_pointer_send_frame(s->seat.pointer_res);
            }
            break;
        case ButtonPress:
        case ButtonRelease: {
            uint32_t btn; bool down = ev.type == ButtonPress;
            switch (ev.xbutton.button) {
            case 1: btn = BTN_LEFT; break;
            case 2: btn = BTN_MIDDLE; break;
            case 3: btn = BTN_RIGHT; break;
            default: continue;
            }
            if (s->seat.pointer_res) {
                wl_pointer_send_button(s->seat.pointer_res, next_serial(),
                                       now_ms(), btn, down ?
                                       WL_POINTER_BUTTON_STATE_PRESSED :
                                       WL_POINTER_BUTTON_STATE_RELEASED);
                wl_pointer_send_frame(s->seat.pointer_res);
            }
            break;
        }
        case KeyPress:
        case KeyRelease:
            if (s->seat.keyboard_res && s->app) {
                wl_keyboard_send_key(s->seat.keyboard_res, next_serial(),
                                     now_ms(),
                                     ev.xkey.keycode > 8 ? ev.xkey.keycode - 8 : 0,
                                     ev.type == KeyPress ?
                                     WL_KEYBOARD_KEY_STATE_PRESSED :
                                     WL_KEYBOARD_KEY_STATE_RELEASED);
            }
            break;
        case Expose:
            if (s->app && s->app->buf) blit(s, s->app);
            if (!s->xd_focused) {
                s->xd_focused = 1;
                XRaiseWindow(s->xd, s->xw);
                XSetInputFocus(s->xd, s->xw, RevertToParent, CurrentTime);
            }
            break;
        default:
            break;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    struct wcc_server *s = calloc(1, sizeof(*s));
    if (!s) return 1;

    /* X errors (e.g. a transient BadMatch) must never kill the compositor. */
    XSetErrorHandler(wcc_x_error);

    /* --- X output window --- */
    s->xd = XOpenDisplay(NULL);
    if (!s->xd) { fprintf(stderr, "WuBuCompositor: no X display\n"); return 1; }
    int scr = DefaultScreen(s->xd);
    s->xw = XCreateSimpleWindow(s->xd, RootWindow(s->xd, scr),
                                30, 30, WC_OUTPUT_W, WC_OUTPUT_H, 1,
                                BlackPixel(s->xd, scr), BlackPixel(s->xd, scr));
    XSelectInput(s->xd, s->xw, ExposureMask | KeyPressMask | KeyReleaseMask |
                 ButtonPressMask | ButtonReleaseMask | PointerMotionMask);
    XMapWindow(s->xd, s->xw);
    XDefineCursor(s->xd, s->xw, XCreateFontCursor(s->xd, XC_left_ptr));
    /* Focus is taken once the window is viewable (first Expose); setting it
     * here, right after the async map, raises BadMatch (window not yet
     * viewable) which Xlib's default handler made fatal. */
    XFlush(s->xd);

    s->ximg_owner = calloc((size_t)WC_OUTPUT_W * WC_OUTPUT_H * 4, 1);
    s->ximg = XCreateImage(s->xd, DefaultVisual(s->xd, scr), 24, ZPixmap, 0,
                           (char *)s->ximg_owner,
                           WC_OUTPUT_W, WC_OUTPUT_H, 32, WC_OUTPUT_W * 4);
    s->xfd = ConnectionNumber(s->xd);

    /* --- keymap for the client (US evdev, resolved client-side) --- */
    struct xkb_context *xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_rule_names rn = { 0 };
    struct xkb_keymap *km = xkb_keymap_new_from_names(xkb, &rn,
                                                      XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (km) s->xkb_keymap_str = xkb_keymap_get_as_string(km,
                                                         XKB_KEYMAP_FORMAT_TEXT_V1);

    /* --- wayland display --- */
    s->display = wl_display_create();
    if (!s->display) return 1;
    s->loop = wl_display_get_event_loop(s->display);

    wl_display_init_shm(s->display);
    wl_display_add_shm_format(s->display, WL_SHM_FORMAT_XRGB8888);
    wl_display_add_shm_format(s->display, WL_SHM_FORMAT_ARGB8888);

    wl_global_create(s->display, &wl_compositor_interface, 1, s, compositor_bind);
    wl_global_create(s->display, &wl_seat_interface, 1, s, seat_bind);
    wl_global_create(s->display, &wl_output_interface, 2, s, output_bind);
    wl_global_create(s->display, &xdg_wm_base_interface, 1, s, wm_bind);

    wl_event_loop_add_fd(s->loop, s->xfd, WL_EVENT_READABLE, x11_dispatch, s);

    const char *sock = wl_display_add_socket_auto(s->display);
    if (!sock) return 1;
    setenv("WAYLAND_DISPLAY", sock, 1);

    const char *start = getenv("WUBU_START_CMD");
    if (!start || !*start) start = "/home/wubu/wubuos/src/hosted/wubu";
    pid_t pid = fork();
    if (pid == 0) { execl("/bin/sh", "sh", "-c", start, (char *)NULL); _exit(127); }

    fprintf(stderr, "WuBuCompositor: socket=%s output=%dx%d\n",
            sock, WC_OUTPUT_W, WC_OUTPUT_H);
    wl_display_run(s->display);
    return 0;
}
