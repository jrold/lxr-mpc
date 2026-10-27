#!/usr/bin/env python3
"""The default card VOLTKIT's tracks start with (data/card.vkc).

    voltkit-machine-test --format init.img
    python3 tools/make_card.py init.img data/card.vkc [card.img]

init.img is the card the LXR firmware prepares itself on a blank card (its
format routine: a FAT32 volume, the global settings and the Init project
with its init kit). Two of its global settings (PROJ00/GLO.CFG) are then set
as the plug-in needs them, as a user would on the CONFIG page:
  - the tempo to MID (1): the sequencer follows the MPC's MIDI clock;
  - each voice's MIDI note to 36..42 (the firmware's default puts all seven
    on note 48): the pads play one voice each.
The card is packed in mpc/CardCodec.h's format, VKC2 (and written raw as
card.img for voltkit-machine-test).

Copyright (C) 2026 Jason Rold. GPL-3.0-only.
"""
import struct
import sys

SECTOR = 512
SECTORS = 131072
NOTE1 = 22   # PAR_MIDI_NOTE1 - PAR_BEGINNING_OF_GLOBALS (Parameters.h): its byte in GLO.CFG


def fat32_file(img, path):
    """(first byte offset, size) of a file given as ['DIR', 'NAME.EXT'], from the root."""
    vol = 0
    if img[82:87] != b'FAT32':   # a partition table: the first partition's volume
        vol = struct.unpack_from('<I', img, 0x1C6)[0] * SECTOR
    bps, spc, rsv, nfats = struct.unpack_from('<HBHB', img, vol + 11)
    fat_sectors = struct.unpack_from('<I', img, vol + 36)[0]
    cluster = struct.unpack_from('<I', img, vol + 44)[0]
    data = vol + (rsv + nfats * fat_sectors) * bps
    if bps != SECTOR:
        sys.exit('make_card: not a 512-byte-sector card')

    def next_cluster(c):
        return struct.unpack_from('<I', img, vol + rsv * bps + 4 * c)[0] & 0x0FFFFFFF

    for i, name in enumerate(path):
        want = name.upper()
        found = None
        c = cluster
        while found is None and 2 <= c < 0x0FFFFFF8:
            base = data + (c - 2) * spc * bps
            for e in range(base, base + spc * bps, 32):
                raw = img[e:e + 11]
                if raw[0] == 0:
                    break
                if raw[0] == 0xE5 or img[e + 11] == 0x0F:
                    continue
                stem, ext = raw[:8].decode('ascii', 'replace').strip(), raw[8:].decode('ascii', 'replace').strip()
                if (stem + ('.' + ext if ext else '')) == want:
                    found = e
                    break
            c = next_cluster(c)
        if found is None:
            sys.exit(f'make_card: {"/".join(path[:i + 1])} not on the card')
        first = struct.unpack_from('<H', img, found + 20)[0] << 16 | struct.unpack_from('<H', img, found + 26)[0]
        if i == len(path) - 1:
            return data + (first - 2) * spc * bps, struct.unpack_from('<I', img, found + 28)[0]
        cluster = first


def pack_sector(d):
    out = bytearray()
    i = 0
    while i < SECTOR:
        if d[i]:
            out.append(d[i])
            i += 1
            continue
        run = 0
        while i < SECTOR and d[i] == 0 and run < 255:
            i += 1
            run += 1
        out += bytes((0, run))
    return bytes(out)


def main():
    if len(sys.argv) not in (3, 4):
        sys.exit(__doc__)
    img = bytearray(open(sys.argv[1], 'rb').read())
    if len(img) > SECTORS * SECTOR or len(img) % SECTOR:
        sys.exit('make_card: the card must be whole sectors, at most 64 MB')
    at, size = fat32_file(img, ['PROJ00', 'GLO.CFG'])
    if size < 1:
        sys.exit('make_card: GLO.CFG is empty')
    if size < NOTE1 + 7:
        sys.exit('make_card: GLO.CFG is too short')
    print(f'tempo {img[at]} -> MID, voice notes {list(img[at + NOTE1:at + NOTE1 + 7])} -> 36..42')
    img[at] = 1   # PAR_BPM, the first global: 1 = MIDI clock
    for v in range(7):
        img[at + NOTE1 + v] = 36 + v   # PAR_MIDI_NOTE1..7

    out = bytearray(b'VKC2')
    used = 0
    first = {}   # a sector's bytes -> where they first are
    for s in range(len(img) // SECTOR):
        d = bytes(img[s * SECTOR:(s + 1) * SECTOR])
        if not any(d):
            continue
        used += 1
        if d in first:
            out += struct.pack('<IHI', s, 0xFFFF, first[d])
            continue
        first[d] = s
        p = pack_sector(d)
        out += struct.pack('<IH', s, len(p)) + p
    open(sys.argv[2], 'wb').write(out)
    print(f'{sys.argv[2]}: {used} sectors, {len(out)} bytes')
    if len(sys.argv) == 4:
        open(sys.argv[3], 'wb').write(img)


if __name__ == '__main__':
    main()
