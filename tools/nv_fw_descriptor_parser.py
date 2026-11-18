#!/usr/bin/env python3
"""
Clean-room decoder for NVIDIA GP104 (Pascal) Falcon firmware descriptors,
192-byte LSF signature wrappers, GR Netlist TOCs, Option ROM BIT 'p' Falcon tables,
and raw NV_PROM JFFS InfoROM containers.

License: MIT OR GPL-2.0-or-later
"""

from __future__ import annotations

import hashlib
import json
import os
import struct
from dataclasses import dataclass, asdict
from pathlib import Path
from typing import Dict, List, Optional


LSF_XOR_FOOTER = (0xE931C2B6, 0xD777B203, 0x69A0320E, 0x62804E8F)
FALCON_ID_NAMES: Dict[int, str] = {
    0: "PMU",
    1: "DPU",
    2: "FECS",
    3: "GPCCS",
    4: "NVENC",
    5: "NVDEC",
    7: "SEC2",
    10: "GSP_OR_MINION",
}


@dataclass
class FlcnBlDesc:
    start_tag: int
    boot_vec: int
    reserved: int
    dma_idx_or_flags: int
    code_dma_base_or_size: int
    data_off: int
    data_size: int


@dataclass
class AcrHsDesc:
    version: int
    imem_off: int
    imem_size: int
    dmem_size: int
    num_apps: int
    app_code_off: int
    app_code_size: int
    app_data_off: int
    app_data_size: int


@dataclass
class AppUcodeDesc:
    os_code_offset: int
    os_code_size: int
    os_data_offset: int
    os_data_size: int
    num_apps: int
    entries: List[Dict[str, int]]
    tail_pair: Optional[List[int]]


@dataclass
class RtosDesc:
    desc_size: int
    stored_size: int
    flags: int
    changelist: int
    build_date: str
    app_start_addr: int
    app_entry: int
    code_offset: int
    data_offset_span: int
    total_expected_image_bytes: int
    num_overlays: int


@dataclass
class LsfSigWrapper:
    size: int
    falcon_id: int
    falcon_name: str
    footer_xor_words: List[str]
    sha256: str


@dataclass
class VbiosFalconApp:
    index: int
    app_id: int
    target_id: int
    rom_offset: int
    ucode_size: int
    desc_version: int
    sha256: str


@dataclass
class PromInforomRecord:
    name: str
    record_type: int
    flags_words: List[int]
    payload_len: int
    payload_hex_prefix: str
    ascii_strings: List[str]


def parse_flcn_bl_desc(data: bytes) -> FlcnBlDesc:
    if len(data) != 24:
        raise ValueError(f"Expected 24-byte bootloader descriptor, got {len(data)}B")
    w = struct.unpack_from("<6I", data, 0)
    return FlcnBlDesc(
        start_tag=w[0],
        boot_vec=w[0] << 8,
        reserved=w[1],
        dma_idx_or_flags=w[2],
        code_dma_base_or_size=w[3],
        data_off=w[4],
        data_size=w[5],
    )


def parse_acr_hs_desc(data: bytes) -> AcrHsDesc:
    if len(data) != 36:
        raise ValueError(f"Expected 36-byte ACR HS descriptor, got {len(data)}B")
    w = struct.unpack_from("<9I", data, 0)
    return AcrHsDesc(
        version=w[0],
        imem_off=w[1],
        imem_size=w[2],
        dmem_size=w[3],
        num_apps=w[4],
        app_code_off=w[5],
        app_code_size=w[6],
        app_data_off=w[7],
        app_data_size=w[8],
    )


def parse_app_ucode_desc(data: bytes) -> AppUcodeDesc:
    if len(data) < 28 or (len(data) % 4) != 0:
        raise ValueError(f"Unexpected app ucode descriptor size: {len(data)}B")
    words = struct.unpack_from(f"<{len(data)//4}I", data, 0)
    num_apps = words[4]
    pair_end = len(words) - 2
    entries = []
    pos = 5
    while pos + 1 < pair_end:
        entries.append({"offset": words[pos], "size": words[pos + 1]})
        pos += 2
    tail = [words[-2], words[-1]]
    return AppUcodeDesc(
        os_code_offset=words[0],
        os_code_size=words[1],
        os_data_offset=words[2],
        os_data_size=words[3],
        num_apps=num_apps,
        entries=entries,
        tail_pair=tail,
    )


def parse_rtos_desc(data: bytes) -> RtosDesc:
    if len(data) < 0x90:
        raise ValueError(f"RTOS descriptor too small: {len(data)}B")
    w0 = struct.unpack_from("<4I", data, 0)
    build_raw = data[0x10:0x50]
    build_date = build_raw.split(b"\x00", 1)[0].decode("ascii", errors="replace").strip()
    # Flags/version v2 (e.g. SEC2 RTOS 656B) lays app fields from +0x54 (`w5[0]=0x192`),
    # whereas v4 (e.g. PMU RTOS 940B) lays them from +0x58 (`w5[0]=0xff`).
    base = 0x54 if w0[2] <= 2 else 0x58
    w5 = struct.unpack_from("<5I", data, base)
    num_ovl = struct.unpack_from("<I", data, 0x88)[0]
    return RtosDesc(
        desc_size=w0[0],
        stored_size=w0[1],
        flags=w0[2],
        changelist=w0[3],
        build_date=build_date,
        app_start_addr=w5[1],
        app_entry=w5[2],
        code_offset=w5[3],
        data_offset_span=w5[4],
        total_expected_image_bytes=w5[3] + w5[4],
        num_overlays=num_ovl,
    )


def parse_lsf_sig_wrapper(data: bytes) -> LsfSigWrapper:
    if len(data) != 192:
        raise ValueError(f"Expected 192-byte LSF signature wrapper, got {len(data)}B")
    footer = struct.unpack_from("<4I", data, 176)
    decoded = [footer[i] ^ LSF_XOR_FOOTER[i] for i in range(4)]
    fid = decoded[0] if all(w == 0 for w in struct.unpack_from("<4I", data, 160)) and footer[1] == LSF_XOR_FOOTER[1] else -1
    return LsfSigWrapper(
        size=len(data),
        falcon_id=fid,
        falcon_name=FALCON_ID_NAMES.get(fid, "UNKNOWN_OR_UNMASKED"),
        footer_xor_words=[f"0x{w:08x}" for w in decoded],
        sha256=hashlib.sha256(data).hexdigest(),
    )


def parse_vbios_falcon_apps(rom: bytes) -> Dict[str, object]:
    """Parse Option ROM PCIR chain, BIT token 0x70, and embedded Falcon apps."""
    off = 0
    images = []
    efi_gap = 0
    primary = None
    while off + 0x20 <= len(rom) and len(images) < 16:
        if struct.unpack_from("<H", rom, off)[0] != 0xAA55:
            break
        pcir_off = off + struct.unpack_from("<H", rom, off + 0x18)[0]
        if rom[pcir_off:pcir_off + 4] != b"PCIR":
            break
        vid, did = struct.unpack_from("<HH", rom, pcir_off + 4)
        blocks = struct.unpack_from("<H", rom, pcir_off + 0x10)[0]
        code_type = rom[pcir_off + 0x14]
        ind = rom[pcir_off + 0x15]
        img_len = blocks * 512
        images.append({
            "offset": f"0x{off:x}",
            "length": img_len,
            "vendor_id": f"0x{vid:04x}",
            "device_id": f"0x{did:04x}",
            "code_type": f"0x{code_type:02x}",
            "is_last": bool(ind & 0x80),
        })
        if code_type == 0x00 and primary is None:
            primary = (off, off + img_len)
        elif code_type == 0x03:
            efi_gap += img_len
        off += img_len
        if ind & 0x80:
            break

    if primary is None:
        raise ValueError("Missing primary x86/PCI Option ROM image")
    bit_pos = rom.find(b"\xff\xb8BIT\0", primary[0], primary[1])
    if bit_pos == -1:
        raise ValueError("BIT signature not found in primary ROM image")
    hdr_sz, tok_sz, tok_cnt = rom[bit_pos + 8], rom[bit_pos + 9], rom[bit_pos + 10]
    p_tok = None
    for i in range(tok_cnt):
        t_off = bit_pos + hdr_sz + i * tok_sz
        tid, ver, dsz, doff = struct.unpack_from("<BBHH", rom, t_off)
        if tid == 0x70:
            p_tok = (ver, dsz, doff)
            break

    apps: List[VbiosFalconApp] = []
    table_off = None
    if p_tok is not None:
        _, _, falcon_ptr = p_tok
        # BIT 'p' pointers >= primary[1] bypass the EFI GOP image (`+efi_gap`)
        p_data = falcon_ptr + (efi_gap if falcon_ptr >= primary[1] and falcon_ptr + efi_gap < len(rom) else 0)
        ucode_tbl_raw = struct.unpack_from("<I", rom, p_data)[0]
        tbl_off = ucode_tbl_raw + (efi_gap if ucode_tbl_raw >= primary[1] and ucode_tbl_raw + efi_gap < len(rom) else 0)
        table_off = tbl_off
        if tbl_off + 6 <= len(rom):
            ver, hdr_b, ent_b, cnt = rom[tbl_off], rom[tbl_off + 1], rom[tbl_off + 2], rom[tbl_off + 3]
            for i in range(cnt):
                e_off = tbl_off + hdr_b + i * ent_b
                if ent_b >= 6 and e_off + 6 <= len(rom):
                    app_id, tgt_id, raw_ptr = struct.unpack_from("<BBI", rom, e_off)
                    if app_id != 0 and raw_ptr != 0:
                        app_off = raw_ptr + (efi_gap if raw_ptr >= primary[1] and raw_ptr + efi_gap < len(rom) else 0)
                        if app_off + 12 <= len(rom):
                            desc_ver = rom[app_off] | (rom[app_off + 1] << 8)
                            # v1/v2 Falcon ucode header: estimate span up to next section or header fields
                            imem_sz = struct.unpack_from("<I", rom, app_off + 8)[0] if app_off + 16 <= len(rom) else 0
                            dmem_sz = struct.unpack_from("<I", rom, app_off + 16)[0] if app_off + 20 <= len(rom) else 0
                            blob_sha = hashlib.sha256(rom[app_off:min(len(rom), app_off + max(256, imem_sz + dmem_sz))]).hexdigest()
                            apps.append(VbiosFalconApp(
                                index=i,
                                app_id=app_id,
                                target_id=tgt_id,
                                rom_offset=app_off,
                                ucode_size=max(0, min(len(rom) - app_off, imem_sz + dmem_sz)),
                                desc_version=desc_ver,
                                sha256=blob_sha,
                            ))
    return {
        "images": images,
        "efi_gap_bytes": efi_gap,
        "bit_offset": f"0x{bit_pos:x}",
        "falcon_table_offset": f"0x{table_off:x}" if table_off is not None else None,
        "apps": [asdict(a) for a in apps],
    }


def parse_prom_inforom(prom: bytes) -> Dict[str, object]:
    """Parse JFFS InfoROM chunks from a raw NV_PROM dump (`prom_raw.bin`)."""
    jffs_off = prom.find(b"JFFS\x02\x00\x00\x00")
    if jffs_off == -1:
        raise ValueError("JFFS InfoROM superblock not found in PROM image")
    magic, ver, fs_size = struct.unpack_from("<4sII", prom, jffs_off)
    chunks: Dict[str, bytearray] = {}
    meta: Dict[str, Tuple[int, List[int]]] = {}
    pos = jffs_off + 0x80
    end = min(len(prom), jffs_off + fs_size)
    while pos + 16 <= end:
        hdr = prom[pos:pos + 16]
        if hdr[:4] == b"\xff\xff\xff\xff":
            pos += 0x80
            continue
        rec_type = hdr[0]
        obj_name = hdr[1:4].decode("ascii", errors="replace")
        len_and_off, w2, w3 = struct.unpack_from("<III", hdr, 4)
        chunk_off = len_and_off & 0xFFFF
        chunk_len = (len_and_off >> 16) & 0xFFFF
        if rec_type in (0x01, 0x02, 0x04, 0x06) and obj_name.isalnum() and 0 < chunk_len <= 0x1000 and pos + 16 + chunk_len <= len(prom):
            buf = chunks.setdefault(obj_name, bytearray())
            need = chunk_off + chunk_len
            if len(buf) < need:
                buf.extend(b"\x00" * (need - len(buf)))
            buf[chunk_off:need] = prom[pos + 16:pos + 16 + chunk_len]
            meta[obj_name] = (rec_type, [w2, w3])
            pos += 0x80
        else:
            pos += 0x80

    records: List[PromInforomRecord] = []
    for name, data in chunks.items():
        rt, fl = meta[name]
        raw_bytes = bytes(data)
        strs = []
        cur = bytearray()
        for b in raw_bytes:
            if 32 <= b < 127:
                cur.append(b)
            else:
                if len(cur) >= 3:
                    strs.append(cur.decode("ascii"))
                cur.clear()
        if len(cur) >= 3:
            strs.append(cur.decode("ascii"))
        records.append(PromInforomRecord(
            name=name,
            record_type=rt,
            flags_words=fl,
            payload_len=len(raw_bytes),
            payload_hex_prefix=raw_bytes[:32].hex(),
            ascii_strings=strs[:8],
        ))
    return {
        "jffs_offset": f"0x{jffs_off:x}",
        "version": ver,
        "fs_size": fs_size,
        "records": [asdict(r) for r in records],
    }


def inspect_gp104_blob_directory(root_dir: Path) -> Dict[str, object]:
    """Inspect all extracted GP104 firmware blobs, descriptors, VBIOS, and PROM."""
    out: Dict[str, object] = {"root": str(root_dir), "ls_signatures": [], "descriptors": {}}
    bin_paths: List[Path] = []
    for cur_dir, _, filenames in os.walk(root_dir, followlinks=True):
        for fn in filenames:
            if fn.endswith(".bin"):
                bin_paths.append(Path(cur_dir) / fn)
    for p in sorted(bin_paths, key=lambda x: x.relative_to(root_dir).as_posix()):
        rel = p.relative_to(root_dir).as_posix()
        data = p.read_bytes()
        if rel.endswith("ls_sig.bin") or rel.endswith("ls_sig_alt.bin") or (rel.endswith("id_0x27.bin") and len(data) == 192):
            sig = parse_lsf_sig_wrapper(data)
            rec = asdict(sig)
            rec["file"] = rel
            out["ls_signatures"].append(rec)
        elif len(data) == 24 and ("_bl" in rel or "bl_desc" in rel):
            out["descriptors"][rel] = asdict(parse_flcn_bl_desc(data))
        elif len(data) == 36 and rel.endswith("id_0x18.bin"):
            out["descriptors"][rel] = asdict(parse_acr_hs_desc(data))
        elif len(data) in (204, 236, 364):
            out["descriptors"][rel] = asdict(parse_app_ucode_desc(data))
        elif len(data) in (656, 940) and rel.endswith("id_0x12.bin"):
            out["descriptors"][rel] = asdict(parse_rtos_desc(data))

    vbios_path = root_dir / "vbios" / "gp104_vbios.rom"
    if vbios_path.exists():
        out["vbios"] = parse_vbios_falcon_apps(vbios_path.read_bytes())

    prom_path = root_dir / "vbios" / "prom_raw.bin"
    if prom_path.exists():
        out["prom_inforom"] = parse_prom_inforom(prom_path.read_bytes())

    return out


def main() -> int:
    import argparse
    ap = argparse.ArgumentParser(description="Clean-room NVIDIA GP104 firmware descriptor & InfoROM inspector")
    ap.add_argument("blob_dir", type=Path, help="Extracted GP104 firmware blob directory")
    ap.add_argument("-o", "--output", type=Path, default=None, help="Optional UTF-8 JSON output file")
    args = ap.parse_args()
    payload = json.dumps(inspect_gp104_blob_directory(args.blob_dir), indent=2) + "\n"
    if args.output is not None:
        args.output.write_text(payload, encoding="utf-8")
    else:
        print(payload, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
