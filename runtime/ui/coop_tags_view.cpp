// Co-op name tags: each other player's name above their Link, in their player
// colour, over the game's picture. The anchors come from the game thread (coop_tags.h, published by coop_puppet.c's
// frame hook), the camera is the one that drew the presented frame (vk_scenecam.h). A world point goes to clip space
// as P [V; 0 0 0 1] (x, y, z, 1), to NDC (+y the top of the picture: the replay's viewports have negative heights),
// then into the picture's rectangle in the window (gfx_fit_rect, as gfx_present.c places the picture). The tags go
// into the background draw list while the menu is closed, so the performance panel and the toasts stay above them.
//
// The camera must be the game's view of the world: its aspect the picture's, at least MIN_DRAWS scene draws, and its
// eye within MAX_EYE_M of the local Link but not at his feet. The pause screen draws only its item model (10 draws,
// eye about 115 m from Link, fovy 20), and that must show no tags; the first frames after an area change
// bring a 4:3 camera and one whose eye is Link's origin. Hidden per tag: behind the camera, nearer than
// 0.5 m or farther than 60 m (faded from 45 m), wholly outside the picture; no wall test yet (version 1).
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "imgui.h"

extern "C" {
#include "coop_tags.h"
#include "platform/platform.h"
#include "vk_scenecam.h"
// gfx_present.c (declared in vk.h, which is C only): where a cw x ch picture goes in a dw x dh window, {x, y, w, h}
void gfx_fit_rect(int cw, int ch, int dw, int dh, int out[4]);
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
}
#include "ui/coop_tags_view.h"

#define MIN_DRAWS 20       // scene draws of the camera's block in the frame (the game's view: hundreds)
#define MAX_EYE_M 40.0f    // the camera's eye from the local Link

// The players' colours as text (sRGB): green, red, blue, purple (coop_colours.h); none: a warm white
static const ImU32 kColour[4] = {IM_COL32(91, 227, 107, 255), IM_COL32(255, 92, 92, 255), IM_COL32(96, 165, 255, 255),
                                 IM_COL32(200, 128, 255, 255)};
static const ImU32 kNone = IM_COL32(245, 240, 225, 255);
static const char *const kColourName[4] = {"green", "red", "blue", "purple"};

enum { WHY_OK, WHY_ASPECT, WHY_DRAWS, WHY_NO_LINK, WHY_FAR, WHY_AT_FEET };
static const char *const kWhy[] = {"", "its aspect is not the picture's", "too few scene draws", "Link is not in the world",
                                   "its eye is far from Link", "its eye is at Link's feet: not placed yet"};

static struct {
    CoopTagsCfg cfg;
    CoopTagFrame f;
    VkSceneCam cam;
    float eye[3];
    uint64_t log_ns;
    uint64_t ticks, nocam, refused, drawn_frames, cost_ns, cost_n;
    int cam_logged, why_logged;
} T;

static int log_due(uint64_t now) {
    if (!(T.cfg.log || T.cfg.test) || now - T.log_ns < 1000000000ull) return 0;
    T.log_ns = now;
    return 1;
}

bool coop_tags_tick(const MenuTarget *t) {
    coop_tags_cfg(&T.cfg);
    T.f.n = 0;
    if (!T.cfg.on || !T.cfg.cam) return false;
    if (!coop_tags_read(&T.f)) return false;
    vk_scenecam_want();
    T.ticks++;
    uint64_t now = plat_now_ns();
    if (!vk_scenecam_presented(&T.cam)) {   // no scene drawn in this picture (loading, a menu's picture): nothing
        T.nocam++;
        if (log_due(now))
            rt_log("coop: tags: present %llu: %d anchors (game frame %u, %.0f ms old) but no scene camera in this picture "
                   "(%llu of %llu overlay frames so far)", (unsigned long long)T.cam.frame, T.f.n, T.f.game_frame,
                   (double)T.f.age_ns / 1e6, (unsigned long long)T.nocam, (unsigned long long)T.ticks);
        return false;
    }
    const float *P = T.cam.proj, *v = T.cam.view;
    for (int i = 0; i < 3; i++) T.eye[i] = -(v[i] * v[3] + v[4 + i] * v[7] + v[8 + i] * v[11]);   // -(R^T t)
    float aspect = P[0] != 0.0f ? P[5] / P[0] : 0.0f, pic = t->image_h > 0 ? (float)t->image_w / (float)t->image_h : 0.0f;
    float dx = T.eye[0] - T.f.link_pos[0], dy = T.eye[1] - T.f.link_pos[1], dz = T.eye[2] - T.f.link_pos[2];
    float far = T.f.link_ok ? sqrtf(dx * dx + dy * dy + dz * dz) : 0.0f;
    int why = !(pic > 0.0f) || !(aspect > 0.0f) || fabsf(aspect / pic - 1.0f) > 0.03f ? WHY_ASPECT
            : T.cam.draws < MIN_DRAWS                                                 ? WHY_DRAWS
            : !T.f.link_ok                                                            ? WHY_NO_LINK
            : far > MAX_EYE_M                                                         ? WHY_FAR
            : far < 0.3f                                                              ? WHY_AT_FEET
                                                                                      : WHY_OK;
    if (why != WHY_OK) {
        T.refused++;
        if (log_due(now) || why != T.why_logged)
            rt_log("coop: tags: present %llu: no tags, the camera is not the game's view (%s): block %#llx, %u draws, eye "
                   "%.2f %.2f %.2f, %.1f m from Link, fovy %.1f, aspect %.4f, picture %.4f", (unsigned long long)T.cam.frame,
                   kWhy[why], (unsigned long long)T.cam.block, T.cam.draws, T.eye[0], T.eye[1], T.eye[2], far,
                   P[5] > 0.0f ? 2.0f * atanf(1.0f / P[5]) * 57.29578f : 0.0f, aspect, pic);
        T.why_logged = why;
        return false;
    }
    if (T.why_logged) {
        rt_log("coop: tags: present %llu: the camera is the game's view again (block %#llx, %u draws, %.1f m from Link)",
               (unsigned long long)T.cam.frame, (unsigned long long)T.cam.block, T.cam.draws, far);
        T.why_logged = WHY_OK;
    }
    if (!T.cam_logged) {
        T.cam_logged = 1;
        rt_log("coop: tags: the first scene camera reached the overlay at present %llu (block %#llx, %u draws)",
               (unsigned long long)T.cam.frame, (unsigned long long)T.cam.block, T.cam.draws);
    }
    return true;
}

// clip = P [V; 0 0 0 1] (p, 1); 0 behind the camera (w <= 0.1, the game's near plane).
static int project(const VkSceneCam *k, const float p[3], float *nx, float *ny, float *w) {
    float v[3];
    for (int r = 0; r < 3; r++)
        v[r] = k->view[r * 4] * p[0] + k->view[r * 4 + 1] * p[1] + k->view[r * 4 + 2] * p[2] + k->view[r * 4 + 3];
    const float *P = k->proj;
    float cx = P[0] * v[0] + P[1] * v[1] + P[2] * v[2] + P[3];
    float cy = P[4] * v[0] + P[5] * v[1] + P[6] * v[2] + P[7];
    float cw = P[12] * v[0] + P[13] * v[1] + P[14] * v[2] + P[15];
    if (!(cw > 0.1f)) return 0;
    *nx = cx / cw, *ny = cy / cw, *w = cw;
    return 1;
}

static ImU32 with_alpha(ImU32 c, float alpha) {
    int a = (int)(alpha * 255.0f + 0.5f);
    a = a < 0 ? 0 : a > 255 ? 255 : a;
    return (c & ~IM_COL32_A_MASK) | ((ImU32)a << IM_COL32_A_SHIFT);
}

static const char *tag_name(const CoopTagAnchor *a, char *buf, size_t cap) {
    if (a->name[0]) return a->name;
    if (a->kind == COOP_TAG_TEST_LINK) return "Link";
    if (a->kind == COOP_TAG_TEST_SHADOW) return "Shadow";
    snprintf(buf, cap, "Player %d", a->seat);
    return buf;
}

// Text with an 8-way dark outline.
static void outlined(ImDrawList *dl, ImFont *font, float size, ImVec2 at, ImU32 col, ImU32 ink, float o, const char *s) {
    static const float dx[8] = {-1, 0, 1, -1, 1, -1, 0, 1}, dy[8] = {-1, -1, -1, 0, 0, 1, 1, 1};
    for (int q = 0; q < 8; q++) dl->AddText(font, size, ImVec2(at.x + o * dx[q], at.y + o * dy[q]), ink, s);
    dl->AddText(font, size, at, col, s);
}

static void to_window(const int to[4], float nx, float ny, float *x, float *y) {
    *x = (float)to[0] + (nx * 0.5f + 0.5f) * (float)to[2];
    *y = (float)to[1] + (0.5f - ny * 0.5f) * (float)to[3];
}

// NX2NAVI_COOP_TAGS_TEST=ruler: a dot and its height every 0.25 m from 0 to 2.5 m above an anchor's origin.
static void ruler(ImDrawList *dl, ImFont *font, float size, const int to[4], const CoopTagAnchor *a, ImU32 col, int log) {
    char line[600];
    int o = 0;
    ImVec2 prev;
    int have_prev = 0;
    for (int k = 0; k <= 10; k++) {
        float h = 0.25f * (float)k, p[3] = {a->pos[0], a->pos[1] + h, a->pos[2]}, nx, ny, d, x, y;
        if (!project(&T.cam, p, &nx, &ny, &d)) {
            have_prev = 0;
            continue;
        }
        to_window(to, nx, ny, &x, &y);
        if (have_prev) dl->AddLine(prev, ImVec2(x, y), IM_COL32(255, 255, 255, 160), 1.0f);
        prev = ImVec2(x, y), have_prev = 1;
        float r = k % 2 ? 2.0f : 3.0f;
        dl->AddCircleFilled(ImVec2(x, y), r + 1.0f, IM_COL32(12, 8, 4, 220));
        dl->AddCircleFilled(ImVec2(x, y), r, col);
        if (k % 2 == 0) {
            char t[16];
            snprintf(t, sizeof t, "%.1f", h);
            outlined(dl, font, size * 0.7f, ImVec2(x + 6.0f, y - size * 0.35f), IM_COL32(255, 255, 255, 255),
                     IM_COL32(12, 8, 4, 220), 1.0f, t);
        }
        if (log && o < (int)sizeof line - 40) o += snprintf(line + o, sizeof line - o, " %.2f:%.0f,%.0f", h, x, y);
    }
    if (log && o) rt_log("coop: tags:   ruler of '%s' (height m: window px):%s", a->name[0] ? a->name : "?", line);
}

void coop_tags_draw(const MenuTarget *t, float scale) {
    uint64_t t0 = plat_now_ns();
    int log = log_due(t0);
    int to[4];
    gfx_fit_rect(t->image_w, t->image_h, (int)t->width, (int)t->height, to);
    const VkSceneCam *k = &T.cam;
    if (log) {
        const float *P = k->proj;
        rt_log("coop: tags: present %llu (anchors of game frame %u, %.0f ms old): camera block %#llx (%u draws, %u blocks, "
               "rows from %s), eye %.2f %.2f %.2f, fovy %.1f, aspect %.4f (picture %dx%d), picture in the window at "
               "%d,%d %dx%d", (unsigned long long)k->frame, T.f.game_frame, (double)T.f.age_ns / 1e6,
               (unsigned long long)k->block, k->draws, k->blocks, k->got == 1 ? "the submission's copy" : "memory", T.eye[0],
               T.eye[1], T.eye[2], P[5] > 0.0f ? 2.0f * atanf(1.0f / P[5]) * 57.29578f : 0.0f, P[0] != 0.0f ? P[5] / P[0] : 0.0f,
               t->image_w, t->image_h, to[0], to[1], to[2], to[3]);
    }
    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    ImFont *font = ImGui::GetFont();
    float base = ImGui::GetFontSize() * 0.9f * T.cfg.size;
    int order[COOP_TAG_MAX], m = 0;
    float depth[COOP_TAG_MAX], nx[COOP_TAG_MAX], ny[COOP_TAG_MAX];
    for (int i = 0; i < T.f.n; i++) {
        const CoopTagAnchor *a = &T.f.a[i];
        float head = T.cfg.head[a->adult ? 1 : 0];
        float p[3] = {a->pos[0], a->pos[1] + head, a->pos[2]};
        int seen = project(k, p, &nx[i], &ny[i], &depth[i]);
        if (seen && depth[i] >= 0.5f && depth[i] <= 60.0f) order[m++] = i;
        else if (log)
            rt_log("coop: tags:   '%s' at %.2f %.2f %.2f: hidden (%s)", a->name[0] ? a->name : "?", p[0], p[1], p[2],
                   !seen ? "behind the camera" : depth[i] < 0.5f ? "nearer than 0.5 m" : "beyond 60 m");
    }
    for (int x = 1; x < m; x++)   // farthest first: nearer names are drawn over farther ones
        for (int y = x; y > 0 && depth[order[y]] > depth[order[y - 1]]; y--) {
            int s = order[y];
            order[y] = order[y - 1], order[y - 1] = s;
        }
    for (int q = 0; q < m; q++) {
        int i = order[q];
        const CoopTagAnchor *a = &T.f.a[i];
        char buf[32];
        const char *name = tag_name(a, buf, sizeof buf);
        float d = depth[i], alpha = d < 45.0f ? 1.0f : 1.0f - (d - 45.0f) / 15.0f;
        float size = base * fminf(1.0f, fmaxf(0.65f, 10.0f / d));
        float x, y;
        to_window(to, nx[i], ny[i], &x, &y);
        ImVec2 sz = font->CalcTextSizeA(size, FLT_MAX, 0.0f, name);
        float tri = size * 0.28f;
        ImVec2 at(x - sz.x * 0.5f, y - sz.y - tri * 1.5f);
        if (at.x + sz.x < (float)to[0] || at.x > (float)(to[0] + to[2]) || at.y + sz.y < (float)to[1] ||
            at.y > (float)(to[1] + to[3])) {
            if (log) rt_log("coop: tags:   '%s': hidden (off the picture at %.0f %.0f)", name, x, y);
            continue;
        }
        at.x = fminf(fmaxf(at.x, (float)to[0]), (float)(to[0] + to[2]) - sz.x);   // keep it inside the picture
        ImU32 col = with_alpha(a->colour >= 0 && a->colour < 4 ? kColour[a->colour] : kNone, alpha);
        ImU32 ink = IM_COL32(12, 8, 4, (int)(220.0f * alpha));
        float o = fmaxf(1.0f, 1.5f * scale * size / base);
        if (T.cfg.test == 2) ruler(dl, font, base, to, a, col, log);
        outlined(dl, font, size, at, col, ink, o, name);
        ImVec2 t0v(x - tri, y - tri * 1.3f), t1v(x + tri, y - tri * 1.3f), t2v(x, y);
        dl->AddTriangleFilled(ImVec2(t0v.x - o, t0v.y - o * 0.6f), ImVec2(t1v.x + o, t1v.y - o * 0.6f), ImVec2(t2v.x, t2v.y + o), ink);
        dl->AddTriangleFilled(t0v, t1v, t2v, col);
        if (log) {
            char who[24];
            if (a->kind == COOP_TAG_SEAT) snprintf(who, sizeof who, "seat %d", a->seat);
            else snprintf(who, sizeof who, "%s", a->kind == COOP_TAG_TEST_LINK ? "Link" : "shadow");
            rt_log("coop: tags:   '%s' (%s, %s, %s) at %.2f %.2f %.2f: depth %.2f m, ndc %+.3f %+.3f, px %.0f %.0f, size "
                   "%.1f, alpha %.2f", name, who, a->adult ? "adult" : "child",
                   a->colour >= 0 && a->colour < 4 ? kColourName[a->colour] : "no colour", a->pos[0],
                   a->pos[1] + T.cfg.head[a->adult ? 1 : 0], a->pos[2], d, nx[i], ny[i], x, y, size, alpha);
        }
    }
    uint64_t t1 = plat_now_ns();
    T.drawn_frames++;
    T.cost_ns += t1 - t0, T.cost_n++;
    if (T.cost_n == 600) {
        rt_log("coop: tags: %.4f ms per overlay frame for the tags (mean of 600), %llu frames drawn, %llu without a camera, "
               "%llu with a camera that is not the game's view", (double)T.cost_ns / 600.0 / 1e6,
               (unsigned long long)T.drawn_frames, (unsigned long long)T.nocam, (unsigned long long)T.refused);
        T.cost_ns = 0, T.cost_n = 0;
    }
}
