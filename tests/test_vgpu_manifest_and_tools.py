#!/usr/bin/env python3
"""Unit tests verifying clean-room repo hygiene, BLOB_MANIFEST.json, and extractor tools."""

import json
import shutil
import subprocess
import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent


class TestOpenPascalVgpuRepo(unittest.TestCase):
    def test_no_proprietary_binaries_tracked_or_present(self):
        forbidden_exts = {".bin", ".rom", ".raw", ".run", ".o_binary", ".exe", ".deb", ".rpm", ".ko"}
        offenders = []
        git_bin = shutil.which("git")
        if (REPO_ROOT / ".git").exists() and git_bin:
            res = subprocess.run([git_bin, "-C", str(REPO_ROOT), "ls-files"], capture_output=True, text=True, check=True)
            paths = [REPO_ROOT / line for line in res.stdout.splitlines() if line.strip()]
        else:
            paths = [
                p for p in REPO_ROOT.rglob("*")
                if p.is_file()
                and ".git" not in p.parts
                and "target" not in p.parts
                and "blobs" not in p.parts
                and not (
                    "firmware-blobs" in p.parts
                    and p.name not in ("BLOB_MANIFEST.json", "README.md")
                )
            ]
        for p in paths:
            if p.suffix.lower() in forbidden_exts or (
                p.suffix.lower() != ".json"
                and (p.suffix.lower() == ".so" or ".so." in p.name)
            ):
                offenders.append(str(p.relative_to(REPO_ROOT)))
        self.assertEqual(offenders, [], f"Found unexpected binary artifacts in tracked tree: {offenders}")

    def test_blob_manifest_structure(self):
        manifest_path = REPO_ROOT / "firmware-blobs" / "gp104-vgpu-535" / "BLOB_MANIFEST.json"
        data = json.loads(manifest_path.read_text(encoding="utf-8"))
        self.assertEqual(data["source"], "NVIDIA-Linux-x86_64-535.309.01-vgpu-kvm.run")
        self.assertIn("acr/gp104_acr_bl", data["archives"])
        self.assertIn("pmu/gp104_pmu_rtos", data["archives"])
        self.assertIn("sec2/gp104_sec2_rtos", data["archives"])
        self.assertIn("gr/gp104_gr_netlist", data["singles"])
        self.assertEqual(len(data["singles"]["gr/gp104_gr_netlist"]["netlist_sections"]), 32)

    def test_extract_vgpu_535_blobs_cli_help(self):
        script = REPO_ROOT / "tools" / "extract_vgpu_535_blobs.py"
        r = subprocess.run([sys.executable, str(script), "--help"], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0)
        self.assertIn("BLOB_MANIFEST.json", r.stdout)

    def test_patch_535_guest_linux7_cli_help(self):
        script = REPO_ROOT / "scripts" / "patch_535_guest_linux7.py"
        r = subprocess.run([sys.executable, str(script), "--help"], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0)
        self.assertIn("kernel_dir", r.stdout)


if __name__ == "__main__":
    unittest.main()
