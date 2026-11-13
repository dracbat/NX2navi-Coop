## Install or update

- **New install:** download **`NX2navi-Coop-Setup-0.9.2.1.exe`** below, put it next to your `.nup` dump of the game
  and start it. Windows SmartScreen warns on the first start because the setup is not signed: choose *More info* and
  *Run anyway*. The launcher checks your game files, builds the game (10 to 25 minutes, once), prepares every shader
  and starts it.
- **Beside the official NX2navi:** NX2navi Co-op has its own folder (`%LOCALAPPDATA%\Programs\NX2navi-Coop`), its own
  game folder, shortcuts, Apps entry and Steam shortcut, and it updates from this repository's releases only. The
  official NX2navi stays as it is; uninstalling one leaves the other.
- **Later versions** install by themselves through the launcher, as NX2navi's do.

Nothing of the game is included: everything is built on your PC from your own dump.

## What it adds to NX2navi 0.9.2

- **The Co-op tab** shows in the in-game menu (F1) without any setting: make a room, join one with its secret, accept
  the other players.
- **Your own wallet:** rupees and ammunition stay every player's own in every room, whatever the room's rules say.
  Story progress, items, songs and quest items are shared, and small keys go into a shared pool, as before.
- **The other players' Links (testing):** with `coop_puppets = 1` under `[coop]` in `nx2navi.ini`, the other players
  of your room are shown in your world as Links of their own, moving as they move. The build makes the files for
  them from your own game (*Make the Co-op Players* in the launcher). It is off by default while it is being
  tested; their gear, colours and names come later.
- **The relay** comes with the launcher (`app\0.9.2.1\tools\coop_relay.py`) for whoever hosts the group's games.

Everything else is NX2navi 0.9.2.
