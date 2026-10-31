"""Step 2: redraw every reference grid with Codex image generation, retrying grids whose frames do not line up."""

import argparse
import concurrent.futures
import json
import pathlib
import re
import shutil
import subprocess
import sys
import threading
import time

from PIL import Image

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from hd_frames import fit_grid, grid_quality  # noqa: E402
from hd_pets_common import PETS  # noqa: E402

PROMPT = (
    "Use your built-in image generation tool to create ONE image. The FIRST attached image is a SPRITE SHEET of "
    "animation frames of a desktop pet character, arranged in a regular grid of {cols} columns x {rows} rows of "
    "equal cells with no gaps. Each frame is a small, low-resolution sprite enlarged for reference. Redraw the "
    "WHOLE sheet as clean high-resolution artwork with the SAME grid: every cell stays exactly where it is, and in "
    "every cell the character keeps exactly the same pose, facing, expression, position, size and colours as in "
    "that cell of the reference, including any props or effects shown there. An empty cell stays empty green. "
    "The SECOND attached image is the approved high-resolution design of this character: match its rendering "
    "style, materials, line work and details exactly in every cell, so all frames look like one consistent "
    "animation. Smooth anti-aliased edges, crisp details, no pixelation, no dithering noise. Do not add grid "
    "lines, borders, shadows on the background, text or anything else. The background everywhere must be "
    "perfectly flat pure #00FF00 green. Character: {describe}"
)
lock = threading.Lock()


def log(message):
    with lock:
        print(time.strftime("%H:%M:%S"), message, flush=True)


def newest_image(session, aspect):
    folder = pathlib.Path.home() / ".codex" / "generated_images" / session
    best = None
    for path in folder.glob("*.png"):
        with Image.open(path) as image:
            ratio = image.size[0] / image.size[1]
        if abs(ratio / aspect - 1) < 0.06 and (best is None or path.stat().st_mtime > best.stat().st_mtime):
            best = path
    return best


def redraw(codex, work, kind, manifest, grid, sheet, attempts, prompt=None):
    """Redraw one grid in work/kind (next to its anchor.png) until its frames line up; keeps the best attempt."""
    out_dir = work / kind / "out"
    out_dir.mkdir(exist_ok=True)
    stem = grid["file"][:-4]
    target = out_dir / grid["file"]
    if target.exists():
        return True
    prompt = prompt or PROMPT.format(cols=manifest["cols"], rows=manifest["rows"], describe=PETS[kind]["describe"])
    with Image.open(work / kind / grid["file"]) as ref:
        aspect = ref.size[0] / ref.size[1]
    best = None
    for attempt in range(1, attempts + 1):
        log_path = work / kind / "logs" / f"{stem}-{attempt}.txt"
        log_path.parent.mkdir(exist_ok=True)
        try:
            with log_path.open("w", encoding="utf-8") as handle:
                subprocess.run([codex, "exec", "--skip-git-repo-check", "-s", "read-only", "-C", str(work / kind),
                                "-i", grid["file"], "-i", "anchor.png", "-o", str(log_path.with_suffix(".last")),
                                prompt], stdout=handle, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                               timeout=1200)
        except subprocess.TimeoutExpired:
            log(f"{kind} {stem} attempt {attempt}: timeout")
        match = re.search(r"session id: ([0-9a-f-]+)", log_path.read_text(encoding="utf-8", errors="replace"))
        image_path = match and newest_image(match[1], aspect)
        if not image_path:
            log(f"{kind} {stem} attempt {attempt}: no image")
            continue
        with Image.open(image_path) as output:
            ok, worst, worst_small = grid_quality(fit_grid(output.convert("RGB"), manifest, grid, sheet))
        log(f"{kind} {stem} attempt {attempt}: worst IoU {worst:.3f} (small frames {worst_small:.3f}) "
            f"{'OK' if ok else 'retry'}")
        if best is None or worst > best[0]:
            best = (worst, image_path)
        if ok:
            break
    if best is None:
        return False
    shutil.copyfile(best[1], target)
    (out_dir / f"{stem}.json").write_text(json.dumps({"source": str(best[1]), "worst_iou": float(best[0])}),
                                          encoding="utf-8")
    return True


def reselect(work, kind, manifest, grid, sheet):
    """Among every attempt already made for a grid, keep the one whose frames line up best."""
    with Image.open(work / kind / grid["file"]) as ref:
        aspect = ref.size[0] / ref.size[1]
    best = None
    for log_path in sorted((work / kind / "logs").glob(f"{grid['file'][:-4]}-*.txt")):
        match = re.search(r"session id: ([0-9a-f-]+)", log_path.read_text(encoding="utf-8", errors="replace"))
        image_path = match and newest_image(match[1], aspect)
        if not image_path:
            continue
        with Image.open(image_path) as output:
            ok, worst, worst_small = grid_quality(fit_grid(output.convert("RGB"), manifest, grid, sheet))
        if best is None or (ok, worst, worst_small) > best[0]:
            best = ((ok, worst, worst_small), image_path)
    if best:
        shutil.copyfile(best[1], work / kind / "out" / grid["file"])
        log(f"{kind} {grid['file']}: {'OK' if best[0][0] else 'BELOW THRESHOLD'} worst IoU {best[0][1]:.3f}")
    return best


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--codex", required=True)
    parser.add_argument("--root", type=pathlib.Path, required=True, help="assets/desktop_pet")
    parser.add_argument("--work", type=pathlib.Path, required=True)
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--attempts", type=int, default=3)
    parser.add_argument("--only", default="")
    parser.add_argument("--reselect", action="store_true", help="only re-pick the best existing attempt per grid")
    args = parser.parse_args()
    if args.reselect:
        for kind, spec in PETS.items():
            if args.only and kind not in args.only.split(","):
                continue
            manifest = json.loads((args.work / kind / "manifest.json").read_text(encoding="utf-8"))
            sheet = Image.open(args.root / spec["dir"] / "map.png").convert("RGBA")
            for grid in manifest["grids"]:
                reselect(args.work, kind, manifest, grid, sheet)
        return
    tasks = []
    for kind, spec in PETS.items():
        if args.only and kind not in args.only.split(","):
            continue
        manifest = json.loads((args.work / kind / "manifest.json").read_text(encoding="utf-8"))
        sheet = Image.open(args.root / spec["dir"] / "map.png").convert("RGBA")
        tasks += [(kind, manifest, grid, sheet) for grid in manifest["grids"]]
    log(f"{len(tasks)} grids")
    failed = []
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        futures = {pool.submit(redraw, args.codex, args.work, kind, manifest, grid, sheet, args.attempts):
                   (kind, grid["file"]) for kind, manifest, grid, sheet in tasks}
        for future in concurrent.futures.as_completed(futures):
            try:
                if not future.result():
                    failed.append(futures[future])
            except Exception as error:  # keep the other grids going
                log(f"{futures[future]} failed: {error!r}")
                failed.append(futures[future])
    log(f"done, failed: {failed}")


if __name__ == "__main__":
    main()
