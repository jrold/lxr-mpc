#!/usr/bin/env python3
"""The default card VOLTKIT's tracks start with (data/card.vkc).

    python3 tools/make_card.py <card.img> data/card.vkc

<card.img> is lxr02-recomp's card (4.web/site/card.img.gz, unzipped): a 64 MB
FAT32 card with project 0 and its 11 kits. The tempo in PROJ00/GLO.CFG is set
to MID (1), so the sequencer follows the MPC's MIDI clock (0, EXT, follows the
clock input jack); then the card is packed in mpc/CardCodec.h's format.

Copyright (C) 2026 Jason Rold. GPL-3.0-only.
"""
import struct
import sys

SECTOR = 512
SECTORS = 131072


def fat32_file(img, path):
    """(first byte offset, size) of a file given as ['DIR', 'NAME.EXT'], from the root."""
    bps, spc, rsv, nfats = struct.unpack_from('<HBHB', img, 11)
    fat_sectors = struct.unpack_from('<I', img, 36)[0]
    cluster = struct.unpack_from('<I', img, 44)[0]
    data = (rsv + nfats * fat_sectors) * bps
    if bps != SECTOR:
        sys.exit('make_card: not a 512-byte-sector card')

    def next_cluster(c):
        return struct.unpack_from('<I', img, rsv * bps + 4 * c)[0] & 0x0FFFFFFF

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
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    img = bytearray(open(sys.argv[1], 'rb').read())
    if len(img) > SECTORS * SECTOR or len(img) % SECTOR:
        sys.exit('make_card: the card must be whole sectors, at most 64 MB')
    at, size = fat32_file(img, ['PROJ00', 'GLO.CFG'])
    if size < 1:
        sys.exit('make_card: GLO.CFG is empty')
    print(f'tempo {img[at]} -> MID')
    img[at] = 1   # PAR_BPM, the first global: 1 = MIDI clock

    out = bytearray(b'VKC1')
    used = 0
    for s in range(len(img) // SECTOR):
        d = img[s * SECTOR:(s + 1) * SECTOR]
        if not any(d):
            continue
        p = pack_sector(d)
        out += struct.pack('<IH', s, len(p)) + p
        used += 1
    open(sys.argv[2], 'wb').write(out)
    print(f'{sys.argv[2]}: {used} sectors, {len(out)} bytes')


if __name__ == '__main__':
    main()
