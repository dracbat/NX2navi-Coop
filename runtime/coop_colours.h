// The players' colours in a co-op room (coop_colours.c): green, red, blue and purple, one per player, the same in every
// game of the room. Each player picks one or none (the setting coop_colour, the Co-op tab's "Your colour" row) and
// says so to the room (PICK); the room's host gives the colours (COLOURS) and every game shows what the host gave:
// each player's Link (coop_puppet.c), their name above it (coop_tags.h) and the tab's list of players.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// main.c, after coop_ext_init and before coop_init: the PICK and COLOURS handlers and the hooks on the co-op thread.
void coop_colours_init(void);

// Any thread. A colour is a COOP_COLOUR_* of coop_ext_wire.h (0 green, 1 red, 2 blue, 3 purple), -1 none: outside a
// room, with room colours off (coop_colours = 0), or not given yet.
int coop_colours_on(void);                // room colours are on (the setting coop_colours)
int coop_colours_self(void);              // this game's player
int coop_colours_seat(int seat);          // the player in that seat of coop_ext.h
int coop_colours_of(uint64_t id);         // the player with that id, as the room's view lists them (coop.h)
int coop_colours_held_by_other(int colour);   // 1 when another player of the room has that colour now

#ifdef __cplusplus
}
#endif
