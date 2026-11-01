import fs
from pyfatfs import PyFatFS
from fontTools import ttLib, subset
import base64
import io
import sys
import os
from pathlib import Path

fat = PyFatFS.PyFatFS("./rsrc.bin", read_only=False)

FONT_BASE = "/Resources/Fonts/Helvetica.ttf"
original_info = fat.getinfo(FONT_BASE, namespaces=["details"])

# Read the file, get its name
b = fat.openbin(FONT_BASE, mode="rb")
original_font = ttLib.TTFont(b, recalcTimestamp=False)
original_font_name_table = original_font["name"]
b.close()

# Remove the old font
fat.remove(FONT_BASE)

# Open the file to write our font
b = fat.openbin(FONT_BASE, mode="wb")

with open("./in-otf.bin", "rb") as f:
    # Keep the template's head timestamps stable. Updating them here would
    # make an otherwise identical firmware build depend on wall-clock time.
    fake_font = ttLib.TTFont(f, recalcTimestamp=False)
    fake_font["name"] = original_font_name_table
    fake_font["CFF "].cff[0].CharStrings["space"].calcBounds = lambda x: None

    fake_font.save(b)

b.close()

# Removing and recreating the FAT entry stamps it with the current DOS time.
# Restore the canonical entry's timestamps so clean builds remain identical.
fat.setinfo(FONT_BASE, original_info.raw)

# Only the waiting bootstrap lives here; the resident always comes from disk.
resource_dir = os.environ.get('NANO7_RESOURCE_DIR')
if resource_dir:
    source = Path(resource_dir) / 'NanoApps'
    names = {'loader.hbapp'}
    if {p.name for p in source.iterdir()} != names:
        raise ValueError('unexpected bundled runtime files')
    fat.makedirs('/NanoApps', recreate=True)
    for name in sorted(names):
        target = '/NanoApps/' + name
        fat.writebytes(target, (source / name).read_bytes())
        fat.setinfo(target, original_info.raw)
    fat.setinfo('/NanoApps', original_info.raw)

fat.close()
