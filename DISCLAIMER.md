# Disclaimer, Risk Notice, AI/LLM Usage Disclosure & Clean-Room Boundary

## 1. No Warranty & High-Risk Experimental Software Notice
THIS REPOSITORY — INCLUDING ALL RUST SOURCE CODE (`nova-vgpu/`), LINUX KERNEL MODULES (`kmods/`), SHELL/PYTHON EXTRACTION AND PATCHING SCRIPTS (`scripts/`, `tools/`), RDP PROFILES (`rdp/`), TESTS (`tests/`), AND TECHNICAL DOCUMENTATION (`docs/`) — IS PROVIDED **"AS IS"**, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, AND NONINFRINGEMENT.

**Operational & Hardware Risks:**
- **Kernel & Host Instability:** Loading experimental out-of-tree Linux kernel modules (`nova_vgpu_mdev`, `nova_vgpu_guest`) or patching host/guest GPU drivers across newer kernels (`Linux 7.0–7.3+`) can cause kernel panics, hard lockups, GPU hangs, or Windows BSODs.
- **Data Loss & VM Disk Corruption:** Virtualized storage/NIC driver combinations (for example, `virtio-win 0.1.302` with `cache=writeback` and BitLocker observed during Windows 11 testing) can cause silent filesystem corruption. Always maintain offline backups and test inside isolated, disposable VMs first.
- **Network & Remote Desktop Exposure:** The provided `.rdp` profiles and multi-VM LAN bridge (`br0`) are intended strictly for trusted, isolated local lab networks. Never expose RDP (`:3389`), WinRM (`:5985`), or QEMU VNC (`:5911–:5915`) directly to the public internet without VPN/TLS hardening and strong credentials.
- **No Official Vendor Support:** NVIDIA ended Pascal (`GP104` / Tesla P4) vGPU support in R535. Overlaying R620 Wayland EGL/GBM libraries onto R535 guests or downgrading GNOME Remote Desktop PTX headers is completely unsupported by NVIDIA, Red Hat, Canonical, Debian, Arch Linux, and Microsoft.

By using any code, script, or instruction in this repository, **you assume 100% of the risk** for hardware damage, data loss, downtime, or security incidents.

## 2. AI / Large Language Model (LLM) Usage Disclosure
**An AI / Large Language Model (LLM) agent extensively assisted in creating this repository**, including:
- Synthesizing clean-room reverse-engineering notes and ELF/Falcon analysis summaries (`evidence/`, `docs/`),
- Writing and refactoring the Rust `nova-vgpu` crate (`nova-vgpu/`), clean-room Linux kernel modules (`kmods/`), Python/Bash blob extraction and kernel/PTX patching scripts (`scripts/`, `tools/`), `.rdp` templates (`rdp/`), and automated/live test harnesses (`tests/`),
- Executing and troubleshooting multi-VM deployment tests across Ubuntu 24.04, Debian 13, AlmaLinux 10.3, Arch Linux, and Windows 11 24H2.

LLM-generated systems and kernel code may contain subtle logic bugs, race conditions, security flaws, or version-specific assumptions. **You must independently review, audit, and test all code and scripts before running them on any machine you care about.**

## 3. Clean-Room Open-Source Boundary (`GPL-2.0-only`) & Vendor Licensing
- **100% Open Source in Git:** Every file committed to this repository is original open-source code, tests, manifests, or documentation licensed under **GNU GPL v2.0** ([`LICENSE`](./LICENSE)).
- **Zero Proprietary NVIDIA Binaries Included:** This repository does **not** redistribute NVIDIA `.run` installers, Windows `.exe` installers, `nv-kernel.o_binary`, Falcon signed microcode (`.bin`), VBIOS/PROM dumps, `libnvidia-vgpu.so`, or R620 Wayland EGL platform `.so` files.
- **User-Supplied Runtime Extraction:** Required NVIDIA binaries must be obtained legally by the user under NVIDIA's applicable EULA/software license terms and extracted locally using [`scripts/extract_proprietary_blobs.sh`](./scripts/extract_proprietary_blobs.sh).
- **No DRM, Signature, or Licensing Bypass:** Hardware `PrivSec` Falcon signatures and NVIDIA vGPU licensing (`nvidia-gridd` / NLS / DLS / CLS) are respected and are **never** bypassed, cracked, or forged by this project.
