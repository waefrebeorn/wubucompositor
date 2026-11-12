/*
 * WuBuCompositor -- the WuBuOS compositor.
 *
 * Replaces weston as the Wayland compositor under which the wubu hosted
 * client runs fullscreen. Built on wlroots (the library layering -- not a
 * compositor itself; sway is a wlroots compositor, and so is this one).
 *
 * Design for this purpose:
 *   - ONE client, fullscreen, pinned to the output origin. Pointer and
 *     keyboard input pass straight through to it (no tiling needed).
 *   - Rendering through wlr_scene + wlr_scene_output, which is damage-
 *     driven: only changed regions repaint. The weston behaviour of
 *     repainting everything every frame (read as flashing on the panel)
 *     is gone by construction.
 *   - Software (pixman) renderer by default: pixel-exact, no EGL.
 *
 * Backends: WLR_BACKENDS=x11 runs nested inside an existing X session
 * (development). WLR_BACKENDS=drm boots it directly on the panel.
 *
 * C11.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include <wayland-server-core.h>

#include <wlr/backend.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/allocator.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

#define WC_START_CMD "/home/wubu/wubuos/src/hosted/wubu"

struct wc_server {
    struct wl_display *display;
    struct wlr_backend *backend;
    struct wlr_renderer *renderer;
    struct wlr_allocator *allocator;

    struct wlr_scene *scene;
    struct wlr_output_layout *layout;
    struct wlr_scene_output_layout *scene_layout;
    struct wlr_cursor *cursor;
    struct wlr_seat *seat;
    struct wlr_xcursor_manager *xcursor;
    struct xkb_context *xkb;

    /* the whole desktop is ONE fullscreen client */
    struct wlr_xdg_surface *app;
    struct wlr_scene_tree *app_tree;
    bool app_mapped;

    /* compositor cursor: an always-visible box that follows the pointer */
    struct wlr_scene_rect *cursor_rect;

    struct wl_listener new_output;
    struct wl_listener new_input;
    struct wl_listener new_xdg_surface;
    struct wl_listener app_commit;
    struct wl_listener cursor_motion;
    struct wl_listener cursor_button;
    struct wl_listener cursor_axis;
    struct wl_listener cursor_frame;
    struct wl_listener keyboard_key;
    struct wl_listener keyboard_modifiers;

    const char *start_cmd;
};

/* ---- focus -------------------------------------------------------------- */

static void focus_app(struct wc_server *s)
{
    if (!s->app || !s->app_mapped) return;
    struct wlr_surface *surface = s->app->surface;
    if (!surface) return;

    struct wlr_keyboard *kb = wlr_seat_get_keyboard(s->seat);
    const struct wlr_keyboard_modifiers *mods =
        kb ? &kb->modifiers : NULL;
    wlr_seat_keyboard_notify_enter(s->seat, surface, NULL, 0, mods);
    if (s->cursor->x >= 0 && s->cursor->y >= 0)
        wlr_seat_pointer_notify_enter(s->seat, surface,
                                      s->cursor->x, s->cursor->y);
}

/* ---- xdg shell ----------------------------------------------------------- */

static void new_xdg_surface(struct wl_listener *listener, void *data)
{
    struct wc_server *s = wl_container_of(listener, s, new_xdg_surface);
    struct wlr_xdg_surface *xdg = (struct wlr_xdg_surface *)data;

    if (xdg->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL) return;

    /* One app only: a second toplevel is not our desktop. Drop it. */
    if (s->app) {
        wlr_xdg_toplevel_set_size(xdg->toplevel, 0, 0);
        wlr_xdg_surface_schedule_configure(xdg);
        return;
    }

    s->app = xdg;
    s->app_tree = wlr_scene_xdg_surface_create(&s->scene->tree, xdg);
    if (!s->app_tree) {
        s->app = NULL;
        return;
    }

    wlr_xdg_toplevel_set_fullscreen(xdg->toplevel, true);

    /* The scene tree maps the surface asynchronously; (re-)raise and focus
     * once it is actually mapped so the window is on top and input reaches
     * it. */
    wl_signal_add(&xdg->surface->events.commit, &s->app_commit);
}

static void app_committed(struct wl_listener *listener, void *data)
{
    struct wc_server *s = wl_container_of(listener, s, app_commit);
    (void)data;
    if (!s->app) return;
    if (!s->app_mapped && s->app->surface->mapped) {
        s->app_mapped = true;
        wlr_scene_node_raise_to_top(&s->app_tree->node);
        focus_app(s);
        wlr_log(WLR_INFO, "WuBuOS client fullscreen and focused");
    }
}

/* ---- input --------------------------------------------------------------- */

static void cursor_to_renderer(struct wc_server *s)
{
    if (!s->cursor_rect) return;
    wlr_scene_node_set_position(&s->cursor_rect->node,
                                s->cursor->x, s->cursor->y);
}

static void cursor_motion(struct wl_listener *listener, void *data)
{
    struct wc_server *s = wl_container_of(listener, s, cursor_motion);
    struct wlr_pointer_motion_event *ev =
        (struct wlr_pointer_motion_event *)data;

    wlr_cursor_move(s->cursor, NULL, ev->delta_x, ev->delta_y);
    cursor_to_renderer(s);

    if (s->app_mapped)
        wlr_seat_pointer_notify_motion(s->seat, ev->time_msec,
                                       s->cursor->x, s->cursor->y);
}

static void cursor_motion_absolute(struct wl_listener *listener, void *data)
{
    /* wlr_cursor absorbs absolute motion via wlr_cursor_attach_input_device;
     * nothing to add here. */
    (void)listener; (void)data;
}

static void cursor_button(struct wl_listener *listener, void *data)
{
    struct wc_server *s = wl_container_of(listener, s, cursor_button);
    struct wlr_pointer_button_event *ev =
        (struct wlr_pointer_button_event *)data;
    if (s->app_mapped)
        wlr_seat_pointer_notify_button(s->seat, ev->time_msec,
                                       ev->button, ev->state);
}

static void cursor_axis(struct wl_listener *listener, void *data)
{
    struct wc_server *s = wl_container_of(listener, s, cursor_axis);
    struct wlr_pointer_axis_event *ev =
        (struct wlr_pointer_axis_event *)data;
    if (s->app_mapped)
        wlr_seat_pointer_notify_axis(s->seat, ev->time_msec, ev->orientation,
                                     ev->delta, ev->delta_discrete,
                                     ev->source);
}

static void cursor_frame(struct wl_listener *listener, void *data)
{
    struct wc_server *s = wl_container_of(listener, s, cursor_frame);
    (void)data;
    if (s->app_mapped)
        wlr_seat_pointer_notify_frame(s->seat);
}

static void keyboard_key(struct wl_listener *listener, void *data)
{
    struct wc_server *s = wl_container_of(listener, s, keyboard_key);
    struct wlr_keyboard_key_event *ev =
        (struct wlr_keyboard_key_event *)data;
    wlr_seat_keyboard_notify_key(s->seat, ev->time_msec,
                                 ev->keycode, ev->state);
}

static void keyboard_modifiers(struct wl_listener *listener, void *data)
{
    struct wc_server *s = wl_container_of(listener, s, keyboard_modifiers);
    struct wlr_keyboard_modifiers *modifiers =
        (struct wlr_keyboard_modifiers *)data;
    wlr_seat_keyboard_notify_modifiers(s->seat, modifiers);
}

static void new_input(struct wl_listener *listener, void *data)
{
    struct wc_server *s = wl_container_of(listener, s, new_input);
    struct wlr_input_device *dev = (struct wlr_input_device *)data;

    switch (dev->type) {
    case WLR_INPUT_DEVICE_KEYBOARD: {
        struct wlr_keyboard *kb = wlr_keyboard_from_input_device(dev);
        wlr_seat_set_keyboard(s->seat, kb);
        struct xkb_rule_names rules = { 0 };
        struct xkb_keymap *keymap = xkb_keymap_new_from_names(
            s->xkb, &rules, XKB_KEYMAP_COMPILE_NO_FLAGS);
        if (keymap) {
            wlr_keyboard_set_keymap(kb, keymap);
            xkb_keymap_unref(keymap);
        }
        wlr_keyboard_set_repeat_info(kb, 25, 600);
        wl_signal_add(&kb->events.key, &s->keyboard_key);
        wl_signal_add(&kb->events.modifiers, &s->keyboard_modifiers);
        focus_app(s);
        break;
    }
    case WLR_INPUT_DEVICE_POINTER:
    case WLR_INPUT_DEVICE_TOUCH:
        wlr_cursor_attach_input_device(s->cursor, dev);
        break;
    default:
        break;
    }
}

/* ---- output -------------------------------------------------------------- */

static void new_output(struct wl_listener *listener, void *data)
{
    struct wc_server *s = wl_container_of(listener, s, new_output);
    struct wlr_output *output = (struct wlr_output *)data;

    struct wlr_output_state st = { 0 };
    wlr_output_state_set_enabled(&st, true);
    struct wlr_output_mode *mode = wlr_output_preferred_mode(output);
    if (mode)
        wlr_output_state_set_mode(&st, mode);
    if (!wlr_output_commit_state(output, &st))
        wlr_log(WLR_ERROR, "output commit failed output=%s", output->name);

    struct wlr_output_layout_output *lo =
        wlr_output_layout_add(s->layout, output, 0, 0);
    struct wlr_scene_output *so = wlr_scene_output_create(s->scene, output);
    if (so && lo)
        wlr_scene_output_layout_add_output(s->scene_layout, lo, so);
}

/* ---- main ---------------------------------------------------------------- */

int main(int argc, char **argv)
{
    const char *start_cmd = getenv("WUBU_START_CMD");
    if (!start_cmd || !*start_cmd) start_cmd = WC_START_CMD;

    wlr_log_init(WLR_INFO, NULL);

    struct wc_server s = { 0 };
    s.start_cmd = start_cmd;
    s.xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!s.xkb) { wlr_log(WLR_ERROR, "no xkb"); return 1; }

    s.display = wl_display_create();
    if (!s.display) { wlr_log(WLR_ERROR, "no display"); return 1; }

    s.backend = wlr_backend_autocreate(s.display, NULL);
    if (!s.backend) { wlr_log(WLR_ERROR, "no backend"); return 1; }

    s.renderer = wlr_renderer_autocreate(s.backend);
    if (!s.renderer) { wlr_log(WLR_ERROR, "no renderer"); return 1; }
    wlr_renderer_init_wl_display(s.renderer, s.display);

    s.allocator = wlr_allocator_autocreate(s.backend, s.renderer);
    if (!s.allocator) { wlr_log(WLR_ERROR, "no allocator"); return 1; }

    wlr_compositor_create(s.display, 6, s.renderer);
    wlr_subcompositor_create(s.display);
    wlr_data_device_manager_create(s.display);

    s.scene = wlr_scene_create();
    s.layout = wlr_output_layout_create();
    s.scene_layout = wlr_scene_attach_output_layout(s.scene, s.layout);
    s.cursor = wlr_cursor_create();
    s.seat = wlr_seat_create(s.display, "seat0");
    s.xcursor = wlr_xcursor_manager_create(NULL, 24);

    wlr_cursor_attach_output_layout(s.cursor, s.layout);

    /* an always-visible cursor box until the client takes over via
     * wl_pointer.set_cursor */
    s.cursor_rect = wlr_scene_rect_create(&s.scene->tree, 2, 2,
                                          (float[4]){ 0.0f, 1.0f, 0.2f, 1.0f });
    wlr_scene_node_set_position(&s.cursor_rect->node, 0, 0);

    struct wlr_xdg_shell *shell = wlr_xdg_shell_create(s.display, 6);

    s.new_output.notify = new_output;
    wl_signal_add(&s.backend->events.new_output, &s.new_output);
    s.new_input.notify = new_input;
    wl_signal_add(&s.backend->events.new_input, &s.new_input);
    s.new_xdg_surface.notify = new_xdg_surface;
    wl_signal_add(&shell->events.new_surface, &s.new_xdg_surface);
    s.app_commit.notify = app_committed;
    /* (registered when the first xdg surface arrives) */
    s.cursor_motion.notify = cursor_motion;
    wl_signal_add(&s.cursor->events.motion, &s.cursor_motion);
    s.cursor_button.notify = cursor_button;
    wl_signal_add(&s.cursor->events.button, &s.cursor_button);
    s.cursor_axis.notify = cursor_axis;
    wl_signal_add(&s.cursor->events.axis, &s.cursor_axis);
    s.cursor_frame.notify = cursor_frame;
    wl_signal_add(&s.cursor->events.frame, &s.cursor_frame);

    /* Spawn the client; it inherits WAYLAND_DISPLAY on this socket. */
    const char *socket = wl_display_add_socket_auto(s.display);
    if (!socket) {
        wlr_log(WLR_ERROR, "cannot create wayland socket");
        return 1;
    }
    setenv("WAYLAND_DISPLAY", socket, 1);

    pid_t pid = fork();
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", s.start_cmd, (char *)NULL);
        _exit(127);
    }
    (void)argc; (void)argv;

    if (!wlr_backend_start(s.backend)) {
        wlr_log(WLR_ERROR, "failed to start backend");
        return 1;
    }

    wlr_log(WLR_INFO, "WuBuCompositor running");

    wl_display_run(s.display);
    wl_display_destroy(s.display);
    return 0;
}
