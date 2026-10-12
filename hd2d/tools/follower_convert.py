#!/usr/bin/env python3
"""Converts the overworld ("follower") sprites of pokeemerald-expansion into
the file the game loads for following Pokémon.

    hd2d/scripts/fetch-follower-sprites.sh
    python3 hd2d/tools/follower_convert.py hd2d/external/expansion hd2d/data/followers.bin

Source: graphics/pokemon/<species>[/<form>]/overworld.png (indexed, a strip
of square frames: south x2, north x2, west x2 and sometimes east x2) with
overworld_normal.pal and overworld_shiny.pal (JASC). Frames of 64x64 are
cropped to 32x32 when the Pokémon fits, else shrunk to half size.

Output, little endian:
  char magic[4] "FOL1"
  u16  recordCount        species 0..NUM_SPECIES-1, then Unown B..Z, !, ?
  u16  framesPerRecord    9 (Emerald's standard walking layout)
  per record:
    u8   present, u8 pad[3]
    u8   frames[9][512]   32x32 4bpp, 1D tile order (4x4 tiles)
    u16  palette[16]      BGR555
    u16  shinyPalette[16]
Frame layout: 0 face south, 1 face north, 2 face west, 3-4 walk south,
5-6 walk north, 7-8 walk west (east is west mirrored).
"""

import os
import re
import struct
import sys

from PIL import Image

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
FRAME = 32
FRAMES_OUT = 9

FOLDER_NAMES = {
    "NIDORAN_F": "nidoran_f", "NIDORAN_M": "nidoran_m", "MR_MIME": "mr_mime", "FARFETCHD": "farfetchd",
    "HO_OH": "ho_oh", "DEOXYS": "deoxys/speed",
}
UNOWN_LETTERS = [chr(c) for c in range(ord("b"), ord("z") + 1)] + ["exclamation", "question"]


def species_table():
    """Internal species number -> constant name (without SPECIES_), from the decomp."""
    text = open(os.path.join(ROOT, "include", "constants", "species.h"), encoding="utf-8").read()
    table = {}
    for name, value in re.findall(r"#define SPECIES_(\w+) (\d+)\n", text):
        table[int(value)] = name
    count = int(re.search(r"#define SPECIES_EGG (\d+)", text).group(1))
    return table, count


def read_jasc(path):
    lines = open(path, encoding="utf-8").read().split()
    count = int(lines[2])
    values = list(map(int, lines[3:3 + count * 3]))
    colors = [tuple(values[i:i + 3]) for i in range(0, count * 3, 3)]
    colors += [(0, 0, 0)] * (16 - len(colors))
    return colors[:16]


def bgr555(colors):
    return [(r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) for r, g, b in colors]


def frame_pixels(img, index, size):
    """Palette indices of frame `index` (rows of `size`)."""
    px = img.load()
    x0 = index * size
    return [[px[x0 + x, y] & 15 for x in range(size)] for y in range(size)]


def shrink_64(frames):
    """64x64 frames -> 32x32: crop (feet at the bottom, centered) when the
    Pokémon fits, else halve each side keeping the most common color of
    every 2x2 block."""
    xs, ys = [], []
    for f in frames:
        for y in range(64):
            for x in range(64):
                if f[y][x]:
                    xs.append(x)
                    ys.append(y)
    if not xs:
        return [[[0] * 32 for _ in range(32)] for _ in frames]
    x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
    if x1 - x0 < 32 and y1 - y0 < 32:
        cx = max(0, min(32, (x0 + x1 + 1) // 2 - 16))
        cy = max(0, min(32, y1 + 1 - 32))
        return [[row[cx:cx + 32] for row in f[cy:cy + 32]] for f in frames]
    out = []
    for f in frames:
        small = []
        for y in range(32):
            row = []
            for x in range(32):
                block = [f[2 * y][2 * x], f[2 * y][2 * x + 1], f[2 * y + 1][2 * x], f[2 * y + 1][2 * x + 1]]
                opaque = [v for v in block if v]
                if len(opaque) < 2:
                    row.append(0)
                else:
                    row.append(max(set(opaque), key=lambda v: (opaque.count(v), -v)))
            small.append(row)
        out.append(small)
    return out


def to_tiles(frame):
    """32x32 indices -> 512 bytes of 4bpp tiles in 1D order."""
    data = bytearray()
    for ty in range(4):
        for tx in range(4):
            for y in range(8):
                row = frame[ty * 8 + y]
                for x in range(0, 8, 2):
                    lo, hi = row[tx * 8 + x], row[tx * 8 + x + 1]
                    data.append(lo | (hi << 4))
    return bytes(data)


def convert_folder(folder, palette_folder):
    path = os.path.join(folder, "overworld.png")
    if not os.path.exists(path):
        return None
    img = Image.open(path)
    if img.mode != "P":
        raise ValueError(f"{path}: not an indexed image")
    size = img.size[1]
    count = img.size[0] // size
    frames = [frame_pixels(img, i, size) for i in range(count)]
    if count < 6:
        raise ValueError(f"{path}: only {count} frames")
    if size == 64:
        frames = shrink_64(frames)
    elif size != 32:
        raise ValueError(f"{path}: frames of {size} pixels")
    s1, s2, n1, n2, w1, w2 = frames[:6]
    layout = [s1, n1, w1, s2, s2, n2, n2, w2, w2]
    normal = os.path.join(palette_folder, "overworld_normal.pal")
    shiny = os.path.join(palette_folder, "overworld_shiny.pal")
    pal = read_jasc(normal)
    shiny_pal = read_jasc(shiny) if os.path.exists(shiny) else pal
    return b"".join(to_tiles(f) for f in layout), bgr555(pal), bgr555(shiny_pal)


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 1
    source, out_path = sys.argv[1], sys.argv[2]
    gfx = os.path.join(source, "graphics", "pokemon")
    names, num_species = species_table()
    records = []
    missing = []
    for species in range(num_species):
        name = names.get(species, "")
        if species == 0 or name.startswith("OLD_UNOWN") or not name:
            records.append(None)
            continue
        folder = os.path.join(gfx, FOLDER_NAMES.get(name, name.lower()))
        palette_folder = folder
        # forms keep their palettes in the species folder when they have none
        if not os.path.exists(os.path.join(folder, "overworld_normal.pal")):
            palette_folder = os.path.dirname(folder)
        try:
            record = convert_folder(folder, palette_folder)
        except ValueError as e:
            print("aviso:", e)
            record = None
        if record is None:
            missing.append(name)
        records.append(record)
    for letter in UNOWN_LETTERS:
        folder = os.path.join(gfx, "unown", letter)
        record = convert_folder(folder, os.path.join(gfx, "unown"))
        if record is None:
            missing.append("UNOWN_" + letter)
        records.append(record)

    data = bytearray(struct.pack("<4sHH", b"FOL1", len(records), FRAMES_OUT))
    for record in records:
        if record is None:
            data += bytes(4 + FRAMES_OUT * 512 + 64)
            continue
        frames, pal, shiny = record
        data += bytes((1, 0, 0, 0)) + frames + struct.pack("<16H", *pal) + struct.pack("<16H", *shiny)
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(data)
    present = sum(1 for r in records if r is not None)
    print(f"{present} sprites em {out_path} ({len(data) // 1024} KB)")
    if missing:
        print("sem sprite:", ", ".join(missing))
    return 0


if __name__ == "__main__":
    sys.exit(main())
