#!/usr/bin/env python3
"""Reconstrói cada quadro dos arquivos .hdm/.png e compara com os GIFs originais.
Uso: check_hdmon.py <animated-dir> <hdmon-dir>"""
import os, struct, sys
from PIL import Image, ImageSequence

def read_hdm(path):
    data = open(path, "rb").read()
    magic, w, h, cols, uniq, seqlen, palsize, flags, _ = struct.unpack_from("<4s8H", data, 0)
    assert magic == b"HDM1"
    off = 20
    pal = struct.unpack_from(f"<{palsize}I", data, off); off += 4 * palsize
    shiny = None
    if flags & 1:
        shiny = struct.unpack_from(f"<{palsize}I", data, off); off += 4 * palsize
    seq = struct.unpack_from(f"<{seqlen}H", data, off); off += 2 * seqlen
    dur = struct.unpack_from(f"<{seqlen}H", data, off); off += 2 * seqlen
    assert off == len(data), "tamanho inesperado"
    return w, h, cols, pal, shiny, seq, dur

def unpack(c):
    return (c & 255, (c >> 8) & 255, (c >> 16) & 255, (c >> 24) & 255)

def check(src, hdm_dir, view, name, shiny):
    base = os.path.join(hdm_dir, view, name)
    w, h, cols, pal, shinypal, seq, dur = read_hdm(base + ".hdm")
    palette = shinypal if shiny else pal
    atlas = Image.open(base + ".png")
    prefix = ("" if view.startswith("front") else "back/") + ("shiny/" if shiny or view.endswith("-shiny") else "")
    gif = Image.open(os.path.join(src, prefix + name + ".gif"))
    frames = []
    for f in ImageSequence.Iterator(gif):
        # the crop box may extend past this GIF (normal and shiny GIFs can differ
        # in size); pad with transparency so every placement can be searched
        canvas = Image.new("RGBA", (f.size[0] + w, f.size[1] + h), (0, 0, 0, 0))
        canvas.paste(f.convert("RGBA"), (0, 0))
        frames.append(canvas)
    if len(frames) != len(seq):
        return f"{len(frames)} quadros no GIF, {len(seq)} no .hdm"
    # where is the crop box? find it from the union bbox of the GIF itself
    for i, (f, idx) in enumerate(zip(frames, seq)):
        tile = atlas.crop(((idx % cols) * w, (idx // cols) * h, (idx % cols + 1) * w, (idx // cols + 1) * h))
        rebuilt = [unpack(palette[p]) for p in tile.get_flattened_data()]
        # compare against every placement is costly; the crop is the union bbox, so
        # the frame's content must appear at the same offset in all frames
        if i == 0:
            target_box = None
        fb = f.getchannel("A").getbbox()
        if fb is None:
            continue
        # search the offset once
        if target_box is None:
            for ox in range(0, f.size[0] - w + 1):
                for oy in range(0, f.size[1] - h + 1):
                    if list(f.crop((ox, oy, ox + w, oy + h)).get_flattened_data()) == rebuilt or \
                       [(p if p[3] else (0,0,0,0)) for p in f.crop((ox, oy, ox + w, oy + h)).get_flattened_data()] == rebuilt:
                        target_box = (ox, oy)
                        break
                if target_box:
                    break
            if target_box is None:
                return f"quadro {i} não bate com o GIF"
        ox, oy = target_box
        orig = [(p if p[3] else (0, 0, 0, 0)) for p in f.crop((ox, oy, ox + w, oy + h)).get_flattened_data()]
        if orig != rebuilt:
            return f"quadro {i} difere"
    return None

def main():
    src, hdm_dir = sys.argv[1:3]
    bad = total = 0
    for view in sorted(os.listdir(hdm_dir)):
        for fn in sorted(os.listdir(os.path.join(hdm_dir, view))):
            if not fn.endswith(".hdm"):
                continue
            name = fn[:-4]
            variants = [False]
            if not view.endswith("-shiny"):
                w, h, cols, pal, shinypal, seq, dur = read_hdm(os.path.join(hdm_dir, view, fn))
                if shinypal is not None:
                    variants.append(True)
            for shiny in variants:
                total += 1
                err = check(src, hdm_dir, view, name, shiny)
                if err:
                    bad += 1
                    print(f"{view}/{name}{' (shiny)' if shiny else ''}: {err}")
    print(f"{total} animações conferidas, {bad} com erro")
    return 1 if bad else 0

sys.exit(main())
