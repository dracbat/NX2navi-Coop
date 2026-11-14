// The glue between the co-op room (coop.c) and the co-op modules, which share more than progress: the other players'
// Links, their colours, kills, cutscenes, the time of day, the era and the small keys (coop_ext_wire.h lists them).
// Their messages are the kinds from COOP_EXT_KIND on (coop_ext_wire.h has the formats); coop.c hands each one, from a
// player on the host's list only, to this file's router, which passes it to the module that registered its kind.
//
// Seats. A module names the players of the room by seat, 1..COOP_PLAYERS, never by their Steam id: each id on the room's
// list gets the first free seat while it is there, and its seat is free again when it leaves. A seat's gen is one more
// each time the seat is given, so a seat and its gen name one player's stay in the room. The seats are this game's own
// numbering: other games may seat the same player elsewhere, so a seat never goes into a message.
//
// Threads. Handlers, view hooks and tick hooks run on the co-op thread, one step behind the room (coop_view); they must
// be short and must not wait for the game. coop_ext_view and coop_ext_send work from any thread.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "coop.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t id;                     // the player's id; 0: the seat is free. Never logged and never sent
    uint32_t gen;                    // one more each time a seat is given (a number of this process, unique per stay)
    char name[COOP_NAME_CAP];        // their name as the room shows it ("" until known)
    uint8_t self, host;              // this game; the room's host
    uint8_t in_world;                // their Link is in the game world: their last pose names an area and is fresh
    char area[COOP_SCENE_CAP];       // that area ("" when not in the world)
} CoopSeat;

typedef struct {
    int in_room;                     // this game is in a room (let in by its host)
    uint32_t room;                   // one more at every room entered (never the room's id)
    int self, host, n;               // this game's seat, the host's seat (0 none), the players seated
    int order[COOP_PLAYERS];         // the n seats in the room's list order (on the host: the order they came in)
    CoopSeat seat[COOP_PLAYERS + 1]; // by seat, 1..COOP_PLAYERS
} CoopExtView;

// main.c, before coop_init; the modules register their hooks after it and before coop_init too.
void coop_ext_init(void);

typedef void (*CoopExtMsgFn)(int seat, const uint8_t *b, size_t n, void *user);   // a message of the module's kind
typedef void (*CoopExtViewFn)(const CoopExtView *v, void *user);                  // after any change of the view
typedef void (*CoopExtTickFn)(uint64_t now_ms, void *user);                       // every step while co-op is on
// One handler per kind (from COOP_EXT_KIND on); up to 8 view hooks and 8 tick hooks. 0 when taken or full.
int coop_ext_on_message(uint8_t kind, CoopExtMsgFn fn, void *user);
int coop_ext_on_view(CoopExtViewFn fn, void *user);
int coop_ext_on_tick(CoopExtTickFn fn, void *user);

// A message to everyone in the room (seat 0) or to one seat, from any thread; n at most COOP_MESSAGE_MAX. It leaves at
// the co-op thread's next step. 0 outside a room, for a free seat, or when the room's queue is full: a sender of a
// reliable message keeps it and tries again at its next tick.
int coop_ext_send(int seat, uint8_t kind, const void *b, size_t n, int reliable);
// The view of the last step (zeros outside a room), from any thread.
void coop_ext_view(CoopExtView *out);

// A log line about a message from a seat, from a module's handler: "coop: <text>" while that seat's lines stay within a
// few a second, else counted, and the count said at most once every 30 s. Modules log what other players send with it,
// so that no player can fill this game's log.
void coop_ext_log(int seat, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// --probe-coop-ext (coop_ext_probe.c): the codecs of coop_ext_wire.h against tests/coop_ext_vectors.txt; no game files.
int probe_coop_ext(void);

#ifdef __cplusplus
}
#endif
