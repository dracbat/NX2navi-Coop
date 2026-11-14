// The scene camera of each presented frame as the replay drew it (vk_scenecam.c), for overlays that mark points of the
// world (co-op name tags, ui/coop_tags_view.cpp). C and C++: no Vulkan or CPU headers here.
#pragma once
#include <stdint.h>

struct VkExec;

typedef struct {
    float view[12];      // rows 0-2 of Ocarina's scene context: right, up, back (3x4 row-major; the camera looks down -z)
    float proj[16];      // rows 7-10: the projection (4x4 row-major, for column vectors; row 3 is 0 0 -1 0)
    uint64_t block;      // the block's guest address
    uint64_t frame;      // the present it belongs to (vk_backend.c)
    uint32_t draws;      // scene draws that used the block this frame
    uint32_t blocks;     // scene context blocks seen this frame
    int got;             // how the rows were read: 1 the submission's copy, 2 guest memory (vk_usnap_read)
    int ok;              // 1: a camera; 0: none this frame (loading, a menu's picture, nobody asked)
} VkSceneCam;

void vk_scenecam_draw(struct VkExec *ex);                     // vk_exec_draw.c, beside vk_dlaa_draw (replay, workers)
void vk_scenecam_frame(VkSceneCam *out, uint64_t frame);      // vk_backend.c present_image: this frame's camera; resets
void vk_scenecam_presenting(const VkSceneCam *c);             // vk_backend.c present_main, before vk_present_blit
int vk_scenecam_presented(VkSceneCam *out);                   // the overlay: 1 with the presented frame's camera
void vk_scenecam_want(void);                                  // the overlay: keep reading for 120 more presents
