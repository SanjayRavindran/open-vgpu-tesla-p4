#!/usr/bin/env python3
"""
Clean-room GP104 Falcon blob extractor for NVIDIA KVM vGPU 535.309.01.

Reads the SHA-256 + ELF `.rodata` manifest in
`firmware-blobs/gp104-vgpu-535/BLOB_MANIFEST.json`, extracts every Falcon
archive/single entry and `GR` Netlist section from user-supplied
`nv-kernel.o_binary` (using `ElfBindataParser` from `nv_bindata_analyzer.py`),
and strictly verifies every SHA-256 digest.

License: GPL-2.0-only
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path

from nv_bindata_analyzer import ElfBindataParser, unpack_gr_netlist


def verify_sha256(data: bytes, expected: str, label: str) -> None:
    got = hashlib.sha256(data).hexdigest()
    if got != expected:
        raise ValueError(f"SHA-256 mismatch for {label}: expected {expected}, got {got}")


def extract_from_manifest(elf_path: Path, manifest_path: Path, out_dir: Path) -> dict:
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    parser = ElfBindataParser(elf_path)
    out_dir.mkdir(parents=True, exist_ok=True)

    extracted = []

    for archive_name, arc_spec in manifest.get("archives", {}).items():
        st_off = int(arc_spec["rodata_offset"], 16)
        cnt, pad0 = struct.unpack_from("<II", parser.rodata, st_off)
        if pad0 != 0 or not (1 <= cnt <= 128):
            raise ValueError(f"Unexpected archive header for {archive_name} at .rodata+0x{st_off:x}")
        entries_spec = {e["index"]: e for e in arc_spec["entries"]}
        if cnt < len(entries_spec):
            raise ValueError(f"Archive {archive_name} has {cnt} items, expected >= {len(entries_spec)}")

        for idx, e_spec in entries_spec.items():
            entry_off = parser.ro_relocs[st_off + 16 + idx * 16][1]
            parsed = parser.parse_entry(entry_off, with_payload=True)
            if parsed is None:
                raise ValueError(f"Failed parsing archive entry {idx} at 0x{entry_off:x} in {archive_name}")
            _, payload = parsed
            verify_sha256(payload, e_spec["sha256"], e_spec["file"])
            dst = out_dir / e_spec["file"]
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(payload)
            extracted.append({"file": e_spec["file"], "size": len(payload), "sha256": e_spec["sha256"]})

    for single_name, s_spec in manifest.get("singles", {}).items():
        e_off = int(s_spec["rodata_offset"], 16)
        parsed = parser.parse_entry(e_off, with_payload=True)
        if parsed is None:
            raise ValueError(f"Failed parsing single {single_name} at .rodata+0x{e_off:x}")
        _, payload = parsed
        verify_sha256(payload, s_spec["sha256"], s_spec["file"])
        dst = out_dir / s_spec["file"]
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_bytes(payload)
        extracted.append({"file": s_spec["file"], "size": len(payload), "sha256": s_spec["sha256"]})

        if "netlist_sections" in s_spec:
            sec_dir = dst.parent / "netlist_sections"
            sections_meta = unpack_gr_netlist(payload, sec_dir)
            expected_secs = {s["index"]: s for s in s_spec["netlist_sections"]}
            for sec in sections_meta:
                exp = expected_secs.get(sec["index"])
                if exp and sec["sha256"] != exp["sha256"]:
                    raise ValueError(
                        f"Netlist section {sec['file']} SHA-256 mismatch: expected {exp['sha256']}, got {sec['sha256']}"
                    )

    return {
        "elf_path": str(elf_path),
        "manifest": str(manifest_path),
        "out_dir": str(out_dir),
        "verified_files": len(extracted),
    }


def main() -> int:
    repo_root = Path(__file__).resolve().parent.parent
    ap = argparse.ArgumentParser(description="Extract and SHA-256-verify GP104 Falcon blobs from 535.309.01 vgpu-kvm nv-kernel.o_binary")
    ap.add_argument("elf_path", type=Path, help="Path to extracted 535.309.01 kernel/nvidia/nv-kernel.o_binary")
    ap.add_argument(
        "--manifest",
        type=Path,
        default=repo_root / "firmware-blobs" / "gp104-vgpu-535" / "BLOB_MANIFEST.json",
        help="Path to BLOB_MANIFEST.json",
    )
    ap.add_argument(
        "--out-dir",
        type=Path,
        default=repo_root / "firmware-blobs" / "gp104-vgpu-535",
        help="Output directory for verified GP104 Falcon blobs",
    )
    args = ap.parse_args()
    summary = extract_from_manifest(args.elf_path, args.manifest, args.out_dir)
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
