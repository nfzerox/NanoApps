"""Read the app/resident contract from the same header used by the loaders."""
from pathlib import Path
import re

_header = (Path(__file__).resolve().parents[1] / 'sdk/hb_runtime_compat.h').read_text()


def _value(name):
    match = re.search(r'^#define ' + name + r' (0x[0-9a-fA-F]+|[0-9]+)u?$', _header, re.M)
    if not match:
        raise ValueError('missing runtime compatibility constant: ' + name)
    return int(match[1], 0)


ABI = _value('HB_RUNTIME_ABI')
FIRMWARE = _value('HB_RUNTIME_FIRMWARE')
