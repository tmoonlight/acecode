"""Rover's pixel-art edge-peek art (eyes open / blinking) from two Codex drawings on green.

The open-eyed drawing was made by Codex from the base sprite (head and front paws coming in from the right
edge), the blink by a Codex edit of it that only closes the eyes. Both are keyed, scaled so the peeking head is
PEEK_HEIGHT art pixels tall (the same pixel size as the frames, so PEEK.rover.ratio stays 1) and re-pixelated
with the base palette; only the eye area of the blink is used, so nothing else moves when he blinks.
"""

import argparse
import pathlib
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from grok_rover_pixel_base import BLOCK  # noqa: E402
from grok_video_frames import key  # noqa: E402
from pixelate import pixelate, render  # noqa: E402

PEEK_HEIGHT = 76   # art pixels: the head peeks in at about the size it has in the sitting pose
EYE_MARGIN = 2


def prepared(path, box, height):
    """Keyed drawing cropped to `box` (its right side is the screen edge), scaled to height*BLOCK pixels and
    padded on the left / top to whole cells."""
    keyed = key(np.asarray(Image.open(path).convert("RGB"))).crop(box)
    width = round(keyed.size[0] * height * BLOCK / keyed.size[1])
    scaled = keyed.resize((width, height * BLOCK), Image.LANCZOS)
    cells = -(-width // BLOCK)
    canvas = Image.new("RGBA", (cells * BLOCK, height * BLOCK))
    canvas.alpha_composite(scaled, (cells * BLOCK - width, 0))
    return canvas


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--open", type=pathlib.Path, required=True)
    parser.add_argument("--blink", type=pathlib.Path, required=True)
    parser.add_argument("--palette", type=pathlib.Path, required=True, help="base_palette.npy")
    parser.add_argument("--out", type=pathlib.Path, required=True, help="assets/desktop_pet/classic/rover/peek.png")
    args = parser.parse_args()
    palette = np.load(args.palette)
    first = key(np.asarray(Image.open(args.open).convert("RGB")))
    left, top, _, bottom = first.getchannel("A").getbbox()
    box = (left, top, first.size[0], bottom)   # keep the cut at the right edge of the drawing
    opened, _ = pixelate(prepared(args.open, box, PEEK_HEIGHT), BLOCK, palette)
    blinked, _ = pixelate(prepared(args.blink, box, PEEK_HEIGHT), BLOCK, palette)
    # Only the eyes blink: the blink is taken inside the box around the open eyes' whites (grown by EYE_MARGIN
    # pixels); the edit also nudged the head's shading, which must not flicker on every blink.
    whites = np.isin(opened, np.nonzero(palette.min(axis=1) > 200)[0])   # white, not light yellow
    whites[int(opened.shape[0] * 0.6):] = False
    ys, xs = np.nonzero(whites)
    eyes = np.zeros_like(whites)
    eyes[max(0, ys.min() - EYE_MARGIN):ys.max() + EYE_MARGIN + 1, max(0, xs.min() - EYE_MARGIN):xs.max() + EYE_MARGIN + 1] = True
    blink = np.where(eyes, blinked, opened)
    open_image, blink_image = render(opened, palette), render(blink, palette)
    sheet = Image.new("RGBA", (open_image.size[0] * 2, open_image.size[1]))
    sheet.paste(open_image, (0, 0))
    sheet.paste(blink_image, (open_image.size[0], 0))
    sheet.save(args.out, optimize=True)
    print(f"peek {open_image.size} per frame -> {args.out}")


if __name__ == "__main__":
    main()
