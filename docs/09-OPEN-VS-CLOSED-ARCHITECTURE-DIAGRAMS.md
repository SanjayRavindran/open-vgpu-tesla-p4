# Open-Source vs. Closed-Source vGPU Software Architecture (Host & Guests)

This document visualizes how clean-room open-source components, upstream open-source software, and retained closed-source binaries work together across the **Hypervisor Host** (`192.168.1.24`), the **AlmaLinux 10.3 Wayland/Selkies Guest** (`192.168.1.68`), and the **Windows 11 24H2 WDDM Guest** (`192.168.1.65`) in the 50/50 Tesla P4 (`GRID P4-4Q` / `nvidia-65`) virtualization stack.

### Color Legend (Mermaid Diagrams)

| Class | Meaning | Examples |
| :--- | :--- | :--- |
| Green | **Clean-Room Open Source (Ours)** | `open-nvidia-vgpud` (Rust), `libopen-vgxcfg.so` (C), `open-nvidia-xid-logd` (C), `open-nvidia-vgpu-mgr` (C), `open-nvidia-smi` + `libopen-nvidia-ml.so` (C), provisioning & PTX patch scripts |
| Blue | **Upstream Open Source** | Linux KVM/VFIO/mdev, QEMU/OVMF/`swtpm`, FastAPI-DLS, GNOME 49.5, FreeRDP, Docker/Selkies, NVIDIA MIT/GPL kernel shims & R620 Wayland shims |
| Red / Orange | **Closed Source (NVIDIA Proprietary)** | `nv-kernel.o_binary`, `libnvidia-vgpu.so`, signed Falcon blobs (`PrivSec=1`), Guest `nvlddmkm.sys` & `libcuda` |
| Slate | **Closed Source OS / Client** | Windows 11 24H2 Kernel, DWM, `TermService` (`:3389`) |

---

## 1. Host Software Architecture (`192.168.1.24` — Ubuntu 26.04 KVM / Proxmox VE 8–9)

```mermaid
flowchart TB
    classDef ours fill:#dcfce7,stroke:#15803d,stroke-width:2px,color:#14532d
    classDef oss fill:#dbeafe,stroke:#1d4ed8,stroke-width:2px,color:#1e3a8a
    classDef closed fill:#fee2e2,stroke:#b91c1c,stroke-width:2px,color:#7f1d1d
    classDef hw fill:#fef3c7,stroke:#b45309,stroke-width:2px,color:#78350f

    subgraph H_USER["Host Userspace Control Plane, Mediation, and Licensing"]
        XML["vgpuConfig.xml (/usr/share/nvidia/vgpu/)"]:::closed
        VGPUD["open-nvidia-vgpud (Rust, GPL-2.0): RM profile registrar"]:::ours
        VGXCFG["libopen-vgxcfg.so (C, 9 vgxConfig* exports, 0 diff vs NVIDIA)"]:::ours
        XID["open-nvidia-xid-logd (C, dlopen NVML + 76-char Base64 dump)"]:::ours
        MGR["open-nvidia-vgpu-mgr (C, 60 vmiop* exports & worker runtime)"]:::ours
        VMIOP_SO["libnvidia-vgpu.so.535.309.01 (Closed vmiop 50/50 scheduler & VRPC plugin)"]:::closed
        SMI["open-nvidia-smi + libopen-nvidia-ml.so (C, clean-room RM telemetry & vGPU CLI)"]:::ours
        DLS["FastAPI-DLS v2 (:443, Docker or systemd venv)"]:::oss
        FAN["h61-fan-control (Python PWM >= 60C thermal loop)"]:::ours
        SYSFS["sysfs mdev_supported_types (nvidia-65 = GRID P4-4Q)"]:::oss
    end

    subgraph H_HYP["Hypervisor & Virtual Device Layer (QEMU/KVM or Proxmox VE)"]
        QEMU_ALMA["QEMU + OVMF: vm-alma (192.168.1.68, 4 vCPU, 5120M)"]:::oss
        QEMU_WIN["QEMU + OVMF SecBoot + swtpm: vm-win11 (192.168.1.65, 4 vCPU, 6144M)"]:::oss
        BR0["Host Bridge br0 / vmbr0 (VLAN/LAN 192.168.1.0/24)"]:::oss
    end

    subgraph H_KRN["Host Kernel Space (Linux 7.0.0-43 / Proxmox VE DKMS 535.309.01)"]
        NV_SHIM["nvidia.ko OS Shim (Linux GPL/MIT DKMS wrapper)"]:::oss
        NV_CORE["nv-kernel.o_binary (Closed RM Core & ioctl dispatch)"]:::closed
        VGPU_VFIO["nvidia-vgpu-vfio.ko (Open MIT KVM/VFIO mdev mediation driver)"]:::oss
        KVM_VFIO["Linux KVM, vfio-pci, mdev, iommu=pt"]:::oss
    end

    subgraph H_HW["Physical Silicon (UEFI Coreboot + Intel VT-d)"]
        GPU["Tesla P4 GP104GL (10de:1bb3): 8 GiB GDDR5 (4 GiB + 4 GiB), PrivSec=1"]:::hw
        FALCON["Signed Falcon Firmware: ACR, PMU, SEC2, FECS, GPCCS, NVENC, NVDEC"]:::closed
    end

    XML --> VGPUD
    VGPUD -- "/dev/nvidiactl RM ioctls 0xa0810101 + 0xa0810109" --> NV_SHIM
    NV_SHIM <--> NV_CORE
    NV_CORE <--> VGPU_VFIO
    VGPU_VFIO --> SYSFS

    MGR <-- "/dev/nvidia-vgpu* & vmiop RPC" --> VGPU_VFIO
    MGR <-- "dlvsym VER_5_0 / VER_2_0 & vmiop_* ABI" --> VMIOP_SO
    MGR -. "reads vgxConfig* if queried" .-> VGXCFG
    XID -- "dlopen libnvidia-ml.so.1" --> SMI
    SMI -- "RM NVML ioctl" --> NV_SHIM
    FAN -- "sysfs / nvidia-smi temp" --> SMI

    QEMU_ALMA -- "vfio-pci mdev 1111...0003" --> VGPU_VFIO
    QEMU_WIN -- "vfio-pci mdev 1111...0005" --> VGPU_VFIO
    QEMU_ALMA <--> BR0
    QEMU_WIN <--> BR0
    DLS <-- "HTTPS /-/client-token & leases" --- BR0

    VGPU_VFIO --> KVM_VFIO
    NV_CORE -- "MMIO & DMA via Intel VT-d" --> GPU
    KVM_VFIO -- "PCIe BAR passthrough / traps" --> GPU
    GPU --- FALCON
```

---

## 2. Guest Software Architectures (Separate Per-Guest Stacks)

### 2A. AlmaLinux 10.3 Guest (`192.168.1.68` — Pure Wayland + Headless RDP + Selkies)

```mermaid
flowchart TB
    classDef ours fill:#dcfce7,stroke:#15803d,stroke-width:2px,color:#14532d
    classDef oss fill:#dbeafe,stroke:#1d4ed8,stroke-width:2px,color:#1e3a8a
    classDef closed fill:#fee2e2,stroke:#b91c1c,stroke-width:2px,color:#7f1d1d
    classDef vhw fill:#fef3c7,stroke:#b45309,stroke-width:2px,color:#78350f

    subgraph AL_APP["Desktop, Remote Display, and Container Layer (Pure Wayland)"]
        GNOME["GNOME Shell 49.5 (--no-x11 --headless)"]:::oss
        GTK4["GTK4 Apps (GSK_RENDERER=ngl -> OpenGL 4.6 / EGL)"]:::oss
        GRD["gnome-remote-desktop-headless (:3389 RDP + patched PTX sm_61)"]:::oss
        PTX_PATCH["patch_grd_ptx_535.sh (Open PTX 8.5->8.2 CUDA 12.2 fix)"]:::ours
        SELKIES["Selkies Webtop Container (Docker CE + PixelFlux 10-bit 4:4:4 NVENC)"]:::oss
        CDI["NVIDIA Container Toolkit (CDI, el10 .note.gnu.property stripped)"]:::oss
    end

    subgraph AL_USR["Hybrid Userspace Driver Stack (R620 Wayland + R535 Core)"]
        EGL_WL["Open R620 Shims: libnvidia-egl-wayland, wayland2, gbm"]:::oss
        NV_GL["Closed R535 Libraries: libEGL_nvidia, libnvidia-glcore, libcuda, libnvidia-encode"]:::closed
        GRIDD["nvidia-gridd (GRID Licensing Daemon -> /etc/nvidia/ClientConfigToken/)"]:::closed
        AL_PROV["provision_alma_guest.sh (Open setup automation)"]:::ours
    end

    subgraph AL_KRN["AlmaLinux 10.3 Guest Kernel & Virtual vGPU Hardware"]
        DRM["Open Linux DRM/KMS + nvidia-drm.ko (modeset=1) + nvidia.ko OS shim"]:::oss
        G_CORE["Closed R535 GRID nv-kernel.o_binary"]:::closed
        VDEV["Virtual PCIe 0000:00:03.0 (10de:1bb3 SUBSYS 1206 GRID P4-4Q, BAR0 0x8f000 VRPC)"]:::vhw
    end

    PTX_PATCH -. "patches CUDA PTX kernel" .-> GRD
    AL_PROV -. "configures env, EGL, DLS token" .-> GTK4
    GNOME --> GTK4
    GNOME --> EGL_WL
    GRD -- "NVENC H.264 / CUDA frames" --> NV_GL
    GRD <-- "RDP :3389" --> GNOME
    SELKIES --> CDI
    CDI --> NV_GL
    GTK4 --> EGL_WL
    EGL_WL --> NV_GL
    NV_GL --> DRM
    GRIDD -- "HTTPS :443 FastAPI-DLS" --> VDEV
    DRM <--> G_CORE
    G_CORE -- "BAR0 trap + VRPC ring -> Host nvidia-vgpu-mgr" --> VDEV
```

### 2B. Windows 11 24H2 Guest (`192.168.1.65` — WDDM 3.1 + RDP)

```mermaid
flowchart TB
    classDef ours fill:#dcfce7,stroke:#15803d,stroke-width:2px,color:#14532d
    classDef closed fill:#fee2e2,stroke:#b91c1c,stroke-width:2px,color:#7f1d1d
    classDef winos fill:#e2e8f0,stroke:#475569,stroke-width:2px,color:#1e293b
    classDef vhw fill:#fef3c7,stroke:#b45309,stroke-width:2px,color:#78350f

    subgraph W_PROV["Open Provisioning & Automated Fixes (Zero Hardcoded Secrets)"]
        AUTO["win11-autounattend.xml (Unattended OOBE + WinRM NTLM + RDP)"]:::ours
        PS1["provision_win11_guest.ps1 (Driver bind, DLS token, 0x200d reg fix)"]:::ours
    end

    subgraph W_OS["Windows 11 Desktop, Display & Remote Services (Closed OS)"]
        DWM["Windows 11 DWM & DirectX 12 Compositor"]:::winos
        RDP["TermService (:3389, NLA UserAuthentication=1, AVC override removed -> no 0x200d)"]:::winos
        APPS["NVIDIA Control Panel (AppX 8.1.969.0) & GPU-Z"]:::closed
    end

    subgraph W_DRV["NVIDIA WDDM 3.1 Driver 539.72 WHQL (Closed Proprietary)"]
        UMD["User-Mode Driver: D3D11/12, OpenGL, CUDA, NVENC DLLs"]:::closed
        DISP_SVC["NVDisplay.ContainerLocalSystem (ClientConfigToken -> FastAPI-DLS :443)"]:::closed
        KMD["nvlddmkm.sys (WDDM 3.1 Kernel Miniport Driver)"]:::closed
    end

    subgraph W_HW["Virtual PCIe Hardware (OVMF SecureBoot + swtpm TPM 2.0)"]
        W_VDEV["Virtual PCIe GPU (VEN_10DE&DEV_1BB3&SUBSYS_120610DE GRID P4-4Q, VRPC)"]:::vhw
    end

    AUTO -. "provisions OS + WinRM" .-> RDP
    PS1 -. "pnputil nvgridsw.inf & registry fixes" .-> RDP
    PS1 -. "installs token & restarts container" .-> DISP_SVC
    DWM --> UMD
    RDP --> DWM
    APPS --> UMD
    UMD --> KMD
    DISP_SVC <--> KMD
    KMD -- "BAR0 0x8f000 traps & VRPC ring -> Host nvidia-vgpu-mgr" --> W_VDEV
```

---

## 3. Cross-Boundary Boot-to-Frame Interaction Sequence

```mermaid
sequenceDiagram
    participant vgpud as "open-nvidia-vgpud (Open Rust)"
    participant nvko as "Host nvidia.ko + nvidia-vgpu-vfio.ko (DKMS)"
    participant mgr as "Host open-nvidia-vgpu-mgr (Open C) + libnvidia-vgpu.so (Closed)"
    participant qemu as "QEMU/KVM + VFIO mdev"
    participant guest as "Guest Driver (Alma R535/R620 or Win11 539.72)"
    participant dls as "Host FastAPI-DLS (:443, Open)"

    vgpud->>nvko: "NV_ESC_RM_CONTROL 0xa0810101 (12 expanded profiles) + 0xa0810109"
    nvko-->>vgpud: "Creates /sys/.../mdev_supported_types/nvidia-65 (GRID P4-4Q)"
    qemu->>nvko: "Echo UUID -> nvidia-65/create (binds 2x 4096 MiB mdevs)"
    nvko->>mgr: "Spawn vGPU device plugin instances (50/50 scheduler)"
    guest->>qemu: "PCI probe 10de:1bb3 SUBSYS 1206 + BAR0 0x8f000 VRPC ring init"
    qemu->>nvko: "Trap BAR0/config access -> forward VRPC"
    nvko->>mgr: "VGPU-RPC SetGuestSystemInfo -> GetStaticInfo535 -> Alloc/Submit"
    mgr->>nvko: "Schedule physical GP104 channels via signed Falcon blobs"
    guest->>dls: "HTTPS GET /-/client-token & lease request"
    dls-->>guest: "Signed 90-day GRID-Virtual-WS / QVDWS license lease"
    guest-->>qemu: "Hardware-rendered Wayland/WDDM frames via RDP :3389 or Selkies"
```

---

## 4. Component Boundary Summary

| Component | Layer | Origin | License / Binary State | Responsibility |
| :--- | :--- | :--- | :--- | :--- |
| `open-nvidia-vgpud` | Host Userspace | **Ours** | Open (Rust, GPL-2.0) | Parses `vgpuConfig.xml`, expands license strings, registers 12 Tesla P4 types in RM |
| `libopen-vgxcfg.so` | Host Userspace | **Ours** | Open (C, 0-diff parity) | Exports all 9 `vgxConfig*` XML query functions |
| `open-nvidia-xid-logd` | Host Userspace | **Ours** | Open (C) | Monitors NVML XID events cleanly via `dlopen`, logs 76-char Base64 dumps |
| `open-nvidia-vgpu-mgr` | Host Userspace | **Ours** | Open (C, 60 `vmiop*` exports) | Binds `/dev/nvidia-vgpuctl` & `/dev/nvidia-vgpu%d`, forks per-VM `vgpu` workers, hosts `vmiop-env` ABI |
| `open-nvidia-smi` / `libopen-nvidia-ml.so` | Host Userspace | **Ours** | Open (C, `libnvidia-ml.so.1`) | Direct `/dev/nvidiactl` RM telemetry (`0x2080*`, `0xa081*`), vGPU instance/type queries, and 89-col / CSV CLI |
| `FastAPI-DLS` | Host Service | Upstream Open | Open (Python/Docker/systemd) | Issues TLS licensing tokens & leases (`192.168.1.24:443`) |
| `nvidia-vgpu-vfio.ko` | Host Kernel | NVIDIA / DKMS | Open (MIT C) | KVM/VFIO `mdev` lifecycle and BAR0/VRPC trap forwarding |
| `nvidia.ko` (`nv-kernel.o_binary`) | Host Kernel | NVIDIA Proprietary | Open OS shim + Closed core | Hardware initialization, signed Falcon execution, channel scheduling |
| `libnvidia-vgpu.so` | Host Userspace | NVIDIA Proprietary | Closed `.so` plugin (retained) | Loaded by `open-nvidia-vgpu-mgr` for `vmiop` 50/50 multi-VM mediation & VRAM/channel virtualization |
| AlmaLinux `535` + `620` Stack | Linux Guest | Hybrid | Open R620 EGL/DRM + Closed R535 | Pure Wayland GNOME 49.5 + GTK4 (`ngl`) + CUDA PTX patched RDP + Selkies |
| Windows 11 `539.72 WHQL` Stack | Windows Guest | Closed + Open scripts | Closed WDDM + Open `.ps1`/XML | WDDM 3.1 DX12/CUDA/NVENC + policy-fixed RDP `:3389` without `0x200d` |
