// Co-op puppets, game 1.1.1: the other players of a co-op room shown in this game as Links of their own (in progress).
//
// The hook. The framework's per-frame update (main+0x6e6be0) steps the time manager (main+0x3b4e300) before it updates
// the scene. The patch set "coop_puppet" (recomp/patches-v111.txt) turns that bl into a call of puppet_frame on the patch
// page (+0xf00), which makes the call itself and then does the puppets' work on the game's update thread, between two
// frames: spawning and removing actors, posing them, asking for their animations.
//
// Actors. The actor manager *(main+0x4e91888) takes create requests (main+0x372a70): x1 the address of the actor
// name's pointer ("Pack/Actor/<name>.pack" is loaded), x2 the placement (+0x00 position, +0x0c a 3x3 rotation, +0x30
// scale, +0x40 a text, +0x48 a byte, +0x98 a mode byte), x3 a cell the game puts the request into (a 0x30-byte pool
// entry: +0 its state in the low 16 bits, +8 the actor), w4 1, x5..x7 0, then on the stack a byte 0, a u32 for the
// outcome, and 0. The actor is made (actor state 3) but is in no list of the actor system: when the request reaches
// state 3 its asker must take the actor (main+0x36fbd0(request, 1, 0), as the game's own askers do at main+0x32bcb88
// and main+0xbcd89c). The take wakes it (main+0x94f30: flags at actor +0x30, then main+0xb3db0 links it into the
// actor system's list at +0x3e8), and from that list it enters the scene (state 4) and is updated and drawn; the
// request is the game's again. An actor never taken stays in state 3 for good, not drawn. A request the game gives up
// (state 4) is dropped (main+0x36fde0), one that failed (5, 6) released. A taken actor is removed with the game's
// generic delete (main+0xa6490(actor, 0), the call behind the AI's "delete this actor"); a request not yet made is
// cancelled in the pending queue (main+0xb2e020 over *(main+0x4e90d90)+0x458, with a matcher whose vtable is
// main+0x4c595e0, then main+0xb2e6e0 frees it), else main+0xb2e2e0(request, 1) deletes its actor.
//
// Posing: the engine's transform setter main+0xc6fd60(actor, 3x4 matrix, 0) hands the transform to the actor's physics
// body or keeps it for the actor's next update (+0x358, +0x364); the actor's own copies (+0x31c, +0x328) are written
// as well so that a read in the same frame sees it.
//
// Status (game 1.1.1, run-checked): a pot and the puppet (CoopPuppet_Child / _Adult, tools/make_puppet_mod.py) are made,
// taken and drawn where they were asked for, the puppet plays Link's nodes and follows a pose, and two games in one
// room each make the other's Link as a puppet. The other players' puppets stay off unless NX2NAVI_COOP_PUPPETS is set.
// Arms: the puppet's arms no longer stay in the bind pose (A-pose): its AS slot 1 (Upper) plays
// Link's slot-0 node at slot 0's frame while Link's own slot 1 is empty (NX2NAVI_COOP_PUPPET_ARMS, default upper;
// they hang and swing as Link's). Gear: the shadow puppet is
// dressed as Link is, through the game's own EquipmentUser calls (puppet_dress, NX2NAVI_COOP_PUPPET_GEAR). Colours:
// each Link's tunic and cap glow faintly in his player's colour over the darkened fabric, the
// local Link's own included (section "colours", NX2NAVI_COOP_COLOUR_SELF / _TEST; off until a colour is set).
// Remote players: POS v3 carries each player's gear, colour, pick, join place
// and empty upper slots, so that the other players' puppets get gear, colours and arms as the shadow test's does; the
// room gives every member its own colour (section "room colours": NX2NAVI_COOP_COLOUR picks, NX2NAVI_COOP_COLOURS=0 off).
// NX2NAVI_COOP_PUPPET_TEST=spike|shadow are the one-game tests.
// Kills, shared damage and difficulty live in coop_kill.c: this file's frame
// hook hands it the area and Link (coop_kill_frame), its present switch runs it (coop_kill_present) and keeps the hook
// on while it needs it, and coop_puppet_world_others tells it the members in the game world (from their POS).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "coop.h"
#include "coop_offer.h"
#include "coop_puppet.h"
#include "coop_kill.h"
#include "coop_cutscene.h"
#include "coop_tags.h"
#include "host.h"
#include "platform/platform.h"

#if __has_include("patches.h")
#include "patches.h"
#endif

#ifdef RT_PATCH_PAGE

#define MAIN 0x7000000000ull
#define HOST_FRAME (RT_PATCH_PAGE + 0xf00)
#define TIME_STEP (MAIN + 0x3b4e300)        // the call the site replaces: the time manager's step (x0, s0)
#define ACTOR_MGR (MAIN + 0x4e91888)        // *: the actor manager
#define CREATE_FN (MAIN + 0x372a70)         // the create request
#define ACTOR_SYS (MAIN + 0x4e90d90)        // *: +0x458 the queue of pending creates
#define QUEUE_FIND (MAIN + 0xb2e020)        // (queue, matcher): a pending create the matcher names is taken out (1)
#define MATCH_VTBL (MAIN + 0x4c595e0)       // the matcher: {vtable, address of the request cell}
#define HANDLE_FREE (MAIN + 0xb2e6e0)       // (request): a request whose create never ran goes back to its pool
#define HANDLE_DELETE (MAIN + 0xb2e2e0)     // (request, 1): the actor the request holds is deleted
#define CREATE_TAKE (MAIN + 0x36fbd0)       // (request, 1, 0) -> the actor, woken into the scene
#define CREATE_DROP (MAIN + 0x36fde0)       // (request): a request in state 4 is given up
#define ACTOR_DELETE (MAIN + 0xa6490)       // (actor, 0): the game's generic delete of an actor
#define ACTOR_SET_MTX (MAIN + 0xc6fd60)     // (actor, 3x4 row-major matrix, 0): the engine's transform setter
#define EMPTY_TEXT (MAIN + 0x3ee4f8c)       // "" in main's read-only data
#define PLAYER_HOLDER (MAIN + 0x4e371d0)    // *: +0x480 Link's actor
#define AS_PLAY (MAIN + 0x2022a0)           // (request): an AS node by name on an actor's AS slot
#define AS_FRAME_GET (MAIN + 0x37b57c0)     // (AS control, slot) -> s0
#define AS_FRAME_SET (MAIN + 0x37b5d70)     // (AS control, slot, s0)
#define AS_SET_FOLLOW (MAIN + 0x37b6200)    // (AS control, slot, followee slot) -> 1: entry(slot)+0xc9 = followee
#define FLAGS_MGR (MAIN + 0x4e842a8)        // *: the game's flag database (coop_game.c)

// ---- arms. The Player ASB
// (AS/Player_<age>.root.asb, also Dark Link's) has 9 partial slots: slot table at header +0x28 (16-byte rows {name,
// bone group, root bone, 0}), bone groups at +0x5c (12-byte rows {bone list, name, count}; bone rows {name, u16
// exclude, u16 recursive}).
//   slot 0 Lower    group 7  +Root* +Skl_Root* -Weapon_Root* -Spine_1* -Spine_2* -Face_Root*   hips and legs only
//   slot 1 Upper    group 8  -Root* +Weapon_Root* +Spine_1* +Spine_2* -Face_Root*             torso, arms, head
//   slot 2 RightArm group 9  -Root* +Clavicle_R*          slot 3 LeftArm group 10 -Root* +Clavicle_L*
//   slot 4 Face follows slot 0 (ASInfo PartialConfigs, set up through AS_SET_FOLLOW)
// Link's full-body nodes (Wait, Move: their FSKAs animate Clavicle/Arm/Wrist) are asked for on slot 0 only. Link's
// slot 1 is empty then too, but his was released by the player code after an Upper node; the puppet's slot 1 has never
// played anything, and in that state its group stays in the bind pose (A-pose arms). Seen in a test of the gear
// (full-size screenshots): A-pose while slot 1 was never used, natural arms once slot 1 had played
// WeaponEquipOffUpper (held at its last frame: static arms, no swing). Hence: drive slot 1 with Link's slot-0 node.
// NX2NAVI_COOP_PUPPET_ARMS picks how:
//   upper  (default) when Link's slot 1 is empty, the puppet's slot 1 plays Link's slot-0 node at slot 0's frame
//   upper3 the same for slots 1-3 (RightArm, LeftArm too)
//   all    as upper, and slot-0 nodes are asked for with the request's IsAllSlot byte (+0x21: main+0x2022a0 then
//          plays the node on every slot through main+0x202610, the path event playback uses)
//   follow slot 1 is set once to follow slot 0 (AS_SET_FOLLOW, as the AS set-up does for Face -> Lower and the
//          player code at main+0x2680d70 does at run time)
//   0      as before (slot 0 only)
enum { ARMS_OFF, ARMS_UPPER, ARMS_UPPER3, ARMS_ALL, ARMS_FOLLOW };
static const char *const arms_names[] = {"0", "upper", "upper3", "all", "follow"};

// Actor layout
#define A_STATE 0x2c        // u32: 3 made, 4 in the scene
#define A_SLEEP 0x30        // u32: flags the take sets while the actor waits in the actor system's list (0 in the scene)
#define A_DEF_NAME 0x260    // char *: the actor pack's name
#define A_COMPONENTS 0x270  // -> +0x40 -> +0x18 -> +0x18: the AS control
#define A_POS 0x31c         // float[3]
#define A_ROT 0x328         // float[9], row-major
#define A_POS2 0x2ec        // a second copy of the transform
#define A_ROT2 0x2f8

// The String64 flag of the current area: container +0x608 of the flag database, entries of 0x108 bytes, text at +0x68.
#define S64_BASE 0x608
#define S64_SIZE 0x108
#define S64_TEXT 0x68
#define F_CURRENT_AREA 0x1d6189dau   // Sequence_CurrentBanc

typedef struct {
    uint64_t name;          // char * of the actor's name: the request's x1 points here
    uint64_t handle;        // the create request (x3 points here) until its actor is taken
    uint32_t status;        // the request's outcome
    uint32_t pad;
    uint8_t place[0xa0];    // the placement (x2)
    uint64_t matcher[2];    // for the release
    uint64_t actor;         // the actor once taken
    char text[64];          // the name's bytes
} __attribute__((aligned(16))) Spawn;

// The local Link's pose of one frame, for the shadow test: transform and, per AS slot, the node it plays and its frame.
#define SLOTS 4
typedef struct {
    float pos[3], rot[9];
    int nslots;
    char node[SLOTS][64];
    float frame[SLOTS];
} Pose;
#define DELAY 60      // frames: the shadow is the local Link of one second ago
#define RING 64

static struct {
    int test;               // NX2NAVI_COOP_PUPPET_TEST: 1 spike, 2 shadow
    int remote;             // NX2NAVI_COOP_PUPPETS: the other players' puppets
    unsigned cfg_seen;
    int set;                // the patch set's index
    int want;               // the present thread's verdict: the hook has work
    // the game thread's
    uint32_t frames, player_frames;
    uint64_t player;
    char area[80];
    char anim[4][64], menu_anim[4][64];
    Spawn spawn[2];
    uint64_t actor[2];
    int sp_state[2], played;
    uint32_t asked_frame[2];
    // the shadow test
    char shadow_name[64];   // "": CoopPuppet_<Link's age>
    float side;             // metres to the left of where Link was
    int bb_copy;            // test: copy Link's AS blackboard to the shadow (NX2NAVI_COOP_PUPPET_BB=1; default off)
    int arms;               // test: ARMS_* (NX2NAVI_COOP_PUPPET_ARMS, default upper)
    Pose ring[RING];
} P;

static int ok_ptr(uint64_t a, uint64_t n) { return a >= 0x10000 && a < (1ull << 48) && mem_readable(a, n); }

static float f32_at(uint64_t a) {
    float f;
    memcpy(&f, (const void *)(uintptr_t)a, 4);
    return f;
}

// A C string of the guest at a (at most cap - 1 bytes); "" when a is not readable.
static const char *text_at(uint64_t a, char *out, size_t cap) {
    out[0] = 0;
    if (!ok_ptr(a, 1)) return out;
    size_t i = 0;
    for (; i + 1 < cap && ok_ptr(a + i, 1); i++) {
        char ch = (char)LD8(a + i);
        if (!ch) break;
        out[i] = ch >= 0x20 && ch < 0x7f ? ch : '?';
    }
    out[i] = 0;
    return out;
}

// Seconds since Link's actor was found (frames at 60 per second): the world clock (frame_step_world_us) does not run
// on 1.1.1 in this branch (frame_rate.c player_pos reads 1.2.1's actor list), so the spike times itself.
static double wsec(void);

static double wsec(void) { return P.player ? P.player_frames / 60.0 : -1.0; }

static uint64_t player_actor(void) {
    uint64_t h = LD64(PLAYER_HOLDER);
    if (!ok_ptr(h, 0x488)) return 0;
    uint64_t a = LD64(h + 0x480);
    return ok_ptr(a, 0x400) ? a : 0;
}

static uint64_t as_control(uint64_t actor) {
    uint64_t p = LD64(actor + A_COMPONENTS);
    if (!ok_ptr(p, 0x48)) return 0;
    p = LD64(p + 0x40);
    if (!ok_ptr(p, 0x20)) return 0;
    p = LD64(p + 0x18);
    if (!ok_ptr(p, 0x20)) return 0;
    p = LD64(p + 0x18);
    return ok_ptr(p, 0x140) ? p : 0;
}

static int as_slots(uint64_t asc) { return asc ? (int)LD32(asc + 0x18) : 0; }

static uint64_t as_slot(uint64_t asc, int slot) {
    if (!asc || slot < 0 || slot >= as_slots(asc)) return 0;
    uint64_t arr = LD64(asc + 0x20);
    if (!ok_ptr(arr + 0x48ull * (unsigned)slot, 8)) return 0;
    uint64_t e = LD64(arr + 0x48ull * (unsigned)slot);
    return ok_ptr(e, 0x110) ? e : 0;
}

static float call_f(Cpu *c, uint64_t fn, uint64_t a0, uint64_t a1) {
    rt_call(c, fn, 2, a0, a1);
    float f;
    uint32_t bits = (uint32_t)c->v[0];
    memcpy(&f, &bits, 4);
    return f;
}

static const char *area_now(char *out, size_t cap) {
    out[0] = 0;
    uint64_t mgr = LD64(FLAGS_MGR);
    if (!ok_ptr(mgr, 0xd80)) return out;
    uint64_t cn = mgr + S64_BASE, table = LD64(cn + 0x18), entries = LD64(cn + 0x10);
    uint32_t cap_n = LD32(cn + 0x24), count = LD32(cn + 0x08);
    if (!cap_n || !ok_ptr(table, 8ull * cap_n) || !entries) return out;
    for (uint32_t i = F_CURRENT_AREA % cap_n, k = 0; k < cap_n; k++, i = i + 1 == cap_n ? 0 : i + 1) {
        uint32_t h = LD32(table + 8ull * i);
        if (h == F_CURRENT_AREA) {
            uint32_t idx = LD32(table + 8ull * i + 4);
            if (idx >= count) return out;
            uint64_t e = entries + (uint64_t)idx * S64_SIZE;
            if (ok_ptr(e, S64_SIZE)) text_at(LD64(e + S64_TEXT), out, cap);
            return out;
        }
        if (!h) return out;
    }
    return out;
}

// ---- create and release

static int spawn_request(Cpu *c, Spawn *s, const char *name, const float pos[3], const float rot[9]) {
    memset(s, 0, sizeof *s);
    snprintf(s->text, sizeof s->text, "%s", name);
    s->name = (uint64_t)(uintptr_t)s->text;
    memcpy(s->place + 0x00, pos, 12);
    memcpy(s->place + 0x0c, rot, 36);
    const float one[3] = {1, 1, 1};
    memcpy(s->place + 0x30, one, 12);
    uint64_t empty = EMPTY_TEXT;
    memcpy(s->place + 0x40, &empty, 8);
    // +0x48 and +0x98 as the AI's create node sets them for an actor it makes in play (main+0x32bca60..0x32bcaa0):
    // +0x48 a byte 0 (with 1 the asker leaves the actor asleep when it takes it; other callers store 0x10000 or
    // 0x10001) and +0x98 1
    s->place[0x98] = 1;
    uint64_t sp = c->sp;
    c->sp = (sp - 0x40) & ~15ull;
    ST64(c->sp + 0x00, 0);
    ST64(c->sp + 0x08, (uint64_t)(uintptr_t)&s->status);
    ST64(c->sp + 0x10, 0);
    uint64_t mgr = LD64(ACTOR_MGR);
    uint64_t r = rt_call(c, CREATE_FN, 8, mgr, (uint64_t)(uintptr_t)&s->name, (uint64_t)(uintptr_t)s->place,
                         (uint64_t)(uintptr_t)&s->handle, 1ull, 0ull, 0ull, 0ull);
    c->sp = sp;
    rt_log("coop: puppet: w=%.2f create %s at %.2f %.2f %.2f: returned %llu, outcome %u, handle %#llx (manager %#llx, thread %u)",
           wsec(), name, pos[0], pos[1], pos[2], (unsigned long long)(r & 1), s->status,
           (unsigned long long)s->handle, (unsigned long long)mgr, plat_thread_id());
    return (int)(r & 1);
}

static void spawn_release(Cpu *c, Spawn *s) {
    if (s->actor) {
        uint64_t a = s->actor;
        uint32_t st = LD32(a + A_STATE);
        uint64_t r = rt_call(c, ACTOR_DELETE, 2, a, 0ull) & 1;
        rt_log("coop: puppet: w=%.2f release %s: delete actor %#llx -> %llu (state %u -> %u)", wsec(), s->text,
               (unsigned long long)a, (unsigned long long)r, st, LD32(a + A_STATE));
        s->actor = 0;
        return;
    }
    if (!s->handle) return;
    uint64_t sys = LD64(ACTOR_SYS);
    s->matcher[0] = MATCH_VTBL;
    s->matcher[1] = (uint64_t)(uintptr_t)&s->handle;
    uint64_t pending = rt_call(c, QUEUE_FIND, 2, LD64(sys + 0x458), (uint64_t)(uintptr_t)s->matcher) & 1;
    uint64_t h = s->handle;
    uint32_t st = LD32(h);
    if (pending) rt_call(c, HANDLE_FREE, 1, h);
    else rt_call(c, HANDLE_DELETE, 2, h, 1ull);
    rt_log("coop: puppet: w=%.2f release %s: %s (request state %#x -> %#x)", wsec(), s->text,
           pending ? "the create was still pending" : "its actor is deleted", st, LD32(h));
    s->handle = 0;
}

enum { SPAWN_FAILED = -1, SPAWN_PENDING = 0, SPAWN_MADE = 1 };

// Follows a create request: once it is ready (state 3) the actor is taken as the game's askers take theirs, which
// puts it in the actor system's list and so into the scene; the request is then forgotten. SPAWN_MADE with s->actor
// set, SPAWN_PENDING, or SPAWN_FAILED
// (given up by the game, or failed; nothing is left to release).
static int spawn_take(Cpu *c, Spawn *s) {
    if (s->actor) return SPAWN_MADE;
    if (!s->handle || !ok_ptr(s->handle, 0x30)) return SPAWN_FAILED;
    uint64_t req = s->handle;
    uint32_t st = LD32(req) & 0xffff;
    if (st == 3) {
        uint64_t made = LD64(req + 8);
        uint32_t sleep_was = ok_ptr(made, 0x400) ? LD32(made + A_SLEEP) : 0;
        uint64_t a = rt_call(c, CREATE_TAKE, 3, req, 1ull, 0ull);
        s->handle = 0;
        s->actor = ok_ptr(a, 0x400) ? a : 0;
        rt_log("coop: puppet: w=%.2f %s taken from request %#llx: actor %#llx, state %u, +0x30 %#x -> %#x", wsec(),
               s->text, (unsigned long long)req, (unsigned long long)a, s->actor ? LD32(a + A_STATE) : 0, sleep_was,
               s->actor ? LD32(a + A_SLEEP) : 0);
        return s->actor ? SPAWN_MADE : SPAWN_FAILED;
    }
    if (st == 4) {
        rt_call(c, CREATE_DROP, 1, req);
        s->handle = 0;
        rt_log("coop: puppet: w=%.2f %s: the game gave the create up (request state 4)", wsec(), s->text);
        return SPAWN_FAILED;
    }
    if (st == 5 || st == 6) {
        rt_log("coop: puppet: w=%.2f %s: the create failed (request state %u)", wsec(), s->text, st);
        spawn_release(c, s);
        return SPAWN_FAILED;
    }
    return SPAWN_PENDING;
}

// Puts an actor at pos turned as rot (row-major) through the engine's setter, and into its own copies.
static void put_actor(Cpu *c, uint64_t a, const float pos[3], const float rot[9]) {
    static float m[12] __attribute__((aligned(16)));
    for (int r = 0; r < 3; r++) {
        memcpy(m + 4 * r, rot + 3 * r, 12);
        m[4 * r + 3] = pos[r];
    }
    rt_call(c, ACTOR_SET_MTX, 3, a, (uint64_t)(uintptr_t)m, 0ull);
    memcpy((void *)(uintptr_t)(a + A_POS), pos, 12);
    memcpy((void *)(uintptr_t)(a + A_ROT), rot, 36);
    memcpy((void *)(uintptr_t)(a + A_POS2), pos, 12);
    memcpy((void *)(uintptr_t)(a + A_ROT2), rot, 36);
}

// ---- the spike (NX2NAVI_COOP_PUPPET_TEST=spike): actors of the game two metres in front of Link, left standing there

static void place_ahead(uint64_t player, float dist, float pos[3], float rot[9]) {
    float m[9];
    memcpy(m, (const void *)(uintptr_t)(player + A_ROT), sizeof m);
    memcpy(pos, (const void *)(uintptr_t)(player + A_POS), 12);
    pos[0] += m[2] * dist, pos[1] += m[5] * dist, pos[2] += m[8] * dist;
    memcpy(rot, m, sizeof m);
}

static void log_as(Cpu *c, const char *who, uint64_t actor, int always, char (*seen)[64]) {
    uint64_t asc = as_control(actor);
    int n = as_slots(asc);
    for (int i = 0; i < n && i < 4; i++) {
        uint64_t e = as_slot(asc, i);
        char name[64];
        text_at(e ? LD64(e + 0x10) : 0, name, sizeof name);
        int changed = strcmp(name, seen[i]) != 0;
        if (!changed && !always) continue;
        snprintf(seen[i], sizeof seen[i], "%s", name);
        float fr = call_f(c, AS_FRAME_GET, asc, (uint64_t)i);
        rt_log("coop: puppet: w=%.2f %s AS %#llx slot %d/%d '%s' frame %.2f (+0xd4 %.3f +0xe8 %.3f +0x10c %.3f)",
               wsec(), who, (unsigned long long)asc, i, n, name, fr, f32_at(e + 0xd4),
               f32_at(e + 0xe8), f32_at(e + 0x10c));
    }
}

// Asks the actor's AS for the node name on slot (main+0x2022a0), as the game's own callers do: rate 1, no start frame,
// blend frames (-1: the node's own), and nothing when the slot plays that node already. 1 when the AS took it.
// Request layout (main+0x2022a0): +0x00 actor, +0x08 name, +0x10 slot, +0x14 rate (to entry+0xd4), +0x18 start frame
// (< 0: none), +0x1c blend, +0x20 1 = skip when the slot already plays the name, +0x21 1 = IsAllSlot (main+0x202610
// plays the node on every slot through main+0x2028f0), +0x22 a mode byte (3 when the AS control's +0x130 is 2).
static int as_play(Cpu *c, uint64_t actor, const char *name, int slot, float blend, int all_slots) {
    static char keep[64];
    static uint8_t req[0x30] __attribute__((aligned(16)));
    snprintf(keep, sizeof keep, "%s", name);
    memset(req, 0, sizeof req);
    uint64_t text = (uint64_t)(uintptr_t)keep;
    float rate = 1, start = -1;
    uint32_t s = (uint32_t)slot;
    memcpy(req + 0x00, &actor, 8);
    memcpy(req + 0x08, &text, 8);
    memcpy(req + 0x10, &s, 4);
    memcpy(req + 0x14, &rate, 4);
    memcpy(req + 0x18, &start, 4);
    memcpy(req + 0x1c, &blend, 4);
    req[0x20] = 1;
    req[0x21] = all_slots ? 1 : 0;
    return (int)(rt_call(c, AS_PLAY, 1, (uint64_t)(uintptr_t)req) & 1);
}

// The seconds of the spike: the first actor (a pot) is asked for at 4 s and released at 20 s; the second (the pause
// menu's Link unless named) is asked for at 23 s, asked for two AS nodes at 30 s and released at 45 s. Each stands
// where it was asked for; its place and state are logged every second.
static char spike_names[2][64] = {"Obj_Pot_A_01", "PouchPlayer_Child"};   // spike:<first>:<second> names others
enum { SP_WAIT, SP_ASKED, SP_FOLLOW, SP_GONE };

static void spike_one(Cpu *c, uint64_t player, int k, double w, double ask, double play_at, double end) {
    Spawn *s = &P.spawn[k];
    int *st = &P.sp_state[k];
    uint64_t a = P.actor[k];
    if (*st == SP_WAIT) {
        if (w < ask) return;
        float pos[3], rot[9];
        place_ahead(player, 2.0f, pos, rot);
        spawn_request(c, s, spike_names[k], pos, rot);
        *st = SP_ASKED, P.asked_frame[k] = P.player_frames;
        return;
    }
    uint32_t in = P.player_frames - P.asked_frame[k];
    if (*st == SP_ASKED) {
        int r = spawn_take(c, s);
        if (r == SPAWN_MADE) {
            a = P.actor[k] = s->actor;
            char n1[64];
            rt_log("coop: puppet: w=%.2f %s made after %u frames: actor %#llx, instance name '%s', state %u, "
                   "pos %.2f %.2f %.2f", wsec(), s->text, in, (unsigned long long)a,
                   text_at(LD64(a + 0x20), n1, sizeof n1), LD32(a + A_STATE), f32_at(a + A_POS),
                   f32_at(a + A_POS + 4), f32_at(a + A_POS + 8));
            *st = SP_FOLLOW;
        } else if (r == SPAWN_FAILED || w >= end) {
            rt_log("coop: puppet: w=%.2f %s not made after %u frames", wsec(), s->text, in);
            spawn_release(c, s);
            *st = SP_GONE;
        }
        return;
    }
    if (*st == SP_FOLLOW) {
        if (!ok_ptr(a, 0x400) || (LD32(a + A_STATE) != 4 && LD32(a + A_STATE) != 3)) {
            rt_log("coop: puppet: w=%.2f %s's actor gone (state %u)", wsec(), s->text, ok_ptr(a, 0x400) ? LD32(a + A_STATE) : 0);
            *st = SP_GONE;
            return;
        }
        if (in % 60 == 0) {
            rt_log("coop: puppet: w=%.2f %s: state %u, +0x30 %#x, at %.2f %.2f %.2f; Link at %.2f %.2f %.2f", wsec(),
                   s->text, LD32(a + A_STATE), LD32(a + A_SLEEP), f32_at(a + A_POS), f32_at(a + A_POS + 4),
                   f32_at(a + A_POS + 8), f32_at(player + A_POS), f32_at(player + A_POS + 4), f32_at(player + A_POS + 8));
            if (k) log_as(c, "spike", a, 1, P.menu_anim);
        }
        if (k && !P.played && w >= play_at) {
            // the pause menu Link's AS knows its own nodes, the puppet's Link's: ask one of each
            P.played = 1;
            int r1 = as_play(c, a, "PouchWait", 0, -1.0f, 0);
            int r2 = as_play(c, a, P.anim[0], 0, -1.0f, 0);
            rt_log("coop: puppet: w=%.2f %s: play 'PouchWait' -> %d, play '%s' -> %d", wsec(), s->text, r1, P.anim[0], r2);
        }
        if (w >= end) {
            spawn_release(c, s);
            *st = SP_GONE, P.asked_frame[k] = P.player_frames;
        }
        return;
    }
    if (*st == SP_GONE && a && in < 120 && in % 10 == 0)
        rt_log("coop: puppet: w=%.2f after the release of %s: readable %d, state %u", wsec(), s->text,
               ok_ptr(a, 0x400), ok_ptr(a, 0x400) ? LD32(a + A_STATE) : 0);
}

static void spike(Cpu *c, uint64_t player) {
    double w = wsec();
    if (w < 0) return;
    spike_one(c, player, 0, w, 4, 0, 20);
    if (P.sp_state[0] == SP_GONE) spike_one(c, player, 1, w, 23, 30, 45);
}

// ---- a puppet: an actor this file asked for and poses every frame

typedef struct {
    Spawn s;
    int state;              // SP_WAIT (none yet), SP_ASKED, SP_FOLLOW (made: posed every frame), SP_GONE
    uint64_t actor;
    uint32_t asked_frame;   // P.frames at the request or the release
    char asked[SLOTS][64];  // the node each AS slot was asked for
    int play_ok[SLOTS];
    uint32_t seen_state;    // the actor's state last logged
    int bb_info;            // test: the blackboard info lines were logged for this puppet
    int arms_done;          // test: ARMS_FOLLOW was set up for this actor
} Puppet;

static void pose_of(Cpu *c, uint64_t actor, Pose *p) {
    memcpy(p->pos, (const void *)(uintptr_t)(actor + A_POS), 12);
    memcpy(p->rot, (const void *)(uintptr_t)(actor + A_ROT), 36);
    uint64_t asc = as_control(actor);
    int n = as_slots(asc);
    p->nslots = n < SLOTS ? n : SLOTS;
    for (int i = 0; i < p->nslots; i++) {
        uint64_t e = as_slot(asc, i);
        text_at(e ? LD64(e + 0x10) : 0, p->node[i], sizeof p->node[i]);
        p->frame[i] = e ? call_f(c, AS_FRAME_GET, asc, (uint64_t)i) : 0;
    }
}

static void as_frame_set(Cpu *c, uint64_t asc, int slot, float f) {
    uint32_t bits;
    memcpy(&bits, &f, 4);
    c->v[0] = bits;
    rt_call(c, AS_FRAME_SET, 2, asc, (uint64_t)slot);
}

static void puppet_ask(Cpu *c, Puppet *p, const char *name, const float pos[3], const float rot[9]) {
    spawn_request(c, &p->s, name, pos, rot);
    p->state = SP_ASKED, p->actor = 0, p->asked_frame = P.frames, p->seen_state = 0;
    p->bb_info = 0;
    p->arms_done = 0;
    memset(p->asked, 0, sizeof p->asked);
}

// 1 while the actor is still the one made for the request (its pack name: a freed actor's memory may hold another)
// and alive (state 3 until the game has woken it, then 4).
static int puppet_alive(const Puppet *p) {
    char n[64];
    uint64_t a = p->actor;
    uint32_t st = a && ok_ptr(a, 0x400) ? LD32(a + A_STATE) : 0;
    return (st == 3 || st == 4) && !strcmp(text_at(LD64(a + A_DEF_NAME), n, sizeof n), p->s.text);
}

// A request until its actor is made and taken (given up after 20 s), then the actor while it lives. 1 when it can be
// posed.
static int puppet_step(Cpu *c, Puppet *p) {
    uint32_t in = P.frames - p->asked_frame;
    if (p->state == SP_ASKED) {
        int r = spawn_take(c, &p->s);
        if (r != SPAWN_MADE) {
            if (r == SPAWN_PENDING && in < 20 * 60) return 0;
            rt_log("coop: puppet: w=%.2f %s not made after %u frames", wsec(), p->s.text, in);
            spawn_release(c, &p->s);
            p->state = SP_GONE, p->asked_frame = P.frames;
            return 0;
        }
        uint64_t a = p->actor = p->s.actor;
        rt_log("coop: puppet: w=%.2f %s made after %u frames: actor %#llx, state %u, AS control %#llx with %d slots",
               wsec(), p->s.text, in, (unsigned long long)a, LD32(a + A_STATE), (unsigned long long)as_control(a),
               as_slots(as_control(a)));
        p->state = SP_FOLLOW;
    }
    if (p->state != SP_FOLLOW) return 0;
    if (!puppet_alive(p)) {   // the game removed it (a new area): it is no longer ours to delete
        rt_log("coop: puppet: w=%.2f %s's actor is gone", wsec(), p->s.text);
        p->state = SP_GONE, p->actor = p->s.actor = 0, p->asked_frame = P.frames;
        return 0;
    }
    return 1;
}

static void puppet_release(Cpu *c, Puppet *p) {
    if (p->state == SP_ASKED || (p->state == SP_FOLLOW && puppet_alive(p))) spawn_release(c, &p->s);
    p->state = SP_GONE, p->actor = p->s.actor = p->s.handle = 0, p->asked_frame = P.frames;
}

// Puts the puppet at pos turned as rot (row-major), and has each of its AS slots play the node anim names at anim's
// frame: asked for when it changes, the frame set every frame.
static void puppet_pose(Cpu *c, Puppet *p, const float pos[3], const float rot[9], const Pose *anim, int verbose) {
    uint64_t a = p->actor;
    uint32_t st = LD32(a + A_STATE);
    if (st != p->seen_state) {
        rt_log("coop: puppet: w=%.2f %s: actor state %u, +0x30 %#x after %u frames", wsec(), p->s.text, st,
               LD32(a + A_SLEEP), P.frames - p->asked_frame);
        p->seen_state = st;
    }
    put_actor(c, a, pos, rot);
    uint64_t asc = as_control(a);
    if (P.arms == ARMS_FOLLOW && !p->arms_done && asc) {   // test (arms): slot 1 follows slot 0, once per actor
        p->arms_done = 1;
        for (int i = 1; i <= 1 && i < as_slots(asc); i++) {
            uint64_t r = rt_call(c, AS_SET_FOLLOW, 3, asc, (uint64_t)i, 0ull) & 1;
            uint64_t e = as_slot(asc, i);
            rt_log("coop: puppet: w=%.2f %s arms: slot %d follows slot 0 -> %llu (+0xc9 %d)", wsec(), p->s.text, i,
                   (unsigned long long)r, e ? (int)(int8_t)LD8(e + 0xc9) : -2);
        }
    }
    // test (arms): slot 1 (Upper; with upper3 also 2 RightArm, 3 LeftArm) that Link leaves empty plays Link's
    // slot-0 node at slot 0's frame, so that its bone group (torso, arms, head) is not left in the bind pose
    int top = P.arms == ARMS_UPPER3 ? 3 : P.arms == ARMS_UPPER || P.arms == ARMS_ALL ? 1 : 0;
    int n = as_slots(asc) < anim->nslots ? as_slots(asc) : anim->nslots;
    for (int i = 0; i < n; i++) {
        const char *node = anim->node[i];
        float frame = anim->frame[i];
        int mirror = 0;
        if (!node[0] && i >= 1 && i <= top && anim->node[0][0])
            node = anim->node[0], frame = anim->frame[0], mirror = 1;
        if (!node[0]) continue;
        if (strcmp(node, p->asked[i])) {
            int all = i == 0 && P.arms == ARMS_ALL;
            snprintf(p->asked[i], sizeof p->asked[i], "%s", node);
            p->play_ok[i] = as_play(c, a, node, i, -1.0f, all);
            if (verbose) rt_log("coop: puppet: w=%.2f %s slot %d: play '%s'%s at frame %.1f -> %d", wsec(), p->s.text,
                                i, node, mirror ? " (slot 0's, arms)" : all ? " (all slots)" : "", frame, p->play_ok[i]);
        }
        if (p->play_ok[i]) as_frame_set(c, asc, i, frame);
    }
}

// test (arms): every AS slot's node name and followee (entry+0xc9, -1 none), one line per actor.
static void log_slots(const char *who, uint64_t actor) {
    uint64_t asc = as_control(actor);
    int n = as_slots(asc);
    char line[720];
    int o = 0;
    line[0] = 0;
    for (int i = 0; i < n && i < 9 && o < (int)sizeof line - 72; i++) {
        uint64_t e = as_slot(asc, i);
        char name[48];
        text_at(e ? LD64(e + 0x10) : 0, name, sizeof name);
        int fol = e ? (int)(int8_t)LD8(e + 0xc9) : -1;
        o += snprintf(line + o, sizeof line - o, " %d'%s'", i, name);
        if (fol >= 0) o += snprintf(line + o, sizeof line - o, "<%d", fol);
    }
    rt_log("coop: puppet: w=%.2f slots %s:%s", wsec(), who, line);
}

static void puppet_name(char *out, size_t cap, int adult) { snprintf(out, cap, "CoopPuppet_%s", adult ? "Adult" : "Child"); }

static int is_adult(uint64_t player) {
    char n[64];
    return strstr(text_at(LD64(player + A_DEF_NAME), n, sizeof n), "Adult") != NULL;
}

// The blackboard copy (game 1.1.1): Link's AS blackboard copied to the puppet's, so that the puppet's AS
// picks the same arm layers (Pose_<item> over the arm / upper-body bone masks) as Link's. A test switch, off by default.
// Uses coop_puppet.c's MAIN, ok_ptr, LD8/LD16/LD32/LD64, f32_at, text_at, wsec, rt_call, rt_log.
//
// The AS blackboard. asc+0x40 is the AS control's local blackboard: 0x108 bytes, vtable
// main+0x4c29818. The AS control's set-up main+0x73c260 allocates it (main+0x73ca5c `mov w1,#0x108`, blr at 0x73ca68),
// stores the vtable (GOT main+0x4c97b78 + 0x10, main+0x73ca84..0x73ca90), fills it from the ASB (main+0x73b080) and
// stores it at main+0x73cad0 `str x24, [x19, #0x40]`. It is made only when the ASB has blackboard keys (sum of the
// six counts, main+0x73c4fc). Six value types g: 0 string (char *), 1 s32, 2 (none in Player_<age>), 3 f32,
// 4 bool (u8), 5 vec3f. Every block is {u32 count; data}:
//   +0x10+8g  values        data at +8 for strings (8-byte slots), else at +4; element 8/4/4/4/1/12 bytes
//   +0x40+8g  change counts u8 per key at +4; every set adds 1 (vtable +0x158+8g return them to the AS)
//   +0x70+8g  link table    2-byte entries {s8 linked blackboard 0..3, u8 its key}; NULL when the AS has no links
//   +0xa0+8g  key pointers  8-byte entries at +8; a non-NULL one replaces the local value
//   +0xd0..+0xe8 linked blackboards, +0xf0 the AS resource, +0xf8 an override, +0x100 from the AS's create args
// Getters (vtable +0x120+8g; bool main+0x37ba7a0) return the address the AS reads: override, then link table, then key
// pointer, then the local value. Setters (vtable +0x68+8g: str main+0x37ba4d0, s32 main+0x37b9790, f32 main+0x37b9cc0
// (value in s0), bool main+0x37ba100, vec3f main+0x37ba330 (x2 -> 12 bytes)) write the local value and add 1 to the
// count; ignore keys < 0 and write key 0 for keys >= count. The game's own writers use them: OneShotPlayerSetBoolASBB
// (main+0x290f744/748, slot +0x88) and the player's WeaponType write (main+0x2677f44/48, slot +0x70).
// Find by name: vtable +0x2e0+8g (s32 +0x2e8, f32 +0x2f8, bool +0x300 seen used: main+0x2efcfd8.., 0x2efcf10), x1 ->
// a cell holding the name's char *, returns the key or < 0.

#define AS_BB_VTBL (MAIN + 0x4c29818)
#define BB_NT 6
enum { BB_STR, BB_S32, BB_T2, BB_F32, BB_BOOL, BB_VEC3 };
static const uint8_t bb_esz[BB_NT] = {8, 4, 4, 4, 1, 12};
static const uint8_t bb_doff[BB_NT] = {8, 4, 4, 4, 4, 4};
#define BB_SET(g) (0x68 + 8 * (g))
#define BB_GET(g) (0x120 + 8 * (g))
#define BB_FIND(g) (0x2e0 + 8 * (g))
#define BB_MAXK 128   // per type; Player_<age>.root.asb: str 7, s32 37, f32 100, bool 94, vec3f 1

static uint64_t as_bb(uint64_t asc) {
    if (!asc || !ok_ptr(asc + 0x40, 8)) return 0;
    uint64_t bb = LD64(asc + 0x40);
    return ok_ptr(bb, 0x108) && LD64(bb) == AS_BB_VTBL ? bb : 0;
}

static uint32_t bb_count(uint64_t bb, int g) {
    uint64_t blk = LD64(bb + 0x10 + 8ull * g);
    return blk && ok_ptr(blk, 4) ? LD32(blk) : 0;
}

// 1 when the getter's answer for type g can be computed here: no override, no link table.
static int bb_plain(uint64_t bb, int g) {
    uint64_t lnk = LD64(bb + 0x70 + 8ull * g);
    return !LD64(bb + 0xf8) && !(lnk && ok_ptr(lnk, 4) && LD32(lnk));
}

// The address the AS reads key i of type g from, as the game's getter finds it; 0 when unreadable.
static uint64_t bb_addr(Cpu *c, uint64_t bb, int g, uint32_t i) {
    if (i >= bb_count(bb, g)) return 0;
    uint64_t p;
    if (!bb_plain(bb, g)) {
        p = rt_call(c, LD64(LD64(bb) + BB_GET(g)), 2, bb, (uint64_t)i);
    } else {
        uint64_t ref = LD64(bb + 0xa0 + 8ull * g);
        p = ref && ok_ptr(ref, 8 + 8ull * (i + 1)) && i < LD32(ref) ? LD64(ref + 8 + 8ull * i) : 0;
        if (!p) p = LD64(bb + 0x10 + 8ull * g) + bb_doff[g] + (uint64_t)bb_esz[g] * i;
    }
    return ok_ptr(p, bb_esz[g]) ? p : 0;
}

// The key of type g called name, by the blackboard's own find (vtable +0x2e0+8g), or -1.
static int bb_find(Cpu *c, uint64_t bb, int g, const char *name) {
    static uint64_t box;   // the find reads the name's pointer from [x1], as main+0x2efcf04..08 passes it
    box = (uint64_t)(uintptr_t)name;
    int32_t k = (int32_t)rt_call(c, LD64(LD64(bb) + BB_FIND(g)), 2, bb, (uint64_t)(uintptr_t)&box);
    return k >= 0 && (uint32_t)k < bb_count(bb, g) ? k : -1;
}

static void raw_text(uint64_t a, char *out, size_t cap) {   // a guest C string's bytes, unchanged ("" if unreadable)
    size_t i = 0;
    for (; a && i + 1 < cap && ok_ptr(a + i, 1) && LD8(a + i); i++) out[i] = (char)LD8(a + i);
    out[i] = 0;
}

// What a copy remembers per destination blackboard. The puppet's AS keeps pointers to text[] (its strings) and reads
// them for as long as it lives, so a slot is never reused while its puppet is alive (4 > 3 remote puppets + shadow).
typedef struct {
    uint64_t from, to;
    int primed;
    uint32_t used;                       // P.frames of the last copy (to pick a slot to reuse)
    uint8_t seen[BB_NT][BB_MAXK];        // Link's change count last passed on
    char text[8][64];                    // the puppet's own copies of Link's strings (never Link's pointers)
    float vec[4][4];                     // the vec3f values handed to the setter
    uint32_t sets, sets_g[BB_NT], sets_logged;
} BBCopy;
static BBCopy bbc[4];

static BBCopy *bbc_for(uint64_t from, uint64_t to) {
    BBCopy *k = NULL;
    for (int i = 0; i < 4 && !k; i++)
        if (bbc[i].to == to) k = &bbc[i];
    if (!k) {
        k = &bbc[0];
        for (int i = 1; i < 4; i++)
            if (bbc[i].used < k->used) k = &bbc[i];
    }
    if (k->to != to || k->from != from) memset(k, 0, sizeof *k), k->from = from, k->to = to;
    k->used = P.frames;
    return k;
}

// Copies the AS blackboard of from_asc (Link) to to_asc (the puppet): every key whose value differs, and every key
// whose change count Link's AS saw move since the last call, through the puppet's own setter (so that its count moves
// as Link's did). Never copies pointers: strings go as text into BBCopy.text. Call it on the game's update thread
// (puppet_frame), before puppet_pose asks for nodes, so that the puppet's selectors start with Link's values.
static void as_bb_copy(Cpu *c, uint64_t from_asc, uint64_t to_asc) {
    uint64_t a = as_bb(from_asc), b = as_bb(to_asc);
    if (!a || !b || a == b) return;
    BBCopy *k = bbc_for(a, b);
    int first = !k->primed;
    for (int g = 0; g < BB_NT; g++) {
        uint32_t n = bb_count(a, g);
        if (g == BB_T2 || !n || n != bb_count(b, g) || n > BB_MAXK) continue;   // same ASB: same counts
        if ((g == BB_STR && n > 8) || (g == BB_VEC3 && n > 4)) continue;
        uint64_t cnt = LD64(a + 0x40 + 8ull * g);
        int has_cnt = cnt && ok_ptr(cnt, 4 + n) && LD32(cnt) >= n;
        uint64_t set = LD64(LD64(b) + BB_SET(g));
        for (uint32_t i = 0; i < n; i++) {
            uint64_t pa = bb_addr(c, a, g, i), pb = bb_addr(c, b, g, i);
            if (!pa || !pb) continue;
            uint8_t ca = has_cnt ? LD8(cnt + 4 + i) : 0;
            int moved = !first && ca != k->seen[g][i];
            k->seen[g][i] = ca;
            char ta[64] = "", tb[64] = "";
            int differs;
            if (g == BB_STR) {
                raw_text(LD64(pa), ta, sizeof ta), raw_text(LD64(pb), tb, sizeof tb);
                differs = strcmp(ta, tb) != 0;
            } else if (g == BB_BOOL) {
                differs = (LD8(pa) & 1) != (LD8(pb) & 1);
            } else {
                differs = memcmp((const void *)(uintptr_t)pa, (const void *)(uintptr_t)pb, bb_esz[g]) != 0;
            }
            if (!differs && !moved) continue;
            switch (g) {
            case BB_S32: rt_call(c, set, 3, b, (uint64_t)i, (uint64_t)LD32(pa)); break;
            case BB_F32: c->v[0] = LD32(pa); rt_call(c, set, 2, b, (uint64_t)i); break;
            case BB_BOOL: rt_call(c, set, 3, b, (uint64_t)i, (uint64_t)(LD8(pa) & 1)); break;
            case BB_VEC3:
                memcpy(k->vec[i], (const void *)(uintptr_t)pa, 12);
                rt_call(c, set, 3, b, (uint64_t)i, (uint64_t)(uintptr_t)k->vec[i]);
                break;
            case BB_STR:
                memcpy(k->text[i], ta, sizeof ta);
                rt_call(c, set, 3, b, (uint64_t)i, (uint64_t)(uintptr_t)k->text[i]);
                break;
            }
            k->sets++, k->sets_g[g]++;
        }
    }
    k->primed = 1;
    if (first)
        rt_log("coop: puppet: w=%.2f BB first copy %#llx -> %#llx: set str %u s32 %u f32 %u bool %u vec3 %u", wsec(),
               (unsigned long long)a, (unsigned long long)b, k->sets_g[BB_STR], k->sets_g[BB_S32], k->sets_g[BB_F32],
               k->sets_g[BB_BOOL], k->sets_g[BB_VEC3]);
}

// Once per AS (Link's and the puppet's when it is made): counts, the ASB file, and whether anything redirects reads.
// Expect: keys 7/37/0/100/94/1, override 0, linked 0, link tables 0, key pointers 0.
static void as_bb_info(const char *who, uint64_t asc) {
    uint64_t bb = as_bb(asc);
    if (!bb) {
        rt_log("coop: puppet: BB %s: none (AS %#llx, +0x40 %#llx)", who, (unsigned long long)asc,
               asc && ok_ptr(asc + 0x40, 8) ? (unsigned long long)LD64(asc + 0x40) : 0ull);
        return;
    }
    int links = 0, refs = 0;
    for (int g = 0; g < BB_NT; g++) {
        uint64_t l = LD64(bb + 0x70 + 8ull * g), r = LD64(bb + 0xa0 + 8ull * g);
        links += l && ok_ptr(l, 4) && LD32(l);
        uint32_t n = r && ok_ptr(r, 4) ? LD32(r) : 0;
        for (uint32_t i = 0; i < n && i < 256 && ok_ptr(r + 8 + 8ull * i, 8); i++) refs += LD64(r + 8 + 8ull * i) != 0;
    }
    uint64_t res = LD64(bb + 0xf0), h = ok_ptr(res, 0x18) ? LD64(res + 0x10) : 0;
    rt_log("coop: puppet: BB %s: bb %#llx keys str %u s32 %u t2 %u f32 %u bool %u vec3 %u, ASB file %#llx, override %#llx, "
           "linked %#llx, link tables %d, key pointers %d", who, (unsigned long long)bb, bb_count(bb, 0), bb_count(bb, 1),
           bb_count(bb, 2), bb_count(bb, 3), bb_count(bb, 4), bb_count(bb, 5),
           (unsigned long long)(ok_ptr(h, 0x10) ? LD64(h + 8) : 0), (unsigned long long)LD64(bb + 0xf8),
           (unsigned long long)LD64(bb + 0xd0), links, refs);
}

// Every second: the keys the arm layers select on, Link/puppet, the sets of the last second and how many keys still
// differ after the copy (expect 0; with NX2NAVI_COOP_PUPPET_BB=0 expect the ASB defaults on the puppet side:
// WeaponType 0, UsingItem -1, LiftableType 8, EquipShield 0).
static void as_bb_log(Cpu *c, uint64_t from_asc, uint64_t to_asc) {
    static const struct { int g; const char *name; } K[] = {
        {BB_S32, "WeaponType"}, {BB_S32, "ShieldType"}, {BB_S32, "UsingItem"}, {BB_S32, "LiftableType"},
        {BB_S32, "ArrowType"}, {BB_BOOL, "EquipShield"}, {BB_BOOL, "IsCaution"}, {BB_BOOL, "IsChild"},
        {BB_BOOL, "IsBoomerangShooting"}, {BB_F32, "LinearVelXZ"}, {BB_F32, "DrawnSheathRate"},
        {BB_STR, "UpperWeaponEpuipType"}};
    uint64_t bb[2] = {as_bb(from_asc), as_bb(to_asc)};
    if (!bb[0] || !bb[1]) return;
    char line[900];
    int o = 0;
    for (size_t q = 0; q < sizeof K / sizeof *K && o < (int)sizeof line - 80; q++) {
        o += snprintf(line + o, sizeof line - o, " %s", K[q].name);
        for (int s = 0; s < 2; s++) {
            int i = bb_find(c, bb[s], K[q].g, K[q].name);
            uint64_t p = i >= 0 ? bb_addr(c, bb[s], K[q].g, (uint32_t)i) : 0;
            char t[48], s40[40];
            if (!p) snprintf(t, sizeof t, "-");
            else if (K[q].g == BB_S32) snprintf(t, sizeof t, "%d", (int32_t)LD32(p));
            else if (K[q].g == BB_F32) snprintf(t, sizeof t, "%.2f", f32_at(p));
            else if (K[q].g == BB_BOOL) snprintf(t, sizeof t, "%u", LD8(p) & 1u);
            else snprintf(t, sizeof t, "'%s'", text_at(LD64(p), s40, sizeof s40));
            o += snprintf(line + o, sizeof line - o, "%s%s", s ? "/" : " ", t);
        }
    }
    int diff = 0;
    for (int g = 0; g < BB_NT; g++) {
        uint32_t n = bb_count(bb[0], g);
        if (g == BB_STR || g == BB_T2 || n != bb_count(bb[1], g) || n > BB_MAXK) continue;
        for (uint32_t i = 0; i < n; i++) {
            uint64_t pa = bb_addr(c, bb[0], g, i), pb = bb_addr(c, bb[1], g, i);
            if (pa && pb && (g == BB_BOOL ? (LD8(pa) & 1) != (LD8(pb) & 1)
                                          : memcmp((const void *)(uintptr_t)pa, (const void *)(uintptr_t)pb, bb_esz[g]) != 0))
                diff++;
        }
    }
    BBCopy *k = NULL;
    for (int i = 0; i < 4; i++)
        if (bbc[i].to == bb[1]) k = &bbc[i];
    uint32_t sets = k ? k->sets - k->sets_logged : 0;
    if (k) k->sets_logged = k->sets;
    rt_log("coop: puppet: w=%.2f BB Link/puppet: %u sets in the last second, %d keys differ;%s", wsec(), sets, diff, line);
}



// ---- gear (game 1.1.1): the puppet wears and holds what Link does, through the game's own
// EquipmentUser API.
//
// The EquipmentUser (EU) is component +0x200 of an actor's components: *(*(actor+0x270)+0x200) (main+0x7f7690 is the
// game's getter). Fields used:
//   +0x18    the owner actor
//   +0x20    8 dynamic links (hand and back items) of 24 bytes by slot j; j -> kind {0,1,2,5,6,11,12,0}
//            (f_7002dfec90, table main+0x3ef95b8)
//   +0xe0    13 static links (armour, sheath, quiver, accessories) of 24 bytes by slot s; s -> kind
//            {8,8,9,10,3,7,7,4,13,14,15,17,16} (table main+0x3ef95d0)
//   link     +0x08 a handle table, +0x10 u32 generation (~0: empty); table +0x40 the actor, +0x48 its generation
//   +0x10b0  per kind: the create request in flight; +0x1270 the EquipmentUserParam; +0x12d0 a bit per kind in flight
// Kinds (names at main+0x3ef9540): 0 Weapon 1 SubWeapon 2 ThirdWeapon 3 Sheath 4 Quiver 5 Bow 6 Shield 7 ArmorHead
// 8 ArmorUpper 9 ArmorLower 10 ArmorHand 11 Tool 12 SubTool 13..16 Accessory1..4 17 Extra.
// An item's name is *(item+0x20); its state is main+0x4a3c00(eu, j): 1 drawn (in the hand), 2 sheathed (on the
// body), 0 none or changing.
// The game's calls (each checks that it runs on the game's update thread, where puppet_frame runs):
//   EQ_CREATE_DYN  main+0x2df7b20 (eu, kind, const char **name, state, life -1, no_fade 1, by_pouch 0) -> 1 when queued;
//                  the same name in the same state already attached: nothing; in another state: made again in it
//   EQ_CREATE_STAT main+0x1013ea0 (eu, kind, const char **name, by_pouch 0, no_fade 1) -> 1 when queued
//                  (ArmorUpper "Armor_Adult_Normal" removes static slot 1 instead: the body's own tunic)
//   EQ_DRAW        main+0x6b6820 (eu, j), EQ_SHEATHE main+0xe34c70 (eu, j): what the pause menu Link's
//                  ExecuteSetPouchPlayerDrawn calls
//   EQ_REMOVE_DYN  main+0x2dfd1b0 (eu, j, 0, 0, 0, 0, 1, 0), EQ_REMOVE_STAT main+0x10aceb0 (eu, s, 0)
// The creates are asynchronous: the EU makes the item as any create request (a few frames), then attaches it at the
// bone its EquipmentUserParam names. The puppet keeps PouchPlayer_<age>'s EquipmentUserRef (IsPouchUser false: it does
// not dress itself from the local inventory).
//
// NX2NAVI_COOP_PUPPET_GEAR: 0 off; 1 (default) weapon, bow, shield (drawn or sheathed as Link's) and the static items;
// 2 also the subweapon, tool and subtool (bottles, boomerang, sticks: each change makes and removes an actor).
// NX2NAVI_COOP_PUPPET_GEAR_DRAWN=0 keeps the puppet's weapon and shield sheathed whatever Link does.
// NX2NAVI_COOP_PUPPET_GEAR_MASK (test, hex): the slots copied, bits 0..6 dynamic j0..j6, bits 8..20 static s0..s12.

#define EU_DYN 0x20
#define EU_STAT 0xe0
#define EU_LINK 24
#define EU_OWNER 0x18
#define EU_REQ 0x10b0
#define EU_PARAM 0x1270
#define EU_INFLIGHT 0x12d0
#define A_NAME 0x20           // char *: the actor's name, what the EU compares (f_7002dfd420 returns it)
#define A_POUCH 0x6a4         // bit 0: made as a pouch (menu) actor -> "Equipment_IsCreatedAsPouchActor"
#define EQ_CREATE_DYN (MAIN + 0x2df7b20)
#define EQ_CREATE_STAT (MAIN + 0x1013ea0)
#define EQ_STATE (MAIN + 0x4a3c00)
#define EQ_DRAW (MAIN + 0x6b6820)
#define EQ_SHEATHE (MAIN + 0xe34c70)
#define EQ_REMOVE_DYN (MAIN + 0x2dfd1b0)
#define EQ_REMOVE_STAT (MAIN + 0x10aceb0)

enum { GEAR_DYN = 8, GEAR_STAT = 13, GEAR_KINDS = 18, GEAR_NAME = 48 };
static const uint8_t dyn_kind[GEAR_DYN] = {0, 1, 2, 5, 6, 11, 12, 0};
static const uint8_t stat_kind[GEAR_STAT] = {8, 8, 9, 10, 3, 7, 7, 4, 13, 14, 15, 17, 16};
static const char *const kind_name[GEAR_KINDS] = {"Weapon", "SubWeapon", "ThirdWeapon", "Sheath", "Quiver", "Bow",
    "Shield", "ArmorHead", "ArmorUpper", "ArmorLower", "ArmorHand", "Tool", "SubTool", "Accessory1", "Accessory2",
    "Accessory3", "Accessory4", "Extra"};

// What one Link wears: the names of his dynamic items by slot j with their states, of his static items by slot s.
// The same struct is what a gear message would carry for a remote player.
typedef struct {
    char dyn[GEAR_DYN][GEAR_NAME];
    uint8_t state[GEAR_DYN];
    char stat[GEAR_STAT][GEAR_NAME];
} Gear;

// The dynamic slots copied: weapon, bow, shield; then subweapon, tool, subtool with NX2NAVI_COOP_PUPPET_GEAR=2.
static const int gear_js[] = {0, 3, 4, 1, 5, 6};

#define GEAR_SETTLE 10        // frames the puppet is in the scene (state 4) before its first request
#define GEAR_RETRY 60         // frames between two creates of one kind
#define GEAR_STATE_WAIT 20    // frames a draw/sheathe request is given before the item is made again in that state
#define GEAR_TRIES 4          // requests of one kind without the puppet matching, then that kind waits for a change
#define GEAR_BUSY_LOG 300     // frames a kind may stay busy (a create in flight) before it is logged
#define GEAR_STABLE 6         // frames what is wanted of a kind must stay the same before it is asked for (a draw
                              // passes through state 0 for a frame or two)
#define GEAR_STOW_WAIT 90     // frames a stowed-versus-shown difference lasts before the item is made again stowed

typedef struct {
    uint64_t puppet;          // the actor this is for (reset when another actor takes the slot)
    uint32_t seen_frame;      // P.frames when this puppet was first seen
    uint32_t next[GEAR_KINDS];
    uint8_t tries[GEAR_KINDS];
    uint32_t sig[GEAR_KINDS];        // what was wanted of the kind last (name and state): a change starts again
    uint32_t sig_frame[GEAR_KINDS];  // P.frames when it last changed
    uint32_t busy_since[GEAR_KINDS];
    uint32_t diff_since[GEAR_KINDS];  // P.frames + 1 when the puppet's dynamic item of the kind began to differ
    char ask[GEAR_KINDS][GEAR_NAME]; // the name asked for, per kind: the request's name cell points here
    uint64_t cell[GEAR_KINDS];
    Gear seen_want, seen_have;       // for the change logs
    int info, matched, bow_logged;
    uint32_t requests;
} GearSync;

static int gear_mode = 1;     // NX2NAVI_COOP_PUPPET_GEAR
static int gear_drawn = 1;    // NX2NAVI_COOP_PUPPET_GEAR_DRAWN
static uint32_t gear_mask = ~0u;   // NX2NAVI_COOP_PUPPET_GEAR_MASK

static uint64_t eu_of(uint64_t actor) {
    uint64_t comps = actor && ok_ptr(actor, 0x278) ? LD64(actor + A_COMPONENTS) : 0;
    uint64_t eu = ok_ptr(comps, 0x208) ? LD64(comps + 0x200) : 0;
    return ok_ptr(eu, 0x13c8) && LD64(eu + EU_OWNER) == actor ? eu : 0;
}

// The live actor an EU link holds, or 0 (empty, stale generation, or being deleted: states 7..11, as the game checks).
static uint64_t link_actor(uint64_t link) {
    if (!ok_ptr(link, EU_LINK)) return 0;
    uint64_t t = LD64(link + 8);
    uint32_t gen = LD32(link + 0x10);
    if (gen == 0xffffffffu || !ok_ptr(t, 0x50) || LD32(t + 0x48) != gen) return 0;
    uint64_t a = LD64(t + 0x40);
    if (!ok_ptr(a, 0x400)) return 0;
    uint32_t st = LD32(a + A_STATE);
    return st - 7u < 5u ? 0 : a;
}

// Names that may be asked for: the game's equipment actors only (Weapon_*, Armor_*, and the ocarina accessories
// Item_Ocarina_*), printable, short. A remote player's gear comes from another game, so this is also the filter for
// network data.
static int gear_name_ok(const char *s) {
    size_t n = strlen(s);
    if (n < 7 || n >= GEAR_NAME ||
        (strncmp(s, "Weapon_", 7) && strncmp(s, "Armor_", 6) && strncmp(s, "Item_Ocarina_", 13)))
        return 0;
    for (size_t i = 0; i < n; i++)
        if (s[i] <= 0x20 || s[i] > 0x7e) return 0;
    return 1;
}

static void gear_read(Cpu *c, uint64_t actor, Gear *g) {
    memset(g, 0, sizeof *g);
    uint64_t eu = eu_of(actor);
    if (!eu) return;
    for (int j = 0; j < 7; j++) {
        uint64_t a = link_actor(eu + EU_DYN + EU_LINK * (uint64_t)j);
        if (!a) continue;
        text_at(LD64(a + A_NAME), g->dyn[j], sizeof g->dyn[j]);
        g->state[j] = (uint8_t)rt_call(c, EQ_STATE, 2, eu, (uint64_t)j);
    }
    for (int s = 0; s < GEAR_STAT; s++) {
        uint64_t a = link_actor(eu + EU_STAT + EU_LINK * (uint64_t)s);
        if (a) text_at(LD64(a + A_NAME), g->stat[s], sizeof g->stat[s]);
    }
}

static int gear_has_stat(const Gear *g, const char *name) {
    for (int s = 0; s < GEAR_STAT; s++)
        if (g->stat[s][0] && !strcmp(g->stat[s], name)) return 1;
    return 0;
}

// The static items the puppet is asked for: Link's, except the green adult tunic, which is the body's own (asking
// for it removes static slot 1).
static int gear_stat_wanted(const char *w) { return w[0] && gear_name_ok(w) && strcmp(w, "Armor_Adult_Normal"); }

// A create of the kind is still in flight: its request cell is set (the in-flight bit at +0x12d0 is left out: it
// stays set after the item is attached). df7b20 cancels a pending request of the kind itself, so this
// only keeps the puppet's own requests from cancelling each other while an item pack loads.
static int kind_busy(uint64_t eu, int kind) { return LD64(eu + EU_REQ + 8ull * (unsigned)kind) != 0; }

static uint32_t gear_sig(const char *name, unsigned state) {
    uint32_t h = 2166136261u ^ state;
    for (; *name; name++) h = (h ^ (uint8_t)*name) * 16777619u;
    return h;
}

// What is wanted of a kind now: when it changed, its tries start again at once.
static void gear_want(GearSync *gs, int kind, const char *name, unsigned state) {
    uint32_t h = gear_sig(name, state);
    if (h != gs->sig[kind]) gs->sig[kind] = h, gs->sig_frame[kind] = P.frames, gs->tries[kind] = 0, gs->next[kind] = 0;
}

// Whether the kind may be asked for now: spaced (next), not given up (tries), what is wanted stable for GEAR_STABLE
// frames, and for a create (create 1) no create of the kind in flight, unless that has lasted GEAR_BUSY_LOG frames.
static int gear_may(GearSync *gs, uint64_t eu, int kind, int create, const char *who) {
    if ((int32_t)(P.frames - gs->next[kind]) < 0 || gs->tries[kind] >= GEAR_TRIES ||
        P.frames - gs->sig_frame[kind] < GEAR_STABLE)
        return 0;
    if (create && kind_busy(eu, kind)) {   // busy_since is P.frames + 1 when first seen busy (0: not busy)
        if (!gs->busy_since[kind]) gs->busy_since[kind] = P.frames + 1 ? P.frames + 1 : 1;
        if (P.frames + 1 - gs->busy_since[kind] < GEAR_BUSY_LOG) return 0;
        rt_log("coop: puppet: gear: w=%.2f %s %s: a create in flight for %u frames (request %#llx): asked anyway",
               wsec(), who, kind_name[kind], GEAR_BUSY_LOG, (unsigned long long)LD64(eu + EU_REQ + 8ull * (unsigned)kind));
    }
    gs->busy_since[kind] = 0;
    return 1;
}

static void gear_tried(GearSync *gs, int kind, uint32_t wait, const char *who, const char *what) {
    gs->next[kind] = P.frames + wait;
    gs->requests++;
    if (++gs->tries[kind] == GEAR_TRIES)
        rt_log("coop: puppet: gear: %s: %s %s still differs after %d requests, left until it changes", who,
               kind_name[kind], what, GEAR_TRIES);
}

static uint64_t gear_cell(GearSync *gs, int kind, const char *name) {
    snprintf(gs->ask[kind], sizeof gs->ask[kind], "%s", name);
    gs->cell[kind] = (uint64_t)(uintptr_t)gs->ask[kind];
    return (uint64_t)(uintptr_t)&gs->cell[kind];
}

static void gear_log_changes(const char *who, const char *side, const Gear *now, Gear *seen, uint32_t since) {
    for (int j = 0; j < 7; j++)
        if (strcmp(now->dyn[j], seen->dyn[j]) || now->state[j] != seen->state[j])
            rt_log("coop: puppet: gear: w=%.2f %s %s j%d %s '%s' state %u (was '%s' %u) after %u frames", wsec(), who,
                   side, j, kind_name[dyn_kind[j]], now->dyn[j], now->state[j], seen->dyn[j], seen->state[j], since);
    for (int s = 0; s < GEAR_STAT; s++)
        if (strcmp(now->stat[s], seen->stat[s]))
            rt_log("coop: puppet: gear: w=%.2f %s %s s%d %s '%s' (was '%s') after %u frames", wsec(), who, side, s,
                   kind_name[stat_kind[s]], now->stat[s], seen->stat[s], since);
    *seen = *now;
}

// Makes the puppet's EU match want: one request per kind at a time, spaced, at most GEAR_TRIES each until what is
// wanted of that kind changes, at most 2 requests a frame. all: also the subweapon, tool and subtool slots; drawn: 0
// keeps the puppet's weapon and shield sheathed.
static void gear_apply(Cpu *c, uint64_t puppet, const Gear *want, GearSync *gs, const char *who, int all, int drawn) {
    if (gs->puppet != puppet) {
        memset(gs, 0, sizeof *gs);
        gs->puppet = puppet, gs->seen_frame = P.frames;
    }
    uint64_t eu = eu_of(puppet);
    if (!eu) {
        if (!gs->info) rt_log("coop: puppet: gear: %s %#llx has no EquipmentUser", who, (unsigned long long)puppet);
        gs->info = 1;
        return;
    }
    if (LD32(puppet + A_STATE) != 4 || P.frames - gs->seen_frame < GEAR_SETTLE) return;
    uint32_t since = P.frames - gs->seen_frame;
    if (!gs->info) {
        gs->info = 1;
        rt_log("coop: puppet: gear: w=%.2f %s %#llx EU %#llx (vtable %#llx) param %#llx pouch flag %u, in flight %#x",
               wsec(), who, (unsigned long long)puppet, (unsigned long long)eu, (unsigned long long)LD64(eu),
               (unsigned long long)LD64(eu + EU_PARAM), LD32(puppet + A_POUCH) & 1u, LD32(eu + EU_INFLIGHT));
    }
    Gear have;
    gear_read(c, puppet, &have);
    gear_log_changes(who, "Link", want, &gs->seen_want, since);
    gear_log_changes(who, "puppet", &have, &gs->seen_have, since);
    int asked = 0, match = 1;
    // Bow items (slot j3: slingshot, bow, hookshot) read their owner's component +0x358 (the behaviour-state
    // animation one, which Link has and the pause menu Link's copy has not) with no null check: the slingshot's
    // update crashed at once on the puppet (f_7002dcee60 reading 0x7d8 via f_700082acc0). So the slot is
    // left out while the puppet lacks that component.
    uint64_t comps = LD64(puppet + A_COMPONENTS);
    int has_bsa = ok_ptr(comps, 0x360) && LD64(comps + 0x358) != 0;
    if (!has_bsa && want->dyn[3][0] && !gs->bow_logged) {
        gs->bow_logged = 1;
        rt_log("coop: puppet: gear: %s j3 Bow '%s' left out: the puppet has no component +0x358, which bow items read",
               who, want->dyn[3]);
    }

    for (size_t q = 0; q < sizeof gear_js / sizeof *gear_js; q++) {
        int j = gear_js[q], kind = dyn_kind[j];
        if (q >= 3 && !all) break;
        if (!(gear_mask >> j & 1) || (j == 3 && !has_bsa)) continue;
        const char *w = want->dyn[j], *h = have.dyn[j];
        unsigned ws = want->state[j] <= 2 ? want->state[j] : 0u;   // 0 stowed (not shown), 1 drawn, 2 sheathed
        if (ws == 1 && !drawn) ws = 2;
        int ok = gear_name_ok(w);
        gear_want(gs, kind, ok ? w : "", ok ? ws : 0);
        if (ok ? strcmp(w, h) || have.state[j] != ws : !w[0] && h[0]) {
            match = 0;
            if (!gs->diff_since[kind]) gs->diff_since[kind] = P.frames + 1 ? P.frames + 1 : 1;
        } else {
            gs->diff_since[kind] = 0;
            continue;
        }
        // the same item in another state: a draw/sheathe request first (state 1/2), then the item made again in it.
        // Stowed (0) has no request: the item is made again, but only once the difference has lasted GEAR_STOW_WAIT
        // frames (an area change shows Link's items stowed for a few seconds while the puppet's EU sets its new
        // items sheathed by itself).
        int same = ok && !strcmp(w, h);
        int request = same && gs->tries[kind] == 0 && ws != 0;
        if (same && ws == 0 && P.frames + 1 - gs->diff_since[kind] < GEAR_STOW_WAIT) continue;
        if (asked >= 2 || !gear_may(gs, eu, kind, !request, who)) continue;
        if (!ok) {   // Link has none there: take the puppet's off
            uint64_t r = rt_call(c, EQ_REMOVE_DYN, 8, eu, (uint64_t)j, 0ull, 0ull, 0ull, 0ull, 1ull, 0ull) & 1;
            rt_log("coop: puppet: gear: w=%.2f %s remove j%d %s '%s' -> %llu", wsec(), who, j, kind_name[kind], h,
                   (unsigned long long)r);
            gear_tried(gs, kind, GEAR_RETRY, who, "removal"), asked++;
        } else if (strcmp(w, h)) {   // another item, or none yet: make Link's in his state
            uint64_t r = rt_call(c, EQ_CREATE_DYN, 7, eu, (uint64_t)kind, gear_cell(gs, kind, w), (uint64_t)ws,
                                 (uint64_t)0xffffffffu, 1ull, 0ull) & 1;
            rt_log("coop: puppet: gear: w=%.2f %s ask %s j%d '%s' state %u -> %llu (puppet had '%s')", wsec(), who,
                   kind_name[kind], j, w, ws, (unsigned long long)r, h);
            gear_tried(gs, kind, GEAR_RETRY, who, w), asked++;
        } else {
            uint64_t r;
            if (request) r = rt_call(c, ws == 1 ? EQ_DRAW : EQ_SHEATHE, 2, eu, (uint64_t)j) & 1;
            else r = rt_call(c, EQ_CREATE_DYN, 7, eu, (uint64_t)kind, gear_cell(gs, kind, w), (uint64_t)ws,
                             (uint64_t)0xffffffffu, 1ull, 0ull) & 1;
            rt_log("coop: puppet: gear: w=%.2f %s %s j%d '%s' %u -> %u (%s) -> %llu", wsec(), who,
                   ws == 1 ? "draw" : ws == 2 ? "sheathe" : "stow", j, w, have.state[j], ws,
                   request ? "state request" : "made again",
                   (unsigned long long)r);
            gear_tried(gs, kind, request ? GEAR_STATE_WAIT : GEAR_RETRY, who, w), asked++;
        }
    }

    for (int s = 0; s < GEAR_STAT; s++) {   // static: Link's items the puppet lacks
        int kind = stat_kind[s];
        const char *w = want->stat[s];
        if (!(gear_mask >> (8 + s) & 1) || !gear_stat_wanted(w) || gear_has_stat(&have, w)) continue;
        match = 0;
        gear_want(gs, kind, w, 0);
        if (asked >= 2 || !gear_may(gs, eu, kind, 1, who)) continue;
        uint64_t r = rt_call(c, EQ_CREATE_STAT, 5, eu, (uint64_t)kind, gear_cell(gs, kind, w), 0ull, 1ull) & 1;
        rt_log("coop: puppet: gear: w=%.2f %s ask %s s%d '%s' -> %llu", wsec(), who, kind_name[kind], s, w,
               (unsigned long long)r);
        gear_tried(gs, kind, GEAR_RETRY, who, w), asked++;
    }
    for (int s = 0; s < GEAR_STAT; s++) {   // static: the puppet's items Link does not wear
        int kind = stat_kind[s];
        const char *h = have.stat[s];
        if (!(gear_mask >> (8 + s) & 1) || !h[0] || gear_has_stat(want, h)) continue;
        match = 0;
        if (asked >= 2 || !gear_may(gs, eu, kind, 1, who)) continue;
        uint64_t r = rt_call(c, EQ_REMOVE_STAT, 3, eu, (uint64_t)s, 0ull) & 1;
        rt_log("coop: puppet: gear: w=%.2f %s remove s%d %s '%s' -> %llu", wsec(), who, s, kind_name[kind], h,
               (unsigned long long)r);
        gear_tried(gs, kind, GEAR_RETRY, who, "removal"), asked++;
    }
    if (match != gs->matched) {
        gs->matched = match;
        rt_log("coop: puppet: gear: w=%.2f %s %s Link after %u frames (%u requests so far; EU +0x12d0 %#x, requests "
               "j0 %#llx j4 %#llx)", wsec(), who, match ? "matches" : "differs from", since, gs->requests,
               LD32(eu + EU_INFLIGHT), (unsigned long long)LD64(eu + EU_REQ), (unsigned long long)LD64(eu + EU_REQ + 48));
    }
}

// Per puppet actor: by least recent use (3 remote puppets and the shadow).
static GearSync gear_sync[4];
static uint32_t gear_used[4];

static GearSync *gear_sync_for(uint64_t puppet) {
    int k = 0;
    for (int i = 0; i < 4; i++)
        if (gear_sync[i].puppet == puppet) { k = i; goto found; }
    for (int i = 1; i < 4; i++)
        if (gear_used[i] < gear_used[k]) k = i;
found:
    gear_used[k] = P.frames;
    return &gear_sync[k];
}

// Dresses a puppet as want says: called every frame while the puppet lives; it asks for nothing while the puppet
// matches, so it covers the puppet being made (or made again after an area change) and every change of Link's gear.
// The shadow test passes Link's gear of DELAY frames ago (shadow_gear), a remote player's puppet that player's gear.
static void puppet_dress(Cpu *c, uint64_t puppet, const Gear *want, const char *who) {
    if (!gear_mode || !puppet || !want) return;
    gear_apply(c, puppet, want, gear_sync_for(puppet), who, gear_mode == 2, gear_drawn);
}

// The shadow test's gear: Link's gear of each frame, kept as long as his poses (P.ring).
static Gear shadow_gear[RING];

// Test: NX2NAVI_COOP_PUPPET_GEAR_TEST="j0=Weapon_Sword_Master,s1=Armor_Adult_Goron,..." replaces those slots of
// the shadow's gear (j<slot>=<name>[:<state>] keeps Link's state, or sheathed when he has none there; s<slot>=<name>;
// an empty name empties the slot), to try gear Link does not have: an adult puppet in adult gear on a child's save.
static char gear_test[256];

static void gear_test_apply(Gear *g) {
    for (const char *p = gear_test; *p;) {
        const char *end = strchr(p, ',');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        char tok[80];
        snprintf(tok, sizeof tok, "%.*s", (int)(len < sizeof tok ? len : sizeof tok - 1), p);
        p += len + (end ? 1 : 0);
        char *eq = strchr(tok, '=');
        int slot = atoi(tok + 1);
        if (!eq || slot < 0) continue;
        char *name = eq + 1, *st = strchr(name, ':');
        if (st) *st++ = 0;
        if (tok[0] == 'j' && slot < 7) {
            snprintf(g->dyn[slot], GEAR_NAME, "%s", name);
            g->state[slot] = (uint8_t)(!name[0] ? 0 : st ? atoi(st) : g->state[slot] ? g->state[slot] : 2);
        } else if (tok[0] == 's' && slot < GEAR_STAT) {
            snprintf(g->stat[slot], GEAR_NAME, "%s", name);
        }
    }
}

// ---- the shadow (NX2NAVI_COOP_PUPPET_TEST=shadow[:<actor>[:<side metres>]]): a puppet that is the local Link of one
// second ago, side metres to his left then (so that the two stay apart while he stands): his transform, and on each
// AS slot the node he played and its frame. It is asked for 3 s after Link is found and released at 75 s.

static Puppet shadow_p;

static void shadow(Cpu *c, uint64_t player) {
    double w = wsec();
    Pose *now = &P.ring[P.player_frames % RING];
    pose_of(c, player, now);
    const Pose *then = P.player_frames > DELAY ? &P.ring[(P.player_frames - DELAY) % RING] : now;
    if (gear_mode) {
        gear_read(c, player, &shadow_gear[P.player_frames % RING]);
        if (gear_test[0]) gear_test_apply(&shadow_gear[P.player_frames % RING]);   // test
    }
    const Gear *gear_then = &shadow_gear[(P.player_frames > DELAY ? P.player_frames - DELAY : P.player_frames) % RING];
    float pos[3], rot[9];
    memcpy(rot, then->rot, sizeof rot);
    for (int k = 0; k < 3; k++) pos[k] = then->pos[k] + rot[k * 3] * P.side;   // the first column: his left
    Puppet *p = &shadow_p;
    if (p->state == SP_WAIT) {
        if (w < 3) return;
        char name[64];
        if (P.shadow_name[0]) snprintf(name, sizeof name, "%s", P.shadow_name);
        else puppet_name(name, sizeof name, is_adult(player));
        puppet_ask(c, p, name, pos, rot);
        return;
    }
    if (!puppet_step(c, p)) {
        uint32_t in = P.frames - p->asked_frame;
        if (p->state == SP_GONE && in > 180) {   // test: gone (a new area): ask again
            rt_log("coop: puppet: w=%.2f the shadow is asked for again", wsec());
            p->state = SP_WAIT, p->actor = 0;
            return;
        }
        if (p->state == SP_GONE && p->actor && in < 120 && in % 10 == 0)
            rt_log("coop: puppet: w=%.2f after the release of %s: readable %d, state %u", wsec(), p->s.text,
                   ok_ptr(p->actor, 0x400), ok_ptr(p->actor, 0x400) ? LD32(p->actor + A_STATE) : 0);
        return;
    }
    if (!p->bb_info) {   // test: the blackboards of Link and the puppet, once
        p->bb_info = 1;
        as_bb_info("Link", as_control(player));
        as_bb_info(p->s.text, as_control(p->actor));
    }
    if (P.bb_copy) as_bb_copy(c, as_control(player), as_control(p->actor));   // test: Link's arm layers
    puppet_pose(c, p, pos, rot, then, 1);
    puppet_dress(c, p->actor, gear_then, "shadow");
    uint64_t a = p->actor;
    if ((P.frames - p->asked_frame) % 60 == 0) {
        rt_log("coop: puppet: w=%.2f %s at %.2f %.2f %.2f (Link %.2f %.2f %.2f, a second ago %.2f %.2f %.2f)", wsec(),
               p->s.text, f32_at(a + A_POS), f32_at(a + A_POS + 4), f32_at(a + A_POS + 8), now->pos[0], now->pos[1],
               now->pos[2], then->pos[0], then->pos[1], then->pos[2]);
        log_as(c, "puppet", a, 1, P.menu_anim);
        if (P.bb_copy) as_bb_log(c, as_control(player), as_control(a));
        log_slots("Link", player);   // test (arms)
        log_slots(p->s.text, a);
    }
    (void)w;   // test: the shadow stays until the game removes it (it is asked for again after an area change)
}

// ---- the other players of a co-op room. Each sends its Link's pose (POS, coop_proto.h) about 15 times a second while
// it is in a room with others; a pose received is kept with its arrival time (the coop thread), and every frame the
// game thread shows each other player whose Link is in this Link's area as a puppet at his pose of about 100 ms ago,
// between the two poses received around then (their senders' clocks put on ours by the smallest delay seen). A
// puppet goes when its player leaves the area, the room or the world, or goes silent for 3 s. A pose counts only above
// the last one taken from the same member in the same session (coop_pos_fresh): the session layer can deliver a
// message again (coop.h), and an old pose must not move a puppet back while the player's own keep arriving. After 3 s
// with none taken, the next is taken whatever its counter, so a relay that held a player's datagrams back that long can
// replay the player's recorded poses: the puppet walks an old path, and nothing else (progress, saves) follows from it.
//
// POS v3: a POS also carries the sender's gear (when it changes, three POS in
// a row, and every 2 s), its colour, the colour it picked and its place in the room's join order (section "room
// colours"), and which of its AS slots 1..3 play nothing (the arms: a puppet's slot there plays slot 0's node, as in
// the shadow test). A remote puppet is dressed in its player's gear (puppet_dress) and tinted in its player's colour.
// NX2NAVI_COOP_POS_GEAR=0 sends no gear (the puppets then wear nothing); NX2NAVI_COOP_PUPPET_GEAR=0 dresses none.

#define HIST 8
#define SILENT_MS 3000
#define BEHIND_MS 100
#define GEAR_EVERY_MS 2000    // the gear goes again this often while it stays the same
#define GEAR_REPEAT 3         // POS in a row that carry a changed gear
typedef struct {
    CoopPos pose[HIST];     // the latest poses by pseq, newest at head
    uint64_t recv_ms[HIST];
    int n, head;
    int32_t offset;         // our clock minus the sender's, the smallest seen (rising 1 ms a message so it follows)
    CoopPosSeen seen;       // the sender's counter, per session
    CoopGear gear;          // POS v3: the sender's gear, from the last POS that carried it
    uint32_t gear_sig;      // its signature (0: none yet)
    uint64_t gear_session;  // the session it came in
    uint32_t got, got_gear; // poses taken, and those with gear (for the log)
} Remote;

static struct {
    PlatMutex lock;         // around r, session and the view's copy below
    Remote r[COOP_MAX_MEMBERS + 1];
    uint64_t session[COOP_MAX_MEMBERS + 1];   // each member's session, as the view lists it
    char name[COOP_MAX_MEMBERS + 1][COOP_NAME_MAX + 1];   // and its name
    int in_view[COOP_MAX_MEMBERS + 1];        // listed by the view now
    _Atomic int others;     // other members of the room this session is in (the view hook)
    _Atomic int self;
    _Atomic int in_room;    // joined (or reconnecting to) a room
    _Atomic int alive;      // puppets the game thread holds
    _Atomic unsigned epoch; // one more at every new room or leave: the join place starts again
    uint64_t epoch_ms;      // when the current room was first seen
    char room[COOP_SECRET_TEXT + 1];
    // the game thread's
    Puppet puppet[COOP_MAX_MEMBERS + 1];
    uint32_t pseq;
    uint32_t sent, sent_gear;
    uint32_t sent_sig;      // the gear signature sent last
    int gear_left;          // POS still to carry a changed gear
    uint64_t gear_at;       // when gear went last
    uint32_t logged_sig[COOP_MAX_MEMBERS + 1];   // the remote gear last logged, per member
    uint8_t logged_upper[COOP_MAX_MEMBERS + 1];
    int pos_gear;           // NX2NAVI_COOP_POS_GEAR (default 1)
} N;

// room colours (below): this player's place, pick and colour for the POS it sends, and the assignment every frame
static void room_colours(const Remote *r, const uint64_t *session, const int *in_view, char (*names)[COOP_NAME_MAX + 1],
                         uint64_t now);
static uint8_t room_colour_byte(void);
static uint8_t room_want_byte(void);
static uint16_t room_place(void);

static void on_pos(int from, const uint8_t *bytes, size_t n, void *user) {
    (void)user;
    CoopPos p;
    CoopGear g;
    if (from < 1 || from > COOP_MAX_MEMBERS || !coop_pos_parse_gear(bytes, n, &p, &g)) return;
    uint64_t now = plat_now_ms();
    plat_mutex_lock(&N.lock);
    Remote *r = &N.r[from];
    int fresh = coop_pos_fresh(&r->seen, N.session[from], p.pseq, now);
    if (fresh == 2) r->n = 0;   // another session's poses, or ones from before a silence: not this sender's now
    if (fresh) {   // a pose sent again or an older one is dropped
        int32_t off = (int32_t)((uint32_t)now - p.t_ms);
        r->offset = !r->n || off < r->offset ? off : r->offset + 1;
        r->head = (r->head + 1) % HIST;
        r->pose[r->head] = p, r->recv_ms[r->head] = now;
        if (r->n < HIST) r->n++;
        r->got++;
        if (p.flags & COOP_POS_GEAR) {
            r->gear = g, r->gear_sig = coop_gear_sig(&g), r->gear_session = N.session[from];
            r->got_gear++;
        }
    }
    plat_mutex_unlock(&N.lock);
}

static void on_view(const CoopView *v, void *user) {
    (void)user;
    int in_room = v->state == COOP_JOINED || v->state == COOP_RECONNECTING;
    int others = v->state == COOP_JOINED && v->n > 1 ? v->n - 1 : 0;
    N.others = others;
    N.self = v->self;
    plat_mutex_lock(&N.lock);
    if (!others) memset(N.r, 0, sizeof N.r);   // a new room starts from nothing
    memset(N.session, 0, sizeof N.session);
    memset(N.in_view, 0, sizeof N.in_view);
    for (int i = 0; i < v->n; i++) {
        int id = v->member[i].id;
        if (id < 1 || id > COOP_MAX_MEMBERS) continue;
        N.session[id] = v->member[i].session, N.in_view[id] = 1;
        snprintf(N.name[id], sizeof N.name[id], "%s", v->member[i].name);
    }
    if (v->self >= 1 && v->self <= COOP_MAX_MEMBERS) snprintf(N.name[v->self], sizeof N.name[v->self], "%s", v->name);
    if (!in_room ? N.in_room : strcmp(N.room, v->code) != 0) {   // a new room, or out of one: the place starts again
        snprintf(N.room, sizeof N.room, "%s", in_room ? v->code : "");
        N.epoch_ms = plat_now_ms();
        N.epoch++;
    }
    N.in_room = in_room;
    plat_mutex_unlock(&N.lock);
}

// Kills and difficulty (coop_kill.c, any thread): the other members in the game world now, by their POS: accepted and
// identified (coop_game_shown), heard within SILENT_MS, and their newest pose says IN_WORLD. -1 when no member's POS
// came at all (the puppets are off: no POS goes out), so that the caller counts the roster instead.
int coop_puppet_world_others(void) {
    if (!N.others) return 0;
    uint32_t shown = coop_game_shown();
    uint64_t now = plat_now_ms();
    int n = 0, heard = 0, self = N.self;
    plat_mutex_lock(&N.lock);
    for (int id = 1; id <= COOP_MAX_MEMBERS; id++) {
        const Remote *r = &N.r[id];
        if (id == self || !r->n) continue;
        heard = 1;
        if (shown >> id & 1 && now - r->recv_ms[r->head] < SILENT_MS && (r->pose[r->head].flags & COOP_POS_IN_WORLD)) n++;
    }
    plat_mutex_unlock(&N.lock);
    return heard ? n : -1;
}

// Kills tests (coop_kill.c, any thread): the other members whose newest POS (heard within SILENT_MS, accepted and
// identified, IN_WORLD) names this area.
int coop_puppet_area_others(const char *area) {
    if (!N.others || !area || !area[0]) return 0;
    uint32_t shown = coop_game_shown();
    uint64_t now = plat_now_ms();
    int n = 0, self = N.self;
    plat_mutex_lock(&N.lock);
    for (int id = 1; id <= COOP_MAX_MEMBERS; id++) {
        const Remote *r = &N.r[id];
        if (id == self || !r->n || !(shown >> id & 1) || now - r->recv_ms[r->head] >= SILENT_MS) continue;
        const CoopPos *q = &r->pose[r->head];
        if ((q->flags & COOP_POS_IN_WORLD) && !strcmp(q->area, area)) n++;
    }
    plat_mutex_unlock(&N.lock);
    return n;
}

static float lerp_angle(float a, float b, float t) {
    float d = fmodf(b - a + 3.14159265f, 6.28318531f);
    if (d < 0) d += 6.28318531f;
    return a + (d - 3.14159265f) * t;
}

// The pose of r shown now: between the two poses around BEHIND_MS ago (sender's clock), else the nearest one.
static void remote_pose(const Remote *r, uint32_t now, CoopPos *out) {
    uint32_t t = now - (uint32_t)r->offset - BEHIND_MS;
    const CoopPos *newest = &r->pose[r->head];
    *out = *newest;
    if ((int32_t)(t - newest->t_ms) >= 0) return;
    for (int k = 0; k + 1 < r->n; k++) {
        const CoopPos *b = &r->pose[(r->head - k + HIST) % HIST], *a = &r->pose[(r->head - k - 1 + HIST) % HIST];
        if ((int32_t)(t - a->t_ms) < 0) {
            *out = *a;   // older than every pose kept: the oldest
            continue;
        }
        float f = b->t_ms == a->t_ms ? 1.0f : (float)(int32_t)(t - a->t_ms) / (float)(int32_t)(b->t_ms - a->t_ms);
        *out = f < 0.5f ? *a : *b;
        for (int i = 0; i < 3; i++) out->pos[i] = a->pos[i] + (b->pos[i] - a->pos[i]) * f;
        out->yaw = lerp_angle(a->yaw, b->yaw, f);
        for (int i = 0; i < out->nslots && i < a->nslots && i < b->nslots; i++)
            if (!strcmp(a->slot[i].node, b->slot[i].node) && b->slot[i].frame >= a->slot[i].frame)
                out->slot[i].frame = a->slot[i].frame + (b->slot[i].frame - a->slot[i].frame) * f;
        return;
    }
}

static void yaw_rot(float yaw, float rot[9]) {
    float s = sinf(yaw), c = cosf(yaw);
    const float m[9] = {c, 0, s, 0, 1, 0, -s, 0, c};   // a turn about +y: the third column (forward) is (sin, 0, cos)
    memcpy(rot, m, sizeof m);
}

// The puppet's Gear (gear section) from a POS's gear block, and back.
static void gear_from_pos(const CoopGear *g, Gear *out) {
    memset(out, 0, sizeof *out);
    for (int j = 0; j < COOP_POS_GEAR_DYN && j < GEAR_DYN; j++) {
        snprintf(out->dyn[j], sizeof out->dyn[j], "%s", g->dyn[j]);
        out->state[j] = g->state[j];
    }
    for (int s = 0; s < COOP_POS_GEAR_STAT && s < GEAR_STAT; s++) snprintf(out->stat[s], sizeof out->stat[s], "%s", g->stat[s]);
}

static void gear_to_pos(const Gear *g, CoopGear *out) {
    memset(out, 0, sizeof *out);
    for (int j = 0; j < COOP_POS_GEAR_DYN && j < GEAR_DYN; j++) {
        if (!g->dyn[j][0] || !gear_name_ok(g->dyn[j])) continue;   // only the game's equipment actors go out
        snprintf(out->dyn[j], sizeof out->dyn[j], "%s", g->dyn[j]);
        out->state[j] = g->state[j] <= 2 ? g->state[j] : 0;
    }
    for (int s = 0; s < COOP_POS_GEAR_STAT && s < GEAR_STAT; s++)
        if (g->stat[s][0] && gear_name_ok(g->stat[s])) snprintf(out->stat[s], sizeof out->stat[s], "%s", g->stat[s]);
}

static void gear_log_line(const char *who, const char *what, const CoopGear *g, uint32_t sig) {
    char line[900];
    int o = snprintf(line, sizeof line, "coop: pos: w=%.2f %s %s (sig %08x):", wsec(), who, what, sig);
    for (int j = 0; j < COOP_POS_GEAR_DYN && o < (int)sizeof line - 64; j++)
        if (g->dyn[j][0]) o += snprintf(line + o, sizeof line - o, " j%d '%s' %u", j, g->dyn[j], g->state[j]);
    for (int s = 0; s < COOP_POS_GEAR_STAT && o < (int)sizeof line - 64; s++)
        if (g->stat[s][0]) o += snprintf(line + o, sizeof line - o, " s%d '%s'", s, g->stat[s]);
    rt_log("%s", line);
}

static void send_pose(Cpu *c, uint64_t player, const char *area) {
    CoopPos me;
    memset(&me, 0, sizeof me);
    me.pseq = ++N.pseq, me.t_ms = (uint32_t)plat_now_ms();
    CoopGear cg;
    int with_gear = 0;
    if (player) {
        Pose p;
        pose_of(c, player, &p);
        me.flags = (uint8_t)(COOP_POS_IN_WORLD | (is_adult(player) ? COOP_POS_ADULT : 0));
        snprintf(me.area, sizeof me.area, "%s", area);
        memcpy(me.pos, p.pos, sizeof me.pos);
        me.yaw = atan2f(p.rot[2], p.rot[8]);
        me.nslots = (uint8_t)p.nslots;
        for (int i = 0; i < p.nslots; i++) {
            snprintf(me.slot[i].node, sizeof me.slot[i].node, "%s", p.node[i]);
            me.slot[i].frame = p.frame[i];
            if (i >= 1 && i <= 3 && !p.node[i][0]) me.upper |= (uint8_t)(1u << (i - 1));
        }
        if (N.pos_gear) {
            Gear g;
            gear_read(c, player, &g);
            if (!P.test && gear_test[0]) {   // test: other gear than Link's goes out (NX2NAVI_COOP_PUPPET_GEAR_TEST)
                static int said;
                if (!said++) rt_log("coop: pos: the gear sent is changed by NX2NAVI_COOP_PUPPET_GEAR_TEST='%s'", gear_test);
                gear_test_apply(&g);
            }
            gear_to_pos(&g, &cg);
            me.gear_sig = coop_gear_sig(&cg);
            uint64_t now = plat_now_ms();
            if (me.gear_sig != N.sent_sig) {
                gear_log_line("Link", N.sent_sig ? "wears other gear: sent" : "gear sent", &cg, me.gear_sig);
                N.sent_sig = me.gear_sig, N.gear_left = GEAR_REPEAT;
            }
            if (N.gear_left > 0 || now - N.gear_at >= GEAR_EVERY_MS) {
                with_gear = 1, N.gear_at = now;
                if (N.gear_left > 0) N.gear_left--;
            }
        }
    }
    me.colour = room_colour_byte(), me.want = room_want_byte(), me.place = room_place();
    uint8_t buf[COOP_MAX_PAYLOAD];
    size_t n = coop_pos_pack_gear(buf, sizeof buf, &me, with_gear ? &cg : NULL);
    if (!n && with_gear) {   // names too long for one datagram: the pose without them
        static int said;
        if (!said++) rt_log("coop: pos: the gear does not fit a POS; sent without it");
        with_gear = 0;
        n = coop_pos_pack_gear(buf, sizeof buf, &me, NULL);
    }
    if (n && coop_send(COOP_K_POS, buf, n)) N.sent++, N.sent_gear += with_gear;
}

static void remotes(Cpu *c, uint64_t player, const char *area) {
    int others = N.others, self = N.self;
    static uint64_t pose_at;   // test: on a clock, not every 4th frame (the frame rate is unlimited)
    uint64_t pose_now = plat_now_ms();
    if (others && pose_now - pose_at >= 66) pose_at = pose_now, send_pose(c, player, area);   // 15 a second
    static Remote r[COOP_MAX_MEMBERS + 1];
    static uint64_t session[COOP_MAX_MEMBERS + 1];
    static int in_view[COOP_MAX_MEMBERS + 1];
    static char names[COOP_MAX_MEMBERS + 1][COOP_NAME_MAX + 1];
    plat_mutex_lock(&N.lock);
    memcpy(r, N.r, sizeof r);
    memcpy(session, N.session, sizeof session);
    memcpy(in_view, N.in_view, sizeof in_view);
    memcpy(names, N.name, sizeof names);
    plat_mutex_unlock(&N.lock);
    uint64_t now = plat_now_ms();
    room_colours(r, session, in_view, names, now);
    int alive = 0;
    uint32_t shown = coop_game_shown();   // members accepted and identified (coop_offer_shown)
    static uint64_t stats_at;
    if (others && now - stats_at >= 10000) {
        stats_at = now;
        char line[400];
        int o = snprintf(line, sizeof line, "coop: pos: w=%.2f sent %u (%u with gear);", wsec(), N.sent, N.sent_gear);
        for (int id = 1; id <= COOP_MAX_MEMBERS && o < (int)sizeof line - 80; id++)
            if (id != self && in_view[id])
                o += snprintf(line + o, sizeof line - o, " member %d '%s': %u taken (%u with gear)%s%s", id, names[id],
                              r[id].got, r[id].got_gear, shown >> id & 1 ? "" : ", not shown (not accepted or identified)",
                              r[id].n ? "" : ", none yet");
        rt_log("%s", line);
    }
    for (int id = 1; id <= COOP_MAX_MEMBERS; id++) {
        Puppet *p = &N.puppet[id];
        CoopPos pose = {0};
        int trusted = shown >> id & 1;
        int heard = others && id != self && trusted && r[id].n && now - r[id].recv_ms[r[id].head] < SILENT_MS;
        if (heard) remote_pose(&r[id], (uint32_t)now, &pose);
        int here = heard && player && area[0] && (pose.flags & COOP_POS_IN_WORLD) && !strcmp(pose.area, area);
        int scene = here && coop_cutscene_hides_puppets();   // no puppet stands in a story cutscene here
        int want = here && !scene;
        char name[64] = "";
        if (want) puppet_name(name, sizeof name, pose.flags & COOP_POS_ADULT);
        if ((p->state == SP_ASKED || p->state == SP_FOLLOW) && (!want || strcmp(p->s.text, name))) {
            rt_log("coop: puppet: member %d's puppet goes (%s)", id,
                   !trusted ? "not accepted or not identified" : !heard ? "not heard" : scene ? "a cutscene plays here"
                   : !want ? "not in this area" : "other age");
            puppet_release(c, p);
        }
        if (want && p->state != SP_ASKED && p->state != SP_FOLLOW && (p->state == SP_WAIT || P.frames - p->asked_frame > 120)) {
            float pos[3], rot[9];
            memcpy(pos, pose.pos, sizeof pos);
            yaw_rot(pose.yaw, rot);
            rt_log("coop: puppet: member %d is in %s: asking for %s", id, area, name);
            puppet_ask(c, p, name, pos, rot);
        }
        if (puppet_step(c, p)) {
            float rot[9];
            yaw_rot(pose.yaw, rot);
            Pose anim = {.nslots = pose.nslots};
            const CoopPos *newest = &r[id].pose[r[id].head];
            for (int i = 0; i < pose.nslots && i < SLOTS; i++) {
                snprintf(anim.node[i], sizeof anim.node[i], "%s", pose.slot[i].node);
                anim.frame[i] = pose.slot[i].frame;
                // POS v3: the sender's slots 1..3 that play nothing stay empty here, so that puppet_pose has the
                // puppet's slot there play slot 0's node (the arms), whatever an older pose between them said
                if (i >= 1 && i <= 3 && newest->upper >> (i - 1) & 1) anim.node[i][0] = 0;
            }
            if (newest->upper != N.logged_upper[id]) {
                N.logged_upper[id] = newest->upper;
                rt_log("coop: pos: w=%.2f member %d's upper slots: %s%s%s (slot 1 '%s')", wsec(), id,
                       newest->upper & 1 ? "1 empty" : "1 plays", newest->upper & 2 ? ", 2 empty" : ", 2 plays",
                       newest->upper & 4 ? ", 3 empty" : ", 3 plays", newest->slot[1].node);
            }
            char who[24];
            snprintf(who, sizeof who, "member %d", id);
            puppet_pose(c, p, pose.pos, rot, &anim, 1);
            if (r[id].gear_sig && r[id].gear_session == session[id]) {
                if (r[id].gear_sig != N.logged_sig[id]) {
                    N.logged_sig[id] = r[id].gear_sig;
                    gear_log_line(who, "gear received", &r[id].gear, r[id].gear_sig);
                }
                Gear g;
                gear_from_pos(&r[id].gear, &g);
                puppet_dress(c, p->actor, &g, who);
            }
            if ((P.frames - p->asked_frame) % 300 == 0) {
                rt_log("coop: puppet: member %d's puppet at %.2f %.2f %.2f, slot 0 '%s' %.1f", id, pose.pos[0],
                       pose.pos[1], pose.pos[2], anim.node[0], anim.frame[0]);
                log_slots(who, p->actor);
            }
        }
        alive += p->state == SP_ASKED || p->state == SP_FOLLOW;
    }
    N.alive = alive;
}

// ---- colours: every Link's tunic and cap in his player's colour (green, red, blue, purple),
// the local Link's own included,
// through the game's own per-material shader parameters.
//
// What draws a Link is the model set of his actor, f_7000974de0(actor): +0x20 s32 count, +0x28 pointers to entries
// whose first u64 is a model unit; also the sets listed at *(*(actor+0x270)+0x1d0) (+0x20 count, +0x28 entries of 0x28
// bytes, the set first): the sets the GameModelController's hit flash walks (f_7002e1f060). A model unit (one FMDL:
// Player_Child, Player_Adult, Armor_Adult_Goron, Armor_Adult_Zora) answers, by its vtable:
//   +0x28  name() -> const char *                   +0xc8  material_count()
//   +0xd0  find_material(const char **name) -> int  +0xe0  find_param(int mat, const char **name) -> int (-1: none)
//   +0x128 set_float(int mat, int param, s0)        +0x168 set_vec4(int mat, int param, const float *v)
// A name goes in as the address of a cell that holds the C string's pointer (f_70031cfd00, f_70009656a0,
// main+0x2642714). The values are the unit's own: another Link with the same model keeps his.
//
// What shows on Link's "model" materials (test runs): p_model_blend_color's alpha darkens the
// material towards black, but its colour shows only where a material has an emission mask (the eyes glow in it; the
// tunic just goes dark); p_death_petrified_* and p_death_burned_ratio show nothing (that program has no death
// effects: the shading model's static option o_special_material_behave); the expression colours ns_param_color0..2 show
// nothing. The warp glow does: p_warp_emission_color0 = (r, g, b, 1) and p_warp_emission_intensity light exactly the
// material in the colour, skin and hair untouched. So the tunic and cap get a faint glow in the player's colour over
// their own fabric, darkened first by p_model_blend_color's alpha so that the green does not shine through: the
// fabric's folds and weave stay. The game writes p_model_blend_color for the hit flash and status tints (every
// material, f_70031cfd00, when the controller's colours change) and the warp glow when Link warps; the colour steps
// aside for both (the controller's flash alpha, and a warp intensity that someone else changed), and is written every
// frame otherwise, from the frame hook (the game's update thread, before the scene update).
//
// NX2NAVI_COOP_COLOUR_SELF=<green|red|blue|purple>  the local Link's colour (test: there is no room colour
//     assignment yet; unset: none). Members' puppets take CL.member[id], none until the room assigns colours.
// NX2NAVI_COOP_COLOUR_TEST=<colour>  the shadow puppet's colour (NX2NAVI_COOP_PUPPET_TEST=shadow).
// NX2NAVI_COOP_COLOUR_STRENGTH  the glow's intensity (default 0.03), NX2NAVI_COOP_COLOUR_DARK  how much the fabric is
//     darkened under it (0..1, default 0.8; the Goron and Zora tunics at least 0.92; 0 darkens nothing).
// NX2NAVI_COOP_COLOUR_PARAM (test): warp (the default, above); blend = p_model_blend_color (r, g, b, strength);
//     petrify = p_death_petrified_color (r, g, b, 1), _albedo_affect_ratio strength, _ratio 1; body = the whole model
//     through the controller's extra colour (main+0x2e1d7e0), skin and hair too; none = only the probes below.
// NX2NAVI_COOP_COLOUR_RGB="red:1,0.05,0.03;blue:..." (test: other colours), NX2NAVI_COOP_COLOUR_NATIVE=1 (test: also
// colour a tunic that already has the colour: green Kokiri, red Goron, blue Zora; by default those keep their art),
// NX2NAVI_COOP_COLOUR_DIAG=1 (test: each tint material's parameters, used or not, and their values, logged).
// Nothing is written unless a colour is set; a colour taken away puts the file's values back.

#define CL_MODEL_SET (MAIN + 0x974de0)    // (actor) -> its model set, or 0
#define CL_GMC (MAIN + 0xc71c20)          // (actor) -> its GameModelController, or 0
#define CL_GMC_EXTRA (MAIN + 0x2e1d7e0)   // (controller, const float rgba[4]): the whole model's extra colour
#define MU_NAME 0x28
#define MU_NMAT 0xc8
#define MU_FINDMAT 0xd0
#define MU_FINDPRM 0xe0
#define MU_SETF 0x128
#define MU_SETV4 0x168

enum { COL_NONE = -1, COL_GREEN, COL_RED, COL_BLUE, COL_PURPLE, COL_N };
static const char *const col_name[COL_N] = {"green", "red", "blue", "purple"};
// The glow's colours, linear RGB of about the same luminance (0.25), so that no colour glows brighter than another.
// Blue leans to cyan and purple to magenta, so that the two stay apart on a red Goron tunic.
static float col_rgb[COL_N][3] = {{0.04f, 0.34f, 0.06f}, {1.0f, 0.05f, 0.03f}, {0.03f, 0.30f, 1.0f},
                                  {0.70f, 0.05f, 0.90f}};

enum { CP_WARP, CP_BLEND, CP_PETRIFY, CP_BODY, CP_NONE };
static const char *const cp_name[] = {"warp", "blend", "petrify", "body", "none"};

// Test: NX2NAVI_COOP_COLOUR_PROBE (Link) and _PROBE_TEST (the shadow) write any shader parameters every frame,
// "[<material>:]<param>=<v>[,<v>,<v>,<v>];...": one value with the float setter, four with the vec4 one (a float4
// parameter only); without a material on every tint material of the unit. To find what shows a colour.
#define NPROBE 6
typedef struct {
    char mat[40], prm[64];
    int n;
    float v[4];
} Probe;
static Probe probe[2][NPROBE];
static int nprobe[2];

// The tunic and cap materials per model (found by their bones, hidden groups and texture colours), the colour
// the model's tunic already has, and the least it is darkened under the glow (the Goron and Zora tunics are darker
// and more saturated than the Kokiri green, which shows through: a blue Goron tunic turned violet at 0.8).
typedef struct {
    const char *model;
    int native;
    float dark_min;
    const char *mat[6];
} TintSet;
static const TintSet tint_sets[] = {
    {"Player_Child", COL_GREEN, 0, {"Mt_Cloth", "Mt_Cloth_Parts", "Mt_Cloth_Parts_AlphaMask", "Mt_Vest", 0}},
    {"Player_Adult", COL_GREEN, 0, {"Mt_Angin", "Mt_Angin_Acs", "Mt_Angin_Acs_AlphaMask", "Mt_Vest", 0}},
    {"Armor_Adult_Goron", COL_RED, 0.92f, {"Mt_Leather", "Mt_Vest", "Mt_HoodAcs", 0}},
    {"Armor_Adult_Zora", COL_BLUE, 0.92f, {"Mt_Angin", "Mt_Angin_Acs", "Mt_Vest", 0}},
};
#define NTINT ((int)(sizeof tint_sets / sizeof tint_sets[0]))

enum { PR_BLEND, PR_PCOLOR, PR_PRATIO, PR_PAFFECT, PR_WCOLOR, PR_WINT, PR_N };
static const char *const prm_name[PR_N] = {"p_model_blend_color", "p_death_petrified_color", "p_death_petrified_ratio",
                                           "p_death_petrified_albedo_affect_ratio", "p_warp_emission_color0",
                                           "p_warp_emission_intensity"};

static struct {
    _Atomic int self;                    // colours: the local Link (the room's assignment in a room, else self_env)
    int test;                            // the shadow puppet
    int self_env;                        // NX2NAVI_COOP_COLOUR_SELF (test: the local Link outside a room)
    int member[COOP_MAX_MEMBERS + 1];    // a member's puppet: the room's assignment (section "room colours")
    int param, native, diag;   // diag: test, NX2NAVI_COOP_COLOUR_DIAG=1 logs the parameters' values
    float strength, dark;
    _Atomic int dirty;                   // units still carry a colour (the hook stays on to take it away)
    uint32_t frames, writes, skips, yields;
} CL = {.self = COL_NONE, .test = COL_NONE, .self_env = COL_NONE, .member = {[0 ... COOP_MAX_MEMBERS] = COL_NONE},
        .param = CP_WARP, .strength = 0.03f, .dark = 0.8f};

// A model unit looked at: its tunic materials and their parameters, and what it carries now.
typedef struct {
    uint64_t unit, vt;
    const TintSet *ts;
    int nmat, mat[6], prm[6][PR_N];
    int colour, param;    // COL_NONE: the file's values
    uint32_t used;
    uint32_t first;       // test: CL.frames at the first write (the diagnostics follow it)
    float wlast, wseen;   // the warp intensity this file wrote last, and the one found before this frame's write
    int wrote, wseen_ok;
    uint32_t yield_until; // CL.frames until which the game's own warp glow has the unit
    int pres[2], pmat[2][NPROBE], pidx[2][NPROBE][6];   // test: the probes' materials and parameters
} UnitCol;
#define UNITS 16
static UnitCol ucol[UNITS];

static uint64_t vfn(uint64_t obj, unsigned slot) { return LD64(LD64(obj) + slot); }

static int col_parse(const char *s, const char *key) {
    if (!s || !*s || !strcmp(s, "none") || !strcmp(s, "off") || !strcmp(s, "0")) return COL_NONE;
    char low[16];
    size_t i = 0;
    for (; s[i] && i + 1 < sizeof low; i++) low[i] = (char)(s[i] >= 'A' && s[i] <= 'Z' ? s[i] + 32 : s[i]);
    low[i] = 0;
    for (int k = 0; k < COL_N; k++)
        if (!strcmp(low, col_name[k])) return k;
    rt_log("coop: colour: %s='%s' is no colour (green, red, blue, purple): none", key, s);
    return COL_NONE;
}

// NX2NAVI_COOP_COLOUR_RGB="<colour>:r,g,b;...": test values for the colours.
static void col_rgb_parse(const char *s) {
    for (const char *p = s; p && *p;) {
        char name[16] = "";
        float r, g, b;
        if (sscanf(p, "%15[^:]:%f,%f,%f", name, &r, &g, &b) == 4) {
            int k = col_parse(name, "NX2NAVI_COOP_COLOUR_RGB");
            if (k != COL_NONE) col_rgb[k][0] = r, col_rgb[k][1] = g, col_rgb[k][2] = b;
        }
        p = strchr(p, ';');
        if (p) p++;
    }
}

static int32_t mu_find(Cpu *c, uint64_t unit, int mat, const char *name) {
    static const char *cell;   // the guest reads the name's pointer from here
    cell = name;
    uint64_t r = mat < 0 ? rt_call(c, vfn(unit, MU_FINDMAT), 2, unit, (uint64_t)(uintptr_t)&cell)
                         : rt_call(c, vfn(unit, MU_FINDPRM), 3, unit, (uint64_t)(uint32_t)mat, (uint64_t)(uintptr_t)&cell);
    return (int32_t)r;
}

static void mu_vec4(Cpu *c, uint64_t unit, int mat, int prm, const float v[4]) {
    static float keep[4] __attribute__((aligned(16)));   // the setter copies it
    memcpy(keep, v, sizeof keep);
    rt_call(c, vfn(unit, MU_SETV4), 4, unit, (uint64_t)(uint32_t)mat, (uint64_t)(uint32_t)prm, (uint64_t)(uintptr_t)keep);
}

static void mu_float(Cpu *c, uint64_t unit, int mat, int prm, float f) {
    uint32_t bits;
    memcpy(&bits, &f, 4);
    c->v[0] = bits;   // s0, as as_frame_set passes it
    rt_call(c, vfn(unit, MU_SETF), 3, unit, (uint64_t)(uint32_t)mat, (uint64_t)(uint32_t)prm);
}

static void probe_parse(int side, const char *s) {
    nprobe[side] = 0;
    for (const char *p = s; p && *p && nprobe[side] < NPROBE;) {
        const char *end = strchr(p, ';');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        char tok[160];
        snprintf(tok, sizeof tok, "%.*s", (int)(len < sizeof tok ? len : sizeof tok - 1), p);
        p += len + (end ? 1 : 0);
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq = 0;
        Probe *q = &probe[side][nprobe[side]];
        memset(q, 0, sizeof *q);
        char *colon = strchr(tok, ':');
        if (colon) *colon = 0;
        snprintf(q->mat, sizeof q->mat, "%s", colon ? tok : "");
        snprintf(q->prm, sizeof q->prm, "%s", colon ? colon + 1 : tok);
        q->n = sscanf(eq + 1, "%f,%f,%f,%f", &q->v[0], &q->v[1], &q->v[2], &q->v[3]);
        if (q->n == 1 || q->n == 4) {
            rt_log("coop: colour: probe for %s: %s%s%s = %.3f %.3f %.3f %.3f (%d values)", side ? "the shadow" : "Link",
                   q->mat, q->mat[0] ? ":" : "", q->prm, q->v[0], q->v[1], q->v[2], q->v[3], q->n);
            nprobe[side]++;
        }
    }
}

// Where a parameter of a unit's material lives (set vec4 main+0x3596050): the material instance is
// *(*(unit+0x110)+8*mat), or *(unit+0x108)+0x80*mat when +0x110 is 0; its resource *(inst) maps the parameter to the
// shader's block (+0x60: s32 per parameter, < 0: the program does not use it), the shading model's records (+0x10 ->
// +0 -> +0x20, 0x18 bytes each, +0x10 the u16 offset) place it in the instance's values at *(inst+0x48).
static int mu_where(uint64_t unit, int mat, int prm, int32_t *map, uint64_t *val) {
    *map = -2, *val = 0;
    uint64_t t = LD64(unit + 0x110);
    uint64_t inst = t ? (ok_ptr(t + 8ull * (unsigned)mat, 8) ? LD64(t + 8ull * (unsigned)mat) : 0)
                      : LD64(unit + 0x108) + 0x80ull * (unsigned)mat;
    if (!ok_ptr(inst, 0x50)) return 0;
    uint64_t res = LD64(inst);
    if (!ok_ptr(res, 0x68) || !ok_ptr(LD64(res + 0x60), 4ull * (unsigned)prm + 4)) return 0;
    *map = (int32_t)LD32(LD64(res + 0x60) + 4ull * (unsigned)prm);
    uint64_t sh = ok_ptr(LD64(res + 0x10), 8) ? LD64(LD64(res + 0x10)) : 0;
    uint64_t rec = ok_ptr(sh, 0x28) ? LD64(sh + 0x20) : 0;
    if (!ok_ptr(rec + 0x18ull * (unsigned)prm, 0x18)) return 0;
    uint64_t v = LD64(inst + 0x48) + LD16(rec + 0x18ull * (unsigned)prm + 0x10);
    if (!ok_ptr(v, 16)) return 0;
    *val = v;
    return 1;
}

// Test: each tint material's parameters: used by the program (map >= 0), offset and value now.
static void unit_diag(UnitCol *u, const char *who, const char *when) {
    for (int k = 0; k < u->nmat; k++) {
        if (u->mat[k] < 0) continue;
        char line[400];
        int o = snprintf(line, sizeof line, "coop: colour: %s %s %s#%d %s:", who, u->ts->model, u->ts->mat[k], u->mat[k], when);
        for (int p = 0; p < PR_N && o < (int)sizeof line; p++) {
            int32_t map;
            uint64_t v;
            if (u->prm[k][p] < 0 || !mu_where(u->unit, u->mat[k], u->prm[k][p], &map, &v)) {
                o += snprintf(line + o, sizeof line - o, " %s ?", prm_name[p] + 2);
                continue;
            }
            o += snprintf(line + o, sizeof line - o, " %s map %d = %.3f %.3f %.3f %.3f", prm_name[p] + 2, map, f32_at(v),
                          p == PR_BLEND || p == PR_PCOLOR ? f32_at(v + 4) : 0.0f,
                          p == PR_BLEND || p == PR_PCOLOR ? f32_at(v + 8) : 0.0f,
                          p == PR_BLEND || p == PR_PCOLOR ? f32_at(v + 12) : 0.0f);
        }
        rt_log("%s", line);
    }
}

static UnitCol *unit_col(Cpu *c, uint64_t unit, const TintSet *ts, const char *who) {
    uint64_t vt = LD64(unit);
    UnitCol *u = &ucol[0];
    for (int i = 0; i < UNITS; i++) {
        if (ucol[i].unit == unit && ucol[i].vt == vt && ucol[i].ts == ts) {
            ucol[i].used = CL.frames;
            return &ucol[i];
        }
        if (ucol[i].used < u->used) u = &ucol[i];
    }
    memset(u, 0, sizeof *u);
    u->unit = unit, u->vt = vt, u->ts = ts, u->used = CL.frames, u->colour = COL_NONE, u->param = CL.param;
    int nm = (int32_t)rt_call(c, vfn(unit, MU_NMAT), 1, unit);
    char line[640];
    int o = snprintf(line, sizeof line, "coop: colour: w=%.2f %s unit '%s' %#llx (vtable main+%#llx), %d materials:", wsec(),
                     who, ts->model, (unsigned long long)unit, (unsigned long long)(vt - MAIN), nm);
    for (int k = 0; k < 6 && ts->mat[k]; k++) {
        int m = nm > 0 ? mu_find(c, unit, -1, ts->mat[k]) : -1;
        if (m >= nm) m = -1;
        u->mat[u->nmat] = m;
        for (int p = 0; p < PR_N; p++) u->prm[u->nmat][p] = m >= 0 ? mu_find(c, unit, m, prm_name[p]) : -1;
        if (o < (int)sizeof line)
            o += snprintf(line + o, sizeof line - o, " %s#%d(glow %d/%d, blend %d, petrified %d/%d/%d)", ts->mat[k], m,
                          u->prm[u->nmat][PR_WCOLOR], u->prm[u->nmat][PR_WINT], u->prm[u->nmat][PR_BLEND],
                          u->prm[u->nmat][PR_PCOLOR], u->prm[u->nmat][PR_PRATIO], u->prm[u->nmat][PR_PAFFECT]);
        u->nmat++;
    }
    rt_log("%s", line);
    if (CL.diag) unit_diag(u, who, "before");
    static int vt_logged;
    if (!vt_logged++)
        rt_log("coop: colour: the unit's calls: count main+%#llx, find material main+%#llx, find param main+%#llx, "
               "set float main+%#llx, set vec4 main+%#llx", (unsigned long long)(vfn(unit, MU_NMAT) - MAIN),
               (unsigned long long)(vfn(unit, MU_FINDMAT) - MAIN), (unsigned long long)(vfn(unit, MU_FINDPRM) - MAIN),
               (unsigned long long)(vfn(unit, MU_SETF) - MAIN), (unsigned long long)(vfn(unit, MU_SETV4) - MAIN));
    return u;
}

// Puts the file's values back on what the unit carries (u->param's parameter).
static void unit_clear(Cpu *c, UnitCol *u) {
    static const float zero[4] = {0, 0, 0, 0}, stone[4] = {0.5f, 0.46f, 0.38f, 1};   // the material files' values
    for (int k = 0; k < u->nmat; k++) {
        int m = u->mat[k], *p = u->prm[k];
        if (m < 0) continue;
        if ((u->param == CP_BLEND || u->param == CP_WARP) && p[PR_BLEND] >= 0) mu_vec4(c, u->unit, m, p[PR_BLEND], zero);
        if (u->param == CP_WARP && p[PR_WCOLOR] >= 0 && p[PR_WINT] >= 0) {
            static const float white[4] = {1, 1, 1, 1};   // file values
            mu_vec4(c, u->unit, m, p[PR_WCOLOR], white);
            mu_float(c, u->unit, m, p[PR_WINT], 0);
        }
        if (u->param == CP_PETRIFY && p[PR_PCOLOR] >= 0 && p[PR_PRATIO] >= 0) {
            mu_float(c, u->unit, m, p[PR_PRATIO], 0);
            mu_vec4(c, u->unit, m, p[PR_PCOLOR], stone);
            if (p[PR_PAFFECT] >= 0) mu_float(c, u->unit, m, p[PR_PAFFECT], 0.1726f);
        }
    }
    u->colour = COL_NONE, u->wrote = 1, u->wlast = 0;
}

// The game's own warp glow (Farore's Wind, the warp songs) writes p_warp_emission_* on every material: when the
// intensity found before this frame's write is not the one the last write left (or, after none, the one seen the frame
// before), the game is writing it, and the colour waits until the value has been still for 30 frames.
static int warp_yield(UnitCol *u, const char *who) {
    int32_t map;
    uint64_t a;
    if (u->mat[0] < 0 || u->prm[0][PR_WINT] < 0 || !mu_where(u->unit, u->mat[0], u->prm[0][PR_WINT], &map, &a)) return 0;
    float now = f32_at(a), expect = u->wrote ? u->wlast : u->wseen;
    if (u->wseen_ok && fabsf(now - expect) > 1e-5f) {
        if (CL.frames >= u->yield_until)
            rt_log("coop: colour: w=%.2f %s: the game writes the warp glow (%.3f, expected %.3f): the colour waits", wsec(),
                   who, now, expect);
        u->yield_until = CL.frames + 30;
    }
    u->wseen = now, u->wseen_ok = 1;
    if (CL.frames < u->yield_until) {
        u->wrote = 0;
        CL.yields++;
        return 1;
    }
    return 0;
}

static void unit_write(Cpu *c, UnitCol *u, int colour, int flash, const char *who) {
    const float *rgb = col_rgb[colour];
    if (CL.param == CP_WARP && warp_yield(u, who)) return;
    for (int k = 0; k < u->nmat; k++) {
        int m = u->mat[k], *p = u->prm[k];
        if (m < 0) continue;
        if (CL.param == CP_WARP && p[PR_WCOLOR] >= 0 && p[PR_WINT] >= 0) {
            const float v[4] = {rgb[0], rgb[1], rgb[2], 1};
            mu_vec4(c, u->unit, m, p[PR_WCOLOR], v);
            mu_float(c, u->unit, m, p[PR_WINT], CL.strength);
            CL.writes += 2;
            if (!flash && CL.dark > 0 && p[PR_BLEND] >= 0) {   // the fabric darkened under the glow
                const float d[4] = {0, 0, 0, CL.dark > u->ts->dark_min || !CL.dark ? CL.dark : u->ts->dark_min};
                mu_vec4(c, u->unit, m, p[PR_BLEND], d);
                CL.writes++;
            }
        } else if (CL.param == CP_BLEND && p[PR_BLEND] >= 0 && !flash) {
            const float v[4] = {rgb[0], rgb[1], rgb[2], CL.strength};
            mu_vec4(c, u->unit, m, p[PR_BLEND], v);
            CL.writes++;
        } else if (CL.param == CP_PETRIFY && p[PR_PCOLOR] >= 0 && p[PR_PRATIO] >= 0) {
            const float v[4] = {rgb[0], rgb[1], rgb[2], 1};
            mu_vec4(c, u->unit, m, p[PR_PCOLOR], v);
            if (p[PR_PAFFECT] >= 0) mu_float(c, u->unit, m, p[PR_PAFFECT], CL.strength);
            mu_float(c, u->unit, m, p[PR_PRATIO], 1.0f);
            CL.writes += 3;
        }
    }
    u->colour = colour, u->param = CL.param;
    if (CL.param == CP_WARP) u->wrote = 1, u->wlast = CL.strength;
}

static void probe_write(Cpu *c, uint64_t unit, int mat, int prm, const Probe *q) {
    if (mat < 0 || prm < 0) return;
    if (q->n == 4) mu_vec4(c, unit, mat, prm, q->v);
    else mu_float(c, unit, mat, prm, q->v[0]);
    CL.writes++;
}

static void unit_probe(Cpu *c, UnitCol *u, int side, const char *who) {
    if (side < 0 || side > 1 || !nprobe[side]) return;
    if (!u->pres[side]) {
        u->pres[side] = 1;
        for (int k = 0; k < nprobe[side]; k++) {
            const Probe *q = &probe[side][k];
            if (q->mat[0]) {
                int m = mu_find(c, u->unit, -1, q->mat);
                u->pmat[side][k] = m;
                u->pidx[side][k][0] = m >= 0 ? mu_find(c, u->unit, m, q->prm) : -1;
                rt_log("coop: colour: %s probe %s:%s -> material %d, param %d", who, q->mat, q->prm, m, u->pidx[side][k][0]);
            } else {
                u->pmat[side][k] = -1;
                for (int j = 0; j < u->nmat; j++) u->pidx[side][k][j] = u->mat[j] >= 0 ? mu_find(c, u->unit, u->mat[j], q->prm) : -1;
                rt_log("coop: colour: %s probe %s -> param %d on the tint materials", who, q->prm, u->pidx[side][k][0]);
            }
        }
    }
    for (int k = 0; k < nprobe[side]; k++) {
        const Probe *q = &probe[side][k];
        if (q->mat[0]) probe_write(c, u->unit, u->pmat[side][k], u->pidx[side][k][0], q);
        else
            for (int j = 0; j < u->nmat; j++) probe_write(c, u->unit, u->mat[j], u->pidx[side][k][j], q);
    }
}

// The model units of an actor: its model set's and those of the sets at components +0x1d0. 0 when it has none.
static int actor_units(Cpu *c, uint64_t actor, uint64_t *units, int cap, int *nsets) {
    uint64_t sets[8];
    int ns = 0, n = 0;
    uint64_t s0 = rt_call(c, CL_MODEL_SET, 1, actor);
    if (ok_ptr(s0, 0x30)) sets[ns++] = s0;
    uint64_t comps = LD64(actor + A_COMPONENTS);
    uint64_t list = ok_ptr(comps, 0x1d8) ? LD64(comps + 0x1d0) : 0;
    if (ok_ptr(list, 0x30) && LD64(list + 0x28)) {
        int32_t cnt = (int32_t)LD32(list + 0x20);
        uint64_t arr = LD64(list + 0x28);
        for (int32_t i = 0; i < cnt && ns < 8; i++) {
            if (!ok_ptr(arr + 0x28ull * (uint64_t)i, 8)) break;
            uint64_t s = LD64(arr + 0x28ull * (uint64_t)i);
            if (ok_ptr(s, 0x30)) sets[ns++] = s;
        }
    }
    for (int k = 0; k < ns; k++) {
        int32_t cnt = (int32_t)LD32(sets[k] + 0x20);
        uint64_t arr = LD64(sets[k] + 0x28);
        for (int32_t i = 0; i < cnt && i < 16 && n < cap; i++) {
            if (!ok_ptr(arr + 8ull * (uint64_t)i, 8)) break;
            uint64_t ent = LD64(arr + 8ull * (uint64_t)i);
            uint64_t unit = ok_ptr(ent, 8) ? LD64(ent) : 0;
            if (ok_ptr(unit, 0x10) && ok_ptr(LD64(unit), 0x170)) units[n++] = unit;
        }
    }
    if (nsets) *nsets = ns;
    return n;
}

// Once per actor: its model sets' units, for the log.
static void actor_info(Cpu *c, uint64_t actor, const char *who) {
    static uint64_t seen[8];
    static int next;
    for (int i = 0; i < 8; i++)
        if (seen[i] == actor) return;
    seen[next] = actor, next = (next + 1) % 8;
    uint64_t units[16];
    int ns = 0, n = actor_units(c, actor, units, 16, &ns);
    char an[64], line[512];
    int o = snprintf(line, sizeof line, "coop: colour: w=%.2f %s actor %#llx '%s': %d model sets, %d units:", wsec(),
                     who, (unsigned long long)actor, text_at(LD64(actor + A_DEF_NAME), an, sizeof an), ns, n);
    for (int i = 0; i < n && o < (int)sizeof line; i++) {
        char nm[64];
        text_at(rt_call(c, vfn(units[i], MU_NAME), 1, units[i]), nm, sizeof nm);
        o += snprintf(line + o, sizeof line - o, " '%s' (%d materials)", nm,
                      (int32_t)rt_call(c, vfn(units[i], MU_NMAT), 1, units[i]));
    }
    rt_log("%s", line);
}

// Tints (or clears, colour COL_NONE) every tunic unit of one actor: a Link, a puppet, or an armour bound to either.
static void colour_actor(Cpu *c, uint64_t actor, int colour, const char *who, int side) {
    if (!actor || !ok_ptr(actor, 0x400) || LD32(actor + A_STATE) != 4) return;
    uint64_t gmc = rt_call(c, CL_GMC, 1, actor);
    if (CL.param == CP_BODY) {   // test: the whole model, through the controller (a no-op when unchanged)
        if (!ok_ptr(gmc, 0x130)) return;
        static float keep[4] __attribute__((aligned(16)));
        const float *rgb = colour != COL_NONE ? col_rgb[colour] : NULL;
        keep[0] = rgb ? rgb[0] : 0, keep[1] = rgb ? rgb[1] : 0, keep[2] = rgb ? rgb[2] : 0;
        keep[3] = rgb ? CL.strength : 0;
        rt_call(c, CL_GMC_EXTRA, 2, gmc, (uint64_t)(uintptr_t)keep);
        return;
    }
    // While the controller flashes the actor (its damage or extra colour has alpha) it writes p_model_blend_color on
    // every material itself: the flash shows, and the tint comes back the frame after.
    int flash = (CL.param == CP_BLEND || CL.param == CP_WARP) && ok_ptr(gmc, 0x40) &&
                (f32_at(gmc + 0x2c) > 0 || f32_at(gmc + 0x3c) > 0);
    if (colour != COL_NONE) actor_info(c, actor, who);
    uint64_t units[16];
    int n = actor_units(c, actor, units, 16, NULL);
    for (int i = 0; i < n; i++) {
        char nm[64];
        text_at(rt_call(c, vfn(units[i], MU_NAME), 1, units[i]), nm, sizeof nm);
        const TintSet *ts = NULL;
        for (int k = 0; k < NTINT && !ts; k++)
            if (!strcmp(nm, tint_sets[k].model)) ts = &tint_sets[k];
        if (!ts) continue;
        int want = colour == ts->native && !CL.native ? COL_NONE : colour;
        UnitCol *u = NULL;
        for (int k = 0; k < UNITS && !u; k++)
            if (ucol[k].unit == units[i] && ucol[k].ts == ts && ucol[k].vt == LD64(units[i])) u = &ucol[k];
        if (!u && want == COL_NONE) continue;   // never tinted: nothing to put back
        if (!u) u = unit_col(c, units[i], ts, who);
        u->used = CL.frames;
        if (u->colour != COL_NONE && (u->colour != want || u->param != CL.param)) unit_clear(c, u);
        if (want == COL_NONE) continue;
        if (flash) CL.skips++;
        if (flash && CL.param == CP_BLEND) {
            u->colour = want, u->param = CL.param;
            continue;
        }
        if (CL.diag && u->first && CL.frames == u->first + 120) unit_diag(u, who, "120 frames on, before that frame's write");
        unit_write(c, u, want, flash, who);
        unit_probe(c, u, side, who);   // test
        if (!u->first) {
            u->first = CL.frames;
            if (CL.diag) unit_diag(u, who, "after the first write");
        }
    }
}

// A Link (or puppet) and the armour bound to him (EU static links: Armor_Adult_Goron / _Zora in ArmorUpper).
static void colour_link(Cpu *c, uint64_t actor, int colour, const char *who, int side) {
    colour_actor(c, actor, colour, who, side);
    uint64_t eu = eu_of(actor);
    for (int s = 0; eu && s < GEAR_STAT; s++) {
        uint64_t a = link_actor(eu + EU_STAT + EU_LINK * (uint64_t)s);
        char n[16];
        if (a && !strncmp(text_at(LD64(a + A_NAME), n, sizeof n), "Armor_", 6)) {
            char w[48];
            snprintf(w, sizeof w, "%s's armour", who);
            colour_actor(c, a, colour, w, side);
        }
    }
}

static void colour_frame(Cpu *c, uint64_t player) {
    if (CL.self == COL_NONE && CL.test == COL_NONE && !CL.dirty) {
        int any = 0;
        for (int id = 1; id <= COOP_MAX_MEMBERS; id++) any |= CL.member[id] != COL_NONE;
        if (!any) return;
    }
    CL.frames++;
    if (player) colour_link(c, player, CL.self, "Link", 0);
    if (P.test == 2 && shadow_p.state == SP_FOLLOW && puppet_alive(&shadow_p)) colour_link(c, shadow_p.actor, CL.test, "shadow", 1);
    for (int id = 1; id <= COOP_MAX_MEMBERS; id++) {
        Puppet *p = &N.puppet[id];
        char who[16];
        snprintf(who, sizeof who, "member %d", id);
        if (p->state == SP_FOLLOW && puppet_alive(p)) colour_link(c, p->actor, CL.member[id], who, -1);
    }
    int dirty = 0;
    for (int k = 0; k < UNITS; k++) dirty |= ucol[k].colour != COL_NONE && CL.frames - ucol[k].used < 600;
    CL.dirty = dirty;
    if (CL.frames % 600 == 0) {
        rt_log("coop: colour: w=%.2f %u writes, %u frames of hit flash, %u of the game's warp glow in the last 600 frames "
               "(%s, strength %.3f, dark %.2f)", wsec(), CL.writes, CL.skips, CL.yields, cp_name[CL.param], CL.strength,
               CL.dark);
        CL.writes = CL.skips = CL.yields = 0;
    }
}

static void colour_cfg(void) {
    CL.self_env = col_parse(cfg_get("NX2NAVI_COOP_COLOUR_SELF"), "NX2NAVI_COOP_COLOUR_SELF");
    CL.self = CL.self_env;   // in a room, the room's assignment replaces it (room_colours)
    CL.test = col_parse(cfg_get("NX2NAVI_COOP_COLOUR_TEST"), "NX2NAVI_COOP_COLOUR_TEST");
    for (int id = 0; id <= COOP_MAX_MEMBERS; id++) CL.member[id] = COL_NONE;   // the room's assignment: not yet
    const char *pm = cfg_get("NX2NAVI_COOP_COLOUR_PARAM");
    CL.param = pm && !strcmp(pm, "blend") ? CP_BLEND : pm && !strcmp(pm, "body") ? CP_BODY
             : pm && !strcmp(pm, "none") ? CP_NONE : pm && !strcmp(pm, "petrify") ? CP_PETRIFY : CP_WARP;
    probe_parse(0, cfg_get("NX2NAVI_COOP_COLOUR_PROBE"));        // test
    probe_parse(1, cfg_get("NX2NAVI_COOP_COLOUR_PROBE_TEST"));
    const char *dg = cfg_get("NX2NAVI_COOP_COLOUR_DIAG");
    CL.diag = dg && *dg && strcmp(dg, "0") != 0;
    const char *st = cfg_get("NX2NAVI_COOP_COLOUR_STRENGTH");
    float s = st && *st ? (float)atof(st) : CL.param == CP_WARP ? 0.03f : CL.param == CP_PETRIFY ? 0.6f : 0.7f;
    CL.strength = s < 0 ? 0 : s > 1 ? 1 : s;
    const char *dk = cfg_get("NX2NAVI_COOP_COLOUR_DARK");
    float d = dk && *dk ? (float)atof(dk) : 0.8f;
    CL.dark = d < 0 ? 0 : d > 1 ? 1 : d;
    const char *nt = cfg_get("NX2NAVI_COOP_COLOUR_NATIVE");
    CL.native = nt && *nt && strcmp(nt, "0") != 0;
    col_rgb_parse(cfg_get("NX2NAVI_COOP_COLOUR_RGB"));
    if (CL.self == COL_NONE && CL.test == COL_NONE)
        rt_log("coop: colour: no test colours (NX2NAVI_COOP_COLOUR_SELF, _TEST); in a room the room's colours apply");
    else
        rt_log("coop: colour: Link %s, shadow %s, %s, strength %.3f, dark %.2f, native tunics %s; green %.2f %.2f %.2f, red %.2f "
               "%.2f %.2f, blue %.2f %.2f %.2f, purple %.2f %.2f %.2f",
               CL.self != COL_NONE ? col_name[CL.self] : "-", CL.test != COL_NONE ? col_name[CL.test] : "-",
               cp_name[CL.param], CL.strength, CL.dark, CL.native ? "tinted" : "kept", col_rgb[0][0], col_rgb[0][1],
               col_rgb[0][2], col_rgb[1][0], col_rgb[1][1], col_rgb[1][2], col_rgb[2][0], col_rgb[2][1], col_rgb[2][2],
               col_rgb[3][0], col_rgb[3][1], col_rgb[3][2]);
}

// ---- room colours: every member of a room has a colour of its own (green,
// red, blue, purple), and every game shows the same. Each member's POS carries its place in the room's join order (one
// more than the highest place it heard from the members already in the room when it joined; it waits up to 1.5 s to
// hear them all), the colour it picked (NX2NAVI_COOP_COLOUR=green|red|blue|purple; unset or auto: none) and the colour
// it shows now. Every game runs the same assignment over the members it hears, in join order (place, then member id
// for two that joined at the same moment; a member whose place is not known yet comes last): a member gets the colour
// it picked if no earlier member has it, else the colour it shows now if no earlier member has that (a colour once
// given stays), else the first free one in the order green, red, blue, purple. So of two members that want one
// colour the earlier joiner keeps it, and a member that picked none gets the first free colour in join order. The
// local Link and each member's puppet show the result (colour_frame). A member that sends no POS (puppets off) takes
// part in nothing. NX2NAVI_COOP_COLOURS=0 turns room colours off; they are on with NX2NAVI_COOP_PUPPETS, since the
// POS that carries them goes only then. Outside a room the test colour NX2NAVI_COOP_COLOUR_SELF applies again.

#define PLACE_WAIT_MS 1500
static struct {
    int on;                 // NX2NAVI_COOP_COLOURS (default: with NX2NAVI_COOP_PUPPETS)
    int want;               // NX2NAVI_COOP_COLOUR: COL_NONE (none picked) or a colour
    // the game thread's
    unsigned epoch;         // the N.epoch this place belongs to
    int place;              // this player's place in the room's join order, 0 until known
    _Atomic int colour;     // this player's colour now (what its POS says it shows)
    int shown[COOP_MAX_MEMBERS + 1];   // the assignment logged last
} RC = {.want = COL_NONE, .colour = COL_NONE, .shown = {[0 ... COOP_MAX_MEMBERS] = COL_NONE}};

static uint8_t room_colour_byte(void) {
    int c = RC.colour;
    return c >= 0 && c < COL_N ? (uint8_t)c : COOP_COLOUR_NONE;
}
static uint8_t room_want_byte(void) { return RC.on && RC.want >= 0 && RC.want < COL_N ? (uint8_t)RC.want : COOP_COLOUR_NONE; }
static uint16_t room_place(void) { return RC.on ? (uint16_t)RC.place : 0; }

enum { WHY_PICKED, WHY_KEPT, WHY_FIRST_FREE, WHY_NONE_FREE };
static const char *const why_text[] = {"picked", "kept", "first free", "none free"};

typedef struct { int id, place, want, colour; } RoomMember;

// The assignment, the same in every game for the same members: m is sorted into join order, out[id] and why[id] set.
static void room_assign(RoomMember *m, int n, int *out, int *why) {
    for (int i = 1; i < n; i++)
        for (int k = i; k > 0; k--) {
            unsigned ka = (m[k - 1].place ? (unsigned)m[k - 1].place : 0x10000u) * 16u + (unsigned)m[k - 1].id;
            unsigned kb = (m[k].place ? (unsigned)m[k].place : 0x10000u) * 16u + (unsigned)m[k].id;
            if (ka <= kb) break;
            RoomMember t = m[k];
            m[k] = m[k - 1], m[k - 1] = t;
        }
    int taken[COL_N] = {0};
    for (int i = 0; i < n; i++) {
        int c = COL_NONE, w = WHY_NONE_FREE;
        if (m[i].want >= 0 && m[i].want < COL_N && !taken[m[i].want]) c = m[i].want, w = WHY_PICKED;
        else if (m[i].colour >= 0 && m[i].colour < COL_N && !taken[m[i].colour]) c = m[i].colour, w = WHY_KEPT;
        else
            for (int k = 0; k < COL_N && c == COL_NONE; k++)
                if (!taken[k]) c = k, w = WHY_FIRST_FREE;
        if (c != COL_NONE) taken[c] = 1;
        out[m[i].id] = c, why[m[i].id] = w;
    }
}

static void room_colours(const Remote *r, const uint64_t *session, const int *in_view, char (*names)[COOP_NAME_MAX + 1],
                         uint64_t now) {
    int self = N.self;
    unsigned epoch = N.epoch;
    if (epoch != RC.epoch) {   // a new room, or none: the place is found again and the colours given again
        RC.epoch = epoch, RC.place = 0, RC.colour = COL_NONE;
        for (int id = 0; id <= COOP_MAX_MEMBERS; id++) RC.shown[id] = COL_NONE;
    }
    if (!RC.on || !N.in_room || self < 1 || self > COOP_MAX_MEMBERS) {
        if (RC.colour != COL_NONE || CL.self != CL.self_env) rt_log("coop: colour: w=%.2f out of the room's colours", wsec());
        CL.self = CL.self_env, RC.colour = COL_NONE;
        for (int id = 0; id <= COOP_MAX_MEMBERS; id++) CL.member[id] = COL_NONE;
        return;
    }
    // the other members the view lists whose POS came in their current session
    int heard[COOP_MAX_MEMBERS + 1] = {0}, all = 1, top = 0;
    for (int id = 1; id <= COOP_MAX_MEMBERS; id++) {
        if (id == self || !in_view[id]) continue;
        heard[id] = r[id].n > 0 && r[id].seen.session == session[id];
        if (!heard[id]) {
            all = 0;
            continue;
        }
        int pl = r[id].pose[r[id].head].place;
        if (pl > top) top = pl;
    }
    if (!RC.place && (all || now - N.epoch_ms > PLACE_WAIT_MS)) {
        RC.place = top < 65535 ? top + 1 : 65535;
        rt_log("coop: colour: w=%.2f this player (member %d '%s') is number %d in the room's join order (%s)", wsec(),
               self, names[self], RC.place, all ? "heard every other member" : "1.5 s without hearing every other member");
    }
    RoomMember m[COOP_MAX_MEMBERS + 1];
    int n = 0;
    m[n++] = (RoomMember){self, RC.place, RC.want, RC.colour};
    for (int id = 1; id <= COOP_MAX_MEMBERS; id++)
        if (heard[id]) {
            const CoopPos *p = &r[id].pose[r[id].head];
            m[n++] = (RoomMember){id, p->place, p->want < COL_N ? p->want : COL_NONE, p->colour < COL_N ? p->colour : COL_NONE};
        }
    int out[COOP_MAX_MEMBERS + 1], why[COOP_MAX_MEMBERS + 1];
    for (int id = 0; id <= COOP_MAX_MEMBERS; id++) out[id] = COL_NONE, why[id] = WHY_NONE_FREE;
    room_assign(m, n, out, why);
    int changed = 0;
    for (int id = 1; id <= COOP_MAX_MEMBERS; id++) changed |= out[id] != RC.shown[id];
    if (changed) {
        char line[900];
        int o = snprintf(line, sizeof line, "coop: colour: w=%.2f room colours in join order:", wsec());
        for (int i = 0; i < n && o < (int)sizeof line - 160; i++) {
            int id = m[i].id, w = m[i].want;
            o += snprintf(line + o, sizeof line - o, "%s member %d '%s'%s place %d: %s (%s%s%s)", i ? ";" : "", id,
                          names[id], id == self ? " (this player)" : "", m[i].place, out[id] >= 0 ? col_name[out[id]] : "none",
                          why_text[why[id]], w >= 0 && out[id] != w ? "; it picked " : "", w >= 0 && out[id] != w ? col_name[w] : "");
        }
        rt_log("%s", line);
        for (int id = 0; id <= COOP_MAX_MEMBERS; id++) RC.shown[id] = out[id];
    }
    RC.colour = out[self];
    CL.self = out[self];
    for (int id = 1; id <= COOP_MAX_MEMBERS; id++) CL.member[id] = id == self ? COL_NONE : out[id];
}

// ---- name tags (coop_tags.h): where the shown players' Links stand, for
// the overlay that writes each one's name above his head (ui/coop_tags_view.cpp). Read at the start of the hook, before
// this frame's poses move the puppets, so Link and the puppets are where the frame just drawn had them. Each anchor
// carries its player's name and room colour (the same CL.member the tunics show). The local Link's position goes with
// them: the overlay draws only through a camera near him, so a menu's model camera (the pause screen's item view) or a
// far event camera shows no tags. NX2NAVI_COOP_TAGS=0 publishes nothing; NX2NAVI_COOP_TAGS_TEST also tags the local
// Link and the shadow puppet.

static void tags_publish(uint64_t player) {
    CoopTagsCfg tc;
    coop_tags_cfg(&tc);
    if (!tc.on) return;
    static char names[COOP_MAX_MEMBERS + 1][COOP_NAME_MAX + 1];
    static CoopTagFrame f;
    int self = N.self;
    plat_mutex_lock(&N.lock);
    memcpy(names, N.name, sizeof names);
    plat_mutex_unlock(&N.lock);
    CoopTagAnchor *a = f.a;
    int n = 0;
    f.link_ok = player && LD32(player + A_STATE) == 4;
    if (f.link_ok) memcpy(f.link_pos, (const void *)(uintptr_t)(player + A_POS), 12);
    for (int id = 1; id <= COOP_MAX_MEMBERS && n < COOP_TAG_MAX; id++) {
        Puppet *p = &N.puppet[id];
        if (id == self || p->state != SP_FOLLOW || !puppet_alive(p) || LD32(p->actor + A_STATE) != 4) continue;
        a[n] = (CoopTagAnchor){.kind = COOP_TAG_MEMBER, .member = (uint8_t)id, .adult = strstr(p->s.text, "Adult") != NULL,
                               .colour = (int8_t)CL.member[id]};
        memcpy(a[n].pos, (const void *)(uintptr_t)(p->actor + A_POS), 12);
        snprintf(a[n].name, sizeof a[n].name, "%s", names[id]);
        n++;
    }
    if (tc.test && player && n < COOP_TAG_MAX) {
        a[n] = (CoopTagAnchor){.kind = COOP_TAG_TEST_LINK, .adult = (uint8_t)is_adult(player), .colour = (int8_t)CL.self};
        memcpy(a[n].pos, (const void *)(uintptr_t)(player + A_POS), 12);
        snprintf(a[n].name, sizeof a[n].name, "%s", self >= 1 && self <= COOP_MAX_MEMBERS && names[self][0] ? names[self] : "Link");
        n++;
        if (P.test == 2 && shadow_p.state == SP_FOLLOW && puppet_alive(&shadow_p) && LD32(shadow_p.actor + A_STATE) == 4 &&
            n < COOP_TAG_MAX) {
            a[n] = (CoopTagAnchor){.kind = COOP_TAG_TEST_SHADOW, .adult = strstr(shadow_p.s.text, "Adult") != NULL,
                                   .colour = (int8_t)CL.test};
            memcpy(a[n].pos, (const void *)(uintptr_t)(shadow_p.actor + A_POS), 12);
            snprintf(a[n].name, sizeof a[n].name, "Shadow");
            n++;
        }
    }
    f.n = n, f.game_frame = P.frames;
    coop_tags_publish(&f);
    static int last_n = -1;
    if (n != last_n) {
        char line[400];
        int o = snprintf(line, sizeof line, "coop: tags: w=%.2f game frame %u: %d anchors", wsec(), P.frames, n);
        for (int i = 0; i < n && o < (int)sizeof line - 80; i++)
            o += snprintf(line + o, sizeof line - o, "%s '%s' %s at %.2f %.2f %.2f", i ? ";" : ":", a[i].name,
                          a[i].adult ? "adult" : "child", a[i].pos[0], a[i].pos[1], a[i].pos[2]);
        rt_log("%s", line);
        last_n = n;
    }
}

// ---- the hook

static void puppet_frame(Cpu *c) {
    rt_call(c, TIME_STEP, 1, c->x[0]);   // the replaced bl: x0 and s0 are the game's
    P.frames++;
    if (P.frames == 1) rt_log("coop: puppet: the frame hook runs (thread %u)", plat_thread_id());
    uint64_t player = player_actor();
    if (player != P.player) {
        char n[64];
        rt_log("coop: puppet: w=%.2f Link's actor %#llx (%s), state %u", wsec(),
               (unsigned long long)player, player ? text_at(LD64(player + A_DEF_NAME), n, sizeof n) : "-",
               player ? LD32(player + A_STATE) : 0);
        P.player = player, P.player_frames = 0;
    }
    tags_publish(player);   // name tags: before this frame's poses move the puppets
    char area[80] = "";
    if (player) {
        P.player_frames++;
        area_now(area, sizeof area);
        if (strcmp(area, P.area)) {
            rt_log("coop: puppet: w=%.2f area '%s'", wsec(), area);
            snprintf(P.area, sizeof P.area, "%s", area);
        }
        if (P.test && P.player_frames % 30 == 0) log_as(c, "Link", player, 0, P.anim);
        if (P.test == 1) spike(c, player);
        if (P.test == 2) shadow(c, player);
    }
    coop_kill_frame(c, player, area);   // kills, shared damage, difficulty (coop_kill.c): the area and Link's life component
    coop_cutscene_frame(c, player, area);   // cutscenes together (coop_cutscene.c): the event manager, leader and follower
    // the other players (and, in a room, the room's colours: the local Link's too, even alone in it); once more after
    // leaving a room, to take its colours away
    if ((P.remote && (N.others || N.in_room)) || N.alive || RC.colour != COL_NONE) remotes(c, player, area);
    colour_frame(c, player);
}

void coop_puppet_present(void) {
    unsigned v = cfg_version();
    if (v != P.cfg_seen) {
        P.cfg_seen = v;
        const char *t = cfg_get("NX2NAVI_COOP_PUPPET_TEST");
        P.test = !t ? 0 : !strncmp(t, "spike", 5) ? 1 : !strncmp(t, "shadow", 6) ? 2 : 0;
        const char *r = cfg_get("NX2NAVI_COOP_PUPPETS");   // no row in the settings table: "" or unset is off
        P.remote = r && *r && strcmp(r, "0") != 0;
        const char *bv = cfg_get("NX2NAVI_COOP_PUPPET_BB");   // test: off unless set (and not "0")
        P.bb_copy = bv && *bv && strcmp(bv, "0") != 0;
        const char *am = cfg_get("NX2NAVI_COOP_PUPPET_ARMS");   // test: upper (default), upper3, all, follow, 0
        int arms = !am || !*am || !strcmp(am, "upper") ? ARMS_UPPER : !strcmp(am, "upper3") ? ARMS_UPPER3
                 : !strcmp(am, "all") ? ARMS_ALL : !strcmp(am, "follow") ? ARMS_FOLLOW : ARMS_OFF;
        if (P.test || P.remote)   // read again only when the settings change
            rt_log("coop: puppet: arms mode %s (NX2NAVI_COOP_PUPPET_ARMS='%s')", arms_names[arms], am ? am : "");
        P.arms = arms;
        const char *gv = cfg_get("NX2NAVI_COOP_PUPPET_GEAR");   // on unless "0"; "2" adds the hand items
        gear_mode = !gv || !*gv ? 1 : atoi(gv);
        const char *gd = cfg_get("NX2NAVI_COOP_PUPPET_GEAR_DRAWN");   // "0": weapon and shield always sheathed
        gear_drawn = !(gd && !strcmp(gd, "0"));
        const char *gm = cfg_get("NX2NAVI_COOP_PUPPET_GEAR_MASK");   // test: hex, the slots copied
        gear_mask = gm && *gm ? (uint32_t)strtoul(gm, NULL, 16) : ~0u;
        const char *gt = cfg_get("NX2NAVI_COOP_PUPPET_GEAR_TEST");   // test: gear Link does not have
        snprintf(gear_test, sizeof gear_test, "%s", gt ? gt : "");
        rt_log("coop: puppet: gear %d (drawn %d, mask %#x%s%s), blackboard copy %d", gear_mode, gear_drawn, gear_mask,
               gear_test[0] ? ", test gear " : "", gear_test, P.bb_copy);
        colour_cfg();
        // room colours, this player's pick, and the gear in POS v3
        const char *rcv = cfg_get("NX2NAVI_COOP_COLOURS");   // on with the puppets unless "0"
        RC.on = P.remote && !(rcv && !strcmp(rcv, "0"));
        const char *pick = cfg_get("NX2NAVI_COOP_COLOUR");   // green, red, blue, purple; unset or auto: the first free
        RC.want = pick && *pick && strcmp(pick, "auto") ? col_parse(pick, "NX2NAVI_COOP_COLOUR") : COL_NONE;
        const char *pg = cfg_get("NX2NAVI_COOP_POS_GEAR");   // the gear in POS: on unless "0"
        N.pos_gear = !(pg && !strcmp(pg, "0"));
        if (P.remote)
            rt_log("coop: colour: room colours %s (NX2NAVI_COOP_COLOURS='%s'); this player picks %s (NX2NAVI_COOP_COLOUR='%s'); "
                   "POS version %d, gear in it %s (NX2NAVI_COOP_POS_GEAR='%s')", RC.on ? "on" : "off", rcv ? rcv : "",
                   RC.want != COL_NONE ? col_name[RC.want] : "none (the first free colour)", pick ? pick : "",
                   COOP_POS_VERSION, N.pos_gear ? "on" : "off", pg ? pg : "");
        if (P.test == 1 && t[5] == ':') {
            char a[64] = "", b[64] = "";
            if (sscanf(t + 6, "%63[^:]:%63s", a, b) >= 1 && a[0]) snprintf(spike_names[0], sizeof spike_names[0], "%s", a);
            if (b[0]) snprintf(spike_names[1], sizeof spike_names[1], "%s", b);
        }
        if (P.test == 2) {
            char a[64] = "";
            float side = 1.0f;
            if (t[6] == ':') sscanf(t + 7, "%63[^:]:%f", a, &side);
            snprintf(P.shadow_name, sizeof P.shadow_name, "%s", a);
            P.side = side;
        }
    }
    // on while there are others in the room to show (NX2NAVI_COOP_PUPPETS) or puppets to remove, or the local Link
    // has a colour (or still carries one), or this player is in a room with room colours; off (the original call)
    // otherwise
    // ... and while kills, shared damage or difficulty need the area and Link from it (coop_kill.c)
    int kills = coop_kill_present();
    int cuts = coop_cutscene_present();   // ... and while cutscenes together have a room with others (or a test)
    int want = P.test != 0 || (P.remote && N.others) || N.alive || CL.self != COL_NONE || CL.dirty ||
               (RC.on && N.in_room) || RC.colour != COL_NONE || kills || cuts;
    if (want != P.want) {
        P.want = want;
        rt_patch_switch(P.set, want);
        rt_log("coop: puppet: the frame hook %s (present thread %u)", want ? "on" : "off", plat_thread_id());
    }
}

void coop_puppet_init(void) {
    plat_mutex_init(&N.lock);
    N.pos_gear = 1;
    if (P.set < 0) return;
    coop_on_message(COOP_K_POS, on_pos, NULL);
    coop_on_view(on_view, NULL);
    coop_kill_init();   // kills, shared damage, difficulty (coop_kill.c): its messages and hooks on the co-op thread
    coop_cutscene_init();   // cutscenes together (coop_cutscene.c): its message and hooks on the co-op thread
}

__attribute__((constructor)) static void puppet_register(void) {
    P.set = rt_patch_set("coop_puppet");
    P.cfg_seen = ~0u;
    if (P.set >= 0) rt_register_host(HOST_FRAME, puppet_frame);
}

#else
void coop_puppet_present(void) {}
void coop_puppet_init(void) {}
int coop_puppet_world_others(void) { return -1; }
int coop_puppet_area_others(const char *area) {
    (void)area;
    return 0;
}
#endif
