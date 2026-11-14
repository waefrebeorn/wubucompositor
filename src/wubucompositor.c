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
#include <X11/extensions/XShm.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include <xkbcommon/xkbcommon.h>

#include "wcc_monitors.h"
#include <sys/ipc.h>
#include <sys/shm.h>

/* Output size.  Defaults; main() overwrites them with the X screen size so
 * the compositor fills the panel -- this is the product surface, not a
 * developer window. */
static int WC_OUTPUT_W = 1024;
static int WC_OUTPUT_H = 768;
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

    /* What is actually attached.  Replaces the previous single hardcoded output
     * (geometry 0,0,0,0 and one fixed mode) with the real display's geometry, name,
     * physical size and full mode list -- which is what resolution support needs:
     * a guest can only CHOOSE from modes it was told about. */
    wcc_monitors_t monitors;

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

/* Track a held modifier key (X keycodes) so wl_keyboard.modifiers reflects
 * the real state -- the client relies on it for shortcuts and text. */
static void seat_mod_key(struct wcc_server *s, uint32_t keycode, bool down)
{
    uint32_t bit = 0;
    switch (keycode) {
    case 50:  case 62:  bit = WCC_MOD_SHIFT; break;  /* Shift_L / Shift_R */
    case 37:  case 105: bit = WCC_MOD_CTRL;  break;  /* Control_L / Control_R */
    case 64:  case 108: bit = WCC_MOD_ALT;   break;  /* Alt_L / Alt_R */
    case 133: case 134: bit = WCC_MOD_SUPER; break;  /* Super_L / Super_R */
    default: return;
    }
    if (down) s->seat.mods_depressed |=  bit;
    else      s->seat.mods_depressed &= ~bit;
}

static void seat_send_modifiers(struct wcc_server *s)
{
    if (!s->seat.keyboard_res) return;
    wl_keyboard_send_modifiers(s->seat.keyboard_res, next_serial(),
                               s->seat.mods_depressed, s->seat.mods_locked,
                               0 /* latched */, 0 /* group */);
}

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

/* --- GPU present --------------------------------------------------- */
/* The client renders 0x00RRGGBB into its shm buffer; in memory that is the
 * byte order B,G,R,00, which is exactly GL_BGRA.  Blending stays off, so the
 * unused alpha byte never reaches the panel. */

static void gpu_destroy(struct wcc_server *s)
{
    if (!s->gpu.ok) return;
    glXMakeCurrent(s->xd, None, NULL);
    if (s->gpu.ctx) glXDestroyContext(s->xd, s->gpu.ctx);
    if (s->gpu.tex) glDeleteTextures(1, &s->gpu.tex);
    free(s->gpu.last);
    if (s->gpu.vi) XFree(s->gpu.vi);
    memset(&s->gpu, 0, sizeof s->gpu);
}

static void gpu_setup(struct wcc_server *s)
{
    const int w = WC_OUTPUT_W, h = WC_OUTPUT_H;
    glViewport(0, 0, w, h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, w, h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);

    glGenTextures(1, &s->gpu.tex);
    glBindTexture(GL_TEXTURE_2D, s->gpu.tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA,
                 GL_UNSIGNED_BYTE, NULL);

    s->gpu.last = calloc((size_t)w * h * 4, 1);
    if (!s->gpu.last) { gpu_destroy(s); return; }
    s->gpu.ok = true;
}

static void gpu_present(struct wcc_server *s, const void *data,
                        int w, int h, int stride)
{
    if (!s->gpu.ok) return;
    glBindTexture(GL_TEXTURE_2D, s->gpu.tex);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, stride / 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_BGRA,
                    GL_UNSIGNED_BYTE, data);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    glEnable(GL_TEXTURE_2D);
    glColor4f(1.f, 1.f, 1.f, 1.f);
    glBegin(GL_QUADS);
        glTexCoord2f(0.f, 0.f); glVertex2f(0.f, 0.f);
        glTexCoord2f(1.f, 0.f); glVertex2f((GLfloat)w, 0.f);
        glTexCoord2f(1.f, 1.f); glVertex2f((GLfloat)w, (GLfloat)h);
        glTexCoord2f(0.f, 1.f); glVertex2f(0.f, (GLfloat)h);
    glEnd();
    glDisable(GL_TEXTURE_2D);
    glXSwapBuffers(s->xd, s->xw);
}

static void blit(struct wcc_server *s, struct wcc_surface *surf)
{
    if (!surf->buf || !s->xw) return;
    void *data = wl_shm_buffer_get_data(surf->buf);
    int w = wl_shm_buffer_get_width(surf->buf);
    int h = wl_shm_buffer_get_height(surf->buf);
    int stride = wl_shm_buffer_get_stride(surf->buf);
    if (w <= 0 || h <= 0 || stride < w * 4) return;
    /* The client renders at the output size; anything else is a stale buffer
     * from before a resize and must not be blitted. */
    if (w != WC_OUTPUT_W || h != WC_OUTPUT_H) return;
    size_t len = (size_t)stride * (size_t)h;

    /* GPU path: the last presented frame is our change detector, so an
     * unchanged surface costs one memcmp and no upload. */
    if (s->gpu.ok) {
        if (memcmp(data, s->gpu.last, len) == 0)
            return;
        memcpy(s->gpu.last, data, len);
        gpu_present(s, data, w, h, stride);
        return;
    }

    if (!s->ximg) return;
    if (w != s->ximg->width || h != s->ximg->height) return;
    char *dst = (char *)s->ximg->data;

    /* Present-on-change: if the buffer is byte-identical to what we last
     * showed, skip the put -- a repaint of an unchanged surface is what
     * reads as flashing on the panel.  The XImage backing store IS our
     * last-shown copy, so the compare is free. */
    if (memcmp(data, dst, len) == 0)
        return;
    memcpy(dst, data, len);

    if (s->shm_ok)
        XShmPutImage(s->xd, s->xw, DefaultGC(s->xd, 0), s->ximg,
                     0, 0, 0, 0, w, h, False);
    else
        XPutImage(s->xd, s->xw, DefaultGC(s->xd, 0), s->ximg,
                  0, 0, 0, 0, w, h);
    /* Flush, do not sync: XSync blocks the compositor until the X server
     * has processed every request, which serialises the frame and adds a
     * round trip of input latency to every repaint. */
    XFlush(s->xd);
}

static void wcc_capture_screenshot(struct wcc_server *s, int w, int h)
{
    XImage *im = XGetImage(s->xd, s->xw, 0, 0, (unsigned)w, (unsigned)h,
                           AllPlanes, ZPixmap);
    if (!im) { fprintf(stderr, "[wcc] screenshot: XGetImage failed\n"); return; }
    FILE *f = fopen(s->shot_path, "wb");
    if (!f) { fprintf(stderr, "[wcc] screenshot: cannot open %s\n", s->shot_path); }
    else {
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        unsigned char *row = (unsigned char *)im->data;
        int bpp = im->bits_per_pixel / 8;
        for (int y = 0; y < h; y++) {
            unsigned char *pix = row;
            for (int x = 0; x < w; x++) {
                unsigned v = (bpp >= 4) ? ((unsigned)pix[0]|((unsigned)pix[1]<<8)|
                                           ((unsigned)pix[2]<<16)|((unsigned)pix[3]<<24))
                                        : (unsigned)pix[0]|((unsigned)pix[1]<<8)|
                                          ((unsigned)pix[2]<<16);
                fputc((v>>16)&0xff, f);   /* R */
                fputc((v>>8)&0xff, f);    /* G */
                fputc(v&0xff, f);         /* B */
                pix += bpp;
            }
            row += im->bytes_per_line;
        }
        fclose(f);
        fprintf(stderr, "[wcc] screenshot: wrote %s (%dx%d)\n", s->shot_path, w, h);
    }
    XDestroyImage(im);
    s->shot_done = 1;
    wl_display_terminate(s->display);   /* exit cleanly after capture */
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
        /* self-screenshot: after a few settled commits, capture and stop. */
        if (s->shot_path && !s->shot_done) {
            s->shot_commits++;
            uint32_t now = now_ms();
            int due = s->shot_commits >= 4 &&
                      (int)(now - s->shot_start_ms) >= s->shot_delay_ms;
            if (due) wcc_capture_screenshot(s, WC_OUTPUT_W, WC_OUTPUT_H);
        }
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

/* Clear cached resources when the client destroys them.  Without these the
 * seat/output globals keep pointing at freed resources after a disconnect,
 * and the next event dereferences a NULL interface pointer -- observed as a
 * hard segfault (NULL+0x53) inside libwayland-server.  The surface objects
 * themselves are intentionally not freed here: the xdg resources share the
 * same user_data and destruction order is not guaranteed. */
static void wcc_resource_destroy(struct wl_resource *r)
{
    struct wcc_server *s = (struct wcc_server *)wl_resource_get_user_data(r);
    if (!s) return;
    if (s->seat.keyboard_res == r) s->seat.keyboard_res = NULL;
    if (s->seat.pointer_res   == r) s->seat.pointer_res   = NULL;
    if (s->seat_res           == r) s->seat_res           = NULL;
    if (s->output_res         == r) s->output_res         = NULL;
    if (s->wm_res             == r) s->wm_res             = NULL;
    if (s->compositor_res     == r) s->compositor_res     = NULL;
}

static void wcc_surface_destroy(struct wl_resource *r)
{
    struct wcc_surface *surf = (struct wcc_surface *)wl_resource_get_user_data(r);
    if (!surf || surf->resource != r) return;
    surf->resource  = NULL;
    surf->configured = false;
    surf->buf        = NULL;
    surf->pending_buffer = NULL;
    if (surf->server && surf->server->app == surf)
        surf->server->app = NULL;   /* the client's surface is gone */
}

static void wcc_xdg_destroy(struct wl_resource *r)
{
    struct wcc_surface *surf = (struct wcc_surface *)wl_resource_get_user_data(r);
    if (!surf) return;
    if (surf->toplevel == r) surf->toplevel = NULL;
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
    wl_resource_set_implementation(surf->resource, &surface_impl, surf,
                                   wcc_surface_destroy);
    if (!s->app) s->app = surf;
}

static const struct wl_compositor_interface compositor_impl = {
    .create_surface = compositor_create_surface,
    .create_region = compositor_create_region,
};
/* -- Refusal ---------------------------------------------------------
 * A second client gets these instead of the real implementations.  Every
 * request is silently ignored, so the intruder cannot create a surface, bind a
 * seat, or receive a single input event, and the owner is never disturbed. */
static void refused_no_op(struct wl_client *c, struct wl_resource *r)
{ (void)c; (void)r; }
static void refused_create_surface(struct wl_client *c, struct wl_resource *r,
                                   uint32_t id)
{ (void)c; (void)r; (void)id; }
static void refused_create_region(struct wl_client *c, struct wl_resource *r,
                                  uint32_t id)
{ (void)c; (void)r; (void)id; }
static const struct wl_compositor_interface refused_compositor_impl = {
    .create_surface = refused_create_surface,
    .create_region  = refused_create_region,
};

/* True when `c` is the desktop that owns this output. */
static bool wcc_is_owner(struct wcc_server *s, struct wl_client *c)
{
    return s->client != NULL && s->client == c;
}

/* Ownership must clear when the owning client goes away, or a perfectly good
 * replacement would be refused for ever after one bad launch. */
static void wcc_client_gone(struct wl_listener *listener, void *data)
{
    (void)data;
    struct wcc_server *s = wl_container_of(listener, s, client_destroy);
    s->client = NULL;
    fprintf(stderr, "WuBuCompositor: the desktop client disconnected\n");
}

static void compositor_bind(struct wl_client *c, void *data,
                            uint32_t version, uint32_t id)
{
    struct wcc_server *s = data;

    if (s->client && s->client != c) {
        /* Do NOT post an error and destroy the client here: tearing a client
         * down inside its own bind callback walks libwayland's resource list
         * while it is being mutated, and this compositor takes the segfault
         * (observed at NULL+0x4 in libwayland-server).  Binding the global to
         * a no-op implementation instead leaves the second client connected
         * but completely inert -- it can draw nothing and receives no input,
         * which is exactly the refusal we want, achieved without touching the
         * owner's state. */
        fprintf(stderr, "WuBuCompositor: refusing a second client "
                        "(one desktop owns this output)\n");
        struct wl_resource *res =
            wl_resource_create(c, &wl_compositor_interface, version, id);
        wl_resource_set_implementation(res, &refused_compositor_impl, s, NULL);
        return;
    }

    if (!s->client) {
        s->client = c;
        s->client_destroy.notify = wcc_client_gone;
        wl_client_add_destroy_listener(c, &s->client_destroy);
    }

    s->compositor_res = wl_resource_create(c, &wl_compositor_interface,
                                           version, id);
    wl_resource_set_implementation(s->compositor_res, &compositor_impl, s,
                                   wcc_resource_destroy);
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
    if (!wcc_is_owner(s, c))
        return;                 /* not the desktop: no pointer for you */
    s->seat.pointer_res = wl_resource_create(c, &wl_pointer_interface,
                                             wl_resource_get_version(r), id);
    wl_resource_set_implementation(s->seat.pointer_res, &pointer_impl, s,
                                   wcc_resource_destroy);
}
static void seat_get_keyboard(struct wl_client *c, struct wl_resource *r,
                              uint32_t id)
{
    struct wcc_server *s = wl_resource_get_user_data(r);
    if (!wcc_is_owner(s, c))
        return;                 /* not the desktop: no keyboard for you */
    s->seat.keyboard_res = wl_resource_create(c, &wl_keyboard_interface,
                                              wl_resource_get_version(r), id);
    wl_resource_set_implementation(s->seat.keyboard_res, &keyboard_impl, s,
                                   wcc_resource_destroy);

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
            seat_send_modifiers(s);
            if (s->app && s->seat.pointer_res) {
                wl_pointer_send_enter(s->seat.pointer_res, next_serial(),
                                      s->app->resource,
                                      wl_fixed_from_double(s->seat.px),
                                      wl_fixed_from_double(s->seat.py));
                s->seat.focused = s->app;
            }
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
    struct wl_resource *res =
        wl_resource_create(c, &wl_seat_interface, version, id);

    if (!wcc_is_owner(s, c)) {
        /* Exists only so the bind completes.  It advertises no capabilities
         * and must NOT take s->seat_res away from the desktop that owns the
         * output -- overwriting it here was how the two clients ended up
         * sharing one seat. */
        wl_resource_set_implementation(res, &seat_impl, s, NULL);
        return;
    }

    s->seat_res = res;
    wl_resource_set_implementation(res, &seat_impl, s, wcc_resource_destroy);
    wl_seat_send_capabilities(res,
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
    wl_resource_set_implementation(s->output_res, &output_impl, NULL,
                                   wcc_resource_destroy);

    /* Prefer the REAL primary display.  Discovery can fail (no X server at all), and
     * then we fall back to the compiled-in default rather than advertising nothing --
     * but never to invented geometry that claims a monitor exists where none does. */
    const wcc_monitor_t *m = (s->monitors.count > 0) ? &s->monitors.m[0] : NULL;

    wl_output_send_geometry(s->output_res,
                            m ? m->x : 0, m ? m->y : 0,
                            m ? m->mm_w : 0, m ? m->mm_h : 0,
                            WL_OUTPUT_SUBPIXEL_UNKNOWN,
                            m ? m->name : "", "",
                            WL_OUTPUT_TRANSFORM_NORMAL);

    if (m && m->nmodes > 0) {
        /* Advertise EVERY mode, flagging the live one.  This is the difference between
         * a resolution readout and resolution SUPPORT: with only the current mode a
         * client has nothing to choose. */
        for (int i = 0; i < m->nmodes; i++) {
            uint32_t flags = 0;
            if (m->modes[i].current) flags |= WL_OUTPUT_MODE_CURRENT;
            /* The first entry is XRandR's preferred mode. */
            if (i == 0) flags |= WL_OUTPUT_MODE_PREFERRED;
            wl_output_send_mode(s->output_res, flags,
                                m->modes[i].w, m->modes[i].h,
                                m->modes[i].refresh_mhz * 1000);
        }
    } else {
        wl_output_send_mode(s->output_res, WL_OUTPUT_MODE_CURRENT |
                            WL_OUTPUT_MODE_PREFERRED,
                            WC_OUTPUT_W, WC_OUTPUT_H, 60000);
    }

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
static void pos_ignore_rect(struct wl_client *c, struct wl_resource *r,
                            int32_t a, int32_t b, int32_t d, int32_t e)
{ (void)c;(void)r;(void)a;(void)b;(void)d;(void)e; }
static void pos_ignore_uint(struct wl_client *c, struct wl_resource *r,
                            uint32_t a)
{ (void)c;(void)r;(void)a; }
static void pos_set_reactive(struct wl_client *c, struct wl_resource *r)
{ (void)c;(void)r; }
static const struct xdg_positioner_interface positioner_impl = {
    .destroy = pos_destroy,
    .set_size = pos_ignore,
    .set_anchor_rect = pos_ignore_rect,
    .set_anchor = pos_ignore_uint,
    .set_gravity = pos_ignore_uint,
    .set_constraint_adjustment = pos_ignore_uint,
    .set_offset = pos_ignore,
    .set_reactive = pos_set_reactive,
    .set_parent_size = pos_ignore,
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
    wl_resource_set_implementation(res, &xdg_surface_impl, surf,
                                   wcc_xdg_destroy);
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
    wl_resource_set_implementation(s->wm_res, &wm_impl, s,
                                   wcc_resource_destroy);
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
    wl_resource_set_implementation(surf->toplevel, &xdg_toplevel_impl, surf,
                                   wcc_xdg_destroy);
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
                                          s->app->resource,
                                          wl_fixed_from_double(s->seat.px),
                                          wl_fixed_from_double(s->seat.py));
                    s->seat.focused = s->app;
                }
                wl_pointer_send_motion(s->seat.pointer_res, now_ms(),
                                       wl_fixed_from_double(s->seat.px),
                                       wl_fixed_from_double(s->seat.py));
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
                /* A click with no prior motion must still carry the pointer
                 * position, or the client sees the press at (0,0). */
                if (s->seat.focused != s->app && s->app) {
                    wl_pointer_send_enter(s->seat.pointer_res, next_serial(),
                                          s->app->resource,
                                          wl_fixed_from_double(s->seat.px),
                                          wl_fixed_from_double(s->seat.py));
                    s->seat.focused = s->app;
                }
                wl_pointer_send_button(s->seat.pointer_res, next_serial(),
                                       now_ms(), btn, down ?
                                       WL_POINTER_BUTTON_STATE_PRESSED :
                                       WL_POINTER_BUTTON_STATE_RELEASED);
                wl_pointer_send_frame(s->seat.pointer_res);
            }
            break;
        }
        case KeyPress:
        case KeyRelease: {
            bool down = ev.type == KeyPress;
            seat_mod_key(s, ev.xkey.keycode, down);
            if (s->seat.keyboard_res && s->app) {
                wl_keyboard_send_key(s->seat.keyboard_res, next_serial(),
                                     now_ms(),
                                     ev.xkey.keycode > 8 ? ev.xkey.keycode - 8 : 0,
                                     down ?
                                     WL_KEYBOARD_KEY_STATE_PRESSED :
                                     WL_KEYBOARD_KEY_STATE_RELEASED);
                /* Modifiers follow the key so the client's view of the
                 * keyboard state is never stale. */
                seat_send_modifiers(s);
            }
            break;
        }
        case LeaveNotify:
            if (s->seat.pointer_res && s->seat.focused) {
                wl_pointer_send_leave(s->seat.pointer_res, next_serial(),
                                      s->seat.focused->resource);
                s->seat.focused = NULL;
            }
            break;
        case Expose:
            if (s->app && s->app->buf) blit(s, s->app);
            if (!s->xd_focused) {
                s->xd_focused = 1;
                XRaiseWindow(s->xd, s->xw);
                XSetInputFocus(s->xd, s->xw, RevertToParent, CurrentTime);
                /* Take the keyboard as well.  A host window manager installs
                 * a passive grab on the Super press, so without this the
                 * compositor never sees Win+W / Win+Z / Win+L -- only the
                 * release arrives. */
                XGrabKeyboard(s->xd, s->xw, True, GrabModeAsync,
                              GrabModeAsync, CurrentTime);
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
    const char *shot_path = NULL;
    int shot_delay = 0;
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--screenshot") == 0) shot_path = argv[i + 1];
        else if (strcmp(argv[i], "--shot-delay") == 0)
            shot_delay = atoi(argv[i + 1]);
    }

    struct wcc_server *s = calloc(1, sizeof(*s));
    if (shot_path) {
        s->shot_path = shot_path;
        s->shot_delay_ms = shot_delay > 0 ? shot_delay : 0;
        s->shot_start_ms = now_ms();
        fprintf(stderr,
                "WuBuCompositor: --screenshot %s delay=%d ms (self-capture)\n",
                shot_path, s->shot_delay_ms);
    }
    if (!s) return 1;

    /* X errors (e.g. a transient BadMatch) must never kill the compositor. */
    XSetErrorHandler(wcc_x_error);

    /* --- X output window: fullscreen, override-redirect (no WM chrome) --- */
    s->xd = XOpenDisplay(NULL);
    if (!s->xd) { fprintf(stderr, "WuBuCompositor: no X display\n"); return 1; }
    int scr = DefaultScreen(s->xd);
    WC_OUTPUT_W = WidthOfScreen(ScreenOfDisplay(s->xd, scr));
    WC_OUTPUT_H = HeightOfScreen(ScreenOfDisplay(s->xd, scr));

    XSetWindowAttributes swa = { 0 };
    swa.override_redirect = True;
    swa.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask |
                     ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                     LeaveWindowMask | EnterWindowMask;

    /* --- GPU present: pick a GLX-capable visual for the output window.
     * Without this the window keeps CopyFromParent and glXMakeCurrent on it
     * fails, which would silently drop us back to the CPU blit. --- */
    int gl_attribs[] = {
        GLX_RGBA, True,
        GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8,
        GLX_DOUBLEBUFFER, True,
        None
    };
    s->gpu.vi = glXChooseVisual(s->xd, scr, gl_attribs);
    if (s->gpu.vi) {
        s->gpu.visual = s->gpu.vi->visual;
        s->gpu.depth  = s->gpu.vi->depth;
    }
    unsigned long swa_mask = CWOverrideRedirect | CWEventMask;
    if (s->gpu.visual) {
        swa.colormap = XCreateColormap(s->xd, RootWindow(s->xd, scr),
                                      s->gpu.visual, AllocNone);
        swa_mask |= CWColormap;
    }
    s->xw = XCreateWindow(s->xd, RootWindow(s->xd, scr), 0, 0,
                          WC_OUTPUT_W, WC_OUTPUT_H, 0,
                          s->gpu.depth ? s->gpu.depth : CopyFromParent,
                          InputOutput,
                          s->gpu.visual ? s->gpu.visual : CopyFromParent,
                          swa_mask, &swa);
    XMapWindow(s->xd, s->xw);
    XStoreName(s->xd, s->xw, "WuBuCompositor");
    XDefineCursor(s->xd, s->xw, XCreateFontCursor(s->xd, XC_left_ptr));
    /* Focus is taken once the window is viewable (first Expose); setting it
     * here, right after the async map, raises BadMatch (window not yet
     * viewable) which Xlib's default handler made fatal. */
    XFlush(s->xd);

    /* --- GPU present: create the GLX context on the output window --- */
    if (s->gpu.vi) {
        s->gpu.ctx = glXCreateContext(s->xd, s->gpu.vi, NULL, True);
        if (s->gpu.ctx && glXMakeCurrent(s->xd, s->xw, s->gpu.ctx)) {
            gpu_setup(s);
            fprintf(stderr, "WuBuCompositor: GPU present active (GLX)\n");
        } else {
            fprintf(stderr, "WuBuCompositor: GLX unavailable, CPU blit\n");
            if (s->gpu.ctx) { glXDestroyContext(s->xd, s->gpu.ctx); s->gpu.ctx = NULL; }
            XFree(s->gpu.vi); s->gpu.vi = NULL; s->gpu.visual = NULL;
        }
    }

    /* Shm blit path: one copy into our own segment, then a zero-copy put.
     * Only needed when the GPU present is unavailable; falls back to a malloc
     * XImage when MIT-SHM is missing. */
    if (!s->gpu.ok) {
    if (XShmQueryExtension(s->xd)) {
        s->ximg = XShmCreateImage(s->xd, DefaultVisual(s->xd, scr),
                                  DefaultDepth(s->xd, scr), ZPixmap, NULL,
                                  &s->shm, WC_OUTPUT_W, WC_OUTPUT_H);
        if (s->ximg) {
            s->shm.shmid = shmget(IPC_PRIVATE,
                                  s->ximg->bytes_per_line * s->ximg->height,
                                  IPC_CREAT | 0777);
            if (s->shm.shmid >= 0) {
                s->shm.shmaddr = (char *)shmat(s->shm.shmid, NULL, 0);
                if (s->shm.shmaddr != (char *)-1) {
                    s->ximg->data = s->shm.shmaddr;
                    s->shm.readOnly = False;
                    if (XShmAttach(s->xd, &s->shm)) {
                        s->shm_ok = true;
                    } else {
                        shmdt(s->shm.shmaddr);
                        shmctl(s->shm.shmid, IPC_RMID, NULL);
                    }
                } else {
                    shmctl(s->shm.shmid, IPC_RMID, NULL);
                }
            }
        }
    }
    if (!s->shm_ok) {
        s->ximg_owner = calloc((size_t)WC_OUTPUT_W * WC_OUTPUT_H * 4, 1);
        s->ximg = XCreateImage(s->xd, DefaultVisual(s->xd, scr),
                               DefaultDepth(s->xd, scr), ZPixmap, 0,
                               (char *)s->ximg_owner,
                               WC_OUTPUT_W, WC_OUTPUT_H, 32, WC_OUTPUT_W * 4);
    }
    if (!s->ximg) { fprintf(stderr, "WuBuCompositor: no XImage\n"); return 1; }
    }
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
    /* Discover displays BEFORE creating the output global, so the first bind
     * already carries real geometry and modes. */
    if (wcc_monitors_discover(&s->monitors) < 0) {
        fprintf(stderr, "[wcc] no display enumerated; falling back to %dx%d\n",
                WC_OUTPUT_W, WC_OUTPUT_H);
    } else {
        fprintf(stderr, "[wcc] display: %s %dx%d@%dHz, %d mode(s)\n",
                s->monitors.m[0].name,
                s->monitors.m[0].w, s->monitors.m[0].h,
                s->monitors.m[0].refresh_mhz, s->monitors.m[0].nmodes);
    }

    wl_global_create(s->display, &wl_output_interface, 2, s, output_bind);
    wl_global_create(s->display, &xdg_wm_base_interface, 1, s, wm_bind);

    wl_event_loop_add_fd(s->loop, s->xfd, WL_EVENT_READABLE, x11_dispatch, s);

    const char *sock = wl_display_add_socket_auto(s->display);
    if (!sock) return 1;
    setenv("WAYLAND_DISPLAY", sock, 1);

    /* The guest is spawned here so `./wubucompositor` alone brings up the
     * desktop.  WUBU_NO_SPAWN=1 skips that, which is what you want when you are
     * starting the guest yourself -- spawning a second one only produced two
     * clients fighting over one seat. */
    const char *no_spawn = getenv("WUBU_NO_SPAWN");
    if (!(no_spawn && *no_spawn && no_spawn[0] != '0')) {
        const char *start = getenv("WUBU_START_CMD");
        if (!start || !*start) start = "/home/wubu/wubuos/src/hosted/wubu";
        pid_t pid = fork();
        if (pid == 0) { execl("/bin/sh", "sh", "-c", start, (char *)NULL); _exit(127); }
    } else {
        fprintf(stderr, "WuBuCompositor: WUBU_NO_SPAWN set; waiting for a client\n");
    }

    fprintf(stderr, "WuBuCompositor: socket=%s output=%dx%d\n",
            sock, WC_OUTPUT_W, WC_OUTPUT_H);
    wl_display_run(s->display);
    return 0;
}
