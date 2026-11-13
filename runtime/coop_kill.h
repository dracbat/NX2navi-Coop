// Co-op kills, shared damage and difficulty (coop_kill.c), game 1.1.1.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "cpu.h"

// coop_puppet_init, after coop_init: the KILL and DAMAGE handlers, the roster and tick hooks on the co-op thread.
void coop_kill_init(void);
// coop_puppet_present (present thread, every present): settings, the players present and the difficulty factors, the
// life hook's switch (the patch set coop_life), expiry of what other players sent. 1 when the frame hook should run
// for this module (coop_kill_frame).
int coop_kill_present(void);
// puppet_frame (the game's update thread, between two frames): the area the local Link is in and his life component,
// for the life hook on the actors' threads.
void coop_kill_frame(Cpu *c, uint64_t player, const char *area);

// ---- the wire format (pure functions)
enum { COOP_K_KILL = 0x40, COOP_K_DAMAGE = 0x41 };   // coop_proto.h's kinds are 1..7, tests 0xF0 and above
enum { COOP_KILL_VERSION = 1, COOP_KILL_AREA_MAX = 47, COOP_KILL_MSG_MAX = 80 };
enum { COOP_KILL_BOSS = 1 };   // flags
typedef struct {
    uint32_t epoch;              // the sender's process: random at start (a restart starts the receiver's window again)
    uint32_t eseq;               // the sender's event counter (KILL and DAMAGE together); the 3 copies of one event share it
    char area[COOP_KILL_AREA_MAX + 1];   // the sender's Sequence_CurrentBanc
    uint64_t inst;               // the enemy's placement hash *(*(actor+0x3d8)+0x88)
    uint32_t name_hash;          // FNV-1a of the actor pack's name
    uint16_t raw;                // DAMAGE: the hit as the sender's game made it, before any scaling (1..1000)
    uint8_t flags;               // COOP_KILL_BOSS
} CoopKillMsg;
// Bytes (little-endian): u8 version, u32 epoch, u32 eseq, u8 area length (1..47), the area, u64 inst, u32 name hash,
// [DAMAGE: u16 raw], u8 flags. 0 when it does not fit or a value is out of range.
size_t coop_kill_pack(uint8_t kind, const CoopKillMsg *m, uint8_t *out, size_t cap);
// 1 when bytes are exactly one message of that kind: version 1, a printable area, inst != 0, DAMAGE raw 1..1000.
int coop_kill_parse(uint8_t kind, const uint8_t *b, size_t n, CoopKillMsg *m);
uint32_t coop_kill_name_hash(const char *name);
