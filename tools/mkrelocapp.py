#!/usr/bin/env python3
"""Pack a relocatable homebrew app ELF into a loadable .hbapp blob.

The app is built non-PIC, linked at base 0, with -Wl,-q (keep relocations) and
-e hb_blob_main. This tool flattens it and extracts the ABS32 relocations so the
runtime loader (sdk/hb_reloc.c) can drop it at any OS-heap address and rebase.

  ELF in  -> .hbapp out:
    header: magic 'HRL1', entry_off, image_size, span_size, reloc_count, align
    image:  image_size bytes (text+rodata+data laid out from VMA 0, gaps zeroed)
    relocs: reloc_count * u32  (VMA offsets to add the load base to)

Only R_ARM_ABS32 relocations against allocated sections are emitted; PC-relative
branch relocs (THM_CALL/JUMP24) are intra-blob and need no fixup. The blob must
have NO undefined symbols (fully self-contained — fold in libgcc).

Usage: mkrelocapp.py <in.elf> <out.hbapp> [entry_symbol]
"""
import sys, struct
try:
    from elftools.elf.elffile import ELFFile
    from elftools.elf.constants import SH_FLAGS
    from elftools.elf.sections import SymbolTableSection
    from elftools.elf.relocation import RelocationSection
except ModuleNotFoundError:
    sys.exit("error: pyelftools is required to pack .hbapp blobs.\n"
             "       install it with:  python3 -m pip install --user pyelftools\n"
             "       (or just run ./start, which installs build dependencies for you)")

R_ARM_ABS32 = 2
# Absolute relocations the loader can't apply (it only rebases 32-bit words). GCC emits
# these for movw/movt address pairs at -O2/-O3 unless built with -mword-relocations.
R_ARM_UNSUPPORTED_ABS = {
    43: "R_ARM_MOVW_ABS_NC", 44: "R_ARM_MOVT_ABS",
    47: "R_ARM_THM_MOVW_ABS_NC", 48: "R_ARM_THM_MOVT_ABS",
}
MAGIC = 0x31_4C_52_48  # 'HRL1' little-endian


def main(in_path, out_path, entry_sym="hb_blob_main"):
    with open(in_path, "rb") as f:
        elf = ELFFile(f)

        # 1) no undefined (external) symbols allowed — blob must be self-contained
        undef = []
        entry_off = None
        for sec in elf.iter_sections():
            if not isinstance(sec, SymbolTableSection):
                continue
            for sym in sec.iter_symbols():
                if sym.name == entry_sym and sym["st_info"]["type"] == "STT_FUNC":
                    entry_off = sym["st_value"] & ~1  # strip thumb bit
                if sym["st_shndx"] == "SHN_UNDEF" and sym.name:
                    undef.append(sym.name)
        if undef:
            sys.exit(f"error: blob has undefined (external) symbols: {sorted(set(undef))[:8]}\n"
                     f"       link it self-contained (fold in libgcc).")
        if entry_off is None:
            sys.exit(f"error: entry symbol '{entry_sym}' not found")

        # 2) image span: lowest..highest VMA across ALLOC sections
        alloc = [s for s in elf.iter_sections()
                 if (s["sh_flags"] & SH_FLAGS.SHF_ALLOC) and s["sh_size"] > 0]
        if not alloc:
            sys.exit("error: no allocatable sections")
        lo = min(s["sh_addr"] for s in alloc)
        if lo != 0:
            sys.exit(f"error: lowest section VMA is {lo:#x}, expected 0 (link -Ttext 0)")
        span = max(s["sh_addr"] + s["sh_size"] for s in alloc)
        # PROGBITS image (NOBITS/.bss excluded — zeroed at load)
        progbits_end = max((s["sh_addr"] + s["sh_size"] for s in alloc
                            if s["sh_type"] == "SHT_PROGBITS"), default=0)
        image = bytearray(progbits_end)
        for s in alloc:
            if s["sh_type"] == "SHT_PROGBITS":
                a = s["sh_addr"]
                image[a:a + s["sh_size"]] = s.data()

        # 3) ABS32 relocations against allocatable sections
        reloc_offs = []
        for sec in elf.iter_sections():
            if not isinstance(sec, RelocationSection):
                continue
            target = elf.get_section(sec["sh_info"])
            if target is None or not (target["sh_flags"] & SH_FLAGS.SHF_ALLOC):
                continue  # skip .rel.debug_* etc.
            symtab = elf.get_section(sec["sh_link"])
            for r in sec.iter_relocations():
                if r["r_info_type"] in R_ARM_UNSUPPORTED_ABS:
                    sym = symtab.get_symbol(r["r_info_sym"])
                    if sym["st_shndx"] != "SHN_ABS":
                        sys.exit(f"error: {R_ARM_UNSUPPORTED_ABS[r['r_info_type']]} at "
                                 f"{r['r_offset']:#x} ({sym.name or 'section'}): the loader only "
                                 f"rebases 32-bit words; build with -mword-relocations")
                if r["r_info_type"] == R_ARM_ABS32:
                    # A reference to an absolute symbol (e.g. --just-symbols data at a fixed
                    # segmented address) must not move with the image.
                    sym = symtab.get_symbol(r["r_info_sym"])
                    if sym["st_shndx"] == "SHN_ABS":
                        continue
                    reloc_offs.append(r["r_offset"])
        # 4) linker-generated interworking/long-branch stubs ("veneers"). ld writes these
        # itself (e.g. an ARM-state tail call into a Thumb function becomes
        # "__foo_from_arm: ldr pc, [pc, #-4]; .word foo+1") and emits NO relocation for
        # their absolute target word, so without this they jump to a base-0 address after
        # the loader rebases the image, causing ARM/VFP builds to crash on such calls.
        stubs = []
        for sec in elf.iter_sections():
            if not isinstance(sec, SymbolTableSection):
                continue
            for sym in sec.iter_symbols():
                n = sym.name
                if n.endswith(("_from_arm", "_from_thumb", "_veneer", "_change_to_arm")):
                    stubs.append((sym["st_value"] & ~1, n))
        LDR_PC_PC_M4 = 0xE51FF004   # ldr pc, [pc, #-4] (ARM): absolute target in next word
        for addr, n in sorted(set(stubs)):
            words = [struct.unpack_from("<I", image, addr + 4 * i)[0]
                     for i in range(4) if addr + 4 * i + 4 <= progbits_end]
            if LDR_PC_PC_M4 not in words:
                sys.exit(f"error: linker stub {n} at {addr:#x} has an unrecognised form "
                         f"({' '.join(f'{w:08x}' for w in words)}); the loader can't rebase it")
            reloc_offs.append(addr + 4 * (words.index(LDR_PC_PC_M4) + 1))
        reloc_offs = sorted(set(reloc_offs))
        for off in reloc_offs:
            if off + 4 > progbits_end:
                sys.exit(f"error: reloc offset {off:#x} outside image")

    align = 16
    hdr = struct.pack("<IIIIII", MAGIC, entry_off, progbits_end, span,
                      len(reloc_offs), align)
    body = b"".join(struct.pack("<I", o) for o in reloc_offs)
    with open(out_path, "wb") as out:
        out.write(hdr)
        out.write(image)
        out.write(body)

    print(f"wrote {out_path}: entry@{entry_off:#x} image={progbits_end} "
          f"span={span} bss={span - progbits_end} relocs={len(reloc_offs)}")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else "hb_blob_main")
