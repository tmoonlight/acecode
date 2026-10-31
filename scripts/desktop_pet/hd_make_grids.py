"""Step 1: lay the frames that need redrawing out as reference grids for Codex image generation."""

import argparse
import json
import pathlib

from PIL import Image

from hd_pets_common import PETS, load_agent, near_duplicates, tile_order, used_animations


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=pathlib.Path, required=True, help="assets/desktop_pet")
    parser.add_argument("--work", type=pathlib.Path, required=True, help="working directory for grids")
    args = parser.parse_args()
    for kind, spec in PETS.items():
        folder = args.root / spec["dir"]
        agent = load_agent(folder / "agent.js")
        names = used_animations(args.root, kind, agent)
        sheet = Image.open(folder / "map.png").convert("RGBA")
        fw, fh = agent["framesize"]
        reps, owner = near_duplicates(sheet, agent["framesize"], tile_order(agent, names))
        k, cols, rows = spec["scale"], spec["cols"], spec["rows"]
        per = cols * rows
        out = args.work / kind
        out.mkdir(parents=True, exist_ok=True)
        # Re-running keeps the grids already laid out (and redrawn); only frames they miss get new grids.
        previous = out / "manifest.json"
        grids = json.loads(previous.read_text(encoding="utf-8"))["grids"] if previous.exists() else []
        covered = {tuple(cell) for grid in grids for cell in grid["cells"]}
        missing = [rep for rep in reps if rep not in covered]
        for g in range(0, len(missing), per):
            cells = missing[g:g + per]
            image = Image.new("RGB", (cols * fw * k, rows * fh * k), (0, 255, 0))
            for i, (x, y) in enumerate(cells):
                tile = sheet.crop((x, y, x + fw, y + fh)).resize((fw * k, fh * k), spec["ref_resample"])
                cell = Image.new("RGBA", tile.size, (0, 255, 0, 255))
                cell.alpha_composite(tile)
                image.paste(cell.convert("RGB"), ((i % cols) * fw * k, (i // cols) * fh * k))
            name = f"grid{len(grids):03d}.png"
            image.save(out / name)
            grids.append({"file": name, "cells": [list(c) for c in cells]})
        manifest = {"kind": kind, "framesize": [fw, fh], "scale": k, "cols": cols, "rows": rows,
                    "animations": names, "owner": {f"{a[0]},{a[1]}": list(b) for a, b in owner.items()},
                    "grids": grids}
        (out / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        print(f"{kind}: {len(names)} animations, {len(owner)} frames, {len(reps)} distinct, {len(grids)} grids "
              f"({len(missing)} frames newly laid out)")


if __name__ == "__main__":
    main()
