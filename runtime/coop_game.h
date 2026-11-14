// Co-op, the game side (coop_game.c): where this game's Link is and the progress a room shares. coop.h declares the
// part the co-op thread calls (coop_game_message, coop_game_joined, coop_game_room).
#pragma once

#include "cpu.h"

void coop_game_init(void);
// At each present, on the game's thread, between two frames: reads Link and the game's flags and sets the flags the
// room has and this game lacks, through the game's own routines. Does nothing while the COOP setting is off.
void coop_present(Cpu *c);
// Each RomFS file the game opens (fs_host.c): the title screen's start is told by its event flow being opened.
void coop_game_rom_open(const char *path);
// Cutscenes (coop_cutscene.c), any thread: on, the room's values wait instead of being applied (a follower
// plays its own copy of a scene); the sync's gate at the last present (a save is played, Link in the world, no load or
// scene change, the clock runs).
void coop_game_hold(int on);
int coop_game_running(void);
// The era (coop_cutscene.c), any thread: a counter that moves whenever the title screen comes back (another
// save may be played next, so the era is read anew).
unsigned coop_game_save_serial(void);
// The other players' Links and the time of day (coop_puppet.c, coop_cutscene.c), any thread: 1 from the first present
// with the gate open (coop_game_running) after the title screen until the title screen comes back, a pause or a load
// included. On the title screen Link's actor and the area flag stay from the save played before, so they alone do not
// tell that this player is in the world.
int coop_game_played(void);
// The time of day (coop_cutscene.c), any thread: a counter that moves whenever a save is loaded (another one, or the
// same one again) and whenever the title screen comes back, so that a time read before it may be another save's.
unsigned coop_game_load_serial(void);
// The co-op modules, any thread: 1 once the save was copied for this room (coop_present starts the copy in a room
// with a save played, whether progress is shared or not); a received kill, a followed scene, an era's teleport or a time
// of day waits for it.
int coop_game_backed_up(void);
