<p align="center"><img src="docs/images/navi.png" alt="NX2navi" width="160"></p>

# NX2navi Co-op

**NX2navi Co-op** is an unofficial build of NX2navi for playing *Ocarina of Time* together with friends: each player
runs their own game and save, and the story is shared through a small relay server. It installs beside the official
NX2navi and updates itself from this repository's releases. Nothing of the game is included: you need your own dump,
as for NX2navi. Please do not report NX2navi Co-op's problems to the official NX2navi.

### Install

1. Download `NX2navi-Coop-Setup-<version>.exe` from this repository's [releases](../../releases), put it next to
   your `.nup` dump of the game (version 1.1.1) and start it. The setup is not signed, so SmartScreen warns:
   choose *More info* and *Run anyway*.
2. The launcher builds the game on your PC (10 to 25 minutes, once) and starts it. It lives in its own folder
   (`%LOCALAPPDATA%\Programs\NX2navi-Coop`) with its own shortcuts, *NX2navi Co-op*; an official NX2navi on the same
   PC is left as it is, and either can be uninstalled from Settings > Apps without touching the other.
3. New versions install through the launcher, as NX2navi's do.

The requirements are NX2navi's ([Installing](#installing) below). Where the rest of this page names
`NX2navi-Setup`, NX2navi Co-op's is `NX2navi-Coop-Setup`.

### The relay

The players' games meet on a relay: a small program that one person in the group runs and the others connect to over
the internet (UDP port 24680). It only passes the games' sealed messages on: it cannot read them, never sees the
room's secret, your game or your save, and keeps nothing on disk.

- **The host** starts it in the NX2navi Co-op folder with
  `python\python.exe -I app\<version>\tools\coop_relay.py` (or `python3 tools/coop_relay.py` from this repository on
  any machine with Python 3), lets UDP port 24680 through the PC's firewall and, for friends elsewhere, forwards that
  port on the router to the PC. It runs while the window stays open.
- **Everyone** puts the host's address into the Co-op tab's *Relay address* (or `coop_server = <address>` under
  `[coop]` in `nx2navi.ini`). Pass the address on privately, never in an issue or anywhere public.

### Playing together

1. Start the game and open the menu (F1, or Minus and Plus together on a gamepad): the **Co-op** tab. Turn *Co-op*
   on, set *Your name* and the *Relay address*.
2. One player chooses *Make a room* and sends the room's secret (`XXXXXX-XXXXXX-XXXXXX`, *Copy the secret*) to the
   others in a private message. The others choose *Join a room* and paste it.
3. Each player then accepts the room under *Accept or not*. Compare the key the tab shows with the one its owner reads
   out to you first: nothing from the room reaches your save before you accept, and your save is copied to
   `coop-backup` in the profile folder when you do.
4. Shared by everyone: story progress (events, chests, doors, switches, bosses), items, songs and quest items; small
   keys go into one pool per dungeon. Your own: rupees and ammunition, always, and where you are, your hearts and your
   magic.
5. Seeing each other is being tested: with `coop_puppets = 1` under `[coop]` in `nx2navi.ini`, the other players of
   your room are shown in your world as Links of their own. The launcher makes the files for them from your own game
   when it builds it (*Make the Co-op Players*).

[docs/coop.md](docs/coop.md) explains rooms, the rules and what joining a room means for your save in full.

# NX2navi

**Join the official NX2navi update discord server: https://discord.com/invite/uf6gSrsPpq** 

**NX2navi** is a static recompilation of *The Legend of Zelda: Ocarina of Time* for Nintendo Switch 2 (game
version 1.1.1) into a native x86-64 application for Windows, with a Vulkan renderer
([docs/architecture.md](docs/architecture.md)).

> [!IMPORTANT]
> **No game files are included.** You need your own legally obtained copy of the game, dumped from your own console.
> Instructions for obtaining those files will not be provided.
>
> The game is built from your files on your machine, so the result contains the game. It is for your own use. Do not share it.

## Status

Early development, playable. Kokiri Forest, Kakariko Village, Death Mountain Trail and its crater, the Temple of
Time and the Great Fairy fountains play at 60 fps and above on current GPUs, from the title screen and from saves.
Visual bugs and occasional crashes are still expected ([Known issues](#known-issues)).

Report problems as issues, following [CONTRIBUTING.md](CONTRIBUTING.md).

## Features

- Native code, not emulation
- 60 fps by default, or a limit of 30 to 240 fps, the display's refresh rate or unlimited
- Native widescreen and ultrawide (the window's shape, 21:9 or 32:9) with the HUD, menus and movies kept 16:9
- Game resolutions from 1080p to above the game's 1440p: 1800p, 4K, 5K or the window's own size, sharp on high-DPI
  displays
- Upscaling and anti-aliasing: NVIDIA DLSS (DLAA, Quality, Balanced, Performance, Ultra Performance; RTX GPUs) or
  AMD FSR 3.1 (Native AA and the same modes; any GPU) or AMD FSR 4 (the same modes; Radeon RX 6000, RX 7000 and
  RX 9000 GPUs), which render the 3D scene at the mode's share and keep the HUD sharp; or AMD FSR 1 or bilinear scaling to the window, with a sharpness setting; black bars or stretch
- Frame generation: NVIDIA DLSS Frame Generation (RTX 40 and 50 GPUs, paced with Reflex) at 2x, or 3x and 4x on RTX 50
  GPUs (one, two or three generated frames for every frame the game draws), or AMD FSR 3.1 (any GPU), a generated
  frame between every two the game draws, with the HUD kept out of it, for about twice the frame rate on a fast
  display; in a window of another shape than the picture, frames are generated inside the picture only, so the black
  bars stay clean. The frame limit counts the frames the game draws, and frame generation adds its
  frames on top of it. The frame limit can also be
  handed to RivaTuner Statistics Server
- HDR on HDR displays, two kinds: the game's scene tone mapped to the display's peak, matched to the game's mid grey and tone, with true blacks, or the game's own native HDR; peak, game and HUD brightness in the menu
- The game's known shaders prepared when the game is built, and the pipelines met in play built again in the
  background at each start, so play rarely stops to compile them; a shader or pipeline met for the first time can
  still hold a frame, and a draw that must wait for its pipeline has it built on worker threads, never on the game's
  drawing thread
- In-game settings menu (F1, or Minus and Plus together on a gamepad) and performance overlay (F10)
- Keyboard, mouse and gamepads with remapping, Nintendo, Xbox or PlayStation button layout, rumble and gyro; the game
  follows whichever connected gamepad you press, and DualShock 3 controllers are read directly when enabled
- Xbox, PlayStation, Steam Deck and keyboard and mouse button prompts in the game's own text, drawn on your PC when the game is
  built; the keyboard prompts follow your key bindings
- Stereo or 5.1 surround sound output, on the output device of your choice; the default microphone for the game's
  microphone input
- The monitor the game opens on, straight in fullscreen when fullscreen is on
- Your Discord status shows "Playing NX2navi" with the play time (on by default; the Game tab of the menu or the
  launcher's Settings turns it off)
- Docked or handheld console mode
- The game's textures stay compressed (BC3, about half the video memory at the same speed) unless the PC has 20 GiB of
  video memory and 48 GiB of RAM, as Windows counts video memory against the system's commit limit

## Installing

Download `NX2navi-Setup-<version>.exe` from the releases and start it next to your `.nup` dump. It is the only file
you need. The launcher it
installs finds the dump, downloads the build tools, builds the game on your PC (20 to 45 minutes, once) and starts
it; afterwards it keeps the port up to date and rebuilds only what changed ([docs/launcher.md](docs/launcher.md)).
[docs/README-BuildKit.md](docs/README-BuildKit.md) describes the same build by hand, [docs/building.md](docs/building.md)
building from source. Settings are in the in-game menu and in `nx2navi.ini` ([nx2navi.ini.example](nx2navi.ini.example)
lists every key). Controls: [docs/controls.md](docs/controls.md).

Requirements: Windows 10 or later; an x86-64 CPU with SSE4.2 (AVX2/FMA machines get a faster build); 16 GB of RAM; about
34 GB free for the game and 1.5 GB for the launcher and its tools; a GPU with Vulkan 1.3, `VK_EXT_external_memory_host`
and `VK_EXT_mesh_shader`. The setup is not signed, so SmartScreen warns on its first start.

## Known issues

- The first start after an install or update, and the first visit to an area, can pause briefly while the graphics
  driver builds what it has not seen; later starts are smooth.
- Machines with 16 GB of RAM can run out of system memory in Hyrule Field.
- Integrated AMD GPUs can fail at the title screen (their host memory heap is too small for the game's memory).
- A rare crash in the game's particle code.

## License

The source is public to read. Its additions and changes are all rights reserved ([LICENSE-ADDITIONS](LICENSE-ADDITIONS)):
no part of them may be copied or published without written permission. The base import stays under its
[MIT License](LICENSE), and third-party code keeps its own license ([THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)).
No license grants anything for Nintendo's game.

NX2navi is an independent, non-commercial fan project, not affiliated with or endorsed by Nintendo. *The Legend
of Zelda*, *Ocarina of Time* and *Nintendo Switch* are trademarks of Nintendo.

## Building from source

[docs/building.md](docs/building.md) lists the toolchain and the steps. `tools/make_game.py` also builds the game
natively on Linux ([docs/README-BuildKit.md](docs/README-BuildKit.md), "Linux"); the launcher and the setup are
Windows only. Everything the build writes stays under
`build\` and `generated\`, which git ignores, as it does the runtime's shaders the build compiles
(`runtime\*_spv.h`; the repository holds their source only); run `tools/check_content.py` before a commit.
