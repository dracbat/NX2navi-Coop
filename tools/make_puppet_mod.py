"""The co-op puppets' files, made from the player's own game files: the actor runtime/coop_puppet.c shows the other
players of a co-op room as, written as an ordinary mod folder. tools/make_game.py runs it once the RomFS image is
packed (its coop step); nothing of the game is in this script.

    python tools/make_puppet_mod.py --game FOLDER [--out FOLDER] [--romfs FOLDER]

It reads <game>/romfs (romfs.bin and index.json, as tools/make_game.py writes them; --romfs names another) and writes
<out>/romfs/, by default <game>/mods/Co-op puppets/romfs/:
  Pack/Actor/CoopPuppet_Child.pack.zs, Pack/Actor/CoopPuppet_Adult.pack.zs
      the pause menu's Link (PouchPlayer_<age>) with an ActorParam under the puppet's name whose ASRef and ASInfoRef
      point at Link's in-world animation set and its settings (copied in from Player_<age>) and whose AIInfoRef is
      blank; zstd with the dictionary the game's own pack uses
  RSDB/ActorInfo.Product.111.rstbl.byml.zs, RSDB/GameActorInfo.Product.111.rstbl.byml.zs
      the game's tables with a row CoopPuppet_<age> copied from PouchPlayer_<age> (ActorInfo: Link's own instance
      heap size), in the tables' row order
  System/Resource/ResourceSizeTable.Product.111.Nin_Ounce_NVN2.rsizetable.zs
      a small table the runtime merges into the game's (romfs.c restbl_collect, restbl.c restbl_merge): the sizes of
      the two new ActorParam files inside the packs, which the runtime's own estimate does not cover

The same game files give the same output, byte for byte. It is made from the game, so it stays in the game folder
like the rest of the build and is never shared: a folder inside this repository is refused unless it is under build/
or generated/. --variant (tests) also makes <PREFIX>_<age> actors with more of Link's components.

Needs Python 3.14 (compression.zstd) or the zstandard package.
"""
import argparse
import bisect
import json
import os
import struct
import sys
import zlib
from pathlib import Path

KIT = Path(__file__).resolve().parents[1]
MOD_NAME = 'Co-op puppets'   # tools/make_game.py COOP_MOD, launcher/mods.py COOP_PUPPETS
AGES = ('Child', 'Adult')
PREFIX = 'CoopPuppet'        # the actor names runtime/coop_puppet.c asks for: CoopPuppet_<age>
DICTIONARIES = 'Pack/ZsDic.pack.zs'
RSDB = ('RSDB/ActorInfo.Product.111.rstbl.byml.zs', 'RSDB/GameActorInfo.Product.111.rstbl.byml.zs')
REST = 'System/Resource/ResourceSizeTable.Product.111.Nin_Ounce_NVN2.rsizetable.zs'
NUL = bytes([0])


class PuppetError(Exception):
    pass


# ---------------------------------------------------------------- zstd

def _zstd():
    try:
        from compression import zstd   # Python 3.14
        return 'stdlib', zstd
    except ImportError:
        import zstandard
        return 'zstandard', zstandard


def frame_dict_id(data):
    """The dictionary id a zstd frame names (0: none)."""
    kind, z = _zstd()
    return z.get_frame_info(data).dictionary_id if kind == 'stdlib' else z.get_frame_parameters(data).dict_id


def decompress(data, dict_bytes=None):
    kind, z = _zstd()
    if kind == 'stdlib':
        return z.decompress(data, zstd_dict=z.ZstdDict(dict_bytes) if dict_bytes else None)
    d = z.ZstdCompressionDict(dict_bytes) if dict_bytes else None
    return z.ZstdDecompressor(dict_data=d).decompressobj().decompress(data)


def compress(data, dict_bytes=None):
    """zstd at level 19, with the dictionary when one is given (the frame names it, as the game's files do)."""
    kind, z = _zstd()
    if kind == 'stdlib':
        return z.compress(data, level=19, zstd_dict=z.ZstdDict(dict_bytes) if dict_bytes else None)
    d = z.ZstdCompressionDict(dict_bytes) if dict_bytes else None
    return z.ZstdCompressor(level=19, dict_data=d).compress(data)


# ---------------------------------------------------------------- the RomFS image

class Romfs:
    """The game's files in the RomFS image tools/make_game.py packs: romfs.bin, and index.json with each file's offset
    and size. read() gives a file as stored, read_zs() a .zs file decompressed with the dictionary its frame names
    (Pack/ZsDic.pack.zs, a SARC of the dictionaries, compressed without one)."""

    def __init__(self, folder):
        self.folder = Path(folder)
        index = self.folder / 'index.json'
        if not index.is_file() or not (self.folder / 'romfs.bin').is_file():
            raise PuppetError(f'{self.folder} holds no RomFS image (romfs.bin and index.json): build the game first')
        self.files = {e['path']: (e['offset'], e['size'])
                      for e in json.loads(index.read_text(encoding='utf-8'))['files']}
        self._dicts = None

    def read(self, rel):
        if rel not in self.files:
            raise PuppetError(f'the game files lack {rel}')
        off, size = self.files[rel]
        with open(self.folder / 'romfs.bin', 'rb') as f:
            f.seek(off)
            data = f.read(size)
        if len(data) != size:
            raise PuppetError(f'romfs.bin ends inside {rel}')
        return data

    def dicts(self):
        """{dictionary id: dictionary}"""
        if self._dicts is None:
            self._dicts = {}
            for name, d in sarc_members(decompress(self.read(DICTIONARIES))).items():
                if name.endswith('.zsdic') and len(d) >= 8 and struct.unpack_from('<I', d, 0)[0] == 0xEC30A437:
                    self._dicts[struct.unpack_from('<I', d, 4)[0]] = d
        return self._dicts

    def read_zs(self, rel):
        data = self.read(rel)
        did = frame_dict_id(data)
        if did and did not in self.dicts():
            raise PuppetError(f'{rel} names zstd dictionary {did}, which the game files lack')
        return decompress(data, self.dicts()[did] if did else None)

    def compress_like(self, data, original):
        """data compressed as the game compressed original: with the same dictionary, or none."""
        did = frame_dict_id(original)
        d = self.dicts()[did] if did else None
        blob = compress(data, d)
        if decompress(blob, d) != data:
            raise PuppetError('zstd round trip')
        return blob


# ---------------------------------------------------------------- SARC

def sarc_members(data):
    """{name: bytes} of a SARC archive (little-endian, as the game's are)."""
    if len(data) < 0x14 or data[:4] != b'SARC':
        raise PuppetError('not a SARC archive')
    hl = struct.unpack_from('<H', data, 4)[0]
    base = struct.unpack_from('<I', data, 0xC)[0]
    count = struct.unpack_from('<H', data, hl + 6)[0]
    ents = hl + 0xC
    names = ents + 16 * count + 8
    if data[hl:hl + 4] != b'SFAT' or data[ents + 16 * count:ents + 16 * count + 4] != b'SFNT':
        raise PuppetError('a SARC archive without its file table')
    out = {}
    for i in range(count):
        _h, attr, start, end = struct.unpack_from('<IIII', data, ents + 16 * i)
        at = names + (attr & 0xFFFF) * 4
        name = data[at:data.index(b'\0', at)].decode('utf-8')
        if not start <= end or base + end > len(data):
            raise PuppetError(f'SARC member {name} lies outside the archive')
        out[name] = data[base + start:base + end]
    return out


def sarc_hash(name, key=0x65):
    h = 0
    for c in name.encode():
        h = (h * key + c) & 0xffffffff
    return h


def sarc_build(members, align=8):
    """A SARC of members ({name: bytes}): entries in the order of their names' hashes, as the game's archives are,
    the data from the next 0x100 after the tables, each file aligned to align."""
    names = sorted(members, key=sarc_hash)
    fnt = bytearray()
    offs = {}
    for n in names:
        offs[n] = len(fnt)
        fnt += n.encode() + NUL
        fnt += NUL * (-len(fnt) % 4)
    sfat = bytearray(b'SFAT' + struct.pack('<HHI', 0xC, len(names), 0x65))
    header_len = 0x14 + len(sfat) + 16 * len(names) + 8 + len(fnt)
    data_off = (header_len + 0xff) & ~0xff
    blob = bytearray()
    for n in names:
        blob += NUL * (-len(blob) % align)
        s = len(blob)
        blob += members[n]
        sfat += struct.pack('<IIII', sarc_hash(n), 0x01000000 | (offs[n] // 4), s, len(blob))
    sfnt = b'SFNT' + struct.pack('<HH', 8, 0) + bytes(fnt)
    body = bytes(sfat) + sfnt
    total = data_off + len(blob)
    head = b'SARC' + struct.pack('<HHIIHH', 0x14, 0xFEFF, total, data_off, 0x100, 0)
    out = head + body
    return out + NUL * (data_off - len(out)) + bytes(blob)


# ---------------------------------------------------------------- BYML (the types the actor and RSDB files use)

class Float(float):
    """A BYML float (0xD2): written back as one, also when its value is whole."""


class UInt(int):
    """A BYML unsigned 32-bit integer (0xD3)."""


def byml_load(data):
    """(version, document) of a little-endian BYML file, with the exact types kept: Float for 0xD2, UInt for 0xD3,
    bool, int, str, list and dict. Other node types are refused: these files have none."""
    if data[:2] != b'YB':
        raise PuppetError('not a little-endian BYML file')
    version, koff, soff, root = struct.unpack_from('<HIII', data, 2)

    def u32(o):
        return struct.unpack_from('<I', data, o)[0]

    def strtab(off):
        if not off:
            return []
        out = []
        for i in range(u32(off) >> 8):
            s = off + u32(off + 4 + 4 * i)
            out.append(data[s:data.index(b'\0', s)].decode('utf-8'))
        return out
    keys, strs = strtab(koff), strtab(soff)

    def val(t, v):
        if t == 0xA0:
            return strs[v]
        if t in (0xC0, 0xC1):
            return node(v)
        if t == 0xD0:
            return bool(v)
        if t == 0xD1:
            return struct.unpack('<i', struct.pack('<I', v))[0]
        if t == 0xD2:
            return Float(struct.unpack('<f', struct.pack('<I', v))[0])
        if t == 0xD3:
            return UInt(v)
        raise PuppetError(f'BYML value type {t:#04x}')

    def node(o):
        w = u32(o)
        t, n = w & 0xff, w >> 8
        if t == 0xC0:
            tys = data[o + 4:o + 4 + n]
            vo = o + 4 + ((n + 3) & ~3)
            return [val(tys[i], u32(vo + 4 * i)) for i in range(n)]
        if t == 0xC1:
            out = {}
            for i in range(n):
                w = u32(o + 4 + 8 * i)
                out[keys[w & 0xffffff]] = val(w >> 24, u32(o + 8 + 8 * i))
            return out
        raise PuppetError(f'BYML node type {t:#04x}')
    return version, node(root)


def byml_dump(doc, version=7, dedup=True):
    """A little-endian BYML file of doc: keys and strings sorted by their bytes, containers after their parent, and
    identical containers written once and shared, as the game's files do."""
    keys, strs = set(), set()

    def collect(x):
        if isinstance(x, dict):
            for k, v in x.items():
                keys.add(k)
                collect(v)
        elif isinstance(x, list):
            for v in x:
                collect(v)
        elif isinstance(x, str):
            strs.add(x)
    collect(doc)
    keys = sorted(keys, key=lambda s: s.encode())
    strs = sorted(strs, key=lambda s: s.encode())
    kidx = {k: i for i, k in enumerate(keys)}
    sidx = {s: i for i, s in enumerate(strs)}

    def strtab(items):
        if not items:
            return b''
        enc = [s.encode() + NUL for s in items]
        head = 4 + 4 * (len(items) + 1)
        offs, at = [], head
        for e in enc:
            offs.append(at)
            at += len(e)
        offs.append(at)
        out = struct.pack('<I', 0xC2 | (len(items) << 8)) + b''.join(struct.pack('<I', o) for o in offs) + b''.join(enc)
        return out + NUL * (-len(out) % 4)

    out = bytearray(16)
    ktab, stab = strtab(keys), strtab(strs)
    koff = len(out) if keys else 0
    out += ktab
    soff = len(out) if strs else 0
    out += stab

    def typ(v):
        if isinstance(v, bool):
            return 0xD0
        if isinstance(v, str):
            return 0xA0
        if isinstance(v, list):
            return 0xC0
        if isinstance(v, dict):
            return 0xC1
        if isinstance(v, Float):
            return 0xD2
        if isinstance(v, UInt):
            return 0xD3
        if isinstance(v, int):
            return 0xD1
        if isinstance(v, float):
            return 0xD2
        raise PuppetError(f'no BYML type for {type(v).__name__}')

    def scalar(t, v):
        if t == 0xA0:
            return sidx[v]
        if t == 0xD0:
            return int(v)
        if t == 0xD1:
            return struct.unpack('<I', struct.pack('<i', v))[0]
        if t == 0xD2:
            return struct.unpack('<I', struct.pack('<f', v))[0]
        if t == 0xD3:
            return int(v)
        raise PuppetError(f'BYML scalar type {t:#04x}')

    def canon(x):   # a key equal for identical subtrees (types included), for sharing them
        if isinstance(x, dict):
            return ('d',) + tuple((k, canon(v)) for k, v in sorted(x.items()))
        if isinstance(x, list):
            return ('l',) + tuple(canon(v) for v in x)
        t = typ(x)
        return (t, struct.pack('<f', x)) if t == 0xD2 else (t, x)   # by bits: -0.0 and 0.0 stay apart

    done = {}

    def write(x):   # the node's offset; children after their parent; identical containers written once
        if dedup:
            key = canon(x)
            if key in done:
                return done[key]
            done[key] = len(out)
        at = len(out)
        if isinstance(x, list):
            n = len(x)
            size = 4 + ((n + 3) & ~3) + 4 * n
            out.extend(NUL * size)
            struct.pack_into('<I', out, at, 0xC0 | (n << 8))
            vo = at + 4 + ((n + 3) & ~3)
            for i, v in enumerate(x):
                t = typ(v)
                out[at + 4 + i] = t
                struct.pack_into('<I', out, vo + 4 * i, write(v) if t in (0xC0, 0xC1) else scalar(t, v))
            return at
        items = sorted(x.items(), key=lambda kv: kidx[kv[0]])
        n = len(items)
        out.extend(NUL * (4 + 8 * n))
        struct.pack_into('<I', out, at, 0xC1 | (n << 8))
        for i, (k, v) in enumerate(items):
            t = typ(v)
            struct.pack_into('<II', out, at + 4 + 8 * i, kidx[k] | (t << 24),
                             write(v) if t in (0xC0, 0xC1) else scalar(t, v))
        return at
    root = write(doc)
    out[0:16] = b'YB' + struct.pack('<HIII', version, koff, soff, root)
    return bytes(out)


# ---------------------------------------------------------------- the resource size table (RESTBL)

# The runtime's estimate for a file the game's table lacks (restbl.c): size * factor / 1000 + 8 KiB, in 4 KiB pages.
FACTOR = {'bfres': 1500, 'bkres': 2050, 'bgyml': 3850, 'asb': 1400, 'bstar': 1200, 'bphsc': 1100, 'byml': 1000,
          'pack': 1000, 'ainb': 1000}


def estimate(key, size):
    leaf = key.rsplit('/', 1)[-1]
    f = FACTOR.get(leaf.rsplit('.', 1)[-1], 1500) if '.' in leaf else 1500
    e = (size * f + 999) // 1000 + 8192
    return (e + 4095) & ~4095


def key_of(path):
    """A file's key in the table: its path without .zs or .mc."""
    return path[:-3] if path.endswith(('.zs', '.mc')) else path


class Restbl:
    """A RESTBL: "RESTBL", version, name length, the hashed entries (CRC-32 of the key, size; sorted by hash) and the
    named ones."""

    def __init__(self, d):
        if d[:6] != b'RESTBL':
            raise PuppetError('not a resource size table')
        self.ver, self.nl, nh, nn = struct.unpack_from('<4I', d, 6)
        self.h = [struct.unpack_from('<II', d, 22 + 8 * i) for i in range(nh)]
        self.hs = [x for x, _ in self.h]
        base = 22 + 8 * nh
        self.named = {}
        for i in range(nn):
            e = d[base + i * (self.nl + 4): base + (i + 1) * (self.nl + 4)]
            self.named[e[:self.nl].split(NUL)[0].decode()] = struct.unpack_from('<I', e, self.nl)[0]

    def get(self, key):
        if key in self.named:
            return self.named[key]
        c = zlib.crc32(key.encode()) & 0xffffffff
        i = bisect.bisect_left(self.hs, c)
        return self.h[i][1] if i < len(self.h) and self.hs[i] == c else None


def restbl_bytes(template, entries):
    """A RESTBL of the template's version and name length holding only the hashed entries {crc32: size}."""
    ver, name_len = struct.unpack_from('<II', template, 6)
    items = sorted(entries.items())
    out = bytearray(template[:6] + struct.pack('<4I', ver, name_len, len(items), 0))
    for h, size in items:
        out += struct.pack('<II', h, size)
    return bytes(out)


# ---------------------------------------------------------------- the puppet files

def puppet_files(rom, variants=()):
    """{RomFS path: bytes} of the mod, and {crc32: size} of its size entries. variants: [(prefix, [component, ...])]
    of test actors with these components of Link's ActorParam added."""
    rt = Restbl(rom.read_zs(REST))
    files, rest_entries = {}, {}
    kinds = [(PREFIX, [])] + list(variants)
    for (prefix, extra), age in [(v, g) for v in kinds for g in AGES]:
        src_pack = f'Pack/Actor/PouchPlayer_{age}.pack.zs'
        link_pack = f'Pack/Actor/Player_{age}.pack.zs'
        raw = rom.read(src_pack)
        members = sarc_members(rom.read_zs(src_pack))
        link = sarc_members(rom.read_zs(link_pack))
        new = dict(members)
        # Link's animation settings: the child has its own and the common one, the adult the common one
        asinfos = ['Component/ASInfo/Player.engine__component__ASInfo.bgyml']
        if age == 'Child':
            asinfos.insert(0, 'Component/ASInfo/Player_Child.engine__component__ASInfo.bgyml')
        for m in asinfos:
            new[m] = link[m]
        src_param = f'Actor/PouchPlayer_{age}.engine__actor__ActorParam.bgyml'
        ver, param = byml_load(members[src_param])
        comp = param['Components']
        if comp.get('ASRef') != '?AS/Player_Pouch.root.asb':
            raise PuppetError(f'{src_pack}: the menu Link\'s ASRef is {comp.get("ASRef")!r}, not the one expected')
        comp['ASRef'] = f'?AS/Player_{age}.root.asb'
        comp['ASInfoRef'] = '?' + asinfos[0]
        comp['AIInfoRef'] = ''
        if extra:   # components of Link (Player_Base and Player_<age>) and the files they name, from Link's pack
            _, lb = byml_load(link['Actor/Player_Base.engine__actor__ActorParam.bgyml'])
            _, lc = byml_load(link[f'Actor/Player_{age}.engine__actor__ActorParam.bgyml'])
            lcomp = dict(lb.get('Components', {}))
            lcomp.update(lc.get('Components', {}))
            for cname in extra:
                ref = lcomp[cname]
                comp[cname] = ref
                member = ref.lstrip('?')
                if member in link and member not in new:
                    new[member] = link[member]
        param_name = f'Actor/{prefix}_{age}.engine__actor__ActorParam.bgyml'
        new[param_name] = byml_dump(param, ver)   # the menu Link's own ActorParam stays in the pack, unused
        sarc = sarc_build(new)
        back = sarc_members(sarc)
        if back.keys() != new.keys() or any(back[k] != new[k] for k in new):
            raise PuppetError('SARC round trip')
        _, check = byml_load(back[param_name])
        if check['Components']['ASRef'] != comp['ASRef'] or check['Components']['AIInfoRef'] != '':
            raise PuppetError('BYML round trip of the ActorParam')
        files[f'Pack/Actor/{prefix}_{age}.pack.zs'] = rom.compress_like(sarc, raw)
        # the new member's size entry: the menu Link's own ActorParam entry, or the runtime's estimate if larger
        key = key_of(param_name)
        have = rt.get(key_of(src_param)) or 0
        rest_entries[zlib.crc32(key.encode()) & 0xffffffff] = max(have, estimate(key, len(new[param_name])))

    for table in RSDB:
        raw = rom.read(table)
        ver, rows = byml_load(rom.read_zs(table))
        ids = [r['__RowId'] for r in rows]
        for name, age in [(f'{v[0]}_{g}', g) for v in kinds for g in AGES]:
            if name in ids:
                raise PuppetError(f'{table} already has {name}')
            row = dict(next(r for r in rows if r['__RowId'] == f'PouchPlayer_{age}'))
            row['__RowId'] = name
            if 'InstanceHeapSize' in row and table.startswith('RSDB/ActorInfo.'):
                heap = next(r for r in rows if r['__RowId'] == f'Player_{age}')['InstanceHeapSize']
                if type(row['InstanceHeapSize']) is not type(heap):
                    raise PuppetError(f'{table}: the instance heap sizes differ in type')
                row['InstanceHeapSize'] = heap
            pos = bisect.bisect_left([i.encode() for i in ids], name.encode())
            rows.insert(pos, row)
            ids.insert(pos, name)
        if ids != sorted(ids, key=lambda s: s.encode()):
            raise PuppetError(f'{table}: its rows are not in byte order')
        data = byml_dump(rows, ver)
        if byml_load(data)[1] != rows:
            raise PuppetError(f'BYML round trip of {table}')
        files[table] = rom.compress_like(data, raw)

    template = rom.read_zs(REST)
    rest = restbl_bytes(template, rest_entries)
    parsed = Restbl(rest)
    names = [key_of(f'Actor/{v[0]}_{g}.engine__actor__ActorParam.bgyml') for v in kinds for g in AGES]
    if len(parsed.h) != len(rest_entries) or not all(parsed.get(n) for n in names):
        raise PuppetError('the size table round trip')
    files[REST] = rom.compress_like(rest, rom.read(REST))
    return files, rest_entries


def refused(out):
    """Why out may not be written, or '': a folder of this repository (or kit) outside build/ and generated/."""
    try:
        rel = out.resolve().relative_to(KIT.resolve())
    except ValueError:
        return ''
    if rel.parts[:1] in (('build',), ('generated',)):
        return ''
    return f'{out} is inside {KIT}: files made from the game go to the game folder, or under build/ or generated/'


def write(out, files):
    """Each file whole or not at all (written beside itself, then moved into place)."""
    for rel, blob in files.items():
        p = out / 'romfs' / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        tmp = p.with_name(p.name + '.part')
        tmp.write_bytes(blob)
        os.replace(tmp, p)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--game', type=Path, help='the built game folder (its romfs/ is read)')
    ap.add_argument('--romfs', type=Path, help='the folder with romfs.bin and index.json (default <game>/romfs)')
    ap.add_argument('--out', type=Path, help=f'the mod folder to write (default <game>/mods/{MOD_NAME})')
    ap.add_argument('--variant', action='append', default=[], metavar='PREFIX:Comp1,Comp2',
                    help='tests: also make <PREFIX>_<age> with these components of Link added')
    a = ap.parse_args()
    if not a.game and not (a.romfs and a.out):
        ap.error('give --game, or both --romfs and --out')
    romfs = a.romfs or a.game / 'romfs'
    out = Path(os.path.abspath(a.out or a.game / 'mods' / MOD_NAME))
    variants = []
    for v in a.variant:
        prefix, _, comps = v.partition(':')
        if not prefix or prefix == PREFIX:
            ap.error(f'--variant {v}: give a prefix other than {PREFIX}')
        variants.append((prefix, [c for c in comps.split(',') if c]))
    why = refused(out)
    if why:
        sys.exit(f'make_puppet_mod: {why}')
    try:
        files, rest_entries = puppet_files(Romfs(romfs), variants)
        write(out, files)
    except (PuppetError, OSError, KeyError, ValueError, StopIteration) as e:
        sys.exit(f'make_puppet_mod: {type(e).__name__}: {e}' if not isinstance(e, PuppetError) else f'make_puppet_mod: {e}')
    for rel, blob in files.items():
        print(f'wrote {rel} ({len(blob)} bytes)')
    print('size entries: ' + ', '.join(f'{h:08x}={n}' for h, n in sorted(rest_entries.items())))
    print(f'{MOD_NAME}: {len(files)} files in {out}')


if __name__ == '__main__':
    main()
