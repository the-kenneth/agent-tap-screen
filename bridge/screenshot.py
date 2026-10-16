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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("out", nargs="?", default="screen.png")
    parser.add_argument("--scenes", help="scene numbers for the two cards, e.g. 2,3 (default: today's)")
    args = parser.parse_args()
    scenes = [int(s) for s in args.scenes.split(",")] if args.scenes else bridge.daily_scenes(date.today())
    accounts = [bridge.Account("Claude", "#D97757", bridge.fetch_claude(True)),
                bridge.Account("Codex", "#10A37F", bridge.fetch_codex())]
    for account in accounts:
        account.refresh(True)
    now = int(time.time())
    fd = bridge.open_serial(bridge.find_port())
    time.sleep(2.5)
    rows = bridge.screen_rows(accounts, now, scenes)[0]
    os.write(fd, (json.dumps({"now": now, "accounts": rows}) + "\n").encode())
    time.sleep(2)
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
