#!/usr/bin/env python3
"""Map converter for the HD-2D field.

Commands:
  build [layouts...] [--out DIR]             placement tables the game loads
                                             (all layouts by default)
  render2d <layout> <out.png> [--margin N]   flat render, like the GBA
  sheet <layout> <out.png> [--secondary]     metatile sheet with ids
  inspect <layout>... --sheet out.png        print the block grid (id, # blocked)
                                             and a sheet of the metatiles used
  tour [layouts...] [--out DIR] [--ppb N]    the game takes a picture of each layout
                                             in 3D (default: all, hd2d/out/tour)
  survey [layouts...] [--out DIR] [--per N]  game-camera pictures at N spots of each layout,
                                             <layout>_survey.png (default hd2d/out/survey)
  cells <layout> <out.png> [--box x0,y0,x1,y1] [--scale N]
                                             the layout with each cell's metatile id
                                             (yellow: annotated), placement and level
  set <tileset> <ids> <bottom> <top>         annotate metatiles, e.g.
                                             set gTileset_General 1D4-1D7,1E0 "face" "card"
  unset <tileset> <ids>                      back to the automatic rules
  level <layout> <level|auto> fill X,Y       hand-set the level of the walkable area
  level <layout> <level|auto> rect X0,Y0,X1,Y1   ... or of a rectangle (inclusive)
  particles <layout> [theme|auto]            show or set the layout's particle theme
                                             (none meadow forest cave indoor embers underwater ocean)
  status [tilesets...]                       how many used metatiles are annotated
  placements <tileset> <out.png> [--range A-B]  sheet of the metatiles of a tileset
                                             that layouts use, with their placements
  editor [--port N]                          the map editor, in the browser

Layouts are given by name or id (LittlerootTown_Layout, LAYOUT_ROUTE101).
Run from anywhere; --root points at the pokeemerald checkout (default: the
one this script lives in).
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from decomp import Decomp  # noqa: E402
import placement  # noqa: E402
import render2d  # noqa: E402
import tour  # noqa: E402
import edit  # noqa: E402

DEFAULT_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
HD2D_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def stores():
    annotations = placement.Annotations(os.path.join(HD2D_DIR, "mapdata", "tilesets"))
    levels = placement.LayoutLevels(os.path.join(HD2D_DIR, "mapdata", "layouts"))
    return annotations, levels


def layout_state(decomp, layout, analyses=None):
    """Placements (bottom, top Specs per metatile), which are hand-made, and
    the levels (quarter blocks) of a layout, as the build would make them."""
    annotations, levels = stores()
    analyses = analyses or placement.Analyses(decomp, render2d.MetatileRenderer)
    analysis = analyses.for_layout(layout)
    placements, manual = placement.resolve_placements(layout, annotations, analysis)
    return placements, manual, placement.compute_levels(layout, placements, levels.load(layout), None,
                                                        placement.terrain_faces(analysis, placements))


def layout_number(decomp, layout):
    return list(decomp.layouts()).index(layout.id) + 1


def build(decomp, names, out_dir):
    annotations, levels = stores()
    analyses = placement.Analyses(decomp, render2d.MetatileRenderer)
    os.makedirs(out_dir, exist_ok=True)
    ids = list(decomp.layouts())  # layouts.json order: gMapLayouts[mapLayoutId - 1]
    wanted = set(names) if names else None
    count = 0
    for index, layout_id in enumerate(ids):
        info = decomp.layouts()[layout_id]
        if wanted and layout_id not in wanted and info["name"] not in wanted:
            continue
        layout = decomp.layout(layout_id)
        data, _, _ = placement.build_layout_table(decomp, layout, index + 1, annotations, analyses, levels)
        path = os.path.join(out_dir, f"layout_{index + 1}.fld")
        with open(path + ".tmp", "wb") as f:
            f.write(data)
        os.replace(path + ".tmp", path)  # the game may be reading it
        count += 1
    print(f"{count} layouts escritos em {out_dir}")


def print_status(decomp, wanted):
    from decomp import unpack_block
    annotations, _ = stores()
    used = {}
    cells = {}
    for layout_id in decomp.layouts():
        layout = decomp.layout(layout_id)
        for b in layout.blocks:
            mid = unpack_block(b)[0]
            ts = layout.primary.name if mid < 512 else layout.secondary.name
            used.setdefault(ts, set()).add(mid)
            cells[ts] = cells.get(ts, 0) + 1
    print(f"{'tileset':38} {'usados':>6} {'anotados':>8}  células")
    for ts in sorted(used, key=lambda t: -cells[t]):
        if wanted and ts not in wanted:
            continue
        table, _ = annotations.load(ts)
        done = len([m for m in used[ts] if m in table])
        print(f"{ts:38} {len(used[ts]):6} {done:8}  {cells[ts]}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--root", default=DEFAULT_ROOT, help="pokeemerald checkout")
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("build")
    p.add_argument("layouts", nargs="*")
    p.add_argument("--out", default=os.path.join(HD2D_DIR, "data", "field"))

    p = sub.add_parser("render2d")
    p.add_argument("layout")
    p.add_argument("out")
    p.add_argument("--margin", type=int, default=0)

    p = sub.add_parser("sheet")
    p.add_argument("layout")
    p.add_argument("out")
    p.add_argument("--secondary", action="store_true")

    p = sub.add_parser("inspect")
    p.add_argument("layouts", nargs="+")
    p.add_argument("--sheet")

    p = sub.add_parser("survey")
    p.add_argument("layouts", nargs="*")
    p.add_argument("--out", default=os.path.join(HD2D_DIR, "out", "survey"))
    p.add_argument("--per", type=int, default=4)

    p = sub.add_parser("cells")
    p.add_argument("layout")
    p.add_argument("out")
    p.add_argument("--box")
    p.add_argument("--scale", type=int, default=3)

    p = sub.add_parser("set")
    p.add_argument("tileset")
    p.add_argument("ids")
    p.add_argument("bottom")
    p.add_argument("top")

    p = sub.add_parser("unset")
    p.add_argument("tileset")
    p.add_argument("ids")

    p = sub.add_parser("level")
    p.add_argument("layout")
    p.add_argument("value")
    p.add_argument("how", choices=["fill", "rect"])
    p.add_argument("where")

    p = sub.add_parser("placements")
    p.add_argument("tileset")
    p.add_argument("out")
    p.add_argument("--range")
    p.add_argument("--columns", type=int, default=12)

    p = sub.add_parser("particles")
    p.add_argument("layout")
    p.add_argument("theme", nargs="?")

    p = sub.add_parser("status")
    p.add_argument("tilesets", nargs="*")

    p = sub.add_parser("editor")
    p.add_argument("--port", type=int, default=8642)
    p.add_argument("--no-browser", action="store_true")

    p = sub.add_parser("tour")
    p.add_argument("layouts", nargs="*")
    p.add_argument("--out", default=os.path.join(HD2D_DIR, "out", "tour"))
    p.add_argument("--ppb", type=int, default=16, help="pixels per block")
    p.add_argument("--settle", type=int, default=90, help="frames to wait on each map")
    p.add_argument("--shots", action="store_true", help="also save the game screen (<layout>_game.png)")

    args = parser.parse_args()
    decomp = Decomp(args.root)

    if args.command == "build":
        build(decomp, args.layouts, args.out)
    elif args.command == "render2d":
        render2d.render_layout(decomp.layout(args.layout), args.margin).save(args.out)
    elif args.command == "inspect":
        from decomp import unpack_block
        layouts = [decomp.layout(name) for name in args.layouts]
        for layout in layouts:
            print(f"{layout.name} {layout.width}x{layout.height} "
                  f"{layout.primary.name} + {layout.secondary.name}")
            print("    " + " ".join(f"{x:<4}" for x in range(layout.width)))
            for y in range(layout.height):
                cells = []
                for x in range(layout.width):
                    mid, collision, _ = unpack_block(layout.block(x, y))
                    cells.append(f"{mid:03X}{'#' if collision else ' '}")
                print(f"{y:3} " + " ".join(cells))
        if args.sheet:
            render2d.render_used_sheet(layouts).save(args.sheet)
    elif args.command == "survey":
        wanted = set(args.layouts)
        ids = [i for i, info in decomp.layouts().items()
               if not wanted or i in wanted or info["name"] in wanted]
        pictures = tour.survey(args.root, decomp, ids, args.out, args.per)
        print(f"{len(pictures)} imagens em {args.out}")
    elif args.command == "cells":
        layout = decomp.layout(args.layout)
        placements, manual, levels = layout_state(decomp, layout)
        box = tuple(int(v) for v in args.box.split(",")) if args.box else None
        render2d.render_cells(layout, placements, manual, levels, box, args.scale).save(args.out)
    elif args.command in ("set", "unset"):
        ids = edit.parse_id_list(args.ids)
        if args.command == "set":
            edit.set_metatiles(args.tileset, ids, args.bottom, args.top)
        else:
            edit.set_metatiles(args.tileset, ids, None, None)
        print(f"{len(ids)} metatiles em {args.tileset}")
    elif args.command == "level":
        layout = decomp.layout(args.layout)
        value = None if args.value == "auto" else int(args.value)
        coords = [int(v) for v in args.where.split(",")]
        if args.how == "fill":
            cells = edit.region(layout, coords[0], coords[1])
        else:
            x0, y0, x1, y1 = coords
            cells = [(x, y) for y in range(min(y0, y1), max(y0, y1) + 1) for x in range(min(x0, x1), max(x0, x1) + 1)]
        edit.set_levels(layout, cells, value)
        print(f"{len(cells)} células")
    elif args.command == "placements":
        from decomp import unpack_block
        ids, sample = set(), None
        for layout_id, info in decomp.layouts().items():
            if args.tileset not in (info["primary_tileset"], info["secondary_tileset"]):
                continue
            layout = decomp.layout(layout_id)
            is_primary = info["primary_tileset"] == args.tileset
            for b in layout.blocks:
                mid = unpack_block(b)[0]
                if (mid < 512) == is_primary:
                    ids.add(mid)
            # a layout of the pair used most, so secondary palettes are right
            if sample is None or (not is_primary and layout.width * layout.height > sample.width * sample.height):
                sample = layout
        if args.range:
            lo, hi = (int(v, 16) for v in args.range.split("-"))
            ids = {m for m in ids if lo <= m <= hi}
        placements, manual, _ = layout_state(decomp, sample)
        render2d.render_placement_sheet(sample, placements, manual, sorted(ids), args.columns).save(args.out)
        print(f"{len(ids)} metatiles (paleta de {sample.name})")
    elif args.command == "particles":
        layout = decomp.layout(args.layout)
        _, level_store = stores()
        if args.theme:
            if args.theme != "auto" and args.theme not in placement.PARTICLE_THEMES:
                raise SystemExit(f"tema desconhecido: {args.theme} ({' '.join(placement.PARTICLE_THEMES)})")
            level_store.set_option(layout, "particles", None if args.theme == "auto" else args.theme)
        theme, manual = placement.particle_theme(decomp, layout, level_store.options(layout))
        print(f"{layout.name}: {theme}{' (à mão)' if manual else ' (automático)'}")
    elif args.command == "status":
        print_status(decomp, args.tilesets)
    elif args.command == "editor":
        import editor
        editor.serve(decomp, args.port, not args.no_browser)
    elif args.command == "tour":
        stops = tour.stops_for(decomp, set(args.layouts) if args.layouts else None)
        print(f"{len(stops)} mapas")
        pictures = tour.run(args.root, stops, args.out, args.ppb, args.settle, shots=args.shots)
        print(f"{len(pictures)} imagens em {args.out}")
    elif args.command == "sheet":
        render2d.render_tileset_sheet(decomp.layout(args.layout), args.secondary).save(args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
