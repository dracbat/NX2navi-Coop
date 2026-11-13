// The small keys of a co-op room (coop_keys.h), for Ocarina 1.1.1.
//
// Where the game keeps them. Each dungeon has a struct of flags under the flag database's DungeonInfo struct
// (coop_game.c has the database's layout): its map, compass and boss key, and two Int counters, the small keys held
// (SmallKeyNum) and the keys ever picked up there. The table below names the SmallKeyNum flags of the nine dungeons that
// hold small keys, by hash, with the most keys each one holds; the five dungeons that hold none are left alone. The
// game changes a count in one routine (main+0x3283160, on its dungeon-info object, with n): a key picked up (from a
// chest, lying about, or dropped by a guard) adds 1 in the dungeon Link is in, and a locked door takes 1 while the
// count is above 0. The routine queues the change on the Int container's ring of pending sets as an addition, which
// the game's per-frame drain applies. The boss keys and the maps are flags the room's progress shares (coop_game.c).
//
// The pool. The room's host keeps one count per dungeon: at first its own save's, and after a host left, the counts the
// new host had. Every other game, once it has the host's counts (KEYS), sets its own to them. What changes in a game
// is a change of the room: the host applies its own at once; any other game sends it to the host (KEY: a session
// number of its own for this room, a sequence number, the dungeon and the change), and again every few seconds until a
// KEYS of the host acks it. The host takes each player's changes once and in order, keeps every count between 0 and
// the most keys that dungeon holds, and sends KEYS to everyone at each change, to a player who comes, and every 10 s.
// A player who joins takes the room's counts, whatever their own save had. Meanwhile a game's own count is the room's
// plus its changes the host has not acked yet, so that a key picked up is not taken back while it is on its way.
//
// One key a door. Each world has its own copy of a locked door, which the room's progress opens in the other worlds
// only after it was opened in one: two players can each open the same door in their own world before either game
// knows of the other's. So a change carries the place where Link stood when it happened (KEY), and the host counts a
// key used within DOOR_NEAR of the place of a key the room already paid, in the same dungeon, as that door's: taken
// and acked, but not counted, so that the KEYS which acks it sets that player's count back to the room's and they have
// their key again. The host's own uses go the same way. The small-key doors of a dungeon stand at least 6.8 m apart in
// every dungeon (the closest two are in the Gerudo Training Ground; the Water Temple has two above each other, 7.9 m
// apart), and Link opens a door standing at it (the game's event that unlocks one moves him to a place before the
// lock), so two keys used at one door, from either side, fall within DOOR_NEAR of each other and keys used at two
// doors do not. The host remembers the places of the keys used in this room (PAID of them, the oldest giving way); a
// new host starts with none, so a door opened in two worlds about a change of host can still cost two keys.
//
// The game's side, at each present (coop_keys_present): while the gate of coop_game.c is open (a save played, Link in
// the world, no load or scene change), each count is read. A count with a set of it still waiting on the ring is
// neither read nor written in that frame, so that no write of this file overwrites the game's own change and no change
// is taken for one of this file's writes. After a load, of another save or of the same one (coop_game_load_serial),
// the counts are read anew, and the difference is not a change. Writes go through the game's own SetInt
// (main+0x163b70), as the room's progress does, and only once the save is copied for the room (coop_game_backed_up).
//
// Settings: NX2NAVI_COOP_KEYS = pool (the default) or own, with a row on the Co-op tab. The pool needs
// NX2NAVI_COOP_SHARE too, since a door opens in every world only with progress shared. A game with own keeps its keys to
// itself and takes no counts; with a host that keeps own, the room has no pool and every player's keys stay their own.
// Lines about what other players sent go through coop_ext_log; no line names a player's id.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "coop.h"
#include "coop_ext.h"
#include "coop_ext_wire.h"
#include "coop_game.h"
#include "coop_keys.h"
#include "cpu.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"

#define MGR_PTR (MOD_MAIN_BASE + 0x4e842a8ull)
#define SET_INT_FN (MOD_MAIN_BASE + 0x163b70ull)
#define PLAYER_PTR (MOD_MAIN_BASE + 0x4e371d0ull)
#define DOOR_NEAR 3.4f             // metres: keys used this close in one dungeon opened one door ("One key a door")

enum {
    C_INT = 0x0c8, CN_COUNT = 0x08, CN_ENTRIES = 0x10, CN_TABLE = 0x18, CN_CAP = 0x24,
    CN_RING_CAP = 0x28, CN_RING = 0x30, CN_RING_CTR = 0x38, RING_SLOT = 12,   // a pending set: u32 value, u32 hash, u32 kind
    INT_SIZE = 0x28, I_CURRENT = 0x1c, MGR_SIZE = 0xd80,
    PENDING = 16,              // this game's changes the host has not acked yet
    RESEND_MS = 3000,          // a change not acked goes to the host again this often
    BEAT_MS = 10000,           // the host's KEYS to everyone
    QUIET_MS = 30000,          // no KEYS from the host for this long: the room has no pool (its host keeps own keys)
    PAID = 64,                 // the places of keys used that the host remembers (the game has 43 small-key doors)
};

// The dungeons that hold small keys: the hash of their SmallKeyNum flag, the most keys each one holds (its chests, keys
// lying about and guards that drop one), and its name for the log.
static const struct { uint32_t hash; uint8_t most; const char *name; } dungeons[] = {
    {0x90237092u, 5, "the Forest Temple"},          {0x1df11487u, 8, "the Fire Temple"},
    {0x877bdceeu, 8, "the Water Temple"},           {0x5edda9e3u, 5, "the Shadow Temple"},
    {0x79eeec8eu, 5, "the Spirit Temple"},          {0x7dd79d10u, 3, "the Bottom of the Well"},
    {0xea9786f5u, 9, "the Gerudo Training Ground"}, {0xfbfd2910u, 2, "Ganon's Castle"},
    {0x3a28b6a4u, 4, "the Gerudo Fortress"},
};
#define ND ((int)(sizeof dungeons / sizeof dungeons[0]))
_Static_assert(ND <= COOP_KEYS_DUNGEONS, "a KEYS holds every dungeon");

typedef struct {
    uint32_t seq;
    int8_t d, delta;               // the dungeon (an index in the table), the keys picked up (> 0) or used (< 0)
    uint8_t placed;                // at: where Link stood when the change happened
    float at[3];
} Change;

typedef struct {
    int8_t d;                      // the dungeon
    float at[3];                   // where Link stood when a key was used there
} Paid;

// Shared between the co-op thread and the game's thread, under lock.
static struct {
    PlatMutex lock;
    int on;                         // NX2NAVI_COOP_KEYS is pool and NX2NAVI_COOP_SHARE is on
    int in_room, self, host;        // the glue's view: in a room, this game's seat and the host's
    uint64_t self_id;               // never logged
    // the room's counts: the host's own, or the last KEYS taken from the host
    int have;
    uint8_t keys[COOP_KEYS_DUNGEONS];
    uint32_t serial;
    struct { uint64_t id; uint32_t sess, seq; } ack[COOP_PLAYERS];   // the last change taken from each player
    int nack;
    uint64_t heard_at;              // when the last KEYS was taken from the host
    // this game's changes the host has not acked yet
    uint32_t sess, seq;             // this game's session in the room (random, not 0) and its last change's number
    Change pend[PENDING];
    int npend, sent;                // pend[0..sent) went out since the last round
    uint64_t resend_at;
    // the host's sending
    int dirty;                      // KEYS to everyone at the next step
    unsigned greet;                 // and to these seats (bit s)
    uint64_t beat_at;
    // the host's: the places of the keys used in this room, one a door ("One key a door")
    Paid paid[PAID];
    int npaid;
} K;

// The co-op thread's own.
static struct {
    unsigned cfg_seen;
    CoopExtView v;                  // the glue's view at its last change
    uint32_t gen[COOP_PLAYERS + 1]; // each seat's gen, to tell a player who came
    int was_host;
    CoopKeysMsg held;               // a KEYS from a seat the view did not name the host (yet)
    uint64_t held_from;             // its sender (0: none kept); never logged
    uint8_t said[COOP_KEYS_DUNGEONS];   // the room's counts the log said last
    int said_any;
} T;

// The game thread's own.
static struct {
    unsigned load_serial;
    uint64_t mgr, int_entries;
    uint64_t addr[COOP_KEYS_DUNGEONS];   // each counter's entry, 0 when this save's table lacks it
    int based;                      // seen holds this save's counts
    int32_t seen[COOP_KEYS_DUNGEONS];   // each count as last read or written here
} G;

static int dungeon_of(uint32_t hash) {
    for (int d = 0; d < ND; d++)
        if (dungeons[d].hash == hash) return d;
    return -1;
}

static int clamp_keys(int d, int v) { return v < 0 ? 0 : v > dungeons[d].most ? dungeons[d].most : v; }

static const char *seat_name(int s) { return s >= 1 && s <= COOP_PLAYERS && T.v.seat[s].name[0] ? T.v.seat[s].name : "(name not known yet)"; }

static int seat_of(uint64_t id) {
    for (int s = 1; s <= COOP_PLAYERS; s++)
        if (id && T.v.seat[s].id == id) return s;
    return 0;
}

// "the Forest Temple 2, the Fire Temple 1" for the counts that are not 0, "none" else.
static void counts_text(const uint8_t *keys, char *out, size_t cap) {
    size_t o = 0;
    out[0] = 0;
    for (int d = 0; d < ND && o < cap; d++)
        if (keys[d]) o += (size_t)snprintf(out + o, cap - o, "%s%s %u", o ? ", " : "", dungeons[d].name, keys[d]);
    if (!out[0]) snprintf(out, cap, "none");
}

static const char *change_text(int delta) { return delta > 0 ? "picked up" : "used"; }

// The place of a key the room already paid, in dungeon d, that at is within DOOR_NEAR of (lock held): its index, or -1.
static int paid_near(int d, const float at[3]) {
    for (int i = 0; i < K.npaid; i++) {
        if (K.paid[i].d != d) continue;
        float dx = K.paid[i].at[0] - at[0], dy = K.paid[i].at[1] - at[1], dz = K.paid[i].at[2] - at[2];
        if (dx * dx + dy * dy + dz * dz <= DOOR_NEAR * DOOR_NEAR) return i;
    }
    return -1;
}

// A key used in dungeon d at at, counted (lock held): its door is paid for.
static void paid_add(int d, const float at[3]) {
    if (K.npaid == PAID) memmove(K.paid, K.paid + 1, sizeof K.paid[0] * (PAID - 1)), K.npaid--;   // the oldest gives way
    K.paid[K.npaid].d = (int8_t)d;
    memcpy(K.paid[K.npaid].at, at, sizeof K.paid[0].at);
    K.npaid++;
}

// ---- the co-op thread (and under lock)

// Nothing of a room stays: another room, out of one, or the pool switched off or on. The game's counts stay as they are.
static void reset_room(void) {
    K.have = 0, K.serial = 0, K.nack = 0, K.heard_at = 0;
    memset(K.keys, 0, sizeof K.keys);
    memset(K.ack, 0, sizeof K.ack);
    K.npend = K.sent = 0, K.resend_at = 0, K.seq = 0;
    uint32_t s = 0;
    if (!plat_random(&s, sizeof s) || !s) s = (uint32_t)plat_now_ns() | 1;
    K.sess = s;
    K.dirty = 0, K.greet = 0, K.beat_at = 0;
    K.npaid = 0;
    T.held_from = 0, T.said_any = 0;
}

static int hosting(void) { return K.on && K.in_room && K.self && K.host == K.self; }

// The host's counts taken (lock held): the counts of the dungeons this build knows, and this game's changes they ack.
static void take(const CoopKeysMsg *m, uint64_t now) {
    int unknown = 0;
    for (int i = 0; i < m->n; i++) {
        int d = dungeon_of(m->d[i].hash);
        if (d < 0) unknown++;
        else K.keys[d] = (uint8_t)clamp_keys(d, m->d[i].keys);
    }
    K.have = 1, K.serial = m->serial, K.heard_at = now;
    K.nack = m->nack;
    for (int i = 0; i < m->nack; i++) K.ack[i].id = m->ack[i].id, K.ack[i].sess = m->ack[i].sess, K.ack[i].seq = m->ack[i].seq;
    int acked = 0;
    for (int i = 0; i < m->nack; i++) {
        if (m->ack[i].id != K.self_id || m->ack[i].sess != K.sess) continue;
        int keep = 0;
        for (int p = 0; p < K.npend; p++)
            if ((int32_t)(K.pend[p].seq - m->ack[i].seq) > 0) K.pend[keep++] = K.pend[p];
            else acked++;
        K.npend = keep;
        K.sent = K.sent > acked ? K.sent - acked : 0;
    }
    if (!T.said_any || memcmp(T.said, K.keys, sizeof T.said) || unknown || acked) {
        char line[512];
        counts_text(K.keys, line, sizeof line);
        rt_log("coop: keys: the room's small keys (the host's change %u): %s%s%s", K.serial, line,
               acked ? "; the host took this game's changes" : "", unknown ? "; some dungeons this game does not know were left out" : "");
        memcpy(T.said, K.keys, sizeof T.said), T.said_any = 1;
    }
}

static void on_keys(int seat, const uint8_t *b, size_t n, void *user) {
    (void)user;
    CoopKeysMsg m;
    if (seat < 1 || seat > COOP_PLAYERS) return;
    if (!coop_keys_parse(b, n, &m)) {
        coop_ext_log(seat, "keys: a KEYS of %zu bytes from seat %d does not parse; dropped", n, seat);
        return;
    }
    plat_mutex_lock(&K.lock);
    if (K.on && !hosting()) {
        if (seat == K.host) {
            take(&m, plat_now_ms());
        } else {   // kept until the view says whether this seat is the host (its first message can come first)
            T.held = m, T.held_from = T.v.seat[seat].id;
            coop_ext_log(seat, "keys: KEYS from seat %d '%s', which the room does not name its host (yet): kept aside", seat, seat_name(seat));
        }
    }
    plat_mutex_unlock(&K.lock);
}

static void on_key(int seat, const uint8_t *b, size_t n, void *user) {
    (void)user;
    CoopKeyMsg m;
    if (seat < 1 || seat > COOP_PLAYERS) return;
    if (!coop_key_parse(b, n, &m)) {
        coop_ext_log(seat, "keys: a KEY of %zu bytes from seat %d does not parse; dropped", n, seat);
        return;
    }
    int d = dungeon_of(m.hash);
    uint64_t id = T.v.seat[seat].id;
    plat_mutex_lock(&K.lock);
    if (!hosting() || !K.have || d < 0 || !id) {
        int why = !hosting() ? 0 : !K.have ? 1 : 2;
        plat_mutex_unlock(&K.lock);
        if (why) coop_ext_log(seat, "keys: a KEY from seat %d '%s' %s; not taken", seat, seat_name(seat),
                              why == 1 ? "before this game has the room's counts (it comes again)" : "for a dungeon this game does not know");
        return;
    }
    int a = -1;
    for (int i = 0; i < K.nack; i++)
        if (K.ack[i].id == id) a = i;
    if (a >= 0 && K.ack[a].sess == m.sess && (int32_t)(m.seq - K.ack[a].seq) <= 0) {
        K.greet |= 1u << seat;   // taken before: the player learns so from a KEYS
        plat_mutex_unlock(&K.lock);
        return;
    }
    if (a < 0) {   // a player's first change: their place, or the place of one who is gone
        if (K.nack < COOP_PLAYERS) a = K.nack++;
        for (int i = 0; a < 0 && i < K.nack; i++)
            if (!seat_of(K.ack[i].id)) a = i;
        if (a < 0) a = 0;
    }
    K.ack[a].id = id, K.ack[a].sess = m.sess, K.ack[a].seq = m.seq;
    int keys = m.delta > 0 ? m.delta : -m.delta, placed = m.delta < 0 && (m.flags & COOP_KEY_PLACE);
    if (placed && paid_near(d, m.xyz) >= 0) {   // a door the room paid a key for already: taken, not counted
        K.greet |= 1u << seat;   // the KEYS that acks it sets the player's count back to the room's
        int has = K.keys[d];
        plat_mutex_unlock(&K.lock);
        coop_ext_log(seat, "keys: seat %d '%s' used %d small key%s in %s at %.1f %.1f %.1f, at a door the room already paid a key "
                     "for: not counted, they have it back (the room has %d)", seat, seat_name(seat), keys, keys == 1 ? "" : "s",
                     dungeons[d].name, m.xyz[0], m.xyz[1], m.xyz[2], has);
        return;
    }
    if (placed) paid_add(d, m.xyz);
    int was = K.keys[d], now = clamp_keys(d, was + m.delta);
    K.keys[d] = (uint8_t)now;
    K.serial++, K.dirty = 1;
    plat_mutex_unlock(&K.lock);
    char where[64] = "";
    if (placed) snprintf(where, sizeof where, " at %.1f %.1f %.1f", m.xyz[0], m.xyz[1], m.xyz[2]);
    coop_ext_log(seat, "keys: seat %d '%s' %s %d small key%s in %s%s: the room has %d (was %d)%s", seat, seat_name(seat),
                 change_text(m.delta), keys, keys == 1 ? "" : "s", dungeons[d].name, where, now, was,
                 now != was + m.delta ? ", within 0 and the keys the dungeon holds" : "");
}

static void on_view(const CoopExtView *v, void *user) {
    (void)user;
    int was_in = T.v.in_room;
    uint32_t was_room = T.v.room;
    int was_host_seat = T.v.host;
    T.v = *v;
    plat_mutex_lock(&K.lock);
    if (was_in && (!v->in_room || v->room != was_room)) {   // out of a room, or into another: nothing of the last stays
        reset_room();
        T.was_host = 0;
    } else if (!was_in && v->in_room) {
        // into a room: nothing of an earlier one stays, except a KEYS kept aside while this game was in none. The host
        // greets a player who comes in the step after letting them in, while this game's glue lists the room only in
        // the step after it was let in (coop_ext_step reads the room's view of the step before), so the greeting can
        // come first; it is taken below, since the view now names its sender the host.
        CoopKeysMsg held = T.held;
        uint64_t held_from = T.held_from;
        reset_room();
        T.held = held, T.held_from = held_from;
        T.was_host = 0;
    }
    K.in_room = v->in_room, K.self = v->self, K.host = v->host;
    K.self_id = v->self ? v->seat[v->self].id : 0;
    int host = hosting();
    if (T.held_from && v->in_room) {   // a KEYS kept aside: taken if the view names its sender the host, else dropped
        int s = seat_of(T.held_from);
        if (K.on && s && s == v->host && !host) take(&T.held, plat_now_ms());
        T.held_from = 0;
    }
    if (host && !T.was_host) {
        if (K.have) {   // the last host left: this game carries on from the counts it had, its own changes in them
            for (int p = 0; p < K.npend; p++) {
                int d = K.pend[p].d;
                K.keys[d] = (uint8_t)clamp_keys(d, K.keys[d] + K.pend[p].delta);
            }
            K.npend = K.sent = 0;
            K.serial++, K.dirty = 1;
            char line[512];
            counts_text(K.keys, line, sizeof line);
            rt_log("coop: keys: this game keeps the room's small keys now (the room's host), from the counts it had: %s", line);
        } else {
            rt_log("coop: keys: this game keeps the room's small keys (the room's host), from this save's once a save is played");
        }
    }
    if (!host && K.in_room && v->host != was_host_seat && K.npend) K.sent = 0, K.resend_at = 0;   // another host: this game's changes go to it
    T.was_host = host;
    for (int s = 1; s <= COOP_PLAYERS; s++) {   // a player who came gets the counts at once
        uint32_t gen = v->in_room && v->seat[s].id ? v->seat[s].gen : 0;
        if (gen == T.gen[s]) continue;
        T.gen[s] = gen;
        if (gen && !v->seat[s].self && host) K.greet |= 1u << s;
    }
    plat_mutex_unlock(&K.lock);
}

// The host's KEYS (lock held): every dungeon's count, and the last change taken from each player in the room.
static size_t pack_keys(uint8_t *out, size_t cap) {
    CoopKeysMsg m;
    memset(&m, 0, sizeof m);
    m.serial = K.serial;
    for (int d = 0; d < ND; d++) m.d[m.n].hash = dungeons[d].hash, m.d[m.n++].keys = K.keys[d];
    for (int i = 0; i < K.nack; i++)
        if (K.ack[i].id && K.ack[i].sess && seat_of(K.ack[i].id))
            m.ack[m.nack].id = K.ack[i].id, m.ack[m.nack].sess = K.ack[i].sess, m.ack[m.nack++].seq = K.ack[i].seq;
    return coop_keys_pack(&m, out, cap);
}

static void on_tick(uint64_t now, void *user) {
    (void)user;
    unsigned ver = cfg_version();
    if (ver != T.cfg_seen) {
        T.cfg_seen = ver;
        const char *mode = cfg_value("NX2NAVI_COOP_KEYS");
        int share = cfg_bool("NX2NAVI_COOP_SHARE"), on = strcmp(mode, "own") && share;
        static int said;
        plat_mutex_lock(&K.lock);
        int was = K.on;
        if (on != was) {
            K.on = on;
            if (K.in_room) reset_room();   // the counts start again: from this save's on the host, else from the host's
            T.was_host = 0;
            for (int s = 1; s <= COOP_PLAYERS; s++) T.gen[s] = 0;   // and every player in the room gets them again
        }
        plat_mutex_unlock(&K.lock);
        if (!said || on != was)
            rt_log("coop: keys: %s (NX2NAVI_COOP_KEYS='%s', NX2NAVI_COOP_SHARE %s)",
                   on ? "small keys are the room's: one count per dungeon, kept by the room's host"
                      : "small keys are each player's own", mode, share ? "on" : "off");
        said = 1;
        if (on != was && T.v.in_room) on_view(&T.v, NULL);   // the seats in the room, again
    }
    plat_mutex_lock(&K.lock);
    if (!K.on || !K.in_room) {
        plat_mutex_unlock(&K.lock);
        return;
    }
    uint8_t b[COOP_KEYS_MSG_MAX];
    if (hosting()) {
        if (K.have && (K.dirty || now >= K.beat_at)) {
            size_t n = pack_keys(b, sizeof b);
            if (n && coop_ext_send(0, COOP_K_KEYS, b, n, 1)) K.dirty = 0, K.greet = 0, K.beat_at = now + BEAT_MS;
        } else if (K.have && K.greet) {
            size_t n = pack_keys(b, sizeof b);
            for (int s = 1; n && s <= COOP_PLAYERS; s++)
                if (K.greet & 1u << s && (!T.v.seat[s].id || coop_ext_send(s, COOP_K_KEYS, b, n, 1))) K.greet &= ~(1u << s);
        }
    } else {
        // heard_at can be a moment later than now: a KEYS kept aside is taken by the view hook, which runs in the step
        // after the step's clock was read
        if (K.have && K.heard_at && now > K.heard_at && now - K.heard_at >= QUIET_MS) {
            K.have = 0, T.said_any = 0;
            rt_log("coop: keys: no counts from the room's host for %d s: this game's small keys are its own until the host "
                   "sends them (a host with NX2NAVI_COOP_KEYS=own keeps no pool)", QUIET_MS / 1000);
        }
        if (K.npend && K.host) {
            if (now >= K.resend_at) K.sent = 0, K.resend_at = now + RESEND_MS;
            while (K.sent < K.npend) {   // in order, and none after one the room's queue did not take
                const Change *p = &K.pend[K.sent];
                CoopKeyMsg m = {.sess = K.sess, .seq = p->seq, .hash = dungeons[p->d].hash, .delta = p->delta,
                                .flags = p->placed ? COOP_KEY_PLACE : 0};
                if (p->placed) memcpy(m.xyz, p->at, sizeof m.xyz);
                size_t n = coop_key_pack(&m, b, sizeof b);
                if (!n || !coop_ext_send(K.host, COOP_K_KEY, b, n, 1)) break;
                K.sent++;
            }
        }
    }
    plat_mutex_unlock(&K.lock);
}

void coop_keys_init(void) {
    plat_mutex_init(&K.lock);
    T.cfg_seen = ~0u;
    coop_ext_on_message(COOP_K_KEYS, on_keys, NULL);
    coop_ext_on_message(COOP_K_KEY, on_key, NULL);
    coop_ext_on_view(on_view, NULL);
    coop_ext_on_tick(on_tick, NULL);
}

// ---- the game's thread

static int rd_ok(uint64_t a, uint64_t n) { return a >= 0x1000 && mem_readable(a, n); }

// Where Link stands: the Player actor [[main+0x4e371d0] + 0x480] while its name, [+0x20], reads "Player", its position
// in metres at +0x31c (coop_game.c reads him the same way). 0 when there is none, or the place does not go into a KEY.
static int link_place(float at[3]) {
    uint64_t m = rd_ok(PLAYER_PTR, 8) ? LD64(PLAYER_PTR) : 0, a = rd_ok(m + 0x480, 8) ? LD64(m + 0x480) : 0;
    uint64_t name = rd_ok(a + 0x20, 8) ? LD64(a + 0x20) : 0;
    if (!rd_ok(name, 7) || memcmp((const void *)(uintptr_t)name, "Player", 7) || !rd_ok(a + 0x31c, 12)) return 0;
    memcpy(at, (const void *)(uintptr_t)(a + 0x31c), 12);
    for (int i = 0; i < 3; i++)
        if (!isfinite(at[i]) || fabsf(at[i]) > 1e6f) return 0;
    return 1;
}

// The Int entry of hash, 0 when absent (coop_game.c entry_of, for the Int container).
static uint64_t int_entry(uint64_t mgr, uint32_t hash) {
    uint64_t c = mgr + C_INT, table = LD64(c + CN_TABLE), entries = LD64(c + CN_ENTRIES);
    uint32_t cap = LD32(c + CN_CAP), count = LD32(c + CN_COUNT);
    if (!hash || !cap || !table || !entries || !rd_ok(table, 8ull * cap)) return 0;
    for (uint32_t i = hash % cap, k = 0; k < cap; k++, i = i + 1 == cap ? 0 : i + 1) {
        uint32_t h = LD32(table + 8ull * i);
        if (h == hash) {
            uint32_t idx = LD32(table + 8ull * i + 4);
            uint64_t e = idx < count ? entries + (uint64_t)idx * INT_SIZE : 0;
            return e && rd_ok(e, INT_SIZE) ? e : 0;
        }
        if (!h) return 0;
    }
    return 0;
}

// A set of hash waits on the Int ring (the game's own, or one of this file's): its value is not the current one yet.
static int ring_has(uint64_t mgr, uint32_t hash) {
    uint64_t c = mgr + C_INT, ring = LD64(c + CN_RING);
    uint32_t n = LD32(c + CN_RING_CTR) & 0xfffff, cap = LD32(c + CN_RING_CAP);
    if (!n) return 0;
    if (!ring || n > cap || !rd_ok(ring, (uint64_t)RING_SLOT * n)) return 1;
    for (uint32_t i = 0; i < n; i++)
        if (LD32(ring + (uint64_t)RING_SLOT * i + 4) == hash) return 1;
    return 0;
}

void coop_keys_present(Cpu *c) {
    plat_mutex_lock(&K.lock);
    int on = K.on && K.in_room;
    plat_mutex_unlock(&K.lock);
    if (!on || !RT_V111) {   // own keys: what changes meanwhile is no change of a room's
        G.based = 0;
        return;
    }
    uint64_t mgr = LD64(MGR_PTR);
    if (!rd_ok(mgr, MGR_SIZE)) {
        G.based = 0;
        return;
    }
    if (!coop_game_running()) return;   // the counts are read again once the game is played
    unsigned loads = coop_game_load_serial();
    uint64_t entries = LD64(mgr + C_INT + CN_ENTRIES);
    if (loads != G.load_serial || mgr != G.mgr || entries != G.int_entries) {   // a save loaded: its counts are read anew
        G.load_serial = loads, G.mgr = mgr, G.int_entries = entries, G.based = 0;
        for (int d = 0; d < ND; d++) G.addr[d] = int_entry(mgr, dungeons[d].hash);
    }
    int32_t cur[COOP_KEYS_DUNGEONS];
    int ready[COOP_KEYS_DUNGEONS], waiting = 0;
    for (int d = 0; d < ND; d++) {
        ready[d] = G.addr[d] && !ring_has(mgr, dungeons[d].hash);
        waiting += G.addr[d] && !ready[d];
        cur[d] = ready[d] ? (int32_t)LD32(G.addr[d] + I_CURRENT) : 0;
    }
    if (!G.based) {
        if (waiting) return;   // a count is changing: read them all once none is
        uint8_t mine[COOP_KEYS_DUNGEONS] = {0};
        for (int d = 0; d < ND; d++) G.seen[d] = cur[d], mine[d] = (uint8_t)clamp_keys(d, cur[d]);
        G.based = 1;
        char line[512];
        counts_text(mine, line, sizeof line);
        rt_log("coop: keys: this save's small keys: %s", line);
    }
    plat_mutex_lock(&K.lock);
    if (hosting() && !K.have) {   // the room's host without counts: the room's start from this save's, as last read
        for (int d = 0; d < ND; d++) K.keys[d] = (uint8_t)clamp_keys(d, G.seen[d]);
        K.have = 1, K.serial++, K.dirty = 1;
        char line[512];
        counts_text(K.keys, line, sizeof line);
        rt_log("coop: keys: the room's small keys start from this save's (this game is the room's host): %s", line);
    }
    plat_mutex_unlock(&K.lock);
    // what changed here since the last reading or writing: keys picked up or used in this game, where Link stands
    int changes = 0;
    Change ch[COOP_KEYS_DUNGEONS];
    float at[3] = {0, 0, 0};
    int placed = -1;
    for (int d = 0; d < ND; d++) {
        if (!ready[d] || cur[d] == G.seen[d]) continue;
        int delta = cur[d] - G.seen[d];
        G.seen[d] = cur[d];
        if (delta > COOP_KEYS_MAX) delta = COOP_KEYS_MAX;
        if (delta < -COOP_KEYS_MAX) delta = -COOP_KEYS_MAX;
        if (placed < 0) placed = link_place(at);
        ch[changes] = (Change){0, (int8_t)d, (int8_t)delta, (uint8_t)placed, {at[0], at[1], at[2]}};
        changes++;
    }
    int32_t target[COOP_KEYS_DUNGEONS];
    plat_mutex_lock(&K.lock);
    int host = hosting(), have = K.have;
    for (int i = 0; i < changes; i++) {
        int d = ch[i].d, n = ch[i].delta > 0 ? ch[i].delta : -ch[i].delta, door = ch[i].delta < 0 && ch[i].placed;
        char where[64] = "";
        if (ch[i].placed) snprintf(where, sizeof where, " at %.1f %.1f %.1f", ch[i].at[0], ch[i].at[1], ch[i].at[2]);
        if (host && have && door && paid_near(d, ch[i].at) >= 0) {   // the write below sets this game's count back
            rt_log("coop: keys: this game used %d small key%s in %s%s, at a door the room already paid a key for: not counted, "
                   "this game has it back (the room has %d)", n, n == 1 ? "" : "s", dungeons[d].name, where, K.keys[d]);
        } else if (host && have) {
            if (door) paid_add(d, ch[i].at);
            int was = K.keys[d], now = clamp_keys(d, was + ch[i].delta);
            K.keys[d] = (uint8_t)now, K.serial++, K.dirty = 1;
            rt_log("coop: keys: this game %s %d small key%s in %s%s: the room has %d (was %d)%s", change_text(ch[i].delta), n,
                   n == 1 ? "" : "s", dungeons[d].name, where, now, was,
                   now != was + ch[i].delta ? ", within 0 and the keys the dungeon holds" : "");
        } else if (K.npend < PENDING) {
            ch[i].seq = ++K.seq;
            K.pend[K.npend++] = ch[i];
            rt_log("coop: keys: this game %s %d small key%s in %s%s (%d now): change %u goes to the room's host",
                   change_text(ch[i].delta), n, n == 1 ? "" : "s", dungeons[d].name, where, cur[d], ch[i].seq);
        } else {
            rt_log("coop: keys: this game %s %d small key%s in %s, but %d changes already wait for the room's host: this one "
                   "is not sent", change_text(ch[i].delta), n, n == 1 ? "" : "s", dungeons[d].name, PENDING);
        }
    }
    for (int d = 0; d < ND; d++) {   // the room's count, and this game's changes on their way to the host
        int t = K.keys[d];
        for (int p = 0; !host && p < K.npend; p++)
            if (K.pend[p].d == d) t += K.pend[p].delta;
        target[d] = clamp_keys(d, t);
    }
    plat_mutex_unlock(&K.lock);
    if (!have || !coop_game_backed_up()) return;   // no counts of the room yet, or the save not copied for it yet
    Cpu keep = *c;
    int wrote = 0;
    for (int d = 0; d < ND; d++) {
        if (!ready[d] || target[d] == G.seen[d]) continue;
        rt_call(c, SET_INT_FN, 3, mgr, (uint64_t)(uint32_t)target[d], (uint64_t)dungeons[d].hash);
        rt_log("coop: keys: %s: this game's %d small key%s set to the room's %d", dungeons[d].name, G.seen[d], G.seen[d] == 1 ? "" : "s",
               target[d]);
        G.seen[d] = target[d], wrote = 1;
    }
    if (wrote) {
        memcpy(c->x, keep.x, sizeof c->x);
        memcpy(c->v, keep.v, sizeof c->v);
        c->nzcv = keep.nzcv, c->fpsr = keep.fpsr;
    }
}
