#!/usr/bin/env python3
"""Converts the Black/White animated Pokémon GIFs (PokeAPI/sprites) into the
format the game loads for HD battle sprites.

For every animation (front and back of each species/form) it writes:

  <out>/<view>/<name>.png   atlas of unique frames, 8-bit grayscale where the
                            gray level is a palette index (0 = transparent)
  <out>/<view>/<name>.hdm   metadata, little endian:
      char[4] magic "HDM1"
      u16 width, height     frame size (all frames cropped to one box)
      u16 atlasColumns
      u16 uniqueFrames
      u16 sequenceLength
      u16 paletteSize       entries, including index 0
      u16 flags             bit 0: a shiny palette follows the normal one
      u16 reserved
      u32 palette[paletteSize]        RGBA, R in the low byte
      u32 shinyPalette[paletteSize]   only with flag bit 0
      u16 frameIndex[sequenceLength]  atlas frame shown at each step
      u16 durationMs[sequenceLength]

The shiny GIF uses the same pixels with other colors, so it becomes a second
palette. When the colors do not map one to one, the shiny animation is
written on its own under <view>-shiny/.

Names follow PokeAPI: national dex number, plus a form suffix for Unown
(201-b ... 201-question), Castform (351-sunny ...) and Deoxys (386-speed ...).

Usage: hdmon_convert.py <animated-dir> <out-dir> [-j N]
"""

import argparse
import math
import os
import struct
import sys
from multiprocessing import Pool

from PIL import Image, ImageSequence

UNOWN_FORMS = [chr(c) for c in range(ord("a"), ord("z") + 1)] + ["exclamation", "question"]

# Every animation Emerald can show: species 1-386 plus the forms it uses.
def animation_names():
    names = [str(n) for n in range(1, 387)]
    names += ["201-" + f for f in UNOWN_FORMS]
    names += ["351-sunny", "351-rainy", "351-snowy"]
    names += ["386-normal", "386-speed"]
    return names


def load_frames(path):
    """Returns (RGBA frames, durations in ms)."""
    im = Image.open(path)
    frames, durations = [], []
    for frame in ImageSequence.Iterator(im):
        frames.append(frame.convert("RGBA"))
        durations.append(max(10, int(frame.info.get("duration", 100))))
    return frames, durations


def union_bbox(frame_lists):
    box = None
    for frames in frame_lists:
        for f in frames:
            b = f.getchannel("A").getbbox()
            if b is None:
                continue
            box = b if box is None else (min(box[0], b[0]), min(box[1], b[1]), max(box[2], b[2]), max(box[3], b[3]))
    return box


def rgba_pixels(frame):
    return list(frame.get_flattened_data()) if hasattr(frame, "get_flattened_data") else list(frame.getdata())


def build_palette(frames):
    colors = {}
    for f in frames:
        for px in rgba_pixels(f):
            if px[3] != 0 and px not in colors:
                colors[px] = len(colors) + 1
    if len(colors) > 255:
        raise ValueError(f"{len(colors)} colors, more than an 8-bit index holds")
    return colors


def index_frames(frames, colors):
    out = []
    for f in frames:
        out.append(bytes(0 if px[3] == 0 else colors[px] for px in rgba_pixels(f)))
    return out


def shiny_palette(normal_frames, shiny_frames, colors):
    """Maps every normal color to its shiny color, or None if they do not match one to one."""
    if len(normal_frames) != len(shiny_frames):
        return None
    mapping = {}
    for nf, sf in zip(normal_frames, shiny_frames):
        if nf.size != sf.size:
            return None
        for npx, spx in zip(rgba_pixels(nf), rgba_pixels(sf)):
            if (npx[3] == 0) != (spx[3] == 0):
                return None
            if npx[3] == 0:
                continue
            if mapping.setdefault(npx, spx) != spx:
                return None
    palette = [(0, 0, 0, 0)] * (len(colors) + 1)
    for color, index in colors.items():
        palette[index] = mapping.get(color, color)
    return palette


def pack_rgba(c):
    return c[0] | (c[1] << 8) | (c[2] << 16) | (c[3] << 24)


def write_animation(out_dir, view, name, frames, durations, colors, shiny):
    width, height = frames[0].size
    indexed = index_frames(frames, colors)

    unique, sequence = [], []
    seen = {}
    for data in indexed:
        if data not in seen:
            seen[data] = len(unique)
            unique.append(data)
        sequence.append(seen[data])

    columns = math.ceil(math.sqrt(len(unique)))
    rows = math.ceil(len(unique) / columns)
    atlas = Image.new("L", (columns * width, rows * height), 0)
    for i, data in enumerate(unique):
        tile = Image.frombytes("L", (width, height), data)
        atlas.paste(tile, ((i % columns) * width, (i // columns) * height))

    palette = [(0, 0, 0, 0)] * (len(colors) + 1)
    for color, index in colors.items():
        palette[index] = color

    os.makedirs(os.path.join(out_dir, view), exist_ok=True)
    atlas.save(os.path.join(out_dir, view, name + ".png"), optimize=True)
    flags = 1 if shiny is not None else 0
    meta = struct.pack("<4s8H", b"HDM1", width, height, columns, len(unique), len(sequence), len(palette), flags, 0)
    meta += struct.pack(f"<{len(palette)}I", *(pack_rgba(c) for c in palette))
    if shiny is not None:
        meta += struct.pack(f"<{len(shiny)}I", *(pack_rgba(c) for c in shiny))
    meta += struct.pack(f"<{len(sequence)}H", *sequence)
    meta += struct.pack(f"<{len(durations)}H", *durations)
    with open(os.path.join(out_dir, view, name + ".hdm"), "wb") as f:
        f.write(meta)
    return len(unique), len(sequence)


def convert(job):
    src_dir, out_dir, view, name = job
    prefix = "" if view == "front" else "back/"
    normal_path = os.path.join(src_dir, prefix + name + ".gif")
    shiny_path = os.path.join(src_dir, prefix + "shiny/" + name + ".gif")
    if not os.path.exists(normal_path):
        return (view, name, "missing", None)

    frames, durations = load_frames(normal_path)
    shiny_frames = load_frames(shiny_path)[0] if os.path.exists(shiny_path) else None

    # One crop box for normal and shiny, so the two stay pixel aligned
    box = union_bbox([frames] + ([shiny_frames] if shiny_frames else []))
    frames = [f.crop(box) for f in frames]
    colors = build_palette(frames)
    shiny = None
    note = ""
    if shiny_frames:
        shiny_frames = [f.crop(box) for f in shiny_frames]
        shiny = shiny_palette(frames, shiny_frames, colors)
        if shiny is None:
            # Not a pure palette swap: keep the shiny animation as its own file
            shiny_colors = build_palette(shiny_frames)
            write_animation(out_dir, view + "-shiny", name, shiny_frames,
                            load_frames(shiny_path)[1], shiny_colors, None)
            note = "shiny separado"
    else:
        note = "sem shiny"
    unique, length = write_animation(out_dir, view, name, frames, durations, colors, shiny)
    return (view, name, "ok", f"{frames[0].size[0]}x{frames[0].size[1]} {unique}/{length} quadros {len(colors)} cores {note}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("animated_dir", help="PokeAPI sprites/pokemon/versions/generation-v/black-white/animated")
    parser.add_argument("out_dir")
    parser.add_argument("-j", "--jobs", type=int, default=os.cpu_count())
    args = parser.parse_args()

    jobs = [(args.animated_dir, args.out_dir, view, name)
            for view in ("front", "back") for name in animation_names()]
    missing = 0
    with Pool(args.jobs) as pool:
        for view, name, status, info in pool.imap_unordered(convert, jobs):
            if status != "ok":
                missing += 1
                print(f"{view}/{name}: {status}", file=sys.stderr)
            elif info and ("shiny" in info):
                print(f"{view}/{name}: {info}")
    print(f"{len(jobs) - missing} animações convertidas, {missing} faltando")
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
