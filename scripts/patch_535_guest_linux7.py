#!/usr/bin/env python3
"""
Clean-room Linux 7.3+ kernel compatibility patcher for NVIDIA 535.309.01-grid
guest kernel modules (verified against Arch Linux 7.3.4-arch1-2 + GCC 16.2.1).

Addresses two Linux 7.3 kernel header / DRM API breaks:
  1. Removal of `strncpy()` from `<linux/string.h>` -> injects `nv_compat_strncpy`.
  2. Rename of `struct drm_atomic_state` -> `struct drm_atomic_commit` and
     `drm_atomic_state_*` -> `drm_atomic_commit_*` in `nvidia-drm`.

License: GPL-2.0-only
"""

from __future__ import annotations

import argparse
from pathlib import Path

STRNCPY_COMPAT = """
#ifndef NV_COMPAT_STRNCPY_DEFINED
#define NV_COMPAT_STRNCPY_DEFINED
static inline char *nv_compat_strncpy(char *dest, const char *src, unsigned long n)
{
    unsigned long i;
    for (i = 0; i < n && src[i] != '\\0'; i++)
        dest[i] = src[i];
    for (; i < n; i++)
        dest[i] = '\\0';
    return dest;
}
#undef strncpy
#define strncpy nv_compat_strncpy
#endif
"""


def patch_kernel_tree(kernel_dir: Path) -> None:
    for rel in (
        "common/inc/nv-linux.h",
        "nvidia-uvm/uvm_linux.h",
        "nvidia-modeset/nvidia-modeset-linux.c",
    ):
        p = kernel_dir / rel
        text = p.read_text(encoding="utf-8")
        if "NV_COMPAT_STRNCPY_DEFINED" not in text:
            if "#include <linux/string.h>" in text:
                text = text.replace("#include <linux/string.h>", "#include <linux/string.h>\n" + STRNCPY_COMPAT, 1)
            elif "#include <linux/kernel.h>" in text:
                text = text.replace("#include <linux/kernel.h>", "#include <linux/kernel.h>\n" + STRNCPY_COMPAT, 1)
            else:
                text = STRNCPY_COMPAT + "\n" + text
            p.write_text(text, encoding="utf-8")

    drm_files = (
        "nvidia-drm/nvidia-drm-crtc.c",
        "nvidia-drm/nvidia-drm-drv.c",
        "nvidia-drm/nvidia-drm-helper.c",
        "nvidia-drm/nvidia-drm-helper.h",
        "nvidia-drm/nvidia-drm-modeset.c",
        "nvidia-drm/nvidia-drm-modeset.h",
    )
    for rel in drm_files:
        p = kernel_dir / rel
        text = p.read_text(encoding="utf-8")
        for old, new in (
            ("struct drm_atomic_state", "struct drm_atomic_commit"),
            ("drm_atomic_state_alloc", "drm_atomic_commit_alloc"),
            ("drm_atomic_state_init", "drm_atomic_commit_init"),
            ("drm_atomic_state_default_clear", "drm_atomic_commit_default_clear"),
            ("drm_atomic_state_default_release", "drm_atomic_commit_default_release"),
            ("drm_atomic_state_put", "drm_atomic_commit_put"),
            ("defined(NV_DRM_PLANE_ATOMIC_CHECK_HAS_ATOMIC_STATE_ARG)", "1"),
            ("defined(NV_DRM_CRTC_ATOMIC_CHECK_HAS_ATOMIC_STATE_ARG)", "1"),
            ("defined(NV_DRM_ATOMIC_STATE_REF_COUNTING_PRESENT)", "1"),
            ("defined(NV_DRM_ATOMIC_HELPER_SWAP_STATE_HAS_STALL_ARG)", "1"),
            ("defined(NV_DRM_ATOMIC_HELPER_SWAP_STATE_RETURN_INT)", "1"),
        ):
            text = text.replace(old, new)
        p.write_text(text, encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser(description="Patch 535.309.01 guest kernel/ tree for Linux 7.3+ DRM & string headers")
    ap.add_argument("kernel_dir", type=Path, help="Path to extracted 535.309.01 kernel/ directory")
    args = ap.parse_args()
    patch_kernel_tree(args.kernel_dir)
    print(f"Patched {args.kernel_dir} for Linux 7.3+ cleanly.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
