"""Rover's pixel-art base pose: the chosen Codex drawing re-pixelated on the shipped grid.

Codex's "pixel art" is not on an exact grid, so it is keyed, scaled so the dog is DOG_HEIGHT pixels tall and
re-pixelated (pixelate.py, 5x5 video pixels per art pixel). The result is laid out on the video canvas (144x144
art pixels = 720x720 video pixels; the 108x108 sprite box sits bottom-centre, see grok_rover_videos.BOX) and
saved twice: the native sprite (base_pixel.png) and its 5x enlargement on flat green, which every Grok clip
starts and ends on (base_canvas.png).
"""

import argparse
import pathlib
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from grok_rover_videos import CANVAS  # noqa: E402
from grok_video_frames import key  # noqa: E402
from pixelate import build_palette, pixelate, render  # noqa: E402

BLOCK = 5          # video pixels per art pixel
DOG_HEIGHT = 100   # art pixels; the sprite box is 108
BOTTOM = 2         # art pixels between the paws and the bottom of the box


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=pathlib.Path, required=True, help="the chosen drawing on green")
    parser.add_argument("--out", type=pathlib.Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    keyed = key(np.asarray(Image.open(args.image).convert("RGB")))
    dog = keyed.crop(keyed.getchannel("A").getbbox())
    scale = DOG_HEIGHT * BLOCK / dog.size[1]
    dog = dog.resize((round(dog.size[0] * scale), round(dog.size[1] * scale)), Image.LANCZOS)
    layout = Image.new("RGBA", (CANVAS, CANVAS))
    x = (CANVAS - dog.size[0]) // 2 // BLOCK * BLOCK
    y = (CANVAS - dog.size[1] - BOTTOM * BLOCK) // BLOCK * BLOCK
    layout.alpha_composite(dog, (x, y))
    palette = build_palette([layout], 32)
    index, _ = pixelate(layout, BLOCK, palette)
    sprite = render(index, palette)
    sprite.save(args.out / "base_pixel.png")
    np.save(args.out / "base_palette.npy", palette)
    canvas = Image.new("RGBA", (CANVAS, CANVAS), (0, 255, 0, 255))
    canvas.alpha_composite(sprite.resize((CANVAS, CANVAS), Image.NEAREST))
    canvas.convert("RGB").save(args.out / "base_canvas.png")
    print(f"sprite {sprite.size}, dog box {sprite.getchannel('A').getbbox()}")


if __name__ == "__main__":
    main()
