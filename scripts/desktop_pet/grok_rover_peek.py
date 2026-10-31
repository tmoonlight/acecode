"""Rover's edge-peek art (eyes open / blinking) redrawn with Grok image_edit to match his Grok-animated look.

The 2026-10-16 sprite-resolution peek art is the only reference (given together with the sitting pose, Grok
drew the whole sitting dog with two loose paws instead): --ref is that art enlarged on green, ideally with a
faithful upscaler. Three candidates are drawn and the one matching the original silhouette best is kept. The
blink is an edit of it, and only the eye area of the blink is used, so nothing else moves when he blinks. Both
are aligned to the original peek silhouette at the HD frame scale (4.5x), so PEEK.rover.ratio stays 1.
"""

import argparse
import concurrent.futures
import os
import pathlib
import shutil
import subprocess
import sys

import numpy as np
from PIL import Image, ImageFilter

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import grok_media  # noqa: E402
from grok_video_frames import key  # noqa: E402
from hd_frames import align, place  # noqa: E402

SCALE = 4.5
OPEN = ("Redraw this exact image as a clean, high-resolution glossy 3D cartoon render of the same character (Rover, "
        "the yellow Windows XP search dog with a red collar). Keep the composition exactly: he peeks in shyly from the "
        "right edge of the picture as if hiding behind it - only his head, collar and two front paws gripping the "
        "edge are visible, and everything is cut off by a perfectly straight vertical line at the right edge. Same "
        "position, size and pose as this image; do not show the rest of his body. Background: perfectly flat "
        "chroma-key green filling everything else, no shadows, nothing else.")
BLINK = ("Edit this image: the dog closes both eyes in a happy blink (curved closed eyelids). Change nothing else at "
         "all - same pose, same position, same lighting, same flat green background.")


def git_original(root):
    """The sprite-resolution peek art as committed before the HD redraws (composition reference)."""
    data = subprocess.run(["git", "-C", str(root), "show", "f2a86ae0:assets/desktop_pet/classic/rover/peek.png"],
                          capture_output=True, check=True).stdout
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=pathlib.Path, required=True, help="repository root")
    parser.add_argument("--ref", type=pathlib.Path, required=True, help="the original peek art enlarged on green")
    parser.add_argument("--work", type=pathlib.Path, required=True)
    args = parser.parse_args()
    work = args.work
    work.mkdir(parents=True, exist_ok=True)
    original_path = work / "peek-original.png"
    original_path.write_bytes(git_original(args.root))
    original = Image.open(original_path).convert("RGBA")
    fw, fh = original.size[0] // 2, original.size[1]
    path = lambda p: os.path.normpath(str(p.resolve()))   # noqa: E731
    size = (round(fw * SCALE), round(fh * SCALE))
    target = np.asarray(original.crop((0, 0, fw, fh)).getchannel("A").resize(size, Image.BILINEAR), np.float32) / 255.0

    def candidate(i):
        picture = work / f"peek_open{i}.jpg"
        if not picture.exists():
            result = grok_media.run("image_edit", {"prompt": OPEN, "image": [path(args.ref)]}, work / f"run_open{i}",
                                    work / f"log_open{i}.txt")
            if not result:
                return 0.0, picture
            shutil.copyfile(result, picture)
        keyed = key(np.asarray(Image.open(picture).convert("RGB")))
        return align(keyed, target, size[1] / keyed.size[1])[3], picture

    with concurrent.futures.ThreadPoolExecutor(3) as pool:
        scored = sorted(pool.map(candidate, range(3)), key=lambda item: item[0])
    print("candidates IoU", [round(float(score), 3) for score, _ in scored])
    opened = scored[-1][1]
    closed = work / "peek_blink.jpg"
    if not closed.exists():
        result = grok_media.run("image_edit", {"prompt": BLINK, "image": [path(opened)]}, work / "run_blink",
                                work / "log_blink.txt")
        shutil.copyfile(result, closed)
    keyed_open = key(np.asarray(Image.open(opened).convert("RGB")))
    keyed_blink = key(np.asarray(Image.open(closed).convert("RGB")).copy())
    if keyed_blink.size != keyed_open.size:
        keyed_blink = keyed_blink.resize(keyed_open.size, Image.LANCZOS)
    s, tx, ty, iou = align(keyed_open, target, size[1] / keyed_open.size[1])
    print(f"peek alignment IoU {iou:.3f}")
    open_frame, blink_frame = place(keyed_open, s, tx, ty, size), place(keyed_blink, s, tx, ty, size)
    # Only the eyes blink: take the blink picture where the two differ around the eyes, the open one elsewhere.
    diff = np.abs(np.asarray(open_frame, np.float32) - np.asarray(blink_frame, np.float32)).max(axis=2)
    eyes = np.zeros_like(diff)
    eyes[: size[1] // 2] = diff[: size[1] // 2] > 40
    mask = Image.fromarray((eyes * 255).astype(np.uint8)).filter(ImageFilter.MaxFilter(9)).filter(ImageFilter.GaussianBlur(3))
    m = np.asarray(mask, np.float32)[..., None] / 255.0
    blink = (np.asarray(blink_frame, np.float32) * m + np.asarray(open_frame, np.float32) * (1 - m)).astype(np.uint8)
    out = Image.new("RGBA", (size[0] * 2, size[1]))
    out.paste(open_frame, (0, 0))
    out.paste(Image.fromarray(blink), (size[0], 0))
    target_path = args.root / "assets" / "desktop_pet" / "classic" / "rover" / "peek.png"
    out.save(target_path, optimize=True)
    print(f"rover peek {out.size} -> {target_path}")


if __name__ == "__main__":
    main()
