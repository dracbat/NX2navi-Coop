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

// ERA: u8 version 1, u32 epoch, u32 seq, u32 order (a Lamport clock: each change of era makes one more), u8 flags,
// u8 scene length (0..79), the scene, u8 length (0..63), the scene's area, f32 x y z (where the scene started), u8
// length (0..63), the area, f32 x y z (a place of the room's era: the sender's at its change, or now for a beat); at most
// 246 bytes. A change goes out three times; the room's keeper (the lowest member id) repeats the room's era every 3 s,
// and a game that hears an older order answers with its newest.
enum { COOP_K_ERA = 0x44 };
enum { COOP_ERA_VERSION = 1, COOP_ERA_MSG_MAX = 256 };
enum { COOP_ERA_ADULT = 1, COOP_ERA_CHANGE = 2, COOP_ERA_SCENE_ADULT = 4 };   // flags: the room's era, a change (else a
                                                                              // beat), the scene's era at its start
typedef struct {
    uint32_t epoch, seq, order;
    uint8_t flags;
    char scene[COOP_CUT_EVENT_MAX + 1];         // the story scene that brought the change ("" none)
    char scene_area[COOP_CUT_AREA_MAX + 1];     // where it started
    float scene_xyz[3];
    char area[COOP_CUT_AREA_MAX + 1];           // a place of the room's era
    float xyz[3];
} CoopEraMsg;
size_t coop_era_pack(const CoopEraMsg *m, uint8_t *out, size_t cap);
int coop_era_parse(const uint8_t *b, size_t n, CoopEraMsg *m);
