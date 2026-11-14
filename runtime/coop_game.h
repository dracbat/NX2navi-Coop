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
// The other players' Links (coop_puppet.c), any thread: 1 from the first present with the sync's gate open (a save is
// played, Link in the world, no load or scene change, the clock runs) after the title screen until the title screen
// comes back, a pause or a load included. On the title screen Link's actor and the area flag stay from the save played
// before, so they alone do not tell that this player is in the world.
int coop_game_played(void);
// The co-op modules, any thread: 1 once the save was copied for this room (coop_present starts the copy in a room
// with a save played, whether progress is shared or not); a received kill waits for it.
int coop_game_backed_up(void);
