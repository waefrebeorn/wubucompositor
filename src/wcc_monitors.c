/*
 * wcc_monitors.c -- XRandR discovery for the compositor.  See wcc_monitors.h.
 *
 * One concern: turn "what is plugged into this machine" into plain descriptors.
 * Deliberately no Wayland types and no rendering -- the wl_output events are emitted
 * elsewhere from this list, so the geometry here can be tested without a client.
 */
#include "wcc_monitors.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>

int wcc_monitors_discover(wcc_monitors_t *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) return -1;                 /* headless: nothing honest to report */

    int n = 0;
    Window root = DefaultRootWindow(dpy);

    /* The primary output is a property of the SCREEN, not of XRROutputInfo -- asking
     * per-output for it silently yields false on every display. */
    XID primary_output = XRRGetOutputPrimary(dpy, root);

    XRRScreenResources *res = XRRGetScreenResources(dpy, root);
    if (res) {
        for (int i = 0; i < res->noutput && n < WCC_MAX_MONITORS; i++) {
            XRROutputInfo *oi = XRRGetOutputInfo(dpy, res, res->outputs[i]);
            if (!oi) continue;

            /* Connection state is the authoritative signal.  Requiring a CRTC as well
             * is wrong: a connected, configured output can legitimately expose no CRTC
             * handle to a second X client (verified on this machine -- eDP-1 reports
             * RR_Connected with crtc == None while driving the desktop), and demanding
             * one hides the only real monitor. */
            bool connected = (oi->connection == RR_Connected);

            if (connected) {
                wcc_monitor_t *m = &out->m[n];
                memset(m, 0, sizeof(*m));
                snprintf(m->name, sizeof(m->name), "%s", oi->name);
                m->mm_w = (int)oi->mm_width;
                m->mm_h = (int)oi->mm_height;

                XRRCrtcInfo *ci = (oi->crtc != None)
                                      ? XRRGetCrtcInfo(dpy, res, oi->crtc)
                                      : NULL;

                if (ci) {
                    m->x = ci->x;
                    m->y = ci->y;
                    m->w = (int)ci->width;
                    m->h = (int)ci->height;

                    for (int k = 0; k < res->nmode; k++) {
                        if (res->modes[k].id == (XID)ci->mode) {
                            m->refresh_mhz =
                                (int)(res->modes[k].dotClock * 1000.0 /
                                      ((double)res->modes[k].hTotal *
                                       (double)res->modes[k].vTotal));
                            break;
                        }
                    }
                } else {
                    /* No CRTC visible: fall back to the output's FIRST mode, which
                     * XRandR orders preferred-first.  Position is unknown, so the
                     * origin is the least surprising claim and is overwritten the
                     * moment a CRTC becomes visible. */
                    /* oi->modes[] holds XRandR MODE IDS, not dimensions -- treating
                     * them as widths produced absurd resolutions that still passed a
                     * "> 0" check.  Resolve the id through the screen's mode table. */
                    if (oi->nmode > 0) {
                        m->x = 0;
                        m->y = 0;
                        for (int k = 0; k < res->nmode; k++) {
                            if (res->modes[k].id == (XID)oi->modes[0]) {
                                m->w = (int)res->modes[k].width;
                                m->h = (int)res->modes[k].height;
                                m->refresh_mhz =
                                    (int)(res->modes[k].dotClock * 1000.0 /
                                          ((double)res->modes[k].hTotal *
                                           (double)res->modes[k].vTotal));
                                break;
                            }
                        }
                    }
                }

                /* Every mode this output offers, resolved from ids through the
                 * screen's mode table.  Order is kept as XRandR presents it, which is
                 * preferred-first -- the same order a resolution menu should show. */
                for (int mi = 0; mi < oi->nmode && m->nmodes < WCC_MAX_MODES; mi++) {
                    for (int k = 0; k < res->nmode; k++) {
                        if (res->modes[k].id != (XID)oi->modes[mi]) continue;

                        wcc_mode_t *mode = &m->modes[m->nmodes];
                        mode->w          = (int)res->modes[k].width;
                        mode->h          = (int)res->modes[k].height;
                        mode->refresh_mhz =
                            (int)(res->modes[k].dotClock * 1000.0 /
                                  ((double)res->modes[k].hTotal *
                                   (double)res->modes[k].vTotal));
                        mode->current = (ci && ci->mode == res->modes[k].id);
                        m->nmodes++;
                        break;
                    }
                }

                /* When no CRTC was visible we could not observe WHICH mode is live,
                 * so nothing was flagged above.  The resolution we reported comes from
                 * modes[0], so flagging that same entry keeps the descriptor honest --
                 * a monitor with zero current modes tells a client "nothing is
                 * running", which is false and leaves a menu with no marked choice. */
                if (m->nmodes > 0) {
                    bool any_current = false;
                    for (int k = 0; k < m->nmodes; k++)
                        if (m->modes[k].current) { any_current = true; break; }
                    if (!any_current) m->modes[0].current = true;
                }

                m->primary   = (res->outputs[i] == primary_output);
                m->connected = true;
                n++;

                if (ci) XRRFreeCrtcInfo(ci);
            }
            XRRFreeOutputInfo(oi);
        }
        XRRFreeScreenResources(res);
    }

    XCloseDisplay(dpy);

    /* If XRandR gave us nothing usable, say so rather than handing back a list of
     * zero-sized entries -- the old hardcoded path did exactly that and clients had
     * no way to tell a real display from a placeholder. */
    if (n == 0) { memset(out, 0, sizeof(*out)); return -1; }

    out->count = n;
    return n;
}
