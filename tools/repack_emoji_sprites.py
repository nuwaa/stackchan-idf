#!/usr/bin/env python3
"""Repack generated Noto Emoji sprites from 32 to 16 colours per image.

The ATOM S3R source stores a 32-entry RGB565 palette plus 5-bit pixels
(224 bytes/image).  Stack-chan's fixed 5 MiB app slot needs a denser form, so
this keeps every Unicode sequence while reducing each image to a 16-entry
palette plus 4-bit pixels (160 bytes/image).
"""

from __future__ import annotations

import argparse
import collections
import re
from pathlib import Path


ARRAY_RE = re.compile(
    r"(const uint8_t kEmojiSprites\[\] PROGMEM = \{\n)(.*?)(\n\};)", re.S
)


def rgb565_components(lo: int, hi: int) -> tuple[int, int, int]:
    value = lo | (hi << 8)
    return ((value >> 11) & 31, (value >> 5) & 63, value & 31)


def distance(a: tuple[int, int, int], b: tuple[int, int, int]) -> int:
    return 4 * (a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2 + 4 * (a[2] - b[2]) ** 2


def unpack_indices(data: list[int]) -> list[int]:
    result: list[int] = []
    for pos in range(256):
        bit = pos * 5
        byte = bit >> 3
        shift = bit & 7
        value = data[byte]
        if shift > 3:
            value |= data[byte + 1] << 8
        result.append((value >> shift) & 31)
    return result


def repack_sprite(sprite: list[int]) -> list[int]:
    palette = [sprite[i : i + 2] for i in range(0, 64, 2)]
    pixels = unpack_indices(sprite[64:])
    counts = collections.Counter(pixels)
    chosen = [index for index, _ in counts.most_common(16)]
    while len(chosen) < 16:
        chosen.append(0)

    chosen_rgb = [rgb565_components(*palette[index]) for index in chosen]
    remap: list[int] = []
    for index, color in enumerate(palette):
        if index in chosen:
            remap.append(chosen.index(index))
            continue
        rgb = rgb565_components(*color)
        remap.append(min(range(16), key=lambda i: distance(rgb, chosen_rgb[i])))

    packed = bytearray()
    for pos in range(0, 256, 2):
        packed.append(remap[pixels[pos]] | (remap[pixels[pos + 1]] << 4))
    compact_palette = [byte for index in chosen for byte in palette[index]]
    return compact_palette + list(packed)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    args = parser.parse_args()

    text = args.source.read_text(encoding="utf-8")
    match = ARRAY_RE.search(text)
    if not match:
        raise SystemExit("kEmojiSprites array not found")
    values = [int(value) for value in re.findall(r"\d+", match.group(2))]
    if len(values) % 224:
        raise SystemExit(f"unexpected sprite byte count: {len(values)}")

    compact: list[int] = []
    for offset in range(0, len(values), 224):
        compact.extend(repack_sprite(values[offset : offset + 224]))

    lines = ["  " + ",".join(map(str, compact[i : i + 20])) + ","
             for i in range(0, len(compact), 20)]
    replacement = match.group(1) + "\n".join(lines) + match.group(3)
    args.source.write_text(text[: match.start()] + replacement + text[match.end() :],
                           encoding="utf-8")
    print(f"repacked {len(values) // 224} sprites: {len(values)} -> {len(compact)} bytes")


if __name__ == "__main__":
    main()
