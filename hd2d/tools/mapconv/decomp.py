"""Reads map and tileset data straight from the pokeemerald source tree.

Formats (see include/global.fieldmap.h and include/fieldmap.h):
- map.bin / border.bin: one u16 per block; bits 0-9 metatile id, 10-11
  collision, 12-15 elevation. Metatile ids below 512 belong to the primary
  tileset, the rest to the secondary one.
- metatiles.bin: 8 u16 tile entries per metatile, bottom layer (TL, TR, BL,
  BR) then top layer. Entry bits: 0-9 tile, 10 h-flip, 11 v-flip, 12-15
  palette. Tiles from 512 on come from the secondary tileset.
- metatile_attributes.bin: one u16 per metatile; bits 0-7 behavior,
  12-15 layer type (0 normal, 1 covered, 2 split).
- Palettes 0-5 come from the primary tileset, 6-12 from the secondary.
"""

import json
import os
import re
import struct
from dataclasses import dataclass, field

from PIL import Image

NUM_METATILES_IN_PRIMARY = 512
NUM_TILES_IN_PRIMARY = 512
NUM_PALS_IN_PRIMARY = 6
NUM_PALS_TOTAL = 13

LAYER_NORMAL, LAYER_COVERED, LAYER_SPLIT = 0, 1, 2


@dataclass
class Tileset:
    name: str
    is_secondary: bool
    tiles: Image.Image          # indexed, 8x8 tiles, 16 per row
    palettes: list              # 16 palettes of 16 (r, g, b)
    metatiles: list             # per metatile: 8 tile entries
    attributes: list            # per metatile: u16


@dataclass
class Layout:
    id: str
    name: str
    width: int
    height: int
    blocks: list                # width*height u16
    border: list                # border blocks (2x2 usually)
    border_width: int
    border_height: int
    primary: Tileset
    secondary: Tileset

    def block(self, x, y):
        if 0 <= x < self.width and 0 <= y < self.height:
            return self.blocks[y * self.width + x]
        bx = x % self.border_width
        by = y % self.border_height
        return self.border[by * self.border_width + bx] | 0x0C00  # border cells are impassable

    def metatile_entries(self, metatile_id):
        if metatile_id < NUM_METATILES_IN_PRIMARY:
            ts, i = self.primary, metatile_id
        else:
            ts, i = self.secondary, metatile_id - NUM_METATILES_IN_PRIMARY
        if i >= len(ts.metatiles):
            return None
        return ts.metatiles[i]

    def metatile_attribute(self, metatile_id):
        if metatile_id < NUM_METATILES_IN_PRIMARY:
            ts, i = self.primary, metatile_id
        else:
            ts, i = self.secondary, metatile_id - NUM_METATILES_IN_PRIMARY
        return ts.attributes[i] if i < len(ts.attributes) else 0


@dataclass
class MapInfo:
    id: str
    name: str
    layout_id: str
    map_type: str
    connections: list = field(default_factory=list)
    warps: list = field(default_factory=list)   # (x, y) of each warp event
    number: int = -1                            # group << 8 | number, as the game counts them


def unpack_block(block):
    return block & 0x3FF, (block >> 10) & 3, block >> 12


def behavior_of(attribute):
    return attribute & 0xFF


def layer_type_of(attribute):
    return attribute >> 12


class Decomp:
    """Lazy access to layouts, maps and tilesets of a pokeemerald checkout."""

    def __init__(self, root):
        self.root = root
        self._tileset_sources = None
        self._tilesets = {}
        self._layouts = None
        self._maps = None
        self._behaviors = None

    def path(self, *parts):
        return os.path.join(self.root, *parts)

    # --- tilesets -------------------------------------------------------

    def _read(self, rel):
        with open(self.path(rel), encoding="utf-8") as f:
            return f.read()

    def _scan_tileset_sources(self):
        tiles, palettes, metatiles, attributes = {}, {}, {}, {}
        for rel in ("src/graphics.c", "src/data/tilesets/graphics.h"):
            text = self._read(rel)
            for sym, path in re.findall(r'const u32 (gTilesetTiles_\w+)\[\] = INCGFX_U32\("([^"]+)"', text):
                tiles[sym] = path
            for sym, body in re.findall(r'const u16 (gTilesetPalettes_\w+)\[\]\[16\] =\s*\{(.*?)\};', text, re.S):
                palettes[sym] = re.findall(r'INCGFX_U16\("([^"]+)"', body)
        text = self._read("src/data/tilesets/metatiles.h")
        for sym, path in re.findall(r'const u16 (gMetatiles_\w+)\[\] = INCBIN_U16\("([^"]+)"\)', text):
            metatiles[sym] = path
        for sym, path in re.findall(r'const u16 (gMetatileAttributes_\w+)\[\] = INCBIN_U16\("([^"]+)"\)', text):
            attributes[sym] = path
        sources = {}
        text = self._read("src/data/tilesets/headers.h")
        for name, body in re.findall(r'const struct Tileset (gTileset_\w+) =\s*\{(.*?)\};', text, re.S):
            fields = dict(re.findall(r'\.(\w+)\s*=\s*([\w]+)', body))
            sources[name] = {
                "secondary": fields.get("isSecondary") == "TRUE",
                "tiles": tiles.get(fields.get("tiles")),
                "palettes": palettes.get(fields.get("palettes"), []),
                "metatiles": metatiles.get(fields.get("metatiles")),
                "attributes": attributes.get(fields.get("metatileAttributes")),
            }
        self._tileset_sources = sources

    def tileset(self, name):
        if name in self._tilesets:
            return self._tilesets[name]
        if self._tileset_sources is None:
            self._scan_tileset_sources()
        if name not in self._tileset_sources:
            # Some special layouts have no secondary tileset ("0", i.e. NULL)
            empty = Tileset(name, True, Image.new("P", (128, 8)), [[(0, 0, 0)] * 16] * 16, [], [])
            self._tilesets[name] = empty
            return empty
        src = self._tileset_sources[name]
        tiles = Image.open(self.path(src["tiles"]))
        palettes = [read_jasc_pal(self.path(p)) for p in src["palettes"]]
        while len(palettes) < 16:
            palettes.append([(0, 0, 0)] * 16)
        raw = open(self.path(src["metatiles"]), "rb").read()
        entries = struct.unpack(f"<{len(raw) // 2}H", raw)
        metatiles = [entries[i:i + 8] for i in range(0, len(entries), 8)]
        raw = open(self.path(src["attributes"]), "rb").read()
        attributes = list(struct.unpack(f"<{len(raw) // 2}H", raw))
        ts = Tileset(name, src["secondary"], tiles, palettes, metatiles, attributes)
        self._tilesets[name] = ts
        return ts

    # --- layouts and maps -----------------------------------------------

    def layouts(self):
        if self._layouts is None:
            data = json.load(open(self.path("data/layouts/layouts.json")))
            self._layouts = {l["id"]: l for l in data["layouts"] if "id" in l}
        return self._layouts

    def layout(self, layout_id_or_name):
        layouts = self.layouts()
        info = layouts.get(layout_id_or_name)
        if info is None:
            info = next((l for l in layouts.values() if l["name"] == layout_id_or_name), None)
        if info is None:
            raise KeyError(layout_id_or_name)
        raw = open(self.path(info["blockdata_filepath"]), "rb").read()
        blocks = list(struct.unpack(f"<{len(raw) // 2}H", raw))
        # a few map.bin files are larger than width*height; the game reads only that much
        size = info["width"] * info["height"]
        blocks = (blocks + [0x3FF] * size)[:size]
        raw = open(self.path(info["border_filepath"]), "rb").read()
        border = list(struct.unpack(f"<{len(raw) // 2}H", raw))
        side = int(len(border) ** 0.5)
        return Layout(info["id"], info["name"], info["width"], info["height"], blocks, border,
                      side, len(border) // side, self.tileset(info["primary_tileset"]),
                      self.tileset(info["secondary_tileset"]))

    def maps(self):
        if self._maps is None:
            self._maps = {}
            maps_dir = self.path("data/maps")
            for name in sorted(os.listdir(maps_dir)):
                path = os.path.join(maps_dir, name, "map.json")
                if not os.path.exists(path):
                    continue
                m = json.load(open(path))
                warps = [(w["x"], w["y"]) for w in m.get("warp_events") or []]
                self._maps[m["id"]] = MapInfo(m["id"], m["name"], m["layout"], m.get("map_type", ""),
                                             m.get("connections") or [], warps)
            # map numbers: position of the group in group_order, then of the map in its group
            groups = json.load(open(self.path("data/maps/map_groups.json")))
            by_name = {m.name: m for m in self._maps.values()}
            for group, group_name in enumerate(groups["group_order"]):
                for number, map_name in enumerate(groups[group_name]):
                    if map_name in by_name:
                        by_name[map_name].number = (group << 8) | number
        return self._maps

    def map(self, map_id_or_name):
        maps = self.maps()
        if map_id_or_name in maps:
            return maps[map_id_or_name]
        for m in maps.values():
            if m.name == map_id_or_name:
                return m
        raise KeyError(map_id_or_name)

    # --- metatile behaviors -----------------------------------------------

    def behaviors(self):
        """Behavior number -> name (MB_*), from include/constants/metatile_behaviors.h"""
        if self._behaviors is None:
            text = self._read("include/constants/metatile_behaviors.h")
            names = {}
            m = re.search(r'enum\s*\{(.*?)\};', text, re.S)
            value = 0
            for line in m.group(1).splitlines():
                line = line.split("//")[0].strip().rstrip(",")
                if not line.startswith("MB_"):
                    continue
                if "=" in line:
                    name, expr = [p.strip() for p in line.split("=", 1)]
                    value = int(expr, 0)
                else:
                    name = line
                names[value] = name
                value += 1
            self._behaviors = names
        return self._behaviors


def read_jasc_pal(path):
    lines = open(path, encoding="utf-8").read().split()
    # JASC-PAL, 0100, count, then r g b triplets
    count = int(lines[2])
    values = list(map(int, lines[3:3 + count * 3]))
    return [tuple(values[i:i + 3]) for i in range(0, count * 3, 3)]
