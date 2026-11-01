"""Shared ARM ELF symbol reader for local diagnostic/emulator tools."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def symbols(elf):
    result = subprocess.run(["arm-none-eabi-nm", "-n", str(elf)],
                            check=True, text=True, capture_output=True)
    return {parts[2]: int(parts[0], 16)
            for line in result.stdout.splitlines()
            if len(parts := line.split()) == 3}

