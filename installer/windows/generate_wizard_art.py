"""Generate navy ACECode Inno Setup wizard bitmaps."""

from __future__ import annotations

from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parent
OUT = ROOT / "images"
NAVY = (8, 22, 48)
NAVY_MID = (14, 42, 86)
NAVY_HI = (27, 72, 140)
ACCENT = (59, 130, 246)
ACCENT_SOFT = (147, 197, 253)
WHITE = (244, 248, 255)
MARK = (232, 240, 255)


def lerp(a: tuple[int, int, int], b: tuple[int, int, int], t: float) -> tuple[int, int, int]:
    return tuple(int(x + (y - x) * t) for x, y in zip(a, b))


def vertical_gradient(size: tuple[int, int]) -> Image.Image:
    width, height = size
    image = Image.new("RGB", size, NAVY)
    pixels = image.load()
    for y in range(height):
        t = y / max(height - 1, 1)
        if t < 0.55:
            color = lerp(NAVY, NAVY_MID, t / 0.55)
        else:
            color = lerp(NAVY_MID, NAVY_HI, (t - 0.55) / 0.45)
        for x in range(width):
            edge = abs((x / max(width - 1, 1)) - 0.5) * 0.18
            pixels[x, y] = lerp(color, NAVY, edge)
    return image


def add_glow(image: Image.Image, center: tuple[int, int], radius: int, color: tuple[int, int, int], strength: float) -> None:
    overlay = Image.new("RGBA", image.size, (0, 0, 0, 0))
    draw = ImageDraw.Draw(overlay)
    x, y = center
    draw.ellipse((x - radius, y - radius, x + radius, y + radius), fill=(*color, int(255 * strength)))
    overlay = overlay.filter(ImageFilter.GaussianBlur(radius=radius * 0.45))
    composed = Image.alpha_composite(image.convert("RGBA"), overlay)
    image.paste(composed.convert("RGB"))


def draw_mark(draw: ImageDraw.ImageDraw, box: tuple[int, int, int, int], color: tuple[int, int, int]) -> None:
    left, top, right, bottom = box
    width = right - left
    height = bottom - top
    scale = min(width, height) / 24.0

    def pt(x: float, y: float) -> tuple[int, int]:
        return (int(left + x * scale), int(top + y * scale))

    a_outer = [pt(2.4, 19.2), pt(8.35, 4.8), pt(12.25, 4.8), pt(18.2, 19.2), pt(14.75, 19.2), pt(13.61, 16.32), pt(6.99, 16.32), pt(5.85, 19.2)]
    a_cut = [pt(7.95, 13.8), pt(12.65, 13.8), pt(10.3, 8.05)]
    chevron = [pt(16.55, 7.35), pt(19.9, 12.0), pt(16.55, 16.65), pt(19.4, 16.65), pt(22.9, 12.0), pt(19.4, 7.35)]
    draw.polygon(a_outer, fill=color)
    draw.polygon(a_cut, fill=NAVY_MID)
    draw.polygon(chevron, fill=color)


def load_font(size: int, bold: bool = False) -> ImageFont.ImageFont:
    candidates = [
        "C:/Windows/Fonts/segoeuib.ttf" if bold else "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/msyhbd.ttc" if bold else "C:/Windows/Fonts/msyh.ttc",
        "C:/Windows/Fonts/arialbd.ttf" if bold else "C:/Windows/Fonts/arial.ttf",
    ]
    for path in candidates:
        try:
            return ImageFont.truetype(path, size)
        except OSError:
            continue
    return ImageFont.load_default()


def make_side(width: int, height: int) -> Image.Image:
    image = vertical_gradient((width, height))
    add_glow(image, (int(width * 0.52), int(height * 0.28)), int(width * 0.72), ACCENT, 0.28)
    add_glow(image, (int(width * 0.35), int(height * 0.78)), int(width * 0.55), (14, 165, 233), 0.16)
    draw = ImageDraw.Draw(image)
    margin = int(width * 0.12)
    mark_size = int(min(width, height) * 0.38)
    mark_top = int(height * 0.16)
    draw_mark(draw, (margin, mark_top, margin + mark_size, mark_top + mark_size), MARK)
    title = load_font(max(18, int(width * 0.145)), bold=True)
    subtitle = load_font(max(10, int(width * 0.055)))
    draw.text((margin, int(height * 0.56)), "ACECode", font=title, fill=WHITE)
    draw.text((margin, int(height * 0.68)), "AI Coding", font=subtitle, fill=ACCENT_SOFT)
    draw.text((margin, int(height * 0.74)), "Everywhere", font=subtitle, fill=ACCENT_SOFT)
    draw.rectangle((margin, int(height * 0.86), margin + int(width * 0.28), int(height * 0.86) + 3), fill=ACCENT)
    return image


def make_small(size: int) -> Image.Image:
    image = vertical_gradient((size, size))
    add_glow(image, (size // 2, size // 2), int(size * 0.55), ACCENT, 0.35)
    draw = ImageDraw.Draw(image)
    pad = int(size * 0.14)
    draw_mark(draw, (pad, pad, size - pad, size - pad), WHITE)
    return image


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    make_side(164, 314).save(OUT / "wizard-side.bmp")
    make_side(328, 628).save(OUT / "wizard-side@2x.bmp")
    make_small(55).save(OUT / "wizard-small.bmp")
    make_small(110).save(OUT / "wizard-small@2x.bmp")
    print(f"wrote wizard art to {OUT}")


if __name__ == "__main__":
    main()
