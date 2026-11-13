// NX2navi Co-op's features, while they are moved onto the co-op through Steam (coop.c, coop_game.c): the other players'
// Links (coop_puppet.c), shared kills, hits and difficulty (coop_kill.c), and cutscenes, the time of day and the era
// together (coop_cutscene.c) were written for the co-op of versions up to 0.9.2, whose room layer is gone. Until each is
// moved onto the new room, its source waits in runtime/coop_pending, a folder the build does not read, and the two entry
// points the rest of the runtime calls are these, which do nothing. Their patch sets, coop_puppet and coop_life
// (recomp/patches-v111.txt), default to off and only those modules switch them on, so the game runs as without them.
// A step that moves coop_puppet.c back into runtime/ deletes this file.
#include "coop_puppet.h"

void coop_puppet_init(void) {}
void coop_puppet_present(void) {}
