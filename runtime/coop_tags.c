// Co-op name tags (coop_tags.h): the settings, and the anchors the game thread hands to the present thread.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "coop_tags.h"
#include "host.h"
#include "platform/platform.h"

#define FRESH_NS 250000000ull   // anchors older than this are not shown (the frame hook stopped)

static PlatMutex lock = PLAT_MUTEX_INIT;   // around S, S_ns and C; held only to copy
static CoopTagFrame S;
static uint64_t S_ns;   // when S was published
static unsigned cfg_seen = ~0u;
static CoopTagsCfg C;

static float num_or(const char *s, float def, float lo, float hi) {
    if (!s || !*s) return def;
    float v = (float)atof(s);
    return v < lo ? lo : v > hi ? hi : v;
}

static void parse(void) {
    const char *on = cfg_value("NX2NAVI_COOP_NAMES");
    const char *test = cfg_get("NX2NAVI_COOP_TAGS_TEST");
    const char *lg = cfg_get("NX2NAVI_COOP_TAGS_LOG");
    const char *head = cfg_get("NX2NAVI_COOP_TAGS_HEAD");
    const char *size = cfg_get("NX2NAVI_COOP_TAGS_SIZE");
    const char *cam = cfg_get("NX2NAVI_COOP_TAGS_CAM");
    C.on = cfg_bool("NX2NAVI_COOP_NAMES");
    C.test = !test || !*test || !strcmp(test, "0") ? 0 : !strcmp(test, "ruler") ? 2 : 1;
    C.log = lg && *lg && strcmp(lg, "0") != 0;
    C.cam = !(cam && !strcmp(cam, "0"));
    // Calibrated with the ruler (NX2NAVI_COOP_TAGS_TEST=ruler, in Goron City): the cap's top stands 1.13 m above the actor's origin
    // (the soles) for the child and 1.66 m for the adult; the tag's pointer sits about 0.17 m and 0.2 m above it
    C.head[0] = 1.30f, C.head[1] = 1.85f;
    if (head && *head) {
        float c = 0, a = 0;
        int k = sscanf(head, "%f,%f", &c, &a);
        if (k >= 1 && c > 0.1f && c < 5.0f) C.head[0] = c;
        if (k == 2 && a > 0.1f && a < 5.0f) C.head[1] = a;
    }
    C.size = num_or(size, 1.0f, 0.3f, 4.0f);
    rt_log("coop: tags: %s (NX2NAVI_COOP_NAMES='%s'), test %s (NX2NAVI_COOP_TAGS_TEST='%s'), log %d, scene camera %s, "
           "head %.2f m child / %.2f m adult, size %.2f", C.on ? "on" : "off", on,
           C.test == 2 ? "ruler" : C.test ? "Link and the shadow" : "none", test ? test : "", C.log,
           C.cam ? "read" : "never read (NX2NAVI_COOP_TAGS_CAM=0)", C.head[0], C.head[1], C.size);
}

void coop_tags_cfg(CoopTagsCfg *out) {
    unsigned v = cfg_version();
    plat_mutex_lock(&lock);
    if (v != cfg_seen) cfg_seen = v, parse();
    *out = C;
    plat_mutex_unlock(&lock);
}

void coop_tags_publish(const CoopTagFrame *f) {
    uint64_t now = plat_now_ns();
    plat_mutex_lock(&lock);
    S = *f;
    if (S.n < 0) S.n = 0;
    if (S.n > COOP_TAG_MAX) S.n = COOP_TAG_MAX;
    S_ns = now;
    plat_mutex_unlock(&lock);
}

int coop_tags_read(CoopTagFrame *out) {
    uint64_t now = plat_now_ns();
    plat_mutex_lock(&lock);
    *out = S;
    uint64_t ns = S_ns;
    plat_mutex_unlock(&lock);
    out->age_ns = ns && now > ns ? now - ns : 0;
    if (!ns || out->age_ns > FRESH_NS) out->n = 0;
    return out->n;
}
