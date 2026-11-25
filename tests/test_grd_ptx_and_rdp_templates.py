#!/usr/bin/env python3
"""Unit tests for R580 helper tools, 2-VM RDP profiles, Selkies 10-bit 4:4:4 Compose config, shell scripts, and disclaimer."""

import importlib.util
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent


class TestR580HelpersRdpAndScripts(unittest.TestCase):
    def test_shell_scripts_pass_bash_syntax(self):
        bash = shutil.which("bash")
        if not bash:
            self.skipTest("bash not installed on this platform")
        scripts = [
            REPO_ROOT / "scripts" / "extract_proprietary_blobs.sh",
            REPO_ROOT / "scripts" / "setup_selkies_pixelflux_vgpu.sh",
            REPO_ROOT / "scripts" / "start_vgpu_vms.sh",
            REPO_ROOT / "tests" / "live_verify_linux_wayland_vgpu.sh",
        ]
        for s in scripts:
            self.assertTrue(s.exists(), f"Missing script: {s}")
            r = subprocess.run([bash, "-n", str(s)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, f"bash -n failed for {s}: {r.stderr}")

    def test_patch_r580_guest_nv_pci_patches_and_is_idempotent(self):
        tool = REPO_ROOT / "tools" / "patch_r580_guest_nv_pci.py"
        spec = importlib.util.spec_from_file_location("patch_r580_guest_nv_pci", tool)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)

        fixture = (
            "    if (!nv_lock_init_locks(sp, nv))\n"
            "        goto err_not_supported;\n\n"
            "    if ((rm_is_supported_device(sp, nv)) != NV_OK)\n"
            "        goto err_not_supported;\n\n"
            "    if (!rm_init_private_state(sp, nv))\n"
        )
        with tempfile.TemporaryDirectory() as td:
            kdir = Path(td) / "kernel"
            (kdir / "nvidia").mkdir(parents=True)
            nv_pci = kdir / "nvidia" / "nv-pci.c"
            nv_pci.write_text(fixture, encoding="utf-8")

            changed1, _ = mod.patch_nv_pci(kdir)
            self.assertTrue(changed1)
            txt = nv_pci.read_text(encoding="utf-8")
            self.assertIn("pci_dev->vendor == 0x10de && pci_dev->device == 0x1bb3", txt)
            self.assertTrue((kdir / "nvidia" / "nv-pci.c.orig").is_file())

            changed2, msg2 = mod.patch_nv_pci(kdir)
            self.assertFalse(changed2)
            self.assertIn("No patch needed", msg2)

    def test_patch_r580_vgpu_config_xml_injects_and_is_idempotent(self):
        tool = REPO_ROOT / "tools" / "patch_r580_vgpu_config_xml.py"
        spec = importlib.util.spec_from_file_location("patch_r580_vgpu_config_xml", tool)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)

        minimal_xml = '<?xml version="1.0" encoding="utf-8"?>\n<vgpuconfig>\n</vgpuconfig>\n'
        with tempfile.TemporaryDirectory() as td:
            xml_path = Path(td) / "vgpuConfig.xml"
            xml_path.write_text(minimal_xml, encoding="utf-8")

            changed1, _ = mod.patch_vgpu_config(xml_path)
            self.assertTrue(changed1)
            patched = xml_path.read_text(encoding="utf-8")
            self.assertIn('name="GRID P4-4Q"', patched)
            self.assertIn('subsystemId="0x11D8"', patched)
            self.assertIn('deviceId="0x1BB3"', patched)

            changed2, _ = mod.patch_vgpu_config(xml_path)
            self.assertFalse(changed2)

    def test_rdp_profiles_valid_two_vm_focus_and_no_embedded_passwords(self):
        rdp_dir = REPO_ROOT / "rdp"
        files = sorted(rdp_dir.glob("*.rdp"))
        self.assertEqual(
            [f.name for f in files],
            ["vm-alma.rdp", "vm-win11.rdp"],
            f"Expected only vm-alma.rdp and vm-win11.rdp, found {[f.name for f in files]}",
        )
        for f in files:
            text = f.read_text(encoding="utf-8")
            self.assertNotIn("password 51:", text.lower(), f"Password hash found in {f.name}")
            self.assertIn("connection type:i:6", text, f"Missing LAN profile in {f.name}")
            self.assertIn("session bpp:i:32", text, f"Missing 32-bit color in {f.name}")
            self.assertIn("smart sizing:i:1", text, f"Missing smart sizing in {f.name}")
            self.assertIn(":3389", text, f"Missing RDP port in {f.name}")

    def test_selkies_pixelflux_10bit_444_compose_config(self):
        compose = (REPO_ROOT / "docker-compose.yml").read_text(encoding="utf-8")
        self.assertIn("lscr.io/linuxserver/webtop", compose)
        self.assertIn("SELKIES_ENCODER", compose)
        self.assertIn("h265enc,h264enc", compose)
        self.assertIn("SELKIES_VIDEO_FULLCOLOR", compose)
        self.assertIn("SELKIES_VIDEO_10BIT", compose)
        self.assertIn("GBM_BACKENDS_PATH", compose)

    def test_disclaimer_covers_risks_and_llm_usage(self):
        disclaimer = (REPO_ROOT / "DISCLAIMER.md").read_text(encoding="utf-8").lower()
        for keyword in ["as is", "risk", "llm", "large language model", "gpl-2.0", "no warranty"]:
            self.assertIn(keyword, disclaimer, f"DISCLAIMER.md missing required section/term: {keyword}")


if __name__ == "__main__":
    unittest.main()
