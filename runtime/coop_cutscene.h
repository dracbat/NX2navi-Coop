// Co-op cutscenes watched together, the time of day and the era (coop_cutscene.c), game 1.1.1.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "coop_ext_wire.h"   // CUTSCENE, TIME and ERA: their messages and codecs
#include "cpu.h"

// coop_puppet_init, after coop_init: the CUTSCENE handler and the co-op thread's hooks.
void coop_cutscene_init(void);
// coop_puppet_present (present thread, every present): settings; 1 when the frame hook should run for this module.
int coop_cutscene_present(void);
// puppet_frame (the game's update thread, between two frames): the leader's watch of the event manager and the
// follower's steps. player: Link's actor or 0; area: Sequence_CurrentBanc ("" without Link).
void coop_cutscene_frame(Cpu *c, uint64_t player, const char *area);
// coop_puppet.c remotes (game thread): 1 while a story cutscene plays here and the puppets should not stand in it.
int coop_cutscene_hides_puppets(void);

// ---- the era (coop_cutscene.c "the era")
// Sequence_IsChildhood (Bool, true for a child, saved): the game sets it from each area it loads (an area of the other
// era makes Link that age), so the era changes when the Master Sword scenes take Link into the other era's areas.
#define COOP_ERA_FLAG 0xcea220d3u
// NX2NAVI_COOP_ERA (environment or nx2navi.ini, no menu row):
//   follow (default)  the flag is kept out of progress sync; a change of era goes to the room (the ERA message, with the
//                     story scene that brought it), and a player whose era is not the room's follows that scene where
//                     it started, else (or when it did not change the era) is teleported into the room's era
//   scene             the same without the teleport (only the scene)
//   shared            the old way: the flag in progress sync (child to adult only)
//   own (or 0, off)   kept out of progress sync, nothing carried: each player's own era
enum { COOP_ERA_FOLLOW, COOP_ERA_SCENE, COOP_ERA_SHARED, COOP_ERA_OWN };
int coop_era_setting(const char **raw);   // any thread: the mode asked for now; *raw (when given) the text ("" unset)
