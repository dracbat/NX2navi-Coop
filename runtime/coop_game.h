// Co-op progress sync, the game side (Ocarina 1.1.1): reads the game's flag database between frames, hands what changed
// to the merge engine (coop_state.c), and gives the game what the room has through the game's own set routines, so the
// game takes it as if the player had done it. Nothing is read or written while the game is on its title screen, loading
// or changing scene; what arrives meanwhile waits. docs/coop.md says what is shared.
#pragma once
#include "coop_offer.h"
#include "coop_proto.h"
#include "coop_state.h"
#include "host.h"

// Registers the message handlers and the coop thread's hooks (main.c, after coop_init). Allocates nothing until the COOP
// setting is on.
void coop_game_init(void);
// nvnQueuePresentTexture, between frames on the game's thread with its core held (as res_present). With COOP off it reads
// one counter and returns.
void coop_present(Cpu *c);

// The offer the player accepts before anything of a room is applied, the maker's remedies and the pools' contributors:
// coop_offer.h (coop_game_offer, coop_game_accept, coop_game_reset_pools, coop_game_remove_key, coop_game_contributors).
// fs_host.c, at every RomFS file open on any thread: notes the start of the title screen, after which sync waits for a
// save to be loaded. With COOP off it reads one value and returns.
void coop_game_rom_open(const char *path);
// Cutscenes (coop_cutscene.c), any thread: on, the room's values wait instead of being applied (a follower
// plays its own copy of a scene); the sync's gate at the last present (a save is played, Link in the world, no load or
// scene change, the clock runs).
void coop_game_hold(int on);
int coop_game_running(void);
// The era (coop_cutscene.c), any thread: a counter that moves whenever the title screen comes back (another
// save may be played next, so the era is read anew).
unsigned coop_game_save_serial(void);
