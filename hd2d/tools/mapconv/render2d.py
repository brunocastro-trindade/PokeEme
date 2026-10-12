"""Draws metatiles and whole layouts the way the GBA does, for previews and
for checking the data reader against in-game screenshots."""

from PIL import Image

from decomp import NUM_PALS_IN_PRIMARY, NUM_TILES_IN_PRIMARY, unpack_block


class MetatileRenderer:
    """Renders the 16x16 bottom and top layers of a layout's metatiles (RGBA)."""

    def __init__(self, layout):
        self.layout = layout
        self._tile_cache = {}
        self._metatile_cache = {}
        self._index_cache = {}

    def _tile(self, entry):
        if entry in self._tile_cache:
            return self._tile_cache[entry]
        tile = entry & 0x3FF
        hflip, vflip, pal = (entry >> 10) & 1, (entry >> 11) & 1, entry >> 12
        ts = self.layout.primary if tile < NUM_TILES_IN_PRIMARY else self.layout.secondary
        if tile >= NUM_TILES_IN_PRIMARY:
            tile -= NUM_TILES_IN_PRIMARY
        palette_ts = self.layout.primary if pal < NUM_PALS_IN_PRIMARY else self.layout.secondary
        colors = palette_ts.palettes[pal]
        img = Image.new("RGBA", (8, 8), (0, 0, 0, 0))
        tx, ty = (tile % 16) * 8, (tile // 16) * 8
        src = ts.tiles
        if ty + 8 <= src.size[1]:
            px = src.load()
            out = img.load()
            for y in range(8):
                for x in range(8):
                    index = px[tx + x, ty + y] & 0xF
                    if index:
                        r, g, b = colors[index]
                        out[x, y] = (r, g, b, 255)
        if hflip:
            img = img.transpose(Image.FLIP_LEFT_RIGHT)
        if vflip:
            img = img.transpose(Image.FLIP_TOP_BOTTOM)
        self._tile_cache[entry] = img
        return img

    def layers(self, metatile_id):
        """Returns (bottom, top) 16x16 RGBA images of a metatile."""
        if metatile_id in self._metatile_cache:
            return self._metatile_cache[metatile_id]
        entries = self.layout.metatile_entries(metatile_id)
        result = []
        for layer in range(2):
            img = Image.new("RGBA", (16, 16), (0, 0, 0, 0))
            if entries is not None:
                for i in range(4):
                    tile = self._tile(entries[layer * 4 + i])
                    img.alpha_composite(tile, ((i % 2) * 8, (i // 2) * 8))
            result.append(img)
        self._metatile_cache[metatile_id] = tuple(result)
        return self._metatile_cache[metatile_id]

    def indices(self, metatile_id, layer):
        """Palette color index (0 = transparent) of each pixel of a layer, row by row."""
        key = (metatile_id, layer)
        if key in self._index_cache:
            return self._index_cache[key]
        out = [0] * 256
        entries = self.layout.metatile_entries(metatile_id)
        if entries is not None:
            for i in range(4):
                entry = entries[layer * 4 + i]
                tile = entry & 0x3FF
                hflip, vflip = (entry >> 10) & 1, (entry >> 11) & 1
                ts = self.layout.primary if tile < NUM_TILES_IN_PRIMARY else self.layout.secondary
                if tile >= NUM_TILES_IN_PRIMARY:
                    tile -= NUM_TILES_IN_PRIMARY
                tx, ty = (tile % 16) * 8, (tile // 16) * 8
                if ty + 8 > ts.tiles.size[1]:
                    continue
                px = ts.tiles.load()
                for y in range(8):
                    for x in range(8):
                        sx = 7 - x if hflip else x
                        sy = 7 - y if vflip else y
                        out[((i // 2) * 8 + y) * 16 + (i % 2) * 8 + x] = px[tx + sx, ty + sy] & 0xF
        self._index_cache[key] = out
        return out

    def flat(self, metatile_id, backdrop=None):
        bottom, top = self.layers(metatile_id)
        img = Image.new("RGBA", (16, 16), backdrop or (0, 0, 0, 0))
        img.alpha_composite(bottom)
        img.alpha_composite(top)
        return img


def backdrop_color(layout):
    r, g, b = layout.primary.palettes[0][0]
    return (r, g, b, 255)


def render_layout(layout, margin=0):
    """Whole layout (plus margin cells of border) as one RGBA image."""
    mr = MetatileRenderer(layout)
    w, h = layout.width + 2 * margin, layout.height + 2 * margin
    img = Image.new("RGBA", (w * 16, h * 16), backdrop_color(layout))
    for y in range(h):
        for x in range(w):
            metatile, _, _ = unpack_block(layout.block(x - margin, y - margin))
            img.alpha_composite(mr.flat(metatile), (x * 16, y * 16))
    return img


def render_tileset_sheet(layout, secondary=False, columns=8, scale=2):
    """All metatiles of the primary (or secondary) tileset in a grid, with ids."""
    from PIL import ImageDraw
    mr = MetatileRenderer(layout)
    ts = layout.secondary if secondary else layout.primary
    base = 512 if secondary else 0
    count = len(ts.metatiles)
    rows = (count + columns - 1) // columns
    cell = 16 * scale + 30
    img = Image.new("RGBA", (columns * cell, rows * (16 * scale + 12)), (40, 40, 40, 255))
    draw = ImageDraw.Draw(img)
    for i in range(count):
        x, y = (i % columns) * cell, (i // columns) * (16 * scale + 12)
        tile = mr.flat(base + i, backdrop_color(layout)).resize((16 * scale, 16 * scale), Image.NEAREST)
        img.paste(tile, (x, y + 10))
        draw.text((x, y), f"{base + i:03X}", fill=(255, 255, 0, 255))
    return img


def render_used_sheet(layouts, columns=10, scale=3):
    """Metatiles used by the given layouts (same tileset pair), bottom and top
    layer side by side, with hex ids."""
    from PIL import ImageDraw
    ids = sorted({unpack_block(b)[0] for layout in layouts for b in layout.blocks})
    mr = MetatileRenderer(layouts[-1])
    cell = 16 * scale
    rows = (len(ids) + columns - 1) // columns
    img = Image.new("RGB", (columns * (cell * 2 + 10), rows * (cell + 14)), (40, 40, 40))
    draw = ImageDraw.Draw(img)
    for i, mid in enumerate(ids):
        x, y = (i % columns) * (cell * 2 + 10), (i // columns) * (cell + 14)
        for j, layer in enumerate(mr.layers(mid)):
            bg = Image.new("RGBA", (16, 16), (255, 0, 255, 255))
            bg.alpha_composite(layer)
            img.paste(bg.convert("RGB").resize((cell, cell), Image.NEAREST), (x + j * cell, y + 12))
        draw.text((x + 1, y), f"{mid:03X}", fill=(255, 255, 0))
    return img


KIND_COLORS = {
    "none": (0, 0, 0, 0), "ground": (0, 0, 0, 0), "card": (255, 220, 0, 110), "wall": (170, 100, 40, 120),
    "roof": (230, 40, 40, 110), "water": (40, 120, 255, 90), "grass": (60, 220, 60, 110),
    "face": (255, 140, 0, 120), "top": (170, 70, 230, 110),
}
KIND_LETTERS = {"none": "-", "ground": "", "card": "C", "wall": "W", "roof": "R", "water": "~", "grass": "G",
                "face": "F", "top": "T"}


def spec_name(spec):
    from placement import KIND_NAMES
    return spec.role or KIND_NAMES[spec.kind]


def render_cells(layout, placements, manual, levels, box=None, scale=3):
    """Part of a layout, scaled, with each cell's metatile id (yellow: hand
    annotated), its bottom placement (color and letter, + rise) and level."""
    from PIL import ImageDraw
    x0, y0, x1, y1 = box or (0, 0, layout.width, layout.height)
    x0, y0 = max(0, x0), max(0, y0)
    x1, y1 = min(layout.width, x1), min(layout.height, y1)
    mr = MetatileRenderer(layout)
    cell = 16 * scale
    img = Image.new("RGBA", ((x1 - x0) * cell, (y1 - y0) * cell), backdrop_color(layout))
    overlay = Image.new("RGBA", img.size, (0, 0, 0, 0))
    draw = ImageDraw.Draw(overlay)
    for y in range(y0, y1):
        for x in range(x0, x1):
            mid, collision, _ = unpack_block(layout.block(x, y))
            px, py = (x - x0) * cell, (y - y0) * cell
            img.alpha_composite(mr.flat(mid).resize((cell, cell), Image.NEAREST), (px, py))
            bottom = placements[mid][0]
            name = spec_name(bottom)
            draw.rectangle((px, py, px + cell - 1, py + cell - 1), fill=KIND_COLORS[name],
                           outline=(0, 0, 0, 90))
            if collision:
                draw.line((px + 1, py + 1, px + 6, py + 1), fill=(255, 0, 0, 255), width=2)
            color = (255, 255, 0, 255) if manual[mid] else (255, 255, 255, 255)
            draw.text((px + 2, py + 2), f"{mid:03X}", fill=color, stroke_width=1, stroke_fill=(0, 0, 0, 255))
            letter = KIND_LETTERS[name] + (str(bottom.rise) if bottom.rise else "")
            top = placements[mid][1]
            tname = spec_name(top)
            if tname not in ("none",):
                letter += "/" + (KIND_LETTERS[tname] or "g") + (str(top.rise) if top.rise else "")
            draw.text((px + 2, py + cell - 13), letter, fill=(255, 255, 255, 255), stroke_width=1,
                      stroke_fill=(0, 0, 0, 255))
            level = levels[y * layout.width + x]
            if level:
                text = f"{level / 4:g}"
                draw.text((px + cell - 8 * len(text) - 2, py + cell - 13), text, fill=(120, 255, 255, 255),
                          stroke_width=1, stroke_fill=(0, 0, 0, 255))
    img.alpha_composite(overlay)
    return img


def render_placement_sheet(layout, placements, manual, ids, columns=12, scale=3):
    """Metatiles (bottom and top layer side by side) with their placements;
    yellow ids are hand annotated."""
    from PIL import ImageDraw
    mr = MetatileRenderer(layout)
    cell_w, cell_h = 16 * scale * 2 + 8, 16 * scale + 26
    rows = (len(ids) + columns - 1) // columns
    img = Image.new("RGB", (columns * cell_w, rows * cell_h), (40, 40, 40))
    draw = ImageDraw.Draw(img)
    for i, mid in enumerate(ids):
        x, y = (i % columns) * cell_w, (i // columns) * cell_h
        for j, layer in enumerate(mr.layers(mid)):
            bg = Image.new("RGBA", (16, 16), (255, 0, 255, 255))
            bg.alpha_composite(layer)
            img.paste(bg.convert("RGB").resize((16 * scale, 16 * scale), Image.NEAREST), (x + j * (16 * scale + 2), y + 12))
        bottom, top = placements[mid]
        draw.text((x + 1, y), f"{mid:03X}", fill=(255, 255, 0) if manual[mid] else (255, 255, 255))
        draw.text((x + 1, y + 12 + 16 * scale + 1), f"{bottom.text()} | {top.text()}"[:22], fill=(150, 220, 255))
    return img
