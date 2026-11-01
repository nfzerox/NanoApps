# There's no good font library for Rust, someone please make one

from fontTools import ttLib
from fontTools import cffLib
import base64
import sys
import io
import os

with open("./in-cff.bin", "rb") as input_font:
    x = cffLib.CFFFontSet()
    x.decompile(input_font, None, isCFF2=False)
    # Resolve the exploit program using the raw CFF's own charset before
    # opening the selected carrier.  The raw exploit omits a charset and was
    # built against the reviewed Viafont template, whose glyph order supplies
    # that missing metadata.  Apple's 2,240-glyph Helvetica uses a different
    # index for ``space``, so its charset must not be applied to the raw CFF.
    template = ttLib.TTFont(
        "./helpers/viafont/viafont_converted.otf", recalcTimestamp=False
    )
    x[0].charset = template["CFF "].cff[0].charset
    exploit_space = x[0].CharStrings["space"]
    template.close()

    # Firmware releases must be byte-reproducible. FontTools otherwise updates
    # head.modified to the wall clock during save, changing the OTF, FAT image,
    # and MSE on every clean build.
    # The reviewed Viafont carrier remains the default, so the established
    # build is byte-for-byte unchanged.  A font-only candidate may point this
    # at a locally generated CFF Helvetica carrier.  The carrier is never
    # committed because it is derived from Apple's firmware.
    carrier = os.environ.get(
        "NANO7_CFF_CARRIER", "./helpers/viafont/viafont_converted.otf"
    )
    tt = ttLib.TTFont(carrier, recalcTimestamp=False)

    tt_cff = tt["CFF "].cff[0]
    tt_cff.CharStrings["space"] = exploit_space
    tt_cff.CharStrings["space"].calcBounds = lambda x:  None

with open("./out-otf.bin", "wb") as out_file:
    tt.save(out_file, False)
