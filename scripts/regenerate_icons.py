"""Regenerate ACECode platform icons from the canonical brand artwork.

The default source is ``assets/branding/acecode-icon.svg``. Raster sources are
also accepted for one-off regeneration. Pillow is required; SVG input uses
``sips`` on macOS, ``rsvg-convert``, or ImageMagick. When ``iconutil`` is
available, the macOS ICNS used by both the app bundle and DMG is regenerated as
well.

Usage:
  python3 scripts/regenerate_icons.py [source.svg|source.png]
"""

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SOURCE = ROOT / "assets" / "branding" / "acecode-icon.svg"
MAC_ICNS = ROOT / "assets" / "macos" / "acecode.icns"
WIN_PNG = ROOT / "assets" / "windows" / "acecode_icon.png"
WIN_ICO = ROOT / "assets" / "windows" / "acecode.ico"
WEB_LOGO = ROOT / "web" / "public" / "acecode-logo.png"
WEB_FAVICON = ROOT / "web" / "public" / "favicon.ico"

ICO_SIZES = [16, 24, 32, 48, 64, 128, 256]
FAVICON_SIZES = [16, 32, 48]
MASTER_SIZE = 1024
WEB_LOGO_SIZE = 256


def rasterize_svg(src_path: Path) -> Image.Image:
    with tempfile.TemporaryDirectory(prefix="acecode-icon-source-") as temp_dir:
        output_path = Path(temp_dir) / "source.png"
        sips = Path("/usr/bin/sips")
        rsvg_convert = shutil.which("rsvg-convert")
        imagemagick = shutil.which("magick")

        if sys.platform == "darwin" and sips.is_file():
            command = [
                str(sips),
                "-s", "format", "png",
                "-z", str(MASTER_SIZE), str(MASTER_SIZE),
                str(src_path),
                "--out", str(output_path),
            ]
        elif rsvg_convert:
            command = [
                rsvg_convert,
                "--width", str(MASTER_SIZE),
                "--height", str(MASTER_SIZE),
                "--output", str(output_path),
                str(src_path),
            ]
        elif imagemagick:
            command = [
                imagemagick,
                "-background", "none",
                str(src_path),
                "-resize", f"{MASTER_SIZE}x{MASTER_SIZE}",
                str(output_path),
            ]
        else:
            raise RuntimeError(
                "SVG input requires sips (macOS), rsvg-convert, or ImageMagick"
            )

        subprocess.run(command, check=True, stdout=subprocess.DEVNULL)
        with Image.open(output_path) as image:
            return image.convert("RGBA").copy()


def load_square_rgba(src_path: Path) -> Image.Image:
    if src_path.suffix.lower() == ".svg":
        img = rasterize_svg(src_path)
    else:
        img = Image.open(src_path).convert("RGBA")
    if img.width != img.height:
        side = max(img.width, img.height)
        canvas = Image.new("RGBA", (side, side), (0, 0, 0, 0))
        canvas.paste(img, ((side - img.width) // 2, (side - img.height) // 2))
        img = canvas
    return img


def resize(img: Image.Image, size: int) -> Image.Image:
    return img.resize((size, size), Image.LANCZOS)


def write_macos_icon(master: Image.Image) -> None:
    iconutil = shutil.which("iconutil")
    if not iconutil:
        print(f"skipped {MAC_ICNS} (iconutil is unavailable)")
        return

    iconset_sizes = [
        ("icon_16x16.png", 16),
        ("icon_16x16@2x.png", 32),
        ("icon_32x32.png", 32),
        ("icon_32x32@2x.png", 64),
        ("icon_128x128.png", 128),
        ("icon_128x128@2x.png", 256),
        ("icon_256x256.png", 256),
        ("icon_256x256@2x.png", 512),
        ("icon_512x512.png", 512),
        ("icon_512x512@2x.png", 1024),
    ]
    with tempfile.TemporaryDirectory(prefix="acecode-iconset-") as temp_dir:
        iconset_path = Path(temp_dir) / "acecode.iconset"
        generated_icns = Path(temp_dir) / "acecode.icns"
        iconset_path.mkdir()
        for filename, size in iconset_sizes:
            resize(master, size).save(
                iconset_path / filename, format="PNG", optimize=True
            )
        subprocess.run(
            [iconutil, "-c", "icns", str(iconset_path), "-o", str(generated_icns)],
            check=True,
        )
        MAC_ICNS.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(generated_icns, MAC_ICNS)
    print(f"wrote {MAC_ICNS} (16x16 through 512x512@2x)")


def main() -> int:
    if len(sys.argv) > 2:
        print("usage: regenerate_icons.py [source.svg|source.png]", file=sys.stderr)
        return 2

    src = Path(sys.argv[1]) if len(sys.argv) == 2 else DEFAULT_SOURCE
    if not src.exists():
        print(f"source not found: {src}", file=sys.stderr)
        return 1

    try:
        base = load_square_rgba(src)
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"could not load icon source {src}: {error}", file=sys.stderr)
        return 1
    master = resize(base, MASTER_SIZE) if base.width != MASTER_SIZE else base

    write_macos_icon(master)

    WIN_PNG.parent.mkdir(parents=True, exist_ok=True)
    master.save(WIN_PNG, format="PNG", optimize=True)
    print(f"wrote {WIN_PNG} ({master.size[0]}x{master.size[1]})")

    # Pillow 的 ICO encoder 会从 base 图自己下采到 sizes 列表里的每一档,
    # 所以必须用高分辨率(>= 最大 size)作 base,否则小图被上采,所有尺寸都糊。
    master.save(WIN_ICO, format="ICO", sizes=[(s, s) for s in ICO_SIZES])
    print(f"wrote {WIN_ICO} (sizes: {ICO_SIZES})")

    WEB_LOGO.parent.mkdir(parents=True, exist_ok=True)
    web_logo = resize(master, WEB_LOGO_SIZE)
    web_logo.save(WEB_LOGO, format="PNG", optimize=True)
    print(f"wrote {WEB_LOGO} ({WEB_LOGO_SIZE}x{WEB_LOGO_SIZE})")

    master.save(WEB_FAVICON, format="ICO", sizes=[(s, s) for s in FAVICON_SIZES])
    print(f"wrote {WEB_FAVICON} (sizes: {FAVICON_SIZES})")

    return 0


if __name__ == "__main__":
    sys.exit(main())
