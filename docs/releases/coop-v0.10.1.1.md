## Install or update

- **New install:** download **`NX2navi-Coop-Setup-0.10.1.1.exe`** below, put it next to your `.nup` dump of the game
  and start it. Windows SmartScreen warns on the first start because the setup is not signed: choose *More info* and
  *Run anyway*. The launcher checks your game files, builds the game (10 to 25 minutes, once), prepares every shader,
  makes the Co-op Players and starts it.
- **Beside the official NX2navi:** NX2navi Co-op has its own folder (`%LOCALAPPDATA%\Programs\NX2navi-Coop`), its own
  game folder, shortcuts, Apps entry and Steam shortcut, and it updates from this repository's releases only. The
  official NX2navi stays as it is; uninstalling one leaves the other.
- **Later versions** install by themselves through the launcher, as NX2navi's do.

Nothing of the game is included: everything is built on your PC from your own dump.

## Playing together through Steam

- **No server and no port forwarding.** The games meet in rooms that are Steam lobbies and talk through Steam's
  network. Co-op is off until you turn it on: the in-game menu's Co-op tab (F1), *Co-op (experimental)*. One player
  then chooses *Create a room* and passes the room ID to the others, or sends Steam friends an invite with *Invite
  a friend*; the others choose *Join a room* or accept the invite.
- **Every player needs** Steam running and signed in, at least one Steam game from 2021 or later installed (co-op uses
  Steam's library file, `steam_api64.dll`, which the launcher copies beside the game from it), and this version of
  NX2navi Co-op. NX2navi Co-op signs in to Steam as its test app, so Steam shows you as playing *Spacewar*.
- **Its rooms are its own:** a game of the official NX2navi cannot join an NX2navi Co-op room, nor the other way
  round, and a game of another NX2navi Co-op version is told so and stays out.
- **Your save is copied first:** before anything of a room reaches it, to `coop-backup` in the profile folder.

## What it adds to the co-op through Steam

- **The other players as Links of their own** in your world, on by default (`coop_puppets = 0` under `[coop]` in
  `nx2navi.ini` turns them off), moving as theirs do, in their gear, with their names above them. The launcher makes
  the files for them from your own game when it builds it (*Make the Co-op Players*).
- **Colours:** every player of a room has a colour of their own, green, red, blue or purple, on their Link and their
  name, the same in every game. The Co-op tab's *Your colour* picks one; the room's host makes sure no two players
  share one.
- **Shared kills and hits:** a placed enemy one player kills dies in the world of every player in that area, each
  world dropping its own rupee or item, and everyone's hits on it count. A boss takes the others' hits but its killing
  blow stays each world's own.
- **Difficulty by the number of players:** enemy health x1.5, x2 and x2.5 and the damage Link takes x1.25, x1.5 and
  x1.75 with 2, 3 and 4 players in the game world (`coop_scale = 0` turns it off).
- **Story cutscenes together:** a story cutscene one player starts plays in the game of every player of the same age;
  a player in another area is teleported to it and stays there afterwards (`coop_cutscene_return = back` brings you
  back, `coop_cutscenes = 0` turns it off).
- **Dialog per player:** talks with NPCs stay each player's own.
- **The room's time of day:** the Sun's Song, a bed or a scene in one game sets the time in every game.
- **The era follows the players both ways:** when one player changes era, the others follow the scene that did it, or
  are teleported into an area of the room's era; a player who joins in the other era is taken into the room's era too
  (`coop_era = scene` skips the teleport).
- **Small keys are the room's:** one count per dungeon, so a key anyone picks up opens a door for everyone
  (*Shared small keys* on the Co-op tab turns it off).
- **Your own wallet:** rupees and ammunition stay each player's own, and so do hearts and magic.

`docs/coop.md` in the repository explains every part, its settings, and what joining a room means for your save.

## The base

NX2navi Co-op 0.10.1.1 is NX2navi 0.10.1 with the co-op through Steam, which NX2navi took out of 0.10.0, kept, and
NX2navi Co-op's additions to it. NX2navi's `docs/releases/v0.10.0.md` and `v0.10.1.md` list what 0.10.1 changes since
0.9.2: among them the frame rate fixes above 60 fps, less video memory, F8 bug captures, text speed, a sprint button
and low latency. The parts of a release that NX2navi Co-op does not change, such as the shader translator and the
upscalers' libraries, are those of NX2navi 0.10.1.
