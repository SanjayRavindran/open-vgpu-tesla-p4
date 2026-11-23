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
            "nvidia-vgpu-mgr.analyze.json",
            "nvidia-vgpu-mgr.decompile.json",
            "nvidia-vgpu-mgr.exports.json",
            "nvidia-vgpu-mgr.exposure.json",
            "nvidia-vgpu-mgr.layout.json",
            "nvidia-smi.exports.json",
            "nvidia-smi.exposure.json",
            "nvidia-smi.layout.json",
            "libnvidia-ml.so.535.309.01.exports.json",
            "libnvidia-ml.so.535.309.01.exposure.json",
            "libnvidia-ml.so.535.309.01.layout.json",
            "libnvidia-vgpu.so.535.309.01.exports.json",
            "libnvidia-vgpu.so.535.309.01.exposure.json",
            "libnvidia-vgpu.so.535.309.01.layout.json",
            "libnvidia-encode.analyze.json",
            "libnvidia-encode.decompile_summary.json",
            "libnvidia-encode.exports.json",
            "libnvidia-encode.exposure.json",
            "libnvidia-encode.layout.json",
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
            ROOT / "open-vgpu-mgr" / "nv_vgpu_vfio_abi.h",
            ROOT / "open-vgpu-mgr" / "rm_client.h",
            ROOT / "open-vgpu-mgr" / "rm_client.c",
            ROOT / "open-vgpu-mgr" / "vmiop_env.h",
            ROOT / "open-vgpu-mgr" / "vmiop_env.c",
            ROOT / "open-vgpu-mgr" / "open_vgpu_mgr.c",
            ROOT / "open-vgpu-mgr" / "test_vgpu_mgr_unit.c",
            ROOT / "open-vgpu-mgr" / "Makefile",
            ROOT / "open-nvml-smi" / "open_nvml.h",
            ROOT / "open-nvml-smi" / "open_nvml.c",
            ROOT / "open-nvml-smi" / "open_nvidia_smi.c",
            ROOT / "open-nvml-smi" / "test_nvml_smi_unit.c",
            ROOT / "open-nvml-smi" / "Makefile",
            ROOT / "open-vgpu-plugin" / "open_vgpu_plugin.h",
            ROOT / "open-vgpu-plugin" / "open_vgpu_rm.h",
            ROOT / "open-vgpu-plugin" / "gp104_vgpu_core.c",
            ROOT / "open-vgpu-plugin" / "open_vgpu_rm.c",
            ROOT / "open-vgpu-plugin" / "open_vrpc_rm.c",
            ROOT / "open-vgpu-plugin" / "open_vrpc_mmu.c",
            ROOT / "open-vgpu-plugin" / "open_vgpu_static_info.c",
            ROOT / "open-vgpu-plugin" / "open_vgpu_plugin.c",
            ROOT / "open-vgpu-plugin" / "mock_vendor_vgpu.c",
            ROOT / "open-vgpu-plugin" / "test_vgpu_plugin_unit.c",
            ROOT / "open-vgpu-plugin" / "vmiop_plugin.map",
            ROOT / "open-vgpu-plugin" / "Makefile",
            ROOT / "open-vgpu-guest" / "open_vgpu_guest_rpc.h",
            ROOT / "open-vgpu-guest" / "open_vgpu_guest_rpc.c",
            ROOT / "open-vgpu-guest" / "test_host_guest_loopback.c",
            ROOT / "open-vgpu-guest" / "Makefile",
            ROOT / "open-vgpu-guest" / "overlay" / "open_vgpu_bar0_rpc.h",
            ROOT / "open-vgpu-guest" / "overlay" / "open_vgpu_bar0_rpc.c",
            ROOT / "open-vgpu-guest" / "scripts" / "prepare_open_535_guest_rm.sh",
            ROOT / "open-vgpu-guest" / "scripts" / "patch_open_535_guest_rm.py",
            ROOT / "open-vgpu-guest" / "scripts" / "build_open_535_guest_kmod.sh",
            ROOT / "open-nvenc" / "open_nvenc.h",
            ROOT / "open-nvenc" / "open_nvenc.c",
            ROOT / "open-nvenc" / "test_nvenc_unit.c",
            ROOT / "open-nvenc" / "Makefile",
            ROOT / "open-host-rm" / "open_host_rm_gp104.h",
            ROOT / "open-host-rm" / "open_host_rm_gp104.c",
            ROOT / "open-host-rm" / "test_open_host_rm_unit.c",
            ROOT / "open-host-rm" / "Makefile",
            ROOT / "docs" / "12-OPEN-VGPU-PLUGIN-AND-VRPC-TRACE-CATALOG.md",
            ROOT / "docs" / "13-OPEN-GUEST-VGPU-KERNEL-DRIVER.md",
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
            ROOT / "scripts" / "install_open_vgpu.sh",
            ROOT / "scripts" / "install_open_580.sh",
            ROOT / "scripts" / "install_vgpu_host_dkms.sh",
            ROOT / "scripts" / "install_fastapi_dls.sh",
            ROOT / "scripts" / "setup_proxmox_vgpu_vms.sh",
            ROOT / "scripts" / "provision_alma_guest.sh",
            ROOT / "open-xid-logd" / "test_xid_logd.sh",
            ROOT / "open-vgpu-guest" / "scripts" / "prepare_open_535_guest_rm.sh",
            ROOT / "open-vgpu-guest" / "scripts" / "build_open_535_guest_kmod.sh",
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
            ["bash", str(ROOT / "scripts" / "install_open_vgpu.sh"), "--role", "host", "--dry-run"],
            ["bash", str(ROOT / "scripts" / "install_open_vgpu.sh"), "--role", "guest-linux", "--dry-run"],
            ["bash", str(ROOT / "scripts" / "install_open_vgpu.sh"), "--role", "guest-windows", "--dry-run"],
            ["bash", str(ROOT / "scripts" / "install_open_580.sh"), "--role", "host", "--dry-run"],
            ["bash", str(ROOT / "scripts" / "install_open_580.sh"), "--role", "guest-linux", "--dry-run"],
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
            ["bash", str(ROOT / "open-vgpu-guest" / "scripts" / "prepare_open_535_guest_rm.sh"), "--dry-run"],
            ["bash", str(ROOT / "open-vgpu-guest" / "scripts" / "build_open_535_guest_kmod.sh"), "--dry-run"],
        ]
        for cmd in dry_cases:
            r = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, f"Dry-run failed ({cmd}): {r.stderr}\n{r.stdout}")

    def test_patch_open_535_guest_rm_idempotency(self) -> None:
        import importlib.util
        import pathlib
        import tempfile

        patcher_path = ROOT / "open-vgpu-guest" / "scripts" / "patch_open_535_guest_rm.py"
        spec = importlib.util.spec_from_file_location("patch_open_535_guest_rm", patcher_path)
        self.assertIsNotNone(spec)
        self.assertIsNotNone(spec.loader)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)

        fixtures = {
            "src/nvidia/srcs.mk": "SRCS += kernel/vgpu/nv/rpc.c\n",
            "src/nvidia/generated/g_hal_archimpl.h": (
                "    {                               0x0,                            0x0,   0x0 } , // GP104  (disabled)\n"
            ),
            "src/nvidia/generated/g_hal_register.h": (
                "NV_STATUS registerHalModule_TU102(void);\n"
                "    rmStatus = REGISTER_TU10X_HALS();\n"
            ),
            "src/nvidia/generated/g_hal_private.h": (
                "#if defined(RMCFG_HAL_SETUP_TU102)\n"
                "static const HAL_IFACE_SETUP halIface_TU102 = {};\n"
                "#endif\n"
            ),
            "src/nvidia/generated/g_chips2halspec_nvoc.c": "    // TU102\n    if(arch == 0x16 && impl == 0x2)\n",
            "src/nvidia/arch/nvalloc/unix/src/osapi.c": (
                "    rmStatus = halmgrGetHalForGpu(pHalMgr, pmc_boot_0, pmc_boot_42, &myHalPublicID);\n"
            ),
            "src/nvidia/generated/g_gpu_nvoc.h": (
                "#define IS_VIRTUAL(pGpu)                         NV_FALSE\n"
                "#define IS_VIRTUAL_WITHOUT_SRIOV(pGpu)           NV_FALSE\n"
            ),
            "src/nvidia/src/kernel/gpu/gpu.c": (
                '#include "gpu/bus/kern_bus.h"\n'
                "    // create remaining gpu offspring\n"
                "    rmStatus = gpuCreateChildObjects(pGpu, /* bConstructEarly */ NV_FALSE);\n"
                "    if (rmStatus != NV_OK)\n"
                "        return rmStatus;\n"
                "    // Free children in reverse order from construction\n"
                "    // Early detection at GPU creation time should be consistent to the real detection\n"
                "    NV_ASSERT_OR_RETURN(pGpu->isVirtual == bIsVirtual, NV_ERR_INVALID_STATE);\n"
                "VGPU_STATIC_INFO *gpuGetStaticInfo(OBJGPU *pGpu)\n{\n\n    return NULL;\n}\n"
                "GspStaticConfigInfo *gpuGetGspStaticInfo(OBJGPU *pGpu)\n{\n    NV_ASSERT_OR_RETURN(GPU_GET_KERNEL_GSP(pGpu) != NULL, NULL);\n    return &(GPU_GET_KERNEL_GSP(pGpu)->gspStaticInfo);\n}\n"
                "OBJRPC *gpuGetVgpuRpc(OBJGPU *pGpu)\n{\n    return NULL;\n}\n"
            ),
            "src/nvidia/arch/nvalloc/unix/src/osinit.c": (
                "    nv_firmware_chip_family_t chipFamily;\n"
                "    nv_priv_t *nvp = NV_GET_NV_PRIV(nv);\n"
                "    else if (nv->request_fw_client_rm)\n"
            ),
            "src/nvidia/kernel/vgpu/nv/objvgpu.c": (
                '#include "nvRmReg.h"\n'
                "NV_STATUS\nvgpuCreateObject\n(\n    OBJGPU *pGpu\n)\n{\n    NV_STATUS  rmStatus = NV_OK;\n    return rmStatus;\n}\n"
                "void\nvgpuDestructObject\n(\n    OBJGPU *pGpu\n)\n{\n}\n"
            ),
            "src/nvidia/src/kernel/rmapi/rpc_common.c": (
                '#include "vgpu/vgpu_version.h"\n'
                "    if (IS_VIRTUAL(pGpu))\n    {\n        // none for now\n    }\n    else if (IS_GSP_CLIENT(pGpu))\n"
            ),
            "src/nvidia/kernel/inc/vgpu/rpc.h": (
                "            NV_ASSERT(IS_GSP_CLIENT(pGpu));\n"
            ),
            "src/nvidia/kernel/vgpu/nv/rpc.c": (
                "    if (IS_GSP_CLIENT(pGpu))\n    {\n        status = _issuePteDescRpc(\n"
            ),
            "src/nvidia/src/kernel/gpu/fifo/kernel_fifo.c": (
                "    // Do internal control call and set numChannels\n    if (IS_GSP_CLIENT(pGpu))\n"
            ),
            "src/nvidia/src/kernel/gpu/intr/arch/turing/intr_tu102.c": (
                "    // GSP RM services both MMU non-replayable fault and FIFO interrupts\n    if (IS_GSP_CLIENT(pGpu))\n"
            ),
            "src/nvidia/kernel/inc/vgpu/rpc_vgpu.h": (
                "#ifndef __vgpu_dev_nv_rpc_vgpu_h__\n"
                "#define __vgpu_dev_nv_rpc_vgpu_h__\n"
                "static NV_INLINE void NV_RM_RPC_ALLOC_LOCAL_USER(OBJGPU *pGpu, ...) { }\n"
                "static NV_INLINE void NV_RM_RPC_ALLOC_VIDMEM(OBJGPU *pGpu, ...) { }\n"
                "static NV_INLINE void NV_RM_RPC_ALLOC_VIRTMEM(OBJGPU *pGpu, ...) { }\n"
                "static NV_INLINE void NV_RM_RPC_MAP_MEMORY(OBJGPU *pGpu, ...) { }\n"
                "static NV_INLINE void NV_RM_RPC_UNMAP_MEMORY(OBJGPU *pGpu, ...) { }\n"
                "static NV_INLINE void NV_RM_RPC_SET_GUEST_SYSTEM_INFO_EXT(OBJGPU *pGpu, ...) { }\n"
                "static NV_INLINE void NV_RM_RPC_GET_CONSOLIDATED_STATIC_INFO(OBJGPU *pGpu, ...) { }\n"
                "static NV_INLINE void NV_RM_RPC_GET_CONSOLIDATED_GR_STATIC_INFO(OBJGPU *pGpu, ...) { }\n"
                "static NV_INLINE void NV_RM_RPC_UPDATE_PDE_2(OBJGPU *pGpu, ...) { }\n"
                "static NV_INLINE void NV_RM_RPC_ALLOC_CONTEXT_DMA(OBJGPU *pGpu, ...) { }\n"
                "#endif\n"
            ),
        }

        with tempfile.TemporaryDirectory() as tmp:
            src_root = pathlib.Path(tmp)
            for rel, content in fixtures.items():
                p = src_root / rel
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text(content, encoding="utf-8")

            overlay_dir = ROOT / "open-vgpu-guest" / "overlay"
            mod.patch_rm_sources(src_root, overlay_dir)
            first_pass = {rel: (src_root / rel).read_text(encoding="utf-8") for rel in fixtures}

            mod.patch_rm_sources(src_root, overlay_dir)
            second_pass = {rel: (src_root / rel).read_text(encoding="utf-8") for rel in fixtures}
            self.assertEqual(first_pass, second_pass)

            self.assertIn("GP104  (open-pascal-vgpu)", first_pass["src/nvidia/generated/g_hal_archimpl.h"])
            self.assertIn("registerHalModule_GP104", first_pass["src/nvidia/generated/g_hal_private.h"])
            self.assertEqual(first_pass["src/nvidia/generated/g_hal_private.h"].count("#if defined(RMCFG_HAL_SETUP_TU102)"), 1)
            self.assertIn("OPEN_VGPU_GP104_GATE", first_pass["src/nvidia/arch/nvalloc/unix/src/osapi.c"])
            self.assertIn("OPEN_VGPU_GP104_NO_GSP_FW", first_pass["src/nvidia/arch/nvalloc/unix/src/osinit.c"])
            self.assertIn("!IS_VIRTUAL(pGpu)", first_pass["src/nvidia/arch/nvalloc/unix/src/osinit.c"])
            self.assertIn("openVgpuDetectBar0VirtualMode", first_pass["src/nvidia/src/kernel/gpu/gpu.c"])
            self.assertIn("openVgpuGetGspStaticInfo", first_pass["src/nvidia/src/kernel/gpu/gpu.c"])
            self.assertLess(
                first_pass["src/nvidia/src/kernel/gpu/gpu.c"].index("vgpuCreateObject(pGpu)"),
                first_pass["src/nvidia/src/kernel/gpu/gpu.c"].index("gpuCreateChildObjects(pGpu"),
            )
            self.assertIn("openVgpuRmApiAlloc", first_pass["src/nvidia/src/kernel/rmapi/rpc_common.c"])
            self.assertIn("openVgpuRmApiFree", first_pass["src/nvidia/src/kernel/rmapi/rpc_common.c"])
            self.assertIn("openVgpuRmApiDupObject", first_pass["src/nvidia/src/kernel/rmapi/rpc_common.c"])
            self.assertIn("IS_VIRTUAL(pGpu)", first_pass["src/nvidia/kernel/inc/vgpu/rpc.h"])
            self.assertIn("IS_VIRTUAL(pGpu)", first_pass["src/nvidia/kernel/vgpu/nv/rpc.c"])
            self.assertIn("IS_VIRTUAL(pGpu)", first_pass["src/nvidia/src/kernel/gpu/fifo/kernel_fifo.c"])
            self.assertIn(
                "IS_VIRTUAL_WITHOUT_SRIOV(pGpu)",
                first_pass["src/nvidia/src/kernel/gpu/intr/arch/turing/intr_tu102.c"],
            )
            self.assertIn("openVgpuRpcAllocVidmem", first_pass["src/nvidia/kernel/inc/vgpu/rpc_vgpu.h"])
            self.assertIn("open_vgpu_bar0_rpc.c", first_pass["src/nvidia/srcs.mk"])
            self.assertTrue((src_root / "src/nvidia/kernel/inc/vgpu/open_vgpu_bar0_rpc.h").is_file())
            self.assertTrue((src_root / "src/nvidia/kernel/vgpu/nv/open_vgpu_bar0_rpc.c").is_file())


if __name__ == "__main__":
    unittest.main()
