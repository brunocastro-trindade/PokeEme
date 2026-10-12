"""Where each metatile layer goes in 3D, and the per-layout tables the game loads.

Every metatile has two layers (bottom, top); each gets a placement:
  none      not drawn
  ground    flat on the cell's ground
  card R    standing upright on the south edge of the cell R rows below,
            covering heights R..R+1 (the GBA draws things that stand up as
            if they were lying on the cells north of their base). Pixels
            equal to the floor under the cell are cut out, or, with
            "cut=12-15", pixels of those palette colors (ground and shadows
            painted around the object).
  wall R    like card, drawn without cutting out
  roof H    flat, at height H, moved H cells south (the top of a box
            whose front walls are H cells tall)
  water     flat, slightly below the ground
  grass     standing grass: the floor, plus the metatile as two rows of
            swaying blades (needs "cut=" for the colors between the blades)
  face      a card that stacks: each face cell of a column sits one block
            above the face cell below it (cliffs, rock walls, house fronts)
  top       flat at the height its column's faces reach (the top of a rock
            mass, a roof)

Heights: every cell has a level (blocks). A cell at level L is drawn L blocks
up and L blocks south, so from the game camera it stays where the GBA draws
it (the same trick as "roof"). Levels come from:
  - walkable cells: hd2d/mapdata/layouts/<layout>.json, "levels" (one
    string per row, '.' = automatic, '0'-'9' and 'a'-'v' = 0-31); an area
    left automatic sits on top of the rock faces along its south edge when
    they make up most of it (cliff tops, rock islands), else at 0;
  - blocked cells: the level of the walkable cell below their column, plus
    one per "face" cell under them in the same column (at most MAX_STACK);
    cards, walls and roofs (which have their own rise) stay on that base.

Annotations live in hd2d/mapdata/tilesets/<tileset>.json:
  {"default_floor": "001",
   "metatiles": {"1DC": ["card 0", "card 2"], "208-20A": ["ground", "roof 2"]}}
Ids are hex as Porymap shows them (secondary tilesets start at 200).
Metatiles without an annotation get automatic rules (see PairAnalysis).

Particles: every layout has a theme (see particle_theme), or the one set in
its layout file: {"particles": "embers"}. Themes: PARTICLE_THEMES.

Output, one file per layout (hd2d/data/field/layout_<id>.fld), little endian:
  char magic[4] "FLD5"
  u16  layoutId        (1-based, as gMapHeader.mapLayoutId)
  u16  width, height
  u16  defaultFloor    metatile drawn under non-ground cells outside the map
  u16  particleTheme   index in PARTICLE_THEMES
  u8   placement[1024][2]   (kind << 4) | rise, for metatiles 0-1023, bottom then top
                            (kinds as in KINDS, plus face 7 and top 8)
  u16  cutMask[1024][2]     palette colors (bit n = color n) cut out of cards
  u16  floor[width*height]  metatile drawn under each cell whose bottom is not ground
  u8   level[width*height]  level of each cell, in quarter blocks
  u8   base[width*height]   level of the ground under each cell (under walls
                            stacked on it, it is the level they stand on)
  u8   elevationLevel[16]   level of each GBA elevation, in quarter blocks
                            (people on bridge cells, elevation 15, stand at
                            the level of their own elevation: on the deck or
                            under it)
"""

import collections
import json
import os
import struct
from dataclasses import dataclass

from decomp import NUM_METATILES_IN_PRIMARY, LAYER_COVERED, behavior_of, layer_type_of, unpack_block

KINDS = {"none": 0, "ground": 1, "card": 2, "wall": 3, "roof": 4, "water": 5, "grass": 6}
FILE_KINDS = {"face": 7, "top": 8}  # roles get their own kind in the table (the game draws them as card / ground)
KIND_NAMES = {v: k for k, v in KINDS.items()}
ROLES = {"face": "card", "top": "ground"}  # stacking roles and the kind they are drawn as

WATER_BEHAVIORS = {
    "MB_POND_WATER", "MB_INTERIOR_DEEP_WATER", "MB_DEEP_WATER", "MB_SOOTOPOLIS_DEEP_WATER",
    "MB_OCEAN_WATER", "MB_UNUSED_SOOTOPOLIS_DEEP_WATER", "MB_NO_SURFACING", "MB_SEAWEED",
    "MB_SEAWEED_NO_SURFACING", "MB_EASTWARD_CURRENT", "MB_WESTWARD_CURRENT",
    "MB_NORTHWARD_CURRENT", "MB_SOUTHWARD_CURRENT",
}
GRASS_BEHAVIORS = {"MB_TALL_GRASS", "MB_LONG_GRASS", "MB_ASHGRASS"}

LEVEL_CHARS = "0123456789abcdefghijklmnopqrstuv"

# Same order as PARTICLE_THEME_* in include/platform/field_particles.h
PARTICLE_THEMES = ["none", "meadow", "forest", "cave", "indoor", "embers", "underwater", "ocean"]
EMBER_LAYOUTS = {"LAYOUT_MT_CHIMNEY", "LAYOUT_FIERY_PATH", "LAYOUT_LAVARIDGE_TOWN_GYM_1F",
                 "LAYOUT_LAVARIDGE_TOWN_GYM_B1F"}
FOREST_LAYOUTS = {"LAYOUT_PETALBURG_WOODS", "LAYOUT_ROUTE119", "LAYOUT_ROUTE120", "LAYOUT_FORTREE_CITY",
                  "LAYOUT_SAFARI_ZONE_NORTH", "LAYOUT_SAFARI_ZONE_NORTHEAST", "LAYOUT_SAFARI_ZONE_NORTHWEST",
                  "LAYOUT_SAFARI_ZONE_SOUTH", "LAYOUT_SAFARI_ZONE_SOUTHEAST", "LAYOUT_SAFARI_ZONE_SOUTHWEST"}
BUILDING_WALL_ROWS = 2  # rows of wall under the roof of a house with a door
MAX_STACK = 4           # faces stack up to this many blocks over their base


@dataclass(frozen=True)
class Spec:
    kind: int
    rise: int = 0
    cut: int = 0
    role: str = None  # "face", "top" or None

    def text(self):
        name = self.role or KIND_NAMES[self.kind]
        parts = [name]
        if self.rise:
            parts.append(str(self.rise))
        if self.cut:
            parts.append("cut=" + mask_text(self.cut))
        return " ".join(parts)


NONE = Spec(KINDS["none"])
GROUND = Spec(KINDS["ground"])


def mask_text(mask):
    colors = [c for c in range(16) if mask >> c & 1]
    ranges, start = [], None
    for i, c in enumerate(colors):
        if start is None:
            start = c
        if i + 1 == len(colors) or colors[i + 1] != c + 1:
            ranges.append(f"{start}" if start == c else f"{start}-{c}")
            start = None
    return ",".join(ranges)


def parse_spec(spec):
    """'card 1 cut=12-15' -> Spec"""
    parts = spec.split()
    name = parts[0]
    role = name if name in ROLES else None
    kind = KINDS[ROLES.get(name, name)]
    rise, mask = 0, 0
    for part in parts[1:]:
        if part.startswith("cut="):
            for item in part[4:].split(","):
                a, _, b = item.partition("-")
                for color in range(int(a), int(b or a) + 1):
                    mask |= 1 << color
        else:
            rise = int(part)
    if not 0 <= rise <= 15:
        raise ValueError(f"rise out of range in '{spec}'")
    return Spec(kind, rise, mask, role)


def parse_ids(key):
    if "-" in key:
        a, b = key.split("-")
        return range(int(a, 16), int(b, 16) + 1)
    return [int(key, 16)]


class Annotations:
    """Hand-made placements, per tileset."""

    def __init__(self, directory):
        self.directory = directory
        self._cache = {}

    def path(self, tileset_name):
        return os.path.join(self.directory, tileset_name + ".json")

    def raw(self, tileset_name):
        path = self.path(tileset_name)
        if os.path.exists(path):
            return json.load(open(path, encoding="utf-8"))
        return {"metatiles": {}}

    def load(self, tileset_name):
        if tileset_name in self._cache:
            return self._cache[tileset_name]
        data = self.raw(tileset_name)
        table = {}
        for key, specs in data.get("metatiles", {}).items():
            for mid in parse_ids(key):
                table[mid] = (parse_spec(specs[0]), parse_spec(specs[1]))
        default_floor = int(data["default_floor"], 16) if "default_floor" in data else None
        self._cache[tileset_name] = (table, default_floor)
        return self._cache[tileset_name]

    def forget(self, tileset_name=None):
        if tileset_name is None:
            self._cache.clear()
        else:
            self._cache.pop(tileset_name, None)


class LayoutLevels:
    """Hand-set levels, per layout (hd2d/mapdata/layouts/<layout>.json)."""

    def __init__(self, directory):
        self.directory = directory

    def path(self, layout_name):
        return os.path.join(self.directory, layout_name + ".json")

    def load(self, layout):
        """Grid of levels in blocks (None where not set)."""
        path = self.path(layout.name)
        grid = [None] * (layout.width * layout.height)
        if not os.path.exists(path):
            return grid
        rows = json.load(open(path, encoding="utf-8")).get("levels", [])
        for y, row in enumerate(rows[:layout.height]):
            for x, ch in enumerate(row[:layout.width]):
                if ch != ".":
                    grid[y * layout.width + x] = LEVEL_CHARS.index(ch.lower())
        return grid

    def options(self, layout):
        path = self.path(layout.name)
        return json.load(open(path, encoding="utf-8")) if os.path.exists(path) else {}

    def set_option(self, layout, key, value):
        """value None removes the key"""
        path = self.path(layout.name)
        data = self.options(layout)
        if value is None:
            data.pop(key, None)
        else:
            data[key] = value
        os.makedirs(self.directory, exist_ok=True)
        if data:
            write_json(path, data)
        elif os.path.exists(path):
            os.remove(path)

    def save(self, layout, grid):
        rows = []
        for y in range(layout.height):
            rows.append("".join("." if v is None else LEVEL_CHARS[v]
                                for v in grid[y * layout.width:(y + 1) * layout.width]))
        path = self.path(layout.name)
        data = json.load(open(path, encoding="utf-8")) if os.path.exists(path) else {}
        if all(set(row) == {"."} for row in rows):
            data.pop("levels", None)
        else:
            data["levels"] = rows
        os.makedirs(self.directory, exist_ok=True)
        if data:
            write_json(path, data)
        elif os.path.exists(path):
            os.remove(path)


def write_json(path, data):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)
        f.write("\n")
    os.replace(tmp, path)


def is_door(name):
    return "DOOR" in name


class PairAnalysis:
    """Automatic placements for one tileset pair, from how its metatiles are
    used over every layout that has the pair (more samples than one map):

    - walkable most of the time: ground (water, grass by behavior);
    - blocked, with plain ground or water in the bottom layer and something
      in the top layer: ground (or water), and the top layer stands up (card,
      rise 1 when its pixels go on into the top layer of the cell below);
    - blocked, looking like the walkable ground (its colors are the ground's)
      and sitting on other blocked cells: "top" of a rock mass;
    - blocked, in a house with a door, above the two wall rows: "top" (roof);
    - doors: "face" (they are walkable, but part of the house front);
    - blocked, mostly with the same metatile above or below it: card (a
      hedge or fence running north-south: each cell stands on the ground);
    - other blocked: "face" (stacks: rock walls, house fronts, trees);
    - top layer drawn over the player on a walkable cell: card 1 (the upper
      part of the thing south of it), or roof 2 behind a house, or ground on
      bridges and walkways (cells above the map's ground elevation)."""

    def __init__(self, decomp, layouts, renderer):
        self.decomp = decomp
        self.layouts = layouts
        self.renderer = renderer
        self.behaviors = decomp.behaviors()
        self._analyze()

    def behavior(self, layout, mid):
        return self.behaviors.get(behavior_of(layout.metatile_attribute(mid)), "")

    def _analyze(self):
        layout0 = self.layouts[0]
        walk = collections.Counter()
        block = collections.Counter()
        for layout in self.layouts:
            for b in layout.blocks:
                mid, collision, _ = unpack_block(b)
                (block if collision else walk)[mid] += 1

        # Colors of the walkable ground (not water, not grass), by how much of it they cover
        colors = collections.Counter()
        for mid, count in walk.items():
            name = self.behavior(layout0, mid)
            if name in WATER_BEHAVIORS or name in GRASS_BEHAVIORS or block[mid] > count:
                continue
            for rgba, n in self._pixels(mid, 0).items():
                colors[rgba] += n * count
        # (95%: the edges of walkable cells bring in a bit of the walls' colors)
        self.ground_colors = self._main_colors(colors, 0.95)
        # and of the water; plus the 8x8 tiles both are made of (the same
        # whatever the palettes)
        colors = collections.Counter()
        self.ground_tiles, self.water_tiles = set(), set()
        for mid, count in walk.items():
            name = self.behavior(layout0, mid)
            tiles = self._bottom_tiles(mid)
            if name in WATER_BEHAVIORS:
                self.water_tiles.update(tiles)
                for rgba, n in self._pixels(mid, 0).items():
                    colors[rgba] += n * count
            elif name not in GRASS_BEHAVIORS and block[mid] <= count:
                self.ground_tiles.update(tiles)
        self.water_colors = self._main_colors(colors, 0.99)

        self.walk, self.block = walk, block
        self.roof_votes = collections.Counter()    # roof rows of houses
        self.wall_votes = collections.Counter()    # wall rows of houses
        self.stand_votes = collections.Counter()   # top layer continues the one below: rise + 1
        self.top_votes = collections.Counter()
        self.face_votes = collections.Counter()
        self.repeat_votes = collections.Counter()  # blocked, same metatile right above or below
        self.raised_votes = collections.Counter()  # walkable above the map's ground elevation (bridges, walkways)
        self.roof_top_votes = collections.Counter()  # walkable cells behind a roof
        for layout in self.layouts:
            self._vote(layout)

    @staticmethod
    def _main_colors(colors, share):
        """Colors that make up this share of the pixels"""
        total = sum(colors.values())
        out, covered = set(), 0
        for rgba, n in colors.most_common():
            if covered >= total * share:
                break
            out.add(rgba)
            covered += n
        return out

    def _bottom_tiles(self, mid):
        entries = self.layouts[0].metatile_entries(mid)
        return [e & 0x3FF for e in entries[:4]] if entries is not None else []

    def tile_share(self, mid, tiles):
        """Share of the bottom layer's 8x8 tiles that are in tiles."""
        own = self._bottom_tiles(mid)
        return sum(1 for t in own if t in tiles) / len(own) if own else 0.0

    def _edges(self, mid, layer):
        """Whether a layer has pixels on its top row and on its bottom row"""
        alpha = self.renderer.layers(mid)[layer].getchannel("A")
        top = any(alpha.getpixel((x, 0)) for x in range(16))
        bottom = any(alpha.getpixel((x, 15)) for x in range(16))
        return top, bottom

    def _pixels(self, mid, layer):
        img = self.renderer.layers(mid)[layer]
        counts = collections.Counter()
        for r, g, b, a in img.getdata():
            if a:
                counts[(r, g, b)] += 1
        return counts

    def similarity(self, mid, colors=None):
        """Share of the bottom layer drawn in ground colors (or these colors)."""
        colors = self.ground_colors if colors is None else colors
        pixels = self._pixels(mid, 0)
        total = sum(pixels.values())
        if total == 0:
            return 0.0
        return sum(n for c, n in pixels.items() if c in colors) / total

    def _vote(self, layout):
        w, h = layout.width, layout.height
        cells = [unpack_block(b) for b in layout.blocks]
        blocked = [c[1] != 0 for c in cells]
        land = collections.Counter(c[2] for c in cells if not c[1] and 1 <= c[2] <= 14)
        if land:
            ground_elevation = land.most_common(1)[0][0]
            for mid, collision, elev in cells:
                if not collision and (elev == 15 or (ground_elevation < elev <= 14)):
                    self.raised_votes[mid] += 1
        sims = {}

        def sim(mid):
            """How much a metatile looks like the walkable ground: its colors,
            and its 8x8 tiles (the colors alone mix up cave floors and walls)"""
            if mid not in sims:
                sims[mid] = min(self.similarity(mid), 0.5 + self.tile_share(mid, self.ground_tiles) / 2)
            return sims[mid]

        # Houses: blocked areas with a door (doors are walkable, but part of the
        # wall). Only the secondary tileset's metatiles: the town's buildings are
        # there, and the trees and fences of the primary one are left out.
        door = [is_door(self.behavior(layout, c[0])) for c in cells]
        solid = [(b and c[0] >= NUM_METATILES_IN_PRIMARY) or d for b, d, c in zip(blocked, door, cells)]
        building_row = [None] * (w * h)  # row of the cell above its column's base
        seen = [False] * (w * h)
        for start in range(w * h):
            if not solid[start] or seen[start]:
                continue
            component, stack = [], [start]
            seen[start] = True
            while stack:
                i = stack.pop()
                component.append(i)
                x, y = i % w, i // w
                for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
                    j = ny * w + nx
                    if 0 <= nx < w and 0 <= ny < h and solid[j] and not seen[j]:
                        seen[j] = True
                        stack.append(j)
            if len(component) > 400 or not any(door[i] for i in component):
                continue
            for i in component:
                x, y = i % w, i // w
                row = 0
                while y + row + 1 < h and solid[(y + row + 1) * w + x]:
                    row += 1
                building_row[i] = row

        for i, (mid, collision, _) in enumerate(cells):
            x, y = i % w, i // w
            below = (y + 1) * w + x if y + 1 < h else None
            above = (y - 1) * w + x if y > 0 else None
            if collision and any(j is not None and blocked[j] and cells[j][0] == mid for j in (below, above)):
                self.repeat_votes[mid] += 1
            # the top layer goes on into the one below (not just the same
            # thing repeated down a column): the upper part of something
            if (below is not None and blocked[below] and cells[below][0] != mid and self._edges(mid, 1)[1]
                    and self._edges(cells[below][0], 1)[0]):
                self.stand_votes[mid] += 1
            if collision:
                if building_row[i] is not None:
                    if building_row[i] >= BUILDING_WALL_ROWS:
                        self.top_votes[mid] += 2
                        self.roof_votes[mid] += 1
                    else:
                        self.face_votes[mid] += 2
                        self.wall_votes[mid] += 1
                    continue
                s = sim(mid)
                if s >= 0.97 or (s >= 0.8 and below is not None and blocked[below]):
                    self.top_votes[mid] += 1
                else:
                    self.face_votes[mid] += 1
            elif below is not None and building_row[below] is not None and building_row[below] >= BUILDING_WALL_ROWS:
                self.roof_top_votes[mid] += 1

    def grass_cut(self, mid):
        """Palette colors of a grass metatile that the ground also has."""
        entries = self.layouts[0].metatile_entries(mid)
        mask = 0
        img = self.renderer.layers(mid)[0]
        indices = self.renderer.indices(mid, 0)
        for (r, g, b, a), index in zip(img.getdata(), indices):
            if a and (r, g, b) in self.ground_colors:
                mask |= 1 << index
        return mask if entries is not None else 0

    def _watery(self, mid):
        return self.similarity(mid, self.water_colors) >= 0.9 or self.tile_share(mid, self.water_tiles) >= 0.75

    def _object_on_top(self, mid):
        plain = (self.similarity(mid) >= 0.9 or self.tile_share(mid, self.ground_tiles) >= 0.75)
        if not plain and not self._watery(mid):
            return False
        alpha = self.renderer.layers(mid)[1].getchannel("A")
        return sum(1 for a in alpha.getdata() if a) >= 24  # more than a few stray pixels

    def placement(self, mid):
        layout = self.layouts[0]
        attribute = layout.metatile_attribute(mid)
        behavior = self.behaviors.get(behavior_of(attribute), "")
        walkable = self.walk[mid] >= self.block[mid]
        top_has_pixels = self.renderer.layers(mid)[1].getchannel("A").getbbox() is not None

        if behavior in WATER_BEHAVIORS:
            bottom = Spec(KINDS["water"])
        elif is_door(behavior):
            bottom = Spec(KINDS["card"], role="face")  # part of the house front
        elif behavior in GRASS_BEHAVIORS and walkable:
            bottom = Spec(KINDS["grass"], 0, self.grass_cut(mid))
        elif walkable:
            bottom = GROUND
        elif top_has_pixels and not self.roof_votes[mid] and not self.wall_votes[mid] and self._object_on_top(mid):
            # a sign, post, fence or rock drawn over plain ground or water:
            # the ground stays flat, the thing stands up
            water = self._watery(mid)
            rise = 1 if self.stand_votes[mid] * 2 > self.block[mid] else 0
            return Spec(KINDS["water"] if water else KINDS["ground"]), Spec(KINDS["card"], rise)
        elif self.top_votes[mid] > self.face_votes[mid]:
            bottom = Spec(KINDS["ground"], role="top")
        elif self.repeat_votes[mid] * 5 >= self.block[mid] * 3:
            # the same thing over and over down a column: a hedge or fence
            # running north-south, each cell standing on the ground
            bottom = Spec(KINDS["card"])
        else:
            bottom = Spec(KINDS["card"], role="face")

        if not top_has_pixels:
            top = NONE
        elif not walkable or bottom.role == "face":
            # the upper layer of the same thing
            top = Spec(KINDS["ground"]) if bottom.role == "top" else Spec(KINDS["card"])
        elif layer_type_of(attribute) == LAYER_COVERED:
            top = GROUND  # drawn below the player: decoration on the ground
        elif self.raised_votes[mid] * 2 >= self.walk[mid]:
            top = GROUND  # the deck of a bridge or walkway: people walk on it
        elif self.roof_top_votes[mid] * 2 >= self.walk[mid]:
            top = Spec(KINDS["roof"], BUILDING_WALL_ROWS)
        else:
            top = Spec(KINDS["card"], 1)  # drawn above the player: sticks up
        return bottom, top


COUNTERS = ("walk", "block", "top_votes", "face_votes", "repeat_votes", "roof_top_votes", "roof_votes",
            "wall_votes", "stand_votes", "raised_votes")


class Analyses:
    """PairAnalysis for every tileset pair. The usage counts of a metatile are
    added up over every pair its tileset is in, so a metatile of the primary
    tileset gets the same automatic placement in every town."""

    def __init__(self, decomp, renderer_factory):
        self.decomp = decomp
        self.renderer_factory = renderer_factory
        self._cache = None

    def _analyze_all(self):
        pairs = collections.defaultdict(list)
        for info in self.decomp.layouts().values():
            pairs[(info["primary_tileset"], info["secondary_tileset"])].append(info["id"])
        self._cache = {}
        totals = {name: collections.Counter() for name in COUNTERS}
        for key, ids in pairs.items():
            layouts = [self.decomp.layout(i) for i in ids]
            analysis = PairAnalysis(self.decomp, layouts, self.renderer_factory(layouts[0]))
            self._cache[key] = analysis
            for name in COUNTERS:
                for mid, n in getattr(analysis, name).items():
                    tileset = key[0] if mid < NUM_METATILES_IN_PRIMARY else key[1]
                    totals[name][(tileset, mid)] += n
        for key, analysis in self._cache.items():
            for name in COUNTERS:
                total = totals[name]
                summed = collections.Counter()
                for (tileset, mid), n in total.items():
                    if tileset == (key[0] if mid < NUM_METATILES_IN_PRIMARY else key[1]):
                        summed[mid] = n
                setattr(analysis, name, summed)

    def for_layout(self, layout):
        if self._cache is None:
            self._analyze_all()
        return self._cache[(layout.primary.name, layout.secondary.name)]


def resolve_placements(layout, annotations, analysis):
    """Bottom and top Spec of every metatile 0-1023 for this layout, and
    whether each came from an annotation."""
    primary_table, _ = annotations.load(layout.primary.name)
    secondary_table, _ = annotations.load(layout.secondary.name)
    placements, manual = [], []
    for mid in range(1024):
        table = primary_table if mid < NUM_METATILES_IN_PRIMARY else secondary_table
        if mid in table:
            placements.append(table[mid])
            manual.append(True)
        elif layout.metatile_entries(mid) is None:
            placements.append((NONE, NONE))
            manual.append(False)
        else:
            placements.append(analysis.placement(mid))
            manual.append(False)
    return placements, manual


def _column_levels(layout, placements, walk_levels, levels, bases):
    """Blocked cells (and doors): the level of the walkable cell below their
    column, plus one per face under them."""
    w, h = layout.width, layout.height
    for x in range(w):
        base = 0
        stack = None
        for y in range(h - 1, -1, -1):
            i = y * w + x
            mid, collision, _ = unpack_block(layout.blocks[i])
            if not collision and placements[mid][0].role != "face":
                base = walk_levels[i]
                levels[i] = bases[i] = base
                stack = None
                continue
            # blocked, or a door (walkable, but part of the wall above it)
            if stack is None:
                stack = base
            if walk_levels[i] is not None:
                stack = walk_levels[i]
            bases[i] = base
            bottom = placements[mid][0]
            if bottom.role is None and bottom.kind in (KINDS["card"], KINDS["wall"], KINDS["roof"]):
                # a thing with its own rise (trees, houses) stands on the ground,
                # and whatever stacks above it starts from there again
                stack = walk_levels[i] if walk_levels[i] is not None else base
                levels[i] = stack
                continue
            levels[i] = stack
            if bottom.role == "face" and stack < base + MAX_STACK:
                stack += 1


ELEVATION_MIN_CELLS = 6  # smaller raised spots are left alone (stairs, single tiles)


def elevation_levels(layout, placements):
    """Level (blocks) of each GBA elevation 0-15, as _walk_elevations counts them."""
    land = collections.Counter()
    for mid, collision, elev in (unpack_block(b) for b in layout.blocks):
        bottom = placements[mid][0]
        if not collision and bottom.kind != KINDS["water"] and bottom.role != "face" and 1 <= elev <= 14:
            land[elev] += 1
    base = land.most_common(1)[0][0] if land else 3
    return [(e - base + 1) // 2 if 1 <= e <= 14 and e > base else 0 for e in range(16)]


def _walk_elevations(layout, placements):
    """Height each walkable dry cell has by its GBA elevation, in blocks:
    the most common land elevation is 0, every 2 steps above it one block
    (bridges and walkways at base + 1 also get one). Transition cells
    (elevation 0) get None; multi-level bridges (15) take their neighbors'."""
    w, h = layout.width, layout.height
    cells = [unpack_block(b) for b in layout.blocks]
    land = collections.Counter()
    for mid, collision, elev in cells:
        bottom = placements[mid][0]
        if not collision and bottom.kind != KINDS["water"] and bottom.role != "face" and 1 <= elev <= 14:
            land[elev] += 1
    out = [None] * (w * h)
    if not land:
        return out
    base = land.most_common(1)[0][0]
    for i, (mid, collision, elev) in enumerate(cells):
        if 1 <= elev <= 14 and elev > base:
            out[i] = (elev - base + 1) // 2
        elif 1 <= elev <= 14:
            out[i] = 0
    # multi-level bridges: the highest walkable neighbor level, spread along
    for _ in range(w + h):
        changed = False
        for i, (mid, collision, elev) in enumerate(cells):
            if elev != 15 or out[i] is not None:
                continue
            x, y = i % w, i // w
            near = [out[j] for j in ((i - 1) if x > 0 else None, (i + 1) if x + 1 < w else None,
                                     (i - w) if y > 0 else None, (i + w) if y + 1 < h else None)
                    if j is not None and out[j] is not None]
            if near:
                out[i] = max(near)
                changed = True
        if not changed:
            break
    return out


def _regions(layout, placements, elevations=None):
    """Walkable dry areas (doors and water left out): list of cell lists.
    With elevations, cells of different elevation heights are kept apart
    (cells without one join whatever is next to them)."""
    w, h = layout.width, layout.height
    region = [None] * (w * h)
    out = []
    for start in range(w * h):
        mid, collision, _ = unpack_block(layout.blocks[start])
        bottom = placements[mid][0]
        if collision or bottom.role == "face" or bottom.kind == KINDS["water"] or region[start] is not None:
            continue
        if elevations is not None and elevations[start] is None:
            continue  # transitions are joined later
        cells, stack = [], [start]
        region[start] = len(out)
        while stack:
            i = stack.pop()
            cells.append(i)
            x, y = i % w, i // w
            for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
                j = ny * w + nx
                if not (0 <= nx < w and 0 <= ny < h) or region[j] is not None:
                    continue
                m, c, _ = unpack_block(layout.blocks[j])
                b = placements[m][0]
                if c or b.role == "face" or b.kind == KINDS["water"]:
                    continue
                if elevations is not None and elevations[j] != elevations[start]:
                    continue
                region[j] = len(out)
                stack.append(j)
        out.append(cells)
    if elevations is not None:
        # transition cells join the lowest neighboring region
        for _ in range(4):
            for i in range(w * h):
                mid, collision, _ = unpack_block(layout.blocks[i])
                b = placements[mid][0]
                if region[i] is not None or collision or b.role == "face" or b.kind == KINDS["water"]:
                    continue
                x, y = i % w, i // w
                near = [region[j] for j in ((i - 1) if x > 0 else None, (i + 1) if x + 1 < w else None,
                                            (i - w) if y > 0 else None, (i + w) if y + 1 < h else None)
                        if j is not None and region[j] is not None]
                if near:
                    r = min(near, key=lambda r: elevations[out[r][0]])
                    region[i] = r
                    out[r].append(i)
    return out


def _raise_tops(layout, placements, hand_levels, levels):
    """Blocked rock tops next to higher walkable ground (or higher tops) are
    part of the same plateau: they rise to it, spreading through the tops."""
    w, h = layout.width, layout.height
    cells = [unpack_block(b) for b in layout.blocks]
    tops = [i for i, (mid, collision, _) in enumerate(cells)
            if collision and placements[mid][0].role == "top" and hand_levels[i] is None]

    def flat(j):
        mid, collision, _ = cells[j]
        bottom = placements[mid][0]
        if bottom.role == "top":
            return True
        return not collision and bottom.role != "face" and bottom.kind != KINDS["water"]

    for _ in range(w + h):
        changed = False
        for i in tops:
            x, y = i % w, i // w
            best = levels[i]
            for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
                if 0 <= nx < w and 0 <= ny < h and flat(ny * w + nx):
                    best = max(best, levels[ny * w + nx])
            if best > levels[i]:
                levels[i] = best
                changed = True
        if not changed:
            break


def compute_levels(layout, placements, hand_levels, bases=None, terrain=None):
    """Level of every cell in quarter blocks (and, in bases if given, the
    level of the ground each cell stands on).

    Walkable areas take their hand-set levels; without them, an area whose
    south edge is mostly terrain faces (terrain: metatile ids of rock walls,
    not houses) sits at the top of those faces: the top of a cliff or of a
    rock island. An area the faces say nothing about takes the height of its
    GBA elevation (bridges, walkways, plateaus with stairs), if it is big
    enough."""
    w, h = layout.width, layout.height
    levels = [0] * (w * h)
    if bases is None:
        bases = [0] * (w * h)
    region_level = {}
    elevations = _walk_elevations(layout, placements) if terrain is not None else None
    regions = _regions(layout, placements, elevations) if terrain is not None else []
    region_elevation = {}
    for r, cells in enumerate(regions):
        heights = collections.Counter(elevations[i] for i in cells if elevations[i] is not None)
        if heights and len(cells) >= ELEVATION_MIN_CELLS:
            region_elevation[r] = heights.most_common(1)[0][0]
    region_of = {}
    for r, cells in enumerate(regions):
        for i in cells:
            region_of[i] = r

    def walk_levels():
        out = [None] * (w * h)
        for i in range(w * h):
            mid, collision, _ = unpack_block(layout.blocks[i])
            if hand_levels[i] is not None:
                out[i] = hand_levels[i]
            elif not collision and placements[mid][0].role != "face":
                out[i] = region_level.get(region_of.get(i), 0)
        return out

    for _ in range(8 if regions else 1):
        walk = walk_levels()
        _column_levels(layout, placements, [0 if v is None else v for v in walk], levels, bases)
        # hand-set blocked cells keep their level
        for i in range(w * h):
            if hand_levels[i] is not None and unpack_block(layout.blocks[i])[1]:
                levels[i] = hand_levels[i]
        if regions:
            _raise_tops(layout, placements, hand_levels, levels)
        changed = False
        for r, cells in enumerate(regions):
            seeds = collections.Counter()
            edge = 0
            for i in cells:
                x, y = i % w, i // w
                if y + 1 >= h or not unpack_block(layout.blocks[i + w])[1]:
                    continue
                edge += 1
                # down the blocked run: only terrain faces, then the ground they stand on
                height, j = 0, i + w
                while j < w * h and unpack_block(layout.blocks[j])[1]:
                    if unpack_block(layout.blocks[j])[0] not in terrain:
                        height = -1
                        break
                    height += 1
                    j += w
                if height > 0 and j < w * h:
                    seeds[min(levels[j] + min(height, MAX_STACK), 31)] += 1
            level = 0
            if seeds and edge:
                best, count = seeds.most_common(1)[0]
                if count >= 2 and count >= edge * 0.6:
                    level = best
            if level == 0:
                level = region_elevation.get(r, 0)
            if region_level.get(r, 0) != level:
                region_level[r] = level
                changed = True
        if not changed:
            break
    for i in range(w * h):
        levels[i] = min(levels[i] * 4, 255)
        bases[i] = min(bases[i] * 4, 255)
    return levels


def terrain_faces(analysis, placements):
    """Metatiles that are rock walls: stacking faces that are not part of a house."""
    return {mid for mid in range(1024)
            if placements[mid][0].role == "face" and not analysis.wall_votes[mid]
            and not is_door(analysis.behavior(analysis.layouts[0], mid))}


def particle_theme(decomp, layout, options=None):
    """(theme name, whether it was set by hand)"""
    if options and options.get("particles") in PARTICLE_THEMES:
        return options["particles"], True
    if layout.id in EMBER_LAYOUTS or layout.id.startswith("LAYOUT_MAGMA_HIDEOUT"):
        return "embers", False
    if layout.id in FOREST_LAYOUTS:
        return "forest", False
    types = {m.map_type for m in decomp.maps().values() if m.layout_id == layout.id}
    for map_type, theme in (("MAP_TYPE_UNDERWATER", "underwater"), ("MAP_TYPE_UNDERGROUND", "cave"),
                            ("MAP_TYPE_INDOOR", "indoor"), ("MAP_TYPE_SECRET_BASE", "indoor"),
                            ("MAP_TYPE_OCEAN_ROUTE", "ocean"), ("MAP_TYPE_TOWN", "meadow"),
                            ("MAP_TYPE_CITY", "meadow"), ("MAP_TYPE_ROUTE", "meadow")):
        if map_type in types:
            return theme, False
    return "none", False


def build_layout_table(decomp, layout, layout_number, annotations, analyses, level_store):
    analysis = analyses.for_layout(layout)
    _, primary_floor = annotations.load(layout.primary.name)
    placements, _ = resolve_placements(layout, annotations, analysis)
    default_floor = primary_floor if primary_floor is not None else 1

    # Floor under non-ground cells: the nearest ground cell's metatile
    w, h = layout.width, layout.height
    floor = [None] * (w * h)
    queue = collections.deque()
    for i, block in enumerate(layout.blocks):
        mid = unpack_block(block)[0]
        bottom = placements[mid][0]
        if bottom.kind in (KINDS["ground"], KINDS["water"]) and bottom.role is None:
            floor[i] = mid
            queue.append(i)
    while queue:
        i = queue.popleft()
        x, y = i % w, i // w
        for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            if 0 <= nx < w and 0 <= ny < h and floor[ny * w + nx] is None:
                floor[ny * w + nx] = floor[i]
                queue.append(ny * w + nx)
    floor = [default_floor if f is None else f for f in floor]
    bases = [0] * (w * h)
    levels = compute_levels(layout, placements, level_store.load(layout), bases,
                            terrain_faces(analysis, placements))

    theme, _ = particle_theme(decomp, layout, level_store.options(layout))
    data = struct.pack("<4s5H", b"FLD5", layout_number, w, h, default_floor, PARTICLE_THEMES.index(theme))
    for bottom, top in placements:
        kinds = [FILE_KINDS.get(spec.role, spec.kind) for spec in (bottom, top)]
        data += bytes(((kinds[0] << 4) | bottom.rise, (kinds[1] << 4) | top.rise))
    for bottom, top in placements:
        data += struct.pack("<2H", bottom.cut, top.cut)
    data += struct.pack(f"<{w * h}H", *floor)
    data += bytes(levels)
    data += bytes(bases)
    data += bytes(min(v * 4, 255) for v in elevation_levels(layout, placements))
    return data, placements, levels
