// The scene camera of each presented frame as the replay drew it, for
// overlays that mark points of the world (co-op name tags, ui/coop_tags_view.cpp).
//
// Ocarina's opaque scene draws bind a 0x3500-byte scene context at c3 of the vertex stage: rows 0-2 are the view (3x4:
// right, up, back; the camera looks down -z) and rows 7-10 the projection (4x4), as vk_dlaa.c ocam_vp reads them, but
// vk_dlaa.c does that only while DLAA runs. Here, for each draw into the scene depth (D32F at least DLAA_SCENE_MIN_W
// wide) and a scene colour target (R11G11B10F or RGBA16F), the blocks are counted and each block's rows 0-10 read
// once per frame, as that draw reads them (vk_usnap_read: the submission's copy, else guest memory). At the back end's
// present (vk_backend.c present_image, in replay order after the frame's draws) the block most draws used is the
// frame's camera; it goes to the present thread with the present job, so the overlay projects with exactly the camera
// of the picture it is drawn over. A held frame's camera goes with it.
//
// Nothing is read unless an overlay asked within the last 120 presents (vk_scenecam_want): one atomic load per draw
// otherwise. NX2NAVI_COOP_TAGS_CAM=0 (coop_tags.h) never reads.
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "host.h"
#include "vk.h"
#include "vk_dlaa.h"
#include "vk_exec.h"
#include "vk_scenecam.h"
#include "platform/platform.h"

#define SC_BLOCK 0x3500u   // Ocarina's scene context (vk_dlaa.c OCAM_BLOCK)
#define SC_BYTES 0xb0u     // its rows 0-10
#define SC_MAX 8
#define WANT_PRESENTS 120

static PlatMutex sc_lock = PLAT_MUTEX_INIT;   // V and NV: the replay workers' draws and the present
static struct {
    uint64_t addr;
    uint32_t n, zw, zh;   // draws this frame; the depth target of the first one (the log)
    int got;
    float rows[SC_BYTES / 4];
} V[SC_MAX];
static int NV;
static _Atomic int want_left;                  // presents left that read the camera
static PlatMutex pres_lock = PLAT_MUTEX_INIT;  // presenting
static VkSceneCam presenting;
static struct { uint64_t frames, with, without, from_copy, from_mem, unread; int on; } ST;

void vk_scenecam_want(void) { atomic_store_explicit(&want_left, WANT_PRESENTS, memory_order_relaxed); }

static int scene_colour(VkFormat f) { return f == VK_FORMAT_B10G11R11_UFLOAT_PACK32 || f == VK_FORMAT_R16G16B16A16_SFLOAT; }

void vk_scenecam_draw(struct VkExec *ex) {
    if (atomic_load_explicit(&want_left, memory_order_relaxed) <= 0) return;   // nobody asked: no work
    const p_bind_buf *b = &ex->ubo[0][0];                                       // c3 of the vertex stage
    if (b->size != SC_BLOCK || !b->addr) return;
    const VkExecTarget *z = &ex->depth;
    if (!z->image || z->format != VK_FORMAT_D32_SFLOAT || z->width < DLAA_SCENE_MIN_W) return;
    if (!ex->color[0].image || !scene_colour(ex->color[0].format)) return;
    int lk = vk_exec_shared();
    if (lk) plat_mutex_lock(&sc_lock);
    int i = 0;
    while (i < NV && V[i].addr != b->addr) i++;
    if (i == NV && NV < SC_MAX) V[NV].addr = b->addr, V[NV].n = 0, V[NV].got = 0, V[NV].zw = z->width, V[NV].zh = z->height, NV++;
    if (i < NV) {
        V[i].n++;
        if (!V[i].got) V[i].got = vk_usnap_read(b->addr, SC_BYTES, V[i].rows);   // the rows as this draw reads them
    }
    if (lk) plat_mutex_unlock(&sc_lock);
}

void vk_scenecam_frame(VkSceneCam *out, uint64_t frame) {
    memset(out, 0, sizeof *out);
    out->frame = frame;
    int left = atomic_load_explicit(&want_left, memory_order_relaxed);
    plat_mutex_lock(&sc_lock);
    int best = -1, unread = 0;
    for (int i = 0; i < NV; i++) {
        if (!V[i].got) unread++;
        else if (best < 0 || V[i].n > V[best].n) best = i;
    }
    if (best >= 0) {
        memcpy(out->view, V[best].rows, 48);
        memcpy(out->proj, V[best].rows + 28, 64);   // rows 7-10
        out->block = V[best].addr, out->draws = V[best].n, out->blocks = (uint32_t)NV, out->got = V[best].got;
        out->ok = 1;
    }
    char blocks[300] = "";
    if (left > 0 && (ST.frames + 1) % 600 == 0)   // the summary below: every block of this frame
        for (int i = 0, o = 0; i < NV && o < (int)sizeof blocks - 60; i++)
            o += snprintf(blocks + o, sizeof blocks - o, "%s %#llx %u draws into %ux%u%s", i ? ";" : "",
                          (unsigned long long)V[i].addr, V[i].n, V[i].zw, V[i].zh, V[i].got ? "" : " (unread)");
    NV = 0;
    plat_mutex_unlock(&sc_lock);
    if (left > 0) {
        atomic_fetch_sub_explicit(&want_left, 1, memory_order_relaxed);
        if (!ST.on) {
            ST.on = 1;
            rt_log("coop: tags: scene camera capture on at present %llu (an overlay asked for it)", (unsigned long long)frame);
        }
        ST.frames++;
        if (!out->ok) ST.without++;
        else if (out->got == 1) ST.with++, ST.from_copy++;
        else ST.with++, ST.from_mem++;
        ST.unread += (uint64_t)unread;
        if (ST.frames % 600 == 0)
            rt_log("coop: tags: scene camera: %llu of the last 600 presents had one (rows from the submission's copy %llu, "
                   "from memory %llu), %llu none, %llu blocks unreadable; present %llu: block %#llx, %u draws, %u blocks:%s",
                   (unsigned long long)ST.with, (unsigned long long)ST.from_copy, (unsigned long long)ST.from_mem,
                   (unsigned long long)ST.without, (unsigned long long)ST.unread, (unsigned long long)frame,
                   (unsigned long long)out->block, out->draws, out->blocks, blocks),
                ST.with = ST.without = ST.from_copy = ST.from_mem = ST.unread = 0;
    } else if (ST.on) {
        ST.on = 0;
        rt_log("coop: tags: scene camera capture off (no overlay asked for %d presents)", WANT_PRESENTS);
    }
}

void vk_scenecam_presenting(const VkSceneCam *c) {
    plat_mutex_lock(&pres_lock);
    presenting = *c;
    plat_mutex_unlock(&pres_lock);
}

int vk_scenecam_presented(VkSceneCam *out) {
    plat_mutex_lock(&pres_lock);
    *out = presenting;
    plat_mutex_unlock(&pres_lock);
    return out->ok;
}
