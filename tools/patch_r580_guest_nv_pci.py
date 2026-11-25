#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Idempotently patch R580 (580.196.07-grid) kernel/nvidia/nv-pci.c to allow Tesla P4 (10de:1bb3)."""

import argparse
import pathlib
import sys

MARKER = "OPEN_VGPU_R580_PASCAL_ALLOWLIST"

OLD_SNIPPET = """    if ((rm_is_supported_device(sp, nv)) != NV_OK)
        goto err_not_supported;"""

NEW_SNIPPET = """    if ((rm_is_supported_device(sp, nv)) != NV_OK) {
        /* %s: 580.196.07 _nv054166rm dropped 10de:1bb3 despite intact GP104 vGPU RM */
        if (pci_dev->vendor == 0x10de && pci_dev->device == 0x1bb3)
            nv_printf(NV_DBG_WARNINGS, "NVRM: Allowing Tesla P4 (10de:1bb3) on R580 vGPU guest.\\n");
        else
            goto err_not_supported;
    }""" % MARKER


def patch_nv_pci(kernel_dir: pathlib.Path, dry_run: bool = False) -> tuple[bool, str]:
    """Patch <kernel_dir>/nvidia/nv-pci.c if unpatched; idempotent across repeated runs."""
    nv_pci = kernel_dir / "nvidia" / "nv-pci.c"
    if not nv_pci.is_file():
        raise FileNotFoundError(f"Missing {nv_pci}")

    text = nv_pci.read_text(encoding="utf-8")
    if MARKER in text:
        return False, f"No patch needed: {nv_pci} already contains {MARKER}"

    count = text.count(OLD_SNIPPET)
    if count != 1:
        raise RuntimeError(
            f"Expected 1 occurrence of rm_is_supported_device check in {nv_pci}, found {count}"
        )

    patched = text.replace(OLD_SNIPPET, NEW_SNIPPET, 1)
    if not dry_run:
        orig = nv_pci.with_name(nv_pci.name + ".orig")
        if not orig.exists():
            orig.write_text(text, encoding="utf-8")
        nv_pci.write_text(patched, encoding="utf-8")
    return True, f"Patched {nv_pci} for Tesla P4 (10de:1bb3) on R580 guest"


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Allow Tesla P4 (10de:1bb3) in extracted R580 GRID guest kernel_dir/nvidia/nv-pci.c"
    )
    parser.add_argument(
        "kernel_dir",
        type=pathlib.Path,
        help="Path to extracted R580 driver kernel/ directory (containing nvidia/nv-pci.c)",
    )
    parser.add_argument("--dry-run", action="store_true", help="Validate match without writing")
    args = parser.parse_args()

    try:
        _, msg = patch_nv_pci(args.kernel_dir, dry_run=args.dry_run)
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    print(msg)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
