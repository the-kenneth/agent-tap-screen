#!/usr/bin/env python3
"""Regenerate firmware/src/sprites.h: 32x32 RGB565 mascots for the card corners and the working spinners.

Needs Pillow:  python3 -m venv .venv && .venv/bin/pip install pillow && .venv/bin/python tools/make_sprites.py
"""
import argparse
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parent.parent
KEY = 0xF81F  # transparent
CLAUDE_ORANGE = (0xDE, 0x73, 0x56)
CHATGPT_ICON = "/Applications/ChatGPT.app/Contents/Resources/icon-chatgpt.png"


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def fit(image, size=32):
    """Crop to visible bounds and centre on a size x size canvas, keeping hard pixel edges."""
    image = image.crop(image.getbbox())
    scale = size / max(image.size)
    image = image.resize((max(1, round(image.width * scale)), max(1, round(image.height * scale))),
                         Image.Resampling.NEAREST)
    canvas = Image.new(image.mode, (size, size))
    canvas.paste(image, ((size - image.width) // 2, (size - image.height) // 2))
    return canvas


def claude_sprite(path):
    """Body pixels are flattened to the Claude brand colour; the dark eyes are kept."""
    image = fit(Image.open(path).convert("RGBA"))
    return [KEY if a < 128 else rgb565(*CLAUDE_ORANGE) if max(r, g, b) > 100 else rgb565(r, g, b)
            for r, g, b, a in image.getdata()]


def chatgpt_knot(path, side):
    """The knot is the dark, opaque part of the app icon, as a square mask."""
    icon = Image.open(path).convert("RGBA")
    alpha = icon.getchannel("A").point(lambda v: 255 if v > 128 else 0)
    dark = icon.convert("L").point(lambda v: 255 if v < 100 else 0)
    mask = Image.composite(dark, Image.new("L", icon.size), alpha)
    mask = mask.crop(mask.getbbox())
    square = Image.new("L", (max(mask.size),) * 2)
    square.paste(mask, ((square.width - mask.width) // 2, (square.height - mask.height) // 2))
    return square.resize((side, side), Image.Resampling.LANCZOS)


def chatgpt_sprite(path, ink=0xF79E):
    """Strokes are thickened so they survive 28px."""
    square = chatgpt_knot(path, 280).filter(ImageFilter.MaxFilter(7))
    small = square.resize((28, 28), Image.Resampling.BOX).point(lambda v: 255 if v > 90 else 0)
    canvas = Image.new("L", (32, 32))
    canvas.paste(small, (2, 2))
    return [ink if v else KEY for v in canvas.getdata()]


SPIN = 18  # working spinner size
SPIN_BIG = 720  # drawing size before the box filter down to SPIN


def asterisk(arms, length, width, rot=0, tear=False, dot=0):
    """An asterisk mask after Claude Code's spinner glyphs; teardrop arms are narrow at the hub and round at the tip."""
    image = Image.new("L", (SPIN_BIG, SPIN_BIG))
    draw, c = ImageDraw.Draw(image), SPIN_BIG / 2
    for k in range(arms):
        a = math.radians(rot + 360 * k / arms)
        ux, uy = math.sin(a), -math.cos(a)
        tip, w = length * c, width * c / 2
        hub = w * 0.35 if tear else w
        draw.polygon([(c - uy * hub, c + ux * hub), (c + ux * tip - uy * w, c + uy * tip + ux * w),
                      (c + ux * tip + uy * w, c + uy * tip - ux * w), (c + uy * hub, c - ux * hub)], fill=255)
        if tear:
            draw.ellipse((c + ux * tip - w, c + uy * tip - w, c + ux * tip + w, c + uy * tip + w), fill=255)
    if dot:
        draw.ellipse((c - dot * c, c - dot * c, c + dot * c, c + dot * c), fill=255)
    return image


def claude_spinner():
    """· ✢ ✳ ✶ ✻ ✽, as alpha masks."""
    shapes = [asterisk(0, 0, 0, dot=0.22), asterisk(4, 0.8, 0.36, tear=True), asterisk(8, 0.92, 0.26),
              asterisk(6, 0.95, 0.42), asterisk(8, 0.85, 0.3, rot=22.5, tear=True),
              asterisk(6, 0.95, 0.4, tear=True, dot=0.2)]
    return [list(s.resize((SPIN, SPIN), Image.Resampling.BOX).getdata()) for s in shapes]


def codex_spinner(path, frames=12):
    """The knot turned through 60 degrees, after which its six-fold symmetry repeats it."""
    knot = Image.new("L", (SPIN_BIG, SPIN_BIG))
    knot.paste(chatgpt_knot(path, SPIN_BIG - 40).filter(ImageFilter.MaxFilter(15)), (20, 20))
    return [list(knot.rotate(-60 * i / frames, Image.Resampling.BICUBIC).resize((SPIN, SPIN), Image.Resampling.BOX)
                 .getdata()) for i in range(frames)]


def alpha_frames(name, frames):
    lines = [f"static const uint8_t {name}[{len(frames)}][SPIN * SPIN] = {{"]
    for frame in frames:
        lines.append("  {")
        lines += ["    " + ", ".join(f"{v:3d}" for v in frame[i:i + SPIN]) + "," for i in range(0, SPIN * SPIN, SPIN)]
        lines.append("  },")
    return lines + ["};", ""]


def c_array(name, values):
    rows = ["  " + ", ".join(f"0x{v:04X}" for v in values[i:i + 16]) + "," for i in range(0, 1024, 16)]
    return [f"static const uint16_t {name}[32 * 32] = {{", *rows, "};", ""]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--claude", default=ROOT / "tools/assets/claude-pixel.png")
    parser.add_argument("--chatgpt", default=CHATGPT_ICON)
    parser.add_argument("--out", default=ROOT / "firmware/src/sprites.h")
    args = parser.parse_args()

    lines = ["#pragma once", "#include <stdint.h>", "",
             "// Generated by tools/make_sprites.py. Claude mascot from token-tv (MIT); ChatGPT knot from the",
             "// ChatGPT app's icon. SPRITE_KEY marks transparent pixels; spinner frames are 8-bit alpha masks.",
             f"static const uint16_t SPRITE_KEY = 0x{KEY:04X};", ""]
    lines += c_array("SPRITE_CLAUDE", claude_sprite(args.claude))
    lines += c_array("SPRITE_CHATGPT", chatgpt_sprite(args.chatgpt))
    lines += [f"static const int SPIN = {SPIN};", ""]
    lines += alpha_frames("SPIN_CLAUDE", claude_spinner())
    lines += alpha_frames("SPIN_CODEX", codex_spinner(args.chatgpt))
    Path(args.out).write_text("\n".join(lines))
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
