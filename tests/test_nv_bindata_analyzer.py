import hashlib
import json
import struct
import tempfile
import unittest
import zlib
from pathlib import Path

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

import nv_bindata_analyzer as nba


def _compress_raw_deflate(data: bytes) -> bytes:
    comp = zlib.compressobj(level=6, method=zlib.DEFLATED, wbits=-15)
    return comp.compress(data) + comp.flush()


def build_synthetic_elf(payload_comp: bytes, payload_raw: bytes, archive_offset: int = 0) -> bytes:
    """Build a minimal valid 64-bit little-endian ELF ET_REL object matching nv-kernel.o_binary layout."""
    # Sections:
    #   0: "" (null)
    #   1: .text
    #   2: .rodata
    #   3: .rela.rodata
    #   4: .rela.text
    #   5: .symtab
    #   6: .strtab
    #   7: .shstrtab
    shstr_names = [b"", b".text", b".rodata", b".rela.rodata", b".rela.text", b".symtab", b".strtab", b".shstrtab"]
    shstr = b""
    shstr_offs = []
    for s in shstr_names:
        shstr_offs.append(len(shstr))
        shstr += s + b"\x00"

    # Layout in .rodata:
    #   payload_comp_bytes @ data_comp_off
    #   payload_raw_bytes  @ data_raw_off
    #   entry0 (24B)       @ entry0_off (compressed flag 0x0b)
    #   entry1 (24B)       @ entry1_off (uncompressed flag 0x02)
    #   BindataStorage (8 + 2*16 = 40B) @ storage_off
    compressed_blob = _compress_raw_deflate(payload_comp)
    ro = bytearray(archive_offset)
    data_comp_off = len(ro)
    ro.extend(compressed_blob)
    while len(ro) % 8:
        ro.append(0)
    data_raw_off = len(ro)
    ro.extend(payload_raw)
    while len(ro) % 8:
        ro.append(0)

    entry0_off = len(ro)
    ro.extend(struct.pack("<IIQII", len(payload_comp), len(compressed_blob), 0, 0x0B, 0))
    entry1_off = len(ro)
    ro.extend(struct.pack("<IIQII", len(payload_raw), len(payload_raw), 0, 0x02, 0))

    storage_off = len(ro)
    ro.extend(struct.pack("<II", 2, 0))
    ro.extend(struct.pack("<IIQ", 0x23, 0, 0))  # reloc @ +16 -> entry0_off
    ro.extend(struct.pack("<IIQ", 0x18, 0, 0))  # reloc @ +32 -> entry1_off

    text = b"\x90" * 32

    # .strtab
    strtab = b"\x00test_getter\x00"

    # .symtab: 0=undef, 1=section(.text), 2=section(.rodata), 3=FUNC test_getter @ .text+0 size=32
    symtab = bytearray()
    symtab.extend(struct.pack("<IBBHQQ", 0, 0, 0, 0, 0, 0))
    symtab.extend(struct.pack("<IBBHQQ", 0, 3, 0, 1, 0, 0))
    symtab.extend(struct.pack("<IBBHQQ", 0, 3, 0, 2, 0, 0))
    symtab.extend(struct.pack("<IBBHQQ", 1, (0 << 4) | 2, 0, 1, 0, 32))

    # .rela.rodata: entries pointing via sym 2 (.rodata)
    rela_ro = bytearray()
    R_X86_64_64 = 1
    rela_ro.extend(struct.pack("<QQq", entry0_off + 8, (2 << 32) | R_X86_64_64, data_comp_off))
    rela_ro.extend(struct.pack("<QQq", entry1_off + 8, (2 << 32) | R_X86_64_64, data_raw_off))
    rela_ro.extend(struct.pack("<QQq", storage_off + 16, (2 << 32) | R_X86_64_64, entry0_off))
    rela_ro.extend(struct.pack("<QQq", storage_off + 32, (2 << 32) | R_X86_64_64, entry1_off))

    # .rela.text: test_getter references storage_off
    rela_tx = struct.pack("<QQq", 8, (2 << 32) | 11, storage_off)

    # Assemble ELF image
    ehdr_size = 64
    buf = bytearray(ehdr_size)
    sections_blobs = [
        (shstr_offs[0], 0, 0, b"", 0, 0, 1, 0),
        (shstr_offs[1], 1, 0x6, text, 0, 0, 16, 0),
        (shstr_offs[2], 1, 0x2, bytes(ro), 0, 0, 8, 0),
        (shstr_offs[3], 4, 0, bytes(rela_ro), 5, 2, 8, 24),
        (shstr_offs[4], 4, 0, bytes(rela_tx), 5, 1, 8, 24),
        (shstr_offs[5], 2, 0, bytes(symtab), 6, 4, 8, 24),
        (shstr_offs[6], 3, 0, strtab, 0, 0, 1, 0),
        (shstr_offs[7], 3, 0, shstr, 0, 0, 1, 0),
    ]
    sh_offsets = []
    for _, _, _, blob, _, _, align, _ in sections_blobs:
        while len(buf) % align:
            buf.append(0)
        sh_offsets.append(len(buf))
        buf.extend(blob)

    while len(buf) % 8:
        buf.append(0)
    e_shoff = len(buf)
    for idx, (nm_off, stype, sflags, blob, slink, sinfo, align, entsz) in enumerate(sections_blobs):
        buf.extend(struct.pack("<IIQQQQIIQQ", nm_off, stype, sflags, 0, sh_offsets[idx], len(blob), slink, sinfo, align, entsz))

    e_ident = b"\x7fELF\x02\x01\x01" + b"\x00" * 9
    struct.pack_into(
        nba.ELF64_EHDR_FMT,
        buf,
        0,
        e_ident,
        1,        # ET_REL
        62,       # EM_X86_64
        1,        # EV_CURRENT
        0, 0,
        e_shoff,
        0,
        ehdr_size,
        0, 0,
        64,
        len(sections_blobs),
        7,
    )
    return bytes(buf)


class TestNvBindataAnalyzer(unittest.TestCase):
    def test_synthetic_raw_deflate_and_uncompressed_roundtrip(self):
        comp_payload = b"Tue Jun 23 10:23:10 2026 GP104 PMU FW" + b"\xab" * 200
        raw_payload = bytes(range(36))
        elf_bytes = build_synthetic_elf(comp_payload, raw_payload)

        with tempfile.TemporaryDirectory() as td:
            elf_path = Path(td) / "mock.o"
            elf_path.write_bytes(elf_bytes)
            parser = nba.ElfBindataParser(elf_path)
            archives = parser.scan_archives()
            self.assertEqual(len(archives), 1)
            arch = archives[0]
            self.assertEqual(arch.count, 2)
            self.assertEqual(arch.referenced_by_funcs, ["test_getter+0x8"])
            id0, e0 = arch.items[0]
            id1, e1 = arch.items[1]
            self.assertEqual(id0, 0x23)
            self.assertTrue(e0.compressed)
            self.assertEqual(e0.sha256, hashlib.sha256(comp_payload).hexdigest())
            self.assertIn("Tue Jun 23 10:23:10 2026 GP104 PMU FW", e0.hints[0])
            self.assertEqual(id1, 0x18)
            self.assertFalse(e1.compressed)
            self.assertEqual(e1.sha256, hashlib.sha256(raw_payload).hexdigest())

    def test_chip_tag_mapping_gp104(self):
        name, tag = nba.CHIP_TAG_MAP_BY_ARCH_IMPL[(0x13, 0x4)]
        self.assertEqual(name, "GP104")
        self.assertEqual(tag, 0x1F)
        self.assertEqual(tag >> 5, 0)
        self.assertEqual(tag & 0x1F, 31)

    def test_decompress_size_mismatch_raises(self):
        comp = _compress_raw_deflate(b"hello world" * 10)
        with self.assertRaises(ValueError):
            nba.decompress_entry_payload(comp, 0, len(comp) + 999, len(comp), 0x0B)


if __name__ == "__main__":
    unittest.main()

