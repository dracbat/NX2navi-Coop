# Co-op

Up to four players, each in their own game with their own save, meet in a room through Steam. In a room you see the
other players as Links of their own in your world, in their gear and their colours with their names above them, you
can chat, and you play the story together: what anyone achieves counts for everyone, and an enemy one player kills
dies in every world.

**Co-op is experimental and off by default.** Turn it on in the menu's Co-op tab (*Co-op (experimental)*), or with
`coop_steam = 1` under `[coop]` in `nx2navi.ini`. With it off the game uses no network at all.

Not in this version: pots, grass, enemies the game spawns, the drops of a kill and the time of day are each game's
own.

## What you need

- **Steam, running and signed in.** NX2navi is no Steam game of its own: it signs in to Steam as *Spacewar* (app 480),
  the test app Valve gives every account. While co-op is on, Steam shows you as playing Spacewar. Nobody forwards a
  port on a router and nobody runs a server: the room is a Steam lobby and the games talk through Steam's own network,
  which finds a direct path between them or relays for them.
- **At least one Steam game from 2021 or later installed** (Windows). Co-op uses Steam's library file,
  `steam_api64.dll`, which NX2navi may not ship; every Steam game has one in its folder. The launcher fetches it for
  you: each time it starts the game and the file is not beside `ocarina.exe`, it looks through your Steam libraries
  for the newest one that Valve signed and that has the calls co-op uses, and copies it there (a second or two, once;
  `launcher/steamlib.py`). You can also copy one there yourself. The Co-op tab says when the file is missing or too
  old; the game loads it from that one place only.
- **The same NX2navi version and game version (1.1.1)** as the others. A game of another version is told so and does
  not get into the room.
- **The Co-op puppets mod**, which the launcher makes from your own game when it builds it (*Make the Co-op Players*):
  the other players' Links are made from it. Without it, or with it switched off in the launcher's Mods screen, you
  play together as well but see neither the other players' Links nor their names; they still see yours.

## Rooms

- **Create a room** makes a room and shows its **room ID**, eight letters and digits as `XXXX-XXXX`. A press on the row
  copies it. Your game is the room's host.
- **Join a room** takes a room ID (typed in any case, with or without the dash; Paste takes the clipboard's text).
- **Invite a friend** lists your Steam friends, those in co-op first, and sends a Steam invite to the one you choose.
  The invite reaches a friend whose NX2navi is running with co-op on: it shows at the top of their screen and on their
  Co-op tab, where *Join their room* accepts it. A friend whose game is not running gets Steam's own invite, which
  would start Spacewar, not NX2navi: tell them the room ID instead. When the game was started from Steam (the
  launcher's *Add to Steam*), Steam's own invite window is offered as well.
- **Players** lists who is in the room, each player's colour and the area each one's Link is in. The host can press a
  player to remove them; a removed player cannot come back into that room.
- **Your colour** (`coop_colour`): Auto or one of the colours no other player of the room has (below, "Colours").
- **Leave the room** asks first. When the host leaves, Steam hands the room to another player, whose game is the host
  from then on.

A room ID is the number of the Steam lobby. Anyone who has it can join while the room is open, so pass it on only to
the people you want in your game; the room is also visible to your Steam friends, who may join it from Steam. To be
rid of someone, the host removes them; to be sure, leave and create a new room.

## What joining a room means for your save

- **Your save is copied first.** When your game starts to share in a room (you are in the room and playing a save),
  it copies your whole save (every slot) to `coop-backup/<date-time>` in the profile folder, and takes nothing from
  the room until the copy is made: no progress and no kill. The newest twelve copies are kept. "Going back" below says
  how to restore one.
- **Progress is shared while *Share progress* is on** (it is, by default): story, world and dungeon flags (events,
  talks, chests, doors, switches, bosses, maps and compasses), items and equipment, songs, quest items, wallet, bag and
  quiver sizes. A flag anyone has set is set for everyone, and of a count the highest wins. A player who joins later
  gets what the room has, and the room what the joiner brings. Your game saves it as its own progress.
- **Each player keeps their own** small keys, rupees, ammunition, heart pieces and containers, gold skulltula tokens,
  bottle contents, the trade sequence, the equipment worn, life and magic.
- **Nothing is undone.** Leaving a room keeps what was shared. A player in the room plays with you: what they do in
  their world happens in yours, and a game that wants to can send progress it did not play for. Join rooms of people
  you trust with your save, as you would hand them your controller, and go back to the copy if you regret it.
- With *Share progress* off, your game sends and takes no progress. The rest of "Playing together" below still goes:
  each part has a setting of its own.

Every value from the room is checked against your game's own table of what is shared before it is used: the flag must
be one of the shared ones, an on/off flag can only be set to "done", an item count only up to 999. Anything else is
dropped and counted in the log.

### Going back to your save from before a room

1. Close the game.
2. In the profile folder, open `coop-backup` and pick the copy to go back to (the time is UTC).
3. Delete the folder `save-tx` (it may hold a save that was being written; it would otherwise be finished over the
   copy at the next start).
4. Move the folder `save/<numbers>` aside (keep it until you are sure), and copy the contents of the backup folder
   into a new `save/<numbers>` of the same name.
5. Start the game: every slot is as it was before the room's progress reached it.

## Playing together

What co-op shares beyond progress. Each part has a setting under `[coop]` in `nx2navi.ini` (the table at the end of
this section); only *Your colour* and *Show player names* have rows on the Co-op tab.

- **The other players in your world** (`runtime/coop_puppet.c`). Each other player whose Link is in your area is a Link
  of its own in your world, made from the Co-op puppets mod: it stands and moves where theirs does, about a tenth of a
  second behind, plays what their Link plays (the arms too), and wears their sword, shield, sheath, quiver, tunic and
  ocarina, the sword and shield drawn and sheathed as theirs. What else they hold in their hands (a bow, a slingshot, a
  bottle) is not shown. It goes when they leave the area or the room, go back to the title screen, or are not heard for
  3 s. `coop_puppets = 0` shows no other players in your game; yours is still shown in theirs.
- **Colours** (`runtime/coop_colours.c`). Every player of a room has a colour of their own, green, red, blue or purple,
  the same in every game: on the tunic and cap of that player's Link, their own Link included, on their name and in
  the Co-op tab's list of players. The room's host gives the colours, in the order the players joined: a colour stays
  with its player, a player who picks a colour no other player has gets it (that is how to change colour), and a
  player who has none gets their pick when it is free, else the first free one of green, red, blue and purple. So the
  host is green and the next player red unless they pick otherwise. Of two players who pick one colour at the same
  moment, the one who joined first gets it. A new host carries on with the colours the room had.
- **Names** (`runtime/coop_tags.c`, `runtime/ui/coop_tags_view.cpp`). Each other player's Steam name stands above their
  Link in their colour, smaller with distance, faded from 45 m and gone beyond 60 m, and never on the pause screen or
  while an area loads. The names are drawn with the camera of the picture shown, so they need no upscaler; they show
  through walls. *Show player names* (`coop_names = 0`) turns them off.
- **Kills and hits** (`runtime/coop_kill.c`). A placed enemy that one player kills dies in the world of every player
  in that area, each world with its own death and its own drop for its player, and the hits each player deals to a
  placed enemy count in every world of that area, so the players fight it together. Enemies the game spawns are left
  out (they are not the same enemy in two worlds), and so are mini-bosses, Gold Skulltulas, Deku scrubs, Poes and a
  few others. A boss takes the other players' hits but keeps at least 1 life from them, so its killing blow and its
  death stay each world's own (`coop_damage_boss = lethal` shares those too). An enemy far from your Link stops
  updating in your world, so another player's kill or hit waits there until you come near (60 s at most; a change of
  area drops it).
- **Difficulty by the number of players.** Enemies and bosses take less damage, and Link more, with every player in
  the game world: enemy health x1, x1.5, x2 and x2.5 and damage to Link x1, x1.25, x1.5 and x1.75 for 1, 2, 3 and 4
  players. It counts this player and the other players of the room whose Link is in the world, whether their Links
  are shown here or not: a player on the title screen counts for none, one who leaves counts for 10 s more, and one
  who stays in the pause menu longer than about 13 s stops counting until they play on. Instant kills and the game's
  own changes of life are never scaled. `coop_scale = 0` turns it off.

| Key | Default | |
|---|---|---|
| `coop_puppets` | `1` | `0`: no other players' Links (nor names) in your game; yours is still sent |
| `coop_puppet_gear` | `1` | `0`: the other players' Links show without their gear |
| `coop_colour` | `auto` | the Co-op tab's *Your colour*: `green`, `red`, `blue` or `purple`; `auto` the first free one |
| `coop_colours` | `1` | `0`: no room colours |
| `coop_kills`, `coop_damage` | `1` | `0`: no shared kills, no shared hits |
| `coop_damage_boss` | `safe` | `lethal`: a boss's killing blow and death are shared too; `0`: bosses share nothing |
| `coop_scale` | `1` | `0`: no difficulty by the number of players |
| `coop_scale_hp`, `coop_scale_taken` | `1,1.5,2,2.5`, `1,1.25,1.5,1.75` | the factors for 1 to 4 players |
| `coop_share_deny` | empty | more actor-name prefixes that are not shared, comma separated |

## Who can see what

| Who | Sees |
|---|---|
| The players in the room | your Steam name and colour, your Link (the area it is in, where it stands, what it plays and its gear), your chat lines, the progress your game shares and the enemies you hit and kill |
| Your Steam friends | that you play Spacewar, and the room (they may join it) |
| Valve | what Steam sees of any game that uses its lobbies and network: who is in which lobby, and the traffic's size and timing. Steam encrypts the traffic between the games |
| Your log (`logs/nx2navi.log`) and "Copy system info" | that a room was made or joined, the other players' Steam names, and the lines below; never a room ID or a Steam id. Steam's library prints lines of its own, one with your Steam id, to the console the game was started from; they are not in the log file, but they are in a file you redirect the game's output to yourself |

## Settings

`nx2navi.ini`, section `[coop]`; the menu's Co-op tab has a row for each. The environment variable is the key with
`NX2NAVI_`. "Playing together" above lists the other co-op settings.

| Key | Default | |
|---|---|---|
| `coop_steam` | `0` | on or off, at once. On: the game signs in to Steam; off: no network. (`coop`, the switch of the co-op of versions up to 0.9.2, does nothing: a line of it left in a file turns nothing on) |
| `coop_share` | `1` | in a room, share progress, items and songs (above) |
| `coop_names` | `1` | show each other player's name above their Link, in their colour |
| `coop_colour` | `auto` | the colour you pick (above, "Colours") |
| `coop_chat` | `1` | show new chat lines, and who joined or left, over the game for a few seconds while the menu is closed |

A settings file cannot put a game into a room: a room is made or joined only from the menu.

## In the log

`coop: on, signed in to Steam as app 480`, `coop: Steam is not available: <why>`, `coop: room made`, `coop: in the
lobby; asking the host`, `coop: joined the room`, `<name> joined the room`, `<name> left the room`, `coop: the host
left; this game is the host now`, `coop: left the room`, `coop: the save as it was before this room is in <folder>`,
`coop: a save is played: N flags and counts can be shared`, `coop: took N values of progress from the room`, `coop: N
values from the room were not ones this game shares; dropped`.

The co-op modules say what they do with their settings at the start and when a setting changes, and then what
happens: `coop: ext:` (the room's players by seat: `seat 2 'name' is in the room`), `coop: colour:` (`the room's
colours: ...`), `coop: puppet:` (`seat 1 'name' is in <area>: asking for CoopPuppet_Child`, the Link `made after N
frames`, its gear), `coop: tags:`, `coop: kill:`, `coop: damage:` and `coop: scale:` (`2 players (...): enemy health
x1.50, damage to Link x1.25`). A player of the room is named by seat and Steam name, never by id. Lines about what
another player sent are a few a second for each player at most, and a line says how many were left out.

## How it works

For a reviewer. The code is `runtime/steam.c` (the Steam library), `runtime/coop.c` (the room), `runtime/coop_game.c`
(the game side), `runtime/ui/coop_page.cpp` (the menu's tab, and the chat and invites over the game) and
`runtime/coop_ext.c` (the glue between the room and the modules that share more than progress, named in "Playing
together").

**The Steam library.** `steam.c` loads `steam_api64.dll` from beside the exe at run time and calls the plain C
functions it exports; it declares them itself, and nothing of the Steamworks SDK is in this repository. It sets
`SteamAppId` and `SteamGameId` to 480 in its own environment before the library loads. Callbacks are taken with the
library's manual dispatch on the co-op thread, the only thread that uses the library. A library that lacks one of the
calls is refused as too old.

**The room.** A room is a friends-only Steam lobby that carries the note `nx2navi = 1`; a lobby without it belongs to
some other program that uses the test app and is left at once. The lobby's owner is the host. A game that entered the
lobby says HELLO to the host with its protocol number and game build; the host answers WELCOME or REFUSED and sends
everyone the list of the players it let in. Every game takes positions, chat, progress and the modules' messages only
from the players on the host's list, and only a lobby member's session is accepted, so a stranger on Steam, a player
the host removed and a game of another version reach nobody. The protocol number is one more whenever a message
changes, so that games that would not understand each other are refused as another version.

**Messages** go through Steam's networking messages (datagrams to a Steam id): positions unreliable, twenty a second
while Link is in the world; everything else reliable. Steam vouches for the sender's id and encrypts. Each message is
one kind byte and a body whose length is checked before it is read; `coop.c`'s header comment lists them. Kinds from
0x40 on are the co-op modules' (`runtime/coop_ext_wire.h` has their bytes and codecs): the Link's pose and what it plays
(PUPPET, unreliable, about 15 a second while other players are in the room) and its gear (GEAR, when it changes and
when a player comes), the colour each player picks (PICK) and the host's colours (COLOURS), kills and hits (KILL,
DAMAGE), cutscenes (CUTSCENE), the time of day (TIME), the era (ERA) and the small keys (KEYS from the host, KEY from
the others), all of them reliable but PUPPET. `coop_ext.c` gives the players of the room seats 1 to 4 for as long as
they stay and hands each message to its part, at most so many a second of each kind from each player.

**Progress.** The game keeps its progress as flags in a database keyed by 32-bit hashes of flag names
(`coop_game.c`'s header comment has the layout). Between two frames the runtime reads the flags that changed and sends
the ones that rose; it sets the ones the room has and this game lacks through the game's own set routines, as if the
player had done it, at most sixteen a frame. Nothing is read or set on the title screen, while a save loads or an area
changes. A game that starts to share asks the others for their whole state and sends its own. Games merge only when
their tables of what is shared are the same (a sum over the table travels with every message).

**The host's part.** It decides who is in the room, and it gives each player their colour (COLOURS), at once to a
player who comes and again at each change, so that a new host carries on with the colours the room had. Progress has
no arbiter, because every shared value only ever rises: two games that set the same flag agree whatever the order.

## Checks

- Two games on one PC cannot both sign in to one Steam account, so the room, the messages and the game side are
  tested with a stand-in for Steam: `NX2NAVI_COOP_LOCAL=<port>` in the environment (never from `nx2navi.ini`) makes a
  game the player whose id is that UDP port on 127.0.0.1; `NX2NAVI_COOP_LOCAL_MAKE=1` makes its room at once and
  `NX2NAVI_COOP_LOCAL_JOIN=<port>` joins another game's. `NX2NAVI_COOP_TRACE=1` logs every two seconds where this
  game's Link is and where it has the others. The stand-in names each player `Player <port>`;
  `NX2NAVI_COOP_LOCAL_NAMES=<port>=<name>,<port>=<name>,...` (environment only) names those ports instead, test
  peers' included, for pictures. Give every game of the test the same list, so that each shows the same names.
- `python tests/coop_peer.py --host <port>` is a second player without a second game (two games need twice the
  memory): it joins the room of a game started with `NX2NAVI_COOP_LOCAL=<port>` and `NX2NAVI_COOP_LOCAL_MAKE=1`,
  stands two metres beside the game's Link so that its name shows there, says a chat line, asks for the game's
  progress and, with `--flags`, sends flags as set. It ends with `peer: ok` when it was let in, saw the game's Link
  in the world and got its progress. Run it on a copy of a profile: the flags it sends reach that save.
- `python tests/coop_ext_peer.py --host <port>` is the same second player, and also speaks the modules' messages: it
  stands two metres beside the game's Link, says a chat line and asks for the game's progress, and its modes send what
  another player would: flags (`--flags`), its own Link from the game's pose and gear (`--mirror`), a colour (`--pick`),
  a kill of an enemy the game hit (`--kill-after-damage`), a cutscene (`--cut`), a time of day (`--time`), an era
  (`--era`) and small keys (`--key`); its docstring lists them. It ends with `peer: ok` or `peer: FAILED` for what its
  modes check. Run it on a copy of a profile: what it sends reaches that save.
- `ocarina --probe-coop-ext` checks the codecs of the co-op modules' messages against the byte vectors of
  `tests/coop_ext_vectors.txt` (which `tests/coop_ext_peer.py --write-vectors` writes from a codec of its own), their
  refusals, every shorter and longer copy of each message, and random bytes. Needs no window, GPU or game files.
- Test hooks, from the environment or `nx2navi.ini`, logged at the start: `NX2NAVI_COOP_PUPPET_TEST=shadow` (a puppet
  that copies this game's own Link, beside him), `NX2NAVI_COOP_KILL_TEST` (hits and kills as the player's own, or as
  another player's) and the debug tools' `NX2NAVI_DBG_TEST` (`warp`, `event`, `flag`); the header comments of
  `coop_puppet.c`, `coop_kill.c` and `debug_tools.c` give their forms.
- `NX2NAVI_MENU_TEST_SET=<frame>:COOP_DO=create | leave | join:<room id> | chat:<text> | page:join | page:invite |
  page:chat | page:leave` (environment only) does what the Co-op tab's row or window does.
- `ocarina --probe-services` checks, among the rest, that every text of the tab has its four languages.
