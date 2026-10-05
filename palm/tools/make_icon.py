#!/usr/bin/env python3
"""Writes the 32x22 1-bit launcher icon, the FujiNet logo, as a BMP for PilRC.

The rows trace the logo in fujinet-firmware data/webui (favicon.ico): a go-board
grid with four black stones and one white.
"""
import struct
import sys

ROWS = [
    "................................",
    ".........#....####.....#........",
    ".........#...######....#........",
    ".........#...#######...#........",
    "......####################......",
    ".........#...######....#........",
    ".........#....####.....#........",
    ".........#.....##......#........",
    ".......####...####...####.......",
    "......######.#....#.######......",
    "......########....#########.....",
    "......########....#########.....",
    "......######.#....#.######......",
    ".......####...####...####.......",
    "........##.....##.....##........",
    ".........#......#....####.......",
    ".........#......#...######......",
    ".........#......#...#######.....",
    "......#####################.....",
    ".........#......#...######......",
    ".........#......#....####.......",
    "......................##........",
]


def main(path):
    width, height = 32, len(ROWS)
    pixels = b""
    for row in reversed(ROWS):  # BMP rows run bottom-up
        bits = 0
        for c in row:
            bits = (bits << 1) | (c == "#")
        pixels += bits.to_bytes(4, "big")
    palette = bytes([255, 255, 255, 0, 0, 0, 0, 0])  # index 1 is black
    info = struct.pack("<IiiHHIIiiII", 40, width, height, 1, 1, 0, len(pixels), 2835, 2835, 2, 0)
    offset = 14 + len(info) + len(palette)
    with open(path, "wb") as f:
        f.write(b"BM" + struct.pack("<IHHI", offset + len(pixels), 0, 0, offset))
        f.write(info + palette + pixels)


if __name__ == "__main__":
    main(sys.argv[1])
