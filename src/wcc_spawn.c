/*
 * wcc_spawn.c -- guest launch (extracted from wubucompositor.c so the
 * spawn concern has one owner and the monolith shrinks).
 *
 * The default guest path used to be the absolute literal
 * "/home/wubu/wubuos/src/hosted/wubu" in TWO files — the compositor only
 * worked on one machine's directory layout. Now it is derived from the
 * compositor binary's own location (/proc/self/exe): the binary lives at
 * <root>/wubucompositor/wubucompositor and the guest at
 * <root>/wubuos/src/hosted/wubu, so the default is "../wubuos/src/hosted/wubu"
 * relative to the executable. WUBU_START_CMD still overrides everything.
 */
#define _POSIX_C_SOURCE 200809L
#include "wcc_spawn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

/* Last-resort fallback only if /proc/self/exe is unreadable. Overridable
 * at build time (-DWUBU_GUEST_FALLBACK='"..."') for unusual layouts. */
#ifndef WUBU_GUEST_FALLBACK
#define WUBU_GUEST_FALLBACK "/home/wubu/wubuos/src/hosted/wubu"
#endif

const char *wcc_default_start_cmd(void) {
    static char path[PATH_MAX];
    static int resolved = 0;
    if (!resolved) {
        resolved = 1;
        ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
        if (n > 0) {
            path[n] = '\0';
            char *slash = strrchr(path, '/');
            if (slash) {
                *slash = '\0';  /* now the executable's directory */
                size_t len = strlen(path);
                const char *rel = "/../wubuos/src/hosted/wubu";
                if (len + strlen(rel) < sizeof(path)) {
                    strcat(path, rel);
                    return path;
                }
            }
        }
        snprintf(path, sizeof(path), "%s", WUBU_GUEST_FALLBACK);
    }
    return path;
}

void wcc_spawn_guest(void) {
    /* WUBU_NO_SPAWN=1 skips the spawn — what you want when starting the
     * guest yourself; two clients on one seat fight over wl_keyboard. */
    const char *no_spawn = getenv("WUBU_NO_SPAWN");
    if (no_spawn && *no_spawn && no_spawn[0] != '0') {
        fprintf(stderr, "WuBuCompositor: WUBU_NO_SPAWN set; waiting for a client\n");
        return;
    }
    const char *start = getenv("WUBU_START_CMD");
    if (!start || !*start) start = wcc_default_start_cmd();
    pid_t pid = fork();
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", start, (char *)NULL);
        _exit(127);
    }
}