// The small keys of a co-op room (coop_keys.c): one count per dungeon for the whole room, kept by the room's host. A
// key any player picks up adds one for everyone, and a locked door any player opens takes one from everyone (the door
// itself is progress the room shares, coop_game.c). The setting coop_keys = pool (the default; it needs coop_share on)
// or own, which keeps each player's keys their own.
#pragma once

#include "cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

// main.c, after coop_ext_init and before coop_init: the KEYS and KEY handlers and the hooks on the co-op thread.
void coop_keys_init(void);
// At each present, on the game's thread, after coop_present: reads this game's small-key counts and sets them to the
// room's, through the game's own routine.
void coop_keys_present(Cpu *c);

#ifdef __cplusplus
}
#endif
