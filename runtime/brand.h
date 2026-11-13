// NX2navi Co-op's name in the game window's title (gfx_window_win.c, gfx_window_sdl.c), so that its window is told
// apart from the official NX2navi's. launcher/brand.py names the launcher, its folder and its releases. The window's
// class (NX2naviRecompWindow) and the Vulkan application name stay as they are: tools and drivers know them.
#pragma once

#define NX2NAVI_WINDOW_TITLE "NX2navi Co-op"
#define NX2NAVI_WINDOW_TITLE_W L"NX2navi Co-op"
