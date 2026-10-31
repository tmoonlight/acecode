"""Generate Rover's animations as green-screen videos with Grok Imagine (Grok Build CLI `reference_to_video`).

Every clip starts and ends on the same approved sitting pose (first_frame = last_frame), so each animation
returns to rest and loops seamlessly; "Show" only pins the last frame (Rover runs in from afar). The pose sits
in the bottom 3/4 of the square canvas so jumps have headroom: that 540px box is the original 80x80 sprite frame.
"""

import argparse
import concurrent.futures
import json
import os
import pathlib
import shutil
import sys
import threading
import time

import numpy as np
from PIL import Image

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import grok_media  # noqa: E402

CANVAS, BOX = 720, 540   # video canvas; the original sprite frame occupies the bottom-centre BOX square
COMMON = (
    " Same yellow cartoon dog character as in the reference frame (Rover, the Windows XP search dog): same design, "
    "colours and proportions throughout, a smooth glossy 3D cartoon render. Static locked-off camera: no camera "
    "movement, no zoom, no cuts. He stays on his sitting spot unless told otherwise. The background stays perfectly "
    "flat, uniform chroma-key green for the whole clip: no shadows, floor, horizon, lighting changes, particles, other "
    "objects or text. Nothing on him or his props is green. Any prop he uses is held in his paws and disappears "
    "before the end, and he ends in exactly the same sitting pose as the first frame."
)
# name: (seconds, action). Durations follow the original Microsoft Agent animations (1-4 s, idle a 6 s loop).
CLIPS = {
    "Idle": (6, "He sits calmly and breathes gently, blinks twice, his ears and tail sway slightly and he glances "
                "left and right before looking back at the viewer."),
    "Sports": (4, "He cheerfully pulls out a black-and-white soccer ball and a purple and gold pennant flag, waves "
                  "the pennant cheering, then puts them away."),
    "Cooking": (4, "He holds a small red cooking pot and stirs it with a wooden spoon, sniffing the delicious smell "
                   "happily, then puts it away."),
    "Shopping": (3, "He picks up a newspaper full of sale advertisements, reads it with interest, then puts it away."),
    "Books": (3, "He picks up an open book, reads it attentively turning a page, then closes it and puts it away."),
    "Money": (3, "He happily holds up a fan of red, blue and gold paper banknotes (nothing green), then puts "
                  "them away."),
    "Celebrity": (3, "He proudly holds up a black movie clapperboard with a gold star on it, then puts it away."),
    "Travel": (3, "He holds up a brown travel suitcase covered with travel stickers, then puts it away."),
    "LookUp": (2, "He looks up curiously at something above him, then looks back at the viewer."),
    "LookUpLeft": (2, "He looks up to the upper left curiously, then looks back at the viewer."),
    "Acknowledge": (2, "He nods twice in agreement with a small smile."),
    "Greet": (2, "He waves hello to the viewer with one front paw, smiling."),
    "Pleased": (3, "He is pleased: he wags his tail happily, does a small happy bounce with a big smile, then settles."),
    "ClickedOn": (2, "He giggles as if tickled, squirming a little with a happy face, then calms down."),
    "Surprised": (2, "He is surprised: his eyes widen and his ears perk up for a moment, then he relaxes."),
    "GestureLeft": (3, "Within half a second he turns his head and points with one front paw towards the viewer's "
                       "left, holds that pointing pose for about a second and a half, then puts the paw down."),
    "Thinking": (3, "He thinks hard: he puts a paw to his chin and looks up, tilting his head, then looks back."),
    "Writing": (3, "He scribbles busily with a pencil on a small notepad held in his other paw, then puts them away."),
    "Searching": (5, "He searches for something: he lowers his nose and sniffs around in front of him, moving his "
                     "head left and right, then sits back up and looks at the viewer."),
    "ImageSearching": (3, "He holds up a golden picture frame with a photo inside, showing it to the viewer, then puts "
                          "it away."),
    "GetAttention": (2, "He barks happily and waves both front paws to get the viewer's attention."),
    "GetAttentionMinor": (2, "He taps one front paw lightly and tilts his head at the viewer, waiting patiently."),
    "Embarrassed": (3, "He is embarrassed: he looks down shyly, his ears droop and he covers his eyes with a paw, "
                       "then recovers with a sheepish smile."),
    "Congratulate": (2, "He celebrates: he jumps up joyfully with his tongue out and tail wagging, then sits back down."),
    "CharacterSucceeds": (2, "He nods proudly with a big smile, wagging his tail."),
    "Show": (2, "He appears: he starts tiny in the distance and happily runs towards the viewer, then sits down in "
                "exactly the pose and spot of the reference frame."),
}
lock = threading.Lock()


def log(message):
    with lock:
        print(time.strftime("%H:%M:%S"), message, flush=True)


def padded_base(base_path, out_path):
    """The approved pose scaled into the bottom-centre BOX of a CANVAS square of the same green."""
    base = Image.open(base_path).convert("RGB")
    rgb = np.asarray(base)
    border = np.concatenate([rgb[:12].reshape(-1, 3), rgb[-12:].reshape(-1, 3), rgb[:, :12].reshape(-1, 3)])
    green = tuple(int(v) for v in np.median(border, axis=0))
    canvas = Image.new("RGB", (CANVAS, CANVAS), green)
    canvas.paste(base.resize((BOX, BOX), Image.LANCZOS), ((CANVAS - BOX) // 2, CANVAS - BOX))
    canvas.save(out_path)
    return out_path


def generate(name, base, work, attempts):
    target = work / f"{name}.mp4"
    if target.exists():
        return True
    seconds, action = CLIPS[name]
    arguments = {"prompt": action + COMMON, "aspect_ratio": "1:1", "duration": seconds, "resolution_name": "720p"}
    if name == "Show":
        arguments["last_frame"] = base
    else:
        arguments["first_frame"] = arguments["last_frame"] = base
    for attempt in range(1, attempts + 1):
        started = time.time()
        result = grok_media.run("reference_to_video", arguments, work / "runs" / f"{name}-{attempt}",
                                work / "logs" / f"{name}-{attempt}.txt")
        if result:
            shutil.copyfile(result, target)
            log(f"{name}: ok in {time.time() - started:.0f}s (attempt {attempt})")
            return True
        log(f"{name}: attempt {attempt} failed")
    return False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", type=pathlib.Path, required=True, help="approved sitting pose on green (square)")
    parser.add_argument("--work", type=pathlib.Path, required=True)
    parser.add_argument("--jobs", type=int, default=6)
    parser.add_argument("--attempts", type=int, default=2)
    parser.add_argument("--only", default="")
    args = parser.parse_args()
    (args.work / "logs").mkdir(parents=True, exist_ok=True)
    base = os.path.normpath(str(padded_base(args.base, args.work / "base_padded.png").resolve()))
    names = [name for name in CLIPS if not args.only or name in args.only.split(",")]
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        results = dict(zip(names, pool.map(lambda n: generate(n, base, args.work, args.attempts), names)))
    log("failed: " + json.dumps([name for name, ok in results.items() if not ok]))


if __name__ == "__main__":
    main()
