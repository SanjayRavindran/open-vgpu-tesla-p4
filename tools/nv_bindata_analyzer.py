#!/usr/bin/env python3
"""
Clean-room ELF64 `.rodata` bindata analyzer & Falcon firmware cataloger
for NVIDIA Linux kernel binary core (`nv-kernel.o_binary`).

This script does NOT contain or execute proprietary NVIDIA code; it statically
parses standard ELF64 relocatable object sections (`.rodata`, `.rela.rodata`,
`.rela.text`, `.symtab`, `.strtab`) and extracts documented raw-DEFLATE
(`zlib` wbits=-15) firmware containers and descriptors.

License: MIT OR GPL-2.0-or-later
"""

from __future__ import annotations

import argparse
import bisect
import hashlib
import json
import os
import re
import struct
import zlib
from dataclasses import dataclass, asdict
from pathlib import Path
from typing import Dict, List, Optional, Tuple


ELF64_EHDR_FMT = "<16sHHIQQQIHHHHHH"
ELF64_SHDR_FMT = "<IIQQQQIIQQ"
ELF64_SYM_FMT = "<IBBHQQ"
ELF64_RELA_FMT = "<QQq"

SHF_ALLOC = 0x2
SHT_RELA = 4
STT_FUNC = 2

# Verified GP104 (Pascal, arch=0x13 impl=0x4 -> HAL_IMPL=0x1f) BindataStorage archives
# resolved directly by executing the decompiled 580.178.04 HAL initializers on 0x1f.
KNOWN_GP104_RODATA_ARCHIVES: Dict[str, int] = {
    "acr/gp104_acr_bl": 0x29068,
    "acr/gp104_acr_load": 0x25AF0,          # _nv005947rm +0x01f0 (_nv016888rm)
    "acr/gp104_acr_unload": 0x231D0,        # _nv005947rm +0x0358 (_nv016860rm)
    "acr/gp104_acr_unload_sig": 0x23140,    # _nv005947rm +0x0360 (_nv016858rm)
    "acr/gp104_acr_slot_2a0": 0x248A8,      # _nv005947rm +0x02a0 (_nv016881rm)
    "acr/gp104_acr_slot_2a8": 0x24268,      # _nv005947rm +0x02a8 (_nv016884rm)
    "acr/gp104_acr_slot_2b0": 0x24200,      # _nv005947rm +0x02b0 (_nv016886rm)
    "acr/gp104_acr_bsi_lock": 0x23D60,      # _nv005947rm +0x02d0 (_nv016852rm)
    "acr/gp104_acr_slot_2e0_sig": 0x23A88,  # _nv005947rm +0x02e0 (_nv016843rm)
    "acr/gp104_acr_slot_300": 0x237E0,      # _nv005947rm +0x0300 (_nv016901rm)
    "acr/gp104_acr_slot_308": 0x23778,      # _nv005947rm +0x0308 (_nv016903rm)
    "acr/gp104_acr_slot_310": 0x23728,      # _nv005947rm +0x0310 (_nv016809rm)
    "acr/gp104_acr_slot_318_sig": 0x23650,  # _nv005947rm +0x0318 (_nv016806rm)
    "acr/gp104_acr_slot_320": 0x235E8,      # _nv005947rm +0x0320 (_nv016792rm)
    "acr/gp104_acr_slot_328": 0x23580,      # _nv005947rm +0x0328 (_nv016794rm)
    "acr/gp104_acr_slot_340": 0x23460,      # _nv005947rm +0x0340 (_nv016869rm)
    "acr/gp104_acr_slot_348": 0x23260,      # _nv005947rm +0x0348 (_nv016862rm)
    "acr/gp104_acr_slot_350": 0x231F8,      # _nv005947rm +0x0350 (_nv016866rm)
    "pmu/gp104_pmu_rtos": 0x26108,          # _nv006035rm +0x0318 (_nv052098rm)
    "pmu/gp104_pmu_bl": 0x25C80,            # _nv006035rm +0x0370 (_nv052066rm)
    "sec2/gp104_sec2_rtos": 0x22E68,        # _nv006010rm +0x02d8 (_nv055720rm)
    "sec2/gp104_sec2_hs": 0x22FD8,          # _nv006010rm +0x02c8 (_nv055671rm)
    "sec2/gp104_sec2_bl": 0x22B78,          # _nv006010rm +0x03a0 (_nv055663rm)
    "nvenc/gp104_nvenc_hs_scrub": 0x298B8,  # _nv005951rm +0x0260 (_nv018229rm)
}

# Verified single-entry bindata blobs for HAL_IMPL=0x1f (GP104).
KNOWN_GP104_SINGLE_ENTRIES: Dict[str, int] = {
    "pmu/gp104_pmu_ls_sig": 0x2DAC0,          # _nv006035rm +0x0378 (_nv052124rm, fid=0)
    "nvenc/gp104_nvenc_ls_sig": 0x29F80,      # _nv005951rm +0x02e8 (_nv018350rm, fid=4)
    "nvenc/gp104_nvenc0_ucode": 0x29AE8,      # _nv005951rm +0x0278 (_nv018314rm)
    "nvenc/gp104_nvenc1_ucode": 0x29B00,      # _nv005951rm +0x0288 (_nv018327rm)
    "nvenc/gp104_nvenc0_desc": 0x29AB8,       # _nv005951rm +0x02c0 (_nv018288rm)
    "nvenc/gp104_nvenc1_desc": 0x29AD0,       # _nv005951rm +0x02c8 (_nv018301rm)
    "nvdec/gp104_nvdec_ucode": 0x2D640,       # _nv006002rm +0x0218 (_nv046336rm)
    "nvdec/gp104_nvdec_desc": 0x2D778,        # _nv006002rm +0x0230 (_nv046326rm)
    "gr/gp104_fecs_ls_sig": 0x2B660,          # _nv005875rm +0x00c8 (_nv027459rm, fid=2)
    "gr/gp104_fecs_bl_code": 0x2BC90,         # _nv005875rm +0x0130 (_nv027386rm)
    "gr/gp104_fecs_bl_desc": 0x2BD98,         # _nv005875rm +0x0138 (_nv027375rm)
    "gr/gp104_gpccs_ls_sig": 0x2C800,         # _nv005879rm +0x00d8 (_nv029721rm, fid=3)
    "gr/gp104_gpccs_ls_sig_alt": 0x2C620,     # _nv005879rm +0x01a8 (_nv029864rm->29707)
    "gr/gp104_gpccs_bl_code": 0x2CE18,        # _nv005879rm +0x0140 (_nv029649rm)
    "gr/gp104_gpccs_bl_desc": 0x2CF08,        # _nv005879rm +0x0148 (_nv029639rm)
    "gr/gp104_gr_netlist": 0x2AE98,           # _nv005889rm +0x12b8 (_nv032828rm, Jun 28 2019)
}

GR_NETLIST_SECTION_NAMES: Dict[int, str] = {
    0: "fecs_data",
    1: "fecs_inst",
    2: "gpccs_data",
    3: "gpccs_inst",
    4: "sw_bundle_init",
    5: "sw_method_init",
    6: "sw_ctx_load",
    7: "sw_nonctx_load",
    8: "sw_veid_bundle_init",
    9: "sw_bundle64_init",
}

CHIP_TAG_MAP_BY_ARCH_IMPL: Dict[Tuple[int, int], Tuple[str, int]] = {
    (0x11, 0x7): ("GM107", 0x18),
    (0x11, 0x8): ("GM108", 0x19),
    (0x12, 0x0): ("GM200", 0x1A),
    (0x12, 0x4): ("GM204", 0x1B),
    (0x12, 0x6): ("GM206", 0x1C),
    (0x13, 0x0): ("GP100", 0x1D),
    (0x13, 0x2): ("GP102", 0x1E),
    (0x13, 0x4): ("GP104", 0x1F),
    (0x13, 0x6): ("GP106", 0x20),
    (0x13, 0x7): ("GP107", 0x21),
    (0x13, 0x8): ("GP108", 0x22),
    (0x14, 0x0): ("GV100", 0x23),
    (0x16, 0x2): ("TU102", 0x25),
    (0x16, 0x4): ("TU104", 0x26),
    (0x16, 0x6): ("TU106", 0x27),
    (0x16, 0x8): ("TU116", 0x28),
    (0x16, 0x7): ("TU117", 0x29),
    (0x17, 0x0): ("GA100", 0x2A),
    (0x17, 0x2): ("GA102", 0x2B),
    (0x17, 0x3): ("GA103", 0x2C),
    (0x17, 0x4): ("GA104", 0x2D),
    (0x17, 0x6): ("GA106", 0x2E),
    (0x17, 0x7): ("GA107", 0x2F),
    (0x19, 0x2): ("AD102", 0x34),
    (0x19, 0x3): ("AD103", 0x35),
    (0x19, 0x4): ("AD104", 0x36),
    (0x19, 0x6): ("AD106", 0x37),
    (0x19, 0x7): ("AD107", 0x38),
}


@dataclass
class Section:
    index: int
    name: str
    sh_type: int
    sh_flags: int
    sh_addr: int
    sh_offset: int
    sh_size: int
    sh_link: int
    sh_info: int
    sh_addralign: int
    sh_entsize: int


@dataclass
class Symbol:
    index: int
    name: str
    st_info: int
    st_other: int
    st_shndx: int
    st_value: int
    st_size: int


@dataclass
class BindataEntry:
    rodata_offset: int
    uncompressed_size: int
    compressed_size: int
    data_offset: int
    flags: int
    cumulative_offset: int
    compressed: bool
    sha256: str
    hints: List[str]


@dataclass
class BindataArchive:
    rodata_offset: int
    count: int
    items: List[Tuple[int, BindataEntry]]
    referenced_by_funcs: List[str]


def unpack_gr_netlist(payload: bytes, out_dir: Path) -> List[dict]:
    """Decode and unpack a packed NVIDIA GR Netlist TOC container (`magic == 0`)."""
    if len(payload) < 8:
        raise ValueError("GR netlist payload too small")
    magic, cnt = struct.unpack_from("<II", payload, 0)
    if magic != 0 or cnt == 0 or cnt > 64 or 8 + cnt * 12 > len(payload):
        raise ValueError(f"Unexpected GR netlist TOC header: magic={magic:#x} cnt={cnt}")
    out_dir.mkdir(parents=True, exist_ok=True)
    sections_meta: List[dict] = []
    for idx in range(cnt):
        sec_type, sec_size, sec_off = struct.unpack_from("<III", payload, 8 + idx * 12)
        sec_bytes = payload[sec_off:sec_off + sec_size]
        label = GR_NETLIST_SECTION_NAMES.get(sec_type, f"type_{sec_type:02d}")
        fname = f"sec_{idx:02d}_{label}.bin"
        (out_dir / fname).write_bytes(sec_bytes)
        sections_meta.append({
            "index": idx,
            "type": sec_type,
            "label": label,
            "offset": f"0x{sec_off:x}",
            "size": sec_size,
            "sha256": hashlib.sha256(sec_bytes).hexdigest(),
            "file": fname,
        })
    return sections_meta


def _read_cstr(blob: bytes, offset: int) -> str:
    if offset <= 0 or offset >= len(blob):
        return ""
    end = blob.find(b"\x00", offset)
    if end == -1:
        end = len(blob)
    return blob[offset:end].decode("utf-8", errors="replace")


def decompress_entry_payload(rodata: bytes, data_offset: int, uncompressed_size: int,
                             compressed_size: int, flags: int) -> bytes:
    """Decode one BindataEntry payload (raw deflate when flags bit 0 is set)."""
    if flags & 0x1:
        bound = compressed_size if 0 < compressed_size <= len(rodata) - data_offset else len(rodata) - data_offset
        chunk = rodata[data_offset:data_offset + bound]
        decomp = zlib.decompressobj(-15)
        out = decomp.decompress(chunk, uncompressed_size)
        out += decomp.flush()
        if len(out) != uncompressed_size:
            raise ValueError(
                f"Decompressed size mismatch at .rodata+0x{data_offset:x}: "
                f"expected {uncompressed_size}, got {len(out)}"
            )
        return out
    end = data_offset + uncompressed_size
    if end > len(rodata):
        raise ValueError(f"Uncompressed slice out of range at .rodata+0x{data_offset:x}")
    return rodata[data_offset:end]


def extract_ascii_hints(payload: bytes, limit: int = 4) -> List[str]:
    matches = re.findall(rb"[A-Za-z0-9_ .:/-]{8,}", payload)
    out: List[str] = []
    for m in matches:
        s = m.decode("ascii", errors="ignore").strip()
        if any(k in s.lower() for k in ("201", "202", "mon ", "tue ", "wed ", "thu ", "fri ", "sat ", "sun ", "nv", "pmu", "acr", "sec2", "nvrng", "nvpka")):
            out.append(s)
            if len(out) >= limit:
                break
    return out


class ElfBindataParser:
    """Static ELF64 ET_REL parser for NVIDIA RM bindata tables."""

    def __init__(self, elf_path: str | Path):
        self.elf_path = Path(elf_path)
        self.raw = self.elf_path.read_bytes()
        self.sections: Dict[str, Section] = {}
        self.sections_by_idx: List[Section] = []
        self.symbols: List[Symbol] = []
        self.symbols_by_name: Dict[str, Symbol] = {}
        self.rodata: bytes = b""
        self.rodata_idx: int = -1
        self.text_idx: int = -1
        self.ro_relocs: Dict[int, Tuple[int, int, str]] = {}
        self.tx_to_ro: Dict[int, List[str]] = {}
        self._func_starts: List[int] = []
        self._func_ranges: List[Tuple[int, int, str]] = []
        self._parse_elf()

    def _parse_elf(self) -> None:
        if len(self.raw) < 64 or self.raw[:4] != b"\x7fELF":
            raise ValueError(f"{self.elf_path} is not a valid ELF file")
        eh = struct.unpack_from(ELF64_EHDR_FMT, self.raw, 0)
        ei_ident = eh[0]
        if ei_ident[4] != 2 or ei_ident[5] != 1:
            raise ValueError("Only 64-bit little-endian ELF objects are supported")
        e_shoff = eh[6]
        e_shentsize, e_shnum, e_shstrndx = eh[11], eh[12], eh[13]

        raw_shdrs = []
        for idx in range(e_shnum):
            off = e_shoff + idx * e_shentsize
            raw_shdrs.append(struct.unpack_from(ELF64_SHDR_FMT, self.raw, off))

        shstr_off, shstr_sz = raw_shdrs[e_shstrndx][4], raw_shdrs[e_shstrndx][5]
        shstr = self.raw[shstr_off:shstr_off + shstr_sz]
        for idx, sh in enumerate(raw_shdrs):
            end = shstr.find(b"\x00", sh[0])
            name = shstr[sh[0]:end].decode("utf-8", errors="replace") if end != -1 else ""
            sec = Section(idx, name, *sh[1:])
            self.sections[name] = sec
            self.sections_by_idx.append(sec)

        if ".rodata" not in self.sections or ".symtab" not in self.sections:
            raise ValueError("Missing required .rodata or .symtab section")

        ro_sec = self.sections[".rodata"]
        self.rodata_idx = ro_sec.index
        self.rodata = self.raw[ro_sec.sh_offset:ro_sec.sh_offset + ro_sec.sh_size]
        if ".text" in self.sections:
            self.text_idx = self.sections[".text"].index

        sym_sec = self.sections[".symtab"]
        str_sec = self.sections_by_idx[sym_sec.sh_link]
        symtab = self.raw[sym_sec.sh_offset:sym_sec.sh_offset + sym_sec.sh_size]
        strtab = self.raw[str_sec.sh_offset:str_sec.sh_offset + str_sec.sh_size]

        for idx in range(len(symtab) // 24):
            st_name, st_info, st_other, st_shndx, st_val, st_size = struct.unpack_from(ELF64_SYM_FMT, symtab, idx * 24)
            name = _read_cstr(strtab, st_name) if st_name else ""
            sym = Symbol(idx, name, st_info, st_other, st_shndx, st_val, st_size)
            self.symbols.append(sym)
            if name:
                self.symbols_by_name[name] = sym
            if st_shndx == self.text_idx and st_size > 0 and name:
                self._func_ranges.append((st_val, st_val + st_size, name))

        self._func_ranges.sort()
        self._func_starts = [r[0] for r in self._func_ranges]

        if ".rela.rodata" in self.sections:
            rsec = self.sections[".rela.rodata"]
            rel = self.raw[rsec.sh_offset:rsec.sh_offset + rsec.sh_size]
            for pos in range(0, len(rel), 24):
                r_off, r_info, r_add = struct.unpack_from(ELF64_RELA_FMT, rel, pos)
                sym = self.symbols[r_info >> 32]
                self.ro_relocs[r_off] = (sym.st_shndx, sym.st_value + r_add, sym.name)

        if ".rela.text" in self.sections:
            rsec = self.sections[".rela.text"]
            rel = self.raw[rsec.sh_offset:rsec.sh_offset + rsec.sh_size]
            for pos in range(0, len(rel), 24):
                r_off, r_info, r_add = struct.unpack_from(ELF64_RELA_FMT, rel, pos)
                sym = self.symbols[r_info >> 32]
                if sym.st_shndx == self.rodata_idx:
                    tgt = sym.st_value + r_add
                    fn = self.lookup_func(r_off)
                    self.tx_to_ro.setdefault(tgt, []).append(fn)

    def lookup_func(self, text_offset: int) -> str:
        i = bisect.bisect_right(self._func_starts, text_offset) - 1
        if i >= 0:
            s, e, nm = self._func_ranges[i]
            if s <= text_offset < e:
                return f"{nm}+0x{text_offset - s:x}"
        return f"text+0x{text_offset:x}"

    def parse_entry(self, rodata_offset: int, with_payload: bool = False) -> Optional[Tuple[BindataEntry, bytes]]:
        if rodata_offset < 0 or rodata_offset + 24 > len(self.rodata):
            return None
        rel = self.ro_relocs.get(rodata_offset + 8)
        if not rel or rel[0] != self.rodata_idx:
            return None
        usz, csz = struct.unpack_from("<II", self.rodata, rodata_offset)
        fl, cum = struct.unpack_from("<II", self.rodata, rodata_offset + 16)
        data_off = rel[1]
        if usz == 0 or usz > 0x4000000 or data_off < 0 or data_off >= len(self.rodata):
            return None
        if (fl & ~0xFF) != 0:
            return None
        try:
            payload = decompress_entry_payload(self.rodata, data_off, usz, csz, fl)
        except Exception:
            return None
        sha = hashlib.sha256(payload).hexdigest()
        hints = extract_ascii_hints(payload)
        entry = BindataEntry(
            rodata_offset=rodata_offset,
            uncompressed_size=usz,
            compressed_size=csz,
            data_offset=data_off,
            flags=fl,
            cumulative_offset=cum,
            compressed=bool(fl & 1),
            sha256=sha,
            hints=hints,
        )
        return entry, (payload if with_payload else b"")

    def scan_archives(self) -> List[BindataArchive]:
        """Discover all BindataStorage tables inside `.rodata`."""
        archives: List[BindataArchive] = []
        seen_offsets = set()
        for r_off in sorted(self.ro_relocs):
            if r_off < 16 or (r_off % 8) != 0:
                continue
            st_off = r_off - 16
            if st_off in seen_offsets or st_off + 24 > len(self.rodata):
                continue
            cnt, pad0 = struct.unpack_from("<II", self.rodata, st_off)
            if not (1 <= cnt <= 128 and pad0 == 0):
                continue
            if st_off + 8 + cnt * 16 > len(self.rodata):
                continue

            items: List[Tuple[int, BindataEntry]] = []
            valid = True
            for i in range(cnt):
                id_i, pad_i = struct.unpack_from("<II", self.rodata, st_off + 8 + i * 16)
                rel = self.ro_relocs.get(st_off + 16 + i * 16)
                if pad_i != 0 or id_i >= 0x200 or not rel or rel[0] != self.rodata_idx:
                    valid = False
                    break
                parsed = self.parse_entry(rel[1], with_payload=False)
                if parsed is None:
                    valid = False
                    break
                items.append((id_i, parsed[0]))
            if valid:
                seen_offsets.add(st_off)
                refs = sorted(set(self.tx_to_ro.get(st_off, [])))
                archives.append(BindataArchive(st_off, cnt, items, refs))
        return archives

    def extract_gp104_firmware(self, out_dir: str | Path) -> Dict[str, dict]:
        """Extract verified GP104 Falcon firmware blobs & metadata to `out_dir`."""
        out_root = Path(out_dir)
        out_root.mkdir(parents=True, exist_ok=True)
        manifest: Dict[str, dict] = {}

        for name, st_off in KNOWN_GP104_RODATA_ARCHIVES.items():
            cnt, pad0 = struct.unpack_from("<II", self.rodata, st_off)
            if pad0 != 0 or not (1 <= cnt <= 32):
                raise ValueError(f"Unexpected header at {name} (.rodata+0x{st_off:x})")
            sub_dir = out_root / name
            sub_dir.mkdir(parents=True, exist_ok=True)
            records = []
            for i in range(cnt):
                id_i, _ = struct.unpack_from("<II", self.rodata, st_off + 8 + i * 16)
                entry_off = self.ro_relocs[st_off + 16 + i * 16][1]
                parsed = self.parse_entry(entry_off, with_payload=True)
                if parsed is None:
                    raise ValueError(f"Failed parsing entry 0x{entry_off:x} in {name}")
                entry, payload = parsed
                fname = f"id_0x{id_i:02x}.bin"
                (sub_dir / fname).write_bytes(payload)
                rec = asdict(entry)
                rec["bindata_id"] = f"0x{id_i:02x}"
                rec["file"] = f"{name}/{fname}"
                records.append(rec)
            manifest[name] = {
                "rodata_offset": f"0x{st_off:x}",
                "count": cnt,
                "entries": records,
            }

        for name, e_off in KNOWN_GP104_SINGLE_ENTRIES.items():
            parsed = self.parse_entry(e_off, with_payload=True)
            if parsed is None:
                raise ValueError(f"Failed parsing single entry 0x{e_off:x} for {name}")
            entry, payload = parsed
            fname = f"{name}.bin"
            dst_path = out_root / fname
            dst_path.parent.mkdir(parents=True, exist_ok=True)
            dst_path.write_bytes(payload)
            rec = asdict(entry)
            rec["file"] = fname
            if name == "gr/gp104_gr_netlist":
                rec["netlist_sections"] = unpack_gr_netlist(
                    payload, dst_path.parent / "netlist_sections"
                )
            manifest[name] = rec

        (out_root / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
        return manifest


def main() -> int:
    ap = argparse.ArgumentParser(description="NVIDIA RM .rodata bindata static analyzer & extractor")
    ap.add_argument("elf_path", type=Path, help="Path to nv-kernel.o_binary (or compatible ET_REL ELF)")
    ap.add_argument("--summary", action="store_true", help="Print JSON summary of all discovered BindataStorage archives")
    ap.add_argument("--extract-gp104", type=Path, default=None, help="Directory to extract GP104 signed Falcon blobs")
    args = ap.parse_args()

    parser = ElfBindataParser(args.elf_path)
    result: Dict[str, object] = {
        "elf_path": str(args.elf_path),
        "rodata_bytes": len(parser.rodata),
        "ro_relocs": len(parser.ro_relocs),
    }

    if args.summary or args.extract_gp104 is None:
        archives = parser.scan_archives()
        result["archive_count"] = len(archives)
        result["compressed_entries"] = sum(
            1 for a in archives for _, e in a.items if e.compressed
        )
        result["total_uncompressed_bytes"] = sum(
            e.uncompressed_size for a in archives for _, e in a.items
        )
        result["sample_archives"] = [
            {
                "rodata_offset": f"0x{a.rodata_offset:x}",
                "count": a.count,
                "refs": a.referenced_by_funcs[:3],
                "items": [{"id": f"0x{id_i:02x}", "size": e.uncompressed_size, "sha256": e.sha256[:12]} for id_i, e in a.items],
            }
            for a in archives[:20]
        ]

    if args.extract_gp104 is not None:
        result["gp104_manifest"] = parser.extract_gp104_firmware(args.extract_gp104)

    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

