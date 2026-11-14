"""A second co-op player without a second game, for tests on one PC, and the Python codec of the co-op modules'
messages (runtime/coop_ext_wire.h, kinds 0x40..0x4a).

It speaks the stand-in for Steam (runtime/coop_local.c: datagrams between UDP ports on 127.0.0.1) and the room's
messages (runtime/coop.c, runtime/coop_game.c) to a game started with NX2NAVI_COOP_LOCAL=<host port> and
NX2NAVI_COOP_LOCAL_MAKE=1, says HELLO with the game's protocol (2; --protocol to say another), and once let in
stands two metres beside the game's Link, says a chat line and asks for the game's shared progress:

    python tests/coop_ext_peer.py --host 27601 [--port 27602] [--seconds 120] [modes]

Modes (any together):
    --flags HASH[,HASH...]   send those flags (hex hashes) with the value 1, as the room's progress
    --flags-at S:HASH[,HASH...]  the same, S seconds after the game's Link was first seen in the world (may be given
                             again for other times)
    --expect-refused         the game must refuse this peer for its version (with --protocol 1: an older build's)
    --separation             first, from a second port, say HELLO with protocol 1, which the game must refuse
    --echo                   (the game with NX2NAVI_COOP_EXT_TEST=echo, which sends every kind back and hands none to
                             its modules) send every message of the vectors file and get each valid one back as it was
                             sent, none of the refused ones and none twice; then a flood of PICK, which the game's rate
                             for the kind must cut. What the game's modules send of their own is not the echo's
    --mirror[=METRES]        send the game's own PUPPET and GEAR back as this peer's, moved METRES (2) aside
    --pick COLOUR            PICK that colour (green, red, blue, purple or auto) once let in
    --pick-at S:COLOUR       PICK that colour S seconds after the game's Link was first seen in the world
    --kill-after-damage      answer each DAMAGE the game sends with a KILL of the same enemy
    --cut EVENT:AREA:X,Y,Z@S a story cutscene's START, S seconds after the game's Link was first seen
    --cut-movie              ... with the MOVIE bit set, whatever the scene is (the game must go by its own table)
    --time HH:MM[/ORDER]@S   a jump of the time of day, S seconds after the game's Link was first seen, with the order
                             given (default: one more than the newest heard); may be given again
    --damage-flood N@S       N DAMAGE messages for made-up enemies of the game's area at once, S seconds after the
                             game's Link was first seen (the game keeps 16 of one sender's at most)
    --era child|adult[:AREA[:X,Y,Z]]@S   a change of era, S seconds after the game's Link was first seen, with a place of
                             that era (default: where the game's Link stands)
    --key HASH:N[:X,Y,Z]@S   a small key change (KEY) to the room's host, S seconds after the game's Link was first seen:
                             N keys picked up (N > 0) or used (N < 0) in the dungeon of that small-key counter (hex hash),
                             with X,Y,Z the place where this peer's Link stood (the host counts one key a door); may be
                             given again. Each must be taken: a later KEYS of the host acks it

It prints what the game sends, with the kinds' names, and ends with "peer: ok" or "peer: FAILED" for what its modes
check; python's exit code says the same.

The codec: --write-vectors PATH writes the byte vectors (tests/coop_ext_vectors.txt) that runtime/coop_ext_probe.c
(--probe-coop-ext) checks the C codec against, and --check-vectors PATH checks this codec against them. Standard
library only.
"""
import argparse
import math
import random
import socket
import struct
import sys
import time
from pathlib import Path

L_JOIN, L_MEMBERS, L_DATA, L_LEAVE = 1, 2, 3, 4
K_HELLO, K_WELCOME, K_REFUSED, K_ROSTER, K_POSE, K_CHAT = 1, 2, 3, 4, 5, 6
K_FLAGS, K_ASK = 16, 17
K_KILL, K_DAMAGE, K_CUTSCENE, K_TIME, K_ERA, K_PUPPET, K_GEAR, K_PICK, K_COLOURS, K_KEYS, K_KEY = range(0x40, 0x4b)
KIND_NAMES = {K_KILL: 'KILL', K_DAMAGE: 'DAMAGE', K_CUTSCENE: 'CUTSCENE', K_TIME: 'TIME', K_ERA: 'ERA',
              K_PUPPET: 'PUPPET', K_GEAR: 'GEAR', K_PICK: 'PICK', K_COLOURS: 'COLOURS', K_KEYS: 'KEYS', K_KEY: 'KEY'}
PROTOCOL, GAME_BUILD = 2, 0xE0F3CCFA
COLOUR_NAMES = ['green', 'red', 'blue', 'purple']
COLOUR_NONE = 0xFF
PLAYERS = 4
DAY = 86400
TEXT, SLOTS, GEAR_DYN, GEAR_STAT = 47, 4, 7, 13
KEYS_DUNGEONS, KEYS_MAX = 16, 15
VECTORS = Path(__file__).resolve().parent / 'coop_ext_vectors.txt'


class Bad(Exception):
    """Bytes or fields that the codec refuses."""


# ---- the codec (runtime/coop_ext_wire.c, the same rules)

def text_ok(b, space):
    lo = 0x20 if space else 0x21
    return all(lo <= c <= 0x7e for c in b)


def f32(x):
    """x as the float32 the C codec holds (finite values only)."""
    return struct.unpack('<f', struct.pack('<f', x))[0]


def bounded(f, top):
    return math.isfinite(f) and abs(f) <= top


def need(cond):
    if not cond:
        raise Bad()


class Reader:
    def __init__(self, b):
        self.b, self.at = b, 0

    def take(self, n):
        need(self.at + n <= len(self.b))
        out = self.b[self.at:self.at + n]
        self.at += n
        return out

    def u8(self):
        return self.take(1)[0]

    def u16(self):
        return struct.unpack('<H', self.take(2))[0]

    def u32(self):
        return struct.unpack('<I', self.take(4))[0]

    def u64(self):
        return struct.unpack('<Q', self.take(8))[0]

    def f(self):
        return struct.unpack('<f', self.take(4))[0]

    def text(self, lo, hi, space):
        n = self.u8()
        need(lo <= n <= hi)
        t = self.take(n)
        need(text_ok(t, space))
        return t

    def xyz(self, top=1e6):
        v = [self.f() for _ in range(3)]
        need(all(bounded(x, top) for x in v))
        return v

    def end(self):
        need(self.at == len(self.b))


def put_text(t):
    return bytes([len(t)]) + t


def put_xyz(v):
    return struct.pack('<3f', *v)


def kill_name_hash(name):
    h = 2166136261
    for c in name:
        h = ((h ^ c) * 16777619) & 0xFFFFFFFF
    return h


def kill_pack(kind, m):
    a = m['area']
    need(kind in (K_KILL, K_DAMAGE) and 1 <= len(a) <= 47 and text_ok(a, False) and m['inst'] not in (0, 2**64 - 1)
         and not m['flags'] & ~1)
    need(kind != K_DAMAGE or 1 <= m['raw'] <= 1000)
    b = struct.pack('<BIIB', 1, m['epoch'], m['eseq'], len(a)) + a + struct.pack('<QI', m['inst'], m['hash'])
    if kind == K_DAMAGE:
        b += struct.pack('<H', m['raw'])
    return b + bytes([m['flags']])


def kill_parse(kind, b):
    need(kind in (K_KILL, K_DAMAGE))
    r = Reader(b)
    need(r.u8() == 1)
    m = {'epoch': r.u32(), 'eseq': r.u32()}
    m['area'] = r.text(1, 47, False)
    m['inst'], m['hash'] = r.u64(), r.u32()
    m['raw'] = r.u16() if kind == K_DAMAGE else 0
    m['flags'] = r.u8()
    r.end()
    kill_pack(kind, m)   # the same range checks
    return m


def cut_pack(m):
    e, a = m['event'], m['area']
    need(m['op'] in (1, 2) and not m['flags'] & ~3 and 1 <= len(e) <= 79 and len(a) <= 63 and text_ok(e, False)
         and text_ok(a, False) and all(bounded(x, 1e6) for x in m['xyz']))
    return struct.pack('<BBIIB', 1, m['op'], m['epoch'], m['seq'], m['flags']) + put_text(e) + put_text(a) + \
        put_xyz(m['xyz'])


def cut_parse(b):
    r = Reader(b)
    need(r.u8() == 1)
    m = {'op': r.u8(), 'epoch': r.u32(), 'seq': r.u32(), 'flags': r.u8()}
    need(m['op'] in (1, 2) and not m['flags'] & ~3)
    m['event'], m['area'] = r.text(1, 79, False), r.text(0, 63, False)
    m['xyz'] = r.xyz()
    r.end()
    return m


def time_pack(m):
    need(m['tod'] < DAY and not m['flags'] & ~1)
    return struct.pack('<BIIIIB', 1, m['epoch'], m['seq'], m['order'], m['tod'], m['flags'])


def time_parse(b):
    need(len(b) == 18 and b[0] == 1)
    _, epoch, seq, order, tod, flags = struct.unpack('<BIIIIB', b)
    need(tod < DAY and not flags & ~1)
    return {'epoch': epoch, 'seq': seq, 'order': order, 'tod': tod, 'flags': flags}


def era_pack(m):
    need(not m['flags'] & ~7 and len(m['scene']) <= 79 and len(m['scene_area']) <= 63 and len(m['area']) <= 63)
    need(all(text_ok(m[k], False) for k in ('scene', 'scene_area', 'area')))
    need(all(bounded(x, 1e6) for x in m['scene_xyz'] + m['xyz']))
    return struct.pack('<BIIIB', 1, m['epoch'], m['seq'], m['order'], m['flags']) + put_text(m['scene']) + \
        put_text(m['scene_area']) + put_xyz(m['scene_xyz']) + put_text(m['area']) + put_xyz(m['xyz'])


def era_parse(b):
    r = Reader(b)
    need(r.u8() == 1)
    m = {'epoch': r.u32(), 'seq': r.u32(), 'order': r.u32(), 'flags': r.u8()}
    need(not m['flags'] & ~7)
    m['scene'], m['scene_area'] = r.text(0, 79, False), r.text(0, 63, False)
    m['scene_xyz'] = r.xyz()
    m['area'] = r.text(0, 63, False)
    m['xyz'] = r.xyz()
    r.end()
    return m


def puppet_pack(m):
    a = m['area']
    need(not m['flags'] & ~3 and not m['upper'] & ~7 and len(m['slots']) <= SLOTS and len(a) <= TEXT and text_ok(a, True))
    need(all(bounded(x, 1e6) for x in m['pos']) and bounded(m['yaw'], 7.0))
    b = struct.pack('<BIIB', 1, m['pseq'], m['t_ms'], m['flags']) + put_text(a) + put_xyz(m['pos']) + \
        struct.pack('<fB', m['yaw'], len(m['slots']))
    for node, frame in m['slots']:
        need(len(node) <= TEXT and text_ok(node, True) and bounded(frame, 1e6))
        b += put_text(node) + struct.pack('<f', frame)
    return b + struct.pack('<BI', m['upper'], m['gear_sig'])


def puppet_parse(b):
    r = Reader(b)
    need(r.u8() == 1)
    m = {'pseq': r.u32(), 't_ms': r.u32(), 'flags': r.u8()}
    need(not m['flags'] & ~3)
    m['area'] = r.text(0, TEXT, True)
    m['pos'] = [r.f() for _ in range(3)]
    m['yaw'] = r.f()
    n = r.u8()
    need(all(bounded(x, 1e6) for x in m['pos']) and bounded(m['yaw'], 7.0) and n <= SLOTS)
    m['slots'] = []
    for _ in range(n):
        node = r.text(0, TEXT, True)
        frame = r.f()
        need(bounded(frame, 1e6))
        m['slots'].append((node, frame))
    m['upper'], m['gear_sig'] = r.u8(), r.u32()
    need(not m['upper'] & ~7)
    r.end()
    return m


def gear_sig(dyn, stat):
    """dyn: GEAR_DYN (name, state) with b'' for none; stat: GEAR_STAT names."""
    h = 2166136261
    for name, state in dyn:
        for c in name:
            h = ((h ^ c) * 16777619) & 0xFFFFFFFF
        h = ((h ^ (0x100 + state if name else 0x55)) * 16777619) & 0xFFFFFFFF
    for name in stat:
        for c in name:
            h = ((h ^ c) * 16777619) & 0xFFFFFFFF
        h = ((h ^ 0x77) * 16777619) & 0xFFFFFFFF
    return h or 1


def gear_pack(g):
    dyn, stat = g['dyn'], g['stat']
    need(len(dyn) == GEAR_DYN and len(stat) == GEAR_STAT)
    dmask = smask = 0
    body = b''
    for j, (name, state) in enumerate(dyn):
        if not name:
            continue
        need(len(name) <= TEXT and text_ok(name, True) and 0 <= state <= 2)
        dmask |= 1 << j
        body += bytes([state]) + put_text(name)
    for s, name in enumerate(stat):
        if not name:
            continue
        need(len(name) <= TEXT and text_ok(name, True))
        smask |= 1 << s
        body += put_text(name)
    return struct.pack('<BIBH', 1, gear_sig(dyn, stat), dmask, smask) + body


def gear_parse(b):
    r = Reader(b)
    need(r.u8() == 1)
    sig, dmask, smask = r.u32(), r.u8(), r.u16()
    need(not dmask >> GEAR_DYN and not smask >> GEAR_STAT)
    dyn = [(b'', 0)] * GEAR_DYN
    stat = [b''] * GEAR_STAT
    for j in range(GEAR_DYN):
        if dmask >> j & 1:
            state = r.u8()
            need(state <= 2)
            dyn[j] = (r.text(1, TEXT, True), state)
    for s in range(GEAR_STAT):
        if smask >> s & 1:
            stat[s] = r.text(1, TEXT, True)
    r.end()
    need(gear_sig(dyn, stat) == sig)
    return {'sig': sig, 'dyn': dyn, 'stat': stat}


def pick_pack(pick):
    need(pick < 4 or pick == COLOUR_NONE)
    return bytes([1, pick])


def pick_parse(b):
    need(len(b) == 2 and b[0] == 1 and (b[1] < 4 or b[1] == COLOUR_NONE))
    return b[1]


def colours_pack(entries):
    need(len(entries) <= PLAYERS)
    ids = [i for i, _ in entries]
    cols = [c for _, c in entries]
    need(all(i for i in ids) and all(c < 4 for c in cols) and len(set(ids)) == len(ids) and len(set(cols)) == len(cols))
    return bytes([1, len(entries)]) + b''.join(struct.pack('<QB', i, c) for i, c in entries)


def colours_parse(b):
    need(len(b) >= 2 and b[0] == 1 and b[1] <= PLAYERS and len(b) == 2 + 9 * b[1])
    entries = [struct.unpack_from('<QB', b, 2 + 9 * i) for i in range(b[1])]
    colours_pack(entries)
    return entries


def keys_pack(m):
    d, acks = m['d'], m['acks']
    need(len(d) <= KEYS_DUNGEONS and len(acks) <= PLAYERS)
    need(all(h and 0 <= k <= KEYS_MAX for h, k in d) and len({h for h, _ in d}) == len(d))
    need(all(i and ss for i, ss, _ in acks) and len({i for i, _, _ in acks}) == len(acks))
    return (struct.pack('<BIB', 1, m['serial'], len(d)) + b''.join(struct.pack('<IB', h, k) for h, k in d) +
            bytes([len(acks)]) + b''.join(struct.pack('<QII', i, ss, q) for i, ss, q in acks))


def keys_parse(b):
    need(len(b) >= 7 and b[0] == 1 and b[5] <= KEYS_DUNGEONS)
    n = b[5]
    at = 6 + 5 * n
    need(at < len(b))
    d = [struct.unpack_from('<IB', b, 6 + 5 * i) for i in range(n)]
    na = b[at]
    need(na <= PLAYERS and len(b) == at + 1 + 16 * na)
    acks = [struct.unpack_from('<QII', b, at + 1 + 16 * i) for i in range(na)]
    m = {'serial': struct.unpack_from('<I', b, 1)[0], 'd': d, 'acks': acks}
    keys_pack(m)   # the same range checks
    return m


KEY_PLACE = 1


def key_pack(m):
    need(m['sess'] and m['seq'] and m['hash'] and m['delta'] and -KEYS_MAX <= m['delta'] <= KEYS_MAX)
    need(m['flags'] in (0, KEY_PLACE))
    need(all(bounded(c, 1e6) for c in m['xyz']) if m['flags'] else all(c == 0.0 for c in m['xyz']))
    return struct.pack('<BIIIbB3f', 1, m['sess'], m['seq'], m['hash'], m['delta'], m['flags'], *m['xyz'])


def key_parse(b):
    need(len(b) == 27 and b[0] == 1)
    _, sess, seq, h, delta, flags, x, y, z = struct.unpack('<BIIIbB3f', b)
    m = {'sess': sess, 'seq': seq, 'hash': h, 'delta': delta, 'flags': flags, 'xyz': [x, y, z]}
    key_pack(m)
    return m


PARSERS = {K_KILL: lambda b: kill_parse(K_KILL, b), K_DAMAGE: lambda b: kill_parse(K_DAMAGE, b), K_CUTSCENE: cut_parse,
           K_TIME: time_parse, K_ERA: era_parse, K_PUPPET: puppet_parse, K_GEAR: gear_parse, K_PICK: pick_parse,
           K_COLOURS: colours_parse, K_KEYS: keys_parse, K_KEY: key_parse}
PACKERS = {K_KILL: lambda m: kill_pack(K_KILL, m), K_DAMAGE: lambda m: kill_pack(K_DAMAGE, m), K_CUTSCENE: cut_pack,
           K_TIME: time_pack, K_ERA: era_pack, K_PUPPET: puppet_pack, K_GEAR: gear_pack, K_PICK: pick_pack,
           K_COLOURS: colours_pack, K_KEYS: keys_pack, K_KEY: key_pack}


def parses(kind, b):
    try:
        PARSERS[kind](b)
        return True
    except (Bad, struct.error):
        return False


# ---- the byte vectors (tests/coop_ext_vectors.txt)

VECTOR_HEADER = """\
# Byte vectors of the co-op modules' messages (runtime/coop_ext_wire.h, kinds 0x40..0x4a). Written by
# `python tests/coop_ext_peer.py --write-vectors tests/coop_ext_vectors.txt`, checked by --check-vectors and by
# runtime/coop_ext_probe.c (--probe-coop-ext, the file named by NX2NAVI_COOP_EXT_VECTORS): each message must parse to
# the fields given and pack to the same bytes; every shorter or longer copy of it must be refused.
# Tokens are separated by spaces. <hex> is the message after its kind byte; a text is hex of its bytes, "-" empty;
# numbers are decimal; floats are float32 values in %.9g.
#
# kill <name> <hex> <epoch> <eseq> <area> <inst> <name hash> <flags>
# damage <name> <hex> <epoch> <eseq> <area> <inst> <name hash> <raw> <flags>
# cutscene <name> <hex> <op> <flags> <epoch> <seq> <event> <area> <x> <y> <z>
# time <name> <hex> <epoch> <seq> <order> <seconds of the day> <flags>
# era <name> <hex> <epoch> <seq> <order> <flags> <scene> <scene area> <x> <y> <z> <area> <x> <y> <z>
# puppet <name> <hex> <pseq> <t_ms> <flags> <area> <x> <y> <z> <yaw> <upper> <gear sig> <slots> (<node> <frame>)...
# gear <name> <hex> <sig> (d<slot>:<state>:<name>)... (s<slot>:<name>)...
# pick <name> <hex> <pick>
# colours <name> <hex> <count> (<id> <colour>)...
# keys <name> <hex> <serial> <count> (<hash> <keys>)... <acks> (<id> <sess> <seq>)...
# key <name> <hex> <sess> <seq> <hash> <change> <flags> <x> <y> <z>
# reject <kind> <name> <hex>                       the kind's parser must refuse it
"""


def tx(b):
    return b.hex() if b else '-'


def fl(x):
    return '%.9g' % x


def vector_set():
    """[(kind, name, message dict or entries)] of valid messages, and [(kind, name, bytes)] that must be refused."""
    good, bad = [], []
    k1 = {'epoch': 0x12345678, 'eseq': 1, 'area': b'TestArea_A', 'inst': 0x0123456789ABCDEF, 'hash': kill_name_hash(b'Enemy_A'),
          'raw': 0, 'flags': 0}
    good += [(K_KILL, 'kill', k1),
             (K_KILL, 'kill-boss-long-area', dict(k1, eseq=0xFFFFFFFF, area=b'A' * 47, inst=1, flags=1)),
             (K_DAMAGE, 'damage', dict(k1, eseq=2, raw=3)),
             (K_DAMAGE, 'damage-max', dict(k1, eseq=3, raw=1000, inst=2**64 - 2, area=b'x'))]
    c1 = {'op': 1, 'flags': 1, 'epoch': 7, 'seq': 1, 'event': b'Dm_Test_0010', 'area': b'TestArea_B',
          'xyz': [f32(12.5), f32(-3.25), f32(100.0)]}
    good += [(K_CUTSCENE, 'cutscene-start', c1),
             (K_CUTSCENE, 'cutscene-end-movie', dict(c1, op=2, flags=2, seq=2, area=b'', xyz=[0.0, -0.0, f32(1e6)])),
             (K_CUTSCENE, 'cutscene-long', dict(c1, event=b'E' * 79, area=b'a' * 63, xyz=[f32(-1e6), 0.0, 0.0]))]
    good += [(K_TIME, 'time-beat', {'epoch': 9, 'seq': 1, 'order': 0, 'tod': 0, 'flags': 0}),
             (K_TIME, 'time-jump', {'epoch': 9, 'seq': 2, 'order': 5, 'tod': DAY - 1, 'flags': 1})]
    e1 = {'epoch': 11, 'seq': 1, 'order': 1, 'flags': 3, 'scene': b'Dm_Test_0020', 'scene_area': b'TestArea_C',
          'scene_xyz': [f32(1.5), f32(2.5), f32(3.5)], 'area': b'TestArea_D', 'xyz': [f32(-7.0), f32(8.0), f32(0.125)]}
    good += [(K_ERA, 'era-change', e1),
             (K_ERA, 'era-beat-empty', dict(e1, seq=2, flags=0, scene=b'', scene_area=b'', area=b'', scene_xyz=[0.0] * 3,
                                            xyz=[0.0] * 3)),
             (K_ERA, 'era-longest', dict(e1, flags=7, scene=b'S' * 79, scene_area=b's' * 63, area=b'a' * 63))]
    p1 = {'pseq': 1, 't_ms': 123456, 'flags': 2, 'area': b'Test Area', 'pos': [f32(1234.5), f32(-56.25), f32(789.125)],
          'yaw': f32(1.5), 'slots': [(b'Walk', f32(12.0)), (b'Lower_Walk', f32(3.5))], 'upper': 6, 'gear_sig': 0x89ABCDEF}
    good += [(K_PUPPET, 'puppet-walk', p1),
             (K_PUPPET, 'puppet-not-in-world', dict(p1, pseq=2, flags=0, area=b'', pos=[0.0] * 3, yaw=0.0, slots=[], upper=0,
                                                    gear_sig=0)),
             (K_PUPPET, 'puppet-adult-edges', dict(p1, pseq=0xFFFFFFFF, flags=3, pos=[f32(1e6), f32(-1e6), 0.0],
                                                   yaw=-7.0, upper=7,
                                                   slots=[(b'', 0.0), (b'n' * 47, f32(1e6)), (b'~ ok', f32(-1e6)),
                                                          (b'Upper', f32(0.5))])),
             (K_PUPPET, 'puppet-longest', dict(p1, area=b'a' * 47, slots=[(b'n' * 47, f32(i)) for i in range(4)]))]
    none = [(b'', 0)] * GEAR_DYN
    g1 = {'dyn': [(b'Sword', 2), (b'Shield', 1)] + none[2:4] + [(b'Ocarina', 0)] + none[5:],
          'stat': [b'', b'Tunic', b'', b'Boots'] + [b''] * 8 + [b'Sheath']}
    good += [(K_GEAR, 'gear', g1),
             (K_GEAR, 'gear-none', {'dyn': none, 'stat': [b''] * GEAR_STAT}),
             (K_GEAR, 'gear-full', {'dyn': [(bytes([65 + j]) * 47, j % 3) for j in range(GEAR_DYN)],
                                    'stat': [bytes([97 + s]) * 47 for s in range(GEAR_STAT)]}),
             (K_GEAR, 'gear-spaces', {'dyn': [(b' ', 0)] + none[1:], 'stat': [b'~ x ~'] + [b''] * 12})]
    good += [(K_PICK, 'pick-green', 0), (K_PICK, 'pick-purple', 3), (K_PICK, 'pick-auto', COLOUR_NONE)]
    good += [(K_COLOURS, 'colours-none', []), (K_COLOURS, 'colours-two', [(101, 0), (102, 1)]),
             (K_COLOURS, 'colours-four', [(2**64 - 1, 3), (1, 2), (0x123456789ABCDEF0, 1), (12345, 0)])]
    good += [(K_KEYS, 'keys-empty', {'serial': 0, 'd': [], 'acks': []}),
             (K_KEYS, 'keys-two', {'serial': 7, 'd': [(0x7DD79D10, 3), (0x90237092, 0)], 'acks': [(21, 5, 9)]}),
             (K_KEYS, 'keys-full', {'serial': 0xFFFFFFFF, 'd': [(0x1000 + i, i % (KEYS_MAX + 1)) for i in range(KEYS_DUNGEONS)],
                                    'acks': [(2**64 - 1, 1, 0), (1, 0xFFFFFFFF, 0xFFFFFFFF), (0x123456789ABCDEF0, 2, 3),
                                             (12345, 4, 1)]})]
    no_place = {'flags': 0, 'xyz': [0.0] * 3}
    good += [(K_KEY, 'key-picked-up', dict(no_place, sess=5, seq=1, hash=0x7DD79D10, delta=1)),
             (K_KEY, 'key-used', {'sess': 0xFFFFFFFF, 'seq': 0xFFFFFFFF, 'hash': 0xFFFFFFFF, 'delta': -1, 'flags': KEY_PLACE,
                                  'xyz': [f32(-10.25), f32(100.0), f32(-21.5)]}),
             (K_KEY, 'key-edges', {'sess': 1, 'seq': 2, 'hash': 1, 'delta': -KEYS_MAX, 'flags': KEY_PLACE,
                                   'xyz': [f32(1e6), f32(-1e6), -0.0]}),
             (K_KEY, 'key-used-no-place', dict(no_place, sess=3, seq=4, hash=0x90237092, delta=-2))]

    def raw(kind, m):
        return PACKERS[kind](m)
    # what the parsers must refuse: wrong versions, flag bits, lengths, texts, numbers and ranges
    kb = raw(K_KILL, k1)
    bad += [(K_KILL, 'kill-version', b'\x02' + kb[1:]), (K_KILL, 'kill-area-empty', kb[:9] + b'\x00' + kb[20:]),
            (K_KILL, 'kill-area-space', kb[:10] + b'Test Area_' + kb[20:]),
            (K_KILL, 'kill-area-control', kb[:10] + b'TestArea\x01A' + kb[20:]),
            (K_KILL, 'kill-inst-zero', kb[:20] + bytes(8) + kb[28:]),
            (K_KILL, 'kill-inst-ones', kb[:20] + b'\xff' * 8 + kb[28:]), (K_KILL, 'kill-flags', kb[:-1] + b'\x02'),
            (K_DAMAGE, 'damage-is-kill', kb), (K_DAMAGE, 'damage-raw-zero', kb[:-1] + b'\x00\x00\x00'),
            (K_DAMAGE, 'damage-raw-high', kb[:-1] + struct.pack('<H', 1001) + b'\x00')]
    cb = raw(K_CUTSCENE, c1)
    bad += [(K_CUTSCENE, 'cutscene-op', cb[:1] + b'\x03' + cb[2:]), (K_CUTSCENE, 'cutscene-flags', cb[:10] + b'\x04' + cb[11:]),
            (K_CUTSCENE, 'cutscene-event-empty', cb[:11] + b'\x00' + cb[24:]),
            (K_CUTSCENE, 'cutscene-nan', cb[:-4] + struct.pack('<f', float('nan'))),
            (K_CUTSCENE, 'cutscene-inf', cb[:-4] + struct.pack('<f', float('inf'))),
            (K_CUTSCENE, 'cutscene-far', cb[:-4] + struct.pack('<f', 1.5e6)),
            (K_CUTSCENE, 'cutscene-event-del', cb[:12] + b'Dm_Test_001\x7f' + cb[24:])]
    tb = raw(K_TIME, good[[n for _, n, _ in good].index('time-jump')][2])
    bad += [(K_TIME, 'time-day', tb[:13] + struct.pack('<I', DAY) + tb[17:]), (K_TIME, 'time-flags', tb[:17] + b'\x03'),
            (K_TIME, 'time-version', b'\x00' + tb[1:])]
    eb = raw(K_ERA, e1)
    bad += [(K_ERA, 'era-flags', eb[:13] + b'\x08' + eb[14:]), (K_ERA, 'era-scene-space', eb[:15] + b'Dm Test_0020' + eb[27:]),
            (K_ERA, 'era-nan', eb[:-4] + struct.pack('<f', float('nan'))),
            (K_ERA, 'era-far', eb[:-12] + struct.pack('<f', -2e6) + eb[-8:])]
    pb = raw(K_PUPPET, p1)
    at_pos = 11 + len(p1['area'])
    bad += [(K_PUPPET, 'puppet-flags', pb[:9] + b'\x04' + pb[10:]),
            (K_PUPPET, 'puppet-yaw', pb[:at_pos + 12] + struct.pack('<f', 7.5) + pb[at_pos + 16:]),
            (K_PUPPET, 'puppet-yaw-nan', pb[:at_pos + 12] + struct.pack('<f', float('nan')) + pb[at_pos + 16:]),
            (K_PUPPET, 'puppet-far', pb[:at_pos] + struct.pack('<f', 1.0e7) + pb[at_pos + 4:]),
            (K_PUPPET, 'puppet-inf', pb[:at_pos + 4] + struct.pack('<f', float('-inf')) + pb[at_pos + 8:]),
            (K_PUPPET, 'puppet-slots', pb[:at_pos + 16] + b'\x05' + pb[at_pos + 17:]),
            (K_PUPPET, 'puppet-frame', pb[:-9] + struct.pack('<f', 2e6) + pb[-5:]),
            (K_PUPPET, 'puppet-upper', pb[:-5] + b'\x08' + pb[-4:]),
            (K_PUPPET, 'puppet-area-control', pb[:11] + b'Test\x0aArea' + pb[20:])]
    gb = raw(K_GEAR, g1)
    bad += [(K_GEAR, 'gear-sig', gb[:1] + struct.pack('<I', (struct.unpack_from('<I', gb, 1)[0] + 1) & 0xFFFFFFFF) + gb[5:]),
            (K_GEAR, 'gear-state', gb[:8] + b'\x03' + gb[9:]), (K_GEAR, 'gear-dmask', gb[:5] + b'\x80' + gb[6:]),
            (K_GEAR, 'gear-smask', gb[:6] + struct.pack('<H', 0x2000) + gb[8:]),
            (K_GEAR, 'gear-name-empty', struct.pack('<BIBH', 1, gear_sig(none, [b''] * GEAR_STAT), 1, 0) + b'\x00\x00'),
            (K_GEAR, 'gear-name-control', gb[:10] + b'Swor\x00' + gb[15:])]
    bad += [(K_PICK, 'pick-colour', b'\x01\x04'), (K_PICK, 'pick-version', b'\x02\x00'), (K_PICK, 'pick-long', b'\x01\x00\x00')]
    bad += [(K_COLOURS, 'colours-count', b'\x01\x05' + struct.pack('<QB', 1, 0) * 5),
            (K_COLOURS, 'colours-id-zero', b'\x01\x01' + struct.pack('<QB', 0, 0)),
            (K_COLOURS, 'colours-colour', b'\x01\x01' + struct.pack('<QB', 1, 4)),
            (K_COLOURS, 'colours-same-id', b'\x01\x02' + struct.pack('<QB', 5, 0) + struct.pack('<QB', 5, 1)),
            (K_COLOURS, 'colours-same-colour', b'\x01\x02' + struct.pack('<QB', 5, 2) + struct.pack('<QB', 6, 2))]
    kk = raw(K_KEYS, {'serial': 7, 'd': [(0x7DD79D10, 3)], 'acks': [(21, 5, 9)]})
    bad += [(K_KEYS, 'keys-version', b'\x02' + kk[1:]),
            (K_KEYS, 'keys-count', b'\x01' + kk[1:5] + b'\x11' + struct.pack('<IB', 1, 0) * 17 + b'\x00'),
            (K_KEYS, 'keys-acks', kk[:11] + b'\x05' + struct.pack('<QII', 1, 1, 0) * 5),
            (K_KEYS, 'keys-sixteen', kk[:10] + b'\x10' + kk[11:]),
            (K_KEYS, 'keys-hash-zero', kk[:6] + b'\x00' * 4 + kk[10:]),
            (K_KEYS, 'keys-same-hash', b'\x01' + kk[1:5] + b'\x02' + kk[6:11] * 2 + b'\x00'),
            (K_KEYS, 'keys-id-zero', kk[:12] + b'\x00' * 8 + kk[20:]),
            (K_KEYS, 'keys-sess-zero', kk[:20] + b'\x00' * 4 + kk[24:]),
            (K_KEYS, 'keys-same-id', kk[:11] + b'\x02' + kk[12:] * 2)]
    ke = raw(K_KEY, {'sess': 5, 'seq': 1, 'hash': 0x7DD79D10, 'delta': -1, 'flags': KEY_PLACE,
                     'xyz': [f32(-10.25), f32(100.0), f32(-21.5)]})
    bad += [(K_KEY, 'key-version', b'\x02' + ke[1:]), (K_KEY, 'key-none', ke[:13] + b'\x00' + ke[14:]),
            (K_KEY, 'key-sixteen', ke[:13] + b'\x10' + ke[14:]), (K_KEY, 'key-minus-sixteen', ke[:13] + b'\xf0' + ke[14:]),
            (K_KEY, 'key-sess-zero', ke[:1] + b'\x00' * 4 + ke[5:]), (K_KEY, 'key-seq-zero', ke[:5] + b'\x00' * 4 + ke[9:]),
            (K_KEY, 'key-hash-zero', ke[:9] + b'\x00' * 4 + ke[13:]),
            (K_KEY, 'key-flag-unknown', ke[:14] + b'\x03' + ke[15:]),
            (K_KEY, 'key-place-without-flag', ke[:14] + b'\x00' + ke[15:]),
            (K_KEY, 'key-place-nan', ke[:15] + struct.pack('<f', float('nan')) + ke[19:]),
            (K_KEY, 'key-place-far', ke[:23] + struct.pack('<f', 2e6) + ke[27:]),
            (K_KEY, 'key-short-place', ke[:23])]
    return good, bad


def vector_line(kind, name, m):
    b = PACKERS[kind](m)
    h = b.hex()
    if kind in (K_KILL, K_DAMAGE):
        f = [m['epoch'], m['eseq'], tx(m['area']), m['inst'], m['hash']] + ([m['raw']] if kind == K_DAMAGE else []) + \
            [m['flags']]
    elif kind == K_CUTSCENE:
        f = [m['op'], m['flags'], m['epoch'], m['seq'], tx(m['event']), tx(m['area'])] + [fl(x) for x in m['xyz']]
    elif kind == K_TIME:
        f = [m['epoch'], m['seq'], m['order'], m['tod'], m['flags']]
    elif kind == K_ERA:
        f = [m['epoch'], m['seq'], m['order'], m['flags'], tx(m['scene']), tx(m['scene_area'])] + \
            [fl(x) for x in m['scene_xyz']] + [tx(m['area'])] + [fl(x) for x in m['xyz']]
    elif kind == K_PUPPET:
        f = [m['pseq'], m['t_ms'], m['flags'], tx(m['area'])] + [fl(x) for x in m['pos']] + \
            [fl(m['yaw']), m['upper'], m['gear_sig'], len(m['slots'])]
        for node, frame in m['slots']:
            f += [tx(node), fl(frame)]
    elif kind == K_GEAR:
        f = [gear_sig(m['dyn'], m['stat'])]
        f += [f'd{j}:{st}:{tx(n)}' for j, (n, st) in enumerate(m['dyn']) if n]
        f += [f's{s}:{tx(n)}' for s, n in enumerate(m['stat']) if n]
    elif kind == K_PICK:
        f = [m]
    elif kind == K_KEYS:
        f = [m['serial'], len(m['d'])] + [x for e in m['d'] for x in e] + [len(m['acks'])] + [x for e in m['acks'] for x in e]
    elif kind == K_KEY:
        f = [m['sess'], m['seq'], m['hash'], m['delta'], m['flags']] + [fl(x) for x in m['xyz']]
    else:
        f = [len(m)] + [x for e in m for x in e]
    return ' '.join([KIND_NAMES[kind].lower(), name, h] + [str(x) for x in f])


def write_vectors(path):
    good, bad = vector_set()
    lines = [vector_line(k, n, m) for k, n, m in good]
    for kind, name, b in bad:
        if parses(kind, b):
            sys.exit(f'peer: the reject vector {name} parses')
        lines.append(f'reject {KIND_NAMES[kind].lower()} {name} {b.hex()}')
    Path(path).write_text(VECTOR_HEADER + '\n'.join(lines) + '\n', newline='\n')
    print(f'peer: {len(good)} messages and {len(bad)} refusals written to {path}')


def read_vectors(path):
    """[(kind, name, bytes, fields)] and [(kind, name, bytes)] of the refusals."""
    by_name = {v.lower(): k for k, v in KIND_NAMES.items()}
    good, bad = [], []
    for line in Path(path).read_text().splitlines():
        t = line.split()
        if not t or t[0].startswith('#'):
            continue
        if t[0] == 'reject':
            bad.append((by_name[t[1]], t[2], bytes.fromhex(t[3])))
        else:
            good.append((by_name[t[0]], t[1], bytes.fromhex(t[2]), t[3:]))
    return good, bad


def check_vectors(path):
    """This codec against the file: each message parses, packs to the same bytes and writes the same line; each
    refusal and each shorter or longer copy of a message is refused; random changes that parse pack back as they were."""
    good, bad = read_vectors(path)
    want = {(k, n): m for k, n, m in vector_set()[0]}
    fails = 0
    rng = random.Random(1001)
    for kind, name, b, fields in good:
        try:
            m = PARSERS[kind](b)
            if PACKERS[kind](m) != b:
                raise Bad()
            if (kind, name) in want and vector_line(kind, name, want[(kind, name)]).split()[2:] != [b.hex()] + fields:
                print(f'peer: FAIL vector {name}: the fields differ from the written ones')
                fails += 1
        except (Bad, struct.error):
            print(f'peer: FAIL vector {name} does not parse and pack back')
            fails += 1
            continue
        for cut in range(len(b)):
            if parses(kind, b[:cut]):
                print(f'peer: FAIL vector {name} cut to {cut} bytes parses')
                fails += 1
        if parses(kind, b + b'\x00'):
            print(f'peer: FAIL vector {name} with one more byte parses')
            fails += 1
        for _ in range(300):
            c = bytearray(b)
            for _ in range(rng.randint(1, 3)):
                c[rng.randrange(len(c))] = rng.randrange(256)
            try:
                if PACKERS[kind](PARSERS[kind](bytes(c))) != bytes(c):
                    print(f'peer: FAIL a changed copy of {name} parses and packs to other bytes')
                    fails += 1
            except (Bad, struct.error):
                pass
    for kind, name, b in bad:
        if parses(kind, b):
            print(f'peer: FAIL refusal {name} parses')
            fails += 1
    print(f'peer: {len(good)} messages and {len(bad)} refusals checked: {"FAILED" if fails else "ok"}')
    return 1 if fails else 0


# ---- the session

def pose_bytes(number, pos, scene):
    s = scene.encode()
    return struct.pack('<I3f3ffBB', number, *pos, 0.0, 0.0, 0.0, 0.0, 0, len(s)) + s


def parse_at(text):
    """'<what>@<seconds>' -> (what, seconds)."""
    what, _, at = text.rpartition('@')
    return what, float(at)


def describe(kind, body):
    try:
        m = PARSERS[kind](body)
    except (Bad, struct.error):
        return f'{KIND_NAMES.get(kind, hex(kind))} of {len(body)} bytes that does not parse'
    if kind in (K_KILL, K_DAMAGE):
        return f"{KIND_NAMES[kind]} in {m['area'].decode()} of enemy {m['inst']:#x}" + (f" raw {m['raw']}" if m['raw'] else '')
    if kind == K_CUTSCENE:
        return f"CUTSCENE {'START' if m['op'] == 1 else 'END'} {m['event'].decode()} in {m['area'].decode() or '-'}"
    if kind == K_TIME:
        return f"TIME {m['tod'] // 3600:02d}:{m['tod'] // 60 % 60:02d} order {m['order']}{' jump' if m['flags'] else ''}"
    if kind == K_ERA:
        return f"ERA {'adult' if m['flags'] & 1 else 'child'} order {m['order']}{' change' if m['flags'] & 2 else ''}"
    if kind == K_PUPPET:
        return f"PUPPET {m['pseq']} in {m['area'].decode() or '-'} at {m['pos'][0]:.1f} {m['pos'][1]:.1f} {m['pos'][2]:.1f}"
    if kind == K_GEAR:
        return 'GEAR ' + ', '.join(n.decode() for n, _ in m['dyn'] if n) + ' | ' + ', '.join(n.decode() for n in m['stat'] if n)
    if kind == K_PICK:
        return 'PICK ' + (COLOUR_NAMES[m] if m < 4 else 'auto')
    if kind == K_KEYS:
        return (f"KEYS serial {m['serial']}: " + (', '.join(f'{h:08x}={k}' for h, k in m['d']) or 'no counts') +
                f"; {len(m['acks'])} acks")
    if kind == K_KEY:
        return (f"KEY {m['delta']:+d} for {m['hash']:08x} (seq {m['seq']})" +
                (' at %.1f %.1f %.1f' % tuple(m['xyz']) if m['flags'] & KEY_PLACE else ''))
    return 'COLOURS ' + ', '.join(f'{COLOUR_NAMES[c]}' for _, c in m)


def refused_from(host, seconds=6.0):
    """From another port: join the game's room and say HELLO with an older build's protocol 1; the reason it is
    refused, 0 when it was let in, None when the game did not answer."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(('127.0.0.1', 0))
    s.settimeout(0.05)
    end, last = time.monotonic() + seconds, 0.0
    in_lobby = False
    try:
        while time.monotonic() < end:
            now = time.monotonic()
            if now - last >= 1.0:
                s.sendto(bytes([L_JOIN]), host)
                if in_lobby:
                    s.sendto(bytes([L_DATA, K_HELLO]) + struct.pack('<HI', 1, GAME_BUILD), host)
                last = now
            try:
                d, who = s.recvfrom(2048)
            except (socket.timeout, ConnectionResetError):
                continue
            if who != host or not d:
                continue
            if d[0] == L_MEMBERS and not in_lobby:
                in_lobby, last = True, 0.0
            elif d[0] == L_DATA and len(d) >= 3 and d[1] == K_REFUSED:
                return d[2]
            elif d[0] == L_DATA and len(d) >= 2 and d[1] == K_WELCOME:
                return 0
    finally:
        s.sendto(bytes([L_LEAVE]), host)
        s.close()
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--host', type=int, help="the game's NX2NAVI_COOP_LOCAL port")
    ap.add_argument('--port', type=int, default=27602)
    ap.add_argument('--seconds', type=float, default=120)
    ap.add_argument('--protocol', type=int, default=PROTOCOL)
    ap.add_argument('--expect-refused', action='store_true')
    ap.add_argument('--separation', action='store_true')
    ap.add_argument('--flags', default='')
    ap.add_argument('--flags-at', action='append', default=[])
    ap.add_argument('--echo', action='store_true')
    ap.add_argument('--vectors', type=Path, default=VECTORS)
    ap.add_argument('--mirror', nargs='?', const=2.0, type=float, default=None)
    ap.add_argument('--pick', default=None)
    ap.add_argument('--pick-at', default=None)
    ap.add_argument('--kill-after-damage', action='store_true')
    ap.add_argument('--cut', default=None)
    ap.add_argument('--cut-movie', action='store_true')
    ap.add_argument('--time', action='append', default=[])
    ap.add_argument('--damage-flood', default=None)
    ap.add_argument('--era', default=None)
    ap.add_argument('--key', action='append', default=[])
    ap.add_argument('--write-vectors', type=Path, default=None)
    ap.add_argument('--check-vectors', type=Path, default=None)
    a = ap.parse_args()
    sys.stdout.reconfigure(line_buffering=True)
    if a.write_vectors:
        write_vectors(a.write_vectors)
        return 0
    if a.check_vectors:
        return check_vectors(a.check_vectors)
    if not a.host:
        ap.error('--host is needed for a session')

    def colour_of(name):
        return COLOUR_NONE if name == 'auto' else COLOUR_NAMES.index(name)
    flags = [int(h, 16) for h in a.flags.split(',') if h]
    host = ('127.0.0.1', a.host)
    fails = []
    if a.separation:
        why = refused_from(host)
        print(f'peer: a HELLO with protocol 1 from a second port: {"refused, reason " + str(why) if why else why}')
        if why != 1:
            fails.append('a game of an older protocol was not refused for its version')
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(('127.0.0.1', a.port))
    sock.settimeout(0.01)

    def send(kind, body=b''):
        sock.sendto(bytes([L_DATA, kind]) + body, host)

    echo_good, echo_bad = read_vectors(a.vectors) if a.echo else ([], [])
    echo_queue = [(k, n, b) for k, n, b, _ in echo_good] + [(k, n, b) for k, n, b in echo_bad]
    echo_wait = {}            # (kind, bytes) -> name, of what must come back
    echo_sent = set()         # (kind, bytes) of every message the echo sent: one coming back is the echo's
    echo_back = echo_stray = echo_own = 0
    flood = {'sent': 0, 'back': 0, 'at': None, 'done': False, 'queue_done': None}
    flood_msg = pick_pack(1)
    start = time.monotonic()
    in_lobby = welcomed = refused = False
    last_join = last_hello = last_pose = last_ask = last_echo = 0.0
    number = 0
    host_pose = None          # (scene, x, y, z) as the game last said
    world_at = None           # when the game's Link was first seen in the world
    schema = None
    got_pairs = 0
    chatted = flags_sent = picked = False
    roster = 0
    timed = []                # (seconds after world_at, what, done)
    if a.pick_at:
        s, _, c = a.pick_at.partition(':')
        timed.append([float(s), ('pick', colour_of(c)), False])
    if a.cut:
        what, at = parse_at(a.cut)
        ev, area, xyz = what.split(':', 2)
        timed.append([at, ('cut', ev.encode(), area.encode(), [f32(float(v)) for v in xyz.split(',')]), False])
    for spec in a.time:
        what, at = parse_at(spec)
        hhmm, _, order = what.partition('/')
        hh, mm = hhmm.split(':')
        timed.append([at, ('time', int(hh) * 3600 + int(mm) * 60, int(order, 0) if order else None), False])
    if a.damage_flood:
        what, at = parse_at(a.damage_flood)
        timed.append([at, ('flood', int(what)), False])
    for spec in a.flags_at:
        s, _, hashes = spec.partition(':')
        timed.append([float(s), ('flags', [int(h, 16) for h in hashes.split(',') if h]), False])
    for spec in a.key:
        what, at = parse_at(spec)
        h, _, rest = what.partition(':')
        delta, _, xyz = rest.partition(':')
        timed.append([at, ('key', int(h, 16), int(delta), [f32(float(v)) for v in xyz.split(',')] if xyz else None), False])
    if a.era:
        what, at = parse_at(a.era)
        era, _, place = what.partition(':')
        area, _, xyz = place.partition(':')
        timed.append([at, ('era', era == 'adult', area.encode(), [f32(float(v)) for v in xyz.split(',')] if xyz else [0.0] * 3),
                      False])
    seen = {}                 # kind -> count received
    key_sess = random.getrandbits(32) | 1
    key_seq = key_acked = 0   # the last KEY sent, and the last the host's KEYS acked
    orders = {'time': 0, 'era': 0}
    mirrored = kills = 0
    epoch = random.getrandbits(32) | 1
    seq = 0
    pseq = 0
    while time.monotonic() - start < a.seconds:
        now = time.monotonic()
        if now - last_join >= 1.0:
            sock.sendto(bytes([L_JOIN]), host)
            last_join = now
        if in_lobby and not welcomed and now - last_hello >= 1.0:
            send(K_HELLO, struct.pack('<HI', a.protocol, GAME_BUILD))
            last_hello = now
        if welcomed and now - last_pose >= 0.05:
            number += 1
            if host_pose and host_pose[0]:
                send(K_POSE, pose_bytes(number, (host_pose[1] + 2.0, host_pose[2], host_pose[3] + 2.0), host_pose[0]))
            else:
                send(K_POSE, pose_bytes(number, (0.0, 0.0, 0.0), ''))
            last_pose = now
        if welcomed and world_at and not chatted:
            send(K_CHAT, 'hello from the test peer'.encode())
            chatted = True
            print('peer: sent a chat line')
        if welcomed and schema is not None and now - last_ask >= 5.0:
            send(K_ASK, struct.pack('<I', schema))
            last_ask = now
        if welcomed and schema is not None and flags and not flags_sent:
            send(K_FLAGS, struct.pack('<I', schema) + b''.join(struct.pack('<II', h, 1) for h in flags))
            flags_sent = True
            print(f'peer: sent {len(flags)} flags as set: {", ".join(f"{h:08x}" for h in flags)}')
        if welcomed and a.pick and not picked:
            send(K_PICK, pick_pack(colour_of(a.pick)))
            picked = True
            print(f'peer: PICK {a.pick}')
        if world_at:
            for t in timed:
                if t[2] or now - world_at < t[0]:
                    continue
                t[2] = True
                what = t[1]
                seq += 1
                if what[0] == 'pick':
                    send(K_PICK, pick_pack(what[1]))
                    print(f'peer: PICK {COLOUR_NAMES[what[1]] if what[1] < 4 else "auto"}')
                elif what[0] == 'flags':
                    if schema is None:   # the game does not share progress (yet): tried again at the next pass
                        t[2] = False
                        continue
                    send(K_FLAGS, struct.pack('<I', schema) + b''.join(struct.pack('<II', h, 1) for h in what[1]))
                    print(f'peer: sent {len(what[1])} flags as set: {", ".join(f"{h:08x}" for h in what[1])}')
                elif what[0] == 'cut':
                    send(K_CUTSCENE, cut_pack({'op': 1, 'flags': 2 if a.cut_movie else 0, 'epoch': epoch, 'seq': seq,
                                               'event': what[1], 'area': what[2], 'xyz': what[3]}))
                    print(f'peer: CUTSCENE START {what[1].decode()} in {what[2].decode()}' + (', MOVIE bit set' if a.cut_movie else ''))
                elif what[0] == 'time':
                    order = what[2] if what[2] is not None else orders['time'] + 1
                    if what[2] is None:
                        orders['time'] = order
                    send(K_TIME, time_pack({'epoch': epoch, 'seq': seq, 'order': order & 0xFFFFFFFF, 'tod': what[1], 'flags': 1}))
                    print(f'peer: TIME jump to {what[1] // 3600:02d}:{what[1] // 60 % 60:02d}, order {order & 0xFFFFFFFF}')
                elif what[0] == 'key':
                    key_seq += 1
                    place = what[3]
                    send(K_KEY, key_pack({'sess': key_sess, 'seq': key_seq, 'hash': what[1], 'delta': what[2],
                                          'flags': KEY_PLACE if place else 0, 'xyz': place or [0.0] * 3}))
                    print(f'peer: KEY {what[2]:+d} for {what[1]:08x} (seq {key_seq})' +
                          (' at %.1f %.1f %.1f' % tuple(place) if place else ''))
                elif what[0] == 'flood':
                    area = (host_pose[0] if host_pose else '').encode()
                    for i in range(what[1]):
                        seq += 1
                        send(K_DAMAGE, kill_pack(K_DAMAGE, {'epoch': epoch, 'eseq': seq, 'area': area, 'inst': 0x5EED0000 + i,
                                                            'hash': kill_name_hash(b'Enemy_Made_Up'), 'raw': 1, 'flags': 0}))
                    print(f'peer: {what[1]} DAMAGE for made-up enemies of {area.decode()} sent at once')
                else:
                    orders['era'] += 1
                    if what[2]:
                        area, xyz = what[2], what[3]
                    else:
                        place = host_pose or ('', 0.0, 0.0, 0.0)
                        area, xyz = place[0].encode(), [f32(v) for v in place[1:]]
                    send(K_ERA, era_pack({'epoch': epoch, 'seq': seq, 'order': orders['era'], 'flags': 2 | (1 if what[1] else 0),
                                          'scene': b'', 'scene_area': b'', 'scene_xyz': [0.0] * 3,
                                          'area': area, 'xyz': xyz}))
                    print(f'peer: ERA change to {"adult" if what[1] else "child"} at {area.decode() or "(nowhere)"}, '
                          f'order {orders["era"]}')
        if a.echo and welcomed and world_at and now - world_at >= 2.0 and now - last_echo >= 0.25:
            last_echo = now
            if echo_queue:
                kind, name, b = echo_queue.pop(0)
                if parses(kind, b):
                    echo_wait[(kind, b)] = name
                echo_sent.add((kind, b))
                send(kind, b)
                if not echo_queue:
                    flood['queue_done'] = now
            elif flood['at'] is None and now - flood['queue_done'] >= 1.5:
                for _ in range(40):   # PICK's rate is 4 a second, 8 at once
                    send(K_PICK, flood_msg)
                flood['sent'], flood['at'] = 40, now
                print('peer: echo: sent 40 PICK at once')
            elif flood['at'] is not None and not flood['done'] and now - flood['at'] >= 2.0:
                flood['done'] = True
                print(f'peer: echo: {flood["back"]} of the 40 PICK came back')
        try:
            d, who = sock.recvfrom(2048)
        except (socket.timeout, ConnectionResetError):   # nothing yet, or the game's port is not open yet
            continue
        if who != host or not d:
            continue
        if d[0] == L_MEMBERS:
            if not in_lobby:
                print(f'peer: in the lobby with {(len(d) - 1) // 8} members')
            in_lobby = True
            continue
        if d[0] != L_DATA or len(d) < 2:
            continue
        kind, b = d[1], d[2:]
        if kind == K_WELCOME:
            welcomed = True
            print('peer: the host let it in')
        elif kind == K_REFUSED:
            print(f'peer: REFUSED, reason {b[0] if b else "?"}')
            refused = b[0] if b else -1
            break
        elif kind == K_ROSTER and b:
            if b[0] != roster:
                roster = b[0]
                print(f'peer: the host lists {roster} players')
        elif kind == K_POSE and len(b) >= 34:
            n, x, y, z, vx, vy, vz, yaw, fl_, sl = struct.unpack_from('<I3f3ffBB', b)
            scene = b[34:34 + sl].decode(errors='replace')
            if host_pose is None or host_pose[0] != scene:
                print(f'peer: the game\'s Link: {scene or "(not in the world)"} at {x:.1f} {y:.1f} {z:.1f}, yaw {yaw:.2f}')
            host_pose = (scene, x, y, z)
            if scene and world_at is None:
                world_at = now
        elif kind == K_CHAT:
            print(f'peer: chat from the game: {b.decode(errors="replace")}')
        elif kind == K_FLAGS and len(b) >= 4:
            if schema is None:
                schema = struct.unpack_from('<I', b)[0]
                print('peer: the game shares progress')
            got_pairs += (len(b) - 4) // 8
        elif kind == K_ASK and len(b) == 4:
            if schema is None:
                schema = struct.unpack_from('<I', b)[0]
                print('peer: the game asked for the room\'s progress')
        elif kind >= 0x40:
            seen[kind] = seen.get(kind, 0) + 1
            if a.echo and (kind, b) in echo_wait:
                del echo_wait[(kind, b)]
                echo_back += 1
                continue
            if a.echo and kind == K_PICK and b == flood_msg and flood['at'] is not None:
                flood['back'] += 1
                continue
            if a.echo:
                if (kind, b) in echo_sent:   # a refused one, or a valid one a second time
                    echo_stray += 1
                    print(f'peer: echo: a {describe(kind, b)} came back that was not waited for')
                else:   # the game's modules still send their own messages (its colours, the time of day, ...)
                    echo_own += 1
                continue
            if kind != K_PUPPET or seen[kind] in (1, 10) or seen[kind] % 100 == 0:
                print(f'peer: from the game: {describe(kind, b)}' + (f' (#{seen[kind]})' if kind == K_PUPPET else ''))
            if a.mirror is not None and kind in (K_PUPPET, K_GEAR) and parses(kind, b):
                if kind == K_PUPPET:
                    m = puppet_parse(b)
                    pseq += 1
                    m['pseq'], m['pos'] = pseq, [f32(m['pos'][0] + a.mirror), m['pos'][1], m['pos'][2]]
                    send(K_PUPPET, puppet_pack(m))
                else:
                    send(K_GEAR, b)
                mirrored += 1
            if a.kill_after_damage and kind == K_DAMAGE and parses(kind, b):
                m = kill_parse(K_DAMAGE, b)
                seq += 1
                send(K_KILL, kill_pack(K_KILL, dict(m, epoch=epoch, eseq=seq, raw=0)))
                kills += 1
                print(f"peer: KILL sent for enemy {m['inst']:#x}")
            if kind == K_TIME and parses(kind, b):
                orders['time'] = max(orders['time'], time_parse(b)['order'])
            if kind == K_ERA and parses(kind, b):
                orders['era'] = max(orders['era'], era_parse(b)['order'])
            if kind == K_KEYS and parses(kind, b):
                for _, ss, q in keys_parse(b)['acks']:
                    if ss == key_sess and q != key_acked:
                        key_acked = q
                        print(f'peer: the host took the KEY changes up to seq {q}')
    sock.sendto(bytes([L_LEAVE]), host)
    seen_link = bool(world_at)
    print(f'peer: welcomed {welcomed}, saw the game\'s Link in the world {seen_link}, got {got_pairs} values of its '
          f'progress; module messages received: ' + (', '.join(f'{KIND_NAMES.get(k, hex(k))} {n}' for k, n in sorted(seen.items()))
                                                   or 'none'))
    if a.expect_refused:
        if refused != 1:
            fails.append(f'not refused for its version ({refused})')
    else:
        if refused is not False:
            fails.append('refused')
        if not (welcomed and seen_link and got_pairs > 0):
            fails.append('not let in, the game\'s Link not seen or none of its progress')
    if a.echo:
        n_good = len(echo_good)
        print(f'peer: echo: {echo_back} of {n_good} messages came back as they were sent, {len(echo_wait)} did not, '
              f'{echo_stray} came back that should not have; {echo_own} of the game\'s own were not counted')
        if echo_wait:
            print('peer: echo: missing: ' + ', '.join(sorted(echo_wait.values())))
        if echo_back != n_good or echo_stray:
            fails.append('the echo')
        if not flood['done'] or not 1 <= flood['back'] <= 8 + 4 * 2 + 1:
            fails.append(f'the flood ({flood["back"]} of 40 came back)')
    if a.mirror is not None and not mirrored:
        fails.append('nothing mirrored')
    if a.kill_after_damage and not kills:
        fails.append('no DAMAGE to answer')
    for t in timed:
        if not t[2]:
            fails.append(f'{t[1][0]} not sent')
    if key_seq and key_acked != key_seq:
        fails.append(f'the host took the KEY changes up to seq {key_acked} of {key_seq}')
    if fails:
        print('peer: FAILED: ' + '; '.join(fails))
        return 1
    print('peer: ok')
    return 0


if __name__ == '__main__':
    sys.exit(main())
