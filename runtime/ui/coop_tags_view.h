// Co-op name tags over the game's picture (ui/coop_tags_view.cpp; coop_tags.h has the settings and the anchors). Present
// path only (Dear ImGui), from ui/menu.cpp's overlay frame.
#pragma once
extern "C" {
#include "ui/menu.h"
}

// Every overlay frame: true when tags are to be drawn this frame (tags on, anchors fresh, a camera for the presented
// frame). Also keeps the scene camera's capture going (vk_scenecam_want) while there are anchors.
bool coop_tags_tick(const MenuTarget *t);
// Between NewFrame and Render, the menu closed: the names into the background draw list.
void coop_tags_draw(const MenuTarget *t, float scale);
