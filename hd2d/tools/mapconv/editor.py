"""Map editor: a small web server for annotating the 3D field in the browser.

    python3 hd2d/tools/mapconv/mapconv.py editor     # opens http://localhost:8642

It reads and writes the same files as the command line (tileset annotations
in hd2d/mapdata/tilesets, levels in hd2d/mapdata/layouts) and rebuilds the
layout tables after every change. A game started with "Abrir no jogo" picks
the new table up on its own (it checks the file twice a second).
"""

import io
import json
import os
import subprocess
import threading
import time
import traceback
import urllib.parse
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from PIL import Image

import edit
import placement
import render2d
import tour
from decomp import unpack_block, behavior_of

HERE = os.path.dirname(os.path.abspath(__file__))
HD2D_DIR = os.path.abspath(os.path.join(HERE, "..", ".."))
ROOT = os.path.abspath(os.path.join(HD2D_DIR, ".."))
FIELD_DIR = os.path.join(HD2D_DIR, "data", "field")
TOUR_DIR = os.path.join(HD2D_DIR, "out", "tour")
PLAY_DIR = os.path.join(HD2D_DIR, "out", "editor")
ATLAS_COLUMNS = 16


class Project:
    def __init__(self, decomp):
        self.decomp = decomp
        self.lock = threading.RLock()
        self.analyses = placement.Analyses(decomp, render2d.MetatileRenderer)
        self.annotations = placement.Annotations(edit.TILESET_DIR)
        self.levels = placement.LayoutLevels(edit.LAYOUT_DIR)
        self.pending_tilesets = set()
        self.rebuild_timer = None
        self.renderers = {}
        self.maps_by_layout = {}
        for m in decomp.maps().values():
            if m.number >= 0:
                self.maps_by_layout.setdefault(m.layout_id, []).append(m)

    # --- reading ---------------------------------------------------------

    def layout(self, name):
        return self.decomp.layout(name)

    def renderer(self, layout):
        key = (layout.primary.name, layout.secondary.name)
        if key not in self.renderers:
            self.renderers[key] = render2d.MetatileRenderer(layout)
        return self.renderers[key]

    def layouts(self):
        out = []
        for layout_id, info in self.decomp.layouts().items():
            maps = self.maps_by_layout.get(layout_id, [])
            out.append({
                "name": info["name"], "width": info["width"], "height": info["height"],
                "primary": info["primary_tileset"], "secondary": info["secondary_tileset"],
                "maps": [m.name for m in maps],
            })
        return out

    def state(self, name):
        with self.lock:
            self.annotations.forget()
            layout = self.layout(name)
            analysis = self.analyses.for_layout(layout)
            placements, manual = placement.resolve_placements(layout, self.annotations, analysis)
            hand = self.levels.load(layout)
            levels = placement.compute_levels(layout, placements, hand, None,
                                              placement.terrain_faces(analysis, placements))
        cells, collision = [], []
        for b in layout.blocks:
            mid, col, _ = unpack_block(b)
            cells.append(mid)
            collision.append(1 if col else 0)
        used = sorted(set(cells))
        behaviors = self.decomp.behaviors()
        metatiles = {}
        for i, mid in enumerate(used):
            bottom, top = placements[mid]
            metatiles[mid] = {
                "atlas": i,
                "bottom": bottom.text(), "top": top.text(),
                "manual": manual[mid],
                "tileset": layout.primary.name if mid < 512 else layout.secondary.name,
                "behavior": behaviors.get(behavior_of(layout.metatile_attribute(mid)), "?"),
            }
        maps = self.maps_by_layout.get(layout.id, [])
        return {
            "name": layout.name, "width": layout.width, "height": layout.height,
            "primary": layout.primary.name, "secondary": layout.secondary.name,
            "maps": [m.name for m in maps],
            "cells": cells, "collision": collision, "levels": levels, "hand": hand,
            "used": used, "metatiles": metatiles, "atlasColumns": ATLAS_COLUMNS,
            "hasOverview": os.path.exists(os.path.join(TOUR_DIR, layout.name + ".png")),
        }

    def palette(self, name, mid):
        """Colors each layer of a metatile uses: index, rgb, pixel count."""
        layout = self.layout(name)
        mr = self.renderer(layout)
        out = []
        for layer in range(2):
            img = mr.layers(mid)[layer]
            counts = {}
            for (r, g, b, a), index in zip(img.getdata(), mr.indices(mid, layer)):
                if a:
                    entry = counts.setdefault(index, {"index": index, "rgb": [r, g, b], "count": 0})
                    entry["count"] += 1
            out.append(sorted(counts.values(), key=lambda e: e["index"]))
        return out

    def atlas_png(self, name):
        layout = self.layout(name)
        mr = self.renderer(layout)
        used = sorted({unpack_block(b)[0] for b in layout.blocks})
        rows = (len(used) + ATLAS_COLUMNS - 1) // ATLAS_COLUMNS
        img = Image.new("RGBA", (ATLAS_COLUMNS * 32, max(1, rows) * 16), (0, 0, 0, 0))
        for i, mid in enumerate(used):
            bottom, top = mr.layers(mid)
            x, y = (i % ATLAS_COLUMNS) * 32, (i // ATLAS_COLUMNS) * 16
            img.paste(bottom, (x, y))
            img.paste(top, (x + 16, y))
        return png_bytes(img)

    def map_png(self, name):
        return png_bytes(render2d.render_layout(self.layout(name)))

    # --- writing ---------------------------------------------------------

    def rebuild(self, layout):
        number = list(self.decomp.layouts()).index(layout.id) + 1
        with self.lock:
            self.annotations.forget()
            data, _, _ = placement.build_layout_table(self.decomp, layout, number, self.annotations,
                                                      self.analyses, self.levels)
        os.makedirs(FIELD_DIR, exist_ok=True)
        path = os.path.join(FIELD_DIR, f"layout_{number}.fld")
        with open(path + ".tmp", "wb") as f:
            f.write(data)
        os.replace(path + ".tmp", path)

    def rebuild_tilesets_later(self, tilesets):
        """Every layout that uses these tilesets, a few seconds after the last change."""
        with self.lock:
            self.pending_tilesets.update(tilesets)
            if self.rebuild_timer is not None:
                self.rebuild_timer.cancel()
            self.rebuild_timer = threading.Timer(3.0, self._rebuild_pending)
            self.rebuild_timer.daemon = True
            self.rebuild_timer.start()

    def _rebuild_pending(self):
        with self.lock:
            tilesets, self.pending_tilesets = self.pending_tilesets, set()
        count = 0
        for info in self.decomp.layouts().values():
            if info["primary_tileset"] in tilesets or info["secondary_tileset"] in tilesets:
                try:
                    self.rebuild(self.layout(info["id"]))
                    count += 1
                except Exception:
                    traceback.print_exc()
        print(f"editor: {count} layouts reconstruídos")

    def annotate(self, name, mids, bottom, top):
        layout = self.layout(name)
        groups = {}
        for mid in mids:
            tileset = layout.primary.name if mid < 512 else layout.secondary.name
            groups.setdefault(tileset, []).append(mid)
        with self.lock:
            for tileset, ids in groups.items():
                edit.set_metatiles(tileset, ids, bottom, top)
        self.rebuild(layout)
        self.rebuild_tilesets_later(groups)

    def set_levels(self, name, cells, level, fill):
        layout = self.layout(name)
        if fill:
            region = []
            for x, y in cells:
                region.extend(edit.region(layout, x, y))
            cells = region
        with self.lock:
            edit.set_levels(layout, cells, level)
        self.rebuild(layout)

    def preview(self, name):
        layout = self.layout(name)
        stops = tour.stops_for(self.decomp, {layout.id})
        if not stops:
            raise ValueError("nenhum mapa usa este layout")
        self.rebuild(layout)
        tour.run(ROOT, stops, TOUR_DIR, verbose=False)

    def play(self, name, x=None, y=None):
        layout = self.layout(name)
        maps = self.maps_by_layout.get(layout.id)
        if not maps:
            raise ValueError("nenhum mapa usa este layout")
        if x is None:
            x, y = tour.spawn_point(self.decomp, layout, maps[0])
        game = os.path.join(ROOT, "pokeemerald64")
        os.makedirs(PLAY_DIR, exist_ok=True)
        env = dict(os.environ)
        env.update({
            "PE_TEST_QUICKSTART": "1",
            "PE_TEST_WARP": f"{maps[0].number},{x},{y}",
            "PE_SAVE": os.path.join(PLAY_DIR, "editor.sav"),
        })
        subprocess.Popen([game], env=env, cwd=PLAY_DIR, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def png_bytes(img):
    buf = io.BytesIO()
    img.save(buf, "PNG")
    return buf.getvalue()


class Handler(BaseHTTPRequestHandler):
    project = None

    def log_message(self, fmt, *args):
        pass

    def send(self, code, body, content_type="application/json"):
        if isinstance(body, (dict, list)):
            body = json.dumps(body).encode("utf-8")
        elif isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        q = {k: v[0] for k, v in urllib.parse.parse_qs(url.query).items()}
        p = self.project
        try:
            if url.path == "/":
                with open(os.path.join(HERE, "editor.html"), "rb") as f:
                    self.send(200, f.read(), "text/html; charset=utf-8")
            elif url.path == "/api/layouts":
                self.send(200, p.layouts())
            elif url.path == "/api/layout":
                self.send(200, p.state(q["name"]))
            elif url.path == "/api/palette":
                self.send(200, p.palette(q["name"], int(q["mid"])))
            elif url.path == "/api/atlas.png":
                self.send(200, p.atlas_png(q["name"]), "image/png")
            elif url.path == "/api/map.png":
                self.send(200, p.map_png(q["name"]), "image/png")
            elif url.path == "/api/overview.png":
                path = os.path.join(TOUR_DIR, q["name"] + ".png")
                if os.path.exists(path):
                    with open(path, "rb") as f:
                        self.send(200, f.read(), "image/png")
                else:
                    self.send(404, {"error": "sem prévia"})
            else:
                self.send(404, {"error": "não encontrado"})
        except Exception as e:
            traceback.print_exc()
            self.send(500, {"error": str(e)})

    def do_POST(self):
        url = urllib.parse.urlparse(self.path)
        length = int(self.headers.get("Content-Length", 0))
        body = json.loads(self.rfile.read(length) or b"{}")
        p = self.project
        try:
            if url.path == "/api/annotate":
                p.annotate(body["name"], body["mids"], body.get("bottom"), body.get("top"))
                self.send(200, p.state(body["name"]))
            elif url.path == "/api/levels":
                p.set_levels(body["name"], body["cells"], body.get("level"), body.get("fill", False))
                self.send(200, p.state(body["name"]))
            elif url.path == "/api/preview":
                started = time.time()
                p.preview(body["name"])
                self.send(200, {"seconds": round(time.time() - started, 1)})
            elif url.path == "/api/play":
                p.play(body["name"], body.get("x"), body.get("y"))
                self.send(200, {"ok": True})
            else:
                self.send(404, {"error": "não encontrado"})
        except Exception as e:
            traceback.print_exc()
            self.send(400, {"error": str(e)})


def serve(decomp, port=8642, open_browser=True):
    Handler.project = Project(decomp)
    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    url = f"http://localhost:{port}/"
    print(f"Editor de mapas em {url} (Ctrl+C para sair)")
    if open_browser:
        threading.Timer(0.5, lambda: webbrowser.open(url)).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
