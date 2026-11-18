#!/usr/bin/env python3
"""Unit tests for GRD PTX patcher, RDP connection profiles, shell scripts, and disclaimer hygiene."""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent


class TestGrdPtxRdpAndScripts(unittest.TestCase):
    def test_shell_scripts_pass_bash_syntax(self):
        bash = shutil.which("bash")
        if not bash:
            self.skipTest("bash not installed on this platform")
        scripts = [
            REPO_ROOT / "scripts" / "extract_proprietary_blobs.sh",
            REPO_ROOT / "scripts" / "patch_grd_ptx_535.sh",
            REPO_ROOT / "scripts" / "start_vgpu_vms.sh",
            REPO_ROOT / "tests" / "live_verify_linux_wayland_vgpu.sh",
        ]
        for s in scripts:
            self.assertTrue(s.exists(), f"Missing script: {s}")
            r = subprocess.run([bash, "-n", str(s)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, f"bash -n failed for {s}: {r.stderr}")

    def test_patch_grd_ptx_535_downgrades_and_is_idempotent(self):
        bash = shutil.which("bash")
        if not bash:
            self.skipTest("bash not installed on this platform")
        script = REPO_ROOT / "scripts" / "patch_grd_ptx_535.sh"
        with tempfile.TemporaryDirectory() as td:
            ptx = Path(td) / "grd-cuda-avc-utils_30.ptx"
            ptx.write_text(".version 8.7\n.target sm_30\n.address_size 64\n", encoding="utf-8")

            r1 = subprocess.run([bash, str(script), str(ptx)], capture_output=True, text=True)
            self.assertEqual(r1.returncode, 0, r1.stderr)
            self.assertIn(".version 8.0", ptx.read_text(encoding="utf-8"))
            self.assertTrue((Path(td) / "grd-cuda-avc-utils_30.ptx.orig").exists())

            # Second run must be a no-op
            r2 = subprocess.run([bash, str(script), str(ptx)], capture_output=True, text=True)
            self.assertEqual(r2.returncode, 0, r2.stderr)
            self.assertIn("No patch needed", r2.stdout)

            # Older PTX <= 8.2 must stay untouched
            ptx2 = Path(td) / "old.ptx"
            ptx2.write_text(".version 8.2\n.target sm_61\n", encoding="utf-8")
            r3 = subprocess.run([bash, str(script), str(ptx2)], capture_output=True, text=True)
            self.assertEqual(r3.returncode, 0, r3.stderr)
            self.assertIn(".version 8.2", ptx2.read_text(encoding="utf-8"))

    def test_rdp_profiles_valid_and_no_embedded_passwords(self):
        rdp_dir = REPO_ROOT / "rdp"
        files = sorted(rdp_dir.glob("*.rdp"))
        self.assertEqual(len(files), 5, f"Expected 5 .rdp profiles, found {[f.name for f in files]}")
        for f in files:
            text = f.read_text(encoding="utf-8")
            self.assertNotIn("password 51:", text.lower(), f"Password hash found in {f.name}")
            self.assertIn("connection type:i:6", text, f"Missing LAN profile in {f.name}")
            self.assertIn("session bpp:i:32", text, f"Missing 32-bit color in {f.name}")
            self.assertIn("dynamic resolution:i:1", text, f"Missing dynamic resolution in {f.name}")
            self.assertIn(":3389", text, f"Missing RDP port in {f.name}")

    def test_disclaimer_covers_risks_and_llm_usage(self):
        disclaimer = (REPO_ROOT / "DISCLAIMER.md").read_text(encoding="utf-8").lower()
        for keyword in ["as is", "risk", "llm", "large language model", "gpl-2.0", "no warranty"]:
            self.assertIn(keyword, disclaimer, f"DISCLAIMER.md missing required section/term: {keyword}")


if __name__ == "__main__":
    unittest.main()
