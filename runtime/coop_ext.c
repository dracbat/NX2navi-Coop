// The glue between the co-op room and the co-op modules (coop_ext.h): seats, the router of their messages,
// and the steps of their hooks.
//
// The router (coop_ext_message, on the co-op thread, for messages from the players on the host's list only) passes a
// message to the handler of its kind while the sender stays within that kind's rate (rates below: each kind's honest
// rate with room to spare); what goes over the rate, and kinds no module here handles, is dropped and counted, and the
// counts are said at most once every 30 s per seat. So a player who floods the room costs this game a counter, neither a
// handler call nor a log line per message. coop_ext_log gives the modules the same for their own lines about what a
// player sent.
//
// NX2NAVI_COOP_EXT_TEST=echo (tests, environment only): every kind of coop_ext_wire.h that no module handles here is
// parsed, packed again and sent back to its sender, so that a test peer (tests/coop_ext_peer.py --echo) checks its
// codec against this one over the room, and the rates with a flood.
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "coop.h"
#include "coop_ext.h"
#include "coop_ext_wire.h"
#include "host.h"
#include "platform/platform.h"

enum {
    KINDS = 256 - COOP_EXT_KIND,
    HOOKS = 8,
    SPOT_KEEP_MS = 2000,      // a seat a message gave before the room's view listed its player: kept this long for it
    SAY_EVERY_MS = 30000,     // the counts of what was dropped or left out of the log, per seat
    LOG_PER_S = 5, LOG_BURST = 30,   // coop_ext_log's lines per seat
    RATE_DEFAULT = 20,        // messages a second of a kind not in the table below, and its burst
};

// Each kind's rate per sender: messages a second, and how many may come at once.
static const struct { uint8_t kind; uint16_t per_s, burst; } rates[] = {
    {COOP_K_KILL, 100, 100}, {COOP_K_DAMAGE, 100, 100},   // a fight's hits (coop_kill.c takes 100 events a second at most)
    {COOP_K_CUTSCENE, 4, 8}, {COOP_K_TIME, 4, 8}, {COOP_K_ERA, 4, 8},   // a beat every 2 or 3 s, and the changes
    {COOP_K_PUPPET, 30, 30},  // 15 a second
    {COOP_K_GEAR, 4, 8}, {COOP_K_PICK, 4, 8}, {COOP_K_COLOURS, 4, 8},   // at a change and when a player comes
    {COOP_K_KEYS, 4, 8},      // at a change, when a player comes and every 10 s
    {COOP_K_KEY, 10, 20},     // a key picked up or used, and the changes not acked yet every 3 s (16 at most)
};

typedef struct {
    uint32_t milli;           // tokens, in thousandths
    uint8_t used;
    uint64_t at;
} Bucket;

// 1 when a token was there (and is taken); the bucket starts full and fills at per_s a second up to burst.
static int bucket_take(Bucket *b, uint64_t now, unsigned per_s, unsigned burst) {
    uint64_t cap = (uint64_t)burst * 1000, t = b->used ? b->milli + (now - b->at) * per_s : cap;
    b->used = 1, b->at = now;
    if (t > cap) t = cap;
    if (t < 1000) {
        b->milli = (uint32_t)t;
        return 0;
    }
    b->milli = (uint32_t)(t - 1000);
    return 1;
}

static struct {
    // the hooks (registered before coop_init)
    struct { CoopExtMsgFn fn; void *user; } handler[KINDS];
    struct { CoopExtViewFn fn; void *user; } view_hook[HOOKS];
    struct { CoopExtTickFn fn; void *user; } tick_hook[HOOKS];
    int nview, ntick;
    // the co-op thread's
    int started, echo;
    CoopExtView v;
    uint64_t spot_at[COOP_PLAYERS + 1];   // when a message gave the seat (0: the view gave it)
    int named[COOP_PLAYERS + 1];          // the seat's line was logged: 1 without a name, 2 with it
    CoopView last;                        // the room's view at the last step
    char code[STEAM_CODE_CAP];            // the room's id when last seen, to tell one room from the next; never logged
    uint32_t gens;
    Bucket rate[COOP_PLAYERS + 1][KINDS];
    uint32_t over[COOP_PLAYERS + 1], unknown[COOP_PLAYERS + 1];   // dropped: over a kind's rate, a kind nobody handles
    uint8_t over_kind[COOP_PLAYERS + 1], unknown_kind[COOP_PLAYERS + 1];   // the last such kind
    uint64_t said_at[COOP_PLAYERS + 1];
    // shared with the other threads, under lock
    PlatMutex lock;
    CoopExtView snap;
    Bucket logb[COOP_PLAYERS + 1];
    uint32_t log_cut[COOP_PLAYERS + 1];
    uint64_t log_said[COOP_PLAYERS + 1];
} E;

static const char *name_of(int s) { return E.v.seat[s].name[0] ? E.v.seat[s].name : "(name not known yet)"; }

static void rate_of(uint8_t kind, unsigned *per_s, unsigned *burst) {
    *per_s = *burst = RATE_DEFAULT;
    for (size_t i = 0; i < sizeof rates / sizeof rates[0]; i++)
        if (rates[i].kind == kind) *per_s = rates[i].per_s, *burst = rates[i].burst;
}

static void publish(void) {
    plat_mutex_lock(&E.lock);
    E.snap = E.v;
    plat_mutex_unlock(&E.lock);
}

static void tell_views(void) {
    publish();
    for (int i = 0; i < E.nview; i++) E.view_hook[i].fn(&E.v, E.view_hook[i].user);
}

// The counts of what a seat's player sent and this game dropped, or left out of its log, said now.
static void say_drops(int s, uint64_t now) {
    if (E.over[s])
        rt_log("coop: ext: seat %d '%s': %u messages over their kinds' rates were dropped (the last a %s)", s, name_of(s), E.over[s],
               coop_ext_kind_name(E.over_kind[s]));
    if (E.unknown[s])
        rt_log("coop: ext: seat %d '%s': %u messages of kinds this game has no use for were dropped (the last of kind %#x)", s,
               name_of(s), E.unknown[s], E.unknown_kind[s]);
    plat_mutex_lock(&E.lock);
    uint32_t cut = E.log_cut[s];
    E.log_cut[s] = 0;
    if (cut) E.log_said[s] = now;
    plat_mutex_unlock(&E.lock);
    if (cut) rt_log("coop: ext: %u more lines about what seat %d '%s' sent were left out of the log", cut, s, name_of(s));
    if (E.over[s] || E.unknown[s]) E.said_at[s] = now;
    E.over[s] = E.unknown[s] = 0;
}

static int seat_of(uint64_t id) {
    for (int s = 1; s <= COOP_PLAYERS; s++)
        if (id && E.v.seat[s].id == id) return s;
    return 0;
}

static int give_seat(uint64_t id, uint64_t now, int spot) {
    for (int s = 1; s <= COOP_PLAYERS; s++) {
        if (E.v.seat[s].id) continue;
        CoopSeat *t = &E.v.seat[s];
        memset(t, 0, sizeof *t);
        if (!++E.gens) ++E.gens;
        t->id = id, t->gen = E.gens;
        E.spot_at[s] = spot ? now : 0, E.named[s] = 0;
        memset(E.rate[s], 0, sizeof E.rate[s]);
        E.over[s] = E.unknown[s] = 0, E.said_at[s] = 0;
        plat_mutex_lock(&E.lock);
        memset(&E.logb[s], 0, sizeof E.logb[s]);
        E.log_cut[s] = 0, E.log_said[s] = 0;
        plat_mutex_unlock(&E.lock);
        if (spot) rt_log("coop: ext: seat %d is a player whose first message came before the room listed them", s);
        return s;
    }
    return 0;
}

static void free_seat(int s, uint64_t now) {
    say_drops(s, now);
    rt_log("coop: ext: seat %d '%s' is free again", s, name_of(s));
    memset(&E.v.seat[s], 0, sizeof E.v.seat[s]);
    E.spot_at[s] = 0, E.named[s] = 0;
}

// The order of the seats: the view's, then seats given by messages that the view does not list yet.
static void make_order(const CoopView *cv) {
    int n = 0, put[COOP_PLAYERS + 1] = {0};
    for (int i = 0; i < cv->n && n < COOP_PLAYERS; i++) {
        int s = seat_of(cv->players[i].id);
        if (s && !put[s]) E.v.order[n++] = s, put[s] = 1;
    }
    for (int s = 1; s <= COOP_PLAYERS && n < COOP_PLAYERS; s++)
        if (E.v.seat[s].id && !put[s]) E.v.order[n++] = s, put[s] = 1;
    for (int i = n; i < COOP_PLAYERS; i++) E.v.order[i] = 0;
    E.v.n = n;
}

static void echo(int seat, const uint8_t *b, size_t n, void *user);

void coop_ext_init(void) {
    plat_mutex_init(&E.lock);
    char v[16];
    E.echo = plat_getenv("NX2NAVI_COOP_EXT_TEST", v, sizeof v) && !strcmp(v, "echo");
}

int coop_ext_on_message(uint8_t kind, CoopExtMsgFn fn, void *user) {
    if (kind < COOP_EXT_KIND || !fn || E.handler[kind - COOP_EXT_KIND].fn || E.started) return 0;
    E.handler[kind - COOP_EXT_KIND].fn = fn, E.handler[kind - COOP_EXT_KIND].user = user;
    return 1;
}

int coop_ext_on_view(CoopExtViewFn fn, void *user) {
    if (!fn || E.nview >= HOOKS || E.started) return 0;
    E.view_hook[E.nview].fn = fn, E.view_hook[E.nview++].user = user;
    return 1;
}

int coop_ext_on_tick(CoopExtTickFn fn, void *user) {
    if (!fn || E.ntick >= HOOKS || E.started) return 0;
    E.tick_hook[E.ntick].fn = fn, E.tick_hook[E.ntick++].user = user;
    return 1;
}

// ---- the co-op thread

void coop_ext_message(uint64_t from, uint8_t kind, const uint8_t *d, size_t n) {
    if (kind < COOP_EXT_KIND || !from) return;
    uint64_t now = plat_now_ms();
    int s = seat_of(from);
    if (!s) {   // the room let this player in a moment ago: its view lists them at the next step
        if (!(s = give_seat(from, now, 1))) return;
        make_order(&E.last);
        tell_views();
    }
    if (E.v.seat[s].self) return;
    if (!E.handler[kind - COOP_EXT_KIND].fn) {
        E.unknown[s]++, E.unknown_kind[s] = kind;
        return;
    }
    unsigned per_s, burst;
    rate_of(kind, &per_s, &burst);
    if (!bucket_take(&E.rate[s][kind - COOP_EXT_KIND], now, per_s, burst)) {
        E.over[s]++, E.over_kind[s] = kind;
        return;
    }
    E.handler[kind - COOP_EXT_KIND].fn(s, d, n, E.handler[kind - COOP_EXT_KIND].user);
}

void coop_ext_step(uint64_t now) {
    if (!E.started) {
        E.started = 1;   // the modules registered before coop_init: the test's echo takes the kinds left over
        for (uint8_t k = COOP_K_KILL; E.echo && k < COOP_K_EXT_END; k++)
            if (!E.handler[k - COOP_EXT_KIND].fn) E.handler[k - COOP_EXT_KIND].fn = echo, E.handler[k - COOP_EXT_KIND].user = (void *)(uintptr_t)k;
        if (E.echo) rt_log("coop: ext: test: the kinds no module handles are parsed and sent back (NX2NAVI_COOP_EXT_TEST=echo)");
    }
    CoopView *cv = &E.last;
    coop_view(cv);
    int in_room = cv->state == COOP_IN_ROOM && cv->n > 0, changed = 0;
    if (in_room && E.v.in_room && E.code[0] && strcmp(cv->code, E.code)) {   // another room without a step outside one
        for (int s = 1; s <= COOP_PLAYERS; s++)
            if (E.v.seat[s].id) free_seat(s, now);
        E.v.in_room = 0, changed = 1;
    }
    snprintf(E.code, sizeof E.code, "%s", in_room ? cv->code : "");
    // gone: seats the view does not list (a seat a message gave waits a little for the view to list it)
    for (int s = 1; s <= COOP_PLAYERS; s++) {
        if (!E.v.seat[s].id) continue;
        int listed = 0;
        for (int i = 0; in_room && i < cv->n; i++) listed |= cv->players[i].id == E.v.seat[s].id;
        if (listed) E.spot_at[s] = 0;
        else if (!E.spot_at[s] || now - E.spot_at[s] >= SPOT_KEEP_MS) free_seat(s, now), changed = 1;
    }
    if (in_room != E.v.in_room) {
        E.v.in_room = in_room, changed = 1;
        if (in_room) E.v.room++;
    }
    // came, and what the view says of each
    E.v.self = E.v.host = 0;
    for (int i = 0; in_room && i < cv->n; i++) {
        const CoopPlayer *p = &cv->players[i];
        int s = seat_of(p->id);
        if (!s && !(s = give_seat(p->id, now, 0))) continue;
        CoopSeat *t = &E.v.seat[s], was = *t;
        snprintf(t->name, sizeof t->name, "%s", p->name);
        t->self = p->self, t->host = p->host;
        t->in_world = p->has_pose && p->pose.scene[0];
        snprintf(t->area, sizeof t->area, "%s", t->in_world ? p->pose.scene : "");
        if (memcmp(&was, t, sizeof was)) changed = 1;
        if (t->self) E.v.self = s;
        if (t->host) E.v.host = s;
        if (E.named[s] < (t->name[0] ? 2 : 1)) {
            E.named[s] = t->name[0] ? 2 : 1;
            rt_log("coop: ext: seat %d '%s'%s%s", s, name_of(s), t->self ? " is this game" : " is in the room",
                   t->host ? ", the host" : "");
        }
    }
    int old_n = E.v.n, old_order[COOP_PLAYERS];
    memcpy(old_order, E.v.order, sizeof old_order);
    make_order(cv);
    if (E.v.n != old_n || memcmp(old_order, E.v.order, sizeof old_order)) changed = 1;
    if (changed) tell_views();
    // the counts of what each seat's player sent and was dropped
    for (int s = 1; s <= COOP_PLAYERS; s++) {
        if (!E.v.seat[s].id) continue;
        plat_mutex_lock(&E.lock);
        uint32_t cut = E.log_cut[s];
        uint64_t log_said = E.log_said[s];
        plat_mutex_unlock(&E.lock);
        if (((E.over[s] || E.unknown[s]) && now - E.said_at[s] >= SAY_EVERY_MS) || (cut && now - log_said >= SAY_EVERY_MS))
            say_drops(s, now);
    }
    if (cv->state != COOP_OFF)
        for (int i = 0; i < E.ntick; i++) E.tick_hook[i].fn(now, E.tick_hook[i].user);
}

// ---- any thread

int coop_ext_send(int seat, uint8_t kind, const void *b, size_t n, int reliable) {
    if (kind < COOP_EXT_KIND || seat < 0 || seat > COOP_PLAYERS) return 0;
    uint64_t to = 0;
    int in_room;
    plat_mutex_lock(&E.lock);
    in_room = E.snap.in_room;
    if (seat) to = E.snap.seat[seat].id;
    plat_mutex_unlock(&E.lock);
    if (!in_room || (seat && !to)) return 0;
    return coop_post(to, kind, b, n, reliable);
}

void coop_ext_view(CoopExtView *out) {
    plat_mutex_lock(&E.lock);
    *out = E.snap;
    plat_mutex_unlock(&E.lock);
}

void coop_ext_log(int seat, const char *fmt, ...) {
    if (seat >= 1 && seat <= COOP_PLAYERS) {
        plat_mutex_lock(&E.lock);
        int ok = bucket_take(&E.logb[seat], plat_now_ms(), LOG_PER_S, LOG_BURST);
        if (!ok) E.log_cut[seat]++;
        plat_mutex_unlock(&E.lock);
        if (!ok) return;
    }
    char line[480];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    rt_log("coop: %s", line);
}

// ---- the test's echo (NX2NAVI_COOP_EXT_TEST=echo)

static void echo(int seat, const uint8_t *b, size_t n, void *user) {
    uint8_t kind = (uint8_t)(uintptr_t)user, out[COOP_MESSAGE_MAX];
    size_t m = coop_ext_wire_repack(kind, b, n, out, sizeof out);
    if (!m) {
        coop_ext_log(seat, "ext: test: a %s of %zu bytes from seat %d does not parse; dropped", coop_ext_kind_name(kind), n, seat);
        return;
    }
    int same = m == n && !memcmp(out, b, n);
    int sent = coop_ext_send(seat, kind, out, m, 1);
    coop_ext_log(seat, "ext: test: a %s of %zu bytes from seat %d parses%s; %s", coop_ext_kind_name(kind), n, seat,
                 same ? " and packs to the same bytes" : ", BUT PACKS TO OTHER BYTES", sent ? "sent back" : "not sent back");
}
