// Co-op name tags: each other player's name above their Link, in their player
// colour, drawn over the game's picture by the menu overlay (ui/coop_tags_view.cpp, called from ui/menu.cpp's overlay
// frame). The game thread publishes where the shown players' Links stand (coop_puppet.c, the frame hook, before the
// frame's poses move the puppets); the present thread projects them with the camera that drew the presented frame
// (vk_scenecam.h) and draws the names with Dear ImGui.
//
// Settings (environment or nx2navi.ini, no menu row; read again when cfg_version changes):
//   NX2NAVI_COOP_TAGS       "0" off; on otherwise (default on)
//   NX2NAVI_COOP_TAGS_TEST  test: "1" also tags the local Link (its co-op name, else "Link") and the shadow puppet
//                           (NX2NAVI_COOP_PUPPET_TEST=shadow, "Shadow"); "ruler" adds marks every 0.25 m from 0 to
//                           2.5 m above their origins, for calibrating the head height
//   NX2NAVI_COOP_TAGS_LOG   "1": a line a second (camera, anchors, where each tag went or why it is hidden)
//   NX2NAVI_COOP_TAGS_HEAD  "<child>,<adult>": metres above the actor's origin where the tag's pointer sits
//   NX2NAVI_COOP_TAGS_SIZE  the name's size against the menu font (default 1.0)
//   NX2NAVI_COOP_TAGS_CAM   "0": the scene camera is never read (vk_scenecam.c), so no tags either
#pragma once
#include <stdint.h>

#include "coop.h"   // COOP_NAME_CAP

enum { COOP_TAG_MAX = 10 };
enum { COOP_TAG_MEMBER, COOP_TAG_TEST_LINK, COOP_TAG_TEST_SHADOW };
typedef struct {
    uint8_t kind;                    // COOP_TAG_*
    uint8_t member;                  // COOP_TAG_MEMBER: the member id
    uint8_t adult;                   // the Link is an adult (the head height)
    int8_t colour;                   // 0 green, 1 red, 2 blue, 3 purple (coop_puppet.c COL_*); -1 none
    float pos[3];                    // the actor's origin (+0x31c), metres
    char name[COOP_NAME_CAP];        // cleaned UTF-8, as the room has it (coop.h); "" when unknown
} CoopTagAnchor;

// What the game thread hands over once a frame.
typedef struct {
    int n;                           // anchors (0: nothing to show)
    uint32_t game_frame;             // the frame hook's count
    int link_ok;                     // the local Link is in the world: link_pos holds his origin
    float link_pos[3];               // the overlay shows tags only through a camera near him (not a menu's)
    uint64_t age_ns;                 // coop_tags_read: how old this is
    CoopTagAnchor a[COOP_TAG_MAX];
} CoopTagFrame;

typedef struct {
    int on;          // NX2NAVI_COOP_TAGS
    int test;        // 0, 1 (Link and the shadow too), 2 (ruler)
    int log;         // NX2NAVI_COOP_TAGS_LOG
    int cam;         // NX2NAVI_COOP_TAGS_CAM
    float head[2];   // child, adult
    float size;      // NX2NAVI_COOP_TAGS_SIZE
} CoopTagsCfg;

// The settings, parsed again when cfg_version changes (any thread; each parse is logged).
void coop_tags_cfg(CoopTagsCfg *out);
// Game thread, once a frame (f->age_ns is ignored).
void coop_tags_publish(const CoopTagFrame *f);
// Any thread: the newest frame and its anchor count; 0 when none was published or it is older than 250 ms (the frame
// hook stopped: an area change, a room left), and then out->n is 0 too.
int coop_tags_read(CoopTagFrame *out);
