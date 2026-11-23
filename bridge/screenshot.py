#!/usr/bin/env python3
"""Send one live reading to the screen, then save what the panel shows as a PNG (stop the bridge first)."""
import argparse
import json
import os
import struct
import time
import zlib
from datetime import date

import usage_bridge as bridge


def read_exact(fd, size, deadline):
    data = b""
    while len(data) < size:
        if time.monotonic() > deadline:
            raise TimeoutError(f"got {len(data)} of {size} bytes")
        data += os.read(fd, size - len(data))
    return data


def write_png(path, width, height, rgb565, swap):
    rows = []
    for y in range(height):
        row = bytearray(b"\x00")
        for x in range(width):
            value = struct.unpack_from(">H" if swap else "<H", rgb565, (y * width + x) * 2)[0]
            r, g, b = value >> 11, (value >> 5) & 0x3F, value & 0x1F
            row += bytes((r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2))
        rows.append(bytes(row))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    with open(path, "wb") as out:
        out.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
                  + chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b""))


def demo_rows(state, now, scenes):
    """Made-up readings for README pictures, so they show each state without touching real usage."""
    def window(label, used, hours):
        return {"label": label, "used": used, "resets": now + hours * 3600}

    def card(name, color, scene, windows, **extra):
        return {"name": name, "color": color, "status": "ok", "age": 60, "scene": scene, "waiting": 0,
                "working": 0, "windows": windows, **extra}

    if state == "calm":
        claude, codex = [window("5H", 18, 3), window("WEEK", 31, 90)], [window("WEEK", 12, 120)]
        claude_extra, codex_extra = {}, {}
    elif state == "busy":
        claude, codex = [window("5H", 64, 1), window("WEEK", 58, 40)], [window("WEEK", 47, 70)]
        claude_extra, codex_extra = {"working": 1}, {"waiting": 2}
    elif state == "alert":  # Claude waiting and working, so the LED breathes orange while Clawd types
        claude, codex = [window("5H", 42, 2), window("WEEK", 36, 60)], [window("WEEK", 22, 100)]
        claude_extra, codex_extra = {"waiting": 1, "working": 1}, {}
    else:  # low: nearly spent, and Codex's reading is an hour old
        claude, codex = [window("5H", 96, 1), window("WEEK", 88, 20)], [window("WEEK", 74, 30)]
        claude_extra, codex_extra = {}, {"age": 3600}
    return [card("Claude", "#D97757", scenes[0], claude, **claude_extra),
            card("Codex", "#10A37F", scenes[1], codex, **codex_extra)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("out", nargs="?", default="screen.png")
    parser.add_argument("--scenes", help="scene numbers for the two cards, e.g. 2,3 (default: today's)")
    parser.add_argument("--demo", choices=["calm", "busy", "alert", "low"],
                        help="show made-up readings instead of live ones")
    parser.add_argument("--hold", action="store_true", help="keep showing the reading until Ctrl-C; no capture")
    args = parser.parse_args()
    scenes = [int(s) for s in args.scenes.split(",")] if args.scenes else bridge.daily_scenes(date.today())
    now = int(time.time())
    if args.demo:
        rows = demo_rows(args.demo, now, scenes)
    else:
        accounts = [bridge.Account("Claude", "#D97757", bridge.fetch_claude(True)),
                    bridge.Account("Codex", "#10A37F", bridge.fetch_codex())]
        for account in accounts:
            account.refresh(True)
        rows = bridge.screen_rows(accounts, now, scenes)[0]
    fd = bridge.open_serial(bridge.find_port())
    time.sleep(2.5)
    os.write(fd, (json.dumps({"now": now, "accounts": rows}) + "\n").encode())
    if args.hold:
        print("holding; Ctrl-C to stop")
        while True:  # resend as the bridge does, with ages fixed so nothing drifts to OLD
            time.sleep(30)
            os.write(fd, (json.dumps({"now": int(time.time()), "accounts": rows}) + "\n").encode())
    time.sleep(4 if args.demo else 2)  # demo shots catch Clawd typing, past his laptop take-out
    termios_flush(fd)
    os.write(fd, b"SHOT\n")
    deadline = time.monotonic() + 40
    buffer = b""
    while b"SHOT " not in buffer:
        buffer += read_exact(fd, 1, deadline)
    header = buffer[buffer.index(b"SHOT "):]
    while not header.endswith(b"\n"):
        header += read_exact(fd, 1, deadline)
    width, height = map(int, header.split()[1:3])
    # Gaps between cards stay the background colour; stars are not captured.
    pixels = bytearray(b"\x08\x65" * width * height)
    while True:
        tag = b""
        while not tag.endswith(b"\n"):
            tag += read_exact(fd, 1, deadline)
        if tag.strip() == b"END":
            break
        if not tag.startswith(b"CARD "):
            continue
        top, rows = map(int, tag.split()[1:3])
        pixels[top * width * 2:(top + rows) * width * 2] = read_exact(fd, width * rows * 2, deadline)
    write_png(args.out, width, height, pixels, swap=True)
    print(f"saved {args.out}")


def termios_flush(fd):
    import termios
    termios.tcflush(fd, termios.TCIFLUSH)


if __name__ == "__main__":
    main()
