// --probe-coop-ext: the codecs of the co-op modules' messages (coop_ext_wire.c), without a window, a network or game
// files:
//   1. the byte vectors of tests/coop_ext_vectors.txt (the file NX2NAVI_COOP_EXT_VECTORS names; tests/coop_ext_peer.py
//      writes it and checks its own codec against it): each message parses to its fields and packs to the same bytes,
//      every shorter and longer copy of it and every refusal of the file is refused;
//   2. the packers refuse what the parsers refuse (texts, numbers, ranges, room in the buffer);
//   3. random changes of the vectors and random bytes into every parser: what parses packs back to the same bytes;
//      random messages packed, parsed and packed again give the same bytes;
//   4. the PUPPET counter (coop_pos_fresh) and the kinds' names.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "coop_ext.h"
#include "coop_ext_wire.h"
#include "platform/platform.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; fprintf(stderr, "FAIL coop-ext: "); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) {
    rng ^= rng >> 12, rng ^= rng << 25, rng ^= rng >> 27;
    return (uint32_t)((rng * 0x2545F4914F6CDD1Dull) >> 32);
}
static uint32_t rnd_below(uint32_t n) { return n ? rnd() % n : 0; }

// ---- the vectors file

static int hexval(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

// "-" is empty. The bytes into out (cap), their count; -1 when the text is not hex or does not fit.
static int unhex(const char *t, uint8_t *out, size_t cap) {
    if (!t) return -1;
    if (!strcmp(t, "-")) return 0;
    size_t n = strlen(t);
    if (n % 2 || n / 2 > cap) return -1;
    for (size_t i = 0; i < n / 2; i++) {
        int a = hexval(t[2 * i]), b = hexval(t[2 * i + 1]);
        if (a < 0 || b < 0) return -1;
        out[i] = (uint8_t)(a << 4 | b);
    }
    return (int)(n / 2);
}

// A text field (hex, "-" empty) as a C string into out (cap); 0 when it is not one.
static int untext(const char *t, char *out, size_t cap) {
    int n = unhex(t, (uint8_t *)out, cap - 1);
    if (n < 0) return 0;
    out[n] = 0;
    return 1;
}

typedef struct { char *t[96]; int n; } Toks;
static void split(char *line, Toks *k) {
    k->n = 0;
    for (char *p = strtok(line, " \t\r\n"); p && k->n < 96; p = strtok(NULL, " \t\r\n")) k->t[k->n++] = p;
}
static const char *tok(const Toks *k, int i) { return i < k->n ? k->t[i] : "?"; }
static uint64_t num(const Toks *k, int i) { return strtoull(tok(k, i), NULL, 10); }
static float fnum(const Toks *k, int i) { return strtof(tok(k, i), NULL); }

static int kind_of(const char *name) {
    for (int k = COOP_K_KILL; k < COOP_K_EXT_END; k++) {
        const char *kn = coop_ext_kind_name((uint8_t)k);
        size_t i = 0;
        for (; kn[i] && name[i] && (kn[i] | 0x20) == name[i]; i++) {}
        if (!kn[i] && !name[i]) return k;
    }
    return -1;
}

static int same_text(const Toks *k, int i, const char *s) {
    char t[COOP_MESSAGE_MAX + 1];
    return untext(tok(k, i), t, sizeof t) && !strcmp(t, s);
}

static int same_xyz(const Toks *k, int i, const float v[3]) {
    return fnum(k, i) == v[0] && fnum(k, i + 1) == v[1] && fnum(k, i + 2) == v[2];
}

// The fields of a message line (k->t[3] on) against what the parser made of its bytes.
static int fields_ok(int kind, const uint8_t *b, size_t n, const Toks *k) {
    switch (kind) {
    case COOP_K_KILL:
    case COOP_K_DAMAGE: {
        CoopKillMsg m;
        int d = kind == COOP_K_DAMAGE;
        return coop_kill_parse((uint8_t)kind, b, n, &m) && k->n == 9 + d && m.epoch == num(k, 3) && m.eseq == num(k, 4) &&
               same_text(k, 5, m.area) && m.inst == num(k, 6) && m.name_hash == num(k, 7) && (!d || m.raw == num(k, 8)) &&
               m.flags == num(k, 8 + d);
    }
    case COOP_K_CUTSCENE: {
        CoopCutMsg m;
        return coop_cut_parse(b, n, &m) && k->n == 12 && m.op == num(k, 3) && m.flags == num(k, 4) && m.epoch == num(k, 5) &&
               m.seq == num(k, 6) && same_text(k, 7, m.event) && same_text(k, 8, m.area) && same_xyz(k, 9, m.xyz);
    }
    case COOP_K_TIME: {
        CoopTimeMsg m;
        return coop_time_parse(b, n, &m) && k->n == 8 && m.epoch == num(k, 3) && m.seq == num(k, 4) && m.order == num(k, 5) &&
               m.tod == num(k, 6) && m.flags == num(k, 7);
    }
    case COOP_K_ERA: {
        CoopEraMsg m;
        return coop_era_parse(b, n, &m) && k->n == 16 && m.epoch == num(k, 3) && m.seq == num(k, 4) && m.order == num(k, 5) &&
               m.flags == num(k, 6) && same_text(k, 7, m.scene) && same_text(k, 8, m.scene_area) && same_xyz(k, 9, m.scene_xyz) &&
               same_text(k, 12, m.area) && same_xyz(k, 13, m.xyz);
    }
    case COOP_K_PUPPET: {
        CoopPuppetMsg m;
        if (!coop_puppet_parse(b, n, &m) || k->n != 14 + 2 * m.nslots || m.pseq != num(k, 3) || m.t_ms != num(k, 4) ||
            m.flags != num(k, 5) || !same_text(k, 6, m.area) || !same_xyz(k, 7, m.pos) || m.yaw != fnum(k, 10) ||
            m.upper != num(k, 11) || m.gear_sig != num(k, 12) || m.nslots != num(k, 13))
            return 0;
        for (int i = 0; i < m.nslots; i++)
            if (!same_text(k, 14 + 2 * i, m.slot[i].node) || m.slot[i].frame != fnum(k, 15 + 2 * i)) return 0;
        return 1;
    }
    case COOP_K_GEAR: {
        CoopGear g, want;
        uint32_t sig;
        if (!coop_gear_parse(b, n, &g, &sig) || sig != num(k, 3)) return 0;
        memset(&want, 0, sizeof want);
        for (int i = 4; i < k->n; i++) {
            int slot, state, at = 0;
            const char *t = k->t[i];
            if (sscanf(t, "d%d:%d:%n", &slot, &state, &at) == 2 && at && slot >= 0 && slot < COOP_GEAR_DYN) {
                want.state[slot] = (uint8_t)state;
                if (!untext(t + at, want.dyn[slot], sizeof want.dyn[slot])) return 0;
            } else if (sscanf(t, "s%d:%n", &slot, &at) == 1 && at && slot >= 0 && slot < COOP_GEAR_STAT) {
                if (!untext(t + at, want.stat[slot], sizeof want.stat[slot])) return 0;
            } else {
                return 0;
            }
        }
        return !memcmp(&g, &want, sizeof g);
    }
    case COOP_K_PICK: {
        uint8_t pick;
        return coop_pick_parse(b, n, &pick) && k->n == 4 && pick == num(k, 3);
    }
    case COOP_K_COLOURS: {
        CoopColoursMsg m;
        if (!coop_colours_parse(b, n, &m) || k->n != 4 + 2 * m.n || m.n != num(k, 3)) return 0;
        for (int i = 0; i < m.n; i++)
            if (m.e[i].id != num(k, 4 + 2 * i) || m.e[i].colour != num(k, 5 + 2 * i)) return 0;
        return 1;
    }
    case COOP_K_KEYS: {
        CoopKeysMsg m;
        if (!coop_keys_parse(b, n, &m) || k->n != 6 + 2 * m.n + 3 * m.nack || m.serial != num(k, 3) || m.n != num(k, 4) ||
            m.nack != num(k, 5 + 2 * m.n))
            return 0;
        for (int i = 0; i < m.n; i++)
            if (m.d[i].hash != num(k, 5 + 2 * i) || m.d[i].keys != num(k, 6 + 2 * i)) return 0;
        for (int i = 0, at = 6 + 2 * m.n; i < m.nack; i++, at += 3)
            if (m.ack[i].id != num(k, at) || m.ack[i].sess != num(k, at + 1) || m.ack[i].seq != num(k, at + 2)) return 0;
        return 1;
    }
    case COOP_K_KEY: {
        CoopKeyMsg m;
        return coop_key_parse(b, n, &m) && k->n == 11 && m.sess == num(k, 3) && m.seq == num(k, 4) && m.hash == num(k, 5) &&
               m.delta == strtol(tok(k, 6), NULL, 10) && m.flags == num(k, 7) && same_xyz(k, 8, m.xyz);
    }
    }
    return 0;
}

static int parses(int kind, const uint8_t *b, size_t n) {
    uint8_t out[COOP_MESSAGE_MAX];
    return coop_ext_wire_repack((uint8_t)kind, b, n, out, sizeof out) != 0;
}

// What parses packs back to the same bytes; 1 when it parsed.
static int round_trip(int kind, const uint8_t *b, size_t n, const char *what) {
    uint8_t out[COOP_MESSAGE_MAX];
    size_t m = coop_ext_wire_repack((uint8_t)kind, b, n, out, sizeof out);
    if (m) CHECK(m == n && !memcmp(out, b, n), "%s: a %s of %zu bytes parses and packs to %zu other bytes", what, coop_ext_kind_name((uint8_t)kind), n, m);
    return m != 0;
}

enum { MAX_VECTORS = 256 };
static struct { int kind; size_t n; uint8_t b[COOP_MESSAGE_MAX]; } vec[MAX_VECTORS];
static int nvec;

static void probe_vectors(int *messages, int *refusals) {
    char path[PLAT_PATH_MAX];
    *messages = *refusals = 0;
    if (!plat_getenv("NX2NAVI_COOP_EXT_VECTORS", path, sizeof path) || !path[0]) {
        printf("probe-coop-ext: no NX2NAVI_COOP_EXT_VECTORS: the byte vectors are not checked\n");
        return;
    }
    FILE *f = fopen(path, "rb");
    CHECK(f, "cannot read the vectors file %s", path);
    if (!f) return;
    static char line[8192];
    int kinds_seen[256] = {0};
    while (fgets(line, sizeof line, f)) {
        Toks k;
        split(line, &k);
        if (!k.n || k.t[0][0] == '#') continue;
        uint8_t b[COOP_MESSAGE_MAX + 1];
        if (!strcmp(k.t[0], "reject")) {
            int kind = kind_of(tok(&k, 1)), n = unhex(tok(&k, 3), b, sizeof b - 1);
            CHECK(kind >= 0 && n >= 0, "the refusal %s is not a kind and hex", tok(&k, 2));
            if (kind < 0 || n < 0) continue;
            CHECK(!parses(kind, b, (size_t)n), "the refusal %s parses", tok(&k, 2));
            (*refusals)++;
            continue;
        }
        int kind = kind_of(k.t[0]), n = unhex(tok(&k, 2), b, sizeof b - 1);
        CHECK(kind >= 0 && n > 0, "the line of %s is not a kind, a name and hex", tok(&k, 1));
        if (kind < 0 || n <= 0) continue;
        CHECK(fields_ok(kind, b, (size_t)n, &k), "the vector %s does not parse to its fields", tok(&k, 1));
        round_trip(kind, b, (size_t)n, tok(&k, 1));
        CHECK(parses(kind, b, (size_t)n), "the vector %s does not parse", tok(&k, 1));
        for (int cut = 0; cut < n; cut++) CHECK(!parses(kind, b, (size_t)cut), "the vector %s cut to %d bytes parses", tok(&k, 1), cut);
        b[n] = 0;
        CHECK(!parses(kind, b, (size_t)n + 1), "the vector %s with one more byte parses", tok(&k, 1));
        b[n] = 0x41;
        CHECK(!parses(kind, b, (size_t)n + 1), "the vector %s with one more letter parses", tok(&k, 1));
        if (nvec < MAX_VECTORS) vec[nvec].kind = kind, vec[nvec].n = (size_t)n, memcpy(vec[nvec].b, b, (size_t)n), nvec++;
        kinds_seen[kind] = 1;
        (*messages)++;
    }
    fclose(f);
    for (int kd = COOP_K_KILL; kd < COOP_K_EXT_END; kd++)
        CHECK(kinds_seen[kd], "the vectors file has no message of kind %s", coop_ext_kind_name((uint8_t)kd));
    CHECK(*messages >= 20 && *refusals >= 20, "the vectors file has %d messages and %d refusals", *messages, *refusals);
}

// ---- the packers' refusals

static void fill(char *s, size_t n, char c) {
    memset(s, c, n);
    s[n] = 0;
}

static void probe_packers(void) {
    uint8_t out[COOP_MESSAGE_MAX];
    CoopKillMsg k = {.epoch = 1, .eseq = 1, .area = "Area", .inst = 5, .name_hash = 7, .raw = 1, .flags = 0};
    CHECK(coop_kill_pack(COOP_K_KILL, &k, out, sizeof out) && coop_kill_pack(COOP_K_DAMAGE, &k, out, sizeof out), "a KILL packs");
    CHECK(!coop_kill_pack(COOP_K_CUTSCENE, &k, out, sizeof out), "a KILL packs as another kind");
    size_t kn = coop_kill_pack(COOP_K_DAMAGE, &k, out, sizeof out);
    CHECK(!coop_kill_pack(COOP_K_DAMAGE, &k, out, kn - 1), "a DAMAGE packs into a buffer one byte short");
    CoopKillMsg kb = k;
    kb.area[0] = 0;
    CHECK(!coop_kill_pack(COOP_K_KILL, &kb, out, sizeof out), "a KILL without an area packs");
    kb = k, strcpy(kb.area, "A rea");
    CHECK(!coop_kill_pack(COOP_K_KILL, &kb, out, sizeof out), "a KILL with a space in its area packs");
    kb = k, kb.inst = 0;
    CHECK(!coop_kill_pack(COOP_K_KILL, &kb, out, sizeof out), "a KILL of inst 0 packs");
    kb = k, kb.inst = UINT64_MAX;
    CHECK(!coop_kill_pack(COOP_K_KILL, &kb, out, sizeof out), "a KILL of inst all ones packs");
    kb = k, kb.flags = 2;
    CHECK(!coop_kill_pack(COOP_K_KILL, &kb, out, sizeof out), "a KILL with an unknown flag packs");
    kb = k, kb.raw = 0;
    CHECK(!coop_kill_pack(COOP_K_DAMAGE, &kb, out, sizeof out) && coop_kill_pack(COOP_K_KILL, &kb, out, sizeof out), "raw 0");
    kb = k, kb.raw = 1001;
    CHECK(!coop_kill_pack(COOP_K_DAMAGE, &kb, out, sizeof out), "a DAMAGE of 1001 packs");

    CoopCutMsg c = {.op = COOP_CUT_START, .flags = COOP_CUT_ADULT, .epoch = 1, .seq = 1, .event = "Dm_Test", .area = "", .xyz = {1, 2, 3}};
    CHECK(coop_cut_pack(&c, out, sizeof out), "a CUTSCENE packs");
    CoopCutMsg cb = c;
    cb.op = 3;
    CHECK(!coop_cut_pack(&cb, out, sizeof out), "a CUTSCENE of op 3 packs");
    cb = c, cb.flags = 4;
    CHECK(!coop_cut_pack(&cb, out, sizeof out), "a CUTSCENE with an unknown flag packs");
    cb = c, cb.event[0] = 0;
    CHECK(!coop_cut_pack(&cb, out, sizeof out), "a CUTSCENE without an event packs");
    cb = c, strcpy(cb.event, "Dm Test");
    CHECK(!coop_cut_pack(&cb, out, sizeof out), "a CUTSCENE with a space in its event packs");
    cb = c, cb.xyz[1] = NAN;
    CHECK(!coop_cut_pack(&cb, out, sizeof out), "a CUTSCENE at NaN packs");
    cb = c, cb.xyz[2] = INFINITY;
    CHECK(!coop_cut_pack(&cb, out, sizeof out), "a CUTSCENE at infinity packs");
    cb = c, cb.xyz[0] = -1.5e6f;
    CHECK(!coop_cut_pack(&cb, out, sizeof out), "a CUTSCENE 1.5e6 m away packs");

    CoopTimeMsg t = {.epoch = 1, .seq = 1, .order = 1, .tod = COOP_TIME_DAY - 1, .flags = COOP_TIME_JUMP};
    CHECK(coop_time_pack(&t, out, sizeof out) == COOP_TIME_MSG && !coop_time_pack(&t, out, COOP_TIME_MSG - 1), "a TIME packs");
    t.tod = COOP_TIME_DAY;
    CHECK(!coop_time_pack(&t, out, sizeof out), "a TIME of 24:00 packs");
    t.tod = 0, t.flags = 2;
    CHECK(!coop_time_pack(&t, out, sizeof out), "a TIME with an unknown flag packs");

    CoopEraMsg e;
    memset(&e, 0, sizeof e);
    e.flags = COOP_ERA_ADULT | COOP_ERA_CHANGE | COOP_ERA_SCENE_ADULT;
    CHECK(coop_era_pack(&e, out, sizeof out), "an empty ERA packs");
    CoopEraMsg eb = e;
    eb.flags = 8;
    CHECK(!coop_era_pack(&eb, out, sizeof out), "an ERA with an unknown flag packs");
    eb = e, fill(eb.scene, COOP_CUT_EVENT_MAX, 'S'), eb.scene[COOP_CUT_EVENT_MAX] = 'S';   // 80 bytes, no end within the cap
    CHECK(!coop_era_pack(&eb, out, sizeof out), "an ERA with a scene of 80 bytes packs");
    eb = e, strcpy(eb.area, "A B");
    CHECK(!coop_era_pack(&eb, out, sizeof out), "an ERA with a space in its area packs");
    eb = e, eb.scene_xyz[0] = -INFINITY;
    CHECK(!coop_era_pack(&eb, out, sizeof out), "an ERA at infinity packs");

    CoopPuppetMsg p;
    memset(&p, 0, sizeof p);
    p.flags = COOP_PUPPET_ADULT | COOP_PUPPET_IN_WORLD, p.nslots = COOP_PUPPET_SLOTS, p.upper = 7, p.yaw = 3.14f;
    strcpy(p.area, "An area");
    CHECK(coop_puppet_pack(&p, out, sizeof out), "a PUPPET packs");
    CoopPuppetMsg pb = p;
    pb.flags = 4;
    CHECK(!coop_puppet_pack(&pb, out, sizeof out), "a PUPPET with an unknown flag packs");
    pb = p, pb.upper = 8;
    CHECK(!coop_puppet_pack(&pb, out, sizeof out), "a PUPPET with upper bit 3 packs");
    pb = p, pb.nslots = COOP_PUPPET_SLOTS + 1;
    CHECK(!coop_puppet_pack(&pb, out, sizeof out), "a PUPPET of 5 slots packs");
    pb = p, pb.yaw = 7.5f;
    CHECK(!coop_puppet_pack(&pb, out, sizeof out), "a PUPPET turned 7.5 radians packs");
    pb = p, pb.yaw = NAN;
    CHECK(!coop_puppet_pack(&pb, out, sizeof out), "a PUPPET turned NaN packs");
    pb = p, pb.pos[2] = 1.5e6f;
    CHECK(!coop_puppet_pack(&pb, out, sizeof out), "a PUPPET 1.5e6 m away packs");
    pb = p, pb.slot[3].frame = INFINITY;
    CHECK(!coop_puppet_pack(&pb, out, sizeof out), "a PUPPET at frame infinity packs");
    pb = p, fill(pb.area, COOP_PUPPET_TEXT, 'a'), pb.area[COOP_PUPPET_TEXT] = 'a';   // 48 bytes, no end within the cap
    CHECK(!coop_puppet_pack(&pb, out, sizeof out), "a PUPPET with an area of 48 bytes packs");
    pb = p, strcpy(pb.slot[1].node, "a\x01");
    CHECK(!coop_puppet_pack(&pb, out, sizeof out), "a PUPPET with a control character packs");

    CoopGear g;
    memset(&g, 0, sizeof g);
    strcpy(g.dyn[0], "Sword"), g.state[0] = COOP_GEAR_SHEATHED, strcpy(g.stat[12], "Boots");
    size_t gn = coop_gear_pack(&g, out, sizeof out);
    CHECK(gn && !coop_gear_pack(&g, out, gn - 1), "a GEAR packs, and not into a buffer one byte short");
    CoopGear gb = g;
    gb.state[0] = 3;
    CHECK(!coop_gear_pack(&gb, out, sizeof out), "a GEAR with state 3 packs");
    gb = g, fill(gb.stat[2], COOP_PUPPET_TEXT, 'x'), gb.stat[2][COOP_PUPPET_TEXT] = 'x';
    CHECK(!coop_gear_pack(&gb, out, sizeof out), "a GEAR with a name of 48 bytes packs");
    gb = g, strcpy(gb.dyn[6], "x\x7f");
    CHECK(!coop_gear_pack(&gb, out, sizeof out), "a GEAR with a DEL packs");
    CHECK(coop_gear_sig(&g) && coop_gear_sig(&g) != coop_gear_sig(&gb), "the gear signature");

    CHECK(coop_pick_pack(COOP_COLOUR_PURPLE, out, sizeof out) && coop_pick_pack(COOP_COLOUR_NONE, out, sizeof out) &&
          !coop_pick_pack(COOP_COLOURS, out, sizeof out) && !coop_pick_pack(0, out, 1), "PICK's range");
    CoopColoursMsg m = {.n = 2, .e = {{11, COOP_COLOUR_RED}, {12, COOP_COLOUR_GREEN}}};
    CHECK(coop_colours_pack(&m, out, sizeof out) == 20, "a COLOURS of two packs");
    CoopColoursMsg mb = m;
    mb.n = COOP_PLAYERS + 1;
    CHECK(!coop_colours_pack(&mb, out, sizeof out), "a COLOURS of 5 players packs");
    mb = m, mb.e[1].id = 0;
    CHECK(!coop_colours_pack(&mb, out, sizeof out), "a COLOURS of id 0 packs");
    mb = m, mb.e[0].colour = COOP_COLOURS;
    CHECK(!coop_colours_pack(&mb, out, sizeof out), "a COLOURS of colour 4 packs");
    mb = m, mb.e[1].id = 11;
    CHECK(!coop_colours_pack(&mb, out, sizeof out), "a COLOURS with one id twice packs");
    mb = m, mb.e[1].colour = COOP_COLOUR_RED;
    CHECK(!coop_colours_pack(&mb, out, sizeof out), "a COLOURS with one colour twice packs");

    CoopKeysMsg ky = {.serial = 7, .n = 2, .nack = 1, .d = {{0x7dd79d10, 3}, {0x90237092, 0}}, .ack = {{21, 5, 9}}};
    CHECK(coop_keys_pack(&ky, out, sizeof out) == 33, "a KEYS of two counts and one ack packs");
    CoopKeysMsg kx = ky;
    kx.n = COOP_KEYS_DUNGEONS + 1;
    CHECK(!coop_keys_pack(&kx, out, sizeof out), "a KEYS of 17 counts packs");
    kx = ky, kx.nack = COOP_PLAYERS + 1;
    CHECK(!coop_keys_pack(&kx, out, sizeof out), "a KEYS of 5 acks packs");
    kx = ky, kx.d[0].keys = COOP_KEYS_MAX + 1;
    CHECK(!coop_keys_pack(&kx, out, sizeof out), "a KEYS of 16 keys packs");
    kx = ky, kx.d[1].hash = 0;
    CHECK(!coop_keys_pack(&kx, out, sizeof out), "a KEYS of hash 0 packs");
    kx = ky, kx.d[1].hash = kx.d[0].hash;
    CHECK(!coop_keys_pack(&kx, out, sizeof out), "a KEYS with one counter twice packs");
    kx = ky, kx.ack[0].id = 0;
    CHECK(!coop_keys_pack(&kx, out, sizeof out), "a KEYS ack of id 0 packs");
    kx = ky, kx.ack[0].sess = 0;
    CHECK(!coop_keys_pack(&kx, out, sizeof out), "a KEYS ack of sess 0 packs");
    kx = ky, kx.nack = 2, kx.ack[1] = kx.ack[0];
    CHECK(!coop_keys_pack(&kx, out, sizeof out), "a KEYS with one player's ack twice packs");
    CHECK(!coop_keys_pack(&ky, out, 32), "a KEYS packs into too little room");
    CoopKeyMsg ke = {.sess = 5, .seq = 1, .hash = 0x7dd79d10, .delta = -1};
    CHECK(coop_key_pack(&ke, out, sizeof out) == COOP_KEY_MSG, "a KEY packs");
    CoopKeyMsg kf = ke;
    kf.delta = 0;
    CHECK(!coop_key_pack(&kf, out, sizeof out), "a KEY of no change packs");
    kf = ke, kf.delta = COOP_KEYS_MAX + 1;
    CHECK(!coop_key_pack(&kf, out, sizeof out), "a KEY of 16 keys packs");
    kf = ke, kf.delta = -COOP_KEYS_MAX - 1;
    CHECK(!coop_key_pack(&kf, out, sizeof out), "a KEY of -16 keys packs");
    kf = ke, kf.sess = 0;
    CHECK(!coop_key_pack(&kf, out, sizeof out), "a KEY of sess 0 packs");
    kf = ke, kf.seq = 0;
    CHECK(!coop_key_pack(&kf, out, sizeof out), "a KEY of seq 0 packs");
    kf = ke, kf.hash = 0;
    CHECK(!coop_key_pack(&kf, out, sizeof out), "a KEY of hash 0 packs");
    kf = ke, kf.flags = COOP_KEY_PLACE, kf.xyz[0] = -10.25f, kf.xyz[1] = 100.0f, kf.xyz[2] = -21.5f;
    CHECK(coop_key_pack(&kf, out, sizeof out) == COOP_KEY_MSG, "a KEY with a place packs");
    CoopKeyMsg kg = kf;
    kg.flags = 2;
    CHECK(!coop_key_pack(&kg, out, sizeof out), "a KEY of an unknown flag packs");
    kg = kf, kg.xyz[1] = NAN;
    CHECK(!coop_key_pack(&kg, out, sizeof out), "a KEY of a place that is not a number packs");
    kg = kf, kg.xyz[2] = 2e6f;
    CHECK(!coop_key_pack(&kg, out, sizeof out), "a KEY of a place too far packs");
    kg = kf, kg.flags = 0;
    CHECK(!coop_key_pack(&kg, out, sizeof out), "a KEY without its place flag but with a place packs");
    CHECK(!coop_key_pack(&kf, out, COOP_KEY_MSG - 1), "a KEY packs into too little room");
}

// ---- random

static void rnd_text(char *s, size_t max, int space, size_t lo) {
    size_t n = lo + rnd_below((uint32_t)(max - lo + 1));
    for (size_t i = 0; i < n; i++) s[i] = (char)((space ? 0x20 : 0x21) + rnd_below(space ? 95 : 94));
    s[n] = 0;
}
static float rnd_f(float max) { return ((float)rnd_below(2000001) / 1000000.0f - 1.0f) * max; }

// A random message of the kind, within its ranges, packed into out; its length (0 when the packer refused it).
static size_t rnd_message(int kind, uint8_t *out, size_t cap) {
    switch (kind) {
    case COOP_K_KILL:
    case COOP_K_DAMAGE: {
        CoopKillMsg m;
        memset(&m, 0, sizeof m);
        m.epoch = rnd(), m.eseq = rnd(), m.inst = ((uint64_t)rnd() << 32 | rnd()) | 1, m.name_hash = rnd();
        m.raw = (uint16_t)(1 + rnd_below(1000)), m.flags = (uint8_t)rnd_below(2);
        rnd_text(m.area, COOP_KILL_AREA_MAX, 0, 1);
        return coop_kill_pack((uint8_t)kind, &m, out, cap);
    }
    case COOP_K_CUTSCENE: {
        CoopCutMsg m;
        memset(&m, 0, sizeof m);
        m.op = (uint8_t)(1 + rnd_below(2)), m.flags = (uint8_t)rnd_below(4), m.epoch = rnd(), m.seq = rnd();
        rnd_text(m.event, COOP_CUT_EVENT_MAX, 0, 1), rnd_text(m.area, COOP_CUT_AREA_MAX, 0, 0);
        for (int i = 0; i < 3; i++) m.xyz[i] = rnd_f(1e6f);
        return coop_cut_pack(&m, out, cap);
    }
    case COOP_K_TIME: {
        CoopTimeMsg m = {rnd(), rnd(), rnd(), rnd_below(COOP_TIME_DAY), (uint8_t)rnd_below(2)};
        return coop_time_pack(&m, out, cap);
    }
    case COOP_K_ERA: {
        CoopEraMsg m;
        memset(&m, 0, sizeof m);
        m.epoch = rnd(), m.seq = rnd(), m.order = rnd(), m.flags = (uint8_t)rnd_below(8);
        rnd_text(m.scene, COOP_CUT_EVENT_MAX, 0, 0), rnd_text(m.scene_area, COOP_CUT_AREA_MAX, 0, 0), rnd_text(m.area, COOP_CUT_AREA_MAX, 0, 0);
        for (int i = 0; i < 3; i++) m.scene_xyz[i] = rnd_f(1e6f), m.xyz[i] = rnd_f(1e6f);
        return coop_era_pack(&m, out, cap);
    }
    case COOP_K_PUPPET: {
        CoopPuppetMsg m;
        memset(&m, 0, sizeof m);
        m.pseq = rnd(), m.t_ms = rnd(), m.flags = (uint8_t)rnd_below(4), m.upper = (uint8_t)rnd_below(8);
        m.nslots = (uint8_t)rnd_below(COOP_PUPPET_SLOTS + 1), m.gear_sig = rnd();
        rnd_text(m.area, COOP_PUPPET_TEXT, 1, 0);
        for (int i = 0; i < 3; i++) m.pos[i] = rnd_f(COOP_PUPPET_POS_MAX);
        m.yaw = rnd_f(COOP_PUPPET_YAW_MAX);
        for (int i = 0; i < m.nslots; i++) rnd_text(m.slot[i].node, COOP_PUPPET_TEXT, 1, 0), m.slot[i].frame = rnd_f(COOP_PUPPET_FRAME_MAX);
        return coop_puppet_pack(&m, out, cap);
    }
    case COOP_K_GEAR: {
        CoopGear g;
        memset(&g, 0, sizeof g);
        for (int j = 0; j < COOP_GEAR_DYN; j++)
            if (rnd_below(2)) rnd_text(g.dyn[j], COOP_PUPPET_TEXT, 1, 1), g.state[j] = (uint8_t)rnd_below(3);
        for (int s = 0; s < COOP_GEAR_STAT; s++)
            if (rnd_below(2)) rnd_text(g.stat[s], COOP_PUPPET_TEXT, 1, 1);
        return coop_gear_pack(&g, out, cap);
    }
    case COOP_K_PICK: return coop_pick_pack(rnd_below(5) == 4 ? COOP_COLOUR_NONE : (uint8_t)rnd_below(COOP_COLOURS), out, cap);
    case COOP_K_COLOURS: {
        CoopColoursMsg m;
        memset(&m, 0, sizeof m);
        m.n = (uint8_t)rnd_below(COOP_PLAYERS + 1);
        uint8_t order[COOP_COLOURS] = {0, 1, 2, 3};
        for (int i = COOP_COLOURS - 1; i > 0; i--) {
            int j = (int)rnd_below((uint32_t)i + 1);
            uint8_t x = order[i];
            order[i] = order[j], order[j] = x;
        }
        for (int i = 0; i < m.n; i++) m.e[i].id = ((uint64_t)rnd() << 32 | rnd()) + (uint64_t)i * 0x1000000001ull + 1, m.e[i].colour = order[i];
        for (int i = 1; i < m.n; i++)
            if (m.e[i].id == m.e[0].id) m.e[i].id++;
        return coop_colours_pack(&m, out, cap);
    }
    case COOP_K_KEYS: {
        CoopKeysMsg m;
        memset(&m, 0, sizeof m);
        m.serial = rnd(), m.n = (uint8_t)rnd_below(COOP_KEYS_DUNGEONS + 1), m.nack = (uint8_t)rnd_below(COOP_PLAYERS + 1);
        for (int i = 0; i < m.n; i++) {   // distinct hashes, none 0
            uint32_t h = rnd();
            for (int again = 1; again; h += again) {
                again = !h;
                for (int j = 0; j < i; j++) again |= m.d[j].hash == h;
            }
            m.d[i].hash = h, m.d[i].keys = (uint8_t)rnd_below(COOP_KEYS_MAX + 1);
        }
        for (int i = 0; i < m.nack; i++)
            m.ack[i].id = ((uint64_t)rnd() << 32 | rnd()) + (uint64_t)i * 0x1000000001ull + 1, m.ack[i].sess = rnd() | 1, m.ack[i].seq = rnd();
        for (int i = 1; i < m.nack; i++)
            if (m.ack[i].id == m.ack[0].id) m.ack[i].id++;
        return coop_keys_pack(&m, out, cap);
    }
    case COOP_K_KEY: {
        int d = (int)rnd_below(2 * COOP_KEYS_MAX) - COOP_KEYS_MAX;   // -15..14, then 0 is 15
        CoopKeyMsg m = {rnd() | 1, rnd() | 2, rnd() | 4, (int8_t)(d ? d : COOP_KEYS_MAX), (uint8_t)rnd_below(2), {0, 0, 0}};
        for (int i = 0; m.flags && i < 3; i++) m.xyz[i] = rnd_f(1e6f);
        return coop_key_pack(&m, out, cap);
    }
    }
    return 0;
}

static long probe_random(void) {
    long cases = 0, parsed = 0;
    uint8_t b[COOP_MESSAGE_MAX + 8], c[COOP_MESSAGE_MAX + 8];
    // random messages: pack, parse, pack again
    for (int kind = COOP_K_KILL; kind < COOP_K_EXT_END; kind++)
        for (int i = 0; i < 2000; i++, cases++) {
            size_t n = rnd_message(kind, b, sizeof b);
            CHECK(n, "a random %s within its ranges does not pack", coop_ext_kind_name((uint8_t)kind));
            if (n) CHECK(round_trip(kind, b, n, "random message"), "a random %s does not parse", coop_ext_kind_name((uint8_t)kind));
        }
    // the vectors changed at random
    for (int v = 0; v < nvec; v++)
        for (int i = 0; i < 3000; i++, cases++) {
            memcpy(c, vec[v].b, vec[v].n);
            for (int k = 1 + (int)rnd_below(3); k > 0; k--) c[rnd_below((uint32_t)vec[v].n)] = (uint8_t)rnd();
            parsed += round_trip(vec[v].kind, c, vec[v].n, "a changed vector");
        }
    // random bytes, mostly short, some as long as a message gets
    for (int kind = COOP_K_KILL; kind < COOP_K_EXT_END; kind++)
        for (int i = 0; i < 20000; i++, cases++) {
            size_t n = rnd_below(8) ? rnd_below(64) : rnd_below(COOP_MESSAGE_MAX + 1);
            for (size_t k = 0; k < n; k++) b[k] = (uint8_t)rnd();
            if (n && rnd_below(2)) b[0] = 1;   // the version most of the kinds want
            parsed += round_trip(kind, b, n, "random bytes");
        }
    printf("probe-coop-ext: %ld random cases, %ld of the changed or random ones parsed (and packed back the same)\n", cases, parsed);
    return cases;
}

// ---- the PUPPET counter, the names

static void probe_fresh(void) {
    CoopPosSeen s;
    memset(&s, 0, sizeof s);
    CHECK(coop_pos_fresh(&s, 1, 10, 1000) == 2, "the first pose is taken and starts the counter");
    CHECK(coop_pos_fresh(&s, 1, 11, 1066) == 1, "a newer pose is taken");
    CHECK(coop_pos_fresh(&s, 1, 11, 1132) == 0, "the same pose again is dropped");
    CHECK(coop_pos_fresh(&s, 1, 5, 1132) == 0, "an older pose is dropped");
    CHECK(coop_pos_fresh(&s, 2, 1, 1200) == 2, "the first pose of another gen starts again");
    CHECK(coop_pos_fresh(&s, 2, 1, 1200 + COOP_POS_QUIET_MS) == 2, "after a quiet time any pose starts again");
    s.last = 0xffffffffu;
    CHECK(coop_pos_fresh(&s, 2, 0, 5000) == 1, "the counter's wrap is newer");
    CHECK(!strcmp(coop_ext_kind_name(COOP_K_KILL), "KILL") && !strcmp(coop_ext_kind_name(COOP_K_COLOURS), "COLOURS") &&
          !strcmp(coop_ext_kind_name(COOP_K_KEYS), "KEYS") && !strcmp(coop_ext_kind_name(COOP_K_KEY), "KEY") &&
          !strcmp(coop_ext_kind_name(COOP_K_EXT_END), "?") && !strcmp(coop_ext_kind_name(0x3f), "?"), "the kinds' names");
}

int probe_coop_ext(void) {
    int messages, refusals;
    probe_vectors(&messages, &refusals);
    if (messages) printf("probe-coop-ext: %d messages and %d refusals of the vectors file checked\n", messages, refusals);
    probe_packers();
    probe_random();
    probe_fresh();
    printf("probe-coop-ext: %s\n", fails ? "FAIL" : "ok");
    return fails ? 1 : 0;
}
