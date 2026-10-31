"""Turn a Codex grid redraw (pure green background) back into frames aligned with the original sprites."""

import numpy as np
from PIL import Image

GREEN = np.array([0.0, 255.0, 0.0], np.float32)


def key_green(image):
    """Chroma-key the flat #00FF00 background: soft alpha on edges, green spill removed."""
    a = np.asarray(image.convert("RGB"), np.float32)
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    spill = g - np.maximum(r, b)
    alpha = 1.0 - np.clip((spill - 28.0) / 92.0, 0.0, 1.0)
    # Recover the foreground colour of edge pixels that were blended with the green background.
    safe = np.maximum(alpha, 1e-3)[..., None]
    colour = np.where((alpha < 0.98)[..., None], (a - (1.0 - alpha)[..., None] * GREEN) / safe, a)
    colour = np.clip(colour, 0, 255)
    colour[..., 1] = np.minimum(colour[..., 1], np.maximum(colour[..., 0], colour[..., 2]) + 24.0)
    alpha[alpha < 0.04] = 0.0
    return Image.fromarray(np.dstack([colour, alpha * 255.0]).astype(np.uint8), "RGBA")


def split_grid(image, cols, rows):
    width, height = image.size
    return [image.crop((round(j * width / cols), round(i * height / rows),
                        round((j + 1) * width / cols), round((i + 1) * height / rows)))
            for i in range(rows) for j in range(cols)]


def _warp(channel, s, tx, ty, size, resample):
    return channel.transform(size, Image.AFFINE, (1 / s, 0, -tx / s, 0, 1 / s, -ty / s), resample=resample)


def _moments(mask):
    ys, xs = np.mgrid[0:mask.shape[0], 0:mask.shape[1]]
    total = mask.sum() + 1e-6
    return total, (xs * mask).sum() / total, (ys * mask).sum() / total


def align(cell, target_alpha, guess_scale, fixed_scale=None, start=None, max_shift=None):
    """Scale and offset that best overlay the cell's silhouette on target_alpha (H x W, 0..1); returns IoU too.

    fixed_scale keeps the scale and only moves the picture; start/max_shift bound that search.
    """
    height, width = target_alpha.shape
    cell_alpha = cell.getchannel("A")
    ca = np.asarray(cell_alpha, np.float32) / 255.0
    t_total, t_cx, t_cy = _moments(target_alpha)
    c_total, c_cx, c_cy = _moments(ca)
    s = np.sqrt(t_total / max(c_total, 1e-6))
    if not 0.7 * guess_scale < s < 1.4 * guess_scale:
        s = guess_scale
    if fixed_scale:
        s = fixed_scale
    tx, ty = start if start else (t_cx - c_cx * s, t_cy - c_cy * s)
    origin = (tx, ty)
    quarter = Image.fromarray((target_alpha * 255).astype(np.uint8)).resize((width // 4, height // 4), Image.BILINEAR)
    quarter = np.asarray(quarter, np.float32) / 255.0

    def score(s, tx, ty, q, target):
        warped = _warp(cell_alpha, s / q, tx / q, ty / q, (target.shape[1], target.shape[0]), Image.BILINEAR)
        m = np.asarray(warped, np.float32) / 255.0
        return np.minimum(m, target).sum() / (np.maximum(m, target).sum() + 1e-6)

    best = 0.0
    for step_s, step_t, q, target in [(0.04, 8.0, 4, quarter), (0.015, 3.0, 4, quarter),
                                      (0.006, 1.0, 1, target_alpha), (0.002, 0.5, 1, target_alpha)]:
        best = score(s, tx, ty, q, target)
        improved = True
        while improved:
            improved = False
            for ds, dx, dy in [(step_s, 0, 0), (-step_s, 0, 0), (0, step_t, 0), (0, -step_t, 0),
                               (0, 0, step_t), (0, 0, -step_t)]:
                if ds and fixed_scale:
                    continue
                ns, ntx, nty = s * (1 + ds), tx + dx, ty + dy
                if ds:  # scale about the silhouette centre
                    centre_x, centre_y = c_cx * s + tx, c_cy * s + ty
                    ntx, nty = centre_x - c_cx * ns, centre_y - c_cy * ns
                if max_shift is not None and max(abs(ntx - origin[0]), abs(nty - origin[1])) > max_shift:
                    continue
                value = score(ns, ntx, nty, q, target)
                if value > best + 1e-5:
                    best, s, tx, ty, improved = value, ns, ntx, nty, True
    return s, tx, ty, best


def place(cell, s, tx, ty, size):
    """Resample the cell onto a transparent canvas of `size` (premultiplied, so keyed edges stay clean)."""
    a = np.asarray(cell, np.float32)
    premultiplied = a.copy()
    premultiplied[..., :3] *= a[..., 3:4] / 255.0
    channels = [np.asarray(_warp(Image.fromarray(premultiplied[..., c], "F"), s, tx, ty, size, Image.BICUBIC),
                           np.float32) for c in range(4)]
    w = np.dstack(channels)
    alpha = np.clip(w[..., 3], 0, 255)
    colour = np.where(alpha[..., None] > 0.5, w[..., :3] * 255.0 / np.maximum(alpha[..., None], 1e-3), 0)
    alpha[alpha < 3] = 0
    return Image.fromarray(np.dstack([np.clip(colour, 0, 255), alpha]).astype(np.uint8), "RGBA")


def target_alpha(sheet, xy, framesize, scale):
    x, y = xy
    fw, fh = framesize
    tile = sheet.crop((x, y, x + fw, y + fh)).getchannel("A")
    return np.asarray(tile.resize((fw * scale, fh * scale), Image.BILINEAR), np.float32) / 255.0


def fit_grid(output, manifest, grid, sheet):
    """Key, split and align one Codex output; returns [(xy, frame, scale, iou, reliable)].

    Codex keeps one scale across a sheet, so the scale is fitted on the large cells only and shared;
    small cells (a pair of eyes, a pet popping up) would give an unreliable scale of their own and only
    have their position refined, starting from where the large cells landed.
    """
    fw, fh = manifest["framesize"]
    k, cols, rows = manifest["scale"], manifest["cols"], manifest["rows"]
    keyed = key_green(output)
    cells = split_grid(keyed, cols, rows)
    guess = k * cols * fw / keyed.size[0]
    targets = [target_alpha(sheet, xy, manifest["framesize"], k) for xy in grid["cells"]]
    free = [align(cell, target, guess) for cell, target in zip(cells, targets)]
    areas = [target.sum() for target in targets]
    large = [i for i, area in enumerate(areas) if area >= 0.3 * max(areas)]
    good = [i for i in large if free[i][3] >= 0.8] or large
    scale = float(np.median([free[i][0] for i in good]))
    # Codex keeps each picture where it was in its cell, so the large cells' offset suits the small ones.
    offset = (float(np.median([free[i][1] for i in good])), float(np.median([free[i][2] for i in good])))
    results = []
    for i, (xy, cell, target) in enumerate(zip(grid["cells"], cells, targets)):
        reliable = i in large
        if reliable:
            s0, tx0, ty0, _ = free[i]
            _, cx, cy = _moments(np.asarray(cell.getchannel("A"), np.float32) / 255.0)
            start = (tx0 + cx * (s0 - scale), ty0 + cy * (s0 - scale))   # same centre, shared scale
            s, tx, ty, iou = align(cell, target, guess, fixed_scale=scale, start=start, max_shift=6 * k)
        else:
            s, tx, ty, iou = align(cell, target, guess, fixed_scale=scale, start=offset, max_shift=3 * k)
        results.append((tuple(xy), place(cell, s, tx, ty, (fw * k, fh * k)), s, iou, reliable))
    return results


def grid_quality(results):
    """(acceptable, worst IoU of the large cells, worst IoU of the small ones)."""
    large = [iou for *_, iou, reliable in results if reliable]
    small = [iou for *_, iou, reliable in results if not reliable]
    worst_large, worst_small = min(large), min(small, default=1.0)
    return worst_large >= 0.85 and worst_small >= 0.45, worst_large, worst_small
