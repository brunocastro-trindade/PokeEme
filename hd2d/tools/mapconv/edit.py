"""Changing annotations: metatile placements (per tileset) and hand-set
levels (per layout). Used by the command line and by the map editor."""

import collections
import os

import placement
from decomp import unpack_block
from placement import parse_ids, parse_spec, write_json

HD2D_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
TILESET_DIR = os.path.join(HD2D_DIR, "mapdata", "tilesets")
LAYOUT_DIR = os.path.join(HD2D_DIR, "mapdata", "layouts")


def parse_id_list(text):
    """'1D4-1D7,1E0' -> [0x1D4, 0x1D5, 0x1D6, 0x1D7, 0x1E0]"""
    ids = []
    for part in text.split(","):
        part = part.strip()
        if part:
            ids.extend(parse_ids(part))
    return ids


def _compress(per_id):
    """{id: [bottom, top]} -> {"1D4-1D7": [...], ...}, consecutive ids with
    the same specs merged into ranges"""
    out = {}
    ids = sorted(per_id)
    i = 0
    while i < len(ids):
        j = i
        while j + 1 < len(ids) and ids[j + 1] == ids[j] + 1 and per_id[ids[j + 1]] == per_id[ids[i]]:
            j += 1
        key = f"{ids[i]:03X}" if i == j else f"{ids[i]:03X}-{ids[j]:03X}"
        out[key] = list(per_id[ids[i]])
        i = j + 1
    return out


def set_metatiles(tileset, ids, bottom, top, directory=TILESET_DIR):
    """Annotates metatiles (bottom/top are spec strings); None removes the
    annotation (back to the automatic rules)."""
    path = os.path.join(directory, tileset + ".json")
    data = placement.Annotations(directory).raw(tileset)
    per_id = {}
    for key, specs in data.get("metatiles", {}).items():
        for mid in parse_ids(key):
            per_id[mid] = list(specs)
    if bottom is None:
        for mid in ids:
            per_id.pop(mid, None)
    else:
        parse_spec(bottom)
        parse_spec(top)
        for mid in ids:
            per_id[mid] = [bottom, top]
    data["metatiles"] = _compress(per_id)
    os.makedirs(directory, exist_ok=True)
    write_json(path, data)


def set_doc(tileset, text, directory=TILESET_DIR):
    path = os.path.join(directory, tileset + ".json")
    data = placement.Annotations(directory).raw(tileset)
    ordered = {"_doc": text}
    ordered.update({k: v for k, v in data.items() if k != "_doc"})
    write_json(path, ordered)


def walkable(layout, x, y):
    return unpack_block(layout.blocks[y * layout.width + x])[1] == 0


def region(layout, x, y):
    """Walkable cells connected to (x, y)."""
    w, h = layout.width, layout.height
    if not walkable(layout, x, y):
        return [(x, y)]
    seen = {(x, y)}
    queue = collections.deque([(x, y)])
    while queue:
        cx, cy = queue.popleft()
        for nx, ny in ((cx + 1, cy), (cx - 1, cy), (cx, cy + 1), (cx, cy - 1)):
            if 0 <= nx < w and 0 <= ny < h and (nx, ny) not in seen and walkable(layout, nx, ny):
                seen.add((nx, ny))
                queue.append((nx, ny))
    return sorted(seen)


def set_levels(layout, cells, level, directory=LAYOUT_DIR):
    """Hand-sets the level of cells (None: back to automatic)."""
    store = placement.LayoutLevels(directory)
    grid = store.load(layout)
    for x, y in cells:
        if 0 <= x < layout.width and 0 <= y < layout.height:
            grid[y * layout.width + x] = level
    store.save(layout, grid)
