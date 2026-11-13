// The messages of NX2navi Co-op's own modules (coop_ext.h carries them between the games): their kinds and byte formats,
// and the codecs, pure functions with no game state, so that --probe-coop-ext (coop_ext_probe.c) and the test peer
// (tests/coop_ext_peer.py) check them against the same byte vectors (tests/coop_ext_vectors.txt).
//
// Every message is the kind byte (coop.h) and a body in the byte order of the PCs this runs on (little-endian). A parser
// takes exactly one message of its kind and version: every length is checked against the body's, a text must be
// printable ASCII (it reaches the log and the game's lookups by name), a number that is not finite or out of its range
// ends the message, and a flag bit this version does not know too. A packer refuses what its parser would refuse, so a
// game never sends what another one drops. The codecs read and write texts of at most their caps; none of them keeps a
// pointer into the bytes.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "coop.h"

#ifdef __cplusplus
extern "C" {
#endif

// The kinds, from COOP_EXT_KIND (coop.h) on. 0x4b and up are free for the next messages.
enum {
    COOP_K_KILL = 0x40,   // an enemy one player killed (coop_kill.c)
    COOP_K_DAMAGE,        // a hit one player dealt to an enemy (coop_kill.c)
    COOP_K_CUTSCENE,      // a story cutscene a player starts or ends (coop_cutscene.c)
    COOP_K_TIME,          // the time of day (coop_cutscene.c)
    COOP_K_ERA,           // the room's era (coop_cutscene.c)
    COOP_K_PUPPET,        // where a player's Link stands and what he plays (coop_puppet.c)
    COOP_K_GEAR,          // what a player's Link wears and holds (coop_puppet.c)
    COOP_K_PICK,          // the colour a player picked (coop_colours.c)
    COOP_K_COLOURS,       // the colours the host gave the players (coop_colours.c)
    COOP_K_KEYS,          // the room's small keys, from its host (coop_keys.c)
    COOP_K_KEY,           // a small key a player picked up or used (coop_keys.c)
    COOP_K_EXT_END,       // the first kind not used yet
};
// The kind's name for log lines ("KILL"), "?" for a kind this build does not know.
const char *coop_ext_kind_name(uint8_t kind);

// ---- KILL and DAMAGE: u8 version 1, u32 epoch, u32 eseq, u8 area length (1..47), the area (printable, no space), u64
// inst (not 0 or all ones), u32 name hash, [DAMAGE: u16 raw (1..1000)], u8 flags (COOP_KILL_BOSS). At most 72 bytes.
enum { COOP_KILL_VERSION = 1, COOP_KILL_AREA_MAX = 47, COOP_KILL_MSG_MAX = 80 };
enum { COOP_KILL_BOSS = 1 };   // flags
typedef struct {
    uint32_t epoch;              // the sender's process: random at start (a restart starts the receiver's window again)
    uint32_t eseq;               // the sender's event counter (KILL and DAMAGE together)
    char area[COOP_KILL_AREA_MAX + 1];   // the sender's Sequence_CurrentBanc
    uint64_t inst;               // the enemy's placement hash *(*(actor+0x3d8)+0x88)
    uint32_t name_hash;          // coop_kill_name_hash of the actor pack's name
    uint16_t raw;                // DAMAGE: the hit as the sender's game made it, before any scaling (1..1000)
    uint8_t flags;               // COOP_KILL_BOSS
} CoopKillMsg;
size_t coop_kill_pack(uint8_t kind, const CoopKillMsg *m, uint8_t *out, size_t cap);   // 0: does not fit or out of range
int coop_kill_parse(uint8_t kind, const uint8_t *b, size_t n, CoopKillMsg *m);         // 1: exactly one message
uint32_t coop_kill_name_hash(const char *name);   // FNV-1a

// ---- CUTSCENE: u8 version 1, u8 op (START, END), u32 epoch, u32 seq, u8 flags (COOP_CUT_ADULT, COOP_CUT_MOVIE), u8
// event length (1..79), the event (a row of RSDB/EventSetting), u8 area length (0..63), the area (both printable, no
// space), f32 x y z (finite, at most 1e6 from the origin on each axis). At most 167 bytes.
enum { COOP_CUT_VERSION = 1, COOP_CUT_EVENT_MAX = 79, COOP_CUT_AREA_MAX = 63, COOP_CUT_MSG_MAX = 176 };
enum { COOP_CUT_START = 1, COOP_CUT_END = 2 };   // op
enum { COOP_CUT_ADULT = 1, COOP_CUT_MOVIE = 2 };  // flags
typedef struct {
    uint8_t op, flags;
    uint32_t epoch;              // the sender's process: random at start
    uint32_t seq;                // the sender's counter (START and END together)
    char event[COOP_CUT_EVENT_MAX + 1];
    char area[COOP_CUT_AREA_MAX + 1];     // the sender's Sequence_CurrentBanc
    float xyz[3];                // the sender's Link, metres
} CoopCutMsg;
size_t coop_cut_pack(const CoopCutMsg *m, uint8_t *out, size_t cap);
int coop_cut_parse(const uint8_t *b, size_t n, CoopCutMsg *m);

// ---- TIME: u8 version 1, u32 epoch, u32 seq, u32 order (a Lamport clock: each jump makes one more), u32 seconds of the
// day (0..86399), u8 flags (COOP_TIME_JUMP; else the host's beat). 18 bytes.
enum { COOP_TIME_VERSION = 1, COOP_TIME_MSG = 18, COOP_TIME_DAY = 86400 };
enum { COOP_TIME_JUMP = 1 };   // flags
typedef struct {
    uint32_t epoch, seq, order, tod;
    uint8_t flags;
} CoopTimeMsg;
size_t coop_time_pack(const CoopTimeMsg *m, uint8_t *out, size_t cap);
int coop_time_parse(const uint8_t *b, size_t n, CoopTimeMsg *m);

// ---- ERA: u8 version 1, u32 epoch, u32 seq, u32 order (a Lamport clock: each change of era makes one more), u8 flags
// (COOP_ERA_*), u8 length (0..79) and the story scene that brought the change, u8 length (0..63) and the scene's area,
// f32 x y z (where the scene started), u8 length (0..63) and an area of the room's era, f32 x y z (a place there): texts
// printable with no space, coordinates as CUTSCENE's. At most 246 bytes.
enum { COOP_ERA_VERSION = 1, COOP_ERA_MSG_MAX = 256 };
enum { COOP_ERA_ADULT = 1, COOP_ERA_CHANGE = 2, COOP_ERA_SCENE_ADULT = 4 };   // flags: the room's era, a change (else a
                                                                              // beat), the scene's era at its start
typedef struct {
    uint32_t epoch, seq, order;
    uint8_t flags;
    char scene[COOP_CUT_EVENT_MAX + 1];         // "" none
    char scene_area[COOP_CUT_AREA_MAX + 1];
    float scene_xyz[3];
    char area[COOP_CUT_AREA_MAX + 1];
    float xyz[3];
} CoopEraMsg;
size_t coop_era_pack(const CoopEraMsg *m, uint8_t *out, size_t cap);
int coop_era_parse(const uint8_t *b, size_t n, CoopEraMsg *m);

// ---- PUPPET (unreliable, about 15 a second while Link is in the world): u8 version 1, u32 pseq (one more for each
// PUPPET sent), u32 t_ms (the sender's clock), u8 flags (COOP_PUPPET_*), u8 area length (0..47) and the area, f32 x y z
// (metres, at most 1e6 from the origin on each axis), f32 yaw (radians, 0 facing +z, at most 7 either way), u8 slot
// count (0..4), per animation slot: u8 node name length (0..47), the node's name, f32 frame (at most 1e6 either way);
// u8 upper (bit i-1: the sender's slot i, 1..3, plays nothing, so the puppet's arms follow slot 0 there), u32 the
// signature of the sender's gear (coop_gear_sig; 0 none known). Texts are printable ASCII (spaces too). At most 288
// bytes.
enum { COOP_PUPPET_VERSION = 1, COOP_PUPPET_TEXT = 47, COOP_PUPPET_SLOTS = 4, COOP_PUPPET_MSG_MAX = 288 };
enum { COOP_PUPPET_ADULT = 1, COOP_PUPPET_IN_WORLD = 2 };   // flags
#define COOP_PUPPET_POS_MAX 1e6f
#define COOP_PUPPET_YAW_MAX 7.0f
#define COOP_PUPPET_FRAME_MAX 1e6f
typedef struct {
    uint32_t pseq, t_ms;
    uint8_t flags, nslots, upper;
    char area[COOP_PUPPET_TEXT + 1];
    float pos[3], yaw;
    struct { char node[COOP_PUPPET_TEXT + 1]; float frame; } slot[COOP_PUPPET_SLOTS];
    uint32_t gear_sig;
} CoopPuppetMsg;
size_t coop_puppet_pack(const CoopPuppetMsg *m, uint8_t *out, size_t cap);
int coop_puppet_parse(const uint8_t *b, size_t n, CoopPuppetMsg *m);

// ---- GEAR (reliable, when the sender's gear changes and when a player comes): u8 version 1, u32 signature
// (coop_gear_sig of what follows: a GEAR whose signature differs is refused), u8 hand mask (bits 0..6: the
// EquipmentUser's dynamic slots), u16 body mask (bits 0..12: its static slots), per hand bit: u8 state (COOP_GEAR_*),
// u8 name length (1..47), the name; per body bit: u8 name length (1..47), the name. Names are actor names of the
// sender's game, printable ASCII (spaces too). At most 975 bytes.
enum { COOP_GEAR_VERSION = 1, COOP_GEAR_DYN = 7, COOP_GEAR_STAT = 13, COOP_GEAR_MSG_MAX = 975 };
enum { COOP_GEAR_STOWED, COOP_GEAR_DRAWN, COOP_GEAR_SHEATHED };   // a hand item's state
typedef struct {
    char dyn[COOP_GEAR_DYN][COOP_PUPPET_TEXT + 1];    // "" none
    uint8_t state[COOP_GEAR_DYN];
    char stat[COOP_GEAR_STAT][COOP_PUPPET_TEXT + 1];  // "" none
} CoopGear;
uint32_t coop_gear_sig(const CoopGear *g);   // FNV-1a over the names and states; never 0
size_t coop_gear_pack(const CoopGear *g, uint8_t *out, size_t cap);
int coop_gear_parse(const uint8_t *b, size_t n, CoopGear *g, uint32_t *sig);

// ---- the players' colours: PICK (everyone, reliable, when the pick changes and when a player comes): u8 version 1, u8
// the colour picked (COOP_COLOUR_*, COOP_COLOUR_NONE for "any"); 2 bytes. COLOURS (the host only, reliable, at each
// change and when a player comes): u8 version 1, u8 count (0..4), count x (u64 the player's id, u8 colour): ids not 0
// and colours below COOP_COLOURS, each id and each colour at most once. At most 38 bytes.
enum { COOP_COLOUR_GREEN, COOP_COLOUR_RED, COOP_COLOUR_BLUE, COOP_COLOUR_PURPLE, COOP_COLOURS, COOP_COLOUR_NONE = 0xff };
enum { COOP_PICK_VERSION = 1, COOP_PICK_MSG = 2, COOP_COLOURS_VERSION = 1, COOP_COLOURS_MSG_MAX = 2 + 9 * COOP_PLAYERS };
typedef struct {
    uint8_t n;
    struct { uint64_t id; uint8_t colour; } e[COOP_PLAYERS];
} CoopColoursMsg;
size_t coop_pick_pack(uint8_t pick, uint8_t *out, size_t cap);
int coop_pick_parse(const uint8_t *b, size_t n, uint8_t *pick);
size_t coop_colours_pack(const CoopColoursMsg *m, uint8_t *out, size_t cap);
int coop_colours_parse(const uint8_t *b, size_t n, CoopColoursMsg *m);

// ---- the room's small keys: KEYS (the host only, reliable, at each change, when a player comes and every few
// seconds): u8 version 1, u32 serial (one more at each change of the host's counts), u8 count (0..16), count x (u32
// the hash of a dungeon's small-key counter, u8 keys 0..COOP_KEYS_MAX), u8 acks (0..4), acks x (u64 a player's id, u32
// their sess, u32 the last seq of theirs the host took): hashes and ids not 0 and each at most once, sess not 0. At
// most 151 bytes. KEY (a player to the host, reliable, until a KEYS acks it): u8 version 1, u32 sess (the sender's,
// random and not 0, another at each room), u32 seq (1 for the first change in that sess, one more for each next), u32
// the counter's hash (not 0), i8 the change (-COOP_KEYS_MAX..COOP_KEYS_MAX, not 0: keys picked up, or used when
// below 0). 14 bytes.
enum { COOP_KEYS_VERSION = 1, COOP_KEYS_DUNGEONS = 16, COOP_KEYS_MAX = 15, COOP_KEYS_MSG_MAX = 7 + 5 * 16 + 16 * COOP_PLAYERS,
       COOP_KEY_VERSION = 1, COOP_KEY_MSG = 14 };
typedef struct {
    uint32_t serial;
    uint8_t n, nack;
    struct { uint32_t hash; uint8_t keys; } d[COOP_KEYS_DUNGEONS];
    struct { uint64_t id; uint32_t sess, seq; } ack[COOP_PLAYERS];
} CoopKeysMsg;
typedef struct {
    uint32_t sess, seq, hash;
    int8_t delta;
} CoopKeyMsg;
size_t coop_keys_pack(const CoopKeysMsg *m, uint8_t *out, size_t cap);
int coop_keys_parse(const uint8_t *b, size_t n, CoopKeysMsg *m);
size_t coop_key_pack(const CoopKeyMsg *m, uint8_t *out, size_t cap);
int coop_key_parse(const uint8_t *b, size_t n, CoopKeyMsg *m);

// Any kind above: the message parsed and packed again into out (cap), its length; 0 when it does not parse (or the
// kind is none of these). A message that parses packs to the same bytes.
size_t coop_ext_wire_repack(uint8_t kind, const uint8_t *b, size_t n, uint8_t *out, size_t cap);

// ---- the receiver's counter of one PUPPET sender. A pose is taken only above the last one taken from the same seat
// and gen (coop_ext.h): a pose sent again or late does not move the puppet back. The first pose of another gen (another
// player in the seat) starts the counter again, and so does any pose after COOP_POS_QUIET_MS with none taken (a game
// that started again counts from 1). Returns 0 to drop the pose, 1 to take it, 2 to take it and drop what was kept of
// the sender's earlier poses.
#define COOP_POS_QUIET_MS 3000
typedef struct {
    uint32_t gen;               // the gen of the last pose taken
    uint32_t last;              // its counter
    uint64_t at_ms;             // when it was taken
    int any;                    // a pose was taken
} CoopPosSeen;
int coop_pos_fresh(CoopPosSeen *s, uint32_t gen, uint32_t pseq, uint64_t now_ms);

#ifdef __cplusplus
}
#endif
