"""Turn Rover's Grok videos into pixel-art frame sheets and the animation table the pet player loads.

Run with a Python that has PyAV (`av`), numpy and Pillow, after grok_rover_pixel_base.py and
grok_rover_videos.py. Every video frame (12 fps) is keyed off the green screen and re-pixelated on the base
sprite's grid and palette (pixelate.py): 5x5 video pixels per art pixel, the base's 32 colours plus up to 16
colours for props. Cells that barely change keep the previous frame's pixel, so video noise does not shimmer,
and identical frames are stored once. Sheets are lossless WebP. hd/frames.js carries Rover's own timing table
(`agent`, same animation names as agent.js, preferred by the player) and `pixelArt: true` (drawn unsmoothed).
"""

import argparse
import hashlib
import json
import pathlib
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from grok_rover_pixel_base import BLOCK  # noqa: E402
from grok_rover_videos import BOX, CANVAS, CLIPS  # noqa: E402
from grok_video_frames import frames, key  # noqa: E402
from hd_assemble import pack  # noqa: E402
from pixelate import build_palette, pixelate, render  # noqa: E402

FPS = 12
SIZE = BOX // BLOCK                                          # the sprite box in art pixels (108)
ORIGIN = ((CANVAS - BOX) // 2 // BLOCK, (CANVAS - BOX) // BLOCK)   # box position on the art canvas
EXTRA_COLOURS = 16


def sampled(path, looping):
    decoded = frames(path)
    step = max(1, round(len(decoded) / (decoded[-1][0] + 1 / 24) / FPS)) if len(decoded) > 1 else 1
    picked = decoded[::step]
    if looping and len(picked) > 1:
        picked = picked[:-1]   # the last frame is the first one again
    return [rgb for _, rgb in picked], round(1000 * step / 24)


def prop_palette(base_palette, clips):
    """Colours props bring in: pixels far from every base colour, quantized to EXTRA_COLOURS."""
    far = []
    for pictures, _ in clips.values():
        for rgb in pictures[::4]:
            keyed = np.asarray(key(rgb))
            opaque = keyed[keyed[..., 3] > 200][:, :3].astype(np.float32)[::7]
            if not len(opaque):
                continue
            d = ((opaque[:, None, :] - base_palette[None]) ** 2).sum(axis=2).min(axis=1)
            far.append(opaque[d > 45 ** 2])
    far = np.concatenate(far) if far else np.zeros((0, 3), np.float32)
    if len(far) < 200:
        return base_palette
    rgba = np.concatenate([far, np.full((len(far), 1), 255, np.float32)], axis=1).astype(np.uint8)
    extra = build_palette([Image.fromarray(rgba.reshape(1, -1, 4))], EXTRA_COLOURS)
    return np.concatenate([base_palette, extra])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=pathlib.Path, required=True, help="assets/desktop_pet")
    parser.add_argument("--work", type=pathlib.Path, required=True, help="grok_rover_videos.py work directory")
    args = parser.parse_args()
    base_sprite = Image.open(args.work / "base_pixel.png").convert("RGBA")
    base_palette = np.load(args.work / "base_palette.npy")
    clips = {name: sampled(args.work / f"{name}.mp4", looping=name != "Show") for name in CLIPS}
    palette = prop_palette(base_palette, clips)
    base_canvas = base_sprite.resize((CANVAS, CANVAS), Image.NEAREST)
    base_index, base_means = pixelate(base_canvas, BLOCK, palette)
    stored, items, animations = {}, [], {}
    clip_names = ["RestPose", *CLIPS]

    def store(index, clip_number):
        digest = hashlib.sha1(index.tobytes()).hexdigest()
        if digest not in stored:
            picture = render(index, palette)
            box = picture.getchannel("A").getbbox()
            if not box:
                return None
            key_ = (clip_number, len([1 for k in stored.values() if k[0] == clip_number]))
            stored[digest] = key_
            items.append((key_, picture.crop(box), box[0] - ORIGIN[0], box[1] - ORIGIN[1]))
        return stored[digest]

    animations["RestPose"] = {"frames": [{"duration": 100, "images": [list(store(base_index, 0))]}]}
    for number, name in enumerate(CLIPS, start=1):
        pictures, duration = clips[name]
        previous, means = (None, None) if name == "Show" else (base_index, base_means)
        frames_out = []
        for i, rgb in enumerate(pictures):
            if i == 0 and name != "Show":
                index, mean = base_index, base_means   # pinned first frame: exactly the base sprite
            else:
                index, mean = pixelate(key(rgb), BLOCK, palette, previous, means)
            previous, means = index, mean
            key_ = store(index, number)
            if key_ is None:
                continue
            if frames_out and frames_out[-1]["images"][0] == list(key_):
                frames_out[-1]["duration"] += duration   # an unchanged frame just lasts longer
            else:
                frames_out.append({"duration": duration, "images": [list(key_)]})
        animations[name] = {"frames": frames_out}
    out = args.root / "classic" / "rover" / "hd"
    out.mkdir(exist_ok=True)
    for old in out.glob("sheet-*.webp"):
        old.unlink()
    by_key = {key_: (picture, dx, dy) for key_, picture, dx, dy in items}
    table, sheets, groups = {}, [], {}
    for number, name in enumerate(clip_names):
        mine = [(key_, by_key[key_][0]) for key_, *_ in items if key_[0] == number]
        for image, spots in pack(mine):
            sheet_index = len(sheets)
            image.save(out / f"sheet-{sheet_index:02d}.webp", "WEBP", lossless=True, quality=100, method=6)
            sheets.append(list(image.size))
            for key_, (sx, sy) in spots.items():
                picture, dx, dy = by_key[key_]
                table[f"{key_[0]},{key_[1]}"] = [sheet_index, sx, sy, *picture.size, dx, dy]
        used = {table[f"{k[0]},{k[1]}"][0] for f in animations[name]["frames"] for k in f["images"]}
        groups[name] = sorted(used)
    agent = {"framesize": [SIZE, SIZE], "animations": animations}
    data = {"framesize": [SIZE, SIZE], "pixelArt": True, "sheets": sheets, "animations": groups, "frames": table,
            "agent": agent}
    (out / "frames.js").write_text(
        "// Generated by scripts/desktop_pet/grok_rover_assemble.py: pixel-art Rover, animated from green-screen\n"
        "// videos made with Grok Imagine; `agent` replaces agent.js timing for this pet.\n"
        f"(window.ClassicPetHD = window.ClassicPetHD || {{}}).rover = {json.dumps(data, separators=(',', ':'))};\n",
        encoding="utf-8")
    total = sum((out / f"sheet-{i:02d}.webp").stat().st_size for i in range(len(sheets)))
    frame_count = sum(len(a["frames"]) for a in animations.values())
    print(f"rover: {len(animations)} animations, {frame_count} frames ({len(items)} distinct), "
          f"{len(palette)} colours, {len(sheets)} sheets, {total / 1e3:.0f} KB")


if __name__ == "__main__":
    main()
