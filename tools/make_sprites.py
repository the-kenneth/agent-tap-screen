#!/usr/bin/env python3
"""Regenerate firmware/src/sprites.h: the card-corner logos and the animations they play while a turn is in progress.

Reads Clawd from the installed Claude desktop app and the ChatGPT knot from the installed ChatGPT app.
Needs Pillow:  python3 -m venv .venv && .venv/bin/pip install pillow && .venv/bin/python tools/make_sprites.py
"""
import argparse
import json
import re
import struct
from pathlib import Path

from PIL import Image, ImageFilter

ROOT = Path(__file__).resolve().parent.parent
KEY = 0xF81F  # transparent
CLAUDE_ORANGE = (0xF5, 0x62, 0x2D)
CHATGPT_INK = (0xF7, 0xF3, 0xF7)
CLAUDE_APP = "/Applications/Claude.app/Contents/Resources/app.asar"
CHATGPT_ICON = "/Applications/ChatGPT.app/Contents/Resources/icon-chatgpt.png"


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def hex24(rgb):
    return "0x" + "".join(f"{c:02X}" for c in rgb)


def asar_file(archive, pattern):
    """Text of the first file in an Electron asar archive whose name matches the regex."""
    with open(archive, "rb") as f:
        _, header_size, _, json_size = struct.unpack("<4I", f.read(16))
        dirs = [json.loads(f.read(json_size))]
        while dirs:
            for name, entry in dirs.pop().get("files", {}).items():
                if "files" in entry:
                    dirs.append(entry)
                elif re.search(pattern, name):
                    f.seek(8 + header_size + int(entry["offset"]))
                    return f.read(entry["size"]).decode()
    raise SystemExit(f"no {pattern} in {archive}")


def clawd_sprite(archive, name):
    """One of Clawd's sprites from the desktop app's Clawd Quest engine, as (palette, frames of L images).

    The app stores a base image of palette digits ('.' clear) and per-frame deltas of base-36 offset plus value.
    """
    js = asar_file(archive, r"^clawdQuestEngine-.*\.js$")
    m = re.search(name + r':\{cat:"\w+",w:(\d+),h:(\d+),fps:\d+,palette:(\[[^\]]*\]),base:"([^"]*)",deltas:"([^"]*)"', js)
    if not m:
        raise SystemExit(f"no {name} sprite in the Claude app")
    w, h, palette, base, deltas = int(m[1]), int(m[2]), json.loads(m[3]), list(m[4]), m[5]
    frames = [base[:]]
    for delta in deltas.split("("):
        for change in filter(None, delta.split(",")):
            base[int(change[:-1], 36)] = change[-1]
        frames.append(base[:])
    images = []
    for frame in frames:
        image = Image.new("L", (w, h))
        image.putdata([255 if c == "." else int(c, 36) for c in frame])
        images.append(image)
    return [tuple(int(p[i:i + 2], 16) for i in (1, 3, 5)) for p in palette], images


def clawd(archive):
    """Clawd at rest and typing on his laptop, as the app plays him (loop frames 17-33 of its Laptop sprite).

    Scaled by 4/3 to fill the logo box; each pose is centred and both share a baseline.
    """
    palette, frames = clawd_sprite(archive, "Laptop")
    frames = [f.resize((round(f.width * 4 / 3), round(f.height * 4 / 3)), Image.Resampling.NEAREST) for f in frames]
    poses = [frames[:1], frames[17:34]]
    boxes = []
    for pose in poses:
        bounds = [f.point(lambda v: 0 if v == 255 else 255).getbbox() for f in pose]
        boxes.append((min(b[0] for b in bounds), min(b[1] for b in bounds),
                      max(b[2] for b in bounds), max(b[3] for b in bounds)))
    w = max(b[2] - b[0] for b in boxes)
    h = max(b[3] - b[1] for b in boxes)
    out = []
    for pose, (x0, y0, x1, y1) in zip(poses, boxes):
        frames = []
        for f in pose:
            canvas = Image.new("L", (w, h), 255)
            canvas.paste(f.crop((x0, y0, x1, y1)), ((w - (x1 - x0)) // 2, h - (y1 - y0)))
            frames.append("".join("." if v == 255 else str(v) for v in canvas.tobytes()))
        out.append(frames)
    # The app's shadow is its body colour darkened; keep that ratio against the brighter orange.
    shadow = tuple(round(c * s / b) for c, s, b in zip(CLAUDE_ORANGE, palette[2], palette[1]))
    return w, h, [palette[0], CLAUDE_ORANGE, shadow, palette[3]], out[0][0], out[1]


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


def chatgpt_sprite(path):
    """Strokes are thickened so they survive 28px."""
    square = chatgpt_knot(path, 280).filter(ImageFilter.MaxFilter(7))
    small = square.resize((28, 28), Image.Resampling.BOX).point(lambda v: 255 if v > 90 else 0)
    canvas = Image.new("L", (32, 32))
    canvas.paste(small, (2, 2))
    return [rgb565(*CHATGPT_INK) if v else KEY for v in canvas.tobytes()]


def knot_turn(path, frames):
    """The knot turning clockwise through 60 degrees, after which its six-fold symmetry repeats it.

    Antialiased rather than thresholded like the resting sprite, so the strokes keep their shape as it turns.
    """
    big = Image.new("L", (320, 320))
    big.paste(chatgpt_knot(path, 280).filter(ImageFilter.MaxFilter(7)), (20, 20))
    # The icon's strands aren't quite six-fold symmetric; averaging the six turns removes the seam that would
    # otherwise jump back each time the loop wraps.
    turns = big
    for k in range(1, 6):
        turns = Image.blend(turns, big.rotate(60 * k, Image.Resampling.BICUBIC), 1 / (k + 1))
    big = turns
    return ["".join("." if v < 9 else "0123456789ABCDEF"[round(v / 17)] for v in
                    big.rotate(-60 * i / frames, Image.Resampling.BICUBIC).resize((32, 32), Image.Resampling.BOX)
                    .tobytes()) for i in range(frames)]


def art(frame, w, indent):
    """A frame as one string literal per row, so the header reads as pixel art."""
    return [f'{indent}"{frame[i:i + w]}"' for i in range(0, len(frame), w)]


def art_frames(decl, frames, w):
    lines = [f"{decl} = {{"]
    for frame in frames:
        rows = art(frame, w, "     ")
        rows[0] = "    {" + rows[0].lstrip()
        lines += rows[:-1] + [rows[-1] + "},"]
    return lines + ["};", ""]


def c_array(name, values):
    rows = ["  " + ", ".join(f"0x{v:04X}" for v in values[i:i + 16]) + "," for i in range(0, 1024, 16)]
    return [f"static const uint16_t {name}[32 * 32] = {{", *rows, "};", ""]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--claude-app", default=CLAUDE_APP)
    parser.add_argument("--chatgpt", default=CHATGPT_ICON)
    parser.add_argument("--knot-frames", type=int, default=30)
    parser.add_argument("--out", default=ROOT / "firmware/src/sprites.h")
    args = parser.parse_args()

    cw, ch, palette, rest, typing = clawd(args.claude_app)
    knot = knot_turn(args.chatgpt, args.knot_frames)
    lines = ["#pragma once", "#include <stdint.h>", "",
             "// Generated by tools/make_sprites.py. Clawd from the Claude desktop app's Clawd Quest; ChatGPT knot",
             "// from the ChatGPT app's icon. SPRITE_KEY marks transparent pixels.",
             f"static const uint16_t SPRITE_KEY = 0x{KEY:04X};", ""]
    lines += c_array("SPRITE_CHATGPT", chatgpt_sprite(args.chatgpt))
    lines += [f"static const uint32_t CHATGPT_INK = {hex24(CHATGPT_INK)};", "",
              f"static const int CLAWD_W = {cw}, CLAWD_H = {ch}, CLAWD_FRAMES = {len(typing)}, CLAWD_FPS = 12;",
              "// Rows of palette digits, '.' clear: eyes, body, shadow, laptop.",
              f"static const uint32_t CLAWD_PALETTE[4] = {{{', '.join(hex24(c) for c in palette)}}};",
              "static const char CLAWD_REST[CLAWD_W * CLAWD_H + 1] ="]
    lines += art(rest, cw, "    ")
    lines[-1] += ";"
    lines += [""]
    lines += art_frames("static const char CLAWD_TYPING[CLAWD_FRAMES][CLAWD_W * CLAWD_H + 1]", typing, cw)
    lines += [f"static const int KNOT = 32, KNOT_FRAMES = {len(knot)};",
              "// 4-bit alpha as hex digits, '.' clear; each frame turns the knot a further 60 / KNOT_FRAMES degrees."]
    lines += art_frames("static const char KNOT_TURN[KNOT_FRAMES][KNOT * KNOT + 1]", knot, 32)
    Path(args.out).write_text("\n".join(lines))
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
