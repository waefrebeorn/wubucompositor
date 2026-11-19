/*
 * wcc_spawn.h -- guest launch for the WuBuCompositor. One concern.
 *
 * The compositor brings up the desktop by spawning the WuBuOS hosted
 * client. This leaf owns that decision: the env knobs (WUBU_START_CMD,
 * WUBU_NO_SPAWN) and the default guest path, derived from the compositor
 * binary's own location instead of a hardcoded absolute home directory.
 */
#ifndef WCC_SPAWN_H
#define WCC_SPAWN_H

/* Spawn the guest unless WUBU_NO_SPAWN is set (non-empty, not "0").
 * WUBU_START_CMD overrides the command; the default is derived from
 * /proc/self/exe (sibling wubuos checkout), never a fixed /home path. */
void wcc_spawn_guest(void);

/* The derived default guest command (cached). Exposed for the gate. */
const char *wcc_default_start_cmd(void);

#endif /* WCC_SPAWN_H */