"""Pictures of the 3D field, taken by the game itself.

The game runs without a window (PE_HEADLESS), starts a new game straight on
the field (PE_TEST_QUICKSTART) and warps through a list of maps
(PE_TEST_TOUR). On each map it renders the whole layout from the game
camera's angle, orthographic, and saves <out>/<layout name>.png.
"""

import os
import subprocess
import tempfile

from decomp import unpack_block, behavior_of

WATERY = ("WATER", "CURRENT", "SEAWEED", "NO_SURFACING", "WATERFALL")


def spawn_point(decomp, layout, map_info):
    """A walkable, dry cell near the middle of the map (the camera follows
    the player, and people only appear near it)."""
    behaviors = decomp.behaviors()
    cx, cy = layout.width / 2, layout.height / 2
    best = None
    for y in range(layout.height):
        for x in range(layout.width):
            mid, collision, _ = unpack_block(layout.blocks[y * layout.width + x])
            if collision:
                continue
            name = behaviors.get(behavior_of(layout.metatile_attribute(mid)), "")
            penalty = 1000 if any(w in name for w in WATERY) else 0
            score = (x - cx) ** 2 + (y - cy) ** 2 + penalty
            if best is None or score < best[0]:
                best = (score, x, y)
    if best is not None:
        return best[1], best[2]
    if map_info.warps:
        return map_info.warps[0]
    return int(cx), int(cy)


def dry_cells(decomp, layout):
    behaviors = decomp.behaviors()
    out = []
    for y in range(layout.height):
        for x in range(layout.width):
            mid, collision, _ = unpack_block(layout.blocks[y * layout.width + x])
            name = behaviors.get(behavior_of(layout.metatile_attribute(mid)), "")
            if not collision and not any(w in name for w in WATERY):
                out.append((x, y))
    return out


def survey_points(decomp, layout, map_info, count):
    """count spots spread over the walkable ground (farthest-point sampling)."""
    cells = dry_cells(decomp, layout)
    if not cells:
        return [spawn_point(decomp, layout, map_info)]
    points = [spawn_point(decomp, layout, map_info)]
    while len(points) < count:
        best = max(cells, key=lambda c: min((c[0] - p[0]) ** 2 + (c[1] - p[1]) ** 2 for p in points))
        if min((best[0] - p[0]) ** 2 + (best[1] - p[1]) ** 2 for p in points) < 36:
            break  # small map: no spot far enough from the others
        points.append(best)
    return points


def maps_by_layout(decomp):
    out = {}
    for m in decomp.maps().values():
        if m.number >= 0:
            out.setdefault(m.layout_id, m)
    return out


# Needs the wireless adapter (the PC port has none, so players never get in)
SKIP_LAYOUTS = {"LAYOUT_UNION_ROOM"}


def stops_for(decomp, wanted=None):
    """One map per layout: (map number, x, y, layout name)."""
    maps_by_layout = {}
    for m in decomp.maps().values():
        if m.number >= 0:
            maps_by_layout.setdefault(m.layout_id, m)
    stops = []
    for layout_id, info in decomp.layouts().items():
        if wanted and layout_id not in wanted and info["name"] not in wanted:
            continue
        m = maps_by_layout.get(layout_id)
        if m is None or (layout_id in SKIP_LAYOUTS and not wanted):
            continue
        layout = decomp.layout(layout_id)
        x, y = spawn_point(decomp, layout, m)
        stops.append((m.number, x, y, info["name"]))
    return stops


def _run_once(root, stops, out_dir, pixels_per_block, settle, game, shots, env_extra=None):
    with tempfile.TemporaryDirectory() as tmp:
        tour = os.path.join(tmp, "tour.txt")
        with open(tour, "w") as f:
            for stop in stops:
                f.write(" ".join(str(v) for v in stop) + "\n")
        env = dict(os.environ)
        env.update({
            "PE_HEADLESS": "1",
            "PE_RENDERER": "gl",
            "PE_TEST_QUICKSTART": "1",
            "PE_TEST_TOUR": tour,
            "PE_TEST_TOUR_PPB": str(pixels_per_block),
            "PE_TEST_TOUR_SETTLE": str(settle),
            "PE_FRAMES": str(600 + len(stops) * (settle + 10)),
            "PE_DUMP_DIR": os.path.abspath(out_dir),
            "PE_SAVE": os.path.join(tmp, "tour.sav"),
            "PE_HDMON": "0",
            "PE_SCALE": "4",
            "PE_TEST_HOUR": "12",  # pictures by day, whatever the clock says
            "PE_TEST_TOUR_SHOTS": "1" if shots else "0",
        })
        env.update(env_extra or {})
        return subprocess.run([game], env=env, cwd=tmp, capture_output=True, text=True)


def run(root, stops, out_dir, pixels_per_block=16, settle=90, game=None, verbose=True, shots=False, env_extra=None):
    """Runs the game over the stops; returns the pictures that came out.
    A map that crashes the game (link rooms and the like) is skipped and the
    tour goes on from the next one."""
    os.makedirs(out_dir, exist_ok=True)
    game = game or os.path.join(root, "pokeemerald64")
    if not os.path.exists(game):
        raise SystemExit(f"{game} não existe; compile com hd2d/scripts/build-linux.sh")

    def picture(name):
        game_only = False
        for stop in stops:
            if stop[3] == name and len(stop) > 4 and "o" not in stop[4]:
                game_only = True
        return os.path.join(out_dir, name + ("_game.png" if game_only else ".png"))

    for name in [stop[3] for stop in stops]:
        if os.path.exists(picture(name)):
            os.remove(picture(name))
    pending = list(stops)
    while pending:
        log = _run_once(root, pending, out_dir, pixels_per_block, settle, game, shots, env_extra)
        if verbose:
            for line in log.stdout.splitlines():
                if line.startswith(("test: tour", "overview:")):
                    print(line)
        missing = [stop for stop in pending if not os.path.exists(picture(stop[3]))]
        if not missing or log.returncode in (0, 2):
            break
        if verbose:
            print(f"o jogo parou (código {log.returncode}) em {missing[0][3]}; pulando")
        pending = missing[1:]

    # the game writes uncompressed PNGs
    from PIL import Image
    done = []
    for stop in stops:
        name = stop[3]
        if os.path.exists(picture(name)):
            Image.open(picture(name)).save(picture(name), optimize=True)
            done.append(picture(name))
        game_shot = os.path.join(out_dir, name + "_game.png")
        if os.path.exists(game_shot):
            Image.open(game_shot).save(game_shot, optimize=True)
    return done


def survey(root, decomp, layout_ids, out_dir, per_map=4, verbose=True, settle=200):
    """Game-camera pictures at a few spots of each layout, no blur, put
    together as <out>/<layout>_survey.png."""
    from PIL import Image
    by_layout = maps_by_layout(decomp)
    stops, groups = [], {}
    for layout_id in layout_ids:
        m = by_layout.get(layout_id)
        if m is None:
            continue
        layout = decomp.layout(layout_id)
        names = []
        for k, (x, y) in enumerate(survey_points(decomp, layout, m, per_map)):
            name = f"{layout.name}@{k}"
            stops.append((m.number, x, y, name, "g"))
            names.append(name)
        groups[layout.name] = names
    # settle long enough for the map name sign to go away
    run(root, stops, out_dir, settle=settle, verbose=verbose,
        env_extra={"PE_DOF": "0", "PE_SCALE": "2", "PE_PARTICLES": "0"})
    done = []
    for layout_name, names in groups.items():
        shots = [os.path.join(out_dir, n + "_game.png") for n in names]
        shots = [p for p in shots if os.path.exists(p)]
        if not shots:
            continue
        images = [Image.open(p) for p in shots]
        w, h = images[0].size
        cols = 2 if len(images) > 1 else 1
        rows = (len(images) + cols - 1) // cols
        sheet = Image.new("RGB", (cols * w, rows * h))
        for i, im in enumerate(images):
            sheet.paste(im, ((i % cols) * w, (i // cols) * h))
        path = os.path.join(out_dir, layout_name + "_survey.png")
        sheet.save(path, optimize=True)
        for p in shots:
            os.remove(p)
        done.append(path)
    return done
