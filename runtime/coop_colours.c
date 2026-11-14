// The players' colours in a co-op room (coop_colours.h), on the co-op thread.
//
// Each game says what its player picked (PICK, to everyone): when it enters a room, when its pick changes, and when
// another player comes, once at once and once more AGAIN_MS later (the newcomer's room may list this game a moment
// after this game's room listed them, and a message from a player not yet listed is not delivered there).
//
// The host's rule. Only the room's host gives colours. It works them out again at every change: a player comes or
// goes, a PICK arrives, its own pick changes, or this game becomes the host (it then starts from the last COLOURS it
// took). In order:
//   1. a colour whose player left is free again;
//   2. in the room's join order (the view's order, which only the host has right: coop.h), a player whose pick no
//      other player holds gets it: so a player changes colour;
//   3. in the same order, a player without a colour gets their pick when it is free, else the first free one of
//      green, red, blue and purple.
// So a colour once given stays, and of two players who pick one colour the earlier joiner keeps it. COLOURS goes to
// everyone when anything changed and whenever a player comes.
//
// Everyone takes COLOURS from the room's host only: from another seat it is kept aside until the room's view says
// whether that seat is the host (the host's first message can come before the view lists it), and dropped otherwise.
// Its entries name players by id; one the room does not list is not shown. Log lines name seats and names, never ids.
//
// NX2NAVI_COOP_COLOURS=0 (environment or nx2navi.ini, no menu row) turns room colours off: no PICK, no COLOURS, and no
// colours shown.
#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "coop.h"
#include "coop_colours.h"
#include "coop_ext.h"
#include "coop_ext_wire.h"
#include "host.h"
#include "platform/platform.h"

#define AGAIN_MS 3000

static const char *const colour_names[COOP_COLOURS] = {"green", "red", "blue", "purple"};

static struct {
    // the co-op thread's
    unsigned cfg_seen;
    int on;                             // NX2NAVI_COOP_COLOURS
    uint8_t own;                        // NX2NAVI_COOP_COLOUR: a COOP_COLOUR_*, or COOP_COLOUR_NONE for auto
    CoopExtView v;                      // the glue's view at its last change
    uint32_t gen[COOP_PLAYERS + 1];     // the gen of the player each seat's pick belongs to
    uint8_t pick[COOP_PLAYERS + 1];     // what each seat's player picked (COOP_COLOUR_NONE: auto, or no PICK yet)
    CoopColoursMsg room;                // the room's colours: the host's own work, or the last COLOURS taken from the host
    CoopColoursMsg held;                // a COLOURS from a seat the view did not flag as the host (yet)
    uint64_t held_from;                 // that seat's player (0: none kept); never logged
    int was_host, pick_due, colours_due;
    uint64_t again_at;                  // PICK (and on the host COLOURS) once more then, after a player came
    int8_t logged[COOP_PLAYERS + 1];    // the colours by seat the log said last
    // for the other threads, under lock
    PlatMutex lock;
    int8_t by_seat[COOP_PLAYERS + 1];
    struct { uint64_t id; int8_t colour; } by_id[COOP_PLAYERS];
    int nid, self_seat, on_now;
} C;

static const char *seat_name(int s) { return C.v.seat[s].name[0] ? C.v.seat[s].name : "(name not known yet)"; }
static const char *colour_name(int c) { return c >= 0 && c < COOP_COLOURS ? colour_names[c] : "none"; }

static int seat_of(uint64_t id) {
    for (int s = 1; s <= COOP_PLAYERS; s++)
        if (id && C.v.seat[s].id == id) return s;
    return 0;
}

static int same(const CoopColoursMsg *a, const CoopColoursMsg *b) {
    if (a->n != b->n) return 0;
    for (int i = 0; i < a->n; i++)
        if (a->e[i].id != b->e[i].id || a->e[i].colour != b->e[i].colour) return 0;
    return 1;
}

static int hosting(void) { return C.v.in_room && C.v.self && C.v.host == C.v.self; }

// The room's colours onto this game's seats, for the other threads, and a log line when they changed.
static void publish(void) {
    int8_t by_seat[COOP_PLAYERS + 1];
    memset(by_seat, -1, sizeof by_seat);
    if (C.on && C.v.in_room)
        for (int i = 0; i < C.room.n; i++) {
            int s = seat_of(C.room.e[i].id);
            if (s && C.room.e[i].colour < COOP_COLOURS) by_seat[s] = (int8_t)C.room.e[i].colour;
        }
    plat_mutex_lock(&C.lock);
    memcpy(C.by_seat, by_seat, sizeof by_seat);
    C.nid = 0;
    for (int s = 1; s <= COOP_PLAYERS; s++)
        if (by_seat[s] >= 0) C.by_id[C.nid].id = C.v.seat[s].id, C.by_id[C.nid++].colour = by_seat[s];
    C.self_seat = C.v.self, C.on_now = C.on;
    plat_mutex_unlock(&C.lock);
    if (!memcmp(by_seat, C.logged, sizeof by_seat)) return;
    memcpy(C.logged, by_seat, sizeof by_seat);
    char line[640];
    int o = snprintf(line, sizeof line, "coop: colour: the room's colours%s:", hosting() ? " (this game gives them)" : "");
    int any = 0;
    for (int i = 0; i < C.v.n && o < (int)sizeof line - 120; i++) {
        int s = C.v.order[i];
        if (s < 1 || s > COOP_PLAYERS || !C.v.seat[s].id) continue;
        uint8_t p = s == C.v.self ? C.own : C.pick[s];
        o += snprintf(line + o, sizeof line - o, "%s seat %d '%s'%s %s", any ? ";" : "", s, seat_name(s),
                      s == C.v.self ? " (this game)" : "", colour_name(by_seat[s]));
        if (p < COOP_COLOURS && p != by_seat[s]) o += snprintf(line + o, sizeof line - o, " (picked %s)", colour_names[p]);
        any = 1;
    }
    if (!any) snprintf(line + o, sizeof line - o, " none");
    rt_log("%s", line);
}

static uint8_t pick_of(int s) { return s == C.v.self ? C.own : C.pick[s]; }

// The host's rule (above). force: COLOURS goes to everyone even when nothing changed (a player came).
static void assign(int force) {
    if (!C.on || !hosting()) return;
    int col[COOP_PLAYERS + 1];
    for (int s = 0; s <= COOP_PLAYERS; s++) col[s] = -1;
    for (int i = 0; i < C.room.n; i++) {   // 1. what the room had, for its players still here
        int s = seat_of(C.room.e[i].id);
        if (s && C.room.e[i].colour < COOP_COLOURS) col[s] = C.room.e[i].colour;
    }
    for (int i = 0; i < C.v.n; i++) {   // 2. a pick no other player holds
        int s = C.v.order[i];
        uint8_t p = pick_of(s);
        if (p >= COOP_COLOURS || col[s] == p) continue;
        int held = 0;
        for (int t = 1; t <= COOP_PLAYERS; t++) held |= t != s && col[t] == p;
        if (!held) col[s] = p;
    }
    for (int i = 0; i < C.v.n; i++) {   // 3. the pick when free, else the first free colour
        int s = C.v.order[i], taken[COOP_COLOURS] = {0};
        if (col[s] >= 0) continue;
        for (int t = 1; t <= COOP_PLAYERS; t++)
            if (col[t] >= 0) taken[col[t]] = 1;
        uint8_t p = pick_of(s);
        if (p < COOP_COLOURS && !taken[p]) col[s] = p;
        for (int c = 0; c < COOP_COLOURS && col[s] < 0; c++)
            if (!taken[c]) col[s] = c;
    }
    CoopColoursMsg m;
    memset(&m, 0, sizeof m);
    for (int i = 0; i < C.v.n; i++) {
        int s = C.v.order[i];
        if (col[s] >= 0 && C.v.seat[s].id) m.e[m.n].id = C.v.seat[s].id, m.e[m.n++].colour = (uint8_t)col[s];
    }
    int changed = !same(&m, &C.room);
    C.room = m;
    if (changed || force) C.colours_due = 1;
    publish();
}

// A COLOURS taken from the room's host.
static void take(const CoopColoursMsg *m) {
    if (hosting()) return;   // this game gives the colours itself
    C.room = *m;
    publish();
}

static void on_pick(int seat, const uint8_t *b, size_t n, void *user) {
    (void)user;
    uint8_t p;
    if (seat < 1 || seat > COOP_PLAYERS) return;
    if (!coop_pick_parse(b, n, &p)) {
        coop_ext_log(seat, "colour: a PICK of %zu bytes from seat %d does not parse; dropped", n, seat);
        return;
    }
    if (p != C.pick[seat])
        coop_ext_log(seat, "colour: seat %d '%s' picks %s", seat, seat_name(seat), p < COOP_COLOURS ? colour_names[p] : "auto");
    C.pick[seat] = p;
    assign(0);
}

static void on_colours(int seat, const uint8_t *b, size_t n, void *user) {
    (void)user;
    CoopColoursMsg m;
    if (seat < 1 || seat > COOP_PLAYERS) return;
    if (!coop_colours_parse(b, n, &m)) {
        coop_ext_log(seat, "colour: a COLOURS of %zu bytes from seat %d does not parse; dropped", n, seat);
        return;
    }
    if (seat != C.v.host) {   // kept until the view says whether this seat is the host
        C.held = m, C.held_from = C.v.seat[seat].id;
        coop_ext_log(seat, "colour: COLOURS from seat %d '%s', which the room does not name its host (yet): kept aside", seat,
                     seat_name(seat));
        return;
    }
    take(&m);
}

static void on_view(const CoopExtView *v, void *user) {
    (void)user;
    int came = 0, was_in = C.v.in_room;
    for (int s = 1; s <= COOP_PLAYERS; s++) {
        uint32_t gen = v->in_room && v->seat[s].id ? v->seat[s].gen : 0;
        if (gen == C.gen[s]) continue;
        if (gen && !v->seat[s].self) came = 1;
        C.gen[s] = gen, C.pick[s] = COOP_COLOUR_NONE;
    }
    C.v = *v;
    if (!v->in_room) {   // out of the room: nothing of it stays
        memset(&C.room, 0, sizeof C.room);
        memset(&C.held, 0, sizeof C.held);
        C.held_from = 0, C.was_host = 0, C.pick_due = C.colours_due = 0, C.again_at = 0;
        publish();
        return;
    }
    if (!was_in || came) C.pick_due = 1;   // a room entered, or a player came: this game's pick goes out
    if (came) C.again_at = plat_now_ms() + AGAIN_MS;
    int host = hosting();
    if (C.held_from) {   // a COLOURS kept aside: taken when the view now names its sender the host, else dropped
        int s = seat_of(C.held_from);
        if (s && s == v->host && !host) C.room = C.held;
        memset(&C.held, 0, sizeof C.held), C.held_from = 0;
    }
    if (host && !C.was_host)
        rt_log("coop: colour: this game is the room's host and gives the colours%s", C.room.n ? ", from the last ones it took" : "");
    C.was_host = host;
    if (host) assign(came || !was_in);
    publish();
}

static void on_tick(uint64_t now, void *user) {
    (void)user;
    unsigned ver = cfg_version();
    if (ver != C.cfg_seen) {
        C.cfg_seen = ver;
        const char *on = cfg_get("NX2NAVI_COOP_COLOURS");
        const char *pick = cfg_value("NX2NAVI_COOP_COLOUR");
        static int said;
        int was_on = C.on;
        uint8_t own = COOP_COLOUR_NONE;
        for (int c = 0; c < COOP_COLOURS; c++)
            if (!strcmp(pick, colour_names[c])) own = (uint8_t)c;
        C.on = !(on && !strcmp(on, "0"));
        if (!said || own != C.own || C.on != was_on)
            rt_log("coop: colour: room colours %s (NX2NAVI_COOP_COLOURS='%s'); this player picks %s (NX2NAVI_COOP_COLOUR='%s')",
                   C.on ? "on" : "off", on ? on : "", own < COOP_COLOURS ? colour_names[own] : "none (the first free colour)",
                   pick);
        said = 1;
        if (own != C.own || (C.on && !was_on)) C.pick_due = 1;
        C.own = own;
        assign(C.on != was_on);
        publish();
    }
    if (!C.on || !C.v.in_room) return;
    if (C.again_at && now >= C.again_at) {
        C.again_at = 0, C.pick_due = 1;
        if (hosting()) C.colours_due = 1;
    }
    if (C.pick_due) {
        uint8_t b[COOP_PICK_MSG];
        size_t n = coop_pick_pack(C.own, b, sizeof b);
        if (!n || coop_ext_send(0, COOP_K_PICK, b, n, 1)) C.pick_due = 0;   // else the room's queue is full: next step
    }
    if (C.colours_due && hosting()) {
        uint8_t b[COOP_COLOURS_MSG_MAX];
        size_t n = coop_colours_pack(&C.room, b, sizeof b);
        if (!n) {
            rt_log("coop: colour: the room's colours do not pack (%d players); not sent", C.room.n);
            C.colours_due = 0;
        } else if (coop_ext_send(0, COOP_K_COLOURS, b, n, 1)) {
            C.colours_due = 0;
        }
    }
}

void coop_colours_init(void) {
    plat_mutex_init(&C.lock);
    C.cfg_seen = ~0u, C.own = COOP_COLOUR_NONE, C.on = 1;
    memset(C.pick, COOP_COLOUR_NONE, sizeof C.pick);
    memset(C.by_seat, -1, sizeof C.by_seat);
    memset(C.logged, -1, sizeof C.logged);
    coop_ext_on_message(COOP_K_PICK, on_pick, NULL);
    coop_ext_on_message(COOP_K_COLOURS, on_colours, NULL);
    coop_ext_on_view(on_view, NULL);
    coop_ext_on_tick(on_tick, NULL);
}

// ---- any thread

int coop_colours_on(void) {
    plat_mutex_lock(&C.lock);
    int on = C.on_now;
    plat_mutex_unlock(&C.lock);
    return on;
}

int coop_colours_seat(int seat) {
    if (seat < 1 || seat > COOP_PLAYERS) return -1;
    plat_mutex_lock(&C.lock);
    int c = C.by_seat[seat];
    plat_mutex_unlock(&C.lock);
    return c;
}

int coop_colours_self(void) {
    plat_mutex_lock(&C.lock);
    int c = C.self_seat >= 1 && C.self_seat <= COOP_PLAYERS ? C.by_seat[C.self_seat] : -1;
    plat_mutex_unlock(&C.lock);
    return c;
}

int coop_colours_of(uint64_t id) {
    int c = -1;
    plat_mutex_lock(&C.lock);
    for (int i = 0; i < C.nid; i++)
        if (id && C.by_id[i].id == id) c = C.by_id[i].colour;
    plat_mutex_unlock(&C.lock);
    return c;
}

int coop_colours_held_by_other(int colour) {
    int held = 0;
    plat_mutex_lock(&C.lock);
    for (int s = 1; s <= COOP_PLAYERS; s++) held |= s != C.self_seat && C.by_seat[s] == colour;
    plat_mutex_unlock(&C.lock);
    return held;
}
