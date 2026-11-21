import json
import os
import pathlib
import shutil
import subprocess
import unittest
import xml.etree.ElementTree as ET

ROOT = pathlib.Path(__file__).resolve().parents[1]


class ReproScriptsAndOpenDaemonsTests(unittest.TestCase):
    def test_rea610_evidence_jsons_exist_and_parse(self):
        ev_dir = ROOT / "evidence" / "vgpu-535-rea610"
        self.assertTrue(ev_dir.is_dir(), "Missing evidence/vgpu-535-rea610")
        expected = [
            "rea_doctor.json",
            "nvidia-vgpud.analyze.json",
            "nvidia-vgpud.decompile.json",
            "nvidia-vgpud.exports.json",
            "nvidia-vgpud.exposure.json",
            "nvidia-vgpud.layout.json",
            "libnvidia-vgxcfg.analyze.json",
            "libnvidia-vgxcfg.decompile.json",
            "libnvidia-vgxcfg.exports.json",
            "libnvidia-vgxcfg.exposure.json",
            "libnvidia-vgxcfg.layout.json",
            "nvidia-xid-logd.analyze.json",
            "nvidia-xid-logd.decompile.json",
            "nvidia-xid-logd.exports.json",
            "nvidia-xid-logd.exposure.json",
            "nvidia-xid-logd.layout.json",
        ]
        for fn in expected:
            p = ev_dir / fn
            self.assertTrue(p.is_file(), f"Missing REA 6.10.1 evidence: {p}")
            data = json.loads(p.read_text(encoding="utf-8"))
            self.assertIsInstance(data, dict)

    def test_open_daemon_sources_exist_without_binaries(self):
        required_files = [
            ROOT / "open-vgxcfg" / "open_vgxcfg.h",
            ROOT / "open-vgxcfg" / "open_vgxcfg.c",
            ROOT / "open-vgxcfg" / "test_vgxcfg_unit.c",
            ROOT / "open-vgxcfg" / "test_vgxcfg_diff.c",
            ROOT / "open-vgxcfg" / "Makefile",
            ROOT / "open-xid-logd" / "open_xid_logd.c",
            ROOT / "open-xid-logd" / "mock_nvml.c",
            ROOT / "open-xid-logd" / "test_xid_logd.sh",
            ROOT / "open-xid-logd" / "Makefile",
            ROOT / "nova-vgpu" / "src" / "rm_ioctl.rs",
            ROOT / "nova-vgpu" / "src" / "bin" / "open_nvidia_vgpud.rs",
        ]
        for p in required_files:
            self.assertTrue(p.is_file(), f"Missing open daemon source: {p}")

    def test_win11_autounattend_template_valid_and_no_secrets(self):
        tmpl = ROOT / "scripts" / "templates" / "win11-autounattend.xml"
        self.assertTrue(tmpl.is_file())
        txt = tmpl.read_text(encoding="utf-8")
        self.assertIn("__ADMIN_PASSWORD__", txt)
        self.assertNotIn("sanjay", txt.lower())
        tree = ET.fromstring(txt)
        self.assertTrue(tree.tag.endswith("unattend"))

    def test_provision_win11_ps1_syntax_markers_and_no_secrets(self):
        ps1 = ROOT / "scripts" / "provision_win11_guest.ps1"
        self.assertTrue(ps1.is_file())
        txt = ps1.read_text(encoding="utf-8")
        for needle in (
            "$DlsUrl",
            "/-/client-token",
            "FeatureType",
            "AVCHardwareEncodePreferred",
            "AVC444ModePreferred",
            "UserAuthentication",
        ):
            self.assertIn(needle, txt)
        self.assertNotIn("sanjay", txt.lower())

    def test_repro_shell_scripts_syntax_and_dry_run_on_posix(self):
        scripts = [
            ROOT / "scripts" / "install_vgpu_host_dkms.sh",
            ROOT / "scripts" / "install_fastapi_dls.sh",
            ROOT / "scripts" / "setup_proxmox_vgpu_vms.sh",
            ROOT / "scripts" / "provision_alma_guest.sh",
            ROOT / "open-xid-logd" / "test_xid_logd.sh",
        ]
        for s in scripts:
            self.assertTrue(s.is_file(), f"Missing script {s}")
            txt = s.read_text(encoding="utf-8")
            self.assertNotIn("sanjay", txt.lower(), f"Hardcoded password leaked in {s}")

        if os.name == "nt" or not shutil.which("bash"):
            self.skipTest("Linux bash syntax/dry-run checks run on Linux CI/.15 host")

        for s in scripts:
            r = subprocess.run(["bash", "-n", str(s)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, f"bash -n failed for {s}: {r.stderr}")

        dry_cases = [
            ["bash", str(ROOT / "scripts" / "install_vgpu_host_dkms.sh"), "--dry-run"],
            ["bash", str(ROOT / "scripts" / "install_fastapi_dls.sh"), "--mode", "docker", "--dry-run"],
            ["bash", str(ROOT / "scripts" / "install_fastapi_dls.sh"), "--mode", "systemd", "--dry-run"],
            ["bash", str(ROOT / "scripts" / "setup_proxmox_vgpu_vms.sh"), "--mode", "fresh", "--dry-run"],
            [
                "bash",
                str(ROOT / "scripts" / "setup_proxmox_vgpu_vms.sh"),
                "--mode",
                "import-qcow2",
                "--alma-disk",
                "/tmp/a.qcow2",
                "--win11-disk",
                "/tmp/w.qcow2",
                "--dry-run",
            ],
            ["bash", str(ROOT / "scripts" / "provision_alma_guest.sh"), "--dry-run", "--with-selkies"],
        ]
        for cmd in dry_cases:
            r = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, f"Dry-run failed ({cmd}): {r.stderr}\n{r.stdout}")


if __name__ == "__main__":
    unittest.main()
