"""Redraw Clippy's edge-peek art (two frames: eyes open / blinking) at the HD frame scale.

Whale Girl's peek art is already drawn at high resolution and is kept; only its size ratio changes. Rover's
is made with Grok (grok_rover_peek.py).
"""

import argparse
import json
import pathlib
import sys

import numpy as np
from PIL import Image, ImageFilter

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from hd_assemble import resize_premultiplied  # noqa: E402
from hd_codex_redraw import redraw  # noqa: E402
from hd_frames import fit_grid  # noqa: E402
from hd_pets_common import PETS  # noqa: E402

PROMPT = (
    "Use your built-in image generation tool to create ONE image. The FIRST attached image holds two frames side "
    "by side of a desktop pet character peeking in from the RIGHT edge of each frame, as if hiding behind the edge "
    "of the screen: the left frame has the eyes open, the right frame is the same picture blinking (eyes closed). "
    "Each frame is a small, low-resolution sprite enlarged for reference. Redraw both frames as clean "
    "high-resolution artwork in the SAME layout: each frame stays in its half, the character keeps exactly the same "
    "pose, position, size and colours, and stays cut off by a perfectly straight vertical line at the right edge "
    "of each frame exactly where the reference cuts it. The two frames must be identical except for the eyes. "
    "The SECOND attached image is the approved high-resolution design of this character: match its rendering "
    "style, materials and details exactly. Smooth anti-aliased edges, crisp details, no pixelation. No text, "
    "borders or shadows on the background; the background must be perfectly flat pure #00FF00 green. "
    "Character: {describe}"
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--codex", required=True)
    parser.add_argument("--root", type=pathlib.Path, required=True, help="assets/desktop_pet")
    parser.add_argument("--work", type=pathlib.Path, required=True)
    args = parser.parse_args()
    for kind in ("clippy",):   # Rover's peek art: grok_rover_peek.py
        spec = PETS[kind]
        folder = args.root / spec["dir"]
        source = folder / "peek.png"
        original = args.work / kind / "peek-original.png"
        if not original.exists():  # keep the sprite-resolution art as the reference for later runs
            original.write_bytes(source.read_bytes())
        sheet = Image.open(original).convert("RGBA")
        fw, fh = sheet.size[0] // 2, sheet.size[1]
        k = spec["scale"]
        work = args.work / f"{kind}-peek"
        work.mkdir(exist_ok=True)
        (work / "anchor.png").write_bytes((args.work / kind / "anchor.png").read_bytes())
        ref = Image.new("RGB", (2 * fw * k, fh * k), (0, 255, 0))
        for i in range(2):
            tile = sheet.crop((i * fw, 0, (i + 1) * fw, fh)).resize((fw * k, fh * k), Image.NEAREST)
            cell = Image.new("RGBA", tile.size, (0, 255, 0, 255))
            cell.alpha_composite(tile)
            ref.paste(cell.convert("RGB"), (i * fw * k, 0))
        ref.save(work / "peek.png")
        manifest = {"framesize": [fw, fh], "scale": k, "cols": 2, "rows": 1}
        grid = {"file": "peek.png", "cells": [[0, 0], [fw, 0]]}
        if not redraw(args.codex, args.work, f"{kind}-peek", manifest, grid, sheet, 3,
                      PROMPT.format(describe=spec["describe"])):
            print(f"{kind}: peek redraw failed")
            continue
        with Image.open(work / "out" / "peek.png") as output:
            (_, opened, *_), (_, closed, *_) = fit_grid(output.convert("RGB"), manifest, grid, sheet)
        out_scale = spec.get("output_scale", k)
        size = (round(fw * out_scale), round(fh * out_scale))
        if opened.size != size:
            opened, closed = resize_premultiplied(opened, size), resize_premultiplied(closed, size)
        # Blinking only changes the eyes: outside them both frames share the open-eyed pixels exactly,
        # so the rest of the picture never twitches when the pet blinks.
        diff = np.any(np.asarray(sheet.crop((0, 0, fw, fh))) != np.asarray(sheet.crop((fw, 0, 2 * fw, fh))), axis=2)
        mask = Image.fromarray((diff * 255).astype(np.uint8)).filter(ImageFilter.MaxFilter(5))
        mask = mask.resize(size, Image.BILINEAR).filter(ImageFilter.GaussianBlur(out_scale * 0.8))
        m = np.asarray(mask, np.float32)[..., None] / 255.0
        blink = (np.asarray(closed, np.float32) * m + np.asarray(opened, np.float32) * (1 - m)).astype(np.uint8)
        result = Image.new("RGBA", (size[0] * 2, size[1]))
        result.paste(opened, (0, 0))
        result.paste(Image.fromarray(blink), (size[0], 0))
        result.save(source, optimize=True)
        print(f"{kind}: peek {result.size}, {source.stat().st_size / 1e3:.0f} KB")


if __name__ == "__main__":
    main()
