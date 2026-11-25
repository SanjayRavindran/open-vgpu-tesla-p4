#!/usr/bin/env python3
"""Unit tests verifying clean-room repo hygiene, BLOB_MANIFEST.json, and extractor tools."""

import json
import shutil
import subprocess
import sys
import tempfile
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

    def test_patch_r580_tools_cli_help(self):
        for rel, needle in (
            ("patch_r580_guest_nv_pci.py", "kernel_dir"),
            ("patch_r580_vgpu_config_xml.py", "vgpuConfig.xml"),
        ):
            script = REPO_ROOT / "tools" / rel
            r = subprocess.run([sys.executable, str(script), "--help"], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0)
            self.assertIn(needle, r.stdout)

    def test_patch_r580_vfio_bar1_fault_idempotent_and_strict(self):
        sys.path.insert(0, str(REPO_ROOT / "tools"))
        try:
            import patch_r580_vfio_bar1_fault as vf_patch
        finally:
            sys.path.pop(0)

        sample = (
            "static NV_STATUS nv_vgpu_vfio_validate_map_request(void) {\n"
            + vf_patch.OLD_VALIDATE_HEAD
            + "    if (node)\n"
            + vf_patch.OLD_VALIDATE_DUP
            + "\n"
            + vf_patch.OLD_VALIDATE_TAIL
            + "\n}\n\n"
            + vf_patch.OLD_FAULT_FN
            + "\n\nstatic int nv_vgpu_vfio_mmap(void) {\n"
            + vf_patch.OLD_MMAP_BAR1
            + "\n}\n"
        )

        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / "nvidia-vgpu-vfio.c"
            p.write_text(sample, encoding="utf-8")

            r1 = subprocess.run([sys.executable, str(REPO_ROOT / "tools" / "patch_r580_vfio_bar1_fault.py"), str(p)],
                                capture_output=True, text=True)
            self.assertEqual(r1.returncode, 0, r1.stderr)
            patched = p.read_text(encoding="utf-8")
            self.assertIn(vf_patch.MARKER, patched)
            self.assertIn("vmf_insert_pfn_prot", patched)
            self.assertIn("vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP)", patched)
            self.assertNotIn("BAR1 guest address already mapped", patched)

            r2 = subprocess.run([sys.executable, str(REPO_ROOT / "tools" / "patch_r580_vfio_bar1_fault.py"), str(p)],
                                capture_output=True, text=True)
            self.assertEqual(r2.returncode, 0, r2.stderr)
            self.assertEqual(p.read_text(encoding="utf-8"), patched)

            bad = Path(tmp) / "bad.c"
            bad.write_text("int main(void) { return 0; }\n", encoding="utf-8")
            r3 = subprocess.run([sys.executable, str(REPO_ROOT / "tools" / "patch_r580_vfio_bar1_fault.py"), str(bad)],
                                capture_output=True, text=True)
            self.assertNotEqual(r3.returncode, 0)


if __name__ == "__main__":
    unittest.main()
