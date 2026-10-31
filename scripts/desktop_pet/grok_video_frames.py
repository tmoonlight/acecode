"""Key green-screen Grok output; decode its videos into frames (PyAV, imported only for decoding)."""

import numpy as np
from PIL import Image


def background_of(rgb):
    """Median colour of the outer border: the chroma-key green actually rendered (not pure #00FF00)."""
    border = np.concatenate([rgb[:12].reshape(-1, 3), rgb[-12:].reshape(-1, 3),
                             rgb[:, :12].reshape(-1, 3), rgb[:, -12:].reshape(-1, 3)])
    return np.median(border, axis=0).astype(np.float32)


def key(rgb, bg=None):
    """Soft chroma key against the rendered green, with the green unmixed from edge pixels and spill removed."""
    a = rgb.astype(np.float32)
    bg = background_of(rgb) if bg is None else bg
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    spill = g - np.maximum(r, b)
    full = max(float(bg[1] - max(bg[0], bg[2])), 40.0)   # spill of the bare background
    lo, hi = 0.22 * full, 0.65 * full
    alpha = np.clip((hi - spill) / (hi - lo), 0.0, 1.0)
    safe = np.maximum(alpha, 1e-3)[..., None]
    colour = np.where((alpha < 0.98)[..., None], (a - (1.0 - alpha)[..., None] * bg) / safe, a)
    colour = np.clip(colour, 0, 255)
    # Green light bouncing off the screen onto the subject: never greener than its red / blue.
    colour[..., 1] = np.minimum(colour[..., 1], np.maximum(colour[..., 0], colour[..., 2]) + 12.0)
    alpha[alpha < 0.04] = 0.0
    return Image.fromarray(np.dstack([colour, alpha * 255.0]).astype(np.uint8))


def frames(path):
    """[(time in seconds, RGB array)] of every decoded video frame."""
    import av   # only the video path needs PyAV

    out = []
    with av.open(str(path)) as container:
        stream = container.streams.video[0]
        for frame in container.decode(stream):
            out.append((float(frame.pts * stream.time_base), frame.to_ndarray(format="rgb24")))
    return out
