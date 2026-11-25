# Disclaimer, Risk Notice, AI/LLM Usage Disclosure & Clean-Room Boundary

## 1. No Warranty & High-Risk Experimental Software Notice
THIS REPOSITORY — INCLUDING ALL RUST SOURCE CODE (`nova-vgpu/`), LINUX KERNEL MODULES (`kmods/`), SHELL/PYTHON EXTRACTION AND PATCHING SCRIPTS (`scripts/`, `tools/`), RDP PROFILES (`rdp/`), TESTS (`tests/`), AND TECHNICAL DOCUMENTATION (`docs/`) — IS PROVIDED **"AS IS"**, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, AND NONINFRINGEMENT.

**Operational & Hardware Risks:**
- **Kernel & Host Instability:** Loading experimental out-of-tree Linux kernel modules (`nova_vgpu_mdev`, `nova_vgpu_guest`) or patching host/guest GPU drivers across newer kernels (`Linux 7.0–7.3+`) can cause kernel panics, hard lockups, GPU hangs, or Windows BSODs.
- **Data Loss & VM Disk Corruption:** Virtualized storage/NIC driver combinations can cause filesystem corruption if misconfigured. Always maintain offline backups and test inside isolated, disposable VMs first.
- **Network & Remote Desktop Exposure:** The provided `.rdp` profiles and multi-VM LAN bridge (`br0`) are intended strictly for trusted, isolated local lab networks. Never expose RDP (`:3389`), WinRM (`:5985`), or QEMU VNC (`:5911–:5915`) directly to the public internet without VPN/TLS hardening and strong credentials.
- **No Official Vendor Support:** Running R580 (`580.196.09` / `580.196.07` / `582.99`) on Pascal (`GP104` / Tesla P4) KVM vGPU hosts/guests with open-C compatibility shims or `vgpuConfig.xml` bypasses is experimental and completely unsupported by NVIDIA, Red Hat, Canonical, Debian, Proxmox, and Microsoft.

By using any code, script, or instruction in this repository, **you assume 100% of the risk** for hardware damage, data loss, downtime, or security incidents.

## 2. AI / Large Language Model (LLM) Usage Disclosure
**An AI / Large Language Model (LLM) agent extensively assisted in creating this repository**, including:
- Synthesizing clean-room reverse-engineering notes and ELF/Falcon analysis summaries (`evidence/`, `docs/`),
- Writing and refactoring the Rust `nova-vgpu` crate (`nova-vgpu/`), clean-room C host/guest daemons (`open-*`), Linux kernel modules (`kmods/`), Python/Bash/PowerShell provisioning and patching scripts (`scripts/`, `tools/`), `.rdp` templates (`rdp/`), and automated/live test harnesses (`tests/`),
- Executing and troubleshooting multi-VM deployment tests across AlmaLinux 10.3, Ubuntu 26.04 LTS, and Windows 11 24H2.

LLM-generated systems and kernel code may contain subtle logic bugs, race conditions, security flaws, or version-specific assumptions. **You must independently review, audit, and test all code and scripts before running them on any machine you care about.**

## 3. Clean-Room Open-Source Boundary (`GPL-2.0-only`) & Vendor Licensing
- **100% Open Source in Git:** Every file committed to this repository is original open-source code, tests, manifests, or documentation licensed under **GNU GPL v2.0** ([`LICENSE`](./LICENSE)).
- **Zero Proprietary NVIDIA Binaries Included:** This repository does **not** redistribute NVIDIA `.run` installers, Windows `.exe` installers, `nv-kernel.o_binary`, Falcon signed microcode (`.bin`), VBIOS/PROM dumps, `libnvidia-vgpu.so`, or guest driver payloads.
- **User-Supplied Runtime Extraction:** Required NVIDIA binaries must be obtained legally by the user under NVIDIA's applicable EULA/software license terms and extracted locally using [`scripts/extract_proprietary_blobs.sh`](./scripts/extract_proprietary_blobs.sh).
