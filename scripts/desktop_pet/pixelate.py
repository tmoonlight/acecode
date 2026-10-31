"""Turn keyed high-resolution artwork into crisp pixel art on a fixed grid and palette.

Each BLOCKxBLOCK cell of the source becomes one pixel: transparent unless at least half of it is opaque,
otherwise the dark outline colour when that covers a good part of the cell (keeps one-pixel outlines), else
the most common palette colour. One palette is shared by every frame, so colours never flicker.
"""

import numpy as np
from PIL import Image

OUTLINE_SHARE = 0.3   # an outline that covers this much of a cell wins over the fill
OUTLINE_LUMA = 70


def build_palette(images, colours):
    """Palette (RGB rows) over the opaque pixels of the given RGBA images.

    Octree keeps small but important colours (the red collar, white of the eyes) that median cut merges away.
    """
    pixels = np.concatenate([np.asarray(im)[np.asarray(im)[..., 3] > 200][:, :3] for im in images])
    strip = Image.fromarray(pixels.reshape(1, -1, 3).astype(np.uint8))
    quantized = strip.quantize(colors=colours, method=Image.Quantize.FASTOCTREE)
    palette = np.array(quantized.getpalette()[:colours * 3], np.float32).reshape(-1, 3)
    return palette


def nearest(rgb, palette):
    """Palette index of every pixel (H x W)."""
    flat = rgb.reshape(-1, 3).astype(np.float32)
    best = np.zeros(len(flat), np.int32)
    best_d = np.full(len(flat), np.inf, np.float32)
    for i, colour in enumerate(palette):
        d = ((flat - colour) ** 2).sum(axis=1)
        closer = d < best_d
        best[closer], best_d[closer] = i, d[closer]
    return best.reshape(rgb.shape[:2])


def pixelate(image, block, palette, previous=None, means=None, hold=7.0):
    """RGBA image (size divisible by block) -> (index map with -1 = transparent, block mean colours).

    previous / means: last frame's result; cells whose mean colour moved less than `hold` keep their old
    pixel, so video noise does not make still parts shimmer.
    """
    a = np.asarray(image, np.float32)
    h, w = a.shape[0] // block, a.shape[1] // block
    a = a[:h * block, :w * block]
    cells = a.reshape(h, block, w, block, 4).transpose(0, 2, 1, 3, 4).reshape(h, w, block * block, 4)
    opaque = cells[..., 3] > 127
    share = opaque.mean(axis=2)
    index = nearest(cells[..., :3].reshape(-1, 3).reshape(h, w * block * block, 3), palette).reshape(h, w, block * block)
    luma = palette @ np.array([0.299, 0.587, 0.114], np.float32)
    outline = int(np.argmin(luma))
    result = np.full((h, w), -1, np.int32)
    counts = np.zeros((h, w, len(palette)), np.int32)
    for k in range(len(palette)):
        counts[..., k] = ((index == k) & opaque).sum(axis=2)
    majority = counts.argmax(axis=2)
    dark = counts[..., outline] >= OUTLINE_SHARE * block * block
    result = np.where(share >= 0.5, np.where(dark & (luma[outline] < OUTLINE_LUMA), outline, majority), -1)
    mean = np.where(opaque[..., None], cells[..., :3], 0).sum(axis=2) / np.maximum(opaque.sum(axis=2), 1)[..., None]
    if previous is not None:
        steady = (np.abs(mean - means).max(axis=2) < hold) & ((result >= 0) == (previous >= 0))
        result = np.where(steady, previous, result)
    return result, mean


def render(index, palette):
    """Index map -> RGBA pixel-art image (one image pixel per art pixel)."""
    rgba = np.zeros(index.shape + (4,), np.uint8)
    visible = index >= 0
    rgba[visible, :3] = palette[index[visible]].astype(np.uint8)
    rgba[visible, 3] = 255
    return Image.fromarray(rgba)
