// Co-op cutscenes watched together, and per-player dialog (coop_cutscene.c), game 1.1.1.
#pragma once
#include <stddef.h>
#include <stdint.h>

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

// The player's own event table (RSDB/EventSetting), read once on a thread of its own when first asked for; any thread.
// 0 not asked yet (this call starts the reading), 1 being read, 2 read (or given up: then the lists are empty).
int coop_events_state(void);
// The flags of the NPC talk events (HavePlayedEvent.<event> of every event whose component list is an NPC one: talks,
// "near" lines, chases, each-frame lines), sorted; the count, 0 until read.
int coop_events_talk_flags(const uint32_t **out);

// ---- the wire format (pure functions)
enum { COOP_K_CUTSCENE = 0x42 };   // coop_proto.h's kinds are 1..7, tests 0xF0 and above; kills 0x40 and 0x41
// TIME (the time of day, coop_cutscene.c): u8 version 1, u32 epoch, u32 seq, u32 order (a Lamport clock), u32 seconds
// of the day (0..86399), u8 flags (1: a jump); 18 bytes.
enum { COOP_K_TIME = 0x43 };
enum { COOP_CUT_VERSION = 1, COOP_CUT_EVENT_MAX = 79, COOP_CUT_AREA_MAX = 63, COOP_CUT_MSG_MAX = 176 };
enum { COOP_CUT_START = 1, COOP_CUT_END = 2 };   // op
enum { COOP_CUT_ADULT = 1, COOP_CUT_MOVIE = 2 };  // flags
typedef struct {
    uint8_t op, flags;
    uint32_t epoch;              // the sender's process: random at start
    uint32_t seq;                // the sender's counter (START and END together); the 3 copies of one message share it
    char event[COOP_CUT_EVENT_MAX + 1];   // the event's name (a row of RSDB/EventSetting)
    char area[COOP_CUT_AREA_MAX + 1];     // the sender's Sequence_CurrentBanc
    float xyz[3];                // the sender's Link, metres
} CoopCutMsg;
// Bytes (little-endian): u8 version, u8 op, u32 epoch, u32 seq, u8 flags, u8 event length (1..79), the event, u8 area
// length (0..63), the area, f32 x y z. 0 when it does not fit or a value is out of range.
size_t coop_cut_pack(const CoopCutMsg *m, uint8_t *out, size_t cap);
// 1 when bytes are exactly one message: version 1, op START or END, printable names, finite coordinates.
int coop_cut_parse(const uint8_t *b, size_t n, CoopCutMsg *m);
