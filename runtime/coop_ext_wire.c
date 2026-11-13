// The codecs of NX2navi Co-op's own messages (coop_ext_wire.h has the byte formats). Pure functions: no game state, no
// lock, no allocation.
#include <math.h>
#include <string.h>

#include "coop_ext_wire.h"

_Static_assert(COOP_K_KILL == COOP_EXT_KIND, "the modules' kinds start where coop.h gives them room");

static const char *const kind_names[] = {"KILL", "DAMAGE", "CUTSCENE", "TIME", "ERA", "PUPPET", "GEAR", "PICK", "COLOURS"};
_Static_assert(sizeof kind_names / sizeof kind_names[0] == COOP_K_EXT_END - COOP_K_KILL, "a name for every kind");

const char *coop_ext_kind_name(uint8_t kind) {
    return kind >= COOP_K_KILL && kind < COOP_K_EXT_END ? kind_names[kind - COOP_K_KILL] : "?";
}

// ---- bytes

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v), put16(p + 2, v >> 16); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v), put32(p + 4, (uint32_t)(v >> 32)); }
static uint32_t get16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t get32(const uint8_t *p) { return get16(p) | get16(p + 2) << 16; }
static uint64_t get64(const uint8_t *p) { return get32(p) | (uint64_t)get32(p + 4) << 32; }
static void putf(uint8_t *p, float f) {
    uint32_t v;
    memcpy(&v, &f, 4);
    put32(p, v);
}
static float getf(const uint8_t *p) {
    uint32_t v = get32(p);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

static int bounded(float f, float max) { return isfinite(f) && fabsf(f) <= max; }

// n bytes of printable ASCII: 0x21..0x7e, and with space also 0x20.
static int text_ok(const void *s, size_t n, int space) {
    const uint8_t *p = s;
    for (size_t i = 0; i < n; i++)
        if (p[i] < (space ? 0x20 : 0x21) || p[i] > 0x7e) return 0;
    return 1;
}

// A text field: u8 length (lo..max) and the bytes, into out (max + 1 bytes, NUL-terminated). 0 when it runs past n.
static int text_take(const uint8_t *b, size_t n, size_t *at, char *out, size_t lo, size_t max, int space) {
    if (*at >= n) return 0;
    size_t len = b[*at];
    if (len < lo || len > max || *at + 1 + len > n || !text_ok(b + *at + 1, len, space)) return 0;
    memcpy(out, b + *at + 1, len), out[len] = 0;
    *at += 1 + len;
    return 1;
}

static uint8_t *text_put(uint8_t *p, const char *s, size_t len) {
    *p++ = (uint8_t)len;
    memcpy(p, s, len);
    return p + len;
}

static int xyz_ok(const float xyz[3]) { return bounded(xyz[0], 1e6f) && bounded(xyz[1], 1e6f) && bounded(xyz[2], 1e6f); }

static uint8_t *xyz_put(uint8_t *p, const float xyz[3]) {
    for (int i = 0; i < 3; i++) putf(p + 4 * i, xyz[i]);
    return p + 12;
}

static int xyz_take(const uint8_t *b, size_t n, size_t *at, float xyz[3]) {
    if (*at + 12 > n) return 0;
    for (int i = 0; i < 3; i++) xyz[i] = getf(b + *at + 4 * (size_t)i);
    *at += 12;
    return xyz_ok(xyz);
}

// ---- KILL and DAMAGE

uint32_t coop_kill_name_hash(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

static int kill_ok(uint8_t kind, const CoopKillMsg *m, size_t al) {
    return (kind == COOP_K_KILL || kind == COOP_K_DAMAGE) && al >= 1 && al <= COOP_KILL_AREA_MAX && text_ok(m->area, al, 0) &&
           m->inst && m->inst != UINT64_MAX && !(m->flags & ~COOP_KILL_BOSS) &&
           (kind != COOP_K_DAMAGE || (m->raw >= 1 && m->raw <= 1000));
}

size_t coop_kill_pack(uint8_t kind, const CoopKillMsg *m, uint8_t *out, size_t cap) {
    size_t al = strnlen(m->area, COOP_KILL_AREA_MAX + 1);
    size_t n = 10 + al + 8 + 4 + (kind == COOP_K_DAMAGE ? 2 : 0) + 1;
    if (!kill_ok(kind, m, al) || n > cap) return 0;
    uint8_t *p = out;
    *p++ = COOP_KILL_VERSION;
    put32(p, m->epoch), p += 4;
    put32(p, m->eseq), p += 4;
    p = text_put(p, m->area, al);
    put64(p, m->inst), p += 8;
    put32(p, m->name_hash), p += 4;
    if (kind == COOP_K_DAMAGE) put16(p, m->raw), p += 2;
    *p++ = m->flags;
    return (size_t)(p - out);
}

int coop_kill_parse(uint8_t kind, const uint8_t *b, size_t n, CoopKillMsg *m) {
    memset(m, 0, sizeof *m);
    if ((kind != COOP_K_KILL && kind != COOP_K_DAMAGE) || n < 10 || b[0] != COOP_KILL_VERSION) return 0;
    size_t al = b[9];
    if (n != 10 + al + 8 + 4 + (kind == COOP_K_DAMAGE ? 2u : 0u) + 1) return 0;
    CoopKillMsg r;
    memset(&r, 0, sizeof r);
    r.epoch = get32(b + 1), r.eseq = get32(b + 5);
    if (al > COOP_KILL_AREA_MAX) return 0;
    memcpy(r.area, b + 10, al);
    const uint8_t *p = b + 10 + al;
    r.inst = get64(p), p += 8;
    r.name_hash = get32(p), p += 4;
    if (kind == COOP_K_DAMAGE) r.raw = (uint16_t)get16(p), p += 2;
    r.flags = *p;
    if (!kill_ok(kind, &r, al)) return 0;
    *m = r;
    return 1;
}

// ---- CUTSCENE

size_t coop_cut_pack(const CoopCutMsg *m, uint8_t *out, size_t cap) {
    size_t el = strnlen(m->event, COOP_CUT_EVENT_MAX + 1), al = strnlen(m->area, COOP_CUT_AREA_MAX + 1);
    if ((m->op != COOP_CUT_START && m->op != COOP_CUT_END) || m->flags & ~(COOP_CUT_ADULT | COOP_CUT_MOVIE) || el < 1 ||
        el > COOP_CUT_EVENT_MAX || al > COOP_CUT_AREA_MAX || !text_ok(m->event, el, 0) || !text_ok(m->area, al, 0) ||
        !xyz_ok(m->xyz))
        return 0;
    size_t n = 11 + 1 + el + 1 + al + 12;
    if (n > cap) return 0;
    uint8_t *p = out;
    *p++ = COOP_CUT_VERSION, *p++ = m->op;
    put32(p, m->epoch), p += 4;
    put32(p, m->seq), p += 4;
    *p++ = m->flags;
    p = text_put(p, m->event, el);
    p = text_put(p, m->area, al);
    p = xyz_put(p, m->xyz);
    return (size_t)(p - out);
}

int coop_cut_parse(const uint8_t *b, size_t n, CoopCutMsg *m) {
    memset(m, 0, sizeof *m);
    if (n < 11 || b[0] != COOP_CUT_VERSION || (b[1] != COOP_CUT_START && b[1] != COOP_CUT_END) ||
        b[10] & ~(COOP_CUT_ADULT | COOP_CUT_MOVIE))
        return 0;
    CoopCutMsg r;
    memset(&r, 0, sizeof r);
    r.op = b[1], r.epoch = get32(b + 2), r.seq = get32(b + 6), r.flags = b[10];
    size_t at = 11;
    if (!text_take(b, n, &at, r.event, 1, COOP_CUT_EVENT_MAX, 0) || !text_take(b, n, &at, r.area, 0, COOP_CUT_AREA_MAX, 0) ||
        !xyz_take(b, n, &at, r.xyz) || at != n)
        return 0;
    *m = r;
    return 1;
}

// ---- TIME

size_t coop_time_pack(const CoopTimeMsg *m, uint8_t *out, size_t cap) {
    if (cap < COOP_TIME_MSG || m->tod >= COOP_TIME_DAY || m->flags & ~COOP_TIME_JUMP) return 0;
    out[0] = COOP_TIME_VERSION;
    put32(out + 1, m->epoch), put32(out + 5, m->seq), put32(out + 9, m->order), put32(out + 13, m->tod);
    out[17] = m->flags;
    return COOP_TIME_MSG;
}

int coop_time_parse(const uint8_t *b, size_t n, CoopTimeMsg *m) {
    memset(m, 0, sizeof *m);
    if (n != COOP_TIME_MSG || b[0] != COOP_TIME_VERSION || get32(b + 13) >= COOP_TIME_DAY || b[17] & ~COOP_TIME_JUMP) return 0;
    m->epoch = get32(b + 1), m->seq = get32(b + 5), m->order = get32(b + 9), m->tod = get32(b + 13), m->flags = b[17];
    return 1;
}

// ---- ERA

enum { ERA_FLAGS = COOP_ERA_ADULT | COOP_ERA_CHANGE | COOP_ERA_SCENE_ADULT };

size_t coop_era_pack(const CoopEraMsg *m, uint8_t *out, size_t cap) {
    size_t sl = strnlen(m->scene, COOP_CUT_EVENT_MAX + 1), sal = strnlen(m->scene_area, COOP_CUT_AREA_MAX + 1),
           al = strnlen(m->area, COOP_CUT_AREA_MAX + 1);
    if (m->flags & ~ERA_FLAGS || sl > COOP_CUT_EVENT_MAX || sal > COOP_CUT_AREA_MAX || al > COOP_CUT_AREA_MAX ||
        !text_ok(m->scene, sl, 0) || !text_ok(m->scene_area, sal, 0) || !text_ok(m->area, al, 0) || !xyz_ok(m->scene_xyz) ||
        !xyz_ok(m->xyz))
        return 0;
    size_t n = 14 + 1 + sl + 1 + sal + 12 + 1 + al + 12;
    if (n > cap) return 0;
    uint8_t *p = out;
    *p++ = COOP_ERA_VERSION;
    put32(p, m->epoch), p += 4;
    put32(p, m->seq), p += 4;
    put32(p, m->order), p += 4;
    *p++ = m->flags;
    p = text_put(p, m->scene, sl);
    p = text_put(p, m->scene_area, sal);
    p = xyz_put(p, m->scene_xyz);
    p = text_put(p, m->area, al);
    p = xyz_put(p, m->xyz);
    return (size_t)(p - out);
}

int coop_era_parse(const uint8_t *b, size_t n, CoopEraMsg *m) {
    memset(m, 0, sizeof *m);
    if (n < 14 || b[0] != COOP_ERA_VERSION || b[13] & ~ERA_FLAGS) return 0;
    CoopEraMsg r;
    memset(&r, 0, sizeof r);
    r.epoch = get32(b + 1), r.seq = get32(b + 5), r.order = get32(b + 9), r.flags = b[13];
    size_t at = 14;
    if (!text_take(b, n, &at, r.scene, 0, COOP_CUT_EVENT_MAX, 0) || !text_take(b, n, &at, r.scene_area, 0, COOP_CUT_AREA_MAX, 0) ||
        !xyz_take(b, n, &at, r.scene_xyz) || !text_take(b, n, &at, r.area, 0, COOP_CUT_AREA_MAX, 0) || !xyz_take(b, n, &at, r.xyz) ||
        at != n)
        return 0;
    *m = r;
    return 1;
}

// ---- PUPPET

enum { PUPPET_FLAGS = COOP_PUPPET_ADULT | COOP_PUPPET_IN_WORLD, PUPPET_UPPER = 7 };

static int puppet_numbers_ok(const float pos[3], float yaw) {
    return bounded(pos[0], COOP_PUPPET_POS_MAX) && bounded(pos[1], COOP_PUPPET_POS_MAX) && bounded(pos[2], COOP_PUPPET_POS_MAX) &&
           bounded(yaw, COOP_PUPPET_YAW_MAX);
}

size_t coop_puppet_pack(const CoopPuppetMsg *m, uint8_t *out, size_t cap) {
    size_t al = strnlen(m->area, COOP_PUPPET_TEXT + 1), n = 11 + al + 17;
    if (m->flags & ~PUPPET_FLAGS || m->upper & ~PUPPET_UPPER || m->nslots > COOP_PUPPET_SLOTS || al > COOP_PUPPET_TEXT ||
        !text_ok(m->area, al, 1) || !puppet_numbers_ok(m->pos, m->yaw))
        return 0;
    size_t nl[COOP_PUPPET_SLOTS];
    for (int i = 0; i < m->nslots; i++) {
        nl[i] = strnlen(m->slot[i].node, COOP_PUPPET_TEXT + 1);
        if (nl[i] > COOP_PUPPET_TEXT || !text_ok(m->slot[i].node, nl[i], 1) || !bounded(m->slot[i].frame, COOP_PUPPET_FRAME_MAX))
            return 0;
        n += 1 + nl[i] + 4;
    }
    n += 5;
    if (n > cap) return 0;
    uint8_t *p = out;
    *p++ = COOP_PUPPET_VERSION;
    put32(p, m->pseq), p += 4;
    put32(p, m->t_ms), p += 4;
    *p++ = m->flags;
    p = text_put(p, m->area, al);
    p = xyz_put(p, m->pos);
    putf(p, m->yaw), p += 4;
    *p++ = m->nslots;
    for (int i = 0; i < m->nslots; i++) {
        p = text_put(p, m->slot[i].node, nl[i]);
        putf(p, m->slot[i].frame), p += 4;
    }
    *p++ = m->upper;
    put32(p, m->gear_sig), p += 4;
    return (size_t)(p - out);
}

int coop_puppet_parse(const uint8_t *b, size_t n, CoopPuppetMsg *m) {
    memset(m, 0, sizeof *m);
    if (n < 11 || b[0] != COOP_PUPPET_VERSION || b[9] & ~PUPPET_FLAGS) return 0;
    CoopPuppetMsg r;
    memset(&r, 0, sizeof r);
    r.pseq = get32(b + 1), r.t_ms = get32(b + 5), r.flags = b[9];
    size_t at = 10;
    if (!text_take(b, n, &at, r.area, 0, COOP_PUPPET_TEXT, 1) || at + 17 > n) return 0;
    for (int k = 0; k < 3; k++) r.pos[k] = getf(b + at + 4 * k);
    r.yaw = getf(b + at + 12);
    r.nslots = b[at + 16];
    at += 17;
    if (!puppet_numbers_ok(r.pos, r.yaw) || r.nslots > COOP_PUPPET_SLOTS) return 0;
    for (int i = 0; i < r.nslots; i++) {
        if (!text_take(b, n, &at, r.slot[i].node, 0, COOP_PUPPET_TEXT, 1) || at + 4 > n) return 0;
        r.slot[i].frame = getf(b + at);
        at += 4;
        if (!bounded(r.slot[i].frame, COOP_PUPPET_FRAME_MAX)) return 0;
    }
    if (at + 5 != n || b[at] & ~PUPPET_UPPER) return 0;
    r.upper = b[at], r.gear_sig = get32(b + at + 1);
    *m = r;
    return 1;
}

// ---- GEAR

uint32_t coop_gear_sig(const CoopGear *g) {
    uint32_t h = 2166136261u;
    for (int j = 0; j < COOP_GEAR_DYN; j++) {
        for (size_t i = 0; i < sizeof g->dyn[j] && g->dyn[j][i]; i++) h = (h ^ (uint8_t)g->dyn[j][i]) * 16777619u;
        h = (h ^ (g->dyn[j][0] ? 0x100u + g->state[j] : 0x55u)) * 16777619u;
    }
    for (int s = 0; s < COOP_GEAR_STAT; s++) {
        for (size_t i = 0; i < sizeof g->stat[s] && g->stat[s][i]; i++) h = (h ^ (uint8_t)g->stat[s][i]) * 16777619u;
        h = (h ^ 0x77u) * 16777619u;
    }
    return h ? h : 1;
}

size_t coop_gear_pack(const CoopGear *g, uint8_t *out, size_t cap) {
    uint32_t dmask = 0, smask = 0;
    size_t n = 8, dl[COOP_GEAR_DYN], sl[COOP_GEAR_STAT];
    for (int j = 0; j < COOP_GEAR_DYN; j++) {
        dl[j] = strnlen(g->dyn[j], COOP_PUPPET_TEXT + 1);
        if (!dl[j]) continue;
        if (dl[j] > COOP_PUPPET_TEXT || !text_ok(g->dyn[j], dl[j], 1) || g->state[j] > COOP_GEAR_SHEATHED) return 0;
        dmask |= 1u << j, n += 2 + dl[j];
    }
    for (int s = 0; s < COOP_GEAR_STAT; s++) {
        sl[s] = strnlen(g->stat[s], COOP_PUPPET_TEXT + 1);
        if (!sl[s]) continue;
        if (sl[s] > COOP_PUPPET_TEXT || !text_ok(g->stat[s], sl[s], 1)) return 0;
        smask |= 1u << s, n += 1 + sl[s];
    }
    if (n > cap) return 0;
    uint8_t *p = out;
    *p++ = COOP_GEAR_VERSION;
    put32(p, coop_gear_sig(g)), p += 4;
    *p++ = (uint8_t)dmask;
    put16(p, smask), p += 2;
    for (int j = 0; j < COOP_GEAR_DYN; j++)
        if (dmask >> j & 1) *p++ = g->state[j], p = text_put(p, g->dyn[j], dl[j]);
    for (int s = 0; s < COOP_GEAR_STAT; s++)
        if (smask >> s & 1) p = text_put(p, g->stat[s], sl[s]);
    return (size_t)(p - out);
}

int coop_gear_parse(const uint8_t *b, size_t n, CoopGear *g, uint32_t *sig) {
    memset(g, 0, sizeof *g);
    if (sig) *sig = 0;
    if (n < 8 || b[0] != COOP_GEAR_VERSION) return 0;
    uint32_t s = get32(b + 1), dmask = b[5], smask = get16(b + 6);
    if (dmask >> COOP_GEAR_DYN || smask >> COOP_GEAR_STAT) return 0;
    CoopGear r;
    memset(&r, 0, sizeof r);
    size_t at = 8;
    for (int j = 0; j < COOP_GEAR_DYN; j++) {
        if (!(dmask >> j & 1)) continue;
        if (at >= n || b[at] > COOP_GEAR_SHEATHED) return 0;
        r.state[j] = b[at++];
        if (!text_take(b, n, &at, r.dyn[j], 1, COOP_PUPPET_TEXT, 1)) return 0;
    }
    for (int k = 0; k < COOP_GEAR_STAT; k++)
        if (smask >> k & 1 && !text_take(b, n, &at, r.stat[k], 1, COOP_PUPPET_TEXT, 1)) return 0;
    if (at != n || coop_gear_sig(&r) != s) return 0;
    *g = r;
    if (sig) *sig = s;
    return 1;
}

// ---- PICK and COLOURS

static int colour_ok(uint8_t c) { return c < COOP_COLOURS; }

size_t coop_pick_pack(uint8_t pick, uint8_t *out, size_t cap) {
    if (cap < COOP_PICK_MSG || (!colour_ok(pick) && pick != COOP_COLOUR_NONE)) return 0;
    out[0] = COOP_PICK_VERSION, out[1] = pick;
    return COOP_PICK_MSG;
}

int coop_pick_parse(const uint8_t *b, size_t n, uint8_t *pick) {
    *pick = COOP_COLOUR_NONE;
    if (n != COOP_PICK_MSG || b[0] != COOP_PICK_VERSION || (!colour_ok(b[1]) && b[1] != COOP_COLOUR_NONE)) return 0;
    *pick = b[1];
    return 1;
}

static int colours_ok(const CoopColoursMsg *m) {
    if (m->n > COOP_PLAYERS) return 0;
    for (int i = 0; i < m->n; i++) {
        if (!m->e[i].id || !colour_ok(m->e[i].colour)) return 0;
        for (int k = 0; k < i; k++)
            if (m->e[k].id == m->e[i].id || m->e[k].colour == m->e[i].colour) return 0;
    }
    return 1;
}

size_t coop_colours_pack(const CoopColoursMsg *m, uint8_t *out, size_t cap) {
    size_t n = 2 + 9 * (size_t)m->n;
    if (!colours_ok(m) || n > cap) return 0;
    out[0] = COOP_COLOURS_VERSION, out[1] = m->n;
    for (int i = 0; i < m->n; i++) put64(out + 2 + 9 * i, m->e[i].id), out[2 + 9 * i + 8] = m->e[i].colour;
    return n;
}

int coop_colours_parse(const uint8_t *b, size_t n, CoopColoursMsg *m) {
    memset(m, 0, sizeof *m);
    if (n < 2 || b[0] != COOP_COLOURS_VERSION || b[1] > COOP_PLAYERS || n != 2 + 9 * (size_t)b[1]) return 0;
    CoopColoursMsg r;
    memset(&r, 0, sizeof r);
    r.n = b[1];
    for (int i = 0; i < r.n; i++) r.e[i].id = get64(b + 2 + 9 * i), r.e[i].colour = b[2 + 9 * i + 8];
    if (!colours_ok(&r)) return 0;
    *m = r;
    return 1;
}

// ---- any kind

size_t coop_ext_wire_repack(uint8_t kind, const uint8_t *b, size_t n, uint8_t *out, size_t cap) {
    switch (kind) {
    case COOP_K_KILL:
    case COOP_K_DAMAGE: {
        CoopKillMsg m;
        return coop_kill_parse(kind, b, n, &m) ? coop_kill_pack(kind, &m, out, cap) : 0;
    }
    case COOP_K_CUTSCENE: {
        CoopCutMsg m;
        return coop_cut_parse(b, n, &m) ? coop_cut_pack(&m, out, cap) : 0;
    }
    case COOP_K_TIME: {
        CoopTimeMsg m;
        return coop_time_parse(b, n, &m) ? coop_time_pack(&m, out, cap) : 0;
    }
    case COOP_K_ERA: {
        CoopEraMsg m;
        return coop_era_parse(b, n, &m) ? coop_era_pack(&m, out, cap) : 0;
    }
    case COOP_K_PUPPET: {
        CoopPuppetMsg m;
        return coop_puppet_parse(b, n, &m) ? coop_puppet_pack(&m, out, cap) : 0;
    }
    case COOP_K_GEAR: {
        CoopGear g;
        uint32_t sig;
        return coop_gear_parse(b, n, &g, &sig) ? coop_gear_pack(&g, out, cap) : 0;
    }
    case COOP_K_PICK: {
        uint8_t pick;
        return coop_pick_parse(b, n, &pick) ? coop_pick_pack(pick, out, cap) : 0;
    }
    case COOP_K_COLOURS: {
        CoopColoursMsg m;
        return coop_colours_parse(b, n, &m) ? coop_colours_pack(&m, out, cap) : 0;
    }
    }
    return 0;
}

// ---- the PUPPET counter

int coop_pos_fresh(CoopPosSeen *s, uint32_t gen, uint32_t pseq, uint64_t now_ms) {
    int again = !s->any || s->gen != gen || now_ms - s->at_ms >= COOP_POS_QUIET_MS;
    if (!again && (int32_t)(pseq - s->last) <= 0) return 0;
    s->any = 1, s->gen = gen, s->last = pseq, s->at_ms = now_ms;
    return again ? 2 : 1;
}
