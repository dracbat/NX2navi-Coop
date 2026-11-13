// Co-op cutscenes watched together, the time of day and the era, game 1.1.1.
//
// The game plays one "main event" at a time: the event manager **[main+0x4c7e090] holds it at +0x68 from its
// preparation to its end, and the event's name (a row of RSDB/EventSetting) is the char * at its +0x8. The frame hook
// (coop_puppet.c puppet_frame, the game's update thread) reads both every frame.
//
// Leader. When this game's main event becomes a story cutscene (a row "Dm_*" of the player's own event table, but not
// Dm_SY_* or Dm_Title_*, whose component list is not an NPC one: 309 of 2380 rows), it sends START (event, area,
// Link's place, age, movie) to the room, and END when the event is over. Talks are never sent: dialog stays per player.
//
// Follower. A START from another player of the room (and of a story cutscene of this player's own table, in the same
// age) waits until this game is free (in the world, no event or request of its own, no menu: progress sync's gate, the
// save copied for the room, the debug tools idle; 45 s at most), then holds progress sync (the room's values wait, so
// the leader's flags cannot change this world before or while its own copy plays), and:
//   - in the leader's area, or a pre-rendered movie (as this game's own table says, whatever the START says): starts
//     the event by its name (the debug tools' start, tried every 0.5 s for 20 s while the game refuses it);
//   - elsewhere: the debug tools' teleport to the leader's area, at its start position nearest the leader, with the
//     event reserved as the one the game plays at the arrival (Sequence_ReservedBootEventName); when it does not start
//     there by itself within 3 s, it is started by its name as above. The game refuses a teleport in a menu, in the
//     air or while swimming: the debug tools ask again for a while, and a teleport that never arrived leaves the follow
//     waiting for the next free moment while its 45 s last.
// When this game's event is over (1 s with none), the hold ends (the room's values come in: the same ones its own copy
// set) and the player stays with the leader (NX2NAVI_COOP_CUTSCENE_RETURN=back: teleported back to the start position
// nearest where they were). A game that leads never follows and a game that follows never leads. Boss scenes
// (*_BossIn/*_BossOut) are followed only by players already in the leader's area (NX2NAVI_COOP_CUTSCENE_BOSSES).
// While a story cutscene plays here, the other players' puppets are taken out of it (NX2NAVI_COOP_CUTSCENE_HIDE).
//
// The story cutscenes are the rows of the player's own event table (coop_events.c) that it lists as such.
//
// Chains: a scene can lead into others by itself, with changes of area between them (the Master Sword's does). In a
// follower's world they come as its own copy of the leader's chain: a story scene that starts within 30 s of the end
// of a followed one (or of one of its chain) is neither sent as a START nor does it let this game follow the same
// leader's next START; and a START of a scene this game played in the last 5 minutes is not followed.
//
// The era (section "the era"): Sequence_IsChildhood is out of progress sync; changes of era go to the room with the
// scene that brought them (ERA), and a player of another era follows that scene, else is teleported into the room's
// era (NX2NAVI_COOP_ERA, coop_cutscene.h). A player who joins a room of the other era is taken into the room's era too.
//
// The room (coop_ext.h). Each message goes once and reliably, from the co-op thread; one the room's queue cannot take
// yet waits for the next step (10 s at most). The players of the room are trusted as they are (coop.c lets only those on
// the host's list speak); a window per sender's seat drops what came twice or late, and the lines about what a player
// sent go through coop_ext_log, a few a second per sender. Nothing the room sends reaches this world before its save was
// copied for the room (coop_game_backed_up): a followed scene, a time of day set and a teleport into the room's era
// wait for it. The room's host keeps the room's time and era: it repeats them, and sends them at once to a player who
// comes.
//
// Orders. TIME and ERA carry an order, a Lamport clock: each jump of the time and each change of era makes one more.
// Orders start again from 0 in each room and are compared as serial numbers: one more than ORDER_STEP ahead of or
// behind the order this game knows is taken from the room's host only (anyone else's is dropped). Within one order the
// host's word wins, then the lower player id (the same in every game). So no message can pin the room's time or era
// (the next jump or change is always newer), and a game that comes from a busier room takes the new room's.
//
// Settings (environment or nx2navi.ini, no menu row; logged at start):
//   NX2NAVI_COOP_CUTSCENES=0          nothing sent or followed (default on)
//   NX2NAVI_COOP_CUTSCENE_BOOT=0      no event reserved at the teleport's arrival (start by name after it)
//   NX2NAVI_COOP_CUTSCENE_RETURN=back back where the player was after a followed scene (default stay)
//   NX2NAVI_COOP_CUTSCENE_BOSSES      area (default: only players in the leader's area) | all | 0
//   NX2NAVI_COOP_CUTSCENE_HIDE=0      the other players' puppets stay in a story cutscene
//   NX2NAVI_COOP_CUTSCENE_TRACE=1     every event's start and end, talks included, with its HavePlayedEvent flag
//   NX2NAVI_COOP_TIME=0               the time of day stays each player's (default shared: section "time of day")
//   NX2NAVI_COOP_TIME_PROBE=<s>       a probe: the world time part's words that changed, every <s> seconds
//   NX2NAVI_COOP_ERA                  follow (default) | scene | shared | own (section "the era")
//   NX2NAVI_COOP_CUTSCENE_TEST        tests, ';'-separated:
//       start:<event>@<s>    this game starts the event by its name <s> seconds after Link first stood in the world
//       start:<event>~<s>    ... <s> seconds after another player was first seen in the game world (their pose)
//       follow:<event>:<area>:<x>,<y>,<z>@<s>   a START as if from another player (the follower alone)
//       settime:<hh>:<mm>@<s> (or ~<s>)        the game's own time setter (its jump goes to the room)
//       warp:<area>[:<start position>]@<s> (or ~<s>)   the debug teleport (into an area of the other era: a change of era)
//       loop                 every message sent is parsed back and logged
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "coop.h"
#include "coop_cutscene.h"
#include "coop_events.h"
#include "coop_ext.h"
#include "coop_game.h"
#include "coop_puppet.h"
#include "debug_tools.h"
#include "host.h"
#include "platform/platform.h"

#define CS_MAIN 0x7000000000ull
#define EVENT_MGR_SLOT (CS_MAIN + 0x4c7e090)   // **: the event manager
#define EVM_MAIN 0x68                          // the main event, from its preparation to its end; 0 none
#define EVM_FLAGS 0xb8                         // u32: 0x4000 an accepted request waits
#define EV_NAME 0x08                           // char *: the event's name
#define FLAGS_MGR (CS_MAIN + 0x4e842a8)        // *: the game's flag database (coop_game.c)
#define PLAYER_HOLDER (CS_MAIN + 0x4e371d0)    // *: +0x480 Link's actor
#define A_DEF_NAME 0x260                       // char *: an actor pack's name
#define A_POS 0x31c

// ---- small helpers

static int rd(uint64_t a, void *out, size_t n) { return a && plat_mem_read(a, out, n); }
static uint64_t rd_ptr(uint64_t a) {
    uint64_t p = 0;
    return rd(a, &p, 8) ? p : 0;
}
static void rd_text(uint64_t a, char *out, size_t cap) {
    out[0] = 0;
    if (!a || !rd(a, out, cap - 1)) {
        out[0] = 0;
        return;
    }
    out[cap - 1] = 0;
    for (size_t i = 0; out[i]; i++)
        if ((unsigned char)out[i] < 0x20 || (unsigned char)out[i] >= 0x7f) out[i] = '?';
}

static uint32_t murmur3(const uint8_t *p, size_t len) {
    uint32_t h = 0;
    size_t n = len & ~(size_t)3;
    for (size_t i = 0; i < n; i += 4) {
        uint32_t k;
        memcpy(&k, p + i, 4);
        k *= 0xcc9e2d51u, k = (k << 15) | (k >> 17), k *= 0x1b873593u;
        h ^= k, h = (h << 13) | (h >> 19), h = h * 5 + 0xe6546b64u;
    }
    uint32_t k = 0;
    switch (len & 3) {
    case 3: k ^= (uint32_t)p[n + 2] << 16;   // fall through
    case 2: k ^= (uint32_t)p[n + 1] << 8;    // fall through
    case 1:
        k ^= p[n];
        k *= 0xcc9e2d51u, k = (k << 15) | (k >> 17), k *= 0x1b873593u;
        h ^= k;
    }
    h ^= (uint32_t)len;
    h ^= h >> 16, h *= 0x85ebca6bu, h ^= h >> 13, h *= 0xc2b2ae35u, h ^= h >> 16;
    return h;
}
static uint32_t hash_of(const char *s) { return murmur3((const uint8_t *)s, strlen(s)); }

// A Bool of the game's flag database (coop_game.c's layout): -1 when it is not there.
static int flag_bool(uint32_t hash) {
    uint64_t mgr = rd_ptr(FLAGS_MGR), c = mgr ? mgr + 0x008 : 0, table = c ? rd_ptr(c + 0x18) : 0, entries = c ? rd_ptr(c + 0x10) : 0;
    uint32_t cap = 0, count = 0;
    if (!table || !entries || !rd(c + 0x24, &cap, 4) || !rd(c + 0x08, &count, 4) || !cap || cap > 1000000) return -1;
    for (uint32_t i = hash % cap, k = 0; k < cap; k++, i = i + 1 == cap ? 0 : i + 1) {
        uint32_t pr[2];
        if (!rd(table + 8ull * i, pr, 8) || !pr[0]) return -1;
        if (pr[0] == hash) {
            uint8_t v = 0;
            return pr[1] < count && rd(entries + (uint64_t)pr[1] * 0x18 + 0x16, &v, 1) ? v & 1 : -1;
        }
    }
    return -1;
}

static int link_adult(uint64_t player) {
    char n[64];
    rd_text(player ? rd_ptr(player + A_DEF_NAME) : 0, n, sizeof n);
    return strstr(n, "Adult") != NULL;
}

// The bytes of CUTSCENE, TIME and ERA: coop_ext_wire.c (coop_cut_*, coop_time_*, coop_era_*).

// ---- state

enum { F_IDLE, F_WAIT, F_WARP, F_START, F_PLAY, F_RETURN };
static const char *const f_names[] = {"idle", "waiting until free", "teleporting", "starting", "playing", "going back"};
enum { BOSS_NEVER, BOSS_AREA, BOSS_ALL };
enum { NOUT = 16, NTEST = 8, NIN = 8 };
enum {
    OUT_KEEP_MS = 10000,    // a message the room's queue did not take within this is dropped
    ORDER_STEP = 1000,      // orders further than this from the one this game knows are taken from the room's host only
    FOLLOW_TRIES = 3,       // teleports a follow asks for while its 45 s last (the game refuses them in menus)
    FAR_SAY_MS = 30000,
};

typedef struct {
    int used;
    uint32_t n;                  // the order it was queued in
    uint64_t t0;
    uint8_t kind;
    uint16_t len;
    uint8_t buf[COOP_ERA_MSG_MAX];   // the largest message kind this module sends (ERA)
} Out;

// A receiver's window per sender's seat: a message is taken once, and none older than the last taken from the same
// player (the seat's gen) and process (epoch). Numbers are compared as serial numbers.
typedef struct { uint32_t gen, epoch, seq; int any; } Win;

static int win_take(Win *w, uint32_t gen, uint32_t epoch, uint32_t seq) {   // 1: new
    if (w->any && w->gen == gen && w->epoch == epoch && (int32_t)(seq - w->seq) <= 0) return 0;
    w->any = 1, w->gen = gen, w->epoch = epoch, w->seq = seq;
    return 1;
}

typedef struct {
    int kind;   // 0 start, 1 follow, 2 settime, 3 warp
    int after_other;
    double at;
    int done;
    char event[COOP_CUT_EVENT_MAX + 1], area[COOP_CUT_AREA_MAX + 1];
    float xyz[3];
} Test;

static struct {
    PlatMutex lock;              // the inboxes, the windows, the seats and the outbox
    unsigned cfg_seen;
    int on, boot, back, bosses, hide, trace, loop;
    int active;                  // the frame hook does this module's work (present thread; read by the game thread)
    Test test[NTEST];
    int ntest;
    uint32_t epoch, seq;
    _Atomic int others, self, host;   // the other players in the room; this game's seat, the host's (0 outside a room)
    // the seats as the co-op thread last saw them (C.lock): each one's player (gen, 0 none) and name
    uint32_t seat_gen[COOP_PLAYERS + 1];
    char seat_name[COOP_PLAYERS + 1][COOP_NAME_CAP];
    uint32_t room_seen;          // the glue's count of rooms while in one, 0 outside
    int f_reset;                 // C.lock: the room changed: a follow of the last room's player ends
    // time of day: the switch, the inbox (co-op thread -> game thread, C.lock)
    int time_on;
    struct { CoopTimeMsg m; int from; uint64_t at; } t_in[NIN];
    int nt_in;
    int t_reset, t_greet;        // the room changed; a player came (the host sends the room's time at once)
    Win t_win[COOP_PLAYERS + 1];
    // the inbox (co-op thread -> game thread)
    CoopCutMsg in;
    int in_have, in_from, in_fake;
    uint64_t in_at;
    CoopCutMsg end;
    int end_have, end_from;
    Win win[COOP_PLAYERS + 1];
    Out out[NOUT];
    uint32_t out_n;
    // the leader (game thread)
    uint64_t ev;
    char name[COOP_CUT_EVENT_MAX + 1];
    uint64_t ev_t0;
    int story, sent, played_at_start;
    char sent_name[COOP_CUT_EVENT_MAX + 1];
    // the follower (game thread)
    int f;
    CoopCutMsg fm;
    int f_from;
    uint64_t f_until, f_retry, f_quiet, f_t0, f_arrived;
    int f_tries, f_hold, f_warps;
    unsigned f_arrivals;         // dbg_arrivals when the follow's teleport was asked for
    char home_area[COOP_CUT_AREA_MAX + 1];
    float home[3];
    // the clock of the tests (game thread)
    uint64_t player_at, other_at;
    int hides;                   // coop_cutscene_hides_puppets (game thread)
    // the time probe: the world time object's words (NX2NAVI_COOP_TIME_PROBE=<seconds between looks>)
    double time_probe;
    uint64_t probe_at, probe_obj;
    uint32_t probe_words[0x60];
    // time of day (game thread)
    uint32_t tl;                 // the newest time order this game knows in this room (a Lamport clock)
    int t_known;                 // ... one is known (the first one heard is taken as it is)
    uint32_t t_last, t_seq;
    int t_have_last, t_pend, t_pend_jump, t_pend_from, t_keeper, t_greet_due, t_low;
    uint32_t t_target, t_expect;
    uint64_t t_beat_at, t_pend_at, t_expect_until, t_moved_at, t_reply_at, t_far_said;
    // story scenes played here lately (any role): a START of one of them is not followed (a chain's scenes come back
    // from the other players' worlds as their own STARTs)
    struct { uint32_t h; uint64_t at; } played[16];
    int nplayed;
    int ev_role;                 // the main event playing here: 0 led, 1 followed, 2 a scene of the chain of a followed one
    // the chain of a followed scene: the story scenes that follow it here by themselves (with changes of area between
    // them, as the Master Sword's) are this game's copies of the leader's, so they are neither sent nor followed again
    uint64_t chain_until;
    int chain_from, chain_hold;  // ... and progress sync held through it (from the followed scene's hold)
    // the chain of story scenes this game leads: its first scene names a change of era that happens in it
    char lc_name[COOP_CUT_EVENT_MAX + 1], lc_area[COOP_CUT_AREA_MAX + 1];
    float lc_xyz[3];
    int lc_adult;
    uint64_t lc_until;
    // the era (NX2NAVI_COOP_ERA): the setting; the inbox (co-op thread -> game thread, C.lock)
    int era_mode, cfg_said_era;
    struct { CoopEraMsg m; int from; } e_in[NIN];
    int ne_in;
    int e_reset, e_greet;        // the room changed; a player came (the host sends the room's era at once)
    Win e_win[COOP_PLAYERS + 1];
    uint32_t e_seq;
    // the era (game thread)
    unsigned e_serial;           // coop_game_save_serial when last read
    int e_mine;                  // this game's era: -1 not known, 0 child, 1 adult (Sequence_IsChildhood)
    uint32_t e_order;            // the newest era order this game knows in this room (a Lamport clock)
    int e_room, e_from;          // the room's era at that order (-1 not known) and the seat behind it (this game's own too)
    CoopEraMsg e_last;           // that change's scene and place
    uint32_t e_change;           // one more each time the room's era is taken anew (never 0)
    uint64_t e_beat_at, e_reply_at, e_behind_at, e_far_said;
    int e_low, e_greet_due;      // a seat heard with an older order (it hears the newest); a beat owed to a newcomer
    // what the catch-up did for a change (e_change; 0 nothing yet): its scene followed, teleports asked (and how many),
    // the waits and the end said
    uint32_t e_scene_tried, e_switch_tried, e_said_behind, e_said_stuck;
    int e_switch_n;
    int e_expect;                // the era a teleport of this module brings (-1 none)
    uint64_t e_expect_until;
    unsigned e_arrivals;         // dbg_arrivals when that teleport was asked for
    int e_keeper, e_names;
} C = {.lock = PLAT_MUTEX_INIT};

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void esay(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void on_time(int from, const uint8_t *b, size_t n, void *user);
static void on_era(int from, const uint8_t *b, size_t n, void *user);

// ---- the time probe: the world's time of day. The event flows' ExecuteWmSetTime node (its name getter in the vtable slot main+0x49e2bf0;
// execute main+0x2890e80) sets it: main+0x34522d0(wm = **[main+0x4c7e898], seconds of the day = hours * 3600 + minutes * 60),
// which hands it to the time part *(wm+0x510) (main+0xeee640(time, seconds, 1, -1)). The probe logs the words of the
// time part that change, to find where the current time is kept; the test settime calls the game's own setter.
#define WM_SLOT (CS_MAIN + 0x4c7e898)
#define WM_SET_TIME (CS_MAIN + 0x34522d0)
static uint64_t world_mgr(void) { return rd_ptr(rd_ptr(WM_SLOT)); }

static void time_probe(uint64_t now) {
    if (now < C.probe_at) return;
    C.probe_at = now + (uint64_t)(C.time_probe * 1000);
    uint64_t wm = world_mgr(), t = wm ? rd_ptr(wm + 0x510) : 0;
    uint32_t w[0x60];
    if (!t || !rd(t, w, sizeof w)) {
        say("time probe: no world time part (world manager %#llx)", (unsigned long long)wm);
        return;
    }
    char line[400];
    int o = snprintf(line, sizeof line, "time probe: part %#llx:", (unsigned long long)t), n = 0;
    for (int i = 0; i < 0x60 && o < (int)sizeof line - 48; i++) {
        if (t == C.probe_obj && w[i] == C.probe_words[i]) continue;
        float f;
        memcpy(&f, &w[i], 4);
        if (t != C.probe_obj && !w[i]) continue;
        o += snprintf(line + o, sizeof line - o, " +%#x=%u(%.3g)", i * 4, w[i], (double)f), n++;
    }
    say("%s%s", line, n ? "" : " no word changed");
    C.probe_obj = t;
    memcpy(C.probe_words, w, sizeof w);
}

static void say(const char *fmt, ...) {
    char line[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    rt_log("coop: cutscene: %s", line);
}

static void esay(const char *fmt, ...) {
    char line[480];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    rt_log("coop: era: %s", line);
}

static int coop_on(void) { return cfg_bool("NX2NAVI_COOP_STEAM"); }

// "seat N 'name'" for a log line (any thread but the co-op thread's handlers, which hold no lock here either): seat 0
// is a test's message, and this game's own seat says so.
typedef struct { char t[COOP_NAME_CAP + 40]; } Who;
static const char *who(int seat, Who *w) {
    if (seat < 1 || seat > COOP_PLAYERS) {
        snprintf(w->t, sizeof w->t, "a test");
        return w->t;
    }
    if (seat == atomic_load(&C.self)) {
        snprintf(w->t, sizeof w->t, "this game (seat %d)", seat);
        return w->t;
    }
    plat_mutex_lock(&C.lock);
    snprintf(w->t, sizeof w->t, "seat %d '%s'", seat, C.seat_name[seat][0] ? C.seat_name[seat] : "(name not known yet)");
    plat_mutex_unlock(&C.lock);
    return w->t;
}

// How order o stands to the order known (serial numbers): the same, newer or older within ORDER_STEP, else far.
enum { O_SAME, O_NEWER, O_OLDER, O_FAR };
static int order_rel(uint32_t o, uint32_t known) {
    int32_t d = (int32_t)(o - known);
    return !d ? O_SAME : d > 0 && d <= ORDER_STEP ? O_NEWER : d < 0 && d >= -ORDER_STEP ? O_OLDER : O_FAR;
}

// Within one order, whose word wins: the room's host's, then the lower player id (the same in every game). 1 when
// seat a's player ranks above seat b's (a seat with nobody in it ranks last).
static int ranks_above(int a, int b) {
    CoopExtView v;
    coop_ext_view(&v);
    const CoopSeat *x = a >= 1 && a <= COOP_PLAYERS ? &v.seat[a] : NULL, *y = b >= 1 && b <= COOP_PLAYERS ? &v.seat[b] : NULL;
    if (!x || !x->id) return 0;
    if (!y || !y->id) return 1;
    if (x->host != y->host) return x->host;
    return x->id < y->id;
}

// ---- the co-op thread

// A message for the room, sent from the co-op thread once (in the order queued); when the queue is full the oldest
// gives way.
static void queue_out(uint8_t kind, const uint8_t *buf, size_t n) {
    plat_mutex_lock(&C.lock);
    int slot = -1;
    for (int i = 0; i < NOUT && slot < 0; i++)
        if (!C.out[i].used) slot = i;
    if (slot < 0) {
        slot = 0;
        for (int i = 1; i < NOUT; i++)
            if ((int32_t)(C.out[i].n - C.out[slot].n) < 0) slot = i;
    }
    Out *o = &C.out[slot];
    o->used = 1, o->n = ++C.out_n, o->t0 = plat_now_ms(), o->kind = kind, o->len = (uint16_t)n;
    memcpy(o->buf, buf, n);
    plat_mutex_unlock(&C.lock);
}

static void queue_msg(const CoopCutMsg *m) {
    uint8_t buf[COOP_CUT_MSG_MAX];
    size_t n = coop_cut_pack(m, buf, sizeof buf);
    if (!n) {
        say("a %s message for '%s' does not pack: not sent", m->op == COOP_CUT_START ? "START" : "END", m->event);
        return;
    }
    if (C.loop) {
        CoopCutMsg back;
        int ok = coop_cut_parse(buf, n, &back);
        say("loop: %s seq %u, %zu bytes, parsed back %d: '%s' in '%s' at %.1f %.1f %.1f flags %u", m->op == COOP_CUT_START ? "START" : "END",
            m->seq, n, ok, back.event, back.area, back.xyz[0], back.xyz[1], back.xyz[2], back.flags);
    }
    queue_out(COOP_K_CUTSCENE, buf, n);
}

static void on_msg(int from, const uint8_t *bytes, size_t n, void *user) {
    (void)user;
    if (from < 1 || from > COOP_PLAYERS || from == atomic_load(&C.self)) return;
    CoopCutMsg m;
    char name[COOP_NAME_CAP];
    plat_mutex_lock(&C.lock);
    snprintf(name, sizeof name, "%s", C.seat_name[from][0] ? C.seat_name[from] : "(name not known yet)");
    plat_mutex_unlock(&C.lock);
    if (!coop_cut_parse(bytes, n, &m)) {
        coop_ext_log(from, "cutscene: a CUTSCENE of %zu bytes from seat %d '%s' does not parse; dropped", n, from, name);
        return;
    }
    plat_mutex_lock(&C.lock);
    if (!win_take(&C.win[from], C.seat_gen[from], m.epoch, m.seq)) {   // sent twice, or an older one
        plat_mutex_unlock(&C.lock);
        return;
    }
    if (m.op == COOP_CUT_START) C.in = m, C.in_have = 1, C.in_from = from, C.in_fake = 0, C.in_at = plat_now_ms();
    if (m.op == COOP_CUT_END) C.end = m, C.end_have = 1, C.end_from = from;
    plat_mutex_unlock(&C.lock);
    coop_ext_log(from, "cutscene: seat %d '%s' %s '%s' in '%s' at %.1f %.1f %.1f (%s%s)", from, name,
                 m.op == COOP_CUT_START ? "starts" : "ends", m.event, m.area, m.xyz[0], m.xyz[1], m.xyz[2],
                 m.flags & COOP_CUT_ADULT ? "adult" : "child", m.flags & COOP_CUT_MOVIE ? ", a movie, it says" : "");
}

static void on_view(const CoopExtView *v, void *user) {
    (void)user;
    int others = 0, came = 0;
    uint32_t room = v->in_room ? v->room : 0;
    plat_mutex_lock(&C.lock);
    for (int s = 1; s <= COOP_PLAYERS; s++) {
        const CoopSeat *t = &v->seat[s];
        uint32_t gen = v->in_room && t->id ? t->gen : 0;
        if (gen != C.seat_gen[s]) came |= gen && !t->self, C.seat_gen[s] = gen;
        snprintf(C.seat_name[s], sizeof C.seat_name[s], "%s", gen ? t->name : "");
        others += gen && !t->self;
    }
    if (room != C.room_seen) {   // another room (or none): its time and era are learnt anew, and nothing of the last stays
        C.room_seen = room;
        C.t_reset = C.e_reset = C.f_reset = 1;
        C.nt_in = C.ne_in = 0, C.in_have = C.end_have = 0;
    }
    if (came) C.t_greet = C.e_greet = 1;
    plat_mutex_unlock(&C.lock);
    atomic_store(&C.self, v->in_room ? v->self : 0);
    atomic_store(&C.host, v->in_room ? v->host : 0);
    atomic_store(&C.others, others);
}

// The messages waiting to go out, each once, in the order they were queued; what the room's queue does not take now
// waits for the next step, OUT_KEEP_MS at most.
static void on_tick(uint64_t now, void *user) {
    (void)user;
    static struct { int slot; uint32_t n; uint8_t kind; uint16_t len; uint8_t buf[COOP_ERA_MSG_MAX]; } go[NOUT];
    int ng = 0, expired = 0;
    plat_mutex_lock(&C.lock);
    for (int i = 0; i < NOUT; i++) {
        Out *o = &C.out[i];
        if (!o->used) continue;
        if (now > o->t0 && now - o->t0 > OUT_KEEP_MS) {   // t0 is the game thread's clock, a moment after this step's now
            o->used = 0, expired++;
            continue;
        }
        int j = ng++;
        while (j > 0 && (int32_t)(o->n - go[j - 1].n) < 0) go[j] = go[j - 1], j--;
        go[j].slot = i, go[j].n = o->n, go[j].kind = o->kind, go[j].len = o->len;
        memcpy(go[j].buf, o->buf, o->len);
    }
    plat_mutex_unlock(&C.lock);
    int sent = 0;
    while (sent < ng && coop_ext_send(0, go[sent].kind, go[sent].buf, go[sent].len, 1)) sent++;
    if (sent) {
        plat_mutex_lock(&C.lock);
        for (int i = 0; i < sent; i++)
            if (C.out[go[i].slot].used && C.out[go[i].slot].n == go[i].n) C.out[go[i].slot].used = 0;
        plat_mutex_unlock(&C.lock);
    }
    static uint64_t said;
    if ((expired || sent < ng) && now - said >= 10000) {
        said = now;
        say("%d messages wait for the room's queue%s", ng - sent,
            expired ? "; some waited too long and were dropped (not in a room, or the queue stayed full)" : "");
    }
}

void coop_cutscene_init(void) {
    uint64_t r = plat_now_ns();
    C.epoch = (uint32_t)(r ^ r >> 32 ^ (uint64_t)(uintptr_t)&C) | 1u;
    C.e_mine = C.e_room = C.e_expect = -1;
    C.e_change = 1;
    coop_ext_on_message(COOP_K_CUTSCENE, on_msg, NULL);
    coop_ext_on_message(COOP_K_TIME, on_time, NULL);
    coop_ext_on_message(COOP_K_ERA, on_era, NULL);
    coop_ext_on_view(on_view, NULL);
    coop_ext_on_tick(on_tick, NULL);
}

// ---- the present thread: settings

static int is0(const char *v) { return v && !strcmp(v, "0"); }

int coop_era_setting(const char **raw) {
    const char *v = cfg_get("NX2NAVI_COOP_ERA");   // no row in the settings table
    if (raw) *raw = v ? v : "";
    if (!v || !*v || !strcmp(v, "follow")) return COOP_ERA_FOLLOW;
    if (!strcmp(v, "scene")) return COOP_ERA_SCENE;
    if (!strcmp(v, "shared")) return COOP_ERA_SHARED;
    if (!strcmp(v, "own") || !strcmp(v, "0") || !strcmp(v, "off")) return COOP_ERA_OWN;
    return COOP_ERA_FOLLOW;   // an unknown word: the default (the start's line shows the text)
}

static void parse_tests(const char *v) {
    C.ntest = 0, C.loop = 0;
    char buf[512];
    snprintf(buf, sizeof buf, "%s", v ? v : "");
    for (char *tok = strtok(buf, ";"); tok; tok = strtok(NULL, ";")) {
        while (*tok == ' ') tok++;
        if (!strcmp(tok, "loop")) {
            C.loop = 1;
            continue;
        }
        if (C.ntest >= NTEST) break;
        Test t = {0};
        char *at = strrchr(tok, '@'), *tilde = strrchr(tok, '~'), *when = at ? at : tilde;
        if (!when) {
            say("test '%s': no @<seconds> or ~<seconds>: left out", tok);
            continue;
        }
        t.after_other = when == tilde && !at;
        t.at = atof(when + 1);
        *when = 0;
        int hh = 0, mm = 0;
        if (!strncmp(tok, "start:", 6) && sscanf(tok + 6, "%79s", t.event) == 1) {
            t.kind = 0;
        } else if (!strncmp(tok, "settime:", 8) && sscanf(tok + 8, "%d:%d", &hh, &mm) == 2 && hh >= 0 && hh < 24 && mm >= 0 && mm < 60) {
            t.kind = 2;   // the game's own time setter, as the event flows' ExecuteWmSetTime
            snprintf(t.event, sizeof t.event, "%02d:%02d", hh, mm);
            t.xyz[0] = (float)(hh * 3600 + mm * 60);
        } else if (!strncmp(tok, "follow:", 7) &&
                   sscanf(tok + 7, "%79[^:]:%63[^:]:%f,%f,%f", t.event, t.area, &t.xyz[0], &t.xyz[1], &t.xyz[2]) == 5) {
            t.kind = 1;
        } else if (!strncmp(tok, "warp:", 5) && sscanf(tok + 5, "%79[^:]:%63s", t.event, t.area) >= 1) {
            t.kind = 3;   // the debug teleport, as a test's own change of area (and of era)
        } else {
            say("test '%s': not start:<event>, follow:<event>:<area>:<x>,<y>,<z>, settime:<hh>:<mm> or warp:<area>[:<start "
                "position>]: left out", tok);
            continue;
        }
        C.test[C.ntest++] = t;
        say("test %d: %s '%s'%s%s %s %.1f s", C.ntest,
            t.kind == 3 ? "teleport to" : t.kind == 2 ? "set the time of day to" : t.kind ? "a START as if from another player for" : "start",
            t.event, t.kind == 1 ? " in " : t.kind == 3 ? " " : "", t.kind == 1 || t.kind == 3 ? t.area : "",
            t.after_other ? "after another player is in the world, plus" : "at", t.at);
    }
}

int coop_cutscene_present(void) {
    unsigned v = cfg_version();
    if (v != C.cfg_seen) {
        C.cfg_seen = v;
        const char *on = cfg_get("NX2NAVI_COOP_CUTSCENES"), *boot = cfg_get("NX2NAVI_COOP_CUTSCENE_BOOT"),
                   *ret = cfg_get("NX2NAVI_COOP_CUTSCENE_RETURN"), *boss = cfg_get("NX2NAVI_COOP_CUTSCENE_BOSSES"),
                   *hide = cfg_get("NX2NAVI_COOP_CUTSCENE_HIDE"), *trace = cfg_get("NX2NAVI_COOP_CUTSCENE_TRACE"),
                   *test = cfg_get("NX2NAVI_COOP_CUTSCENE_TEST");
        C.on = !is0(on), C.boot = !is0(boot), C.back = ret && !strcmp(ret, "back"), C.hide = !is0(hide);
        const char *tm = cfg_get("NX2NAVI_COOP_TIME");   // the time of day shared unless "0"
        C.time_on = !is0(tm);
        if (coop_on())
            say("time: the time of day %s (NX2NAVI_COOP_TIME='%s')", C.time_on ? "is shared" : "stays each player's", tm ? tm : "");
        C.bosses = is0(boss) ? BOSS_NEVER : boss && !strcmp(boss, "all") ? BOSS_ALL : BOSS_AREA;
        C.trace = trace && *trace && !is0(trace);
        const char *tp = cfg_get("NX2NAVI_COOP_TIME_PROBE");   // a probe: the world time object's words
        C.time_probe = tp && *tp && !is0(tp) ? atof(tp) : 0;
        if (C.time_probe > 0 && C.time_probe < 1) C.time_probe = 1;
        parse_tests(test);
        if (coop_on() || C.trace || C.ntest || C.loop)
            say("cutscenes %s (NX2NAVI_COOP_CUTSCENES='%s'); arrival event %s, after a scene %s, boss scenes %s, puppets in "
                "scenes %s, trace %s, %d tests%s", C.on ? "together" : "off", on ? on : "", C.boot ? "on" : "off (start by name)",
                C.back ? "back" : "stay", C.bosses == BOSS_NEVER ? "never" : C.bosses == BOSS_ALL ? "always" : "only in the same area",
                C.hide ? "hidden" : "shown", C.trace ? "on" : "off", C.ntest, C.loop ? ", loop" : "");
        const char *era_raw;
        int mode = coop_era_setting(&era_raw);
        if (coop_on() && (mode != C.era_mode || !C.cfg_said_era))
            esay("%s (NX2NAVI_COOP_ERA='%s')",
                 mode == COOP_ERA_FOLLOW  ? "follow: out of progress sync; a change goes to the room with its scene, and a player "
                                            "of another era follows that scene, else is teleported into the room's era"
                 : mode == COOP_ERA_SCENE ? "scene: out of progress sync; a change goes to the room with its scene, and a player "
                                            "of another era follows that scene (no teleport)"
                 : mode == COOP_ERA_SHARED ? "shared: in progress sync (the old way, child to adult only)"
                                           : "own: out of progress sync, each player's own era",
                 era_raw);
        C.cfg_said_era |= coop_on();
        C.era_mode = mode;
    }
    // the era in a room, even alone in it: a change made then reaches those who join later (the host's beat)
    int era_on = (C.era_mode == COOP_ERA_FOLLOW || C.era_mode == COOP_ERA_SCENE) && atomic_load(&C.self) > 0;
    int want = RT_V111 && dbg_supported() &&
               (C.trace || C.ntest || C.time_probe > 0 || ((C.on || C.time_on) && atomic_load(&C.others) > 0) || era_on);
    if (want) coop_events_state();   // the event table, read once
    __atomic_store_n(&C.active, want, __ATOMIC_RELEASE);
    return want;
}

// ---- the game thread

typedef struct {
    uint64_t ev;
    char name[COOP_CUT_EVENT_MAX + 1];
    uint32_t flags;
} EvNow;

static int ev_now(EvNow *o) {
    memset(o, 0, sizeof *o);
    uint64_t slot = rd_ptr(EVENT_MGR_SLOT), mgr = slot ? rd_ptr(slot) : 0;
    if (!mgr) return 0;
    rd(mgr + EVM_FLAGS, &o->flags, 4);
    o->ev = rd_ptr(mgr + EVM_MAIN);
    if (o->ev) rd_text(rd_ptr(o->ev + EV_NAME), o->name, sizeof o->name);
    return 1;
}

// The room's word may change this world now: co-op's gate is open and the save was copied for the room (without
// co-op, a test's follow has no room and no gate).
static int room_may_act(void) { return !coop_on() || (coop_game_running() && coop_game_backed_up()); }

// ---- time of day, shared (NX2NAVI_COOP_TIME=0 off). The world's time part *(wm+0x510) keeps the time
// of day as seconds at +0x28 (u32 0..86399, about 120 a second while the world runs). The game's own setter
// (the event flows' ExecuteWmSetTime: main+0x34522d0(wm, seconds)) moves it, sky and all, as a scene or the Sun's
// Song does. The room's host keeps the room's time: every 2 s it sends TIME (its time and the newest time order it
// knows), and at once when a player comes. A game whose own clock jumps (the Sun's Song, a scene, a bed: more than 10
// game minutes in a frame, not made by this module) makes a newer order and sends it at once; every game, the host
// included, takes the newest order's time, and within one order the host's (orders: the file's top). A game that hears
// an older order than its own answers with its newest (at most every 2 s). A game sets its time only when free (in the
// world, no event or request, the debug tools idle, not following a scene, the save copied for the room), and for the
// host's beat only when it is more than 5 game minutes off. While a time it took waits to be set here, its own clock
// is not the room's time: it sends neither beats nor answers until it is set.
#define TOD_OFF 0x28

static uint32_t tod_dist(uint32_t a, uint32_t b) {   // game seconds between two times of day, either way round
    uint32_t d = (a + COOP_TIME_DAY - b) % COOP_TIME_DAY;
    return d < COOP_TIME_DAY - d ? d : COOP_TIME_DAY - d;
}

static const char *hhmm(uint32_t tod, char out[8]) {
    snprintf(out, 8, "%02u:%02u", (tod / 3600) % 24, (tod / 60) % 60);
    return out;
}

static void queue_time(uint32_t order, uint32_t tod, uint8_t flags) {
    CoopTimeMsg m = {.epoch = C.epoch, .seq = ++C.t_seq, .order = order, .tod = tod, .flags = flags};
    uint8_t buf[COOP_TIME_MSG];
    size_t n = coop_time_pack(&m, buf, sizeof buf);
    if (n) queue_out(COOP_K_TIME, buf, n);
}

static void on_time(int from, const uint8_t *b, size_t n, void *user) {
    (void)user;
    if (from < 1 || from > COOP_PLAYERS || from == atomic_load(&C.self)) return;
    CoopTimeMsg m;
    if (!coop_time_parse(b, n, &m)) {
        coop_ext_log(from, "time: a TIME of %zu bytes from seat %d does not parse; dropped", n, from);
        return;
    }
    plat_mutex_lock(&C.lock);
    if (win_take(&C.t_win[from], C.seat_gen[from], m.epoch, m.seq)) {
        if (C.nt_in == NIN) memmove(C.t_in, C.t_in + 1, sizeof C.t_in[0] * (NIN - 1)), C.nt_in--;   // the oldest gives way
        C.t_in[C.nt_in].m = m, C.t_in[C.nt_in].from = from, C.t_in[C.nt_in].at = plat_now_ms(), C.nt_in++;
    }
    plat_mutex_unlock(&C.lock);
}

// The game thread, every frame of a room with others: the beat, this game's own jumps, and the room's time.
static void time_frame(Cpu *c, const EvNow *e, uint64_t player, const char *area, uint64_t now) {
    struct { CoopTimeMsg m; int from; uint64_t at; } in[NIN];
    plat_mutex_lock(&C.lock);
    int reset = C.t_reset, greet = C.t_greet, nin = C.nt_in;
    memcpy(in, C.t_in, sizeof in[0] * (size_t)nin);
    C.t_reset = C.t_greet = 0, C.nt_in = 0;
    plat_mutex_unlock(&C.lock);
    if (reset) {   // another room: its time order starts from what is heard there
        C.tl = 0, C.t_known = 0, C.t_pend = C.t_pend_jump = 0, C.t_expect_until = 0, C.t_low = 0, C.t_beat_at = 0;
        C.t_greet_due = 0;
    }
    uint64_t wm = world_mgr(), part = wm ? rd_ptr(wm + 0x510) : 0;
    uint32_t tod = 0;
    if (!part || !rd(part + TOD_OFF, &tod, 4) || tod >= COOP_TIME_DAY) return;
    int in_world = player && area[0];
    char a[8], b[8];
    Who w;
    int self = atomic_load(&C.self), host = atomic_load(&C.host);
    int keeper = self && self == host;
    if (keeper != C.t_keeper) {
        C.t_keeper = keeper;
        say("time: %s", keeper ? "this game keeps the room's time (the room's host): its time goes out every 2 s"
                               : "the room's host keeps the room's time");
    }
    // a jump of this game's own clock
    if (C.t_have_last && tod_dist(tod, C.t_last) > 600) {
        if (now < C.t_expect_until && tod_dist(tod, C.t_expect) < 300) {
            C.t_expect_until = 0;   // the room's time, set here
        } else if (in_world) {
            C.tl++, C.t_known = 1;
            queue_time(C.tl, tod, COOP_TIME_JUMP);
            say("time: this game's clock jumped from %s to %s: sent to the room (order %u)", hhmm(C.t_last, a), hhmm(tod, b), C.tl);
        }
    }
    if (!C.t_have_last || tod != C.t_last) C.t_moved_at = now;
    int running = now - C.t_moved_at < 1500;   // the clock stands in menus, scenes, loads (and where the game stops it)
    C.t_last = tod, C.t_have_last = 1;
    // what the room sent, in the order it came
    for (int i = 0; i < nin; i++) {
        const CoopTimeMsg *m = &in[i].m;
        int from = in[i].from, jump = m->flags & COOP_TIME_JUMP, from_host = from == host;
        int rel = order_rel(m->order, C.tl);   // tl is 0 until an order is known in this room
        if (rel == O_NEWER || (rel == O_FAR && from_host) || (rel == O_SAME && !keeper && (jump || from_host))) {
            if (rel != O_SAME || jump || !C.t_known) C.t_pend_jump = 1;   // the first time heard is set as a jump
            C.tl = m->order, C.t_known = 1;
            C.t_pend = 1, C.t_target = m->tod, C.t_pend_at = in[i].at, C.t_pend_from = from;
        } else if (rel == O_OLDER) {
            C.t_low = from;
        } else if (rel == O_FAR && now - C.t_far_said >= FAR_SAY_MS) {
            C.t_far_said = now;
            say("time: %s sent order %u, far from this game's %u, and is not the room's host: dropped", who(from, &w), m->order, C.tl);
        }
    }
    // the beat (at once for a newcomer), and the newest order for a player that still has an older one: this game's own
    // clock, so only while no time it took waits to be set here
    if (greet) C.t_greet_due = 1;
    if (!keeper) C.t_greet_due = 0;
    if (keeper && in_world && !C.t_pend && ((running && now >= C.t_beat_at) || C.t_greet_due)) {
        C.t_beat_at = now + 2000, C.t_greet_due = 0;
        queue_time(C.tl, tod, 0);
    }
    if (C.t_low && C.t_known && in_world && !C.t_pend && now >= C.t_reply_at) {
        C.t_reply_at = now + 2000;
        queue_time(C.tl, tod, 0);
        say("time: %s still has an older order: the room's newest (%u, %s) sent", who(C.t_low, &w), C.tl, hhmm(tod, a));
        C.t_low = 0;
    }
    if (!C.t_pend || !in_world || e->ev || (e->flags & 0x4000) || dbg_busy() || C.f != F_IDLE) return;
    if (coop_on() && !coop_game_backed_up()) return;   // nothing of the room before the save's copy
    if (!C.t_pend_jump && !running) return;   // a beat only sets a clock that runs (it waits for the next beat)
    // the time it is there now: what was sent, plus the game seconds since (about 120 a second)
    uint32_t target = (C.t_target + (uint32_t)((now - C.t_pend_at) * 120 / 1000)) % COOP_TIME_DAY, off = tod_dist(tod, target);
    if (off > (C.t_pend_jump ? 60u : 300u)) {
        rt_call(c, WM_SET_TIME, 2, wm, (uint64_t)target);
        C.t_expect = target, C.t_expect_until = now + 3000;
        say("time: set to %s (%s's %s, order %u; it was %s here, %u game seconds off)", hhmm(target, a), who(C.t_pend_from, &w),
            C.t_pend_jump ? "jump" : "beat", C.tl, hhmm(tod, b), off);
    }
    C.t_pend = 0, C.t_pend_jump = 0;
}

static int played_flag(const char *event) {
    char f[128];
    snprintf(f, sizeof f, "HavePlayedEvent.%s", event);
    return flag_bool(hash_of(f));
}

static double secs(uint64_t from, uint64_t now) { return from ? (double)(now - from) / 1000.0 : -1.0; }

// Progress sync held while this game follows a scene or plays the chain of a followed one.
static void hold_now(void) { coop_game_hold(C.f_hold || C.chain_hold); }

static void follow_end(const char *why, uint64_t now) {
    if (C.f_hold) C.f_hold = 0, hold_now();
    say("follow of '%s' ends after %.1f s: %s", C.fm.event, secs(C.f_t0, now), why);
    C.f = F_IDLE;
}

// Story scenes played here lately: noted at their start (any role), asked for by name.
static void played_note(const char *event, uint64_t now) {
    uint32_t h = hash_of(event);
    int slot = 0;
    for (int i = 0; i < 16; i++) {
        if (C.played[i].h == h || !C.played[i].at) {
            slot = i;
            break;
        }
        if (C.played[i].at < C.played[slot].at) slot = i;   // else the oldest gives way
    }
    C.played[slot].h = h, C.played[slot].at = now;
}
static uint64_t played_ago(const char *event, uint64_t now) {   // ms, UINT64_MAX when not lately
    uint32_t h = hash_of(event);
    for (int i = 0; i < 16; i++)
        if (C.played[i].at && C.played[i].h == h) return now - C.played[i].at;
    return UINT64_MAX;
}

enum {
    CHAIN_MS = 30000,       // a story scene that starts within this of the end of one of a chain belongs to it
    CHAIN_PLAYING = 600000, // the chain's window while one of its scenes plays
    PLAYED_MS = 300000,     // a START of a scene this game played within this is not followed
};

static void leader(const EvNow *e, uint64_t player, const char *area, uint64_t now) {
    if (e->ev == C.ev && !strcmp(e->name, C.name)) return;
    if (C.ev) {   // the event that played has ended (or another took its place)
        if (C.trace || C.story)
            say("event '%s' ends after %.1f s%s (HavePlayedEvent %d at its start, %d now)", C.name, secs(C.ev_t0, now),
                C.story ? ", a story cutscene" : "", C.played_at_start, played_flag(C.name));
        if (C.story) {   // a story scene that starts soon after this one is the next of its chain
            if (C.ev_role) C.chain_until = now + CHAIN_MS;
            else C.lc_until = now + CHAIN_MS;
        }
        if (C.sent) {
            CoopCutMsg m = {.op = COOP_CUT_END, .epoch = C.epoch, .seq = ++C.seq};
            snprintf(m.event, sizeof m.event, "%s", C.sent_name);
            snprintf(m.area, sizeof m.area, "%s", area);
            if (player) rd(player + A_POS, m.xyz, 12);
            queue_msg(&m);
            say("END '%s' sent (seq %u)", C.sent_name, m.seq);
            C.sent = 0;
        }
    }
    C.ev = e->ev, C.ev_t0 = now, C.story = 0;
    snprintf(C.name, sizeof C.name, "%s", e->name);
    if (!e->ev) return;
    const CoopStory *s = coop_events_story(e->name);
    C.story = s != NULL;
    C.played_at_start = played_flag(e->name);
    if (C.trace || s)
        say("event '%s' starts in '%s' (%s%s; HavePlayedEvent %d; manager flags %#x)", e->name, area,
            s ? "a story cutscene" : "not a story cutscene", s && s->movie ? ", a movie" : "", C.played_at_start, e->flags);
    if (!s) return;
    played_note(e->name, now);
    // its role: followed (a follow plays it), a scene of a followed one's chain (it comes by itself soon after one), or
    // led; a led scene starts a chain of this game's own, or is the next of one
    C.ev_role = C.f != F_IDLE ? 1 : now < C.chain_until ? 2 : 0;
    if (C.ev_role == 1) C.chain_from = C.f_from;
    if (C.ev_role) {
        C.chain_until = now + CHAIN_PLAYING;
    } else {
        if (now >= C.lc_until) {
            snprintf(C.lc_name, sizeof C.lc_name, "%s", e->name);
            snprintf(C.lc_area, sizeof C.lc_area, "%s", area);
            memset(C.lc_xyz, 0, sizeof C.lc_xyz);
            if (player) rd(player + A_POS, C.lc_xyz, 12);
            C.lc_adult = C.e_mine >= 0 ? C.e_mine : player && link_adult(player);
        }
        C.lc_until = now + CHAIN_PLAYING;
    }
    Who w;
    char chain_why[160];
    snprintf(chain_why, sizeof chain_why, "it comes here after the scene of %s this game followed (this world's copy of it)",
             who(C.chain_from, &w));
    const char *why = !C.on ? "cutscenes are off here (NX2NAVI_COOP_CUTSCENES=0)"
                    : C.f != F_IDLE ? "this game plays it as a follower"
                    : C.ev_role == 2 ? chain_why
                    : s->boss && C.bosses == BOSS_NEVER ? "boss scenes are not shared (NX2NAVI_COOP_CUTSCENE_BOSSES=0)"
                    : !atomic_load(&C.others) && !C.loop ? "no other player in the room"
                    : NULL;
    if (why) {
        say("'%s' not sent: %s", e->name, why);
        return;
    }
    CoopCutMsg m = {.op = COOP_CUT_START, .epoch = C.epoch, .seq = ++C.seq};
    snprintf(m.event, sizeof m.event, "%s", e->name);
    snprintf(m.area, sizeof m.area, "%s", area);
    if (player) rd(player + A_POS, m.xyz, 12);
    m.flags = (uint8_t)((player && link_adult(player) ? COOP_CUT_ADULT : 0) | (s->movie ? COOP_CUT_MOVIE : 0));
    queue_msg(&m);
    C.sent = 1;
    snprintf(C.sent_name, sizeof C.sent_name, "%s", e->name);
    say("START '%s' in '%s' at %.1f %.1f %.1f sent (seq %u, %s%s)", m.event, m.area, m.xyz[0], m.xyz[1], m.xyz[2], m.seq,
        m.flags & COOP_CUT_ADULT ? "adult" : "child", s->movie ? ", a movie" : "");
}

// A START taken from the inbox (fake 1: a test's; 2: the era's catch-up): followed or not; 1 when followed. Whether the
// scene is a movie (played where the player stands, no teleport) is this game's own table's word, not the sender's.
static int consider(const CoopCutMsg *m, int from, int fake, uint64_t player, const char *area, uint64_t now) {
    const CoopStory *s = coop_events_story(m->event);
    int adult = player && link_adult(player), same = area[0] && !strcmp(area, m->area);
    uint64_t ago = played_ago(m->event, now);
    Who w;
    char chain_why[160], ago_why[64];
    snprintf(chain_why, sizeof chain_why, "this game plays the scenes of %s in its own world (the chain of the one it followed)",
             who(from, &w));
    snprintf(ago_why, sizeof ago_why, "this game played it %.0f s ago", (double)ago / 1000.0);
    const char *why = !C.on ? "cutscenes are off here (NX2NAVI_COOP_CUTSCENES=0)"
                    : !s ? "not a story cutscene of this game's event table"
                    : C.f != F_IDLE ? "this game already follows a scene"
                    : C.sent ? "this game leads a scene of its own"
                    : now < C.chain_until && from == C.chain_from ? chain_why
                    : ago < PLAYED_MS ? ago_why
                    : !player ? "Link is not in the world"
                    : !!(m->flags & COOP_CUT_ADULT) != adult ? "the other player is of another age"
                    : !m->area[0] && !s->movie ? "no area given"
                    : s->boss && C.bosses == BOSS_NEVER ? "boss scenes are not followed (NX2NAVI_COOP_CUTSCENE_BOSSES=0)"
                    : s->boss && C.bosses == BOSS_AREA && !same ? "a boss scene of another area (NX2NAVI_COOP_CUTSCENE_BOSSES=area)"
                    : NULL;
    if (why) {
        say("the scene '%s' of %s is not followed: %s", m->event, who(from, &w), why);
        return 0;
    }
    C.fm = *m, C.f_from = from, C.f = F_WAIT, C.f_t0 = now, C.f_until = now + 45000, C.f_tries = 0, C.f_arrived = 0, C.f_warps = 0;
    C.fm.flags = (uint8_t)((m->flags & ~COOP_CUT_MOVIE) | (s->movie ? COOP_CUT_MOVIE : 0));
    say("following the scene '%s' of %s%s: waiting until this game is free (45 s at most)", m->event, who(from, &w),
        fake == 2 ? " (to change era as it did)" : fake ? " (test)" : "");
    return 1;
}

static int is_ours(const EvNow *e) { return e->ev && !strcmp(e->name, C.fm.event); }

static void follower(const EvNow *e, uint64_t player, const char *area, uint64_t now) {
    switch (C.f) {
    case F_IDLE:
        return;
    case F_WAIT: {
        if (now > C.f_until) {
            follow_end("this game was not free within 45 s (a talk, a menu, a scene or a load)", now);
            return;
        }
        if (!room_may_act() || !player || !area[0] || e->ev || (e->flags & 0x4000) || dbg_busy()) return;
        if (!C.f_hold) {   // where the player was, for the way back; kept when a refused teleport is asked again
            snprintf(C.home_area, sizeof C.home_area, "%s", area);
            rd(player + A_POS, C.home, 12);
            C.f_hold = 1, hold_now();
        }
        if ((C.fm.flags & COOP_CUT_MOVIE) || !strcmp(area, C.fm.area)) {
            say("'%s': %s: started by its name here", C.fm.event, C.fm.flags & COOP_CUT_MOVIE ? "a movie plays anywhere" : "same area");
            C.f = F_START, C.f_retry = now, C.f_until = now + 20000;
        } else {
            say("'%s': teleporting from '%s' to '%s', near %.1f %.1f %.1f%s", C.fm.event, area, C.fm.area, C.fm.xyz[0], C.fm.xyz[1],
                C.fm.xyz[2], C.boot ? ", the scene reserved for the arrival" : "");
            C.f_arrivals = dbg_arrivals();
            dbg_warp_near(C.fm.area, C.fm.xyz, C.boot ? C.fm.event : "");
            C.f_warps++;
            C.f = F_WARP, C.f_until = now + 60000;
        }
        return;
    }
    case F_WARP:
        if (is_ours(e)) {
            say("'%s' started at the arrival in '%s' (%.1f s after the START came)", C.fm.event, area, secs(C.f_t0, now));
            C.f = F_PLAY, C.f_quiet = 0;
            return;
        }
        if (!C.f_arrived && !strcmp(area, C.fm.area)) C.f_arrived = now;
        if (dbg_busy() && now <= C.f_until) return;
        if (strcmp(area, C.fm.area)) {
            char st[160];
            dbg_status(st);
            if (dbg_arrivals() == C.f_arrivals && now < C.f_t0 + 45000 && C.f_warps < FOLLOW_TRIES) {
                // the game refused it (a menu, Link in the air or swimming): asked again at the next free moment
                say("'%s': the teleport did not happen (the debug tools say: %s); asked again once this game is free", C.fm.event, st);
                C.f = F_WAIT, C.f_until = C.f_t0 + 45000;
                return;
            }
            follow_end(now > C.f_until ? "the teleport took more than 60 s" : "the teleport did not arrive", now);
            say("the debug tools say: %s", st);
            return;
        }
        if (C.f_arrived && now - C.f_arrived < 3000) return;   // the reserved scene's own start, if it comes
        say("'%s' did not start at the arrival: started by its name", C.fm.event);
        C.f = F_START, C.f_retry = now, C.f_until = now + 20000;
        return;
    case F_START:
        if (is_ours(e)) {
            say("'%s' started by its name (%d asks, %.1f s after the START came)", C.fm.event, C.f_tries, secs(C.f_t0, now));
            C.f = F_PLAY, C.f_quiet = 0;
            return;
        }
        if (now > C.f_until) {
            follow_end("the game refused the scene for 20 s", now);
            return;
        }
        if (now >= C.f_retry && !e->ev && !(e->flags & 0x4000) && !dbg_busy()) {
            dbg_event_start(C.fm.event);
            C.f_retry = now + 500, C.f_tries++;
        }
        return;
    case F_PLAY: {
        if (C.f_hold && now - C.f_t0 > 180000) {   // a talk left open, a scene that chains on: the room's values come in
            C.f_hold = 0, hold_now();
            say("'%s': the hold ends after 3 minutes; events still play here", C.fm.event);
        }
        if (e->ev) {
            C.f_quiet = 0;
            return;
        }
        if (!C.f_quiet) C.f_quiet = now;
        if (now - C.f_quiet < 1000) return;
        // the scenes it leads into here by themselves (chain_until) are this world's copies of the leader's: the hold goes
        // on through them, so that the leader's flags cannot change this world's copy of its chain either
        if (C.f_hold) C.f_hold = 0, C.chain_hold = 1, hold_now();
        Who w;
        say("watched '%s' with %s: over here %.1f s after the START came%s", C.fm.event, who(C.f_from, &w), secs(C.f_t0, now),
            C.back && strcmp(C.home_area, area) ? "; going back" : "");
        C.f = C.back && strcmp(C.home_area, area) ? F_RETURN : F_IDLE;
        C.f_until = now + 30000;
        C.chain_from = C.f_from, C.chain_until = now + CHAIN_MS;
        return;
    }
    case F_RETURN:
        if (now > C.f_until) {
            follow_end("could not go back within 30 s", now);
            return;
        }
        if (!player || e->ev || dbg_busy()) return;
        say("back to '%s' near %.1f %.1f %.1f", C.home_area, C.home[0], C.home[1], C.home[2]);
        dbg_warp_near(C.home_area, C.home, "");
        C.f = F_IDLE;
        return;
    }
}

// ---- the era (NX2NAVI_COOP_ERA, coop_cutscene.h). The game sets Sequence_IsChildhood from each area it loads: a
// debug teleport of a child into Market_Adult made Link adult and the flag false, and back into Market_Child child
// again. The Master Sword's scenes change the era the same way, by taking Link into the other era's areas. So the
// era is not a progress flag one world can push into another
// (coop_game.c leaves it out of the table): a world changes era only by loading an area of the other era. Every frame
// Link stands in the world this game reads the flag:
//   - a change of its own (not a follow's, not this module's teleport) makes a new order and goes to the room as ERA,
//     with the first story scene of the chain this game led into it (the Master Sword's, where it started) and its place;
//   - the room's host repeats the room's era every 3 s, and at once when a player comes; a game that hears an older
//     order answers with its newest (orders: the file's top);
//   - a game whose era is not the room's, once it has been free for 4 s, follows the change's scene where it started, if
//     it started in this game's era (its own chain then takes this world into the other era, as it took the other
//     player's); when there is no such scene, or it left this game in its era, it teleports into the room's era (follow
//     mode): the scene's area of the other era (Market_Child -> Market_Adult) near where the scene started, else the
//     place the message gives. The area's load sets the era. So a player who joins a room of the other era is taken
//     into it. A teleport the game refused does not count as one of the two tries.
// A follower's own copy of a Master Sword scene changes its era with the leader's, so most of the time nobody is behind.
enum { ERA_BEAT_MS = 3000, ERA_SETTLE_MS = 4000, ERA_REPLY_MS = 2000, ERA_EXPECT_MS = 60000, ERA_SWITCH_TRIES = 2 };

static const char *era_name(int e) { return e < 0 ? "not known" : e ? "adult" : "child"; }

static void on_era(int from, const uint8_t *b, size_t n, void *user) {
    (void)user;
    if (from < 1 || from > COOP_PLAYERS || from == atomic_load(&C.self)) return;
    CoopEraMsg m;
    if (!coop_era_parse(b, n, &m)) {
        coop_ext_log(from, "era: an ERA of %zu bytes from seat %d does not parse; dropped", n, from);
        return;
    }
    plat_mutex_lock(&C.lock);
    if (win_take(&C.e_win[from], C.seat_gen[from], m.epoch, m.seq)) {   // not sent twice, not an older one
        if (C.ne_in == NIN) memmove(C.e_in, C.e_in + 1, sizeof C.e_in[0] * (NIN - 1)), C.ne_in--;   // the oldest gives way
        C.e_in[C.ne_in].m = m, C.e_in[C.ne_in].from = from, C.ne_in++;
    }
    plat_mutex_unlock(&C.lock);
}

static void queue_era(const CoopEraMsg *m) {
    uint8_t buf[COOP_ERA_MSG_MAX];
    size_t n = coop_era_pack(m, buf, sizeof buf);
    if (!n) {
        esay("an ERA message does not pack: not sent");
        return;
    }
    if (C.loop) {   // test: parsed back
        CoopEraMsg back;
        int ok = coop_era_parse(buf, n, &back);
        esay("loop: ERA seq %u order %u flags %u, %zu bytes, parsed back %d: '%s' in '%s' at %.1f %.1f %.1f, place '%s' at %.1f "
             "%.1f %.1f; same %d", m->seq, back.order, back.flags, n, ok, back.scene, back.scene_area, back.scene_xyz[0],
             back.scene_xyz[1], back.scene_xyz[2], back.area, back.xyz[0], back.xyz[1], back.xyz[2],
             ok && back.order == m->order && back.flags == m->flags && !strcmp(back.scene, m->scene) &&
                 !strcmp(back.scene_area, m->scene_area) && !strcmp(back.area, m->area) &&
                 !memcmp(back.xyz, m->xyz, sizeof back.xyz) && !memcmp(back.scene_xyz, m->scene_xyz, sizeof back.xyz));
    }
    queue_out(COOP_K_ERA, buf, n);
}

// The room's era as this game knows it, with the change's scene; the place area/xyz (NULL: the change's own).
static void era_out(int change, const char *area, const float xyz[3]) {
    CoopEraMsg m = C.e_last;
    m.epoch = C.epoch, m.seq = ++C.e_seq, m.order = C.e_order;
    m.flags = (uint8_t)((C.e_room == 1 ? COOP_ERA_ADULT : 0) | (change ? COOP_ERA_CHANGE : 0) | (C.e_last.flags & COOP_ERA_SCENE_ADULT));
    if (area) {
        snprintf(m.area, sizeof m.area, "%s", area);
        memcpy(m.xyz, xyz, sizeof m.xyz);
    }
    queue_era(&m);
}

// A message from the room (the game thread): the newest order wins; within one order the host's word, then the lower
// player id; an order far from this game's only from the host.
static void era_take(const CoopEraMsg *m, int from, int host, uint64_t now) {
    int era = m->flags & COOP_ERA_ADULT ? 1 : 0;
    int rel = order_rel(m->order, C.e_order);   // e_order is 0 until an order is known in this room
    Who w;
    if (rel == O_NEWER || (rel == O_FAR && from == host) || (rel == O_SAME && era != C.e_room && ranks_above(from, C.e_from))) {
        int was = C.e_room;
        C.e_order = m->order, C.e_room = era, C.e_from = from, C.e_last = *m;
        C.e_behind_at = 0;
        if (!++C.e_change) C.e_change = 1;
        if (era != was || (m->flags & COOP_ERA_CHANGE))
            esay("%s %s: the room's era is %s (order %u%s%s%s, place '%s'); this game is %s", who(from, &w),
                 m->flags & COOP_ERA_CHANGE ? "changed era" : "keeps the room's era", era_name(era), m->order,
                 m->scene[0] ? ", through its scene '" : "", m->scene, m->scene[0] ? "'" : "", m->area, era_name(C.e_mine));
    } else if (rel == O_SAME && era == C.e_room && (from == C.e_from || ranks_above(from, C.e_from))) {
        C.e_from = from, C.e_last = *m;   // the same era: a newer place (the host's beat)
    } else if (rel == O_OLDER) {
        C.e_low = from;
    } else if (rel == O_FAR && now - C.e_far_said >= FAR_SAY_MS) {
        C.e_far_said = now;
        esay("%s sent order %u, far from this game's %u, and is not the room's host: dropped", who(from, &w), m->order, C.e_order);
    }
}

static int area_listed(const char *area) {
    for (int i = 0, n = dbg_places(); i < n; i++)
        if (!strcmp(dbg_place_area(i), area)) return 1;
    return 0;
}

// A place of the room's era: the area of the other era where the change's scene started (Xxx_Child <-> Xxx_Adult), near
// where it started; else the place the message gives (the sender's at its change, or the host's now).
static void era_place(char out[COOP_CUT_AREA_MAX + 1], float xyz[3]) {
    const CoopEraMsg *m = &C.e_last;
    const char *from = C.e_room ? "_Child" : "_Adult", *to = C.e_room ? "_Adult" : "_Child";
    size_t n = strlen(m->scene_area), k = strlen(from);
    if (n > k && !strcmp(m->scene_area + n - k, from) && n - k + strlen(to) <= COOP_CUT_AREA_MAX) {
        char twin[COOP_CUT_AREA_MAX + 1];
        snprintf(twin, sizeof twin, "%.*s%s", (int)(n - k), m->scene_area, to);
        if (area_listed(twin)) {
            snprintf(out, COOP_CUT_AREA_MAX + 1, "%s", twin);
            memcpy(xyz, m->scene_xyz, 12);
            return;
        }
    }
    snprintf(out, COOP_CUT_AREA_MAX + 1, "%s", m->area);
    memcpy(xyz, m->xyz, 12);
}

// This game's era changed from old to now (an area of the other era was loaded).
static void era_changed(int old, int now_era, const char *area, const float here[3], uint64_t now) {
    int self = atomic_load(&C.self);
    Who w;
    if (C.e_expect == now_era && now < C.e_expect_until) {
        C.e_expect = -1;
        esay("now %s in '%s': caught up with the room's era (%s's order %u)", era_name(now_era), area, who(C.e_from, &w), C.e_order);
        return;
    }
    if (C.f != F_IDLE || now < C.chain_until) {
        esay("now %s in '%s', with the scene of %s this game followed (the room: %s, order %u)", era_name(now_era), area,
             who(C.f != F_IDLE ? C.f_from : C.chain_from, &w), era_name(C.e_room), C.e_order);
        return;
    }
    if (now_era == C.e_room) {
        esay("now %s in '%s': the room's era (%s's order %u)", era_name(now_era), area, who(C.e_from, &w), C.e_order);
        return;
    }
    // a change of this game's own: a new order, to the room with the scene of the chain it led into it
    C.e_order++;
    C.e_room = now_era, C.e_from = self;
    if (!++C.e_change) C.e_change = 1;
    CoopEraMsg *l = &C.e_last;
    memset(l, 0, sizeof *l);
    int scene = C.lc_name[0] && now < C.lc_until && C.lc_adult == old;
    if (scene) {
        snprintf(l->scene, sizeof l->scene, "%s", C.lc_name);
        snprintf(l->scene_area, sizeof l->scene_area, "%s", C.lc_area);
        memcpy(l->scene_xyz, C.lc_xyz, sizeof l->scene_xyz);
        l->flags = old ? COOP_ERA_SCENE_ADULT : 0;
    }
    snprintf(l->area, sizeof l->area, "%s", area);
    memcpy(l->xyz, here, sizeof l->xyz);
    era_out(1, NULL, NULL);
    esay("this game went from %s to %s in '%s' at %.1f %.1f %.1f%s%s%s: sent to the room (order %u)", era_name(old),
         era_name(now_era), area, here[0], here[1], here[2], scene ? " after its scene '" : " (no story scene of its own led into it)",
         scene ? l->scene : "", scene ? "'" : "", C.e_order);
}

// A game whose era is not the room's catches up once it has been free for a while.
static void era_catch_up(const EvNow *e, uint64_t player, const char *area, uint64_t now) {
    if (C.e_mine < 0 || C.e_room < 0 || C.e_mine == C.e_room) {
        C.e_behind_at = 0;
        return;
    }
    uint32_t tag = C.e_change;
    Who w;
    if (C.e_switch_tried != tag) C.e_switch_n = 0;
    int teleporting = C.e_expect >= 0 && now < C.e_expect_until;
    if (teleporting && !dbg_busy() && dbg_arrivals() == C.e_arrivals) {   // the game refused it: not a try
        char st[160];
        dbg_status(st);
        C.e_expect = -1, teleporting = 0;
        if (C.e_switch_n > 0) C.e_switch_n--;
        esay("the teleport into the room's era did not happen (the debug tools say: %s); asked again once this game is free", st);
    }
    int free = player && area[0] && room_may_act() && !e->ev && !(e->flags & 0x4000) && !dbg_busy() && C.f == F_IDLE &&
               now >= C.chain_until && !teleporting;
    if (!free) {
        C.e_behind_at = 0;
        return;
    }
    if (!C.e_behind_at) {
        C.e_behind_at = now;
        if (C.e_said_behind != tag)
            esay("this game is %s, the room %s (%s's order %u): it catches up once free for %d s", era_name(C.e_mine),
                 era_name(C.e_room), who(C.e_from, &w), C.e_order, ERA_SETTLE_MS / 1000);
        C.e_said_behind = tag;
        return;
    }
    if (now - C.e_behind_at < ERA_SETTLE_MS) return;
    C.e_behind_at = 0;
    const CoopEraMsg *m = &C.e_last;
    // 1. the change's scene where it started, followed: this world's own chain changes its era as it changed the other's
    if (C.e_scene_tried != tag && m->scene[0] && m->scene_area[0]) {
        C.e_scene_tried = tag;
        int scene_adult = !!(m->flags & COOP_ERA_SCENE_ADULT);
        if (scene_adult == C.e_mine) {
            CoopCutMsg s = {.op = COOP_CUT_START};
            snprintf(s.event, sizeof s.event, "%s", m->scene);
            snprintf(s.area, sizeof s.area, "%s", m->scene_area);
            memcpy(s.xyz, m->scene_xyz, sizeof s.xyz);
            s.flags = (uint8_t)(scene_adult ? COOP_CUT_ADULT : 0);   // consider() takes the movie bit from this game's table
            esay("behind the room (%s since %s's order %u): its change came through the scene '%s' in '%s': followed "
                 "here, so that this world changes era the same way", era_name(C.e_room), who(C.e_from, &w), C.e_order, s.event,
                 s.area);
            if (consider(&s, C.e_from, 2, player, area, now)) return;
        } else {
            esay("the scene '%s' of %s's change started %s, and this game is %s: not followed", m->scene, who(C.e_from, &w),
                 era_name(scene_adult), era_name(C.e_mine));
        }
    }
    // 2. a teleport into the room's era
    if (C.era_mode != COOP_ERA_FOLLOW) {
        if (C.e_said_stuck != tag)
            esay("this game stays %s while the room is %s: NX2NAVI_COOP_ERA=scene does not teleport", era_name(C.e_mine), era_name(C.e_room));
        C.e_said_stuck = tag;
        return;
    }
    if (C.e_switch_tried == tag && C.e_switch_n >= ERA_SWITCH_TRIES) {
        if (C.e_said_stuck != tag)
            esay("%d teleports left this game %s (the room is %s): it stays so until the room's era changes again",
                 C.e_switch_n, era_name(C.e_mine), era_name(C.e_room));
        C.e_said_stuck = tag;
        return;
    }
    char target[COOP_CUT_AREA_MAX + 1];
    float xyz[3];
    era_place(target, xyz);
    C.e_switch_tried = tag, C.e_switch_n++;
    if (!target[0]) {
        esay("no place of the room's era is known: this game stays %s", era_name(C.e_mine));
        C.e_switch_n = ERA_SWITCH_TRIES;
        return;
    }
    esay("teleporting from '%s' to '%s' near %.1f %.1f %.1f (try %d): an area of the room's era makes this game %s too",
         area, target, xyz[0], xyz[1], xyz[2], C.e_switch_n, era_name(C.e_room));
    C.e_arrivals = dbg_arrivals();
    dbg_warp_near(target, xyz, "");
    C.e_expect = C.e_room, C.e_expect_until = now + ERA_EXPECT_MS;
}

// The game thread, every frame in a room, alone in it too (the era's mode follow or scene).
static void era_frame(const EvNow *e, uint64_t player, const char *area, uint64_t now) {
    int self = atomic_load(&C.self), host = atomic_load(&C.host);
    unsigned serial = coop_game_save_serial();
    if (serial != C.e_serial) {
        C.e_serial = serial;
        if (C.e_mine >= 0) esay("the title screen came back: this game's era is read again with the save played next");
        C.e_mine = -1;
    }
    // Link placed in the world, no load (progress sync's gate): during a change of area the flag is already the next
    // area's while Link's actor is the old one or a new one at the origin, so neither is read then
    int in_world = player && area[0] && (coop_game_running() || !coop_on());
    int v = in_world ? flag_bool(COOP_ERA_FLAG) : -1, era = v < 0 ? -1 : !v;
    float here[3] = {0, 0, 0};
    if (in_world) rd(player + A_POS, here, 12);
    if (in_world && !C.e_names) {   // the game's places (the teleport's list), read once with the flag names
        C.e_names = 1;
        dbg_flags_refresh();
        dbg_names(NULL);
    }
    struct { CoopEraMsg m; int from; } in[NIN];
    plat_mutex_lock(&C.lock);
    int reset = C.e_reset, greet = C.e_greet, nin = C.ne_in;
    memcpy(in, C.e_in, sizeof in[0] * (size_t)nin);
    C.e_reset = C.e_greet = 0, C.ne_in = 0;
    plat_mutex_unlock(&C.lock);
    if (reset) {   // another room: its era is learnt anew, and this game's own is read again (it may have changed meanwhile)
        C.e_order = 0, C.e_room = -1, C.e_from = 0, C.e_expect = -1, C.e_behind_at = 0, C.e_mine = -1, C.e_low = 0;
        C.e_scene_tried = C.e_switch_tried = C.e_said_behind = C.e_said_stuck = 0, C.e_switch_n = 0, C.e_greet_due = 0;
        C.e_beat_at = 0;
        if (!++C.e_change) C.e_change = 1;
        memset(&C.e_last, 0, sizeof C.e_last);
    }
    for (int i = 0; i < nin; i++) era_take(&in[i].m, in[i].from, host, now);
    if (era >= 0) {
        Who w;
        if (C.e_mine < 0) {   // the first reading of this save
            C.e_mine = era;
            if (C.e_room < 0) {   // nothing from the room yet: the room's era starts as this game's
                C.e_room = era, C.e_from = self;
                memset(&C.e_last, 0, sizeof C.e_last);
                snprintf(C.e_last.area, sizeof C.e_last.area, "%s", area);
                memcpy(C.e_last.xyz, here, sizeof here);
            }
            esay("this game is %s (in '%s'); the room's era: %s (order %u, %s)", era_name(era), area, era_name(C.e_room),
                 C.e_order, who(C.e_from, &w));
        } else if (era != C.e_mine) {
            int old = C.e_mine;
            C.e_mine = era;
            era_changed(old, era, area, here, now);
        }
    }
    // a player that still has an older order hears the newest
    if (C.e_low && C.e_room >= 0 && now >= C.e_reply_at) {
        Who w;
        C.e_reply_at = now + ERA_REPLY_MS;
        era_out(0, NULL, NULL);
        esay("%s still has an older order: the room's newest (%u, %s) sent", who(C.e_low, &w), C.e_order, era_name(C.e_room));
        C.e_low = 0;
    }
    int keeper = self && self == host;
    if (keeper != C.e_keeper) {
        C.e_keeper = keeper;
        esay("%s", keeper ? "this game keeps the room's era (the room's host): it goes out every 3 s" : "the room's host keeps the room's era");
    }
    if (greet) C.e_greet_due = 1;
    if (!keeper) C.e_greet_due = 0;
    if (keeper && in_world && C.e_room >= 0 && C.e_mine >= 0 && (now >= C.e_beat_at || C.e_greet_due)) {
        C.e_beat_at = now + ERA_BEAT_MS, C.e_greet_due = 0;
        era_out(0, C.e_mine == C.e_room ? area : NULL, here);   // its own place when it is of the room's era
    }
    era_catch_up(e, player, area, now);
}

static void run_tests(Cpu *c, const EvNow *e, uint64_t player, const char *area, uint64_t now) {
    if (player && !C.player_at) C.player_at = now;
    if (!C.other_at && coop_puppet_world_others() > 0) {
        C.other_at = now;
        say("another player is in the game world (%.1f s after Link)", secs(C.player_at, now));
    }
    for (int i = 0; i < C.ntest; i++) {
        Test *t = &C.test[i];
        uint64_t base = t->after_other ? C.other_at : C.player_at;
        if (t->done || !base || now < base + (uint64_t)(t->at * 1000)) continue;
        // a teleport or a start waits (up to a minute) for a moment with no event and the debug tools idle: the game refuses
        // a teleport during an event, and the debug tools drop a start asked for while a teleport settles
        if ((t->kind == 3 || t->kind == 0) && (e->ev || (e->flags & 0x4000) || dbg_busy() || !player) &&
            now < base + (uint64_t)(t->at * 1000) + 60000)
            continue;
        t->done = 1;
        if (t->kind == 0) {
            say("test %d: starting '%s' by its name in '%s'", i + 1, t->event, area);
            dbg_event_start(t->event);
        } else if (t->kind == 3) {
            say("test %d: teleporting from '%s' to '%s' %s", i + 1, area, t->event, t->area[0] ? t->area : "(its first start position)");
            dbg_warp(t->event, t->area);
        } else if (t->kind == 2) {
            uint64_t wm = world_mgr();
            say("test %d: the time of day set to %s (%u s) through the game's setter (world manager %#llx)", i + 1, t->event,
                (unsigned)t->xyz[0], (unsigned long long)wm);
            if (wm) rt_call(c, WM_SET_TIME, 2, wm, (uint64_t)(uint32_t)t->xyz[0]);
        } else {
            CoopCutMsg m = {.op = COOP_CUT_START, .epoch = 1, .seq = (uint32_t)i + 1};
            snprintf(m.event, sizeof m.event, "%s", t->event);
            snprintf(m.area, sizeof m.area, "%s", t->area);
            memcpy(m.xyz, t->xyz, sizeof m.xyz);
            m.flags = (uint8_t)(player && link_adult(player) ? COOP_CUT_ADULT : 0);
            say("test %d: a START as if from another player: '%s' in '%s'", i + 1, m.event, m.area);
            consider(&m, 0, 1, player, area, now);
        }
    }
}

void coop_cutscene_frame(Cpu *c, uint64_t player, const char *area) {
    if (!__atomic_load_n(&C.active, __ATOMIC_ACQUIRE)) {
        if (C.f != F_IDLE) follow_end("cutscenes went off", plat_now_ms());
        if (C.chain_hold) C.chain_hold = 0, C.chain_until = 0, hold_now();
        C.hides = 0;
        return;
    }
    uint64_t now = plat_now_ms();
    plat_mutex_lock(&C.lock);
    int room_changed = C.f_reset;
    C.f_reset = 0;
    plat_mutex_unlock(&C.lock);
    if (room_changed) {   // the last room's players are gone: what this game followed of theirs ends here
        if (C.f != F_IDLE) follow_end("this game is in another room now (or in none)", now);
        if (C.chain_hold) C.chain_hold = 0, hold_now();
        C.chain_until = 0, C.chain_from = 0;
    }
    EvNow e;
    ev_now(&e);
    if (C.ntest) run_tests(c, &e, player, area, now);
    if (C.time_probe > 0) time_probe(now);
    leader(&e, player, area, now);
    CoopCutMsg m, end;
    int have = 0, have_end = 0, from = 0, end_from = 0, fake = 0;
    plat_mutex_lock(&C.lock);
    if (C.in_have) m = C.in, from = C.in_from, fake = C.in_fake, have = 1, C.in_have = 0;
    if (C.end_have) end = C.end, end_from = C.end_from, have_end = 1, C.end_have = 0;
    plat_mutex_unlock(&C.lock);
    if (have) consider(&m, from, fake, player, area, now);
    if (have_end && C.f != F_IDLE && end_from == C.f_from && !strcmp(end.event, C.fm.event)) {
        Who w;
        if (C.f == F_WAIT) follow_end("the other player's scene ended before this game was free", now);
        else say("the scene '%s' of %s is over there; this game's copy goes on (%s)", end.event, who(end_from, &w), f_names[C.f]);
    }
    follower(&e, player, area, now);
    if (C.chain_hold && C.f == F_IDLE && now >= C.chain_until) {   // the chain of the followed scene is over here
        Who w;
        C.chain_hold = 0, hold_now();
        say("no more scenes of the chain of %s here: the room's values are taken again", who(C.chain_from, &w));
    }
    if ((C.era_mode == COOP_ERA_FOLLOW || C.era_mode == COOP_ERA_SCENE) && atomic_load(&C.self) > 0)
        era_frame(&e, player, area, now);   // the era: after the follower, which a catch-up may start
    if (C.time_on && atomic_load(&C.others) > 0) time_frame(c, &e, player, area, now);
    C.hides = C.hide && ((C.story && e.ev) || C.f == F_PLAY);
}

int coop_cutscene_hides_puppets(void) { return C.hides; }
