// Co-op kills, shared damage and difficulty, game 1.1.1.
//
// The hook. Every GameActor's life component (enemies, bosses, Link: *(*(actor+0x270)+0x2b8), vtable main+0x4b4c460)
// is updated by main+0x49d8c0, x19 the component for the whole function. At main+0x49dcb0 (add w9, w10, w9) it adds
// the update's damage terms: w9 the update's selected hit (comp+0x1350; 0 on the kill override), w10 term 2 plus a
// pending life request (MaxLife on the kill override). The game then records the total (+0x1cb0), clamps it at 0 and
// takes it from Life with a CAS, clamped to [0, MaxLife]; Life 0 takes the death branch (a revive check, then
// requestDeath(LifeDepleted) and the enemy's own death: its animation, its drop roll, its respawn flag). The patch set
// coop_life (recomp/patches-v111.txt) turns the add into a call of life_host (patch page +0xf40), which writes
// w9 = hit' + rest and changes nothing else. The hit is w9 plus term 2 (comp+0x228: the sword's hits and the bites on
// Link land there); rest is what is left of w10, a pending life request's part or the kill override:
//   difficulty    hit' is the hit divided by the health factor for an Enemy_* actor and multiplied by the damage factor
//                 for Link, with a fractional carry per component (16.16), for N players: the room's members in the
//                 game world (their POS) and this player, a member gone counting for 10 s more;
//   shared damage plus the hits the other players dealt to the same placed enemy (DAMAGE), scaled the same way, so
//                 the players fight the same enemy together;
//   shared kills  plus lethal damage when another player killed it (KILL): this world runs its own death, with its
//                 own drop for this player;
//   local deaths  a placed enemy whose Life this update's own damage takes to 0 (a hit of this player, a test or a
//                 change the game itself asked for) is checked at its next update (Life still 0: no revive) and sent
//                 to the room as KILL. A death that only the other players' damage caused is not sent back.
// rest is never scaled or shared: instant kills and scripted life changes stay as the game made them.
//
// The same enemy in another world = the same area (Sequence_CurrentBanc) and the same placement hash, the actor's
// *(*(actor+0x3d8)+0x88) (the Banc placement Hash; also the key of the game's "killed, do not respawn" flag), for an
// actor whose *(actor+0x3d8)+0x14 bit 3 is set (placed, with a respawn flag); the pack name's hash is checked on
// arrival. Spawned enemies have no respawn flag and are left out. Bosses (no respawn flag, a placement hash, a list of
// names) share their hits: the other players' hits leave a boss at 1 life at least, so that the killing blow and the
// boss's death stay each world's own (NX2NAVI_COOP_DAMAGE_BOSS=lethal shares those too, 0 nothing). Mini-bosses, Gold
// Skulltulas, shops and puzzles are on a deny list.
//
// Threads. The host runs on whichever thread updates the actor (several): it reads actor memory, takes K.lock only
// when there is something to do for that component, and never calls guest code. Its common case (no hit, nothing
// pending) is two loads and a compare. Messages go out from the co-op thread (coop_on_tick), each event three times
// (0, 150, 450 ms); receivers keep a window per sender and apply only in the area they are in, to an enemy that updates
// within 60 s (the area's change clears everything).
//
// Settings (environment or nx2navi.ini, no menu row; read again when cfg_version changes; all logged at start):
//   NX2NAVI_COOP_KILLS=0          no shared kills          NX2NAVI_COOP_DAMAGE=0        no shared damage
//   NX2NAVI_COOP_DAMAGE_BOSS      safe (default) | lethal | 0
//   NX2NAVI_COOP_SCALE=0          no difficulty scaling    NX2NAVI_COOP_SCALE_PLAYERS=N a forced player count (test)
//   NX2NAVI_COOP_SCALE_HP / _TAKEN  the factors for 1..4 players (defaults 1,1.5,2,2.5 and 1,1.25,1.5,1.75)
//   NX2NAVI_COOP_SHARE_DENY       more actor-name prefixes the room does not share (comma-separated)
//   NX2NAVI_COOP_KILL_LOG=1       every hit on any actor logged
//   NX2NAVI_COOP_KILL_TEST        tests, ';'-separated, times in seconds after the area began:
//       kill:<m>@<s>[x<n>][/<every>]   lethal damage, as this player's own, to a shared enemy within <m> metres
//       hit:<m>@<s>[/<every>][x<n>][=<raw>]   a hit of <raw> (1) to one shared enemy within <m> metres, again
//       hurt:<raw>@<s>[/<every>][x<n>]        a hit of <raw> to Link
//       doom:<inst>@<s>                 as if another player killed that placed enemy (the name is not checked)
//       dmg:<inst>@<s>[/<every>][x<n>][=<raw>]  as if another player hit it
//       loop                            every message sent is parsed back and logged
// The life hook is on only while there is work for it: in a room with others (kills or damage on), with more than one
// player counted (scaling on), or with a test.
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cfg.h"
#include "coop.h"
#include "coop_kill.h"
#include "coop_offer.h"
#include "coop_puppet.h"
#include "host.h"
#include "platform/platform.h"

#if __has_include("patches.h")
#include "patches.h"
#endif

// ---- the wire format (no game state)

uint32_t coop_kill_name_hash(const char *s) {   // FNV-1a
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

static void put32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> 8 * i);
}
static void put64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> 8 * i);
}
static uint32_t get32(const uint8_t *p) {
    uint32_t v = 0;
    for (int i = 3; i >= 0; i--) v = v << 8 | p[i];
    return v;
}
static uint64_t get64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = v << 8 | p[i];
    return v;
}

static int area_ok(const char *a, size_t n) {
    if (n < 1 || n > COOP_KILL_AREA_MAX) return 0;
    for (size_t i = 0; i < n; i++)
        if (a[i] < 0x21 || a[i] > 0x7e) return 0;
    return 1;
}

size_t coop_kill_pack(uint8_t kind, const CoopKillMsg *m, uint8_t *out, size_t cap) {
    size_t al = strnlen(m->area, COOP_KILL_AREA_MAX + 1);
    if ((kind != COOP_K_KILL && kind != COOP_K_DAMAGE) || !area_ok(m->area, al) || !m->inst || m->inst == UINT64_MAX)
        return 0;
    if (kind == COOP_K_DAMAGE && (m->raw < 1 || m->raw > 1000)) return 0;
    size_t n = 10 + al + 8 + 4 + (kind == COOP_K_DAMAGE ? 2 : 0) + 1;
    if (n > cap) return 0;
    uint8_t *p = out;
    *p++ = COOP_KILL_VERSION;
    put32(p, m->epoch), p += 4;
    put32(p, m->eseq), p += 4;
    *p++ = (uint8_t)al;
    memcpy(p, m->area, al), p += al;
    put64(p, m->inst), p += 8;
    put32(p, m->name_hash), p += 4;
    if (kind == COOP_K_DAMAGE) {
        *p++ = (uint8_t)m->raw;
        *p++ = (uint8_t)(m->raw >> 8);
    }
    *p++ = m->flags;
    return (size_t)(p - out);
}

int coop_kill_parse(uint8_t kind, const uint8_t *b, size_t n, CoopKillMsg *m) {
    memset(m, 0, sizeof *m);
    if ((kind != COOP_K_KILL && kind != COOP_K_DAMAGE) || n < 10 || b[0] != COOP_KILL_VERSION) return 0;
    size_t al = b[9];
    if (n != 10 + al + 8 + 4 + (kind == COOP_K_DAMAGE ? 2u : 0u) + 1 || !area_ok((const char *)b + 10, al)) return 0;
    m->epoch = get32(b + 1), m->eseq = get32(b + 5);
    memcpy(m->area, b + 10, al);
    m->area[al] = 0;
    const uint8_t *p = b + 10 + al;
    m->inst = get64(p), p += 8;
    m->name_hash = get32(p), p += 4;
    if (kind == COOP_K_DAMAGE) m->raw = (uint16_t)(p[0] | p[1] << 8), p += 2;
    m->flags = *p;
    return m->inst && m->inst != UINT64_MAX && (kind != COOP_K_DAMAGE || (m->raw >= 1 && m->raw <= 1000));
}

// A receiver's window per sender: an event (its eseq) is taken once, its repeats and anything 64 events older are not;
// a new epoch (the sender started again) starts the window again.
typedef struct {
    uint32_t epoch, top;
    uint64_t bits;
    uint64_t rate_at;
    int rate_n;
} Window;

static int window_take(Window *w, uint32_t epoch, uint32_t seq) {   // 1: not seen before
    if (w->epoch != epoch) {
        w->epoch = epoch, w->top = seq, w->bits = 1;
        return 1;
    }
    if (seq > w->top) {
        uint32_t d = seq - w->top;
        w->bits = d >= 64 ? 1 : w->bits << d | 1;
        w->top = seq;
        return 1;
    }
    uint32_t d = w->top - seq;
    if (d >= 64 || (w->bits >> d & 1)) return 0;
    w->bits |= 1ull << d;
    return 1;
}

#ifdef RT_PATCH_PAGE

#define MAIN 0x7000000000ull
#define HOST_LIFE (RT_PATCH_PAGE + 0xf40)   // the set coop_life: main+0x49dcb0 add w9, w10, w9 -> bl here
#define PLAYER_HOLDER (MAIN + 0x4e371d0)    // *: +0x480 Link's actor

// the life component
#define LC_ACTOR 0x18       // the actor (the life update reads its +0x270 through it)
#define LC_LIFE 0x1c10      // -> {vtable, s32 Life at +8}
#define LC_MAX 0x1c18       // -> {vtable, s32 MaxLife at +8}
#define LC_FLAGS 0x1d18     // bit 8: the update takes nothing from Life (main+0x49dcc4)
#define LC_TERM2 0x228      // s32: the update's second damage term (w10 = this + the request's part); the sword's hits
                            // arrive here; the kill override puts MaxLife here
#define LC_REQ 0x2c         // u8: 1 while a life request is pending (its part of w10: main+0x49ea54, 0x49ec88)
#define LC_OVERRIDE 0x252   // bit 5: the kill override (term 1 0, term 2 MaxLife: main+0x49ea78)
// the actor
#define A_DEF_NAME 0x260    // char *: the actor pack's name
#define A_COMPONENTS 0x270  // +0x2b8: the life component
#define AC_LIFE 0x2b8
#define A_POS 0x31c         // float[3], metres
#define A_INFO 0x3d8        // the instance info: +0x14 flags (bit 3: has a respawn flag), +0x88 the placement hash
#define I_FLAGS 0x14
#define I_INST 0x88

#define LETHAL 0x100000     // added for a kill: past any MaxLife; the game clamps Life at 0
enum { BOSS_OFF, BOSS_SAFE, BOSS_LETHAL };
static const char *const boss_mode_names[] = {"0 (bosses share nothing)",
                                              "safe (their hits; the others' hits leave a boss 1 life)",
                                              "lethal (their hits, killing blows included)"};

// The main actors of the bosses (each with a placement hash; their parts and minions are denied below).
static const char *const bosses[] = {
    "Enemy_DungeonBoss_Gohma", "Enemy_DungeonBoss_Dodongo", "Enemy_DungeonBoss_Barinade",
    "Enemy_DungeonBoss_PhantomGanon", "Enemy_PhantomGanonHorse", "Enemy_DungeonBoss_Volvagia",
    "Enemy_DungeonBoss_Morpha", "Enemy_DungeonBoss_BongoBongo", "Enemy_DungeonBoss_Twinrova",
    "Enemy_DungeonBoss_Kotake", "Enemy_DungeonBoss_Koume", "Enemy_DungeonBoss_Ganondorf", "Enemy_DungeonBoss_Ganon",
    "Enemy_DungeonBoss_Ganon_SecondHalfBattle"};
// Not shared (v1): mini-bosses and scripted fights (room locks, scenes, rewards), Gold Skulltulas (tokens are each
// player's), business scrubs (shops), puzzles (the Deku scrub brothers, the forest stage, the Poe sisters, the
// paintings), Big Poes (caught per player), the boss parts and minions, the dictionary's and attention actors.
static const char *const deny_prefix[] = {
    "Enemy_MiddleBoss_", "Enemy_IronKnuckle_", "Enemy_DeadHand", "Enemy_Skulltula_Gold", "Enemy_Scrub_Business",
    "Enemy_Scrub_Deku", "Enemy_Scrub_ForestStage", "Enemy_GhostPicturePerformer_", "Enemy_Poe_Big", "Enemy_Poe_Sharp",
    "Enemy_Poe_Flat", "Enemy_Poe_Amy", "Enemy_Poe_Beth", "Enemy_Poe_Joelle", "Enemy_Poe_Meg", "Enemy_Poe_SistersBase",
    "Enemy_SkullKid", "Enemy_FlockGuayManager", "Enemy_KotakeKoumeBattleManager", "Enemy_DungeonBoss_"};
static const char *const deny_part[] = {"_Dictionary", "_Attention", "_Haribote", "_AlwaysRespawn"};
static const char *const deny_area[] = {"DreamWorld", "EnemyMuseum", "BossRush"};

#define NE 64      // the other players' kills and hits waiting for their enemy here
#define NC 128     // fractional carries, one per life component
#define NP 16      // local deaths waiting for the next update's confirmation
#define NO 64      // events going out
#define NT 8       // test items

typedef struct {
    _Atomic uint64_t inst;   // 0: free. Written under K.lock; the host's quick probe reads it without
    uint32_t hash;           // the sender's name hash (0: a test, any name)
    uint8_t from, doom, boss, said;
    int32_t pending;         // raw hit points not applied yet
    uint64_t t_ms, doom_ms, applied_comp, applied_ms;
    int tries;
} Entry;

typedef struct {
    uint64_t comp, actor, t_ms;
    int32_t carry;           // 1/65536 of a life point
} Carry;

typedef struct {
    _Atomic uint64_t comp;   // 0: free
    uint64_t inst, t_ms;
    uint32_t hash, area_gen;
    int32_t life, dmg;
    char name[48];
} Pend;

typedef struct {
    uint8_t used, kind, copies, len;
    uint32_t eseq;
    uint64_t t0, inst;
    uint8_t buf[COOP_KILL_MSG_MAX];
} Out;

enum { T_KILL, T_HIT, T_HURT, T_DOOM, T_DMG, T_KINDS };
static const char *const test_names[T_KINDS] = {"kill", "hit", "hurt", "doom", "dmg"};
typedef struct {
    int kind, count, raw, done;
    float radius;
    uint64_t at_ms, every_ms, inst, target, target_ms;
    uint64_t fixed;          // kill/hit: '#<inst>' that enemy only, wherever it is
    int after_mate;          // '~<s>' instead of '@<s>': seconds after another member was first seen in this area
    uint32_t area_gen;
} Test;

static struct {
    int set;                 // the patch set's index, -1 without it
    unsigned cfg_seen;
    // settings (present thread; the other threads read them as they are)
    int kills, damage, scale, boss_mode, log_all, census, loop, forced, factors_dirty;
    double hp[4], taken[4];
    char deny_extra[256];
    Test test[NT];
    int ntest;
    // published
    _Atomic int on, work, ne_live, np_live;
    _Atomic uint32_t mul_enemy, mul_link;   // 16.16: 65536 / health factor, 65536 * damage factor
    _Atomic int others, self;
    _Atomic uint32_t present_ids;           // the other members the roster does not call away
    _Atomic uint64_t link_comp, area_t0;
    _Atomic uint64_t mate_t0;                // when another member was first seen in this area (POS), 0: none now
    int link_said;
    // the present thread's
    int players;                            // N in effect
    uint64_t lower_at, count_at, stats_at;
    // under lock
    PlatMutex lock;
    char area[COOP_KILL_AREA_MAX + 1];
    uint32_t area_gen;
    float link_pos[3];
    Entry e[NE];
    int ne;
    Carry carry[NC];
    struct { uint64_t comp, inst; uint32_t count; float d; char name[40]; } seen[96];   // the census of this area
    int nseen;
    uint64_t census_at;
    Pend pend[NP];
    int np;
    Out out[NO];
    uint32_t epoch, eseq;
    Window win[COOP_MAX_MEMBERS + 1];
    uint32_t hits, link_hits, kills_sent, dmg_sent, kills_got, dmg_got, applied, died, dropped_out;
    uint32_t stats_seen;
    uint64_t log_at;
    int log_n, log_cut;
} K = {.set = -1};

static int ok_ptr(uint64_t a, uint64_t n) { return a >= 0x10000 && a < (1ull << 48) && mem_readable(a, n); }

// A C string of the guest (at most cap - 1 bytes; "" when not readable). Readability is checked per page.
static void text_at(uint64_t a, char *out, size_t cap) {
    out[0] = 0;
    if (!ok_ptr(a, 1)) return;
    size_t i = 0;
    for (; i + 1 < cap; i++) {
        if (i && ((a + i) & 0xfff) == 0 && !ok_ptr(a + i, 1)) break;
        char ch = (char)LD8(a + i);
        if (!ch) break;
        out[i] = ch >= 0x20 && ch < 0x7f ? ch : '?';
    }
    out[i] = 0;
}

static const char *wall(char out[16]) {
    uint64_t ns = plat_unix_time_ns();
    struct tm tm;
    if (!ns || !plat_localtime((int64_t)(ns / 1000000000u), &tm)) {
        snprintf(out, 16, "?");
        return out;
    }
    snprintf(out, 16, "%02d:%02d:%02d.%03d", tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(ns / 1000000u % 1000u));
    return out;
}

static int is_boss(const char *n) {
    for (size_t i = 0; i < sizeof bosses / sizeof *bosses; i++)
        if (!strcmp(n, bosses[i])) return 1;
    return 0;
}

// Why the room does not share this enemy's kills and hits, or NULL. K.deny_extra: more prefixes, comma-separated.
static const char *deny_why(const char *n) {
    for (size_t i = 0; i < sizeof deny_prefix / sizeof *deny_prefix; i++)
        if (!strncmp(n, deny_prefix[i], strlen(deny_prefix[i]))) return deny_prefix[i];
    for (size_t i = 0; i < sizeof deny_part / sizeof *deny_part; i++)
        if (strstr(n, deny_part[i])) return deny_part[i];
    for (const char *p = K.deny_extra; *p;) {
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len && !strncmp(n, p, len)) return "NX2NAVI_COOP_SHARE_DENY";
        p += len + (e ? 1 : 0);
    }
    return NULL;
}

static int area_denied(const char *a) {
    for (size_t i = 0; i < sizeof deny_area / sizeof *deny_area; i++)
        if (!strncmp(a, deny_area[i], strlen(deny_area[i]))) return 1;
    return 0;
}

typedef struct {
    uint64_t inst;
    uint32_t iflags, hash;
    uint8_t enemy, placed, boss;
    const char *deny;        // why the room does not share it; NULL: shared
    float pos[3];
    char name[64];
} Ident;

// A placement hash: 0 and all ones are none (Link's own is all ones; so may be a spawned actor's).
static int placed_id(uint64_t inst) { return inst && inst != UINT64_MAX; }

static void ident(uint64_t actor, uint64_t inst, uint32_t iflags, Ident *id) {
    memset(id, 0, sizeof *id);
    id->inst = inst, id->iflags = iflags;
    text_at(LD64(actor + A_DEF_NAME), id->name, sizeof id->name);
    id->hash = coop_kill_name_hash(id->name);
    id->enemy = !strncmp(id->name, "Enemy_", 6);
    id->boss = id->enemy && is_boss(id->name);
    id->deny = id->enemy && !id->boss ? deny_why(id->name) : NULL;
    id->placed = id->enemy && !id->boss && (iflags & 8) && placed_id(inst);
    memcpy(id->pos, (const void *)(uintptr_t)(actor + A_POS), sizeof id->pos);
}

// The room shares this enemy's kills: placed (a respawn flag and a placement hash), not denied.
static int share_kill(const Ident *id) { return id->placed && !id->deny; }
// and its hits: those, and a boss with a placement hash (unless bosses are off).
static int share_hits(const Ident *id) { return share_kill(id) || (id->boss && placed_id(id->inst) && K.boss_mode != BOSS_OFF); }

static const char *kind_of(const Ident *id) {
    return id->boss ? (placed_id(id->inst) ? "boss" : "boss, no placement hash") : id->deny ? id->deny
         : id->placed ? "placed" : !placed_id(id->inst) ? "no placement hash" : "no respawn flag: spawned";
}

static void work_update(void) {   // under K.lock
    atomic_store(&K.ne_live, K.ne);
    atomic_store(&K.np_live, K.np);
    atomic_store(&K.work, K.ne > 0 || K.np > 0 || K.ntest > 0 || K.census);
}

// ---- log lines from the hosts: gathered under the lock, written after it, at most 40 a second
typedef struct {
    char t[5][260];
    int n;
} Lines;

static void say(Lines *L, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void say(Lines *L, const char *fmt, ...) {
    if (L->n >= 5) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(L->t[L->n++], sizeof L->t[0], fmt, ap);
    va_end(ap);
}

static int budget(Lines *L, uint64_t now, int *cut) {   // under K.lock: how many of L's lines may go
    *cut = 0;
    if (now - K.log_at >= 1000) *cut = K.log_cut, K.log_at = now, K.log_n = K.log_cut = 0;
    int n = L->n;
    if (K.log_n + n > 40) n = 40 - K.log_n > 0 ? 40 - K.log_n : 0;
    K.log_n += n, K.log_cut += L->n - n;
    return n;
}

static void flush(Lines *L, int n, int cut) {
    if (cut) rt_log("coop: kill: %d host log lines left out in the last second (40 a second at most)", cut);
    for (int i = 0; i < n; i++) rt_log("%s", L->t[i]);
}

// ---- the tables (under K.lock)

static Entry *entry_find(uint64_t inst) {
    for (int i = 0; i < NE; i++)
        if (atomic_load_explicit(&K.e[i].inst, memory_order_relaxed) == inst) return &K.e[i];
    return NULL;
}

static int entry_probe(uint64_t inst) {   // without the lock
    if (!atomic_load_explicit(&K.ne_live, memory_order_relaxed)) return 0;
    for (int i = 0; i < NE; i++)
        if (atomic_load_explicit(&K.e[i].inst, memory_order_relaxed) == inst) return 1;
    return 0;
}

static Entry *entry_add(uint64_t inst, uint32_t hash, uint64_t now) {
    Entry *e = entry_find(inst);
    if (e) return e;
    for (int i = 0; i < NE; i++) {
        e = &K.e[i];
        if (atomic_load_explicit(&e->inst, memory_order_relaxed)) continue;
        e->hash = hash, e->from = 0, e->doom = 0, e->boss = 0, e->said = 0, e->pending = 0;
        e->t_ms = now, e->doom_ms = 0, e->applied_comp = 0, e->applied_ms = 0, e->tries = 0;
        atomic_store(&e->inst, inst);
        K.ne++;
        work_update();
        return e;
    }
    return NULL;
}

static void entry_free(Entry *e) {
    atomic_store(&e->inst, 0);
    K.ne--;
    work_update();
}

static int pend_probe(uint64_t comp) {   // without the lock
    if (!atomic_load_explicit(&K.np_live, memory_order_relaxed)) return 0;
    for (int i = 0; i < NP; i++)
        if (atomic_load_explicit(&K.pend[i].comp, memory_order_relaxed) == comp) return 1;
    return 0;
}

static void pend_free(Pend *p) {
    atomic_store(&p->comp, 0);
    K.np--;
    work_update();
}

static void clear_tables(void) {   // under K.lock: everything of the last area or of a room left
    for (int i = 0; i < NE; i++) atomic_store(&K.e[i].inst, 0);
    for (int i = 0; i < NP; i++) atomic_store(&K.pend[i].comp, 0);
    memset(K.carry, 0, sizeof K.carry);
    K.nseen = 0, K.census_at = 0;
    K.ne = K.np = 0;
    work_update();
}

// A message to the room: packed now (the area it happened in), sent from the co-op thread three times.
static void queue_msg(uint8_t kind, uint64_t inst, uint32_t hash, int32_t raw, uint8_t flags, uint64_t now, Lines *L) {
    CoopKillMsg m = {.epoch = K.epoch, .eseq = ++K.eseq, .inst = inst, .name_hash = hash, .flags = flags};
    snprintf(m.area, sizeof m.area, "%s", K.area);
    m.raw = (uint16_t)(raw < 1 ? 1 : raw > 1000 ? 1000 : raw);
    Out *o = NULL, *oldest = &K.out[0];
    for (int i = 0; i < NO && !o; i++) {
        if (!K.out[i].used) o = &K.out[i];
        else if (K.out[i].t0 < oldest->t0) oldest = &K.out[i];
    }
    if (!o) o = oldest, K.dropped_out++;
    o->len = (uint8_t)coop_kill_pack(kind, &m, o->buf, sizeof o->buf);
    if (!o->len) {
        say(L, "coop: kill: a %s message for inst %llu in '%s' does not pack: not sent", kind == COOP_K_KILL ? "KILL" : "DAMAGE",
            (unsigned long long)inst, K.area);
        o->used = 0;
        return;
    }
    o->used = 1, o->kind = kind, o->copies = 0, o->eseq = m.eseq, o->t0 = now, o->inst = inst;
    if (kind == COOP_K_KILL) K.kills_sent++;
    else K.dmg_sent++;
    if (K.loop) {   // test: the bytes parsed back as a receiver would
        CoopKillMsg back;
        int ok = coop_kill_parse(kind, o->buf, o->len, &back);
        say(L, "coop: kill: loop %s eseq %u: %u bytes, parse %d: area '%s' inst %llu hash %08x raw %u flags %u%s",
            kind == COOP_K_KILL ? "KILL" : "DAMAGE", m.eseq, o->len, ok, back.area, (unsigned long long)back.inst,
            back.name_hash, back.raw, back.flags,
            ok && back.inst == m.inst && back.name_hash == m.name_hash && !strcmp(back.area, m.area) &&
                    (kind == COOP_K_KILL || back.raw == m.raw) ? " (the same)" : " (DIFFERS)");
    }
}

// A local death goes to the room (or says why not).
static void kill_out(uint64_t inst, uint32_t hash, const char *name, int32_t life, int32_t dmg, const char *how,
                     uint64_t now, Lines *L) {
    char w[16];
    const char *why = !K.kills ? "kills are off (NX2NAVI_COOP_KILLS=0)"
                    : !atomic_load(&K.others) && !K.loop ? "no other player in the room"
                    : area_denied(K.area) ? "an area the room does not share" : NULL;
    if (!why) queue_msg(COOP_K_KILL, inst, hash, 0, 0, now, L);
    say(L, "coop: kill: %s %s inst %llu died here (life %d, this world's damage %d; %s): %s%s", wall(w), name,
        (unsigned long long)inst, life, dmg, how, why ? "not sent: " : "KILL sent to the room", why ? why : "");
}

static void pend_add(uint64_t comp, const Ident *id, int32_t life, int32_t dmg, uint64_t now, Lines *L) {
    for (int i = 0; i < NP; i++) {
        Pend *p = &K.pend[i];
        if (atomic_load_explicit(&p->comp, memory_order_relaxed)) continue;
        p->inst = id->inst, p->hash = id->hash, p->t_ms = now, p->area_gen = K.area_gen, p->life = life, p->dmg = dmg;
        snprintf(p->name, sizeof p->name, "%s", id->name);
        atomic_store(&p->comp, comp);
        K.np++;
        work_update();
        return;
    }
    kill_out(id->inst, id->hash, id->name, life, dmg, "not confirmed: 16 deaths wait already", now, L);
}

// The component's next update after its Life reached 0 by this world's damage: still 0 is a death.
static void pend_check(uint64_t comp, int32_t life, uint64_t now, Lines *L) {
    for (int i = 0; i < NP; i++) {
        Pend *p = &K.pend[i];
        if (atomic_load_explicit(&p->comp, memory_order_relaxed) != comp) continue;
        if (p->area_gen == K.area_gen) {
            if (life <= 0) kill_out(p->inst, p->hash, p->name, p->life, p->dmg, "confirmed at its next update", now, L);
            else say(L, "coop: kill: %s inst %llu has life %d again after its life reached 0 (a revive): not sent",
                     p->name, (unsigned long long)p->inst, life);
        }
        pend_free(p);
    }
}

static int32_t scale(uint64_t comp, uint64_t actor, int32_t raw, uint32_t mul, uint64_t now, int32_t *carry) {
    Carry *k = NULL, *old = &K.carry[0];
    for (int i = 0; i < NC; i++) {
        if (K.carry[i].comp == comp) {
            k = &K.carry[i];
            break;
        }
        if (K.carry[i].t_ms < old->t_ms) old = &K.carry[i];
    }
    if (!k || k->actor != actor) {   // a new component, or one whose memory another actor took over
        if (!k) k = old;
        k->comp = comp, k->actor = actor, k->carry = 0;
    }
    k->t_ms = now;
    int64_t v = (int64_t)raw * mul + k->carry;
    k->carry = (int32_t)(v & 0xffff);
    *carry = k->carry;
    return (int32_t)(v >> 16);
}

static float dist_link(const float p[3]) {
    float dx = p[0] - K.link_pos[0], dy = p[1] - K.link_pos[1], dz = p[2] - K.link_pos[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

// The tests that act inside the update of an actor (kill, hit, hurt).
static void run_tests(const Ident *id, int link, int32_t life, uint64_t now, uint64_t t_area, int32_t *raw, int *lethal,
                      Lines *L) {
    for (int i = 0; i < K.ntest; i++) {
        Test *t = &K.test[i];
        if (t->kind == T_DOOM || t->kind == T_DMG) continue;   // the present thread's (housekeeping)
        if (t->area_gen != K.area_gen) t->area_gen = K.area_gen, t->done = 0, t->target = 0;   // again in each area
        if (t->target && t->target == id->inst) t->target_ms = now;   // the target still updates
        uint64_t mate = t->after_mate ? atomic_load(&K.mate_t0) : 0, t_rel = t->after_mate ? now - mate : t_area;
        if (t->done >= t->count || (t->after_mate && !mate) || t_rel < t->at_ms + (uint64_t)t->done * t->every_ms) continue;
        if (t->kind == T_HURT) {
            if (!link) continue;
            *raw += t->raw;
            t->done++;
            say(L, "coop: test: hurt %d of %d: %d (raw) to Link, life %d", t->done, t->count, t->raw, life);
            continue;
        }
        if (link || !(t->kind == T_KILL ? share_kill(id) : share_hits(id))) continue;
        if (t->target && !t->fixed && now - t->target_ms > 1000) t->target = 0;   // it stopped updating: another one
        if ((t->fixed && t->fixed != id->inst) || (t->target && t->target != id->inst)) continue;
        if (life <= 0) {   // the target died: the next one within reach
            if (t->target == id->inst) t->target = 0;
            continue;
        }
        float d = dist_link(id->pos);
        if (!t->target && !t->fixed && d > t->radius) continue;
        t->target = id->inst, t->target_ms = now;
        t->done++;
        if (t->kind == T_KILL) *lethal = 1;
        else *raw += t->raw;
        say(L, "coop: test: %s %d of %d: %s inst %llu at %.1f m from Link, life %d", test_names[t->kind], t->done,
            t->count, id->name, (unsigned long long)id->inst, d, life);
    }
}

// What the other players did to this enemy, for its update here.
static void apply_entry(Entry *e, uint64_t comp, const Ident *id, int32_t life, int32_t max, uint64_t now, int32_t *raw,
                        int *lethal, int *remote_kill, Lines *L) {
    char w[16];
    if (e->hash && e->hash != id->hash) {
        say(L, "coop: kill: inst %llu here is %s (name hash %08x), not member %d's (%08x): dropped", (unsigned long long)e->inst,
            id->name, id->hash, e->from, e->hash);
        entry_free(e);
        return;
    }
    if (!id->enemy || !share_hits(id)) {
        say(L, "coop: kill: inst %llu here is %s (%s), which the room does not share: dropped", (unsigned long long)e->inst,
            id->name, kind_of(id));
        entry_free(e);
        return;
    }
    if (e->pending > 0) {
        *raw += e->pending;
        K.applied++;
        say(L, "coop: damage: %s member %d's hits on %s inst %llu: %d (raw) applied here, life %d/%d before", wall(w),
            e->from, id->name, (unsigned long long)id->inst, e->pending, life, max);
        e->pending = 0;
    }
    if (e->doom) {
        if (id->boss && K.boss_mode != BOSS_LETHAL) {
            e->doom = 0;
        } else if (e->applied_comp == comp) {
            if (life <= 0) {
                K.died++;
                say(L, "coop: kill: %s %s inst %llu died here %llu ms after member %d's kill arrived", wall(w), id->name,
                    (unsigned long long)id->inst, (unsigned long long)(now - e->doom_ms), e->from);
                entry_free(e);
                return;
            }
            if (now - e->applied_ms >= 250) {
                if (++e->tries > 40) {
                    say(L, "coop: kill: %s inst %llu stays alive after 40 lethal hits (life %d): given up", id->name,
                        (unsigned long long)id->inst, life);
                    entry_free(e);
                    return;
                }
                *lethal = *remote_kill = 1;
                e->applied_ms = now;
                if (!e->said++)
                    say(L, "coop: kill: %s inst %llu is still alive (life %d, invincible for now?): lethal damage again",
                        id->name, (unsigned long long)id->inst, life);
            }
        } else if (life > 0) {
            *lethal = *remote_kill = 1;
            e->applied_comp = comp, e->applied_ms = now, e->tries = 1;
            say(L, "coop: kill: %s lethal damage to %s inst %llu, life %d/%d (member %d killed it %llu ms ago)", wall(w),
                id->name, (unsigned long long)id->inst, life, max, e->from, (unsigned long long)(now - e->doom_ms));
        } else {
            say(L, "coop: kill: %s inst %llu is already dead here (member %d's kill)", id->name, (unsigned long long)id->inst,
                e->from);
            entry_free(e);
            return;
        }
    }
    if (!e->doom && !e->pending) entry_free(e);
}

static uint64_t link_comp_of(uint64_t player) {
    if (!player) return 0;
    uint64_t t = LD64(player + A_COMPONENTS);
    return ok_ptr(t, AC_LIFE + 8) ? LD64(t + AC_LIFE) : 0;
}

// The slow path: a hit, a request, or work for this component.
static int32_t life_slow(uint64_t comp, int32_t h, int32_t r) {
    uint64_t actor = LD64(comp + LC_ACTOR);
    if (!ok_ptr(actor, 0x400)) return h + r;
    uint64_t info = LD64(actor + A_INFO), inst = 0;
    uint32_t iflags = 0;
    if (info && ok_ptr(info, I_INST + 8)) iflags = LD32(info + I_FLAGS), inst = LD64(info + I_INST);
    int link = comp == atomic_load(&K.link_comp);
    int has_entry = placed_id(inst) && entry_probe(inst), has_pend = pend_probe(comp), tests = K.ntest > 0 || K.census;
    if (h <= 0 && r <= 0 && !has_entry && !has_pend && !tests) return h + r;   // nothing for this one
    uint64_t lh = LD64(comp + LC_LIFE), mh = LD64(comp + LC_MAX);
    int32_t life = ok_ptr(lh, 12) ? (int32_t)LD32(lh + 8) : 0, max = ok_ptr(mh, 12) ? (int32_t)LD32(mh + 8) : 0;
    Ident id;
    ident(actor, inst, iflags, &id);
    if (!link && !id.enemy && h <= 0 && r <= 0 && !has_entry && !has_pend) return h + r;   // tests: enemies and Link only
    uint64_t now = plat_now_ms();
    // w10 = term 2 + the request's part. Term 2 is a hit (the sword's land there); the request (the AI's own life
    // changes, a kill request) and the kill override (term 2 = MaxLife) are never scaled or shared.
    int ovr = (LD8(comp + LC_OVERRIDE) >> 5) & 1, req = LD8(comp + LC_REQ) == 1;
    int32_t t2 = ovr ? 0 : req ? (int32_t)LD32(comp + LC_TERM2) : r;
    if (t2 < 0 || t2 > r) t2 = 0;   // read while another thread changed it: all of w10 stays as it is
    int32_t rest = r - t2;
    int32_t local = (h > 0 ? h : 0) + t2, test_raw = 0, remote_raw = 0, carry = 0;
    int lethal = 0, remote_kill = 0;
    Lines L = {.n = 0};
    char w[16];
    plat_mutex_lock(&K.lock);
    if (has_pend) pend_check(comp, life, now, &L);
    if (K.ntest) run_tests(&id, link, life, now, now - atomic_load(&K.area_t0), &test_raw, &lethal, &L);
    if ((K.ntest || K.census) && (id.enemy || K.log_all)) {   // the census: which enemies update, and how often
        int i = 0;
        while (i < K.nseen && K.seen[i].comp != comp) i++;
        if (i < K.nseen) {
            K.seen[i].count++, K.seen[i].d = dist_link(id.pos);
        } else if (K.nseen < (int)(sizeof K.seen / sizeof *K.seen)) {
            K.seen[i].comp = comp, K.seen[i].inst = id.inst, K.seen[i].count = 1, K.seen[i].d = dist_link(id.pos);
            snprintf(K.seen[i].name, sizeof K.seen[i].name, "%s", id.name + (id.enemy ? 6 : 0));
            K.nseen++;
            say(&L, "coop: kill: census: %s inst %llu (%s, flags %#x) at %.1f %.1f %.1f, %.1f m from Link, life %d/%d, thread %u",
                id.name, (unsigned long long)id.inst, kind_of(&id), id.iflags, id.pos[0], id.pos[1], id.pos[2],
                dist_link(id.pos), life, max, plat_thread_id());
        }
    }
    Entry *e = has_entry ? entry_find(inst) : NULL;
    if (e) apply_entry(e, comp, &id, life, max, now, &remote_raw, &lethal, &remote_kill, &L);
    int32_t own = local + test_raw, raw = own + remote_raw, out = raw;
    uint32_t mul = link ? atomic_load(&K.mul_link) : id.enemy ? atomic_load(&K.mul_enemy) : 65536;
    if (raw > 0 && mul != 65536) out = scale(comp, actor, raw, mul, now, &carry);
    if (h < 0) out += h;   // a negative hit term passes as it came
    // bosses, safe: the other players' hits never take the last life (the killing blow stays this world's own)
    if (id.boss && K.boss_mode == BOSS_SAFE && remote_raw > 0 && own <= 0 && life - out < 1) out = life > 1 ? life - 1 : 0;
    int32_t total = out + rest + (lethal ? LETHAL : 0);
    if (remote_raw > 0)
        say(&L, "coop: damage: %s the other players' %d (raw) on %s inst %llu -> %d (x%.3f, carry %.2f)%s: life %d -> %d", wall(w),
            remote_raw, id.name, (unsigned long long)id.inst, out, mul / 65536.0, carry / 65536.0,
            own > 0 ? ", with this player's own" : "", life, life - total > 0 ? life - total : 0);
    if (rest > 0 && (K.log_all || id.enemy || link))
        say(&L, "coop: damage: %s %s inst %llu: %d of the game's own (%s; not scaled, not shared), life %d/%d", wall(w),
            link ? "Link" : id.name, (unsigned long long)id.inst, rest, ovr ? "the kill override" : "a life request", life, max);
    int others = atomic_load(&K.others) > 0;
    // this player's hit on an enemy the room shares: to the others
    int sent = 0;
    if (own > 0 && !link && share_hits(&id) && K.damage && (others || K.loop) && !area_denied(K.area)) {
        queue_msg(COOP_K_DAMAGE, id.inst, id.hash, own, id.boss ? COOP_KILL_BOSS : 0, now, &L);
        sent = 1;
    }
    if (own > 0 || (K.log_all && h > 0)) {
        if (link) K.link_hits++;
        else if (id.enemy) K.hits++;
        if (link)
            say(&L, "coop: scale: %s Link takes %d (terms %d + %d%s) -> %d (x%.2f%s), life %d/%d", wall(w), own, h > 0 ? h : 0, t2,
                test_raw ? ", test" : "", out, mul / 65536.0, mul != 65536 ? ", with the carry" : "", life, max);
        else if (id.enemy || K.log_all)
            say(&L, "coop: damage: %s hit %d (terms %d + %d%s) on %s inst %llu (%s) -> %d (x%.3f, carry %.2f), life %d/%d%s%s",
                wall(w), own, h > 0 ? h : 0, t2, test_raw ? ", test" : "", id.name, (unsigned long long)id.inst, kind_of(&id), out,
                mul / 65536.0, carry / 65536.0, life, max, sent ? ": DAMAGE sent" : "", remote_raw ? " (with other players' hits)" : "");
    }
    // a death by this update: Life > 0 now, nothing protects it, and the total reaches it
    int invincible = (LD32(comp + LC_FLAGS) >> 8) & 1;
    if (!link && id.enemy && life > 0 && !invincible && total >= life) {
        if ((own > 0 || rest > 0 || (lethal && !remote_kill)) && share_kill(&id))
            pend_add(comp, &id, life, total, now, &L);
        else if (!share_kill(&id) && (own > 0 || rest > 0))
            say(&L, "coop: kill: %s %s inst %llu dies here (%s): not shared", wall(w), id.name, (unsigned long long)id.inst,
                kind_of(&id));
        else if (remote_raw > 0 && !remote_kill)
            say(&L, "coop: damage: %s %s inst %llu dies here of the other players' hits", wall(w), id.name,
                (unsigned long long)id.inst);
    }
    int cut, n = budget(&L, now, &cut);
    plat_mutex_unlock(&K.lock);
    flush(&L, n, cut);
    return total;
}

static void note_thread(void) {
    static _Thread_local int seen;
    static _Atomic int threads;
    if (seen) return;
    seen = 1;
    int k = atomic_fetch_add(&threads, 1) + 1;
    if (k <= 16) rt_log("coop: kill: the life hook runs on thread %u (%d threads so far)", plat_thread_id(), k);
}

// main+0x49dcb0 add w9, w10, w9 (x19 the life component): only w9 may change; x8, x10, x11 and x0 are read after it.
static void life_host(Cpu *c) {
    uint64_t comp = c->x[19];
    int32_t h = (int32_t)(uint32_t)c->x[9], r = (int32_t)(uint32_t)c->x[10];
    note_thread();
    if (!K.link_said && comp == atomic_load_explicit(&K.link_comp, memory_order_relaxed)) {
        K.link_said = 1;
        rt_log("coop: kill: Link's life update passes the hook (life component %#llx, thread %u)",
               (unsigned long long)comp, plat_thread_id());
    }
    if ((h > 0 || r > 0 || atomic_load_explicit(&K.work, memory_order_relaxed)))
        h = life_slow(comp, h, r), r = 0;
    c->x[9] = (uint32_t)((uint32_t)h + (uint32_t)r);
}

// ---- the co-op thread: what the others sent, and the sends


static void on_msg(int from, const uint8_t *bytes, size_t n, void *user) {
    uint8_t kind = (uint8_t)(uintptr_t)user;
    const char *what = kind == COOP_K_KILL ? "KILL" : "DAMAGE";
    CoopKillMsg m;
    if (!coop_kill_parse(kind, bytes, n, &m)) {
        static uint64_t said;
        uint64_t now = plat_now_ms();
        if (now - said >= 5000) said = now, rt_log("coop: kill: a %s message of member %d does not parse (%zu bytes)", what, from, n);
        return;
    }
    if (from < 1 || from > COOP_MAX_MEMBERS || from == atomic_load(&K.self)) return;
    uint64_t now = plat_now_ms();
    int trusted = coop_game_shown() >> from & 1;
    char line[320], w[16];
    plat_mutex_lock(&K.lock);
    Window *wn = &K.win[from];
    if (!window_take(wn, m.epoch, m.eseq)) {   // the second or third copy
        plat_mutex_unlock(&K.lock);
        return;
    }
    if (now - wn->rate_at >= 1000) wn->rate_at = now, wn->rate_n = 0;
    if (++wn->rate_n > 100) {
        int first = wn->rate_n == 101;
        plat_mutex_unlock(&K.lock);
        if (first) rt_log("coop: kill: member %d sends more than 100 events a second: the rest of this second is dropped", from);
        return;
    }
    if (kind == COOP_K_KILL) K.kills_got++;
    else K.dmg_got++;
    const char *why = !trusted ? "the member is not accepted or not identified"
                    : !atomic_load(&K.on) ? "the life hook is off here"
                    : kind == COOP_K_KILL && !K.kills ? "kills are off here (NX2NAVI_COOP_KILLS=0)"
                    : kind == COOP_K_DAMAGE && !K.damage ? "damage is off here (NX2NAVI_COOP_DAMAGE=0)"
                    : strcmp(m.area, K.area) ? "another area"
                    : area_denied(K.area) ? "an area the room does not share"
                    : (m.flags & COOP_KILL_BOSS) && K.boss_mode == BOSS_OFF ? "bosses are off here" : NULL;
    Entry *e = NULL;
    if (!why && !(e = entry_add(m.inst, m.name_hash, now))) why = "the table is full";
    if (e) {
        e->from = (uint8_t)from, e->t_ms = now, e->boss = m.flags & COOP_KILL_BOSS;
        if (kind == COOP_K_KILL) {
            if (!e->doom) e->doom_ms = now;
            e->doom = 1;
        } else {
            e->pending += m.raw;
        }
    }
    if (kind == COOP_K_KILL)
        snprintf(line, sizeof line, "coop: kill: %s member %d killed inst %llu (name hash %08x) in %s: %s%s", wall(w), from,
                 (unsigned long long)m.inst, m.name_hash, m.area, why ? "dropped: " : "it dies here at its next update",
                 why ? why : "");
    else
        snprintf(line, sizeof line, "coop: damage: %s member %d hit inst %llu (name hash %08x) in %s for %u (raw)%s: %s%s",
                 wall(w), from, (unsigned long long)m.inst, m.name_hash, m.area, m.raw, m.flags & COOP_KILL_BOSS ? ", a boss" : "",
                 why ? "dropped: " : "applied at its next update here", why ? why : "");
    plat_mutex_unlock(&K.lock);
    rt_log("%s", line);
}

static void on_view(const CoopView *v, void *user) {
    (void)user;
    int others = v->state == COOP_JOINED && v->n > 1 ? v->n - 1 : 0;
    uint32_t ids = 0;
    for (int i = 0; i < v->n; i++)
        if (v->member[i].id != v->self && !v->member[i].away && v->member[i].id >= 1 && v->member[i].id <= COOP_MAX_MEMBERS)
            ids |= 1u << v->member[i].id;
    atomic_store(&K.self, v->self);
    atomic_store(&K.present_ids, ids);
    atomic_store(&K.others, others);
}

static const uint32_t repeat_ms[3] = {0, 150, 450};

static void on_tick(uint64_t now, void *user) {
    (void)user;
    static struct { uint8_t kind, len; uint8_t buf[COOP_KILL_MSG_MAX]; } go[NO];
    int ng = 0;
    plat_mutex_lock(&K.lock);
    for (int i = 0; i < NO; i++) {
        Out *o = &K.out[i];
        if (!o->used || now < o->t0 + repeat_ms[o->copies]) continue;
        go[ng].kind = o->kind, go[ng].len = o->len;
        memcpy(go[ng].buf, o->buf, o->len);
        ng++;
        if (++o->copies >= 3) o->used = 0;
    }
    plat_mutex_unlock(&K.lock);
    static uint64_t said;
    for (int i = 0; i < ng; i++)
        if (!coop_send(go[i].kind, go[i].buf, go[i].len) && now - said >= 10000) {
            said = now;
            rt_log("coop: kill: a %s message could not be sent (not in a room, or the outbox is full)",
                   go[i].kind == COOP_K_KILL ? "KILL" : "DAMAGE");
        }
}

// ---- the present thread

static int is0(const char *v) { return v && !strcmp(v, "0"); }

static int num(const char **p, double *out) {   // digits with an optional fraction; no sign, no exponent, no hex
    const char *s = *p;
    double v = 0, f = 0.1;
    int any = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0'), any = 1;
    if (*s == '.')
        for (s++; *s >= '0' && *s <= '9'; s++) v += (*s - '0') * f, f /= 10, any = 1;
    if (!any) return 0;
    *p = s, *out = v;
    return 1;
}

static void curve(const char *key, const char *def, double out[4]) {
    const char *v = cfg_get(key);
    const char *s = v && *v ? v : def;
    double f[4];
    int n = 0;
    while (n < 4) {
        double x;
        if (!num(&s, &x) || x < 0.1 || x > 20) break;
        f[n++] = x;
        if (*s == ',') s++;
        else break;
    }
    if (n < 4 || *s) {
        if (v && *v) rt_log("coop: scale: %s='%s' is not four factors (1..4 players): '%s' instead", key, v, def);
        s = def, n = 0;
        while (n < 4 && num(&s, &f[n])) n++, s += *s == ',';
    }
    memcpy(out, f, sizeof f);
}

static void parse_tests(const char *v) {
    K.ntest = 0, K.loop = 0;
    char buf[512];
    snprintf(buf, sizeof buf, "%s", v ? v : "");
    for (char *next = buf, *tok; *next;) {
        while (*next == ';' || *next == ' ') next++;
        if (!*next) break;
        tok = next;
        while (*next && *next != ';' && *next != ' ') next++;
        if (*next) *next++ = 0;
        if (!strcmp(tok, "loop")) {
            K.loop = 1;
            continue;
        }
        char *colon = strchr(tok, ':');
        int kind = -1;
        if (colon) {
            *colon = 0;
            for (int k = 0; k < T_KINDS; k++)
                if (!strcmp(tok, test_names[k])) kind = k;
        }
        Test t = {.kind = kind, .count = 1, .raw = 1, .every_ms = 1000};
        const char *p = colon ? colon + 1 : "";
        double x;
        int ok = kind >= 0 && K.ntest < NT;
        if (ok && (kind == T_DOOM || kind == T_DMG)) {
            char *end;
            t.inst = strtoull(p, &end, 10);
            ok = end != p && t.inst;
            p = end;
        } else if (ok) {
            ok = num(&p, &x);
            if (kind == T_HURT) t.raw = (int)x;
            else t.radius = (float)x;
        }
        while (ok && *p) {
            char c = *p++;
            if (c == '#' && (kind == T_KILL || kind == T_HIT)) {
                char *end;
                t.fixed = strtoull(p, &end, 10);
                ok = end != p && t.fixed;
                p = end;
                continue;
            }
            ok = num(&p, &x);
            if (!ok) break;
            if (c == '@' || c == '~') t.at_ms = (uint64_t)(x * 1000), t.after_mate = c == '~';
            else if (c == '/') t.every_ms = (uint64_t)(x * 1000);
            else if (c == 'x') t.count = (int)x;
            else if (c == '=') t.raw = (int)x;
            else ok = 0;
        }
        if (!ok || t.raw < 1 || t.raw > 1000 || t.count < 1 || t.count > 100 || t.every_ms < 50) {
            rt_log("coop: test: '%s%s%s' is not a test (kill:<m>[#<inst>]@<s>, hit:<m>[#<inst>]@<s>/<every>x<n>=<raw>, hurt:<raw>@<s>, "
                   "doom:<inst>@<s>, dmg:<inst>@<s>, loop): left out", tok, colon ? ":" : "", colon ? colon + 1 : "");
            continue;
        }
        K.test[K.ntest++] = t;
    }
}

static void settings(void) {
    const char *k = cfg_get("NX2NAVI_COOP_KILLS"), *d = cfg_get("NX2NAVI_COOP_DAMAGE"), *s = cfg_get("NX2NAVI_COOP_SCALE");
    const char *b = cfg_get("NX2NAVI_COOP_DAMAGE_BOSS"), *fp = cfg_get("NX2NAVI_COOP_SCALE_PLAYERS");
    const char *dn = cfg_get("NX2NAVI_COOP_SHARE_DENY"), *t = cfg_get("NX2NAVI_COOP_KILL_TEST"), *lg = cfg_get("NX2NAVI_COOP_KILL_LOG");
    K.kills = !is0(k), K.damage = !is0(d), K.scale = !is0(s);
    K.boss_mode = !b || !*b || !strcmp(b, "safe") ? BOSS_SAFE : !strcmp(b, "lethal") ? BOSS_LETHAL : BOSS_OFF;
    K.forced = fp && *fp ? atoi(fp) : 0;
    if (K.forced < 0 || K.forced > 8) K.forced = 0;
    curve("NX2NAVI_COOP_SCALE_HP", "1,1.5,2,2.5", K.hp);
    curve("NX2NAVI_COOP_SCALE_TAKEN", "1,1.25,1.5,1.75", K.taken);
    plat_mutex_lock(&K.lock);
    snprintf(K.deny_extra, sizeof K.deny_extra, "%s", dn ? dn : "");
    plat_mutex_unlock(&K.lock);
    K.log_all = lg && !strcmp(lg, "1");
    K.census = lg && (!strcmp(lg, "1") || !strcmp(lg, "2"));
    parse_tests(t);
    K.factors_dirty = 1;
    plat_mutex_lock(&K.lock);
    work_update();
    plat_mutex_unlock(&K.lock);
    rt_log("coop: kill: kills %s (NX2NAVI_COOP_KILLS='%s'), damage %s (NX2NAVI_COOP_DAMAGE='%s'), bosses %s "
           "(NX2NAVI_COOP_DAMAGE_BOSS='%s'), scale %s (NX2NAVI_COOP_SCALE='%s'): health x%g,%g,%g,%g, damage to Link "
           "x%g,%g,%g,%g for 1..4 players%s%s; deny also '%s'; log every hit %d; %d test%s%s (NX2NAVI_COOP_KILL_TEST='%s')",
           K.kills ? "on" : "off", k ? k : "", K.damage ? "on" : "off", d ? d : "", boss_mode_names[K.boss_mode], b ? b : "",
           K.scale ? "on" : "off", s ? s : "", K.hp[0], K.hp[1], K.hp[2], K.hp[3], K.taken[0], K.taken[1], K.taken[2],
           K.taken[3], K.forced ? ", players forced to " : "", K.forced ? (fp ? fp : "") : "", K.deny_extra, K.log_all,
           K.ntest, K.ntest == 1 ? "" : "s", K.loop ? " and loop" : "", t ? t : "");
    for (int i = 0; i < K.ntest; i++) {
        Test *x = &K.test[i];
        char what[64];
        if (x->kind == T_DOOM || x->kind == T_DMG) snprintf(what, sizeof what, "inst %llu", (unsigned long long)x->inst);
        else if (x->kind == T_HURT) snprintf(what, sizeof what, "Link");
        else if (x->fixed) snprintf(what, sizeof what, "inst %llu", (unsigned long long)x->fixed);
        else snprintf(what, sizeof what, "within %.1f m of Link", x->radius);
        rt_log("coop: test: %s %s at %.1f s after %s, %d time%s every %.1f s, raw %d", test_names[x->kind], what,
               x->at_ms / 1000.0, x->after_mate ? "another member was first seen in the area" : "each area began", x->count,
               x->count == 1 ? "" : "s", x->every_ms / 1000.0, x->raw);
    }
}

static double factor(const double *f, int n) {
    if (n <= 1) return f[0];
    if (n <= 4) return f[n - 1];
    return f[3] + (n - 4) * (f[3] - f[2]);
}

// N: this player and the other members in the game world (their POS: trusted, heard within 3 s, IN_WORLD); without
// POS (puppets off) the members the roster lists, accepted and not away. Up at once, down after 10 s.
static void players(uint64_t now) {
    int others = atomic_load(&K.others), n;
    const char *how;
    if (K.forced > 0) {
        n = K.forced, how = "forced by NX2NAVI_COOP_SCALE_PLAYERS";
    } else if (!others) {
        n = 1, how = "no one else in a room";
    } else {
        int w = coop_puppet_world_others();
        if (w >= 0) {
            n = 1 + w, how = "this player and the members in the game world";
        } else {
            uint32_t ids = atomic_load(&K.present_ids) & coop_game_shown();
            n = 1 + __builtin_popcount(ids), how = "the room's accepted members (no POS heard)";
        }
    }
    if (n > COOP_MAX_MEMBERS) n = COOP_MAX_MEMBERS;
    char area[COOP_KILL_AREA_MAX + 1];
    plat_mutex_lock(&K.lock);
    snprintf(area, sizeof area, "%s", K.area);
    plat_mutex_unlock(&K.lock);
    int mates = others ? coop_puppet_area_others(area) : 0;
    if (mates && !atomic_load(&K.mate_t0)) {
        atomic_store(&K.mate_t0, now);
        rt_log("coop: kill: %d other member%s in %s now (their POS)", mates, mates == 1 ? "" : "s", area);
    } else if (!mates && atomic_load(&K.mate_t0)) {
        atomic_store(&K.mate_t0, 0);
        rt_log("coop: kill: no other member in %s now", area);
    }
    int prev = K.players;
    if (n >= K.players) K.players = n, K.lower_at = 0;
    else if (!K.lower_at) K.lower_at = now;
    else if (now - K.lower_at >= 10000) K.players = n, K.lower_at = 0;
    if (K.players != prev || K.factors_dirty) {
        double fh = K.scale ? factor(K.hp, K.players) : 1.0, ft = K.scale ? factor(K.taken, K.players) : 1.0;
        atomic_store(&K.mul_enemy, (uint32_t)lround(65536.0 / fh));
        atomic_store(&K.mul_link, (uint32_t)lround(65536.0 * ft));
        K.factors_dirty = 0;
        rt_log("coop: scale: %d player%s (%s): enemy health x%.2f, damage to Link x%.2f%s", K.players,
               K.players == 1 ? "" : "s", how, fh, ft, K.scale ? "" : " (scaling off: NX2NAVI_COOP_SCALE=0)");
    }
}

static void housekeeping(uint64_t now) {
    Lines L = {.n = 0};
    int expired = 0, went = 0;
    plat_mutex_lock(&K.lock);
    for (int i = 0; i < NE; i++) {
        Entry *e = &K.e[i];
        if (!atomic_load_explicit(&e->inst, memory_order_relaxed) || now - e->t_ms < 60000) continue;
        if (!e->applied_comp)
            say(&L, "coop: kill: inst %llu (%s from member %d) never updated here in 60 s: dropped", (unsigned long long)e->inst,
                e->doom ? "a kill" : "hits", e->from);
        entry_free(e);
        expired++;
    }
    for (int i = 0; i < NP; i++) {   // a death whose actor went away before its next update: a death all the same
        Pend *p = &K.pend[i];
        if (!atomic_load_explicit(&p->comp, memory_order_relaxed) || now - p->t_ms < 600) continue;
        if (p->area_gen == K.area_gen) kill_out(p->inst, p->hash, p->name, p->life, p->dmg, "its actor did not update again", now, &L);
        pend_free(p);
        went++;
    }
    uint64_t t_area = now - atomic_load(&K.area_t0);
    for (int i = 0; i < K.ntest; i++) {   // as if another player killed or hit it
        Test *t = &K.test[i];
        if (t->kind != T_DOOM && t->kind != T_DMG) continue;
        if (t->area_gen != K.area_gen) t->area_gen = K.area_gen, t->done = 0;
        if (!K.area[0] || t->done >= t->count || t_area < t->at_ms + (uint64_t)t->done * t->every_ms) continue;
        Entry *e = entry_add(t->inst, 0, now);
        t->done++;
        if (!e) continue;
        e->from = 0;
        if (t->kind == T_DOOM) e->doom = 1, e->doom_ms = now;
        else e->pending += t->raw;
        say(&L, "coop: test: %s %d of %d: inst %llu, as if another player %s it", test_names[t->kind], t->done, t->count,
            (unsigned long long)t->inst, t->kind == T_DOOM ? "killed" : "hit");
    }
    if (K.nseen && now - K.census_at >= 5000) {
        char line[1000];
        int o = snprintf(line, sizeof line, "coop: kill: census: life updates in the last %.1f s:",
                         K.census_at ? (now - K.census_at) / 1000.0 : 0.0);
        for (int i = 0; i < K.nseen && o < (int)sizeof line - 60; i++)
            if (K.seen[i].count)
                o += snprintf(line + o, sizeof line - o, " %s ..%06llu %.0fm %u;", K.seen[i].name,
                              (unsigned long long)(K.seen[i].inst % 1000000), K.seen[i].d, K.seen[i].count);
        for (int i = 0; i < K.nseen; i++) K.seen[i].count = 0;
        if (K.census_at) say(&L, "%s", line);
        K.census_at = now;
    }
    uint32_t sum = K.hits + K.link_hits + K.kills_sent + K.dmg_sent + K.kills_got + K.dmg_got;
    int stats = sum != K.stats_seen && now - K.stats_at >= 30000;
    if (stats) {
        K.stats_seen = sum, K.stats_at = now;
        say(&L, "coop: kill: stats: hits on enemies %u, on Link %u; KILL sent %u, got %u; DAMAGE sent %u, got %u; "
            "applied %u, died %u; waiting %d, dropped out %u", K.hits, K.link_hits, K.kills_sent, K.kills_got, K.dmg_sent,
            K.dmg_got, K.applied, K.died, K.ne, K.dropped_out);
    }
    int cut, n = budget(&L, now, &cut);
    plat_mutex_unlock(&K.lock);
    (void)expired, (void)went;
    flush(&L, n, cut);
}

int coop_kill_present(void) {
    if (K.set < 0) return 0;
    unsigned v = cfg_version();
    if (v != K.cfg_seen) {
        K.cfg_seen = v;
        settings();
    }
    uint64_t now = plat_now_ms();
    if (now - K.count_at >= 250) {
        K.count_at = now;
        players(now);
        housekeeping(now);
    }
    int others = atomic_load(&K.others);
    int sync = others > 0 && (K.kills || K.damage);
    int want = sync || (K.scale && K.players > 1) || K.ntest > 0;
    if (want != atomic_load(&K.on)) {
        atomic_store(&K.on, want);
        plat_mutex_lock(&K.lock);
        if (!want) clear_tables();
        work_update();
        plat_mutex_unlock(&K.lock);
        rt_patch_switch(K.set, want);
        rt_log("coop: kill: the life hook %s (%d other member%s in the room, %d player%s counted, %d test%s; present "
               "thread %u)", want ? "on" : "off", others, others == 1 ? "" : "s", K.players, K.players == 1 ? "" : "s",
               K.ntest, K.ntest == 1 ? "" : "s", plat_thread_id());
    }
    return want;
}

void coop_kill_frame(Cpu *c, uint64_t player, const char *area) {
    (void)c;
    uint64_t comp = link_comp_of(player);
    atomic_store(&K.link_comp, comp);
    char old[COOP_KILL_AREA_MAX + 1];
    int changed = 0, ne = 0, np = 0;
    uint64_t now = plat_now_ms();
    plat_mutex_lock(&K.lock);
    if (player) memcpy(K.link_pos, (const void *)(uintptr_t)(player + A_POS), sizeof K.link_pos);
    if (strncmp(area, K.area, COOP_KILL_AREA_MAX)) {
        snprintf(old, sizeof old, "%s", K.area);
        ne = K.ne, np = K.np;
        clear_tables();
        snprintf(K.area, sizeof K.area, "%s", area);
        K.area_gen++;
        atomic_store(&K.area_t0, now);
        changed = 1;
    }
    plat_mutex_unlock(&K.lock);
    if (changed)
        rt_log("coop: kill: area '%s' (was '%s'%s); Link's life component %#llx", area, old,
               ne || np ? ", its waiting kills and hits dropped" : "", (unsigned long long)comp);
}

void coop_kill_init(void) {
    if (K.set < 0) return;
    plat_mutex_init(&K.lock);
    uint32_t e = 0;
    if (!plat_random(&e, sizeof e) || !e) e = (uint32_t)plat_now_ns() | 1;
    K.epoch = e;
    atomic_store(&K.mul_enemy, 65536);
    atomic_store(&K.mul_link, 65536);
    coop_on_message(COOP_K_KILL, on_msg, (void *)(uintptr_t)COOP_K_KILL);
    coop_on_message(COOP_K_DAMAGE, on_msg, (void *)(uintptr_t)COOP_K_DAMAGE);
    coop_on_view(on_view, NULL);
    coop_on_tick(on_tick, NULL);
}

__attribute__((constructor)) static void kill_register(void) {
    K.set = rt_patch_set("coop_life");
    K.cfg_seen = ~0u;
    if (K.set >= 0) rt_register_host(HOST_LIFE, life_host);
}

#else
void coop_kill_init(void) {}
int coop_kill_present(void) { return 0; }
void coop_kill_frame(Cpu *c, uint64_t player, const char *area) {
    (void)c, (void)player, (void)area;
}
#endif
