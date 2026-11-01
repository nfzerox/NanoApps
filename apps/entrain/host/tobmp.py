#!/usr/bin/env python3
"""Convert the headless renderer's BMP screens to PNG.

main_host.c writes BMP because it can do that in twenty lines with no
libraries; Pillow is already a ./start dependency, so the conversion lives
here rather than dragging libpng into the host build.
"""

import glob
import os
import sys

from PIL import Image


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else "shots"
    made = 0
    for bmp in sorted(glob.glob(os.path.join(out_dir, "*.bmp"))):
        png = bmp[:-4] + ".png"
        Image.open(bmp).save(png)
        print("  %s" % png)
        made += 1
    if not made:
        print("no .bmp files in %s" % out_dir)
        return 1
    print("converted %d screens" % made)
    return 0


if __name__ == "__main__":
    sys.exit(main())
