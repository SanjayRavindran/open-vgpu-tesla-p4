#!/usr/bin/env python3
"""
Clean-room Phase 2B Patcher for NVIDIA/open-gpu-kernel-modules @ 535.309.01

Enables virtualized Pascal GP104 (Tesla P4 KVM vGPU guest) support via the
BAR0 0x8f000 "VRPC" overlay while refusing bare-metal GP104 silicon without an
active vGPU hypervisor, and optionally applies Linux 6.13–7.0 kernel-open
compatibility fixes.

License: MIT
"""

from __future__ import annotations

import argparse
import shutil
from pathlib import Path

BAR0_DETECT_MARKER = "/* OPEN_VGPU_GP104_GUEST_PATCH */"


def _patch_once(path: Path, old: str, new: str, marker: str = BAR0_DETECT_MARKER) -> bool:
    text = path.read_text(encoding="utf-8")
    if marker in text and old not in text:
        return False
    if old not in text:
        if marker in text:
            return False
        raise RuntimeError(f"Expected anchor not found in {path}: {old!r}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")
    return True


def _replace_required(path: Path, old: str, new: str, guard_substring: str | None = None) -> None:
    text = path.read_text(encoding="utf-8")
    if guard_substring and guard_substring in text:
        return
    if old not in text:
        raise RuntimeError(f"Expected anchor not found in {path}: {old[:80]!r}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


def patch_rm_sources(src_root: Path, overlay_dir: Path) -> None:
    nv_dir = src_root / "src" / "nvidia"

    # 1. Install overlay C/H files into src/nvidia/kernel/inc/vgpu & vgpu/nv
    inc_vgpu = nv_dir / "kernel" / "inc" / "vgpu"
    src_vgpu = nv_dir / "kernel" / "vgpu" / "nv"
    inc_vgpu.mkdir(parents=True, exist_ok=True)
    src_vgpu.mkdir(parents=True, exist_ok=True)
    shutil.copy2(overlay_dir / "open_vgpu_bar0_rpc.h", inc_vgpu / "open_vgpu_bar0_rpc.h")
    shutil.copy2(overlay_dir / "open_vgpu_bar0_rpc.c", src_vgpu / "open_vgpu_bar0_rpc.c")

    srcs_mk = nv_dir / "srcs.mk"
    mk_txt = srcs_mk.read_text(encoding="utf-8")
    bar0_rule = "SRCS += kernel/vgpu/nv/open_vgpu_bar0_rpc.c"
    if bar0_rule not in mk_txt:
        _replace_required(
            srcs_mk,
            "SRCS += kernel/vgpu/nv/rpc.c",
            "SRCS += kernel/vgpu/nv/rpc.c\n" + bar0_rule,
        )

    # 2. Enable GP104 chip ID & HAL module registration
    archimpl = nv_dir / "generated" / "g_hal_archimpl.h"
    _replace_required(
        archimpl,
        "    {                               0x0,                            0x0,   0x0 } , // GP104  (disabled)",
        "    {                        0x00000013,                     0x00000004,   0x0 } , // GP104  (open-pascal-vgpu)",
        guard_substring="GP104  (open-pascal-vgpu)",
    )

    hal_reg = nv_dir / "generated" / "g_hal_register.h"
    _replace_required(
        hal_reg,
        "NV_STATUS registerHalModule_TU102(void);",
        "NV_STATUS registerHalModule_GP104(void);\nNV_STATUS registerHalModule_TU102(void);",
        guard_substring="registerHalModule_GP104",
    )
    _replace_required(
        hal_reg,
        "    rmStatus = REGISTER_TU10X_HALS();",
        "    rmStatus = registerHalModule_GP104();\n    if (rmStatus != NV_OK)\n    {\n        return rmStatus;\n    }\n\n    rmStatus = REGISTER_TU10X_HALS();",
        guard_substring="rmStatus = registerHalModule_GP104();",
    )

    hal_priv = nv_dir / "generated" / "g_hal_private.h"
    gp104_priv = (
        "#if defined(RMCFG_HAL_SETUP_TU102)\n\n"
        "static const HAL_IFACE_SETUP halIface_GP104 = {\n\n"
        "    rpcHalIfacesSetup_TU102,\n\n"
        "};\n\n"
        "NV_STATUS registerHalModule_GP104(void)\n"
        "{\n"
        "    return registerHalModule(HAL_IMPL_GP104, &halIface_GP104);\n"
        "}\n"
    )
    _replace_required(
        hal_priv,
        "#if defined(RMCFG_HAL_SETUP_TU102)",
        gp104_priv,
        guard_substring="registerHalModule_GP104(void)",
    )

    chips2hal = nv_dir / "generated" / "g_chips2halspec_nvoc.c"
    _replace_required(
        chips2hal,
        "    // TU102\n    if(arch == 0x16 && impl == 0x2)",
        "    // GP104 (open-pascal-vgpu)\n    if(arch == 0x13 && impl == 0x4)\n    {\n        pChipHal->__nvoc_HalVarIdx = 37;\n    }\n    // TU102\n    else if(arch == 0x16 && impl == 0x2)",
        guard_substring="GP104 (open-pascal-vgpu)",
    )

    # 3. Bare-metal GP104 safety gate in osapi.c rm_is_supported_device()
    osapi_c = nv_dir / "arch" / "nvalloc" / "unix" / "src" / "osapi.c"
    gp104_gate = """    rmStatus = halmgrGetHalForGpu(pHalMgr, pmc_boot_0, pmc_boot_42, &myHalPublicID);

    if ((rmStatus == NV_OK) && ((pmc_boot_0 & 0x1ff00000U) == 0x13400000U))
    {
        /* OPEN_VGPU_GP104_GATE: Refuse bare-metal GP104 silicon; allow vGPU guest only */
        GPUHWREG *vgpu_map = osMapKernelSpace(pNv->regs->cpu_address + 0x8f000ULL,
                                              os_page_size,
                                              NV_MEMORY_UNCACHED,
                                              NV_PROTECT_READABLE);
        NvU32 vgpu_cfg = 0U;
        if (vgpu_map != NULL)
        {
            vgpu_cfg = NV_PRIV_REG_RD32(vgpu_map, 0x20U);
            osUnmapKernelSpace(vgpu_map, os_page_size);
        }
        if ((vgpu_map == NULL) || (vgpu_cfg == 0xffffffffU) || ((vgpu_cfg & 1U) == 0U))
        {
            nv_printf(NV_DBG_ERRORS,
                "NVRM: Bare-metal GP104 without vGPU hypervisor is refused by open guest RM (cfg=0x%08x)\\n",
                vgpu_cfg);
            rmStatus = NV_ERR_NOT_SUPPORTED;
            goto threadfree;
        }
        nv_printf(NV_DBG_WARNINGS,
            "NVRM: Accepting virtualized Pascal GP104 vGPU guest via clean-room BAR0 VRPC (cfg=0x%08x)\\n",
            vgpu_cfg);
    }"""
    _replace_required(
        osapi_c,
        "    rmStatus = halmgrGetHalForGpu(pHalMgr, pmc_boot_0, pmc_boot_42, &myHalPublicID);",
        gp104_gate,
        guard_substring="OPEN_VGPU_GP104_GATE",
    )

    # 4. Wire IS_VIRTUAL, objvgpu, OBJRPC BAR0 transport, and rpc_vgpu.h
    gpu_nvoc_h = nv_dir / "generated" / "g_gpu_nvoc.h"
    _replace_required(
        gpu_nvoc_h,
        "#define IS_VIRTUAL(pGpu)                         NV_FALSE",
        "#define IS_VIRTUAL(pGpu)                         ((pGpu)->isVirtual)",
        guard_substring="((pGpu)->isVirtual)",
    )
    _replace_required(
        gpu_nvoc_h,
        "#define IS_VIRTUAL_WITHOUT_SRIOV(pGpu)           NV_FALSE",
        "#define IS_VIRTUAL_WITHOUT_SRIOV(pGpu)           ((pGpu)->isVirtual)",
        guard_substring="#define IS_VIRTUAL_WITHOUT_SRIOV(pGpu)           ((pGpu)->isVirtual)",
    )

    gpu_c = nv_dir / "src" / "kernel" / "gpu" / "gpu.c"
    _replace_required(
        gpu_c,
        '#include "gpu/bus/kern_bus.h"',
        '#include "gpu/bus/kern_bus.h"\n#include "vgpu/open_vgpu_bar0_rpc.h"',
        guard_substring="open_vgpu_bar0_rpc.h",
    )
    _replace_required(
        gpu_c,
        "    // create remaining gpu offspring\n    rmStatus = gpuCreateChildObjects(pGpu, /* bConstructEarly */ NV_FALSE);\n    if (rmStatus != NV_OK)\n        return rmStatus;",
        "    if (IS_VIRTUAL(pGpu))\n    {\n        NV_ASSERT_OK_OR_RETURN(vgpuCreateObject(pGpu));\n    }\n\n    // create remaining gpu offspring\n    rmStatus = gpuCreateChildObjects(pGpu, /* bConstructEarly */ NV_FALSE);\n    if (rmStatus != NV_OK)\n        return rmStatus;",
        guard_substring="NV_ASSERT_OK_OR_RETURN(vgpuCreateObject(pGpu));",
    )
    _replace_required(
        gpu_c,
        "    // Free children in reverse order from construction",
        "    if (IS_VIRTUAL(pGpu))\n    {\n        vgpuDestructObject(pGpu);\n    }\n\n    // Free children in reverse order from construction",
        guard_substring="vgpuDestructObject(pGpu);",
    )
    _replace_required(
        gpu_c,
        "    // Early detection at GPU creation time should be consistent to the real detection\n    NV_ASSERT_OR_RETURN(pGpu->isVirtual == bIsVirtual, NV_ERR_INVALID_STATE);",
        "    if (openVgpuDetectBar0VirtualMode(pGpu))\n    {\n        bIsVirtual        = NV_TRUE;\n        pGpu->isVirtual   = NV_TRUE;\n        pGpu->isGspClient = NV_FALSE;\n    }\n\n    // Early detection at GPU creation time should be consistent to the real detection\n    NV_ASSERT_OR_RETURN(pGpu->isVirtual == bIsVirtual, NV_ERR_INVALID_STATE);",
        guard_substring="openVgpuDetectBar0VirtualMode(pGpu)",
    )
    _replace_required(
        gpu_c,
        "VGPU_STATIC_INFO *gpuGetStaticInfo(OBJGPU *pGpu)\n{\n\n    return NULL;\n}",
        "VGPU_STATIC_INFO *gpuGetStaticInfo(OBJGPU *pGpu)\n{\n    return openVgpuGetStaticInfo(pGpu);\n}",
        guard_substring="openVgpuGetStaticInfo(pGpu)",
    )
    _replace_required(
        gpu_c,
        "GspStaticConfigInfo *gpuGetGspStaticInfo(OBJGPU *pGpu)\n{\n    NV_ASSERT_OR_RETURN(GPU_GET_KERNEL_GSP(pGpu) != NULL, NULL);\n    return &(GPU_GET_KERNEL_GSP(pGpu)->gspStaticInfo);\n}",
        "GspStaticConfigInfo *gpuGetGspStaticInfo(OBJGPU *pGpu)\n{\n    if (IS_VIRTUAL(pGpu))\n    {\n        return openVgpuGetGspStaticInfo(pGpu);\n    }\n    NV_ASSERT_OR_RETURN(GPU_GET_KERNEL_GSP(pGpu) != NULL, NULL);\n    return &(GPU_GET_KERNEL_GSP(pGpu)->gspStaticInfo);\n}",
        guard_substring="openVgpuGetGspStaticInfo(pGpu)",
    )
    _replace_required(
        gpu_c,
        "OBJRPC *gpuGetVgpuRpc(OBJGPU *pGpu)\n{\n    return NULL;\n}",
        "OBJRPC *gpuGetVgpuRpc(OBJGPU *pGpu)\n{\n    return openVgpuGetRpc(pGpu);\n}",
        guard_substring="openVgpuGetRpc(pGpu)",
    )

    osinit_c = nv_dir / "arch" / "nvalloc" / "unix" / "src" / "osinit.c"
    _replace_required(
        osinit_c,
        "    nv_firmware_chip_family_t chipFamily;\n    nv_priv_t *nvp = NV_GET_NV_PRIV(nv);",
        "    nv_firmware_chip_family_t chipFamily;\n"
        "    nv_priv_t *nvp = NV_GET_NV_PRIV(nv);\n\n"
        "    if ((nvp != NULL) && ((nvp->pmc_boot_0 & 0x1ff00000U) == 0x13400000U) && (nv->regs != NULL))\n"
        "    {\n"
        "        GPUHWREG *vgpu_map = osMapKernelSpace(nv->regs->cpu_address + 0x8f000ULL,\n"
        "                                              os_page_size,\n"
        "                                              NV_MEMORY_UNCACHED,\n"
        "                                              NV_PROTECT_READABLE);\n"
        "        NvU32 vgpu_cfg = 0U;\n"
        "        if (vgpu_map != NULL)\n"
        "        {\n"
        "            vgpu_cfg = NV_PRIV_REG_RD32(vgpu_map, 0x20U);\n"
        "            osUnmapKernelSpace(vgpu_map, os_page_size);\n"
        "        }\n"
        "        if ((vgpu_cfg != 0xffffffffU) && ((vgpu_cfg & 1U) != 0U))\n"
        "        {\n"
        "            /* OPEN_VGPU_GP104_NO_GSP_FW: GP104 vGPU guest uses BAR0 VRPC instead of GSP firmware */\n"
        "            portMemSet(pGspFw, 0, sizeof(*pGspFw));\n"
        "            nv->request_firmware = NV_FALSE;\n"
        "            nv->request_fw_client_rm = NV_FALSE;\n"
        "            return NV_OK;\n"
        "        }\n"
        "    }",
        guard_substring="OPEN_VGPU_GP104_NO_GSP_FW",
    )
    _replace_required(
        osinit_c,
        "    else if (nv->request_fw_client_rm)\n",
        "    else if (nv->request_fw_client_rm && !IS_VIRTUAL(pGpu))\n",
        guard_substring="else if (nv->request_fw_client_rm && !IS_VIRTUAL(pGpu))",
    )

    objvgpu_c = nv_dir / "kernel" / "vgpu" / "nv" / "objvgpu.c"
    _replace_required(
        objvgpu_c,
        '#include "nvRmReg.h"',
        '#include "nvRmReg.h"\n#include "vgpu/open_vgpu_bar0_rpc.h"',
        guard_substring="open_vgpu_bar0_rpc.h",
    )
    _replace_required(
        objvgpu_c,
        "NV_STATUS\nvgpuCreateObject\n(\n    OBJGPU *pGpu\n)\n{\n    NV_STATUS  rmStatus = NV_OK;\n    return rmStatus;\n}",
        "NV_STATUS\nvgpuCreateObject\n(\n    OBJGPU *pGpu\n)\n{\n    return openVgpuCreateObject(pGpu);\n}",
        guard_substring="openVgpuCreateObject(pGpu)",
    )
    _replace_required(
        objvgpu_c,
        "void\nvgpuDestructObject\n(\n    OBJGPU *pGpu\n)\n{\n}",
        "void\nvgpuDestructObject\n(\n    OBJGPU *pGpu\n)\n{\n    openVgpuDestructObject(pGpu);\n}",
        guard_substring="openVgpuDestructObject(pGpu)",
    )

    rpc_common_c = nv_dir / "src" / "kernel" / "rmapi" / "rpc_common.c"
    _replace_required(
        rpc_common_c,
        '#include "vgpu/vgpu_version.h"',
        '#include "vgpu/vgpu_version.h"\n#include "vgpu/open_vgpu_bar0_rpc.h"',
        guard_substring="open_vgpu_bar0_rpc.h",
    )
    _replace_required(
        rpc_common_c,
        "    if (IS_VIRTUAL(pGpu))\n    {\n        // none for now\n    }\n    else if (IS_GSP_CLIENT(pGpu))",
        "    if (IS_VIRTUAL(pGpu))\n"
        "    {\n"
        "        pRmApi->Control         = openVgpuRmApiControl;\n"
        "        pRmApi->AllocWithHandle = openVgpuRmApiAlloc;\n"
        "        pRmApi->Free            = openVgpuRmApiFree;\n"
        "        pRmApi->DupObject       = openVgpuRmApiDupObject;\n"
        "    }\n"
        "    else if (IS_GSP_CLIENT(pGpu))",
        guard_substring="openVgpuRmApiControl",
    )

    rpc_h = nv_dir / "kernel" / "inc" / "vgpu" / "rpc.h"
    rpc_h_txt = rpc_h.read_text(encoding="utf-8")
    if "NV_ASSERT(IS_GSP_CLIENT(pGpu) || IS_VIRTUAL(pGpu));" not in rpc_h_txt:
        if "NV_ASSERT(IS_GSP_CLIENT(pGpu));" not in rpc_h_txt:
            raise RuntimeError(f"Expected anchor not found in {rpc_h}: NV_ASSERT(IS_GSP_CLIENT(pGpu));")
        rpc_h_txt = rpc_h_txt.replace(
            "NV_ASSERT(IS_GSP_CLIENT(pGpu));",
            "NV_ASSERT(IS_GSP_CLIENT(pGpu) || IS_VIRTUAL(pGpu));",
        )
        rpc_h.write_text(rpc_h_txt, encoding="utf-8")

    rpc_c = nv_dir / "kernel" / "vgpu" / "nv" / "rpc.c"
    _replace_required(
        rpc_c,
        "    if (IS_GSP_CLIENT(pGpu))\n    {\n        status = _issuePteDescRpc(",
        "    if (IS_GSP_CLIENT(pGpu) || IS_VIRTUAL(pGpu))\n    {\n        status = _issuePteDescRpc(",
        guard_substring="if (IS_GSP_CLIENT(pGpu) || IS_VIRTUAL(pGpu))\n    {\n        status = _issuePteDescRpc(",
    )

    # 5. Pre-GSP Pascal GP104 KernelFifo & Intr virtual guest guards (Rank 3A)
    kernel_fifo_c = nv_dir / "src" / "kernel" / "gpu" / "fifo" / "kernel_fifo.c"
    if kernel_fifo_c.is_file():
        _replace_required(
            kernel_fifo_c,
            "    // Do internal control call and set numChannels\n    if (IS_GSP_CLIENT(pGpu))",
            "    // Do internal control call and set numChannels\n    if (IS_GSP_CLIENT(pGpu) || IS_VIRTUAL(pGpu))",
            guard_substring="// Do internal control call and set numChannels\n    if (IS_GSP_CLIENT(pGpu) || IS_VIRTUAL(pGpu))",
        )

    intr_tu102_c = nv_dir / "src" / "kernel" / "gpu" / "intr" / "arch" / "turing" / "intr_tu102.c"
    if intr_tu102_c.is_file():
        _replace_required(
            intr_tu102_c,
            "    // GSP RM services both MMU non-replayable fault and FIFO interrupts\n    if (IS_GSP_CLIENT(pGpu))",
            "    // GSP RM services both MMU non-replayable fault and FIFO interrupts\n    if (IS_GSP_CLIENT(pGpu) || IS_VIRTUAL_WITHOUT_SRIOV(pGpu))",
            guard_substring="if (IS_GSP_CLIENT(pGpu) || IS_VIRTUAL_WITHOUT_SRIOV(pGpu))",
        )

    rpc_vgpu_h = nv_dir / "kernel" / "inc" / "vgpu" / "rpc_vgpu.h"
    rpc_txt = rpc_vgpu_h.read_text(encoding="utf-8")
    if "open_vgpu_bar0_rpc.h" not in rpc_txt:
        rpc_txt = rpc_txt.replace(
            "#define __vgpu_dev_nv_rpc_vgpu_h__",
            '#define __vgpu_dev_nv_rpc_vgpu_h__\n\n#include "nvVer.h"\n#include "vgpu/open_vgpu_bar0_rpc.h"',
            1,
        )
        rpc_txt = rpc_txt.replace(
            "static NV_INLINE void NV_RM_RPC_ALLOC_LOCAL_USER(OBJGPU *pGpu, ...) { }",
            "#define NV_RM_RPC_ALLOC_LOCAL_USER(pGpu, hClient, hParent, hMemory, pMemDesc, trueLength, attr, attr2, format, status) \\\n"
            "    do { status = openVgpuRpcAllocLocalUser((pGpu), (hClient), (hParent), (hMemory), (pMemDesc), (trueLength), (attr), (attr2), (format)); } while (0)",
            1,
        )
        rpc_txt = rpc_txt.replace(
            "static NV_INLINE void NV_RM_RPC_ALLOC_VIDMEM(OBJGPU *pGpu, ...) { }",
            "#define NV_RM_RPC_ALLOC_VIDMEM(pGpu, hClient, hParent, hMemory, pMemDesc, size, attr, attr2, type, flags, height, width, format, comprCovg, zcullCovg, alignment, pitch, ctagOffset, status) \\\n"
            "    do { \\\n"
            "        (void)(height); (void)(width); (void)(zcullCovg); (void)(alignment); (void)(pitch); (void)(ctagOffset); \\\n"
            "        status = openVgpuRpcAllocVidmem((pGpu), (hClient), (hParent), (hMemory), (pMemDesc), (size), (attr), (attr2), (type), (flags), (format), (comprCovg)); \\\n"
            "    } while (0)",
            1,
        )
        rpc_txt = rpc_txt.replace(
            "static NV_INLINE void NV_RM_RPC_ALLOC_VIRTMEM(OBJGPU *pGpu, ...) { }",
            "#define NV_RM_RPC_ALLOC_VIRTMEM(pGpu, hClient, hParent, hVASpace, hMemory, pOffset, size, attr, attr2, type, flags, height, width, format, comprCovg, zcullCovg, rangeLo, rangeHi, alignment, status) \\\n"
            "    do { \\\n"
            "        (void)(attr); (void)(attr2); (void)(height); (void)(width); (void)(format); (void)(comprCovg); (void)(zcullCovg); (void)(rangeLo); (void)(rangeHi); \\\n"
            "        status = openVgpuRpcAllocVirtmem((pGpu), (hClient), (hParent), (hVASpace), (hMemory), (pOffset), (size), (type), (flags), (alignment)); \\\n"
            "    } while (0)",
            1,
        )
        rpc_txt = rpc_txt.replace(
            "static NV_INLINE void NV_RM_RPC_MAP_MEMORY(OBJGPU *pGpu, ...) { }",
            "#define NV_RM_RPC_MAP_MEMORY(pGpu, hClient, hDevice, hMemory, offset, length, flags, pApertureOffset, status) \\\n"
            "    do { status = openVgpuRpcMapMemory((pGpu), (hClient), (hDevice), (hMemory), (offset), (length), (flags), (pApertureOffset)); } while (0)",
            1,
        )
        rpc_txt = rpc_txt.replace(
            "static NV_INLINE void NV_RM_RPC_UNMAP_MEMORY(OBJGPU *pGpu, ...) { }",
            "#define NV_RM_RPC_UNMAP_MEMORY(pGpu, hClient, hDevice, hMemory, flags, apertureOffset, status) \\\n"
            "    do { status = openVgpuRpcUnmapMemory((pGpu), (hClient), (hDevice), (hMemory), (flags), (apertureOffset)); } while (0)",
            1,
        )
        rpc_txt = rpc_txt.replace(
            "static NV_INLINE void NV_RM_RPC_SET_GUEST_SYSTEM_INFO_EXT(OBJGPU *pGpu, ...) { }",
            "#define NV_RM_RPC_SET_GUEST_SYSTEM_INFO_EXT(pGpu, status) \\\n"
            "    do { status = openVgpuRpcSetGuestSystemInfoExt((pGpu), GPU_GET_RPC(pGpu), NV_VERSION_STRING); } while (0)",
            1,
        )
        rpc_txt = rpc_txt.replace(
            "static NV_INLINE void NV_RM_RPC_GET_CONSOLIDATED_STATIC_INFO(OBJGPU *pGpu, ...) { }",
            "#define NV_RM_RPC_GET_CONSOLIDATED_STATIC_INFO(pGpu, status) \\\n"
            "    do { status = openVgpuRpcGetConsolidatedStaticInfo((pGpu), GPU_GET_RPC(pGpu)); } while (0)",
            1,
        )
        rpc_txt = rpc_txt.replace(
            "static NV_INLINE void NV_RM_RPC_GET_CONSOLIDATED_GR_STATIC_INFO(OBJGPU *pGpu, ...) { }",
            "#define NV_RM_RPC_GET_CONSOLIDATED_GR_STATIC_INFO(pGpu, status) \\\n"
            "    do { status = openVgpuRpcGetConsolidatedGrStaticInfo((pGpu), GPU_GET_RPC(pGpu)); } while (0)",
            1,
        )
        rpc_txt = rpc_txt.replace(
            "static NV_INLINE void NV_RM_RPC_UPDATE_PDE_2(OBJGPU *pGpu, ...) { }",
            "#define NV_RM_RPC_UPDATE_PDE_2(pGpu, hClient, hDevice, pParams, status) \\\n"
            "    do { status = openVgpuRpcUpdatePde2((pGpu), (hClient), (hDevice), (pParams), (NvU32)sizeof(*(pParams))); } while (0)",
            1,
        )
        rpc_txt = rpc_txt.replace(
            "static NV_INLINE void NV_RM_RPC_ALLOC_CONTEXT_DMA(OBJGPU *pGpu, ...) { }",
            "#define NV_RM_RPC_ALLOC_CONTEXT_DMA(pGpu, hClient, hDevice, hDma, hClass, flags, hMemory, offset, limit, status) \\\n"
            "    do { status = openVgpuRpcAllocContextDma((pGpu), (hClient), (hDevice), (hDma), (hClass), (flags), (hMemory), (offset), (limit)); } while (0)",
            1,
        )
        rpc_vgpu_h.write_text(rpc_txt, encoding="utf-8")


def patch_kernel_open_compat(src_root: Path) -> None:
    """Placeholder for Linux 6.13–7.0 kernel-open compat shims populated from real kbuild."""
    kopen = src_root / "kernel-open"
    if not kopen.is_dir():
        return


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Patch NVIDIA/open-gpu-kernel-modules@535.309.01 for open Pascal GP104 vGPU guests"
    )
    ap.add_argument("--src-dir", required=True, help="Path to open-gpu-kernel-modules@535.309.01 checkout")
    ap.add_argument("--overlay-dir", default=None, help="Path to open-vgpu-guest/overlay directory")
    ap.add_argument("--skip-kernel-compat", action="store_true", help="Skip Linux 6.13-7.0 kernel-open shims")
    args = ap.parse_args()

    src_root = Path(args.src_dir).resolve()
    overlay_dir = (
        Path(args.overlay_dir).resolve()
        if args.overlay_dir
        else Path(__file__).resolve().parents[1] / "overlay"
    )
    patch_rm_sources(src_root, overlay_dir)
    if not args.skip_kernel_compat:
        patch_kernel_open_compat(src_root)
    print(f"[patch_open_535_guest_rm] Patched {src_root} using overlay {overlay_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
