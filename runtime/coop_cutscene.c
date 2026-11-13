// Co-op cutscenes watched together, and the event table behind per-player dialog, game 1.1.1.
//
// The game plays one "main event" at a time: the event manager **[main+0x4c7e090] holds it at +0x68 from its
// preparation to its end, and the event's name (a row of RSDB/EventSetting) is the char * at its +0x8. The frame hook
// (coop_puppet.c puppet_frame, the game's update thread) reads both every frame.
//
// Leader. When this game's main event becomes a story cutscene (a row "Dm_*" of the player's own event table, but not
// Dm_SY_* or Dm_Title_*, whose component list is not an NPC one: 309 of 2380 rows), it sends START (event, area,
// Link's place, age, movie) to the room, and END when the event is over. Talks are never sent: dialog stays per player.
//
// Follower. A START from an accepted member (and of a story cutscene of this player's own table, in the same age)
// waits until this game is free (in the world, no event or request of its own, no menu: progress sync's gate, the
// debug tools idle; 45 s at most), then holds progress sync (the room's values wait, so the leader's flags cannot change
// this world before or while its own copy plays), and:
//   - in the leader's area, or a pre-rendered movie: starts the event by its name (the debug tools' start, tried every
//     0.5 s for 20 s while the game refuses it);
//   - elsewhere: the debug tools' teleport to the leader's area, at its start position nearest the leader, with the
//     event reserved as the one the game plays at the arrival (Sequence_ReservedBootEventName); when it does not start
//     there by itself within 3 s, it is started by its name as above.
// When this game's event is over (1 s with none), the hold ends (the room's values come in: the same ones its own copy
// set) and the player stays with the leader (NX2NAVI_COOP_CUTSCENE_RETURN=back: teleported back to the start position
// nearest where he was). A game that leads never follows and a game that follows never leads. Boss scenes
// (*_BossIn/*_BossOut) are followed only by players already in the leader's area (NX2NAVI_COOP_CUTSCENE_BOSSES).
// While a story cutscene plays here, the other players' puppets are taken out of it (NX2NAVI_COOP_CUTSCENE_HIDE).
//
// Dialog per player (coop_game.c): the HavePlayedEvent flags of the NPC talk events (rows with an NPC* component
// list), read here from the player's own event table, and the talk markers stay out of progress sync.
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
//   NX2NAVI_COOP_CUTSCENE_TEST        tests, ';'-separated:
//       start:<event>@<s>    this game starts the event by its name <s> seconds after Link first stood in the world
//       start:<event>~<s>    ... <s> seconds after another member was first seen in the game world (their POS)
//       follow:<event>:<area>:<x>,<y>,<z>@<s>   a START as if from another member (the follower alone)
//       settime:<hh>:<mm>@<s> (or ~<s>)        the game's own time setter (its jump goes to the room)
//       loop                 every message sent is parsed back and logged
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#include "cfg.h"
#include "coop.h"
#include "coop_cutscene.h"
#include "coop_game.h"
#include "coop_offer.h"
#include "coop_puppet.h"
#include "debug_tools.h"
#include "fs.h"
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
#define H_MOVIE 0x5044cc65u                    // murmur3("Movie"): EventFileType of a pre-rendered movie

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

static uint32_t u24(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16; }
static uint32_t u32le(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

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

// ---- the player's own event table (RSDB/EventSetting), read once on a thread of its own

typedef struct {
    char *name;
    uint8_t movie, boss;
} Story;
static Story *stories;        // story cutscenes, sorted by name
static int nstories;
static uint32_t *talk_flags;  // HavePlayedEvent.<event> of the NPC talk events, sorted
static int ntalk;
static volatile int32_t ev_state;   // 0 not asked, 1 being read, 2 read

static int sarc_member(const uint8_t *p, size_t n, const char *name, size_t *at, size_t *size) {
    if (n < 0x20 || memcmp(p, "SARC", 4)) return 0;
    uint16_t head, count;
    uint32_t data;
    memcpy(&head, p + 4, 2);
    memcpy(&data, p + 12, 4);
    if ((size_t)head + 12 > n || memcmp(p + head, "SFAT", 4)) return 0;
    memcpy(&count, p + head + 6, 2);
    size_t names = (size_t)head + 12 + (size_t)count * 16 + 8, len = strlen(name);
    for (unsigned i = 0; i < count; i++) {
        uint32_t attr, lo, hi;
        size_t e = (size_t)head + 12 + (size_t)i * 16;
        if (e + 16 > n) return 0;
        memcpy(&attr, p + e + 4, 4), memcpy(&lo, p + e + 8, 4), memcpy(&hi, p + e + 12, 4);
        size_t s = names + (size_t)(attr & 0xffffff) * 4;
        if (s + len + 1 > n || memcmp(p + s, name, len + 1)) continue;
        if (lo > hi || (size_t)data + hi > n) return 0;
        *at = data + lo, *size = hi - lo;
        return 1;
    }
    return 0;
}

// The i-th text of a BYML text table (node 0xc2: its count, then the texts' offsets from the node).
static const char *byml_text(const uint8_t *p, size_t n, uint32_t table, uint32_t i) {
    if (!table || (size_t)table + 4 > n || p[table] != 0xc2 || i >= u24(p + table + 1) || (size_t)table + 8 + 4 * (size_t)i > n)
        return NULL;
    uint32_t off = u32le(p + table + 4 + 4 * (size_t)i);
    if ((size_t)table + off >= n || !memchr(p + table + off, 0, n - table - off)) return NULL;
    return (const char *)p + table + off;
}

static int key_index(const uint8_t *p, size_t n, uint32_t keys, const char *want) {
    uint32_t count = keys + 4 <= n && p[keys] == 0xc2 ? u24(p + keys + 1) : 0;
    for (uint32_t i = 0; i < count; i++) {
        const char *k = byml_text(p, n, keys, i);
        if (k && !strcmp(k, want)) return (int)i;
    }
    return -1;
}

static int by_story_name(const void *a, const void *b) { return strcmp(((const Story *)a)->name, ((const Story *)b)->name); }
static int by_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

// The rows: a BYML array (node 0xc0) of dictionaries (0xc1: entries of a key's number in 24 bits, the value's kind, the
// value), texts (0xa0) by their number in the text table.
static void events_parse(const uint8_t *p, size_t n, int *rows_out, int *movies_out) {
    if (n < 16 || memcmp(p, "YB", 2)) return;
    uint32_t keys = u32le(p + 4), strs = u32le(p + 8), root = u32le(p + 12);
    if ((size_t)root + 4 > n || p[root] != 0xc0) return;
    int k_row = key_index(p, n, keys, "__RowId"), k_list = key_index(p, n, keys, "EventSettingComponentList"),
        k_type = key_index(p, n, keys, "EventFileType");
    if (k_row < 0 || k_list < 0) return;
    uint32_t count = u24(p + root + 1), vals = root + 4 + ((count + 3) & ~3u);
    if ((size_t)vals + 4ull * count > n) return;
    stories = calloc(count, sizeof *stories);
    talk_flags = calloc(count, sizeof *talk_flags);
    if (!stories || !talk_flags) return;
    int movies = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (p[root + 4 + i] != 0xc1) continue;
        uint32_t d = u32le(p + vals + 4 * i);
        if ((size_t)d + 4 > n || p[d] != 0xc1) continue;
        uint32_t m = u24(p + d + 1);
        const char *row = NULL, *list = "";
        uint32_t type = 0;
        for (uint32_t k = 0; k < m && (size_t)d + 12 + 8 * (size_t)k <= n; k++) {
            const uint8_t *e = p + d + 4 + 8 * (size_t)k;
            int key = (int)u24(e);
            uint32_t v = u32le(e + 4);
            if (e[3] == 0xa0 && key == k_row) row = byml_text(p, n, strs, v);
            else if (e[3] == 0xa0 && key == k_list) list = byml_text(p, n, strs, v);
            else if ((e[3] == 0xd3 || e[3] == 0xd1) && key == k_type) type = v;
            if (!list) list = "";
        }
        if (!row || !row[0] || strlen(row) > COOP_CUT_EVENT_MAX) continue;
        (*rows_out)++;
        const char *base = strrchr(list, '/');
        base = base ? base + 1 : list;
        if (!strncmp(base, "NPC", 3)) {   // an NPC talk, "near" line, chase or each-frame line
            char flag[128];
            snprintf(flag, sizeof flag, "HavePlayedEvent.%s", row);
            talk_flags[ntalk++] = hash_of(flag);
            continue;
        }
        if (strncmp(row, "Dm_", 3) || !strncmp(row, "Dm_SY_", 6) || !strncmp(row, "Dm_Title_", 9)) continue;
        char *copy = malloc(strlen(row) + 1);
        if (!copy) continue;
        strcpy(copy, row);
        Story *s = &stories[nstories++];
        s->name = copy, s->movie = type == H_MOVIE, s->boss = strstr(row, "_BossIn") || strstr(row, "_BossOut");
        movies += s->movie;
    }
    *movies_out = movies;
    qsort(stories, (size_t)nstories, sizeof *stories, by_story_name);
    qsort(talk_flags, (size_t)ntalk, sizeof *talk_flags, by_u32);
}

static void events_thread(void *arg) {
    (void)arg;
    uint64_t t0 = plat_now_ns(), dn = 0, zn = 0;
    const char *path = NULL;
    for (uint64_t i = 0, count = romfs_file_count(); i < count; i++) {   // RSDB/EventSetting.Product.<n>.rstbl.byml.zs
        uint64_t off, size;
        const char *pth = romfs_entry(i, &off, &size);
        size_t len = pth ? strlen(pth) : 0;
        if (pth && !strncmp(pth, "RSDB/EventSetting.Product.", 26) && len > 14 && !strcmp(pth + len - 14, ".rstbl.byml.zs") &&
            (!path || strcmp(pth, path) > 0))
            path = pth;
    }
    uint8_t *dz = romfs_load("Pack/ZsDic.pack.zs", &dn), *dics = NULL, *z = path ? romfs_load(path, &zn) : NULL, *raw = NULL;
    ZSTD_DDict *dd = NULL;
    ZSTD_DCtx *dctx = ZSTD_createDCtx();
    unsigned long long dsize = dz ? ZSTD_getFrameContentSize(dz, (size_t)dn) : 0, rn = z ? ZSTD_getFrameContentSize(z, (size_t)zn) : 0;
    size_t at = 0, size = 0;
    if (dz && dsize && dsize < (64u << 20) && (dics = malloc((size_t)dsize)) &&
        ZSTD_decompress(dics, (size_t)dsize, dz, (size_t)dn) == dsize && sarc_member(dics, (size_t)dsize, "zs.zsdic", &at, &size))
        dd = ZSTD_createDDict(dics + at, size);
    int ok = 0, rows = 0, movies = 0;
    if (z && rn && rn < (64u << 20) && (raw = malloc((size_t)rn))) {
        ok = dd && dctx && ZSTD_decompress_usingDDict(dctx, raw, (size_t)rn, z, (size_t)zn, dd) == rn;
        if (!ok) ok = ZSTD_decompress(raw, (size_t)rn, z, (size_t)zn) == rn;
        if (ok) events_parse(raw, (size_t)rn, &rows, &movies);
    }
    ZSTD_freeDCtx(dctx);
    ZSTD_freeDDict(dd);
    free(dics), free(dz), free(z), free(raw);
    if (nstories && ntalk)
        rt_log("coop: cutscene: the event table %s: %d events, %d story cutscenes (%d movies), %d NPC talk events, in %.0f ms",
               path, rows, nstories, movies, ntalk, (double)(plat_now_ns() - t0) / 1e6);
    else
        rt_log("coop: cutscene: the event table could not be read (%s%s): no cutscene is shared and talk events stay shared",
               path ? path : "no RSDB/EventSetting file", ok ? ", no rows" : "");
    __atomic_store_n(&ev_state, 2, __ATOMIC_RELEASE);
}

int coop_events_state(void) {
    int s = __atomic_load_n(&ev_state, __ATOMIC_ACQUIRE);
    if (!s) {
        int32_t zero = 0;
        if (__atomic_compare_exchange_n(&ev_state, &zero, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            if (!plat_thread_spawn(events_thread, NULL, "coop events", 0)) {
                rt_log("coop: cutscene: no thread for the event table");
                __atomic_store_n(&ev_state, 2, __ATOMIC_RELEASE);
            }
        }
        s = __atomic_load_n(&ev_state, __ATOMIC_ACQUIRE);
    }
    return s;
}

int coop_events_talk_flags(const uint32_t **out) {
    *out = talk_flags;
    return coop_events_state() == 2 ? ntalk : 0;
}

static const Story *story_of(const char *name) {
    if (coop_events_state() != 2 || !name || !name[0]) return NULL;
    Story key = {(char *)name, 0, 0};
    return bsearch(&key, stories, (size_t)nstories, sizeof *stories, by_story_name);
}

// ---- the wire format

static int printable(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)s[i] <= 0x20 || (unsigned char)s[i] >= 0x7f) return 0;
    return 1;
}

static void put32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> 8 * i);
}

size_t coop_cut_pack(const CoopCutMsg *m, uint8_t *out, size_t cap) {
    size_t el = strnlen(m->event, COOP_CUT_EVENT_MAX + 1), al = strnlen(m->area, COOP_CUT_AREA_MAX + 1);
    if ((m->op != COOP_CUT_START && m->op != COOP_CUT_END) || el < 1 || el > COOP_CUT_EVENT_MAX || al > COOP_CUT_AREA_MAX ||
        !printable(m->event, el) || !printable(m->area, al) || !isfinite(m->xyz[0]) || !isfinite(m->xyz[1]) || !isfinite(m->xyz[2]))
        return 0;
    size_t n = 11 + 1 + el + 1 + al + 12;
    if (n > cap) return 0;
    uint8_t *p = out;
    *p++ = COOP_CUT_VERSION, *p++ = m->op;
    put32(p, m->epoch), p += 4;
    put32(p, m->seq), p += 4;
    *p++ = m->flags;
    *p++ = (uint8_t)el, memcpy(p, m->event, el), p += el;
    *p++ = (uint8_t)al, memcpy(p, m->area, al), p += al;
    for (int i = 0; i < 3; i++) {
        uint32_t bits;
        memcpy(&bits, &m->xyz[i], 4);
        put32(p, bits), p += 4;
    }
    return (size_t)(p - out);
}

int coop_cut_parse(const uint8_t *b, size_t n, CoopCutMsg *m) {
    memset(m, 0, sizeof *m);
    if (n < 11 + 2 + 1 + 12 || b[0] != COOP_CUT_VERSION || (b[1] != COOP_CUT_START && b[1] != COOP_CUT_END)) return 0;
    m->op = b[1], m->epoch = u32le(b + 2), m->seq = u32le(b + 6), m->flags = b[10];
    size_t el = b[11];
    if (el < 1 || el > COOP_CUT_EVENT_MAX || 12 + el + 1 > n) return 0;
    size_t al = b[12 + el];
    if (al > COOP_CUT_AREA_MAX || 13 + el + al + 12 != n) return 0;
    if (!printable((const char *)b + 12, el) || !printable((const char *)b + 13 + el, al)) return 0;
    memcpy(m->event, b + 12, el), m->event[el] = 0;
    memcpy(m->area, b + 13 + el, al), m->area[al] = 0;
    for (int i = 0; i < 3; i++) {
        uint32_t bits = u32le(b + 13 + el + al + 4 * (size_t)i);
        memcpy(&m->xyz[i], &bits, 4);
        if (!isfinite(m->xyz[i]) || fabsf(m->xyz[i]) > 1e6f) return 0;
    }
    return 1;
}

// ---- state

enum { F_IDLE, F_WAIT, F_WARP, F_START, F_PLAY, F_RETURN };
static const char *const f_names[] = {"idle", "waiting until free", "teleporting", "starting", "playing", "going back"};
enum { BOSS_NEVER, BOSS_AREA, BOSS_ALL };
enum { NOUT = 8, NTEST = 8 };

typedef struct {
    int used, copies;
    uint64_t t0;
    uint8_t kind, len;
    uint8_t buf[COOP_CUT_MSG_MAX];
} Out;

typedef struct {
    int kind;   // 0 start, 1 follow
    int after_other;
    double at;
    int done;
    char event[COOP_CUT_EVENT_MAX + 1], area[COOP_CUT_AREA_MAX + 1];
    float xyz[3];
} Test;

static struct {
    PlatMutex lock;              // the inbox, the windows and the outbox
    unsigned cfg_seen;
    int on, boot, back, bosses, hide, trace, loop;
    int active;                  // the frame hook does this module's work (present thread; read by the game thread)
    Test test[NTEST];
    int ntest;
    uint32_t epoch, seq;
    _Atomic int others, self;
    _Atomic uint32_t present;    // the room's members not away, this player included (a bit per id)
    // time of day: the switch, the inbox (co-op thread -> game thread)
    int time_on;
    int t_have, t_from, t_jump;
    uint32_t t_lamport, t_tod;
    uint64_t t_at;
    struct { uint32_t epoch, seq; int any; } t_win[COOP_MAX_MEMBERS + 1];
    // the inbox (co-op thread -> game thread)
    CoopCutMsg in;
    int in_have, in_from, in_fake;
    uint64_t in_at;
    CoopCutMsg end;
    int end_have, end_from;
    struct { uint32_t epoch, seq; int any; } win[COOP_MAX_MEMBERS + 1];
    Out out[NOUT];
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
    int f_tries, f_hold;
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
    uint32_t tl;                 // the newest time order this game knows (a Lamport clock: a jump makes one more)
    uint32_t t_last, t_seq;
    int t_have_last, t_pend, t_pend_jump, t_pend_from, t_keeper;
    uint32_t t_target, t_expect;
    uint64_t t_beat_at, t_pend_at, t_expect_until, t_moved_at;
} C = {.lock = PLAT_MUTEX_INIT};

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void on_time(int from, const uint8_t *b, size_t n, void *user);

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

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    char line[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    rt_log("coop: cutscene: %s", line);
}

// ---- the co-op thread

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
    plat_mutex_lock(&C.lock);
    int slot = -1;
    for (int i = 0; i < NOUT && slot < 0; i++)
        if (!C.out[i].used) slot = i;
    if (slot < 0) slot = 0;   // the oldest repeats give way
    Out *o = &C.out[slot];
    o->used = 1, o->copies = 0, o->t0 = plat_now_ms(), o->kind = COOP_K_CUTSCENE, o->len = (uint8_t)n;
    memcpy(o->buf, buf, n);
    plat_mutex_unlock(&C.lock);
}

static void on_msg(int from, const uint8_t *bytes, size_t n, void *user) {
    (void)user;
    CoopCutMsg m;
    if (!coop_cut_parse(bytes, n, &m)) {
        static uint64_t said;
        uint64_t now = plat_now_ms();
        if (now - said >= 5000) said = now, say("a CUTSCENE message of member %d does not parse (%zu bytes)", from, n);
        return;
    }
    if (from < 1 || from > COOP_MAX_MEMBERS || from == atomic_load(&C.self)) return;
    int trusted = coop_game_shown() >> from & 1;
    plat_mutex_lock(&C.lock);
    if (C.win[from].any && C.win[from].epoch == m.epoch && m.seq <= C.win[from].seq) {   // a copy, or an old one
        plat_mutex_unlock(&C.lock);
        return;
    }
    C.win[from].any = 1, C.win[from].epoch = m.epoch, C.win[from].seq = m.seq;
    if (trusted && m.op == COOP_CUT_START) C.in = m, C.in_have = 1, C.in_from = from, C.in_fake = 0, C.in_at = plat_now_ms();
    if (trusted && m.op == COOP_CUT_END) C.end = m, C.end_have = 1, C.end_from = from;
    plat_mutex_unlock(&C.lock);
    say("member %d %s '%s' in '%s' at %.1f %.1f %.1f (%s%s)%s", from, m.op == COOP_CUT_START ? "starts" : "ends", m.event,
        m.area, m.xyz[0], m.xyz[1], m.xyz[2], m.flags & COOP_CUT_ADULT ? "adult" : "child", m.flags & COOP_CUT_MOVIE ? ", a movie" : "",
        trusted ? "" : ": dropped, the member is not accepted or not identified");
}

static void on_view(const CoopView *v, void *user) {
    (void)user;
    atomic_store(&C.self, v->self);
    atomic_store(&C.others, v->state == COOP_JOINED && v->n > 1 ? v->n - 1 : 0);
    uint32_t bits = 0;
    for (int i = 0; v->state == COOP_JOINED && i < v->n; i++)
        if (v->member[i].id >= 1 && v->member[i].id <= COOP_MAX_MEMBERS && (!v->member[i].away || v->member[i].id == v->self))
            bits |= 1u << v->member[i].id;
    atomic_store(&C.present, bits);
}

static const uint32_t repeat_ms[3] = {0, 150, 450};

static void on_tick(uint64_t now, void *user) {
    (void)user;
    static struct { uint8_t kind, len; uint8_t buf[COOP_CUT_MSG_MAX]; } go[NOUT];
    int ng = 0;
    plat_mutex_lock(&C.lock);
    for (int i = 0; i < NOUT; i++) {
        Out *o = &C.out[i];
        if (!o->used || now < o->t0 + repeat_ms[o->copies]) continue;
        go[ng].kind = o->kind, go[ng].len = o->len;
        memcpy(go[ng].buf, o->buf, o->len);
        ng++;
        if (++o->copies >= 3) o->used = 0;
    }
    plat_mutex_unlock(&C.lock);
    static uint64_t said;
    for (int i = 0; i < ng; i++)
        if (!coop_send(go[i].kind, go[i].buf, go[i].len) && now - said >= 10000)
            said = now, say("a %s message could not be sent (not in a room, or the outbox is full)",
                            go[i].kind == COOP_K_TIME ? "TIME" : "CUTSCENE");
}

void coop_cutscene_init(void) {
    uint64_t r = plat_now_ns();
    C.epoch = (uint32_t)(r ^ r >> 32 ^ (uint64_t)(uintptr_t)&C) | 1u;
    coop_on_message(COOP_K_CUTSCENE, on_msg, NULL);
    coop_on_message(COOP_K_TIME, on_time, NULL);
    coop_on_view(on_view, NULL);
    coop_on_tick(on_tick, NULL);
}

// ---- the present thread: settings

static int is0(const char *v) { return v && !strcmp(v, "0"); }

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
        } else {
            say("test '%s': not start:<event> or follow:<event>:<area>:<x>,<y>,<z>: left out", tok);
            continue;
        }
        C.test[C.ntest++] = t;
        say("test %d: %s '%s'%s%s %s %.1f s", C.ntest, t.kind == 2 ? "set the time of day to" : t.kind ? "a START as if from another member for" : "start", t.event,
            t.kind ? " in " : "", t.kind ? t.area : "", t.after_other ? "after another member is in the world, plus" : "at", t.at);
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
        if (cfg_bool("NX2NAVI_COOP"))
            say("time: the time of day %s (NX2NAVI_COOP_TIME='%s')", C.time_on ? "is shared" : "stays each player's", tm ? tm : "");
        C.bosses = is0(boss) ? BOSS_NEVER : boss && !strcmp(boss, "all") ? BOSS_ALL : BOSS_AREA;
        C.trace = trace && *trace && !is0(trace);
        const char *tp = cfg_get("NX2NAVI_COOP_TIME_PROBE");   // a probe: the world time object's words
        C.time_probe = tp && *tp && !is0(tp) ? atof(tp) : 0;
        if (C.time_probe > 0 && C.time_probe < 1) C.time_probe = 1;
        parse_tests(test);
        if (cfg_bool("NX2NAVI_COOP") || C.trace || C.ntest || C.loop)
            say("cutscenes %s (NX2NAVI_COOP_CUTSCENES='%s'); arrival event %s, after a scene %s, boss scenes %s, puppets in "
                "scenes %s, trace %s, %d tests%s", C.on ? "together" : "off", on ? on : "", C.boot ? "on" : "off (start by name)",
                C.back ? "back" : "stay", C.bosses == BOSS_NEVER ? "never" : C.bosses == BOSS_ALL ? "always" : "only in the same area",
                C.hide ? "hidden" : "shown", C.trace ? "on" : "off", C.ntest, C.loop ? ", loop" : "");
    }
    int want = RT_V111 && dbg_supported() && (C.trace || C.ntest || C.time_probe > 0 || ((C.on || C.time_on) && atomic_load(&C.others) > 0));
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

// ---- time of day, shared (NX2NAVI_COOP_TIME=0 off). The world's time part *(wm+0x510) keeps the time
// of day as seconds at +0x28 (u32 0..86399, about 120 a second while the world runs). The game's own setter
// (the event flows' ExecuteWmSetTime: main+0x34522d0(wm, seconds)) moves it, sky and all, as a scene or the Sun's
// Song does. The room's time keeper is the member with the lowest id in the room: every 2 s it sends TIME (its time
// and the newest time order it knows). A game whose own clock jumps (the Sun's Song, a scene, a bed: more than 10 game
// minutes in a frame, not made by this module) makes a newer order and sends it at once (three times); every game,
// the keeper included, takes the newest order's time. A game sets its time only when free (in the world, no event or
// request, the debug tools idle, not following a scene), and for the keeper's beat only when it is more than 5 game
// minutes off.
enum { TIME_VERSION = 1, TIME_MSG = 18, DAY = 86400 };
enum { TIME_JUMP = 1 };
#define TOD_OFF 0x28

static size_t time_pack(uint8_t *out, uint32_t epoch, uint32_t seq, uint32_t lamport, uint32_t tod, uint8_t flags) {
    out[0] = TIME_VERSION;
    put32(out + 1, epoch), put32(out + 5, seq), put32(out + 9, lamport), put32(out + 13, tod);
    out[17] = flags;
    return TIME_MSG;
}

static uint32_t tod_dist(uint32_t a, uint32_t b) {   // game seconds between two times of day, either way round
    uint32_t d = (a + DAY - b) % DAY;
    return d < DAY - d ? d : DAY - d;
}

static const char *hhmm(uint32_t tod, char out[8]) {
    snprintf(out, 8, "%02u:%02u", (tod / 3600) % 24, (tod / 60) % 60);
    return out;
}

static void queue_time(uint32_t lamport, uint32_t tod, uint8_t flags, int copies) {
    plat_mutex_lock(&C.lock);
    int slot = -1;
    for (int i = 0; i < NOUT && slot < 0; i++)
        if (!C.out[i].used) slot = i;
    if (slot >= 0) {
        Out *o = &C.out[slot];
        o->used = 1, o->copies = 3 - copies, o->t0 = plat_now_ms() - repeat_ms[o->copies], o->kind = COOP_K_TIME;
        o->len = (uint8_t)time_pack(o->buf, C.epoch, ++C.t_seq, lamport, tod, flags);
    }
    plat_mutex_unlock(&C.lock);
}

static void on_time(int from, const uint8_t *b, size_t n, void *user) {
    (void)user;
    if (n != TIME_MSG || b[0] != TIME_VERSION || from < 1 || from > COOP_MAX_MEMBERS || from == atomic_load(&C.self)) return;
    uint32_t epoch = u32le(b + 1), seq = u32le(b + 5), lamport = u32le(b + 9), tod = u32le(b + 13);
    if (tod >= DAY || !(coop_game_shown() >> from & 1)) return;
    plat_mutex_lock(&C.lock);
    if (C.t_win[from].any && C.t_win[from].epoch == epoch && seq <= C.t_win[from].seq) {
        plat_mutex_unlock(&C.lock);
        return;
    }
    C.t_win[from].any = 1, C.t_win[from].epoch = epoch, C.t_win[from].seq = seq;
    // the newest order wins; within one order a jump, else the lowest member (the keeper's beat)
    if (!C.t_have || lamport > C.t_lamport || (lamport == C.t_lamport && ((b[17] & TIME_JUMP) || from <= C.t_from)))
        C.t_have = 1, C.t_from = from, C.t_lamport = lamport, C.t_tod = tod, C.t_jump = b[17] & TIME_JUMP, C.t_at = plat_now_ms();
    plat_mutex_unlock(&C.lock);
}

static int lowest_id(uint32_t bits) {
    for (int i = 1; i <= COOP_MAX_MEMBERS; i++)
        if (bits >> i & 1) return i;
    return 0;
}

// The game thread, every frame of a room with others: the beat, this game's own jumps, and the room's time.
static void time_frame(Cpu *c, const EvNow *e, uint64_t player, const char *area, uint64_t now) {
    uint64_t wm = world_mgr(), part = wm ? rd_ptr(wm + 0x510) : 0;
    uint32_t tod = 0;
    if (!part || !rd(part + TOD_OFF, &tod, 4) || tod >= DAY) return;
    int in_world = player && area[0];
    char a[8], b[8];
    int self = atomic_load(&C.self);
    uint32_t present = atomic_load(&C.present);
    int keeper = self && lowest_id(present) == self;
    if (keeper != C.t_keeper) {
        C.t_keeper = keeper;
        say("time: %s", keeper ? "this game keeps the room's time (the lowest member id): its time goes out every 2 s"
                               : "another member keeps the room's time");
    }
    // a jump of this game's own clock
    if (C.t_have_last && tod_dist(tod, C.t_last) > 600) {
        if (now < C.t_expect_until && tod_dist(tod, C.t_expect) < 300) {
            C.t_expect_until = 0;   // the room's time, set here
        } else if (in_world) {
            C.tl++;
            queue_time(C.tl, tod, TIME_JUMP, 3);
            say("time: this game's clock jumped from %s to %s: sent to the room (order %u)", hhmm(C.t_last, a), hhmm(tod, b), C.tl);
        }
    }
    if (!C.t_have_last || tod != C.t_last) C.t_moved_at = now;
    int running = now - C.t_moved_at < 1500;   // the clock stands in menus, scenes, loads (and where the game stops it)
    C.t_last = tod, C.t_have_last = 1;
    if (keeper && in_world && running && now >= C.t_beat_at) {
        C.t_beat_at = now + 2000;
        queue_time(C.tl, tod, 0, 1);
    }
    // what the room sent
    plat_mutex_lock(&C.lock);
    int have = C.t_have, from = C.t_from, jump = C.t_jump;
    uint32_t lam = C.t_lamport, ttod = C.t_tod;
    uint64_t at = C.t_at;
    C.t_have = 0;
    plat_mutex_unlock(&C.lock);
    if (have && (lam > C.tl || (lam == C.tl && !keeper && (jump || from == lowest_id(present))))) {
        if (lam > C.tl || jump) C.t_pend_jump = 1;
        if (lam > C.tl) C.tl = lam;
        C.t_pend = 1, C.t_target = ttod, C.t_pend_at = at, C.t_pend_from = from;
    }
    if (!C.t_pend || !in_world || e->ev || (e->flags & 0x4000) || dbg_busy() || C.f != F_IDLE) return;
    if (!C.t_pend_jump && !running) return;   // a beat only sets a clock that runs (it waits for the next beat)
    // the time it is there now: what was sent, plus the game seconds since (about 120 a second)
    uint32_t target = (C.t_target + (uint32_t)((now - C.t_pend_at) * 120 / 1000)) % DAY, off = tod_dist(tod, target);
    if (off > (C.t_pend_jump ? 60u : 300u)) {
        rt_call(c, WM_SET_TIME, 2, wm, (uint64_t)target);
        C.t_expect = target, C.t_expect_until = now + 3000;
        say("time: set to %s (member %d's%s, order %u; it was %s here, %u game seconds off)", hhmm(target, a), C.t_pend_from,
            C.t_pend_jump ? " jump" : " beat", C.tl, hhmm(tod, b), off);
    }
    C.t_pend = 0, C.t_pend_jump = 0;
}

static int played_flag(const char *event) {
    char f[128];
    snprintf(f, sizeof f, "HavePlayedEvent.%s", event);
    return flag_bool(hash_of(f));
}

static double secs(uint64_t from, uint64_t now) { return from ? (double)(now - from) / 1000.0 : -1.0; }

static void follow_end(const char *why, uint64_t now) {
    if (C.f_hold) coop_game_hold(0), C.f_hold = 0;
    say("follow of '%s' ends after %.1f s: %s", C.fm.event, secs(C.f_t0, now), why);
    C.f = F_IDLE;
}

static void leader(const EvNow *e, uint64_t player, const char *area, uint64_t now) {
    if (e->ev == C.ev && !strcmp(e->name, C.name)) return;
    if (C.ev) {   // the event that played has ended (or another took its place)
        if (C.trace || C.story)
            say("event '%s' ends after %.1f s%s (HavePlayedEvent %d at its start, %d now)", C.name, secs(C.ev_t0, now),
                C.story ? ", a story cutscene" : "", C.played_at_start, played_flag(C.name));
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
    const Story *s = story_of(e->name);
    C.story = s != NULL;
    C.played_at_start = played_flag(e->name);
    if (C.trace || s)
        say("event '%s' starts in '%s' (%s%s; HavePlayedEvent %d; manager flags %#x)", e->name, area,
            s ? "a story cutscene" : "not a story cutscene", s && s->movie ? ", a movie" : "", C.played_at_start, e->flags);
    if (!s) return;
    const char *why = !C.on ? "cutscenes are off here (NX2NAVI_COOP_CUTSCENES=0)"
                    : C.f != F_IDLE ? "this game plays it as a follower"
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

// A START taken from the inbox: followed or not.
static void consider(const CoopCutMsg *m, int from, int fake, uint64_t player, const char *area, uint64_t now) {
    const Story *s = story_of(m->event);
    int adult = player && link_adult(player), same = area[0] && !strcmp(area, m->area);
    const char *why = !C.on ? "cutscenes are off here (NX2NAVI_COOP_CUTSCENES=0)"
                    : !s ? "not a story cutscene of this game's event table"
                    : C.f != F_IDLE ? "this game already follows a scene"
                    : C.sent ? "this game leads a scene of its own"
                    : !player ? "Link is not in the world"
                    : !!(m->flags & COOP_CUT_ADULT) != adult ? "the other player is of another age"
                    : !m->area[0] && !(m->flags & COOP_CUT_MOVIE) ? "no area given"
                    : s->boss && C.bosses == BOSS_NEVER ? "boss scenes are not followed (NX2NAVI_COOP_CUTSCENE_BOSSES=0)"
                    : s->boss && C.bosses == BOSS_AREA && !same ? "a boss scene of another area (NX2NAVI_COOP_CUTSCENE_BOSSES=area)"
                    : NULL;
    if (why) {
        say("member %d's '%s' is not followed: %s", from, m->event, why);
        return;
    }
    C.fm = *m, C.f_from = from, C.f = F_WAIT, C.f_t0 = now, C.f_until = now + 45000, C.f_tries = 0, C.f_arrived = 0;
    say("following member %d's '%s'%s: waiting until this game is free (45 s at most)", from, m->event, fake ? " (test)" : "");
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
        int gate = coop_game_running() || !cfg_bool("NX2NAVI_COOP");   // tests without co-op have no gate
        if (!gate || !player || !area[0] || e->ev || (e->flags & 0x4000) || dbg_busy()) return;
        snprintf(C.home_area, sizeof C.home_area, "%s", area);
        rd(player + A_POS, C.home, 12);
        coop_game_hold(1), C.f_hold = 1;
        if ((C.fm.flags & COOP_CUT_MOVIE) || !strcmp(area, C.fm.area)) {
            say("'%s': %s: started by its name here", C.fm.event, C.fm.flags & COOP_CUT_MOVIE ? "a movie plays anywhere" : "same area");
            C.f = F_START, C.f_retry = now, C.f_until = now + 20000;
        } else {
            say("'%s': teleporting from '%s' to '%s', near %.1f %.1f %.1f%s", C.fm.event, area, C.fm.area, C.fm.xyz[0], C.fm.xyz[1],
                C.fm.xyz[2], C.boot ? ", the scene reserved for the arrival" : "");
            dbg_warp_near(C.fm.area, C.fm.xyz, C.boot ? C.fm.event : "");
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
    case F_PLAY:
        if (C.f_hold && now - C.f_t0 > 180000) {   // a talk left open, a scene that chains on: the room's values come in
            coop_game_hold(0), C.f_hold = 0;
            say("'%s': the hold ends after 3 minutes; events still play here", C.fm.event);
        }
        if (e->ev) {
            C.f_quiet = 0;
            return;
        }
        if (!C.f_quiet) C.f_quiet = now;
        if (now - C.f_quiet < 1000) return;
        if (C.f_hold) coop_game_hold(0), C.f_hold = 0;
        say("watched '%s' with member %d: over here %.1f s after the START came%s", C.fm.event, C.f_from, secs(C.f_t0, now),
            C.back && strcmp(C.home_area, area) ? "; going back" : "");
        C.f = C.back && strcmp(C.home_area, area) ? F_RETURN : F_IDLE;
        C.f_until = now + 30000;
        return;
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

static void run_tests(Cpu *c, uint64_t player, const char *area, uint64_t now) {
    if (player && !C.player_at) C.player_at = now;
    if (!C.other_at && coop_puppet_world_others() > 0) {
        C.other_at = now;
        say("another member is in the game world (%.1f s after Link)", secs(C.player_at, now));
    }
    for (int i = 0; i < C.ntest; i++) {
        Test *t = &C.test[i];
        uint64_t base = t->after_other ? C.other_at : C.player_at;
        if (t->done || !base || now < base + (uint64_t)(t->at * 1000)) continue;
        t->done = 1;
        if (t->kind == 0) {
            say("test %d: starting '%s' by its name in '%s'", i + 1, t->event, area);
            dbg_event_start(t->event);
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
            const Story *s = story_of(m.event);
            if (s && s->movie) m.flags |= COOP_CUT_MOVIE;
            say("test %d: a START as if from member 9: '%s' in '%s'", i + 1, m.event, m.area);
            consider(&m, 9, 1, player, area, now);
        }
    }
}

void coop_cutscene_frame(Cpu *c, uint64_t player, const char *area) {
    if (!__atomic_load_n(&C.active, __ATOMIC_ACQUIRE)) {
        if (C.f != F_IDLE) follow_end("cutscenes went off", plat_now_ms());
        C.hides = 0;
        return;
    }
    uint64_t now = plat_now_ms();
    EvNow e;
    ev_now(&e);
    if (C.ntest) run_tests(c, player, area, now);
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
        if (C.f == F_WAIT) follow_end("the other player's scene ended before this game was free", now);
        else say("member %d's '%s' is over there; this game's copy goes on (%s)", end_from, end.event, f_names[C.f]);
    }
    follower(&e, player, area, now);
    if (C.time_on && atomic_load(&C.others) > 0) time_frame(c, &e, player, area, now);
    C.hides = C.hide && ((C.story && e.ev) || C.f == F_PLAY);
}

int coop_cutscene_hides_puppets(void) { return C.hides; }
