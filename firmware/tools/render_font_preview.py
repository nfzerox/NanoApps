#!/usr/bin/env python3
"""Render stock, current, unhinted and hinted Nano font carriers at 1x."""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


PANEL = 240
LABEL_HEIGHT = 34
SAMPLES = (
    (22, "Connected"),
    (18, "Music"),
    (18, "Now Playing"),
    (18, "Settings"),
    (18, "NanoApps"),
    (14, "About  12:34"),
)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--stock", type=Path, required=True)
    parser.add_argument("--current", type=Path, required=True)
    parser.add_argument("--unhinted", type=Path, required=True)
    parser.add_argument("--hinted", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    entries = (
        ("Stock Apple TTF", args.stock),
        ("Current Viafont", args.current),
        ("Apple CFF, no hints", args.unhinted),
        ("Apple CFF, autohinted", args.hinted),
    )
    image = Image.new("RGB", (PANEL * len(entries), PANEL + LABEL_HEIGHT), "white")
    draw = ImageDraw.Draw(image)
    label_font = ImageFont.load_default(size=14)

    for column, (label, path) in enumerate(entries):
        left = column * PANEL
        draw.rectangle((left, 0, left + PANEL - 1, PANEL - 1), outline="#b8b8b8")
        y = 13
        for size, sample in SAMPLES:
            font = ImageFont.truetype(str(path), size=size)
            draw.text((left + 13, y), sample, font=font, fill="black")
            y += 34 if size >= 18 else 27
        draw.rectangle(
            (left, PANEL, left + PANEL - 1, PANEL + LABEL_HEIGHT - 1),
            fill="#202020",
        )
        draw.text((left + 8, PANEL + 8), label, font=label_font, fill="white")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    image.save(args.output)
    print(args.output)


if __name__ == "__main__":
    main()
