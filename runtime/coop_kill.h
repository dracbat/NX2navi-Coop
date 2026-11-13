// Co-op kills, shared damage and difficulty (coop_kill.c), game 1.1.1.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "coop_ext_wire.h"   // KILL and DAMAGE: CoopKillMsg and its codec
#include "cpu.h"

// coop_puppet_init, after coop_init: the KILL and DAMAGE handlers, the roster and tick hooks on the co-op thread.
void coop_kill_init(void);
// coop_puppet_present (present thread, every present): settings, the players present and the difficulty factors, the
// life hook's switch (the patch set coop_life), expiry of what other players sent. 1 when the frame hook should run
// for this module (coop_kill_frame).
int coop_kill_present(void);
// puppet_frame (the game's update thread, between two frames): the area the local Link is in and his life component,
// for the life hook on the actors' threads.
void coop_kill_frame(Cpu *c, uint64_t player, const char *area);
