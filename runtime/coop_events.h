// The player's own event table (coop_events.c): which events of the game are story cutscenes, which players of a
// co-op room watch together (coop_cutscene.c), and which are talks with NPCs, whose flags stay each player's own
// (coop_game.c: dialog per player). Read from the game's RomFS once, on a thread of its own, when first asked for.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Any thread: 0 not asked yet (this call starts the reading), 1 being read, 2 read (or given up: the lists are empty).
int coop_events_state(void);
// The flags of the NPC talk events (HavePlayedEvent.<event> of every event whose component list is an NPC one: talks,
// "near" lines, chases, each-frame lines), sorted; the count, 0 until read.
int coop_events_talk_flags(const uint32_t **out);

// A story cutscene: a row Dm_* of the table that is neither Dm_SY_* nor Dm_Title_* (whose component lists are not NPC
// ones either).
typedef struct {
    const char *name;
    uint8_t movie;   // a pre-rendered movie (its EventFileType is Movie): it plays anywhere
    uint8_t boss;    // *_BossIn or *_BossOut
} CoopStory;
// The story cutscene of that event name; NULL when the event is none (or the table is not read yet).
const CoopStory *coop_events_story(const char *name);

#ifdef __cplusplus
}
#endif
