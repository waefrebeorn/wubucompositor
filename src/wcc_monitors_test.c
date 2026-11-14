/*
 * wcc_monitors_test.c -- gate for display discovery.
 *
 * The most valuable assertion here is one that can run on real hardware: when an X
 * display is reachable, discovery must report exactly the CONNECTED monitors that
 * `xrandr -q` lists, with sane geometry.  That catches the two failure modes that
 * matter -- advertising a disconnected port (a display that cannot show anything) or
 * inventing a placeholder when there is nothing to enumerate (what the old hardcoded
 * output did).
 *
 * When no display is reachable the test asserts the HONEST failure instead: -1 and an
 * emptied list.  It does not skip silently, because a skipped test looks like a pass.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>

#include "wcc_monitors.h"

static int checks_run = 0, checks_failed = 0;

#define TEST(cond, msg) do { \
    checks_run++; \
    if (!(cond)) { checks_failed++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); } \
} while (0)

/* Count connected outputs the same way xrandr reports them, by asking the same X
 * server through the same protocol.  Independent of the module under test?  No -- but
 * it IS independent code, and a mismatch between two XRandR readers is exactly what we
 * want surfaced.  When DISPLAY is unset this returns -1 and the caller skips. */
static int count_connected_via_xrandr(void)
{
    /* Independent oracle: parse the text output of the `xrandr` tool rather than
     * calling XRandR again -- otherwise the test compares the module against itself and
     * can agree with it while both are wrong.  Returns -1 when the tool or display is
     * unavailable. */
    FILE *fp = popen("xrandr -q 2>/dev/null", "r");
    if (!fp) return -1;

    char line[512];
    int n = 0;
    while (fgets(line, sizeof(line), fp)) {
        /* Match only the OUTPUT line: begins with a non-space, ends with connected
         * or disconnected.  Mode lines begin with whitespace and must not count. */
        if (line[0] == ' ' || line[0] == '\t') continue;
        if (strstr(line, " connected ") || strstr(line, " connected\n")) {
            if (strstr(line, "disconnected")) continue;
            n++;
        }
    }
    pclose(fp);
    return n;
}

int main(void)
{
    /* -- descriptor invariants on a synthetic two-monitor layout --------- */
    wcc_monitors_t synth;
    memset(&synth, 0, sizeof(synth));
    synth.count = 2;
    snprintf(synth.m[0].name, sizeof(synth.m[0].name), "eDP-1");
    synth.m[0].x = 0;    synth.m[0].y = 0;    synth.m[0].w = 1920; synth.m[0].h = 1080;
    synth.m[0].mm_w = 344; synth.m[0].mm_h = 194;
    synth.m[0].refresh_mhz = 144000;         /* milli-Hz, as wl_output wants */
    synth.m[0].connected = true; synth.m[0].primary = true;

    snprintf(synth.m[1].name, sizeof(synth.m[1].name), "DP-1");
    synth.m[1].x = 1920; synth.m[1].y = 0;   synth.m[1].w = 2560; synth.m[1].h = 1440;
    synth.m[1].mm_w = 600; synth.m[1].mm_h = 340;
    synth.m[1].refresh_mhz = 60000;
    synth.m[1].connected = true; synth.m[1].primary = false;

    TEST(synth.count == 2, "two-monitor fixture stands up");
    TEST(synth.m[1].x == synth.m[0].w,
         "second monitor sits immediately right of the first");
    TEST(synth.m[0].refresh_mhz > synth.m[1].refresh_mhz,
         "refresh carried per monitor");

    /* -- real discovery, or honest failure ------------------------------- */
    wcc_monitors_t real;
    int got = wcc_monitors_discover(&real);
    int expect = count_connected_via_xrandr();

    if (expect < 0) {
        checks_run++;
        printf("  NOTE no X display reachable; asserting fail-closed instead\n");
        TEST(got == -1, "discovery fails closed with no X display");
        TEST(real.count == 0, "failed discovery leaves no phantom monitors");
    } else {
        TEST(got == expect, "discovered exactly as many monitors as are connected");
        TEST(real.count > 0, "at least one monitor reported");

        for (int i = 0; i < real.count; i++) {
            TEST(real.m[i].connected, "every advertised monitor is connected");
            TEST(real.m[i].w > 0 && real.m[i].h > 0,
                 "advertised monitor has a real resolution");
            TEST(strlen(real.m[i].name) > 0, "advertised monitor has a name");
            TEST(real.m[i].refresh_mhz > 0, "advertised monitor has a refresh rate");
        }

        /* Resolution must match what xrandr actually reports for the current mode --
         * the assertion that would have caught treating mode IDs as pixel widths. */
        FILE *xr = popen("xrandr -q 2>/dev/null", "r");
        if (xr) {
            char line[512];
            int ref_w = 0, ref_h = 0;
            /* First mode line after the connected output carries WxH. */
            rewind(xr);
            while (fgets(line, sizeof(line), xr)) {
                if (line[0] == ' ' || line[0] == '\t') {
                    int w, h;
                    if (sscanf(line, " %dx%d", &w, &h) == 2) { ref_w = w; ref_h = h; break; }
                }
            }
            pclose(xr);
            if (ref_w > 0) {
                TEST(real.m[0].w == ref_w && real.m[0].h == ref_h,
                     "discovered resolution matches the output's current mode");
            }
        }

        /* Exactly one primary when any exist -- two primaries would leave every
         * taskbar/overview decision ambiguous. */
        int primaries = 0;
        for (int i = 0; i < real.count; i++)
            if (real.m[i].primary) primaries++;
        TEST(primaries <= 1, "at most one primary monitor is advertised");
    }

    printf("%d checks, %d failed\n", checks_run, checks_failed);
    return checks_failed ? 1 : 0;
}
