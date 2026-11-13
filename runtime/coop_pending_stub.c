// NX2navi Co-op's features, while they are moved onto the co-op through Steam (coop.c, coop_ext.h): cutscenes, the time
// of day and the era together (coop_cutscene.c) were written for the co-op of versions up to 0.9.2, whose room layer is
// gone. Until that module is moved onto the new room, its source waits in runtime/coop_pending, a folder the build does
// not read, and the entry points the other players' Links call (coop_puppet.c) are these, which do nothing. The step
// that moves it back into runtime/ deletes this file.
#include "coop_cutscene.h"

void coop_cutscene_init(void) {}
int coop_cutscene_present(void) { return 0; }
void coop_cutscene_frame(Cpu *c, uint64_t player, const char *area) { (void)c, (void)player, (void)area; }
int coop_cutscene_hides_puppets(void) { return 0; }
