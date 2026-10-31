"""Shared data for redrawing the companion pets in high resolution (see hd_companion_pets.py)."""

import json
import pathlib
import re

import numpy as np
from PIL import Image, ImageFilter

PETS = {
    # scale: reference / fitting size relative to the original sprite frame; output_scale: shipped HD frame
    # size (defaults to scale). Clippy's 496px and Rover's 360px frames stay at or above their drawn size
    # up to the same window zoom (184 vs 140 scene units wide). cols/rows: frames per Codex grid.
    "clippy": {"dir": "classic/clippy", "scale": 4, "cols": 3, "rows": 3, "ref_resample": Image.NEAREST,
               "describe": "Clippy, the classic Microsoft Office paperclip assistant with big eyes and eyebrows, "
                           "usually standing on a sheet of yellow lined paper (keep the paper whenever it is there). "
                           "3D-rendered cartoon look, silver-blue metal wire."},
    "rover": {"dir": "classic/rover", "scale": 5, "output_scale": 4.5, "cols": 3, "rows": 3,
              "ref_resample": Image.NEAREST,
              "describe": "Rover, the classic yellow cartoon search dog with a red collar and a small round tag. "
                          "3D-rendered cartoon look."},
    "whale": {"dir": "whale", "scale": 2, "cols": 2, "rows": 2, "ref_resample": Image.LANCZOS, "all": True,
              "describe": "a chibi anime whale girl in a navy maid dress with white apron and frilled headband, long "
                          "wavy dark-blue hair, whale fins on the head and a whale tail behind her. Anime chibi "
                          "illustration style."},
}


def load_agent(path):
    text = pathlib.Path(path).read_text(encoding="utf-8")
    return json.loads(text[text.index("{"):text.rindex("}") + 1])


def used_animations(root, kind, agent):
    """Animations the player can reach: every name in the PROFILES entry, plus RestPose.

    Whale Girl keeps all of her (few) animations, every state is playable through ClassicPet.play.
    """
    if PETS[kind].get("all"):
        return list(agent["animations"])
    js = (root / "classic_pet.js").read_text(encoding="utf-8")
    block = js[js.index("const PROFILES"):js.index("// Docked pets peek")]
    block = block[block.index(f"    {kind}: {{"):]
    block = block[:block.index("\n    },")]
    names = []
    for name in re.findall(r"'([A-Za-z0-9_]+)'", block):
        if name in agent["animations"] and name not in names:
            names.append(name)
    if "RestPose" in agent["animations"] and "RestPose" not in names:
        names.append("RestPose")
    return names


def tile_order(agent, names):
    """Unique tile offsets in playback order of the given animations."""
    order = []
    for name in names:
        for frame in agent["animations"][name]["frames"]:
            for xy in frame.get("images") or []:
                if tuple(xy) not in order:
                    order.append(tuple(xy))
    return order


def near_duplicates(sheet, framesize, order):
    """Map every tile to an earlier tile that only differs by dithering noise (or to itself)."""
    fw, fh = framesize
    feats = []
    for x, y in order:
        tile = sheet.crop((x, y, x + fw, y + fh))
        flat = Image.new("RGBA", tile.size, (0, 0, 0, 255))
        flat.alpha_composite(tile)
        colour = np.asarray(flat.convert("RGB").filter(ImageFilter.GaussianBlur(1.2)), np.float32)
        feats.append((colour, np.asarray(tile.getchannel("A"), np.float32)))
    reps, owner = [], {}
    for i, (colour, alpha) in enumerate(feats):
        match = None
        for j in reversed(reps[-200:]):
            colour2, alpha2 = feats[j]
            diff = np.abs(colour - colour2).max(axis=2)
            if np.abs(alpha - alpha2).mean() < 6 and diff.mean() < 3 and np.percentile(diff, 99.5) < 40:
                match = j
                break
        if match is None:
            reps.append(i)
            owner[order[i]] = order[i]
        else:
            owner[order[i]] = order[match]
    return [order[i] for i in reps], owner
