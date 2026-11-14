// Co-op puppets (coop_puppet.c): the other players of a co-op room shown in this game as Links of their own, game 1.1.1.
#pragma once

// nvnQueuePresentTexture, after coop_present: switches the puppets' frame hook (the patch set coop_puppet) on when there is
// work for it. With co-op off it reads one counter and returns.
void coop_puppet_present(void);
// At start, after coop_ext_init and before coop_init: the puppets' handlers on the co-op thread (the other players'
// PUPPET and GEAR, the room's seats), then those of kills.
void coop_puppet_init(void);
// Kills and difficulty (coop_kill.c), any thread: the other players of the room whose Link is in the game world now
// (coop_ext.h: their last pose names an area and is fresh).
int coop_puppet_world_others(void);
// Kills tests (coop_kill.c), any thread: the other players whose Link is in the game world in this area.
int coop_puppet_area_others(const char *area);
