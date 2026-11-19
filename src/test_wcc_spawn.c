/*
 * test_wcc_spawn.c -- behaviour gate for the guest-launch leaf.
 *
 * Drives the REAL wcc_spawn_guest(): the no-spawn knob, the command
 * override (a child that actually runs), and the derived default path
 * (must point at the sibling wubuos guest, not a hardcoded /home).
 * Headless: no X, no compositor, no live client.
 */
#define _POSIX_C_SOURCE 200809L
#include "wcc_spawn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

int main(void) {
    /* 1. The derived default points at the sibling wubuos guest. */
    const char *def = wcc_default_start_cmd();
    CHECK(strstr(def, "/wubuos/src/hosted/wubu") != NULL,
          "default guest path targets the sibling wubuos checkout");
    CHECK(access(def, X_OK) == 0,
          "derived default guest is executable on this machine");

    /* 2. WUBU_NO_SPAWN=1: returns without forking. */
    setenv("WUBU_NO_SPAWN", "1", 1);
    wcc_spawn_guest();
    CHECK(waitpid(-1, NULL, WNOHANG) == -1, "no child spawned under WUBU_NO_SPAWN");
    unsetenv("WUBU_NO_SPAWN");

    /* 3. WUBU_START_CMD override: the child actually runs. */
    setenv("WUBU_START_CMD", "echo wcc-spawn-ok", 1);
    wcc_spawn_guest();
    int status = 0;
    pid_t p = waitpid(-1, &status, 0);
    CHECK(p > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "WUBU_START_CMD child ran and exited 0");
    unsetenv("WUBU_START_CMD");

    /* 4. WUBU_NO_SPAWN=0 means spawn (the knob is not just presence). */
    setenv("WUBU_NO_SPAWN", "0", 1);
    setenv("WUBU_START_CMD", "true", 1);
    wcc_spawn_guest();
    p = waitpid(-1, &status, 0);
    CHECK(p > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "WUBU_NO_SPAWN=0 still spawns");
    unsetenv("WUBU_NO_SPAWN");
    unsetenv("WUBU_START_CMD");

    if (failures) { printf("wcc_spawn: %d FAILURES\n", failures); return 1; }
    printf("wcc_spawn: ALL PASS\n");
    return 0;
}