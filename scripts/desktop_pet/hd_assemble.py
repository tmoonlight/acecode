"""Step 3: build the HD sheets (WebP, in playback order per animation) and the frame table the pet player loads."""

import argparse
import json
import pathlib
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from hd_frames import fit_grid  # noqa: E402
from hd_pets_common import PETS, load_agent, used_animations  # noqa: E402

SHEET_WIDTH = 2048
SHEET_HEIGHT = 2048   # about 4 megapixels per sheet
GAP = 4   # transparent gutter, so smoothed (mipmapped) sampling never picks up a neighbour


def resize_premultiplied(image, size):
    a = np.asarray(image, np.float32)
    pm = a.copy()
    pm[..., :3] *= a[..., 3:4] / 255.0
    channels = [np.asarray(Image.fromarray(pm[..., c]).resize(size, Image.LANCZOS), np.float32) for c in range(4)]
    w = np.dstack(channels)
    alpha = np.clip(w[..., 3], 0, 255)
    colour = np.where(alpha[..., None] > 0.5, w[..., :3] * 255.0 / np.maximum(alpha[..., None], 1e-3), 0)
    alpha[alpha < 3] = 0
    return Image.fromarray(np.dstack([np.clip(colour, 0, 255), alpha]).astype(np.uint8))


def pack(items):
    """Shelf-pack (key, image) pairs in playback order into sheets of at most SHEET_WIDTH x SHEET_HEIGHT.

    Keeping the order means a playing clip walks through its sheets one after another, so the player
    only needs the few sheets around the playhead decoded. Returns [(sheet image, {key: (x, y)})].
    """
    sheets, spots = [], {}
    x = y = shelf = width = 0

    def close():
        sheet = Image.new("RGBA", (width, y + shelf), (0, 0, 0, 0))
        for key, (sx, sy) in spots.items():
            sheet.paste(images[key], (sx, sy))
        sheets.append((sheet, dict(spots)))

    images = dict(items)
    for key, image in items:
        w, h = image.size
        if x and x + w > SHEET_WIDTH:
            x, y, shelf = 0, y + shelf + GAP, 0
        if spots and y + h > SHEET_HEIGHT:
            y -= GAP if x == 0 else 0
            close()
            spots.clear()
            x = y = shelf = width = 0
        spots[key] = (x, y)
        x += w + GAP
        shelf = max(shelf, h)
        width = max(width, x - GAP)
    if spots:
        close()
    return sheets


def assemble(root, work, kind, quality):
    spec = PETS[kind]
    folder = root / spec["dir"]
    agent = load_agent(folder / "agent.js")
    manifest = json.loads((work / kind / "manifest.json").read_text(encoding="utf-8"))
    sheet = Image.open(folder / "map.png").convert("RGBA")
    fw, fh = manifest["framesize"]
    out_scale = spec.get("output_scale", manifest["scale"])
    size = (round(fw * out_scale), round(fh * out_scale))
    hd = {}
    for grid in manifest["grids"]:
        path = work / kind / "out" / grid["file"]
        if not path.exists():
            continue
        with Image.open(path) as output:
            for xy, frame, *_ in fit_grid(output.convert("RGB"), manifest, grid, sheet):
                hd[xy] = frame if frame.size == size else resize_premultiplied(frame, size)
    owner = {tuple(map(int, key.split(","))): tuple(value) for key, value in manifest["owner"].items()}
    names = used_animations(root, kind, agent)
    frames, missing = {}, set()
    for tile, rep in owner.items():
        picture = hd.get(tile) or hd.get(rep)
        if picture is None:
            missing.add(tile)
            continue
        box = picture.getchannel("A").getbbox()
        if not box:
            frames[tile] = None
            continue
        box = (max(0, box[0] - 1), max(0, box[1] - 1), min(size[0], box[2] + 1), min(size[1], box[3] + 1))
        frames[tile] = (picture.crop(box), box[0], box[1])
    # Animations whose frames are not all redrawn yet are left out; the player skips them.
    playable = [name for name in names
                if all(tuple(xy) in frames for f in agent["animations"][name]["frames"] for xy in f.get("images") or [])]
    out = folder / "hd"
    out.mkdir(exist_ok=True)
    for old in out.glob("sheet-*.webp"):
        old.unlink()
    table, groups, sheets, assigned = {}, {}, [], set()
    for name in playable:
        tiles = []
        for f in agent["animations"][name]["frames"]:
            for xy in f.get("images") or []:
                xy = tuple(xy)
                if xy not in assigned and xy not in tiles:
                    tiles.append(xy)
        drawn = [(xy, frames[xy][0]) for xy in tiles if frames[xy] is not None]
        for xy in tiles:
            assigned.add(xy)
            if frames[xy] is None:
                table[f"{xy[0]},{xy[1]}"] = None
        for image, spots in pack(drawn):
            index = len(sheets)
            image.save(out / f"sheet-{index:02d}.webp", "WEBP", quality=quality, method=6)
            sheets.append(list(image.size))
            for xy, (sx, sy) in spots.items():
                _, dx, dy = frames[xy]
                table[f"{xy[0]},{xy[1]}"] = [index, sx, sy, *frames[xy][0].size, dx, dy]
    for name in playable:
        used = {table[f"{xy[0]},{xy[1]}"][0] for f in agent["animations"][name]["frames"]
                for xy in f.get("images") or [] if table[f"{xy[0]},{xy[1]}"]}
        groups[name] = sorted(used)
    data = {"framesize": list(size), "sheets": sheets, "animations": groups, "frames": table}
    (out / "frames.js").write_text(
        "// Generated by scripts/desktop_pet/hd_assemble.py: high-resolution frames redrawn with Codex image\n"
        "// generation, keyed by the original sprite offsets in agent.js; sheets follow playback order.\n"
        f"(window.ClassicPetHD = window.ClassicPetHD || {{}}).{kind} = {json.dumps(data, separators=(',', ':'))};\n",
        encoding="utf-8")
    total = sum((out / f"sheet-{i:02d}.webp").stat().st_size for i in range(len(sheets)))
    pixels = sum(w * h for w, h in sheets)
    print(f"{kind}: {len(playable)}/{len(names)} animations, {len(frames)} frames, {len(sheets)} sheets, "
          f"{total / 1e6:.1f} MB, {pixels / 1e6:.1f} Mpx, largest sheet {max(sheets, key=lambda s: s[0] * s[1])}, "
          f"missing {len(missing)}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=pathlib.Path, required=True, help="assets/desktop_pet")
    parser.add_argument("--work", type=pathlib.Path, required=True)
    parser.add_argument("--only", default="")
    parser.add_argument("--quality", type=int, default=75)
    args = parser.parse_args()
    for kind, spec in PETS.items():
        if spec.get("replaced_by"):
            print(f"{kind}: skipped, its frames come from {spec['replaced_by']}")
        elif not args.only or kind in args.only.split(","):
            assemble(args.root, args.work, kind, args.quality)


if __name__ == "__main__":
    main()
